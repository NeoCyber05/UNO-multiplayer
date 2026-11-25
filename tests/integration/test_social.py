import os
import tempfile
import time
import unittest
from tests.integration.helpers import (
    ServerProcess, WireClient, login_many, play_until_result
)

AUTH_LIMIT = 100
SEAT_COUNT = 4
PUSH_TIMEOUT_S = 3.0
MATCH_TIMEOUT_S = 60
WINNER_ELO = 1516
LOSER_ELO = 1484
BOT_SEATS = (1, 2, 3)
SECRET_KEYS = ("email", "hash", "token")
# 16 code points but more than 16 bytes, so the limit must count code points.
VIETNAMESE_NAME = "Nguyễn Văn Thành"
TOO_LONG_NAME = VIETNAMESE_NAME + "!"
NEW_AVATAR = "owl"


def error_code(resp):
    if resp.get("type") != "ERROR":
        return None
    return resp.get("payload", {}).get("code")


def assert_ok(test, resp):
    test.assertEqual(resp.get("type"), "OK", resp)
    return resp["payload"]


def recv_until(client, event_type, predicate, timeout=PUSH_TIMEOUT_S):
    """Skip pushes of event_type until one satisfies predicate."""
    deadline = time.time() + timeout
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            raise TimeoutError(f"Timed out waiting for matching {event_type}")
        env = client.recv_event(event_type, timeout=remaining)
        if predicate(env["payload"]):
            return env["payload"]


def friend_row(friends_payload, user_id):
    for row in friends_payload["friends"]:
        if row["userId"] == user_id:
            return row
    return None


def wait_friend(client, user_id, predicate):
    """Wait for a FRIEND_STATE whose row for user_id satisfies predicate."""
    def matches(payload):
        row = friend_row(payload, user_id)
        return row is not None and predicate(row)
    return friend_row(recv_until(client, "FRIEND_STATE", matches), user_id)


def make_friends(test, first, second):
    assert_ok(test, first.request("FRIEND_REQUEST", {"tag": second.me["tag"]}))
    incoming = wait_friend(second, first.me["userId"], lambda row: row["pending"])
    assert_ok(test, second.request("FRIEND_REPLY", {
        "requestId": incoming["requestId"], "accept": True
    }))
    wait_friend(first, second.me["userId"], lambda row: not row["pending"])
    wait_friend(second, first.me["userId"], lambda row: not row["pending"])


def find_secret_keys(value, path="$"):
    """Return every path whose key names a secret, at any depth."""
    found = []
    if isinstance(value, dict):
        for key, child in value.items():
            child_path = f"{path}.{key}"
            if key in SECRET_KEYS:
                found.append(child_path)
            found.extend(find_secret_keys(child, child_path))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            found.extend(find_secret_keys(child, f"{path}[{index}]"))
    return found


def close_all(clients):
    for client in clients:
        client.close()


def relogin(test, server, client):
    fresh = WireClient("127.0.0.1", server.port)
    payload = assert_ok(test, fresh.request("LOGIN", {
        "email": client.email, "password": client.password
    }))
    fresh.me = payload["me"]
    return fresh


class TestSocial(unittest.TestCase):
    def test_friend_flow(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            alice, bob = login_many(server, 2)
            try:
                assert_ok(self, alice.request("FRIEND_REQUEST", {"tag": bob.me["tag"]}))
                incoming = wait_friend(bob, alice.me["userId"], lambda row: row["pending"])
                self.assertIs(incoming["incoming"], True)
                self.assertEqual(incoming["requestId"], alice.me["userId"])
                self.assertEqual(incoming["tag"], alice.me["tag"])
                outgoing = wait_friend(alice, bob.me["userId"], lambda row: row["pending"])
                self.assertIs(outgoing["incoming"], False)
                self.assertNotIn("requestId", outgoing)

                assert_ok(self, bob.request("FRIEND_REPLY", {
                    "requestId": incoming["requestId"], "accept": True
                }))
                seen_by_alice = wait_friend(alice, bob.me["userId"],
                                            lambda row: not row["pending"])
                self.assertIs(seen_by_alice["online"], True)
                seen_by_bob = wait_friend(bob, alice.me["userId"],
                                          lambda row: not row["pending"])
                self.assertNotIn("requestId", seen_by_bob)

                listed = assert_ok(self, alice.request("FRIEND_LIST", {}))
                self.assertIs(friend_row(listed, bob.me["userId"])["pending"], False)

                self.assertEqual(error_code(alice.request(
                    "FRIEND_REQUEST", {"tag": bob.me["tag"]})), "INVALID_ACTION")
                self.assertEqual(error_code(bob.request(
                    "FRIEND_REQUEST", {"tag": alice.me["tag"]})), "INVALID_ACTION")
                self.assertEqual(error_code(alice.request(
                    "FRIEND_REQUEST", {"tag": alice.me["tag"]})), "BAD_REQUEST")
                self.assertEqual(error_code(alice.request(
                    "FRIEND_REQUEST", {"tag": "UNO# 1"})), "BAD_REQUEST")
            finally:
                close_all([alice, bob])

    def test_presence_updates(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            alice, bob = login_many(server, 2)
            try:
                make_friends(self, alice, bob)
                bob_id = bob.me["userId"]
                listed = assert_ok(self, alice.request("FRIEND_LIST", {}))
                self.assertEqual(friend_row(listed, bob_id)["presence"], "idle")

                assert_ok(self, bob.request("ROOM_CREATE", {"mode": "classic"}))
                wait_friend(alice, bob_id, lambda row: row["presence"] == "online")

                for seat in BOT_SEATS:
                    assert_ok(self, bob.request("ROOM_BOT", {"seat": seat, "level": "easy"}))
                assert_ok(self, bob.request("ROOM_START", {}))
                wait_friend(alice, bob_id, lambda row: row["presence"] == "in_match")

                bob.close()
                gone = wait_friend(alice, bob_id, lambda row: row["presence"] == "offline")
                self.assertIs(gone["online"], False)
            finally:
                close_all([alice, bob])

    def test_abort_presence_goes_straight_offline(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            alice, bob = login_many(server, 2)
            try:
                make_friends(self, alice, bob)
                bob_id = bob.me["userId"]
                assert_ok(self, bob.request("ROOM_CREATE", {"mode": "classic"}))
                for seat in BOT_SEATS:
                    assert_ok(self, bob.request("ROOM_BOT", {"seat": seat, "level": "easy"}))
                assert_ok(self, bob.request("ROOM_START", {}))
                wait_friend(alice, bob_id, lambda row: row["presence"] == "in_match")

                # The last human leaving aborts the match; friends must not see
                # a brief online/idle between in_match and offline.
                bob.close()
                after = wait_friend(alice, bob_id, lambda row: row["presence"] != "in_match")
                self.assertEqual(after["presence"], "offline")
            finally:
                close_all([alice, bob])

    def test_profile_privacy(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            alice, bob = login_many(server, 2)
            try:
                payload = assert_ok(self, alice.request("PROFILE_GET",
                                                        {"userId": bob.me["userId"]}))
                self.assertEqual(find_secret_keys(payload), [])
                profile = payload["profile"]
                self.assertEqual(profile["userId"], bob.me["userId"])
                self.assertEqual(profile["tag"], bob.me["tag"])
                self.assertEqual(profile["rank"], "gold")
                self.assertEqual([mode["key"] for mode in profile["modes"]],
                                 ["classic", "2v2", "side", "draft"])
                self.assertEqual(profile["history"], [])
                self.assertEqual(profile["badges"], [])
                self.assertEqual(profile["stats"]["matches"], 0)

                own = assert_ok(self, alice.request("PROFILE_GET", {}))
                self.assertEqual(own["profile"]["userId"], alice.me["userId"])
                self.assertEqual(error_code(alice.request(
                    "PROFILE_GET", {"userId": "999999"})), "NOT_FOUND")
            finally:
                close_all([alice, bob])

    def test_profile_update_rules(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            alice, bob = login_many(server, 2)
            try:
                code = assert_ok(self, alice.request("ROOM_CREATE", {"mode": "classic"}))["code"]
                assert_ok(self, bob.request("ROOM_JOIN", {"code": code}))

                self.assertEqual(error_code(alice.request("PROFILE_UPDATE", {
                    "userId": bob.me["userId"], "name": "Hacker", "avatar": "fox"
                })), "FORBIDDEN")
                self.assertEqual(error_code(alice.request("PROFILE_UPDATE", {
                    "name": TOO_LONG_NAME, "avatar": "fox"
                })), "BAD_REQUEST")
                self.assertEqual(error_code(alice.request("PROFILE_UPDATE", {
                    "name": "Alice", "avatar": "not-an-avatar"
                })), "BAD_REQUEST")

                updated = assert_ok(self, alice.request("PROFILE_UPDATE", {
                    "name": VIETNAMESE_NAME, "avatar": NEW_AVATAR
                }))
                self.assertEqual(updated["profile"]["name"], VIETNAMESE_NAME)
                pushed = alice.recv_event("PROFILE_STATE", timeout=PUSH_TIMEOUT_S)["payload"]
                self.assertEqual(pushed["name"], VIETNAMESE_NAME)
                self.assertEqual(pushed["avatar"], NEW_AVATAR)

                def renamed(state):
                    return any(seat and seat.get("userId") == alice.me["userId"]
                               and seat["name"] == VIETNAMESE_NAME
                               and seat["avatar"] == NEW_AVATAR
                               for seat in state["seats"])
                recv_until(bob, "ROOM_STATE", renamed)
            finally:
                close_all([alice, bob])

    def test_persist_after_restart(self):
        with tempfile.NamedTemporaryFile(suffix=".db", delete=False) as tmp:
            db_path = tmp.name
        try:
            with ServerProcess(db=db_path, auth_limit=AUTH_LIMIT) as server:
                alice, bob = login_many(server, 2)
                try:
                    make_friends(self, alice, bob)
                    assert_ok(self, alice.request("PROFILE_UPDATE", {
                        "name": alice.me["name"], "avatar": NEW_AVATAR
                    }))
                finally:
                    close_all([alice, bob])

            with ServerProcess(db=db_path, auth_limit=AUTH_LIMIT) as server:
                fresh = relogin(self, server, alice)
                try:
                    listed = assert_ok(self, fresh.request("FRIEND_LIST", {}))
                    row = friend_row(listed, bob.me["userId"])
                    self.assertIsNotNone(row, listed)
                    self.assertIs(row["pending"], False)
                    self.assertEqual(row["presence"], "offline")
                    profile = assert_ok(self, fresh.request("PROFILE_GET", {}))["profile"]
                    self.assertEqual(profile["avatar"], NEW_AVATAR)
                    self.assertEqual(profile["name"], alice.me["name"])
                    lobby = assert_ok(self, fresh.request("LOBBY_GET", {}))
                    self.assertEqual(lobby["friends"], listed["friends"])
                    self.assertEqual(len(lobby["leaderboard"]), 2)
                finally:
                    fresh.close()
        finally:
            # WAL mode leaves -wal/-shm siblings next to the database file.
            for path in (db_path, db_path + "-wal", db_path + "-shm"):
                if os.path.exists(path):
                    os.remove(path)

    def test_resume_and_login_resync(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            host, queued = login_many(server, 2)
            extra = []
            try:
                code = assert_ok(self, host.request("ROOM_CREATE", {"mode": "classic"}))["code"]
                assert_ok(self, queued.request("QUEUE_JOIN", {"mode": "classic"}))

                # RESUME on a second socket moves the session there; that
                # socket has seen no snapshot yet.
                resumed_host = WireClient("127.0.0.1", server.port)
                extra.append(resumed_host)
                assert_ok(self, resumed_host.request("RESUME", {"token": host.token}))
                room = resumed_host.recv_event("ROOM_STATE", timeout=PUSH_TIMEOUT_S)["payload"]
                self.assertEqual(room["code"], code)
                resumed_host.recv_event("FRIEND_STATE", timeout=PUSH_TIMEOUT_S)

                resumed_queue = WireClient("127.0.0.1", server.port)
                extra.append(resumed_queue)
                assert_ok(self, resumed_queue.request("RESUME", {"token": queued.token}))
                state = resumed_queue.recv_event("QUEUE_STATE", timeout=PUSH_TIMEOUT_S)["payload"]
                self.assertIs(state["active"], True)
                self.assertEqual(state["mode"], "classic")

                relogged = relogin(self, server, queued)
                extra.append(relogged)
                state = relogged.recv_event("QUEUE_STATE", timeout=PUSH_TIMEOUT_S)["payload"]
                self.assertIs(state["active"], True)
                self.assertEqual([player["userId"] for player in state["players"]],
                                 [queued.me["userId"]])
            finally:
                close_all([host, queued] + extra)

    def test_leaderboard(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients = login_many(server, SEAT_COUNT)
            try:
                for client in clients:
                    assert_ok(self, client.request("QUEUE_JOIN", {"mode": "classic"}))
                play_until_result(clients, timeout=MATCH_TIMEOUT_S)

                entries = assert_ok(self, clients[0].request("LEADERBOARD_GET", {}))["entries"]
                self.assertEqual(len(entries), SEAT_COUNT)
                self.assertEqual(entries[0]["elo"], WINNER_ELO)
                self.assertEqual(entries[0]["rank"], "gold")
                losers = entries[1:]
                self.assertEqual([entry["elo"] for entry in losers], [LOSER_ELO] * 3)
                loser_ids = [int(entry["userId"]) for entry in losers]
                self.assertEqual(loser_ids, sorted(loser_ids))
                for entry in entries:
                    self.assertEqual(entry["tag"], "UNO#" + entry["userId"])
                    self.assertEqual(find_secret_keys(entry), [])
            finally:
                close_all(clients)


if __name__ == "__main__":
    unittest.main()
