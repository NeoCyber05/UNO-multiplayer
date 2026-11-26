#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "accounts.h"
#include "auth_worker.h"
#include "server/game/match.h"
#include "queue.h"
#include "rooms.h"
#include "social.h"
#include "store.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* sig_atomic_t: the only type a signal handler may write and the loop then read. */
static volatile sig_atomic_t gStopRequested = 0;

/* SIGINT/SIGTERM only raise a flag; the poll loop notices it within one
 * POLL_TIMEOUT_MS and shuts down from normal context. */
static void handleStopSignal(int signalNumber) {
    (void)signalNumber;
    gStopRequested = 1;
}

/* Identifies the slot a frame was read from; the generation rejects frames
 * that arrive for a slot already reused by a newer connection. */
typedef struct {
    App *app;
    uint32_t slotIndex;
    uint32_t generation;
} FrameContext;

enum {
    MS_PER_SECOND = 1000,
    NS_PER_MS = 1000000,
    /* Pending connections the kernel queues before accept() runs. */
    LISTEN_BACKLOG = 128,
    /* poll() wakes at least this often so timers and bots advance with no traffic. */
    POLL_TIMEOUT_MS = 50,
    /* Fits the fixed SERVER_FULL error frame sent to a rejected connection. */
    SERVER_FULL_FRAME_SIZE = 256
};

/* Per-connection token bucket: 0.03 tokens/ms is 30 requests/s sustained. */
static const double RATE_TOKENS_PER_MS = 0.03;
static const double RATE_BURST_TOKENS = 60.0;

/* Opcodes routed to the match coordinator; the actor always comes from the session. */
static const char *const MATCH_COMMANDS[] = {
    "PLAY", "DRAW", "PASS", "SAY_UNO", "DRAFT_PICK", "MATCH_LEAVE", "CHAT"
};

/* Opcodes routed to rooms_handle. */
static const char *const ROOM_COMMANDS[] = {
    "ROOM_CREATE", "ROOM_JOIN", "ROOM_LEAVE", "ROOM_BOT", "ROOM_KICK", "ROOM_MODE",
    "ROOM_START", "INVITE_SEND", "INVITE_REPLY", "PARTY_LEAVE"
};

/* Opcodes routed to social_handle. */
static const char *const SOCIAL_COMMANDS[] = {
    "FRIEND_LIST", "FRIEND_REQUEST", "FRIEND_REPLY", "PROFILE_GET", "PROFILE_UPDATE",
    "LEADERBOARD_GET", "LOBBY_GET"
};

enum {
    /* Actor, their room's humans, their party and an inviter, before dedup. */
    PRESENCE_WATCH_MAX = 1 + ROOM_MAX_SEATS + PARTY_MAX_MEMBERS + 1
};

/* Presence of everyone a command may move, taken before it runs, so only
 * real changes reach friends. */
typedef struct {
    UserId users[PRESENCE_WATCH_MAX];
    Presence before[PRESENCE_WATCH_MAX];
    size_t count;
} PresenceWatch;

uint64_t app_now_ms(void) {
    struct timespec now;
    /* CLOCK_MONOTONIC cannot fail on Linux; 0 keeps callers defined if it ever does. */
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * MS_PER_SECOND + (uint64_t)(now.tv_nsec / NS_PER_MS);
}

static const char *statusMessage(Status status) {
    switch (status) {
    case BAD_REQUEST: return "Malformed or unsupported request";
    case UNAUTHENTICATED: return "Authentication required";
    case FORBIDDEN: return "Not allowed";
    case BUSY: return "Resource busy";
    case NOT_FOUND: return "Not found";
    case ROOM_FULL: return "Room is full";
    case INVALID_ACTION: return "Action not allowed now";
    case NOT_YOUR_TURN: return "Not your turn";
    case STALE_TURN: return "Turn already advanced";
    case STALE_REQUEST: return "Request id must increase";
    case RATE_LIMITED: return "Too many requests";
    case STORAGE_ERROR: return "Storage unavailable";
    case SERVER_FULL: return "Server is full";
    default: return "Request failed";
    }
}

/* Returns the slot only while conn still names the same accepted socket. */
static AppConnSlot *liveSlot(App *app, ConnId conn) {
    if (conn.slot >= APP_MAX_CONNS) return NULL;
    AppConnSlot *slot = &app->slots[conn.slot];
    if (!slot->active || slot->generation != conn.generation) return NULL;
    return slot;
}

/* Queue then flush immediately; a failed flush marks the conn closing and the
 * poll loop tears it down, so callers never close a slot mid-dispatch. */
static Status sendEnvelope(AppConnSlot *slot, const Envelope *envelope) {
    Status queued = conn_queue(&slot->conn, envelope);
    if (queued != OK) return queued;
    return conn_flush(&slot->conn);
}

Status app_push(App *app, UserId user, const char *type, json_object *payload) {
    if (!app || !user || !type || !payload) return BAD_REQUEST;
    ConnId conn;
    if (sessions_find_conn(app, user, &conn) != OK) return NOT_FOUND;
    AppConnSlot *slot = liveSlot(app, conn);
    if (!slot) return NOT_FOUND;
    /* Protocol: pushes carry id 0 so clients never match them to a request. */
    Envelope push = {1, 0, type, payload};
    return sendEnvelope(slot, &push);
}

void app_reply_status(App *app, ConnId conn, int32_t id, Status status) {
    if (!app) return;
    AppConnSlot *slot = liveSlot(app, conn);
    if (!slot) return;
    json_object *payload = json_object_new_object();
    if (!payload) return;
    if (status != OK) {
        json_object_object_add(payload, "code", json_object_new_string(status_code(status)));
        json_object_object_add(payload, "message",
                               json_object_new_string(statusMessage(status)));
    }
    Envelope reply = {1, id, status == OK ? "OK" : "ERROR", payload};
    /* A failed send already marked the conn closing; nothing else to undo. */
    (void)sendEnvelope(slot, &reply);
    json_object_put(payload);
}

void app_reply_ok(App *app, ConnId conn, int32_t id, json_object *payload) {
    if (!app || !payload) return;
    AppConnSlot *slot = liveSlot(app, conn);
    if (!slot) return;
    Envelope reply = {1, id, "OK", payload};
    /* A failed send already marked the conn closing; nothing else to undo. */
    (void)sendEnvelope(slot, &reply);
}

static void onMatchPush(void *context, UserId userId, const char *type,
                           json_object *payload) {
    /* An offline seat simply misses the push; RESUME resends the current view. */
    (void)app_push((App *)context, userId, type, payload);
}

static bool recordInMatch(const App *app, const AppMatchRecord *record) {
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (matchesContains(app->matches, record->users[seat])) return true;
    }
    return false;
}

static void recordMatch(App *app, const RoomStart *roomStart) {
    AppMatchRecord *slot = NULL;
    for (int index = 0; index < APP_MATCH_RECORDS && !slot; index++) {
        if (!app->match_records[index].active) slot = &app->match_records[index];
    }
    /* Safety net only: reuse a record whose match is already over. */
    for (int index = 0; index < APP_MATCH_RECORDS && !slot; index++) {
        if (!recordInMatch(app, &app->match_records[index])) {
            slot = &app->match_records[index];
        }
    }
    if (!slot) return;
    memset(slot, 0, sizeof *slot);
    slot->active = true;
    memcpy(slot->roomId, roomStart->roomId, sizeof slot->roomId);
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (!roomStart->players[seat].isBot) slot->users[seat] = roomStart->players[seat].userId;
    }
}

static void onMatchEnded(void *context, const char *roomId) {
    App *app = (App *)context;
    /* The coordinator already cleared the match, so members can rejoin at once. */
    rooms_match_ended(app, roomId);
    for (int index = 0; index < APP_MATCH_RECORDS; index++) {
        AppMatchRecord *record = &app->match_records[index];
        /* A record still holding a seated user belongs to another match whose
         * room later reused the same code. */
        if (!record->active || strcmp(record->roomId, roomId) != 0 ||
            recordInMatch(app, record)) {
            continue;
        }
        record->active = false;
        /* Shutdown aborts matches while closing slots; nobody is left to tell. */
        if (!app->running) continue;
        for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
            UserId seated = record->users[seat];
            /* A leaving user still reads as connected here; announcing that
             * would flash online/idle before the offline push app_user_leave
             * sends once the session is detached. */
            if (seated && seated != app->leaving_user) social_presence(app, seated);
        }
    }
}

static Status onMatchStart(App *app, const RoomStart *roomStart, MatchId *outMatchId) {
    if (!app || !app->matches) return BUSY;
    Status started = matchesStart(app->matches, roomStart, app_now_ms(), outMatchId);
    if (started != OK) return started;
    recordMatch(app, roomStart);
    /* Every in_match transition goes through here, room or queue alike. */
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (!roomStart->players[seat].isBot) social_presence(app, roomStart->players[seat].userId);
    }
    return OK;
}

static bool inList(const char *type, const char *const list[], size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(type, list[index]) == 0) return true;
    }
    return false;
}

static bool isMatchCommand(const char *type) {
    return inList(type, MATCH_COMMANDS, sizeof MATCH_COMMANDS / sizeof MATCH_COMMANDS[0]);
}

static bool isRoomCommand(const char *type) {
    return inList(type, ROOM_COMMANDS, sizeof ROOM_COMMANDS / sizeof ROOM_COMMANDS[0]);
}

static bool isSocialCommand(const char *type) {
    return inList(type, SOCIAL_COMMANDS, sizeof SOCIAL_COMMANDS / sizeof SOCIAL_COMMANDS[0]);
}

static void watchAdd(App *app, PresenceWatch *watch, UserId user) {
    if (user == 0 || watch->count >= PRESENCE_WATCH_MAX) return;
    for (size_t index = 0; index < watch->count; index++) {
        if (watch->users[index] == user) return;
    }
    watch->users[watch->count] = user;
    watch->before[watch->count] = social_presence_of(app, user);
    watch->count++;
}

/* A room, party or queue command can only move the actor and the people
 * sharing their room or party (a kick, a dissolved party, a party entry). */
static void watchBegin(App *app, PresenceWatch *watch, UserId actor) {
    memset(watch, 0, sizeof *watch);
    watchAdd(app, watch, actor);
    const Room *room = rooms_find_by_user(app->rooms, actor);
    for (int seat = 0; room && seat < ROOM_MAX_SEATS; seat++) {
        if (room->seats[seat].occupied && !room->seats[seat].is_bot) {
            watchAdd(app, watch, room->seats[seat].user_id);
        }
    }
    const Party *party = party_find_by_user(app->rooms, actor);
    for (size_t member = 0; party && member < party->member_count; member++) {
        watchAdd(app, watch, party->members[member]);
    }
}

/* Accepting a party invite also moves the inviter from idle to online. */
static void watchInviter(App *app, PresenceWatch *watch, const Envelope *envelope) {
    json_object *idObj = NULL;
    if (!envelope->payload || !json_object_object_get_ex(envelope->payload, "inviteId", &idObj) ||
        !json_object_is_type(idObj, json_type_string)) {
        return;
    }
    const char *inviteId = json_object_get_string(idObj);
    for (int index = 0; index < MAX_INVITES; index++) {
        const Invite *invite = &app->rooms->invites[index];
        if (invite->active && strcmp(invite->invite_id, inviteId) == 0) {
            watchAdd(app, watch, invite->sender_id);
            return;
        }
    }
}

static void watchEnd(App *app, PresenceWatch *watch) {
    for (size_t index = 0; index < watch->count; index++) {
        Presence now = social_presence_of(app, watch->users[index]);
        /* onMatchStart already announced in_match; skip the duplicate. */
        if (now != watch->before[index] && now != PRESENCE_IN_MATCH) {
            social_presence(app, watch->users[index]);
        }
    }
}

/* Runs a room or queue command and tells friends about every presence it moved. */
static Status dispatchWatched(App *app, ConnId conn, const Envelope *envelope, bool roomCommand) {
    PresenceWatch watch;
    UserId actor = 0;
    memset(&watch, 0, sizeof watch);
    if (app->rooms && sessions_user(app, conn, &actor) == OK && actor != 0) {
        watchBegin(app, &watch, actor);
        if (strcmp(envelope->type, "INVITE_REPLY") == 0) watchInviter(app, &watch, envelope);
    }
    Status result = roomCommand ? rooms_handle(app, conn, envelope)
                                : queue_handle(app, conn, envelope);
    watchEnd(app, &watch);
    return result;
}

bool app_conn_check_rate_limit(AppConnSlot *slot, uint64_t nowMs) {
    if (!slot) return false;
    if (nowMs > slot->rate_last_ms) {
        uint64_t elapsed = nowMs - slot->rate_last_ms;
        slot->rate_tokens += (double)elapsed * RATE_TOKENS_PER_MS;
        if (slot->rate_tokens > RATE_BURST_TOKENS) {
            slot->rate_tokens = RATE_BURST_TOKENS;
        }
        slot->rate_last_ms = nowMs;
    }
    if (slot->rate_tokens < 1.0) {
        return false;
    }
    slot->rate_tokens -= 1.0;
    return true;
}

void app_user_leave(App *app, ConnId conn, UserId user) {
    if (!app || user == 0) return;
    PresenceWatch watch;
    memset(&watch, 0, sizeof watch);
    /* Shutdown closes every slot; nobody is left to hear about it. */
    if (app->running) watchBegin(app, &watch, user);
    uint64_t nowMs = app_now_ms();
    /* Mark the seat offline while the session still exists; the room keeps
     * an in-match member so the seat can be reclaimed by RESUME or LOGIN.
     * NOT_FOUND just means the user is not in a match. */
    app->leaving_user = user;
    (void)matchesConnected(app->matches, user, false, nowMs);
    app->leaving_user = 0;
    /* Before rooms_disconnect dissolves the party: a dropped member takes
     * the whole party entry out of the queue and the partner is told. */
    queue_user_gone(app, user);
    rooms_disconnect(app, user, nowMs);
    sessions_disconnect(app, conn);
    /* After sessions_disconnect, so the user now reads as offline. */
    watchEnd(app, &watch);
}

void app_close_slot(App *app, int slotIndex) {
    if (slotIndex < 0 || slotIndex >= APP_MAX_CONNS) return;
    AppConnSlot *slot = &app->slots[slotIndex];
    if (!slot->active) return;
    ConnId conn = {(uint32_t)slotIndex, slot->generation};
    UserId userId = 0;
    if (sessions_user(app, conn, &userId) == OK && userId != 0) {
        app_user_leave(app, conn, userId);
    }
    conn_close(&slot->conn);
    slot->active = false;
    slot->generation++;
    slot->client_ip[0] = '\0';
    slot->rate_last_ms = 0;
    slot->rate_tokens = 0.0;
    slot->last_request_id = 0;
    slot->last_frame_ms = 0;
}

static Status onFrame(const Envelope *envelope, void *context) {
    FrameContext *frameContext = (FrameContext *)context;
    App *app = frameContext->app;
    if (frameContext->slotIndex >= APP_MAX_CONNS) return BAD_REQUEST;
    AppConnSlot *slot = &app->slots[frameContext->slotIndex];
    if (!slot->active || slot->generation != frameContext->generation) {
        return BAD_REQUEST;
    }

    ConnId conn = {frameContext->slotIndex, frameContext->generation};
    uint64_t nowMs = app_now_ms();
    if (!app_conn_check_rate_limit(slot, nowMs)) {
        app_reply_status(app, conn, envelope->id, RATE_LIMITED);
        return OK;
    }

    /* Spec: ids strictly increase per connection; a replayed or reordered id
     * is answered without re-running the command. */
    if (envelope->id <= slot->last_request_id) {
        app_reply_status(app, conn, envelope->id, STALE_REQUEST);
        return OK;
    }
    slot->last_request_id = envelope->id;
    slot->last_frame_ms = nowMs;

    /* Resolve due deadlines first so no command acts on an already expired turn. */
    matchesTick(app->matches, nowMs);

    if (strcmp(envelope->type, "HELLO") == 0) {
        /* Wall-clock time on purpose: clients use it to show real timestamps,
         * while every timer inside the server stays on CLOCK_MONOTONIC. */
        struct timespec wallClock;
        clock_gettime(CLOCK_REALTIME, &wallClock);
        uint64_t serverTimeMs = (uint64_t)wallClock.tv_sec * MS_PER_SECOND +
                                (uint64_t)(wallClock.tv_nsec / NS_PER_MS);

        struct json_object *payload = json_object_new_object();
        json_object_object_add(payload, "serverTimeMs", json_object_new_int64(serverTimeMs));

        Envelope response = {1, envelope->id, "OK", payload};
        Status status = conn_queue(&slot->conn, &response);
        json_object_put(payload);
        if (status != OK) return status;
        return conn_flush(&slot->conn);
    }

    if (strcmp(envelope->type, "PING") == 0) {
        struct json_object *payload = json_object_new_object();
        Envelope response = {1, envelope->id, "OK", payload};
        Status status = conn_queue(&slot->conn, &response);
        json_object_put(payload);
        if (status != OK) return status;
        return conn_flush(&slot->conn);
    }

    if (strcmp(envelope->type, "REGISTER") == 0 ||
        strcmp(envelope->type, "LOGIN") == 0 ||
        strcmp(envelope->type, "RESUME") == 0 ||
        strcmp(envelope->type, "LOGOUT") == 0) {
        return accounts_handle(app, conn, envelope);
    }

    if (isRoomCommand(envelope->type)) {
        return dispatchWatched(app, conn, envelope, true);
    }

    if (strcmp(envelope->type, "QUEUE_JOIN") == 0 || strcmp(envelope->type, "QUEUE_LEAVE") == 0) {
        return dispatchWatched(app, conn, envelope, false);
    }

    if (isSocialCommand(envelope->type)) {
        return social_handle(app, conn, envelope);
    }

    if (isMatchCommand(envelope->type)) {
        UserId userId = 0;
        if (sessions_user(app, conn, &userId) != OK) {
            app_reply_status(app, conn, envelope->id, UNAUTHENTICATED);
            return OK;
        }
        /* GAME_STATE pushes go out inside matchesHandle; the reply follows them. */
        Status result = matchesHandle(app->matches, userId, envelope, nowMs);
        app_reply_status(app, conn, envelope->id, result);
        return OK;
    }

    /* Unknown opcodes, including server-only events such as TIMEOUT or
     * UNO_PENALTY, are rejected but keep the connection open. */
    app_reply_status(app, conn, envelope->id, BAD_REQUEST);
    return OK;
}

Status app_init(App *app, const char *bindIp, uint16_t port, const char *dbPath, int authLimit) {
    if (!app) return BAD_REQUEST;
    memset(app, 0, sizeof(*app));
    app->listen_fd = -1;
    app->auth_pipe_read = -1;
    app->auth_pipe_write = -1;
    app->running = false;
    app->auth_limit = authLimit;
    app->heartbeat_ms = APP_DEFAULT_HEARTBEAT_MS;

    if (bindIp) {
        strncpy(app->bind_ip, bindIp, sizeof(app->bind_ip) - 1);
    } else {
        strcpy(app->bind_ip, "0.0.0.0");
    }

    if (dbPath) {
        strncpy(app->db_path, dbPath, sizeof(app->db_path) - 1);
    } else {
        strcpy(app->db_path, "uno.db");
    }

    sessions_init(app);
    rooms_init(&app->rooms, app, onMatchStart);
    Status queueStatus = queue_init(&app->queue);
    if (queueStatus != OK) return queueStatus;

    /* The auth worker thread writes one byte per finished task so poll() wakes
     * the main thread; both ends are non-blocking so neither side can stall.
     * FIXME: pipe() failure leaves no auth worker and fcntl results are ignored.
     * Return BUSY from app_init so startup fails instead of hanging logins. */
    int authPipe[2];
    if (pipe(authPipe) == 0) {
        int flags = fcntl(authPipe[0], F_GETFL, 0);
        if (flags >= 0) fcntl(authPipe[0], F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(authPipe[1], F_GETFL, 0);
        if (flags >= 0) fcntl(authPipe[1], F_SETFL, flags | O_NONBLOCK);

        app->auth_pipe_read = authPipe[0];
        app->auth_pipe_write = authPipe[1];

        auth_worker_init(&app->auth_worker, app->auth_pipe_write);
    }

    /* A peer that resets mid-send must yield EPIPE, not kill the server. */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction stopAction;
    memset(&stopAction, 0, sizeof(stopAction));
    stopAction.sa_handler = handleStopSignal;
    sigaction(SIGINT, &stopAction, NULL);
    sigaction(SIGTERM, &stopAction, NULL);

    // Step 1: create the IPv4 TCP listening socket.
    int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket < 0) return BUSY;

    /* SO_REUSEADDR lets a restarted server bind the port while old
     * connections are still in TIME_WAIT.
     * FIXME: setsockopt and fcntl(F_SETFL) results are ignored; a blocking
     * listen socket would stall the accept loop. Close it and return BUSY. */
    int reuseAddress = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

    /* Non-blocking so the accept loop can drain the queue until EAGAIN. */
    int flags = fcntl(listenSocket, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(listenSocket, F_SETFL, flags | O_NONBLOCK);
    }

    // Step 2: bind to the configured address; htons() puts the port in network byte order.
    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    if (inet_pton(AF_INET, app->bind_ip, &serverAddress.sin_addr) <= 0) {
        close(listenSocket);
        return BAD_REQUEST;
    }

    if (bind(listenSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) < 0) {
        close(listenSocket);
        return BUSY;
    }

    // Step 3: start listening; accept() happens in app_run's poll loop.
    if (listen(listenSocket, LISTEN_BACKLOG) < 0) {
        close(listenSocket);
        return BUSY;
    }

    /* Port 0 asks the kernel for an ephemeral port; read it back (ntohs converts
     * from network byte order) so app_run can print "PORT n" for tests. */
    struct sockaddr_in boundAddress;
    socklen_t addressLength = sizeof(boundAddress);
    if (getsockname(listenSocket, (struct sockaddr *)&boundAddress, &addressLength) == 0) {
        app->bound_port = ntohs(boundAddress.sin_port);
    } else {
        app->bound_port = port;
    }

    app->listen_fd = listenSocket;
    app->running = true;
    return OK;
}

void app_stop(App *app) {
    if (app) app->running = false;
}

void app_cleanup(App *app) {
    if (!app) return;
    /* Read by app_close_slot: no presence pushes while everything closes. */
    app->running = false;
    for (int i = 0; i < APP_MAX_CONNS; i++) {
        if (app->slots[i].active) {
            app_close_slot(app, i);
        }
    }
    if (app->listen_fd >= 0) {
        close(app->listen_fd);
        app->listen_fd = -1;
    }
    if (app->auth_worker) {
        auth_worker_destroy(app->auth_worker);
        app->auth_worker = NULL;
    }
    if (app->auth_pipe_read >= 0) {
        close(app->auth_pipe_read);
        app->auth_pipe_read = -1;
    }
    if (app->auth_pipe_write >= 0) {
        close(app->auth_pipe_write);
        app->auth_pipe_write = -1;
    }
    /* Every slot is closed above, so no disconnect can reach the queue any more. */
    queue_destroy(app->queue);
    app->queue = NULL;
    if (app->rooms) {
        rooms_destroy(app->rooms);
        app->rooms = NULL;
    }
    sessions_cleanup(app);
    /* The coordinator borrows the store, so it must go first. */
    matchesDestroy(app->matches);
    app->matches = NULL;
    if (app->store) {
        store_close(app->store);
        app->store = NULL;
    }
    app->running = false;
}

/* Tell every authenticated client the server is going away before sockets close. */
static void announceShutdown(App *app) {
    json_object *payload = json_object_new_object();
    if (!payload) return;
    for (int index = 0; index < APP_MAX_CONNS; index++) {
        AppConnSlot *slot = &app->slots[index];
        UserId userId = 0;
        if (!slot->active) continue;
        ConnId conn = {(uint32_t)index, slot->generation};
        if (sessions_user(app, conn, &userId) == OK && userId != 0) {
            (void)app_push(app, userId, "SERVER_SHUTDOWN", payload);
        }
    }
    json_object_put(payload);
}

/* Close connections with no accepted frame within the heartbeat window. */
static void sweepIdleConnections(App *app, uint64_t nowMs) {
    uint64_t limitMs = (uint64_t)app->heartbeat_ms;
    for (int index = 0; index < APP_MAX_CONNS; index++) {
        AppConnSlot *slot = &app->slots[index];
        if (!slot->active || nowMs < slot->last_frame_ms) continue;
        if (nowMs - slot->last_frame_ms >= limitMs) app_close_slot(app, index);
    }
}

int app_run(const char *bindIp, uint16_t port, const char *dbPath, int authLimit,
            int heartbeatMs) {
    App *app = calloc(1, sizeof(App));
    if (!app) {
        fprintf(stderr, "Failed to allocate App memory\n");
        return 1;
    }

    Status status = app_init(app, bindIp, port, dbPath, authLimit);
    if (status != OK) {
        fprintf(stderr, "Failed to initialize server (error %d)\n", status);
        free(app);
        return 1;
    }
    if (heartbeatMs > 0) app->heartbeat_ms = heartbeatMs;

    if (!app->store) {
        status = store_open(&app->store, app->db_path);
        if (status != OK) {
            fprintf(stderr, "Failed to open database at %s (error %d)\n", app->db_path, status);
            app_cleanup(app);
            free(app);
            return 1;
        }
    }

    app->matches = matchesCreate(app->store, onMatchPush, onMatchEnded, app);
    if (!app->matches) {
        fprintf(stderr, "Failed to create match coordinator\n");
        app_cleanup(app);
        free(app);
        return 1;
    }

    printf("PORT %u\n", app->bound_port);
    fflush(stdout);

    struct pollfd pollFds[2 + APP_MAX_CONNS];
    int slotByPollIndex[2 + APP_MAX_CONNS];

    while (app->running && !gStopRequested) {
        /* Turn deadlines and bot moves advance even when no socket is readable. */
        uint64_t tickMs = app_now_ms();
        matchesTick(app->matches, tickMs);
        /* Retries failed match starts and drops logged-out users from the queue. */
        queue_tick(app, tickMs);

        pollFds[0].fd = app->listen_fd;
        pollFds[0].events = POLLIN;
        pollFds[0].revents = 0;

        pollFds[1].fd = app->auth_pipe_read;
        pollFds[1].events = (app->auth_pipe_read >= 0) ? POLLIN : 0;
        pollFds[1].revents = 0;

        int pollCount = 2;

        for (int i = 0; i < APP_MAX_CONNS; i++) {
            if (app->slots[i].active && app->slots[i].conn.fd >= 0) {
                pollFds[pollCount].fd = app->slots[i].conn.fd;
                pollFds[pollCount].events = POLLIN;
                if (app->slots[i].conn.out_len > app->slots[i].conn.out_pos) {
                    pollFds[pollCount].events |= POLLOUT;
                }
                pollFds[pollCount].revents = 0;
                slotByPollIndex[pollCount] = i;
                pollCount++;
            }
        }

        /* Order below matters: auth results, new connections, then client I/O. */
        int readyCount = poll(pollFds, pollCount, POLL_TIMEOUT_MS);
        if (readyCount < 0) {
            /* A signal (e.g. SIGTERM) interrupted poll; re-check the stop flag. */
            if (errno == EINTR) continue;
            break;
        }

        /* Check auth worker pipe for completed auth tasks */
        if (pollFds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            auth_drain(app);
        }

        // Step 4: accept every pending connection on the listening socket.
        if (pollFds[0].revents & POLLIN) {
            while (1) {
                struct sockaddr_in clientAddress;
                socklen_t clientAddressLength = sizeof(clientAddress);
                int connectionSocket = accept(app->listen_fd, (struct sockaddr *)&clientAddress,
                                              &clientAddressLength);
                if (connectionSocket < 0) {
                    if (errno == EINTR) continue;
                    /* Queue drained: the listening socket is non-blocking. */
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    /* Other errors (ECONNABORTED, EMFILE) end this round; the
                     * next poll() retries any connection still queued. */
                    break;
                }

                int freeSlot = -1;
                for (int i = 0; i < APP_MAX_CONNS; i++) {
                    if (!app->slots[i].active) {
                        freeSlot = i;
                        break;
                    }
                }

                if (freeSlot < 0) {
                    /* Server capacity reached: reject client */
                    struct json_object *payload = json_object_new_object();
                    json_object_object_add(payload, "code", json_object_new_string("SERVER_FULL"));
                    const char *fullMessage = "Server connection limit reached";
                    json_object_object_add(payload, "message", json_object_new_string(fullMessage));
                    Envelope fullEnvelope = {1, 0, "ERROR", payload};
                    uint8_t fullFrame[SERVER_FULL_FRAME_SIZE];
                    size_t frameLength = 0;
                    Status encoded = frame_encode(&fullEnvelope, fullFrame, sizeof(fullFrame), &frameLength);
                    if (encoded == OK) {
                        send(connectionSocket, fullFrame, frameLength, MSG_NOSIGNAL);
                    }
                    json_object_put(payload);
                    close(connectionSocket);
                    continue;
                }

                AppConnSlot *slot = &app->slots[freeSlot];
                slot->active = true;
                slot->generation++;
                /* Kept as text for the per-IP auth limiter and logs. */
                inet_ntop(AF_INET, &clientAddress.sin_addr, slot->client_ip,
                          sizeof(slot->client_ip));
                slot->rate_last_ms = app_now_ms();
                slot->rate_tokens = RATE_BURST_TOKENS;
                slot->last_request_id = 0;
                /* A fresh socket gets one full heartbeat window to send its first frame. */
                slot->last_frame_ms = slot->rate_last_ms;
                conn_init(&slot->conn, connectionSocket, (uint32_t)freeSlot, slot->generation);
            }
        }

        /* Check client sockets */
        for (int k = 2; k < pollCount; k++) {
            int slotIndex = slotByPollIndex[k];
            AppConnSlot *slot = &app->slots[slotIndex];
            if (!slot->active) continue;

            // Step 5: send queued output first, then recv() and dispatch new frames.
            if (pollFds[k].revents & POLLOUT) {
                Status ioStatus = conn_flush(&slot->conn);
                if (ioStatus != OK || slot->conn.closing) {
                    app_close_slot(app, slotIndex);
                    continue;
                }
            }

            if (pollFds[k].revents & POLLIN) {
                FrameContext context = {app, (uint32_t)slotIndex, slot->generation};
                Status ioStatus = conn_read(&slot->conn, onFrame, &context);
                if (ioStatus != OK || slot->conn.closing) {
                    app_close_slot(app, slotIndex);
                    continue;
                }
            }

            if (pollFds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                app_close_slot(app, slotIndex);
                continue;
            }
        }

        /* Drain auth after handling any new client requests */
        auth_drain(app);

        /* Teardown slow peers immediately */
        for (int i = 0; i < APP_MAX_CONNS; i++) {
            if (app->slots[i].active && app->slots[i].conn.closing) {
                app_close_slot(app, i);
            }
        }

        sweepIdleConnections(app, app_now_ms());
    }

    announceShutdown(app);
    app_cleanup(app);
    free(app);
    return 0;
}
