import socket
import time
import unittest
from tests.integration.helpers import (
    ServerProcess, WireClient, login_many, play_until_result, pump_events
)

AUTH_LIMIT = 100
RECV_TIMEOUT_S = 3.0
SEAT_COUNT = 4
HEARTBEAT_MS = "500"
HEARTBEAT_CLOSE_WITHIN_S = 2.0
HEARTBEAT_ALIVE_FOR_S = 2.0
PING_INTERVAL_S = 0.2
QUIET_WINDOW_S = 0.5


def error_code(resp):
    if resp.get("type") != "ERROR":
        return None
    return resp.get("payload", {}).get("code")


def recv_game_state(client, predicate=lambda state: True, timeout=RECV_TIMEOUT_S):
    deadline = time.time() + timeout
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            raise TimeoutError("Timed out waiting for matching GAME_STATE")
        state = client.recv_event("GAME_STATE", timeout=remaining)["payload"]
        if predicate(state):
            return state


def wait_for_eof(sock, timeout):
    """Return True once the peer closes the socket within timeout seconds."""
    deadline = time.time() + timeout
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            return False
        sock.settimeout(remaining)
        try:
            if sock.recv(65536) == b"":
                return True
        except (ConnectionResetError, BrokenPipeError):
            return True
        except socket.timeout:
            return False


def start_classic_match(server):
    """Four humans in one classic room; returns (clients, initial GAME_STATE per seat)."""
    clients = login_many(server, SEAT_COUNT)
    host = clients[0]
    created = host.request("ROOM_CREATE", {"mode": "classic"})
    assert created["type"] == "OK", created
    code = created["payload"]["roomId"]
    for client in clients[1:]:
        joined = client.request("ROOM_JOIN", {"code": code})
        assert joined["type"] == "OK", joined
    started = host.request("ROOM_START", {})
    assert started["type"] == "OK", started
    states = [recv_game_state(client) for client in clients]
    for client, state in zip(clients, states):
        client.game_state = state
    return clients, states


def close_all(clients):
    for client in clients:
        client.close()


class TestMatch(unittest.TestCase):
    def test_room_start_sends_private_views(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients, states = start_classic_match(server)
            try:
                match_ids = {state["matchId"] for state in states}
                self.assertEqual(len(match_ids), 1)
                for seat, state in enumerate(states):
                    self.assertEqual(state["viewerSeat"], seat)
                    self.assertEqual(state["phase"], "PLAYING")
                    for player in state["players"]:
                        if player["seat"] == seat:
                            self.assertIn("hand", player)
                        else:
                            self.assertNotIn("hand", player)
            finally:
                close_all(clients)

    def test_play_ok_and_state(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients, states = start_classic_match(server)
            try:
                turn = states[0]["turn"]
                actor = clients[turn]
                before = states[turn]
                req_id = actor.next_id
                actor.next_id += 1
                actor.send_envelope(req_id, "DRAW", {
                    "matchId": before["matchId"], "turnNo": before["turnNo"]
                })
                got_ok = False
                newer_state = None
                deadline = time.time() + RECV_TIMEOUT_S
                while not (got_ok and newer_state):
                    remaining = deadline - time.time()
                    self.assertGreater(remaining, 0, "missing OK or GAME_STATE after DRAW")
                    env = actor.recv_envelope(timeout=remaining)
                    if env.get("id") == req_id:
                        self.assertEqual(env["type"], "OK", env)
                        got_ok = True
                    elif env.get("type") == "GAME_STATE":
                        if env["payload"]["revision"] > before["revision"]:
                            newer_state = env["payload"]
                self.assertEqual(newer_state["viewerSeat"], turn)
            finally:
                close_all(clients)

    def test_full_match_reaches_result(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients, states = start_classic_match(server)
            try:
                result = play_until_result(clients, timeout=60)
                self.assertIs(result["ranked"], False)
                self.assertEqual(len(result["players"]), SEAT_COUNT)
                self.assertEqual(result["matchId"], states[0]["matchId"])
                for client in clients:
                    env = client.recv_event("MATCH_RESULT")
                    self.assertEqual(env["payload"]["matchId"], result["matchId"])
            finally:
                close_all(clients)

    def test_spoof_and_fake_opcodes(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients, states = start_classic_match(server)
            stranger = WireClient("127.0.0.1", server.port)
            try:
                turn = states[0]["turn"]
                state = states[turn]
                hand = state["players"][turn]["hand"]
                spoofed = clients[turn].request("PLAY", {
                    "matchId": state["matchId"], "turnNo": state["turnNo"],
                    "cardId": hand[0]["id"], "seat": 0
                })
                self.assertEqual(error_code(spoofed), "BAD_REQUEST", spoofed)
                for fake in ("TIMEOUT", "UNO_PENALTY"):
                    resp = clients[turn].request(fake, {})
                    self.assertEqual(error_code(resp), "BAD_REQUEST", resp)
                anonymous = stranger.request("DRAW", {
                    "matchId": state["matchId"], "turnNo": state["turnNo"]
                })
                self.assertEqual(error_code(anonymous), "UNAUTHENTICATED", anonymous)
            finally:
                stranger.close()
                close_all(clients)

    def test_stale_request_id(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            with WireClient("127.0.0.1", server.port) as client:
                expected = [(5, None), (5, "STALE_REQUEST"), (4, "STALE_REQUEST"), (6, None)]
                for req_id, code in expected:
                    client.send_envelope(req_id, "PING", {})
                    resp = client.recv_envelope()
                    self.assertEqual(resp["id"], req_id)
                    if code is None:
                        self.assertEqual(resp["type"], "OK", resp)
                    else:
                        self.assertEqual(error_code(resp), code, resp)

    def test_resume_reclaims_seat(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients, _ = start_classic_match(server)
            replacement = None
            try:
                token = clients[2].token
                clients[2].close()
                for seat in (0, 1, 3):
                    recv_game_state(
                        clients[seat], lambda state: state["players"][2]["connected"] is False)
                replacement = WireClient("127.0.0.1", server.port)
                resumed = replacement.request("RESUME", {"token": token})
                self.assertEqual(resumed["type"], "OK", resumed)
                self.assertNotIn("token", resumed["payload"])
                state = recv_game_state(replacement)
                self.assertEqual(state["viewerSeat"], 2)
                self.assertIn("hand", state["players"][2])
            finally:
                if replacement:
                    replacement.close()
                close_all(clients)

    def test_relogin_keeps_seat(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients, _ = start_classic_match(server)
            fresh = WireClient("127.0.0.1", server.port)
            try:
                old = clients[1]
                logged_in = fresh.request("LOGIN", {"email": old.email, "password": old.password})
                self.assertEqual(logged_in["type"], "OK", logged_in)
                self.assertTrue(wait_for_eof(old.sock, RECV_TIMEOUT_S),
                                "old socket should be closed by relogin")
                state = recv_game_state(fresh)
                self.assertEqual(state["viewerSeat"], 1)
                time.sleep(QUIET_WINDOW_S)
                for seat in (0, 2, 3):
                    pump_events(clients[seat])
                    types = [env.get("type") for env in clients[seat]._queue]
                    self.assertNotIn("MATCH_RESULT", types)
            finally:
                fresh.close()
                close_all(clients)

    def test_heartbeat_timeout(self):
        extra = ["--heartbeat-ms", HEARTBEAT_MS]
        with ServerProcess(auth_limit=AUTH_LIMIT, extra_args=extra) as server:
            with WireClient("127.0.0.1", server.port) as silent:
                self.assertTrue(wait_for_eof(silent.sock, HEARTBEAT_CLOSE_WITHIN_S),
                                "silent client should be closed by heartbeat")
            with WireClient("127.0.0.1", server.port) as chatty:
                deadline = time.time() + HEARTBEAT_ALIVE_FOR_S
                while time.time() < deadline:
                    resp = chatty.request("PING", {})
                    self.assertEqual(resp["type"], "OK", resp)
                    time.sleep(PING_INTERVAL_S)
                resp = chatty.request("PING", {})
                self.assertEqual(resp["type"], "OK", resp)

    def test_leave_then_busy(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients, states = start_classic_match(server)
            try:
                left = clients[0].request("MATCH_LEAVE", {"matchId": states[0]["matchId"]})
                self.assertEqual(left["type"], "OK", left)
                created = clients[0].request("ROOM_CREATE", {"mode": "classic"})
                self.assertEqual(error_code(created), "BUSY", created)
            finally:
                close_all(clients)


if __name__ == "__main__":
    unittest.main()
