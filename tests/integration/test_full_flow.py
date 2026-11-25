import os
import tempfile
import unittest
from tests.integration.helpers import ServerProcess, WireClient, login_many, play_until_result
from tests.integration.test_ranking import make_party, recv_queue_state
from tests.integration.test_social import assert_ok, friend_row, make_friends

AUTH_LIMIT = 100
SEAT_COUNT = 4
PUSH_TIMEOUT_S = 3.0
MATCH_TIMEOUT_S = 60
START_ELO = 1500
TEAM_MODE = "2v2"


def close_all(clients):
    for client in clients:
        client.close()


def remove_db(db_path):
    # WAL mode leaves -wal/-shm siblings next to the database file.
    for path in (db_path, db_path + "-wal", db_path + "-shm"):
        if os.path.exists(path):
            os.remove(path)


def won(result, player):
    return player["team"] == result["winnerTeam"]


class TestFullFlow(unittest.TestCase):
    def queue_team_match(self, leader, partner, solos):
        """Party (leader, partner) and two solo players queue 2v2; returns the match id."""
        assert_ok(self, leader.request("QUEUE_JOIN", {"mode": TEAM_MODE}))
        for solo in solos:
            assert_ok(self, solo.request("QUEUE_JOIN", {"mode": TEAM_MODE}))
        clients = [leader, partner] + solos
        states = [client.recv_event("GAME_STATE", timeout=PUSH_TIMEOUT_S)["payload"]
                  for client in clients]
        for client, state in zip(clients, states):
            client.game_state = state
        match_ids = {state["matchId"] for state in states}
        self.assertEqual(len(match_ids), 1)
        match_id = match_ids.pop()
        for client in clients:
            done = recv_queue_state(client, lambda state: state["active"] is False)
            self.assertEqual(done["matchId"], match_id)
        return match_id

    def assert_profile(self, profile, result, player):
        """One ranked 2v2 match: stats, history and badges all come from the server result."""
        winner = won(result, player)
        stats = profile["stats"]
        self.assertEqual(profile["elo"], player["eloAfter"])
        self.assertEqual(stats["matches"], 1)
        self.assertEqual(stats["wins"], int(winner))
        self.assertEqual(stats["teamWins"], int(winner))
        self.assertEqual(stats["streak"], 1 if winner else -1)
        self.assertEqual(stats["peakElo"], max(START_ELO, player["eloAfter"]))
        modes = {mode["key"]: mode for mode in profile["modes"]}
        self.assertEqual(modes[TEAM_MODE]["matches"], 1)
        self.assertEqual(modes["classic"]["matches"], 0)
        self.assertEqual(len(profile["history"]), 1)
        row = profile["history"][0]
        self.assertEqual(row["matchId"], result["matchId"])
        self.assertEqual(row["mode"], TEAM_MODE)
        self.assertIs(row["won"], winner)
        self.assertEqual(row["delta"], player["eloAfter"] - player["eloBefore"])
        self.assertEqual(profile["badges"], ["first-win"] if winner else [])

    def test_register_to_restart(self):
        with tempfile.NamedTemporaryFile(suffix=".db", delete=False) as tmp:
            db_path = tmp.name
        try:
            with ServerProcess(db=db_path, auth_limit=AUTH_LIMIT) as server:
                clients = login_many(server, SEAT_COUNT)
                leader, partner, *solos = clients
                try:
                    make_friends(self, leader, partner)
                    make_party(leader, partner, TEAM_MODE)
                    match_id = self.queue_team_match(leader, partner, solos)

                    result = play_until_result(clients, timeout=MATCH_TIMEOUT_S)
                    self.assertEqual(result["matchId"], match_id)
                    self.assertEqual(result["mode"], TEAM_MODE)
                    self.assertIs(result["ranked"], True)
                    for client in clients:
                        pushed = client.recv_event("MATCH_RESULT")["payload"]
                        self.assertEqual(pushed["matchId"], match_id)
                    by_user = {player["userId"]: player for player in result["players"]}
                    # The party must be placed on one team.
                    self.assertEqual(by_user[leader.me["userId"]]["team"],
                                     by_user[partner.me["userId"]]["team"])
                    self.assertTrue(any(won(result, player) for player in result["players"]))

                    for client in clients:
                        profile = assert_ok(self, client.request("PROFILE_GET", {}))["profile"]
                        self.assert_profile(profile, result, by_user[client.me["userId"]])
                    entries = assert_ok(self, leader.request("LEADERBOARD_GET", {}))["entries"]
                    self.assertEqual({entry["userId"]: entry["elo"] for entry in entries},
                                     {uid: player["eloAfter"] for uid, player in by_user.items()})
                finally:
                    close_all(clients)

            # Everything above must come back from the database alone.
            with ServerProcess(db=db_path, auth_limit=AUTH_LIMIT) as server:
                for client in clients:
                    with WireClient("127.0.0.1", server.port) as fresh:
                        me = assert_ok(self, fresh.request("LOGIN", {
                            "email": client.email, "password": client.password
                        }))["me"]
                        player = by_user[client.me["userId"]]
                        self.assertEqual(me["elo"], player["eloAfter"])
                        profile = assert_ok(self, fresh.request("PROFILE_GET", {}))["profile"]
                        self.assert_profile(profile, result, player)
                        if client is leader:
                            listed = assert_ok(self, fresh.request("FRIEND_LIST", {}))
                            row = friend_row(listed, partner.me["userId"])
                            self.assertIsNotNone(row, listed)
                            self.assertIs(row["pending"], False)
        finally:
            remove_db(db_path)


if __name__ == "__main__":
    unittest.main()
