import json
import struct
import threading
import time
import unittest
from tests.integration.helpers import ServerProcess, WireClient

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

if __name__ == "__main__":
    unittest.main()
