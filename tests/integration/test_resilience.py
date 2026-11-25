import json
import os
import socket
import sqlite3
import struct
import tempfile
import time
import unittest
from tests.integration.helpers import (
    ServerProcess, WireClient, login_many, play_until_result, pump_events
)
from tests.integration.test_match import recv_game_state, start_classic_match, wait_for_eof
from tests.integration.test_social import assert_ok

AUTH_LIMIT = 100
SEAT_COUNT = 4
PUSH_TIMEOUT_S = 3.0
MATCH_TIMEOUT_S = 60
# Must match the server's MAX_ROOMS and MAX_MATCHES.
MAX_ROOMS = 16
# APP_DEFAULT_HEARTBEAT_MS is 15 s; the lower bound proves it is not shorter.
HEARTBEAT_NOT_BEFORE_S = 10.0
HEARTBEAT_CLOSE_WITHIN_S = 10.0
KEEPALIVE_PING_S = 2.0
# Pauses between pieces of one frame, long enough for the server to see each piece alone.
FRAGMENT_GAP_S = 0.05
# Server retries a paused match every STORAGE_RETRY_MS (1 s).
UNPAUSE_TIMEOUT_S = 5.0
KILLED_SEAT = 1


def error_code(resp):
    if resp.get("type") != "ERROR":
        return None
    return resp.get("payload", {}).get("code")


def close_all(clients):
    for client in clients:
        client.close()


def remove_db(db_path):
    # WAL mode leaves -wal/-shm siblings next to the database file.
    for path in (db_path, db_path + "-wal", db_path + "-shm"):
        if os.path.exists(path):
            os.remove(path)


def kill_socket(client):
    """Close with RST instead of FIN, like a process killed without a clean shutdown."""
    client.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    client.close()


def frame(req_id, msg_type, payload):
    body = json.dumps({"v": 1, "id": req_id, "type": msg_type, "payload": payload},
                      separators=(",", ":")).encode("utf-8")
    return struct.pack(">I", len(body)) + body


def current_turn(clients):
    """(client, state) of the seat whose turn it is, from each client's newest GAME_STATE."""
    for client in clients:
        pump_events(client)
        for env in list(client._queue):
            if env.get("type") == "GAME_STATE":
                client._queue.remove(env)
                client.game_state = env["payload"]
    newest = max(clients, key=lambda client: client.game_state["revision"]).game_state
    actor = clients[newest["turn"]]
    return actor, actor.game_state


def start_bot_room(test, human):
    """One human and three easy bots in a classic room; returns the first GAME_STATE."""
    seat = assert_ok(test, human.request("ROOM_CREATE", {"mode": "classic"}))["seat"]
    for bot_seat in range(SEAT_COUNT):
        if bot_seat != seat:
            assert_ok(test, human.request("ROOM_BOT", {"seat": bot_seat, "level": "easy"}))
    assert_ok(test, human.request("ROOM_START", {}))
    return recv_game_state(human)


class TestResilience(unittest.TestCase):
    def test_fragmented_frame_mid_match(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients, states = start_classic_match(server)
            try:
                actor, state = current_turn(clients)
                bystander = clients[(state["turn"] + 1) % SEAT_COUNT]
                req_id = actor.next_id
                actor.next_id += 1
                data = frame(req_id, "DRAW", {"matchId": state["matchId"], "turnNo": state["turnNo"]})
                # Split inside the length prefix and inside the body.
                pieces = (data[:2], data[2:7], data[7:20], data[20:])
                for index, piece in enumerate(pieces):
                    actor.send_raw(piece)
                    time.sleep(FRAGMENT_GAP_S)
                    # A half-received frame from one socket must not block the others.
                    if index < len(pieces) - 1:
                        self.assertEqual(bystander.request("PING", {})["type"], "OK")
                resp = actor.request("PING", {})
                self.assertEqual(resp["type"], "OK", resp)
                answered = [env for env in actor._queue if env.get("id") == req_id]
                self.assertEqual(len(answered), 1, actor._queue)
                self.assertEqual(answered[0]["type"], "OK", answered[0])
                recv_game_state(bystander, lambda newer: newer["revision"] > state["revision"])
            finally:
                close_all(clients)

    def test_killed_client_keeps_seat(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients, states = start_classic_match(server)
            fresh = None
            try:
                victim = clients[KILLED_SEAT]
                kill_socket(victim)
                others = [client for seat, client in enumerate(clients) if seat != KILLED_SEAT]
                for client in others:
                    recv_game_state(
                        client, lambda s: s["players"][KILLED_SEAT]["connected"] is False)
                    chat = client.request("CHAT", {"matchId": states[0]["matchId"], "chatId": 0})
                    self.assertEqual(chat["type"], "OK", chat)

                fresh = WireClient("127.0.0.1", server.port)
                assert_ok(self, fresh.request("LOGIN", {
                    "email": victim.email, "password": victim.password
                }))
                state = recv_game_state(fresh)
                self.assertEqual(state["matchId"], states[0]["matchId"])
                self.assertEqual(state["viewerSeat"], KILLED_SEAT)
                for client in others:
                    recv_game_state(
                        client, lambda s: s["players"][KILLED_SEAT]["connected"] is True)
            finally:
                if fresh:
                    fresh.close()
                close_all(clients)

    def test_default_heartbeat_drops_silent_player(self):
        # Real time on purpose: this checks the shipped 15 s default, not a test override.
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            clients, _ = start_classic_match(server)
            try:
                silent = clients[KILLED_SEAT]
                others = [client for seat, client in enumerate(clients) if seat != KILLED_SEAT]
                start = time.time()
                closed = False
                while time.time() - start < HEARTBEAT_NOT_BEFORE_S + HEARTBEAT_CLOSE_WITHIN_S:
                    for client in others:
                        self.assertEqual(client.request("PING", {})["type"], "OK")
                    if wait_for_eof(silent.sock, KEEPALIVE_PING_S):
                        closed = True
                        break
                elapsed = time.time() - start
                self.assertTrue(closed, "silent player was never dropped")
                self.assertGreaterEqual(elapsed, HEARTBEAT_NOT_BEFORE_S)
                for client in others:
                    recv_game_state(
                        client, lambda s: s["players"][KILLED_SEAT]["connected"] is False)
            finally:
                close_all(clients)

    def test_storage_failure_pauses_then_finalizes_once(self):
        with tempfile.NamedTemporaryFile(suffix=".db", delete=False) as tmp:
            db_path = tmp.name
        try:
            with ServerProcess(db=db_path, auth_limit=AUTH_LIMIT) as server:
                clients, states = start_classic_match(server)
                lock = sqlite3.connect(db_path, isolation_level=None)
                try:
                    actor, state = current_turn(clients)
                    # Holding the write lock makes the server's next write fail with SQLITE_BUSY.
                    lock.execute("BEGIN IMMEDIATE")
                    failed = actor.request("DRAW", {
                        "matchId": state["matchId"], "turnNo": state["turnNo"]
                    })
                    self.assertEqual(error_code(failed), "STORAGE_ERROR", failed)
                    for client in clients:
                        paused = recv_game_state(client, lambda s: s["paused"] is True)
                        # The failed command must not have been applied.
                        self.assertEqual(paused["turnNo"], state["turnNo"])
                    again = actor.request("DRAW", {
                        "matchId": state["matchId"], "turnNo": state["turnNo"]
                    })
                    self.assertEqual(error_code(again), "STORAGE_ERROR", again)

                    lock.execute("ROLLBACK")
                    for client in clients:
                        client.game_state = recv_game_state(
                            client, lambda s: s["paused"] is False, timeout=UNPAUSE_TIMEOUT_S)
                    self.assertEqual(actor.game_state["turnNo"], state["turnNo"])

                    result = play_until_result(clients, timeout=MATCH_TIMEOUT_S)
                    self.assertEqual(result["matchId"], states[0]["matchId"])
                    for client in clients:
                        stats = assert_ok(self, client.request("PROFILE_GET", {}))["profile"]["stats"]
                        self.assertEqual(stats["matches"], 1)
                finally:
                    lock.close()
                    close_all(clients)

            db = sqlite3.connect(db_path)
            try:
                rows = db.execute("SELECT COUNT(*) FROM matches").fetchone()[0]
                players = db.execute("SELECT SUM(matches) FROM profile_stats").fetchone()[0]
            finally:
                db.close()
            self.assertEqual(rows, 1)
            self.assertEqual(players, SEAT_COUNT)
        finally:
            remove_db(db_path)

    def test_full_server_rooms_are_isolated(self):
        with ServerProcess(auth_limit=AUTH_LIMIT) as server:
            humans = login_many(server, MAX_ROOMS + 1)
            hosts, latecomer = humans[:MAX_ROOMS], humans[MAX_ROOMS]
            try:
                match_ids = [start_bot_room(self, host)["matchId"] for host in hosts]
                self.assertEqual(len(set(match_ids)), MAX_ROOMS)
                full = latecomer.request("ROOM_CREATE", {"mode": "classic"})
                self.assertEqual(error_code(full), "SERVER_FULL", full)

                # The only human leaving aborts that match and frees its room...
                kill_socket(hosts[0])
                deadline = time.time() + PUSH_TIMEOUT_S
                created = latecomer.request("ROOM_CREATE", {"mode": "classic"})
                while error_code(created) == "SERVER_FULL" and time.time() < deadline:
                    time.sleep(FRAGMENT_GAP_S)
                    created = latecomer.request("ROOM_CREATE", {"mode": "classic"})
                self.assertEqual(created["type"], "OK", created)

                # ...while every other match keeps running.
                for host, match_id in zip(hosts[1:], match_ids[1:]):
                    chat = host.request("CHAT", {"matchId": match_id, "chatId": 0})
                    self.assertEqual(chat["type"], "OK", chat)
            finally:
                close_all(humans)


if __name__ == "__main__":
    unittest.main()
