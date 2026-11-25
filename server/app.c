#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "accounts.h"
#include "auth_worker.h"
#include "match.h"
#include "queue.h"
#include "rooms.h"
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

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_sig(int sig)
{
    (void)sig;
    g_stop_requested = 1;
}

typedef struct {
    App *app;
    uint32_t slot_idx;
    uint32_t generation;
} FrameContext;

enum {
    MS_PER_SECOND = 1000,
    NS_PER_MS = 1000000
};

/* Opcodes routed to the match coordinator; the actor always comes from the session. */
static const char *const MATCH_COMMANDS[] = {
    "PLAY", "DRAW", "PASS", "SAY_UNO", "DRAFT_PICK", "MATCH_LEAVE", "CHAT"
};

uint64_t app_now_ms(void) {
    struct timespec now;
    /* CLOCK_MONOTONIC cannot fail on Linux; 0 keeps callers defined if it ever does. */
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * MS_PER_SECOND + (uint64_t)(now.tv_nsec / NS_PER_MS);
}

static const char *status_message(Status status) {
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
static AppConnSlot *live_slot(App *app, ConnId conn) {
    if (conn.slot >= APP_MAX_CONNS) return NULL;
    AppConnSlot *slot = &app->slots[conn.slot];
    if (!slot->active || slot->generation != conn.generation) return NULL;
    return slot;
}

/* Queue then flush immediately; a failed flush marks the conn closing and the
 * poll loop tears it down, so callers never close a slot mid-dispatch. */
static Status send_envelope(AppConnSlot *slot, const Envelope *envelope) {
    Status queued = conn_queue(&slot->conn, envelope);
    if (queued != OK) return queued;
    return conn_flush(&slot->conn);
}

Status app_push(App *app, UserId user, const char *type, json_object *payload) {
    if (!app || !user || !type || !payload) return BAD_REQUEST;
    ConnId conn;
    if (sessions_find_conn(app, user, &conn) != OK) return NOT_FOUND;
    AppConnSlot *slot = live_slot(app, conn);
    if (!slot) return NOT_FOUND;
    /* Protocol: pushes carry id 0 so clients never match them to a request. */
    Envelope push = {1, 0, type, payload};
    return send_envelope(slot, &push);
}

void app_reply_status(App *app, ConnId conn, int32_t id, Status st) {
    if (!app) return;
    AppConnSlot *slot = live_slot(app, conn);
    if (!slot) return;
    json_object *payload = json_object_new_object();
    if (!payload) return;
    if (st != OK) {
        json_object_object_add(payload, "code", json_object_new_string(status_code(st)));
        json_object_object_add(payload, "message",
                               json_object_new_string(status_message(st)));
    }
    Envelope reply = {1, id, st == OK ? "OK" : "ERROR", payload};
    /* A failed send already marked the conn closing; nothing else to undo. */
    (void)send_envelope(slot, &reply);
    json_object_put(payload);
}

void app_reply_ok(App *app, ConnId conn, int32_t id, json_object *payload) {
    if (!app || !payload) return;
    AppConnSlot *slot = live_slot(app, conn);
    if (!slot) return;
    Envelope reply = {1, id, "OK", payload};
    /* A failed send already marked the conn closing; nothing else to undo. */
    (void)send_envelope(slot, &reply);
}

static void app_match_push(void *context, UserId userId, const char *type,
                           json_object *payload) {
    /* An offline seat simply misses the push; RESUME resends the current view. */
    (void)app_push((App *)context, userId, type, payload);
}

static void app_match_ended(void *context, const char *roomId) {
    /* The coordinator already cleared the match, so members can rejoin at once. */
    rooms_match_ended((App *)context, roomId);
}

static Status app_on_match_start(App *app, const RoomStart *rs, MatchId *out) {
    if (!app || !app->matches) return BUSY;
    return matchesStart(app->matches, rs, app_now_ms(), out);
}

static bool is_match_command(const char *type) {
    size_t commandCount = sizeof MATCH_COMMANDS / sizeof MATCH_COMMANDS[0];
    for (size_t index = 0; index < commandCount; index++) {
        if (strcmp(type, MATCH_COMMANDS[index]) == 0) return true;
    }
    return false;
}

bool app_conn_check_rate_limit(AppConnSlot *slot, uint64_t now_ms)
{
    if (!slot) return false;
    if (now_ms > slot->rate_last_ms) {
        uint64_t elapsed = now_ms - slot->rate_last_ms;
        slot->rate_tokens += (double)elapsed * 0.03;
        if (slot->rate_tokens > 60.0) {
            slot->rate_tokens = 60.0;
        }
        slot->rate_last_ms = now_ms;
    }
    if (slot->rate_tokens < 1.0) {
        return false;
    }
    slot->rate_tokens -= 1.0;
    return true;
}

void app_close_slot(App *app, int slot_idx)
{
    if (slot_idx < 0 || slot_idx >= APP_MAX_CONNS) return;
    AppConnSlot *s = &app->slots[slot_idx];
    if (!s->active) return;
    UserId uid = 0;
    if (sessions_user(app, (ConnId){(uint32_t)slot_idx, s->generation}, &uid) == OK && uid != 0) {
        uint64_t nowMs = app_now_ms();
        /* Mark the seat offline while the session still exists; the room keeps
         * an in-match member so the seat can be reclaimed by RESUME or LOGIN.
         * NOT_FOUND just means the user is not in a match. */
        (void)matchesConnected(app->matches, uid, false, nowMs);
        /* Before rooms_disconnect dissolves the party: a dropped member takes
         * the whole party entry out of the queue and the partner is told. */
        queue_user_gone(app, uid);
        rooms_disconnect(app, uid, nowMs);
    }
    sessions_disconnect(app, (ConnId){(uint32_t)slot_idx, s->generation});
    conn_close(&s->conn);
    s->active = false;
    s->generation++;
    s->client_ip[0] = '\0';
    s->rate_last_ms = 0;
    s->rate_tokens = 0.0;
    s->last_request_id = 0;
    s->last_frame_ms = 0;
}

static Status app_on_frame(const Envelope *e, void *ctx)
{
    FrameContext *fc = (FrameContext *)ctx;
    App *app = fc->app;
    if (fc->slot_idx >= APP_MAX_CONNS) return BAD_REQUEST;
    AppConnSlot *slot = &app->slots[fc->slot_idx];
    if (!slot->active || slot->generation != fc->generation) {
        return BAD_REQUEST;
    }

    ConnId conn = {fc->slot_idx, fc->generation};
    uint64_t nowMs = app_now_ms();
    if (!app_conn_check_rate_limit(slot, nowMs)) {
        app_reply_status(app, conn, e->id, RATE_LIMITED);
        return OK;
    }

    /* Spec: ids strictly increase per connection; a replayed or reordered id
     * is answered without re-running the command. */
    if (e->id <= slot->last_request_id) {
        app_reply_status(app, conn, e->id, STALE_REQUEST);
        return OK;
    }
    slot->last_request_id = e->id;
    slot->last_frame_ms = nowMs;

    /* Resolve due deadlines first so no command acts on an already expired turn. */
    matchesTick(app->matches, nowMs);

    if (strcmp(e->type, "HELLO") == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);

        struct json_object *payload = json_object_new_object();
        json_object_object_add(payload, "serverTimeMs", json_object_new_int64(ms));

        Envelope resp = {1, e->id, "OK", payload};
        Status s = conn_queue(&slot->conn, &resp);
        json_object_put(payload);
        if (s != OK) return s;
        return conn_flush(&slot->conn);
    }

    if (strcmp(e->type, "PING") == 0) {
        struct json_object *payload = json_object_new_object();
        Envelope resp = {1, e->id, "OK", payload};
        Status s = conn_queue(&slot->conn, &resp);
        json_object_put(payload);
        if (s != OK) return s;
        return conn_flush(&slot->conn);
    }

    if (strcmp(e->type, "REGISTER") == 0 ||
        strcmp(e->type, "LOGIN") == 0 ||
        strcmp(e->type, "RESUME") == 0 ||
        strcmp(e->type, "LOGOUT") == 0) {
        return accounts_handle(app, (ConnId){fc->slot_idx, fc->generation}, e);
    }

    if (strcmp(e->type, "ROOM_CREATE") == 0 ||
        strcmp(e->type, "ROOM_JOIN") == 0 ||
        strcmp(e->type, "ROOM_LEAVE") == 0 ||
        strcmp(e->type, "ROOM_BOT") == 0 ||
        strcmp(e->type, "ROOM_KICK") == 0 ||
        strcmp(e->type, "ROOM_MODE") == 0 ||
        strcmp(e->type, "ROOM_START") == 0 ||
        strcmp(e->type, "INVITE_SEND") == 0 ||
        strcmp(e->type, "INVITE_REPLY") == 0 ||
        strcmp(e->type, "PARTY_LEAVE") == 0) {
        return rooms_handle(app, (ConnId){fc->slot_idx, fc->generation}, e);
    }

    if (strcmp(e->type, "QUEUE_JOIN") == 0 || strcmp(e->type, "QUEUE_LEAVE") == 0) {
        return queue_handle(app, conn, e);
    }

    if (is_match_command(e->type)) {
        UserId userId = 0;
        if (sessions_user(app, conn, &userId) != OK) {
            app_reply_status(app, conn, e->id, UNAUTHENTICATED);
            return OK;
        }
        /* GAME_STATE pushes go out inside matchesHandle; the reply follows them. */
        Status result = matchesHandle(app->matches, userId, e, nowMs);
        app_reply_status(app, conn, e->id, result);
        return OK;
    }

    /* Unknown opcodes, including server-only events such as TIMEOUT or
     * UNO_PENALTY, are rejected but keep the connection open. */
    app_reply_status(app, conn, e->id, BAD_REQUEST);
    return OK;
}

Status app_init(App *app, const char *bind_ip, uint16_t port, const char *db_path, int auth_limit)
{
    if (!app) return BAD_REQUEST;
    memset(app, 0, sizeof(*app));
    app->listen_fd = -1;
    app->auth_pipe_read = -1;
    app->auth_pipe_write = -1;
    app->running = false;
    app->auth_limit = auth_limit;
    app->heartbeat_ms = APP_DEFAULT_HEARTBEAT_MS;

    if (bind_ip) {
        strncpy(app->bind_ip, bind_ip, sizeof(app->bind_ip) - 1);
    } else {
        strcpy(app->bind_ip, "0.0.0.0");
    }

    if (db_path) {
        strncpy(app->db_path, db_path, sizeof(app->db_path) - 1);
    } else {
        strcpy(app->db_path, "uno.db");
    }

    sessions_init(app);
    rooms_init(&app->rooms, app, app_on_match_start);
    Status queueStatus = queue_init(&app->queue);
    if (queueStatus != OK) return queueStatus;

    int pfd[2];
    if (pipe(pfd) == 0) {
        int flags = fcntl(pfd[0], F_GETFL, 0);
        if (flags >= 0) fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(pfd[1], F_GETFL, 0);
        if (flags >= 0) fcntl(pfd[1], F_SETFL, flags | O_NONBLOCK);

        app->auth_pipe_read = pfd[0];
        app->auth_pipe_write = pfd[1];

        auth_worker_init(&app->auth_worker, app->auth_pipe_write);
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return BUSY;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, app->bind_ip, &addr.sin_addr) <= 0) {
        close(fd);
        return BAD_REQUEST;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return BUSY;
    }

    if (listen(fd, 128) < 0) {
        close(fd);
        return BUSY;
    }

    struct sockaddr_in bound_addr;
    socklen_t addr_len = sizeof(bound_addr);
    if (getsockname(fd, (struct sockaddr *)&bound_addr, &addr_len) == 0) {
        app->bound_port = ntohs(bound_addr.sin_port);
    } else {
        app->bound_port = port;
    }

    app->listen_fd = fd;
    app->running = true;
    return OK;
}

void app_stop(App *app)
{
    if (app) app->running = false;
}

void app_cleanup(App *app)
{
    if (!app) return;
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
static void app_announce_shutdown(App *app) {
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
static void app_sweep_idle(App *app, uint64_t nowMs) {
    uint64_t limitMs = (uint64_t)app->heartbeat_ms;
    for (int index = 0; index < APP_MAX_CONNS; index++) {
        AppConnSlot *slot = &app->slots[index];
        if (!slot->active || nowMs < slot->last_frame_ms) continue;
        if (nowMs - slot->last_frame_ms >= limitMs) app_close_slot(app, index);
    }
}

int app_run(const char *bind_ip, uint16_t port, const char *db_path, int auth_limit,
            int heartbeat_ms)
{
    App *app = calloc(1, sizeof(App));
    if (!app) {
        fprintf(stderr, "Failed to allocate App memory\n");
        return 1;
    }

    Status s = app_init(app, bind_ip, port, db_path, auth_limit);
    if (s != OK) {
        fprintf(stderr, "Failed to initialize server (error %d)\n", s);
        free(app);
        return 1;
    }
    if (heartbeat_ms > 0) app->heartbeat_ms = heartbeat_ms;

    if (!app->store) {
        s = store_open(&app->store, app->db_path);
        if (s != OK) {
            fprintf(stderr, "Failed to open database at %s (error %d)\n", app->db_path, s);
            app_cleanup(app);
            free(app);
            return 1;
        }
    }

    app->matches = matchesCreate(app->store, app_match_push, app_match_ended, app);
    if (!app->matches) {
        fprintf(stderr, "Failed to create match coordinator\n");
        app_cleanup(app);
        free(app);
        return 1;
    }

    printf("PORT %u\n", app->bound_port);
    fflush(stdout);

    struct pollfd pfd[2 + APP_MAX_CONNS];
    int slot_map[2 + APP_MAX_CONNS];

    while (app->running && !g_stop_requested) {
        /* Turn deadlines and bot moves advance even when no socket is readable. */
        uint64_t tickMs = app_now_ms();
        matchesTick(app->matches, tickMs);
        /* Retries failed match starts and drops logged-out users from the queue. */
        queue_tick(app, tickMs);

        pfd[0].fd = app->listen_fd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;

        pfd[1].fd = app->auth_pipe_read;
        pfd[1].events = (app->auth_pipe_read >= 0) ? POLLIN : 0;
        pfd[1].revents = 0;

        int nfds = 2;

        for (int i = 0; i < APP_MAX_CONNS; i++) {
            if (app->slots[i].active && app->slots[i].conn.fd >= 0) {
                pfd[nfds].fd = app->slots[i].conn.fd;
                pfd[nfds].events = POLLIN;
                if (app->slots[i].conn.out_len > app->slots[i].conn.out_pos) {
                    pfd[nfds].events |= POLLOUT;
                }
                pfd[nfds].revents = 0;
                slot_map[nfds] = i;
                nfds++;
            }
        }

        int ret = poll(pfd, nfds, 50);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Check auth worker pipe for completed auth tasks */
        if (pfd[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            auth_drain(app);
        }

        /* Check listening socket */
        if (pfd[0].revents & POLLIN) {
            while (1) {
                struct sockaddr_in client_addr;
                socklen_t clen = sizeof(client_addr);
                int cfd = accept(app->listen_fd, (struct sockaddr *)&client_addr, &clen);
                if (cfd < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }

                int free_slot = -1;
                for (int i = 0; i < APP_MAX_CONNS; i++) {
                    if (!app->slots[i].active) {
                        free_slot = i;
                        break;
                    }
                }

                if (free_slot < 0) {
                    /* Server capacity reached: reject client */
                    struct json_object *payload = json_object_new_object();
                    json_object_object_add(payload, "code", json_object_new_string("SERVER_FULL"));
                    json_object_object_add(payload, "message", json_object_new_string("Server connection limit reached"));
                    Envelope full_env = {1, 0, "ERROR", payload};
                    uint8_t out[256];
                    size_t written = 0;
                    if (frame_encode(&full_env, out, sizeof(out), &written) == OK) {
                        send(cfd, out, written, MSG_NOSIGNAL);
                    }
                    json_object_put(payload);
                    close(cfd);
                    continue;
                }

                AppConnSlot *slot = &app->slots[free_slot];
                slot->active = true;
                slot->generation++;
                inet_ntop(AF_INET, &client_addr.sin_addr, slot->client_ip, sizeof(slot->client_ip));
                slot->rate_last_ms = app_now_ms();
                slot->rate_tokens = 60.0;
                slot->last_request_id = 0;
                /* A fresh socket gets one full heartbeat window to send its first frame. */
                slot->last_frame_ms = slot->rate_last_ms;
                conn_init(&slot->conn, cfd, (uint32_t)free_slot, slot->generation);
            }
        }

        /* Check client sockets */
        for (int k = 2; k < nfds; k++) {
            int s_idx = slot_map[k];
            AppConnSlot *slot = &app->slots[s_idx];
            if (!slot->active) continue;

            if (pfd[k].revents & POLLOUT) {
                Status st = conn_flush(&slot->conn);
                if (st != OK || slot->conn.closing) {
                    app_close_slot(app, s_idx);
                    continue;
                }
            }

            if (pfd[k].revents & POLLIN) {
                FrameContext ctx = {app, (uint32_t)s_idx, slot->generation};
                Status st = conn_read(&slot->conn, app_on_frame, &ctx);
                if (st != OK || slot->conn.closing) {
                    app_close_slot(app, s_idx);
                    continue;
                }
            }

            if (pfd[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                app_close_slot(app, s_idx);
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

        app_sweep_idle(app, app_now_ms());
    }

    app_announce_shutdown(app);
    app_cleanup(app);
    free(app);
    return 0;
}
