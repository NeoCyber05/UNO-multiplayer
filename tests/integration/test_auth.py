import json
import os
import sqlite3
import struct
import tempfile
import threading
import time
import unittest
from tests.integration.helpers import ServerProcess, WireClient

# A replaced connection must reach EOF well within this; a timeout fails the test.
PUSH_DRAIN_TIMEOUT_S = 2.0

def is_error(resp, expected_code):
    if resp.get("type") == expected_code:
        return True
    if resp.get("type") == "ERROR" and resp.get("payload", {}).get("code") == expected_code:
        return True
    return False

class TestAuth(unittest.TestCase):
    def test_register_and_login_flow(self):
        with ServerProcess() as server:
            with WireClient("127.0.0.1", server.port) as client:
                # 1. Register a new user
                reg_payload = {
                    "email": "alice@example.com",
                    "password": "secretpassword123",
                    "name": "Alice",
                    "avatar": "fox"
                }
                resp_reg = client.request("REGISTER", reg_payload)
                self.assertEqual(resp_reg["v"], 1)
                self.assertEqual(resp_reg["type"], "OK")

                # 2. Login with correct password
                login_payload = {
                    "email": "alice@example.com",
                    "password": "secretpassword123"
                }
                resp_login = client.request("LOGIN", login_payload)
                self.assertEqual(resp_login["v"], 1)
                self.assertEqual(resp_login["type"], "OK")
                self.assertIn("token", resp_login["payload"])
                token = resp_login["payload"]["token"]
                self.assertEqual(len(token), 64)

                self.assertIn("me", resp_login["payload"])
                me = resp_login["payload"]["me"]
                self.assertEqual(me["name"], "Alice")
                self.assertEqual(me["avatar"], "fox")
                self.assertEqual(me["elo"], 1500)
                self.assertTrue("tag" in me or "userId" in me)
                if "tag" in me:
                    self.assertTrue(me["tag"].startswith("UNO#"))

                # 3. Login with wrong password returns UNAUTHENTICATED
                bad_login = {
                    "email": "alice@example.com",
                    "password": "wrongpassword999"
                }
                resp_bad = client.request("LOGIN", bad_login)
                self.assertTrue(is_error(resp_bad, "UNAUTHENTICATED"), f"Expected UNAUTHENTICATED, got {resp_bad}")

    def test_rate_limiting(self):
        # Default server limit is 5 auth requests/min from the same IP
        with ServerProcess(auth_limit=5) as server:
            with WireClient("127.0.0.1", server.port) as client:
                # Send 5 auth requests
                for i in range(5):
                    resp = client.request("LOGIN", {
                        "email": f"user{i}@example.com",
                        "password": "password123"
                    })
                    # Could be UNAUTHENTICATED or OK, but NOT RATE_LIMITED
                    self.assertFalse(is_error(resp, "RATE_LIMITED"), f"Request {i+1} got rate limited: {resp}")

                # 6th request must return RATE_LIMITED
                resp6 = client.request("LOGIN", {
                    "email": "user6@example.com",
                    "password": "password123"
                })
                self.assertTrue(is_error(resp6, "RATE_LIMITED"), f"Expected RATE_LIMITED on 6th request, got {resp6}")

        # Test that higher --auth-limit allows more requests
        with ServerProcess(auth_limit=20) as server:
            with WireClient("127.0.0.1", server.port) as client:
                for i in range(8):
                    resp = client.request("LOGIN", {
                        "email": f"test{i}@example.com",
                        "password": "password123"
                    })
                    self.assertFalse(is_error(resp, "RATE_LIMITED"), f"Request {i+1} got rate limited with limit 20: {resp}")

    def test_auth_worker_nonblocking(self):
        # Ensure that slow/heavy auth operations on Client A do NOT block PING on Client B
        with ServerProcess(auth_limit=50) as server:
            # Client A will request register/login
            client_a = WireClient("127.0.0.1", server.port)
            client_b = WireClient("127.0.0.1", server.port)
            try:
                # Register an account first
                resp_reg = client_a.request("REGISTER", {
                    "email": "speed@example.com",
                    "password": "heavy_password_12345",
                    "name": "Speedy",
                    "avatar": "panda"
                })
                self.assertEqual(resp_reg["type"], "OK")

                # Send LOGIN on Client A asynchronously
                client_a.send_envelope(999, "LOGIN", {
                    "email": "speed@example.com",
                    "password": "heavy_password_12345"
                })

                # Immediately send PING on Client B and measure round-trip time
                t0 = time.time()
                resp_ping = client_b.request("PING", {}, timeout=0.5)
                elapsed = time.time() - t0

                self.assertEqual(resp_ping["type"], "OK")
                self.assertLess(elapsed, 0.5, f"PING took too long ({elapsed:.3f}s), worker blocked event loop!")

                # Eventually Client A gets LOGIN response
                resp_login = client_a.recv_envelope(timeout=3.0)
                self.assertEqual(resp_login["type"], "OK")
            finally:
                client_a.close()
                client_b.close()

    def test_fd_reuse_after_disconnect(self):
        # Client disconnects right after submitting LOGIN. Slot is reassigned with new generation.
        # Server must safely drop the auth result without assigning session to new connection.
        with ServerProcess(auth_limit=50) as server:
            # Register user
            with WireClient("127.0.0.1", server.port) as reg_client:
                resp = reg_client.request("REGISTER", {
                    "email": "reuse@example.com",
                    "password": "password123",
                    "name": "Reuse",
                    "avatar": "owl"
                })
                self.assertEqual(resp["type"], "OK")

            # Client 1 connects, sends LOGIN, and IMMEDIATELY closes connection
            c1 = WireClient("127.0.0.1", server.port)
            c1.send_envelope(1, "LOGIN", {
                "email": "reuse@example.com",
                "password": "password123"
            })
            c1.close()

            # Client 2 connects immediately (likely grabbing the same slot, but new generation)
            c2 = WireClient("127.0.0.1", server.port)
            try:
                # Send PING on Client 2
                resp = c2.request("PING", {})
                self.assertEqual(resp["type"], "OK")

                # Wait a bit for auth worker to complete its job
                time.sleep(0.3)

                # Client 2 sends another PING and HELLO - should work smoothly without interference
                resp2 = c2.request("HELLO", {})
                self.assertEqual(resp2["type"], "OK")
            finally:
                c2.close()

    def test_new_login_disconnects_old_connection(self):
        with ServerProcess(auth_limit=50) as server:
            # Register user
            with WireClient("127.0.0.1", server.port) as reg_client:
                resp = reg_client.request("REGISTER", {
                    "email": "multilogin@example.com",
                    "password": "password123",
                    "name": "Multi",
                    "avatar": "fox"
                })
                self.assertEqual(resp["type"], "OK")

            # Client A connects and logs in
            client_a = WireClient("127.0.0.1", server.port)
            resp_a = client_a.request("LOGIN", {
                "email": "multilogin@example.com",
                "password": "password123"
            })
            self.assertEqual(resp_a["type"], "OK")
            self.assertIn("token", resp_a["payload"])

            # Client B connects and logs in with the same account
            client_b = WireClient("127.0.0.1", server.port)
            resp_b = client_b.request("LOGIN", {
                "email": "multilogin@example.com",
                "password": "password123"
            })
            self.assertEqual(resp_b["type"], "OK")
            self.assertIn("token", resp_b["payload"])

            # Client A socket should be closed by server (reads EOF). Pushes
            # sent to A after its own LOGIN (e.g. FRIEND_STATE) may still be
            # buffered ahead of the EOF, so skip whole frames until it arrives.
            time.sleep(0.1)
            try:
                while True:
                    env = client_a.recv_envelope(timeout=PUSH_DRAIN_TIMEOUT_S)
                    self.assertEqual(env.get("id"), 0,
                                     f"Expected only pushes before EOF, got {env}")
            except (ConnectionResetError, BrokenPipeError):
                pass
            finally:
                client_a.close()
                client_b.close()

    def test_register_db_error_reports_storage_error(self):
        with tempfile.NamedTemporaryFile(suffix=".db", delete=False) as tmp:
            db_path = tmp.name

        try:
            with ServerProcess(db=db_path, auth_limit=50) as server:
                # Open sqlite connection to the db and lock it exclusively
                db_conn = sqlite3.connect(db_path, timeout=0.1)
                try:
                    db_conn.execute("BEGIN EXCLUSIVE")
                    with WireClient("127.0.0.1", server.port) as client:
                        resp = client.request("REGISTER", {
                            "email": "dberror@example.com",
                            "password": "password123",
                            "name": "DbError",
                            "avatar": "fox"
                        }, timeout=3.0)
                        self.assertEqual(resp["type"], "ERROR")
                        self.assertEqual(resp.get("payload", {}).get("code"), "STORAGE_ERROR")
                finally:
                    db_conn.rollback()
                    db_conn.close()
        finally:
            if os.path.exists(db_path):
                try:
                    os.remove(db_path)
                except OSError:
                    pass

if __name__ == "__main__":
    unittest.main()
