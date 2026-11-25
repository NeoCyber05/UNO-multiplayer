import os
import sqlite3
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
START_ELO = 1500
WINNER_ELO = 1516
LOSER_ELO = 1484
# MatchStatus enum in shared/contracts.h: MATCH_ACTIVE, MATCH_FINALIZED, MATCH_ABORTED.
MATCH_ABORTED = 2


def error_code(resp):
    if resp.get("type") != "ERROR":
        return None
    return resp.get("payload", {}).get("code")


def recv_queue_state(client, predicate, timeout=PUSH_TIMEOUT_S):
    """Skip QUEUE_STATE pushes until one satisfies predicate."""
    deadline = time.time() + timeout
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            raise TimeoutError("Timed out waiting for matching QUEUE_STATE")
        state = client.recv_event("QUEUE_STATE", timeout=remaining)["payload"]
        if predicate(state):
            return state


def relogin(server, client):
    """Log the same account in on a fresh socket and return its `me` object."""
    fresh = WireClient("127.0.0.1", server.port)
    try:
        resp = fresh.request("LOGIN", {"email": client.email, "password": client.password})
        assert resp["type"] == "OK", resp
        return resp["payload"]["me"]
    finally:
        fresh.close()


def close_all(clients):
    for client in clients:
        client.close()


def make_party(inviter, invitee, mode):
    sent = inviter.request("INVITE_SEND", {
        "userId": invitee.me["userId"], "kind": "party", "mode": mode
    })
    assert sent["type"] == "OK", sent
    invite = invitee.recv_event("INVITE", timeout=PUSH_TIMEOUT_S)["payload"]
    accepted = invitee.request("INVITE_REPLY", {"inviteId": invite["inviteId"], "accept": True})
    assert accepted["type"] == "OK", accepted
    for client in (inviter, invitee):
        client.recv_event("PARTY_STATE", timeout=PUSH_TIMEOUT_S)


def queue_classic_match(test, clients):
    """Queue four solo players for classic; returns the shared match id."""
    for client in clients:
        resp = client.request("QUEUE_JOIN", {"mode": "classic"})
        test.assertEqual(resp["type"], "OK", resp)
        test.assertEqual(resp["payload"]["queue"]["mode"], "classic")
        test.assertIs(resp["payload"]["queue"]["active"], True)
    states = [client.recv_event("GAME_STATE", timeout=PUSH_TIMEOUT_S)["payload"]
              for client in clients]
    for client, state in zip(clients, states):
        client.game_state = state
    match_ids = {state["matchId"] for state in states}
    test.assertEqual(len(match_ids), 1)
    match_id = match_ids.pop()
    for client in clients:
        done = recv_queue_state(client, lambda state: state["active"] is False)
        test.assertEqual(done["mode"], "classic")
        test.assertEqual(done["matchId"], match_id)
    return match_id


def assert_ranked_result(test, result):
    test.assertIs(result["ranked"], True)
    winner = result["winnerSeat"]
    test.assertGreaterEqual(winner, 0)
    for player in result["players"]:
        test.assertIs(player["isBot"], False)
        test.assertEqual(player["eloBefore"], START_ELO)
        expected = WINNER_ELO if player["seat"] == winner else LOSER_ELO
        test.assertEqual(player["eloAfter"], expected)
    return {player["userId"]: player["eloAfter"] for player in result["players"]}


class TestRanking(unittest.TestCase):
    def test_queue_ranked_match(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients = login_many(server, SEAT_COUNT)
            try:
                queue_classic_match(self, clients)
                result = play_until_result(clients, timeout=MATCH_TIMEOUT_S)
                elo_by_user = assert_ranked_result(self, result)
                for client in clients:
                    me = relogin(server, client)
                    self.assertEqual(me["elo"], elo_by_user[client.me["userId"]])
            finally:
                close_all(clients)

    def test_queue_then_room_busy(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            queued, roomed = login_many(server, 2)
            try:
                joined = queued.request("QUEUE_JOIN", {"mode": "classic"})
                self.assertEqual(joined["type"], "OK", joined)
                players = joined["payload"]["queue"]["players"]
                self.assertEqual([player["userId"] for player in players],
                                 [queued.me["userId"]])
                self.assertEqual(error_code(queued.request("QUEUE_JOIN", {"mode": "draft"})),
                                 "BUSY")
                self.assertEqual(error_code(queued.request("ROOM_CREATE", {"mode": "classic"})),
                                 "BUSY")

                created = roomed.request("ROOM_CREATE", {"mode": "classic"})
                self.assertEqual(created["type"], "OK", created)
                code = created["payload"]["roomId"]
                self.assertEqual(error_code(queued.request("ROOM_JOIN", {"code": code})), "BUSY")
                self.assertEqual(error_code(roomed.request("QUEUE_JOIN", {"mode": "classic"})),
                                 "BUSY")
                self.assertEqual(error_code(roomed.request("QUEUE_JOIN", {"mode": "bogus"})),
                                 "BAD_REQUEST")

                left = queued.request("QUEUE_LEAVE", {})
                self.assertEqual(left["type"], "OK", left)
                gone = recv_queue_state(queued, lambda state: state["active"] is False)
                self.assertEqual(gone["mode"], "classic")
                rejoined = queued.request("ROOM_JOIN", {"code": code})
                self.assertEqual(rejoined["type"], "OK", rejoined)
            finally:
                close_all([queued, roomed])

    def test_disconnect_removes_party_entry(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            leader, partner = login_many(server, 2)
            try:
                make_party(leader, partner, "2v2")
                self.assertEqual(error_code(partner.request("QUEUE_JOIN", {"mode": "classic"})),
                                 "INVALID_ACTION")
                joined = partner.request("QUEUE_JOIN", {"mode": "2v2"})
                self.assertEqual(joined["type"], "OK", joined)
                ids = [player["userId"] for player in joined["payload"]["queue"]["players"]]
                self.assertEqual(sorted(ids), sorted([leader.me["userId"], partner.me["userId"]]))
                waiting = recv_queue_state(leader, lambda state: state["active"] is True)
                self.assertEqual(waiting["mode"], "2v2")
                self.assertEqual(error_code(leader.request("QUEUE_JOIN", {"mode": "2v2"})),
                                 "BUSY")

                leader.close()
                gone = recv_queue_state(partner, lambda state: state["active"] is False)
                self.assertEqual(gone["mode"], "2v2")
            finally:
                close_all([leader, partner])

    def test_party_leave_removes_party_entry(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            leader, partner, solo = login_many(server, 3)
            try:
                make_party(leader, partner, "side")
                self.assertEqual(leader.request("QUEUE_JOIN", {"mode": "side"})["type"], "OK")
                self.assertEqual(solo.request("QUEUE_JOIN", {"mode": "side"})["type"], "OK")
                left = partner.request("PARTY_LEAVE", {})
                self.assertEqual(left["type"], "OK", left)
                for client in (leader, partner):
                    recv_queue_state(client, lambda state: state["active"] is False)
                remaining = recv_queue_state(
                    solo, lambda state: state["active"] is True and len(state["players"]) == 1)
                self.assertEqual(remaining["players"][0]["userId"], solo.me["userId"])
            finally:
                close_all([leader, partner, solo])

    def test_restart_keeps_leaderboard(self):
        with tempfile.NamedTemporaryFile(suffix=".db", delete=False) as tmp:
            db_path = tmp.name
        try:
            with ServerProcess(db=db_path, auth_limit=AUTH_LIMIT) as server:
                clients = login_many(server, SEAT_COUNT)
                try:
                    queue_classic_match(self, clients)
                    result = play_until_result(clients, timeout=MATCH_TIMEOUT_S)
                    elo_by_user = assert_ranked_result(self, result)
                finally:
                    close_all(clients)

            # Same DB after a restart: ELO survived, then a ranked match is cut by SIGTERM.
            with ServerProcess(db=db_path, auth_limit=AUTH_LIMIT) as server:
                for client in clients:
                    reconnected = WireClient("127.0.0.1", server.port)
                    resp = reconnected.request("LOGIN", {
                        "email": client.email, "password": client.password
                    })
                    self.assertEqual(resp["type"], "OK", resp)
                    self.assertEqual(resp["payload"]["me"]["elo"],
                                     elo_by_user[client.me["userId"]])
                    reconnected.email = client.email
                    reconnected.password = client.password
                    reconnected.me = resp["payload"]["me"]
                    client.successor = reconnected
                successors = [client.successor for client in clients]
                try:
                    aborted_id = queue_classic_match(self, successors)
                    server.stop()
                finally:
                    close_all(successors)

            db = sqlite3.connect(db_path)
            try:
                row = db.execute("SELECT ranked, status FROM matches WHERE match_id = ?",
                                 (aborted_id,)).fetchone()
            finally:
                db.close()
            self.assertEqual(row, (1, MATCH_ABORTED))

            with ServerProcess(db=db_path, auth_limit=AUTH_LIMIT) as server:
                for client in clients:
                    me = relogin(server, client)
                    self.assertEqual(me["elo"], elo_by_user[client.me["userId"]])
        finally:
            # WAL mode leaves -wal/-shm siblings next to the database file.
            for path in (db_path, db_path + "-wal", db_path + "-shm"):
                if os.path.exists(path):
                    os.remove(path)


if __name__ == "__main__":
    unittest.main()
