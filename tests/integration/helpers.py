import json
import os
import re
import select
import socket
import struct
import subprocess
import time
import uuid

class ServerProcess:
    def __init__(self, bind="127.0.0.1", port=0, db=":memory:", auth_limit=5, extra_args=None):
        self.bind = bind
        self.requested_port = port
        self.db = db
        self.auth_limit = auth_limit
        self.extra_args = extra_args or []
        self.proc = None
        self.port = None

    def start(self):
        binary = os.path.abspath("build/uno-server")
        if not os.path.exists(binary):
            raise FileNotFoundError(f"Server binary not found at {binary}")

        cmd = [
            binary,
            "--bind", self.bind,
            "--port", str(self.requested_port),
            "--db", self.db,
            "--auth-limit", str(self.auth_limit)
        ] + self.extra_args

        self.proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1
        )

        start_time = time.time()
        while time.time() - start_time < 5.0:
            if self.proc.poll() is not None:
                err = self.proc.stderr.read()
                raise RuntimeError(f"Server exited prematurely with code {self.proc.returncode}: {err}")
            line = self.proc.stdout.readline()
            if line:
                m = re.search(r"PORT\s+(\d+)", line)
                if m:
                    self.port = int(m.group(1))
                    break
            time.sleep(0.02)

        if self.port is None:
            self.stop()
            raise RuntimeError("Failed to detect server port within timeout")

    def stop(self):
        if self.proc:
            try:
                if self.proc.stdout:
                    self.proc.stdout.close()
                if self.proc.stderr:
                    self.proc.stderr.close()
            except Exception:
                pass
            try:
                self.proc.terminate()
                self.proc.wait(timeout=2.0)
            except Exception:
                try:
                    self.proc.kill()
                    self.proc.wait(timeout=1.0)
                except Exception:
                    pass
            self.proc = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.stop()


class WireClient:
    def __init__(self, host="127.0.0.1", port=9090, timeout=3.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect((host, port))
        self.next_id = 1
        self._buf = bytearray()
        self._queue = []

    def send_raw(self, data: bytes):
        self.sock.sendall(data)

    def send_envelope(self, req_id: int, msg_type: str, payload: dict = None):
        if payload is None:
            payload = {}
        body = json.dumps({
            "v": 1,
            "id": req_id,
            "type": msg_type,
            "payload": payload
        }, separators=(',', ':')).encode('utf-8')
        frame = struct.pack(">I", len(body)) + body
        self.sock.sendall(frame)

    def _read_one_raw(self, timeout=None):
        if timeout is not None:
            self.sock.settimeout(max(0.001, timeout))
        else:
            self.sock.settimeout(self.timeout)

        while len(self._buf) < 4:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionResetError("Connection closed by peer while reading prefix")
            self._buf.extend(chunk)

        length = struct.unpack(">I", self._buf[:4])[0]
        while len(self._buf) < 4 + length:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionResetError("Connection closed by peer while reading body")
            self._buf.extend(chunk)

        body = self._buf[4:4+length]
        del self._buf[:4+length]
        return json.loads(body.decode('utf-8'))

    def recv_envelope(self, timeout=None):
        if self._queue:
            return self._queue.pop(0)
        return self._read_one_raw(timeout=timeout)

    def request(self, msg_type: str, payload: dict = None, timeout=None):
        req_id = self.next_id
        self.next_id += 1
        self.send_envelope(req_id, msg_type, payload)
        start = time.time()
        deadline = (timeout if timeout is not None else self.timeout)
        while True:
            for i, env in enumerate(self._queue):
                if env.get("id") == req_id:
                    return self._queue.pop(i)
            remaining = deadline - (time.time() - start)
            if remaining <= 0:
                raise TimeoutError(f"Timed out waiting for response to {msg_type} (id {req_id})")
            env = self._read_one_raw(timeout=remaining)
            if env.get("id") == req_id:
                return env
            self._queue.append(env)

    def recv_event(self, event_type=None, timeout=None):
        start = time.time()
        deadline = (timeout if timeout is not None else self.timeout)
        while True:
            for i, env in enumerate(self._queue):
                if event_type is None or env.get("type") == event_type:
                    return self._queue.pop(i)
            remaining = deadline - (time.time() - start)
            if remaining <= 0:
                raise TimeoutError(f"Timed out waiting for event {event_type}")
            env = self._read_one_raw(timeout=remaining)
            if event_type is None or env.get("type") == event_type:
                return env
            self._queue.append(env)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()


ACCOUNT_PASSWORD = "password123"
# Codes that only mean the acting client's snapshot was older than the server's.
STALE_SNAPSHOT_CODES = ("STALE_TURN", "NOT_YOUR_TURN")
RATE_LIMIT_BACKOFF_S = 0.1
PUMP_WAIT_S = 0.005
WILD_COLOR_CHOICE = "red"


def login_many(server, n):
    """Register and log in n fresh accounts; each client gets .me, .token, .email, .password."""
    clients = []
    for index in range(n):
        client = WireClient("127.0.0.1", server.port)
        email = f"player{index}_{uuid.uuid4().hex[:12]}@test.com"
        registered = client.request("REGISTER", {
            "email": email,
            "password": ACCOUNT_PASSWORD,
            "name": f"Player{index}",
            "avatar": "fox"
        })
        if registered.get("type") != "OK":
            raise AssertionError(f"REGISTER failed: {registered}")
        logged_in = client.request("LOGIN", {"email": email, "password": ACCOUNT_PASSWORD})
        if logged_in.get("type") != "OK":
            raise AssertionError(f"LOGIN failed: {logged_in}")
        client.me = logged_in["payload"]["me"]
        client.token = logged_in["payload"]["token"]
        client.email = email
        client.password = ACCOUNT_PASSWORD
        clients.append(client)
    return clients


def _has_full_frame(client):
    if len(client._buf) < 4:
        return False
    length = struct.unpack(">I", client._buf[:4])[0]
    return len(client._buf) >= 4 + length


def pump_events(client, wait=0.0):
    """Move every frame already readable on the socket into the client's queue.

    Waits up to `wait` seconds for the first frame only, so pumping several
    clients in a row stays cheap when nothing is pending.
    """
    first = True
    while True:
        if not _has_full_frame(client):
            readable, _, _ = select.select([client.sock], [], [], wait if first else 0)
            if not readable:
                return
        client._queue.append(client._read_one_raw())
        first = False


def _absorb_game_states(client):
    """Keep only the newest GAME_STATE per client; leave other events queued."""
    kept = []
    for env in client._queue:
        if env.get("type") != "GAME_STATE":
            kept.append(env)
            continue
        state = env["payload"]
        latest = getattr(client, "game_state", None)
        if latest is None or state["revision"] >= latest["revision"]:
            client.game_state = state
    client._queue = kept


def _queued_result(clients):
    for client in clients:
        for env in client._queue:
            if env.get("type") == "MATCH_RESULT":
                return env["payload"]
    return None


def _choose_action(state):
    if state["canSayUno"]:
        return "SAY_UNO", {"matchId": state["matchId"]}
    base = {"matchId": state["matchId"], "turnNo": state["turnNo"]}
    legal = state["legalCardIds"]
    if legal:
        hand = state["players"][state["viewerSeat"]]["hand"]
        card = next(card for card in hand if card["id"] == legal[0])
        payload = dict(base, cardId=card["id"])
        if card["color"] == "wild":
            payload["color"] = WILD_COLOR_CHOICE
        return "PLAY", payload
    if state["canPass"]:
        return "PASS", base
    return "DRAW", base


def play_until_result(clients, timeout=60):
    """Drive every human seat with a first-legal-card policy until MATCH_RESULT.

    The MATCH_RESULT envelopes stay queued on each client so callers can still
    recv_event("MATCH_RESULT") on every connection; the returned payload is the
    first one seen.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        for client in clients:
            pump_events(client, PUMP_WAIT_S)
            _absorb_game_states(client)
        result = _queued_result(clients)
        if result is not None:
            return result
        states = [getattr(client, "game_state", None) for client in clients]
        newest = max((state["revision"] for state in states if state), default=None)
        actor = None
        for client, state in zip(clients, states):
            if (state and state["revision"] == newest and state["phase"] == "PLAYING"
                    and state["viewerSeat"] == state["turn"]):
                actor = client
                break
        if actor is None:
            continue
        msg_type, payload = _choose_action(actor.game_state)
        resp = actor.request(msg_type, payload)
        if resp.get("type") == "OK":
            continue
        code = resp.get("payload", {}).get("code")
        if code in STALE_SNAPSHOT_CODES:
            continue
        if code == "RATE_LIMITED":
            time.sleep(RATE_LIMIT_BACKOFF_S)
            continue
        raise AssertionError(f"{msg_type} {payload} rejected: {resp}")
    raise TimeoutError("Match did not reach MATCH_RESULT in time")
