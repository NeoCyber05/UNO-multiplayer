import json
import time
import unittest
from tests.integration.helpers import ServerProcess, WireClient

def is_error(resp, expected_code):
    if resp.get("type") == expected_code:
        return True
    if resp.get("type") == "ERROR" and resp.get("payload", {}).get("code") == expected_code:
        return True
    return False

def recv_until_type(client, expected_type, timeout=3.0):
    start = time.time()
    while time.time() - start < timeout:
        env = client.recv_envelope(timeout=timeout)
        if env.get("type") == expected_type:
            return env
    raise TimeoutError(f"Timed out waiting for {expected_type}")

def register_and_login(server, email, name, avatar):
    client = WireClient("127.0.0.1", server.port)
    client.request("REGISTER", {
        "email": email,
        "password": "password123",
        "name": name,
        "avatar": avatar
    })
    resp = client.request("LOGIN", {
        "email": email,
        "password": "password123"
    })
    user_id = resp["payload"]["me"]["userId"]
    return client, user_id

class TestRooms(unittest.TestCase):
    def test_room_creation_and_join_broadcast(self):
        with ServerProcess(auth_limit=50) as server:
            client_a, uid_a = register_and_login(server, "alice@test.com", "Alice", "fox")
            client_b, uid_b = register_and_login(server, "bob@test.com", "Bob", "bear")
            try:
                # Alice creates room
                resp_create = client_a.request("ROOM_CREATE", {"mode": "classic"})
                self.assertEqual(resp_create["type"], "OK")
                room_code = resp_create["payload"].get("code") or resp_create["payload"].get("roomId")
                self.assertIsNotNone(room_code)
                self.assertEqual(len(room_code), 6)

                # Alice gets push ROOM_STATE (id: 0)
                state_a1 = recv_until_type(client_a, "ROOM_STATE")
                self.assertEqual(state_a1["id"], 0)
                payload_a1 = state_a1["payload"]
                self.assertGreaterEqual(payload_a1["revision"], 1)
                self.assertEqual(payload_a1["hostSeat"], 0)
                self.assertEqual(len(payload_a1["seats"]), 4)

                # Check privacy: no email, token, password, or hash leaked
                raw_json = json.dumps(state_a1)
                self.assertNotIn("alice@test.com", raw_json)
                self.assertNotIn("password", raw_json)
                self.assertNotIn("hash", raw_json)
                self.assertNotIn("token", raw_json)

                # Bob joins room by code
                resp_join = client_b.request("ROOM_JOIN", {"code": room_code})
                self.assertEqual(resp_join["type"], "OK")
                self.assertEqual(resp_join["payload"].get("seat"), 1)

                # Both Alice and Bob receive push ROOM_STATE with updated revision
                state_a2 = recv_until_type(client_a, "ROOM_STATE")
                state_b2 = recv_until_type(client_b, "ROOM_STATE")

                self.assertGreater(state_a2["payload"]["revision"], payload_a1["revision"])
                self.assertEqual(state_a2["payload"]["revision"], state_b2["payload"]["revision"])

                # Check seats in broadcast
                seats = state_b2["payload"]["seats"]
                self.assertIsNotNone(seats[0])
                self.assertIsNotNone(seats[1])
                self.assertEqual(seats[0]["userId"], uid_a)
                self.assertEqual(seats[1]["userId"], uid_b)
                self.assertIsNone(seats[2])
                self.assertIsNone(seats[3])

                # Check privacy again
                raw_b = json.dumps(state_b2)
                self.assertNotIn("bob@test.com", raw_b)
                self.assertNotIn("alice@test.com", raw_b)
                self.assertNotIn("token", raw_b)
            finally:
                client_a.close()
                client_b.close()

    def test_host_privileges_and_bots(self):
        with ServerProcess(auth_limit=50) as server:
            client_a, uid_a = register_and_login(server, "host@test.com", "HostUser", "fox")
            client_b, uid_b = register_and_login(server, "guest@test.com", "GuestUser", "bear")
            try:
                resp_create = client_a.request("ROOM_CREATE", {"mode": "classic"})
                room_code = resp_create["payload"].get("code") or resp_create["payload"].get("roomId")
                client_b.request("ROOM_JOIN", {"code": room_code})

                # Drain join broadcast on client_a
                recv_until_type(client_a, "ROOM_STATE")

                # Guest attempts host actions -> FORBIDDEN
                resp_mode = client_b.request("ROOM_MODE", {"mode": "draft"})
                self.assertTrue(is_error(resp_mode, "FORBIDDEN"), f"Expected FORBIDDEN, got {resp_mode}")

                resp_bot = client_b.request("ROOM_BOT", {"seat": 2, "level": "easy"})
                self.assertTrue(is_error(resp_bot, "FORBIDDEN"), f"Expected FORBIDDEN, got {resp_bot}")

                resp_kick = client_b.request("ROOM_KICK", {"seat": 0})
                self.assertTrue(is_error(resp_kick, "FORBIDDEN"), f"Expected FORBIDDEN, got {resp_kick}")

                # Host configures bot
                resp_host_bot = client_a.request("ROOM_BOT", {"seat": 2, "level": "hard"})
                self.assertEqual(resp_host_bot["type"], "OK")

                # Both receive updated ROOM_STATE with bot at seat 2
                while True:
                    state_a = recv_until_type(client_a, "ROOM_STATE")
                    if state_a["payload"]["seats"][2] is not None:
                        break
                seats = state_a["payload"]["seats"]
                self.assertTrue(seats[2]["isBot"])
                self.assertEqual(seats[2]["botLevel"].lower(), "hard")
            finally:
                client_a.close()
                client_b.close()

    def test_invite_flow(self):
        with ServerProcess(auth_limit=50) as server:
            client_a, uid_a = register_and_login(server, "inviter@test.com", "Inviter", "fox")
            client_b, uid_b = register_and_login(server, "invitee@test.com", "Invitee", "owl")
            try:
                resp_create = client_a.request("ROOM_CREATE", {"mode": "classic"})
                room_code = resp_create["payload"].get("code") or resp_create["payload"].get("roomId")

                # Drain initial push on client A
                recv_until_type(client_a, "ROOM_STATE")

                # Alice sends invite to Bob
                resp_inv = client_a.request("INVITE_SEND", {"userId": uid_b, "kind": "room"})
                self.assertEqual(resp_inv["type"], "OK")
                invite_id = resp_inv["payload"].get("inviteId")
                self.assertIsNotNone(invite_id)

                # Bob receives push INVITE
                push_inv = recv_until_type(client_b, "INVITE")
                self.assertEqual(push_inv["payload"]["inviteId"], invite_id)
                self.assertEqual(push_inv["payload"]["kind"], "room")

                # Bob accepts invite
                resp_reply = client_b.request("INVITE_REPLY", {"inviteId": invite_id, "accept": True})
                self.assertEqual(resp_reply["type"], "OK")

                # Both receive push ROOM_STATE showing Bob in room
                state_a = recv_until_type(client_a, "ROOM_STATE")
                state_b = recv_until_type(client_b, "ROOM_STATE")
                self.assertEqual(state_a["payload"]["seats"][1]["userId"], uid_b)
                self.assertEqual(state_b["payload"]["seats"][1]["userId"], uid_b)
            finally:
                client_a.close()
                client_b.close()

    def test_party_limits(self):
        with ServerProcess(auth_limit=50) as server:
            client_a, uid_a = register_and_login(server, "p1@test.com", "Party1", "fox")
            client_b, uid_b = register_and_login(server, "p2@test.com", "Party2", "bear")
            client_c, uid_c = register_and_login(server, "p3@test.com", "Party3", "cat")
            try:
                # A invites B to party
                resp_inv = client_a.request("INVITE_SEND", {"userId": uid_b, "kind": "party"})
                self.assertEqual(resp_inv["type"], "OK")
                inv_id = resp_inv["payload"]["inviteId"]

                push_inv = recv_until_type(client_b, "INVITE")
                self.assertEqual(push_inv["payload"]["inviteId"], inv_id)

                # B accepts
                resp_reply = client_b.request("INVITE_REPLY", {"inviteId": inv_id, "accept": True})
                self.assertEqual(resp_reply["type"], "OK")

                # Both receive PARTY_STATE
                pstate_a = recv_until_type(client_a, "PARTY_STATE")
                self.assertEqual(len(pstate_a["payload"]["members"]), 2)

                # Party is now full (max 2 members). Attempting to invite C to party fails
                resp_inv_c = client_a.request("INVITE_SEND", {"userId": uid_c, "kind": "party"})
                self.assertTrue(is_error(resp_inv_c, "BUSY") or is_error(resp_inv_c, "ROOM_FULL") or is_error(resp_inv_c, "INVALID_ACTION"))

                # Party leave
                resp_leave = client_b.request("PARTY_LEAVE", {})
                self.assertEqual(resp_leave["type"], "OK")
            finally:
                client_a.close()
                client_b.close()
                client_c.close()

    def test_race_for_last_seat(self):
        with ServerProcess(auth_limit=50) as server:
            host, uid_h = register_and_login(server, "host_race@test.com", "HostRace", "fox")
            c1, uid_1 = register_and_login(server, "joiner1@test.com", "Joiner1", "bear")
            c2, uid_2 = register_and_login(server, "joiner2@test.com", "Joiner2", "cat")
            try:
                # Host creates room
                resp = host.request("ROOM_CREATE", {"mode": "classic"})
                self.assertEqual(resp["type"], "OK")
                code = resp["payload"].get("code") or resp["payload"].get("roomId")

                # Host fills seat 1 and seat 2 with bots
                resp_b1 = host.request("ROOM_BOT", {"seat": 1, "level": "easy"})
                self.assertEqual(resp_b1["type"], "OK")
                resp_b2 = host.request("ROOM_BOT", {"seat": 2, "level": "normal"})
                self.assertEqual(resp_b2["type"], "OK")

                # Room now has 3 players: seat 0 (host), seat 1 (bot), seat 2 (bot).
                # Exactly 1 empty seat remains (seat 3).
                import concurrent.futures
                def do_join(c):
                    return c.request("ROOM_JOIN", {"code": code})

                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                    f1 = executor.submit(do_join, c1)
                    f2 = executor.submit(do_join, c2)
                    r1 = f1.result()
                    r2 = f2.result()

                results = [r1, r2]
                ok_count = sum(1 for r in results if r.get("type") == "OK")
                full_count = sum(1 for r in results if is_error(r, "ROOM_FULL"))

                self.assertEqual(ok_count, 1, f"Expected exactly 1 OK, got {results}")
                self.assertEqual(full_count, 1, f"Expected exactly 1 ROOM_FULL, got {results}")

                ok_resp = next(r for r in results if r.get("type") == "OK")
                self.assertEqual(ok_resp["payload"]["seat"], 3)
            finally:
                host.close()
                c1.close()
                c2.close()

    def test_party_leave_broadcast(self):
        with ServerProcess(auth_limit=50) as server:
            client_a, uid_a = register_and_login(server, "pa@test.com", "PartyA", "fox")
            client_b, uid_b = register_and_login(server, "pb@test.com", "PartyB", "bear")
            try:
                # A invites B to party
                resp_inv = client_a.request("INVITE_SEND", {"userId": uid_b, "kind": "party"})
                self.assertEqual(resp_inv["type"], "OK")
                inv_id = resp_inv["payload"]["inviteId"]

                push_inv = recv_until_type(client_b, "INVITE")
                self.assertEqual(push_inv["payload"]["inviteId"], inv_id)

                # B accepts
                resp_reply = client_b.request("INVITE_REPLY", {"inviteId": inv_id, "accept": True})
                self.assertEqual(resp_reply["type"], "OK")

                # Both receive PARTY_STATE with 2 members
                pstate_a = recv_until_type(client_a, "PARTY_STATE")
                pstate_b = recv_until_type(client_b, "PARTY_STATE")
                self.assertEqual(len(pstate_a["payload"]["members"]), 2)
                self.assertEqual(len(pstate_b["payload"]["members"]), 2)

                # B leaves party
                resp_leave = client_b.request("PARTY_LEAVE", {})
                self.assertEqual(resp_leave["type"], "OK")

                # Both receive disbanded PARTY_STATE with empty members
                pstate_leave_a = recv_until_type(client_a, "PARTY_STATE")
                pstate_leave_b = recv_until_type(client_b, "PARTY_STATE")
                self.assertEqual(len(pstate_leave_a["payload"]["members"]), 0)
                self.assertEqual(len(pstate_leave_b["payload"]["members"]), 0)
            finally:
                client_a.close()
                client_b.close()

if __name__ == "__main__":
    unittest.main()
