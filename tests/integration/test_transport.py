import json
import socket
import struct
import time
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

    def test_slow_consumer_disconnect(self):
        with ServerProcess() as server:
            with WireClient("127.0.0.1", server.port) as client_normal:
                resp = client_normal.request("PING", {})
                self.assertEqual(resp["type"], "OK")

                # Connect slow client with small receive buffer so it fills up quickly
                slow_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                slow_sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
                slow_sock.connect(("127.0.0.1", server.port))
                slow_sock.settimeout(3.0)

                # Send frames exceeding 256 KiB buffer without reading
                body = json.dumps({"v": 1, "id": 1, "type": "PING", "payload": {}}).encode("utf-8")
                frame = struct.pack(">I", len(body)) + body
                batch = frame * 25000
                closed = False
                try:
                    slow_sock.sendall(batch)
                except (BrokenPipeError, ConnectionResetError, OSError):
                    closed = True

                # Client does not read while server processes and pushes frames exceeding 256 KiB
                time.sleep(0.5)

                # Server should mark closing and terminate the socket
                if not closed:
                    start = time.time()
                    while time.time() - start < 3.0:
                        try:
                            data = slow_sock.recv(65536)
                            if len(data) == 0:
                                closed = True
                                break
                        except (ConnectionResetError, ConnectionError, BrokenPipeError):
                            closed = True
                            break
                        except TimeoutError:
                            break
                slow_sock.close()
                self.assertTrue(closed, "Server should terminate slow consumer connection")

                # Verify normal client continues to PING without interruption
                resp = client_normal.request("PING", {})
                self.assertEqual(resp["type"], "OK")

    def test_client_disconnect_mid_frame(self):
        with ServerProcess() as server:
            # Client connects, sends partial 4-byte length prefix (e.g. 2 bytes), then immediately closes socket
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(("127.0.0.1", server.port))
            s.sendall(b"\x00\x00")
            s.close()

            # Verify server is healthy, cleans up slot without hanging or crashing
            time.sleep(0.1)
            with WireClient("127.0.0.1", server.port) as client:
                resp = client.request("PING", {})
                self.assertEqual(resp["type"], "OK")

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

                # 65th connection should receive ERROR with code SERVER_FULL
                c65 = WireClient("127.0.0.1", server.port, timeout=2.0)
                try:
                    resp = c65.recv_envelope(timeout=2.0)
                    self.assertEqual(resp.get("type"), "ERROR")
                    self.assertEqual(resp.get("payload", {}).get("code"), "SERVER_FULL")
                    self.assertEqual(resp.get("payload", {}).get("message"), "Server connection limit reached")
                finally:
                    c65.close()

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
