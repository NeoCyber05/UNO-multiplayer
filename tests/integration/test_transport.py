import json
import struct
import unittest
from tests.integration.helpers import ServerProcess, WireClient

class TestTransport(unittest.TestCase):
    def test_concurrent_clients(self):
        with ServerProcess() as server:
            clients = [WireClient("127.0.0.1", server.port) for _ in range(8)]
            try:
                for i, c in enumerate(clients):
                    resp_hello = c.request("HELLO", {})
                    self.assertEqual(resp_hello["v"], 1)
                    self.assertEqual(resp_hello["type"], "OK")
                    self.assertIn("serverTimeMs", resp_hello["payload"])
                    self.assertIsInstance(resp_hello["payload"]["serverTimeMs"], (int, float))

                    resp_ping = c.request("PING", {})
                    self.assertEqual(resp_ping["v"], 1)
                    self.assertEqual(resp_ping["type"], "OK")
            finally:
                for c in clients:
                    c.close()

    def test_coalesced_frames(self):
        with ServerProcess() as server:
            with WireClient("127.0.0.1", server.port) as client:
                body1 = json.dumps({"v": 1, "id": 101, "type": "HELLO", "payload": {}}).encode("utf-8")
                frame1 = struct.pack(">I", len(body1)) + body1

                body2 = json.dumps({"v": 1, "id": 102, "type": "PING", "payload": {}}).encode("utf-8")
                frame2 = struct.pack(">I", len(body2)) + body2

                # Send both frames coalesced in a single packet/write
                client.send_raw(frame1 + frame2)

                resp1 = client.recv_envelope()
                self.assertEqual(resp1["id"], 101)
                self.assertEqual(resp1["type"], "OK")
                self.assertIn("serverTimeMs", resp1["payload"])

                resp2 = client.recv_envelope()
                self.assertEqual(resp2["id"], 102)
                self.assertEqual(resp2["type"], "OK")

    def test_max_connections(self):
        with ServerProcess() as server:
            clients = []
            try:
                # Open 64 connections (the maximum limit)
                for _ in range(64):
                    c = WireClient("127.0.0.1", server.port)
                    resp = c.request("PING", {})
                    self.assertEqual(resp["type"], "OK")
                    clients.append(c)

                # 65th connection should be rejected: either refused/closed or gets SERVER_FULL
                rejected = False
                try:
                    c65 = WireClient("127.0.0.1", server.port, timeout=1.0)
                    try:
                        resp = c65.request("PING", {}, timeout=1.0)
                        if resp.get("type") == "SERVER_FULL":
                            rejected = True
                    except (ConnectionResetError, ConnectionError, TimeoutError, OSError):
                        rejected = True
                    finally:
                        c65.close()
                except (ConnectionResetError, ConnectionError, OSError):
                    rejected = True

                self.assertTrue(rejected, "65th connection should be rejected or closed")

                # Verify all original 64 connections are still healthy and can PING
                for c in clients:
                    resp = c.request("PING", {})
                    self.assertEqual(resp["type"], "OK")

                # Close one connection and verify a new client can now take the freed slot
                clients[0].close()
                clients.pop(0)

                new_client = WireClient("127.0.0.1", server.port)
                try:
                    resp = new_client.request("PING", {})
                    self.assertEqual(resp["type"], "OK")
                finally:
                    new_client.close()

            finally:
                for c in clients:
                    c.close()


if __name__ == "__main__":
    unittest.main()
