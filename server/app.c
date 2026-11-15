#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "accounts.h"
#include "auth_worker.h"
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

static uint64_t get_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
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
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
        rooms_disconnect(app, uid, now_ms);
    }
    sessions_disconnect(app, (ConnId){(uint32_t)slot_idx, s->generation});
    conn_close(&s->conn);
    s->active = false;
    s->generation++;
    s->client_ip[0] = '\0';
    s->rate_last_ms = 0;
    s->rate_tokens = 0.0;
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

    uint64_t now_ms = get_monotonic_ms();
    if (!app_conn_check_rate_limit(slot, now_ms)) {
        struct json_object *payload = json_object_new_object();
        json_object_object_add(payload, "code", json_object_new_string("RATE_LIMITED"));
        json_object_object_add(payload, "message", json_object_new_string("Too many requests"));
        Envelope resp = {1, e->id, "ERROR", payload};
        conn_queue(&slot->conn, &resp);
        json_object_put(payload);
        conn_flush(&slot->conn);
        return OK;
    }

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

    struct json_object *payload = json_object_new_object();
    Envelope resp = {1, e->id, "BAD_REQUEST", payload};
    Status s = conn_queue(&slot->conn, &resp);
    json_object_put(payload);
    if (s != OK) return s;
    return conn_flush(&slot->conn);
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
    rooms_init(&app->rooms, app, NULL);

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
    if (app->rooms) {
        rooms_destroy(app->rooms);
        app->rooms = NULL;
    }
    sessions_cleanup(app);
    if (app->store) {
        store_close(app->store);
        app->store = NULL;
    }
    app->running = false;
}

int app_run(const char *bind_ip, uint16_t port, const char *db_path, int auth_limit)
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

    if (!app->store) {
        s = store_open(&app->store, app->db_path);
        if (s != OK) {
            fprintf(stderr, "Failed to open database at %s (error %d)\n", app->db_path, s);
            app_cleanup(app);
            free(app);
            return 1;
        }
    }

    printf("PORT %u\n", app->bound_port);
    fflush(stdout);

    struct pollfd pfd[2 + APP_MAX_CONNS];
    int slot_map[2 + APP_MAX_CONNS];

    while (app->running && !g_stop_requested) {
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
                slot->rate_last_ms = get_monotonic_ms();
                slot->rate_tokens = 60.0;
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
    }

    app_cleanup(app);
    free(app);
    return 0;
}
