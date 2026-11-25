#define _POSIX_C_SOURCE 200809L
#include "accounts.h"
#include "auth_worker.h"
#include "match.h"
#include "queue.h"
#include "rooms.h"
#include "social.h"
#include "store.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_RATE_LIMIT_IPS 256
#define MAX_TIMESTAMPS_PER_IP 128

typedef struct {
    char ip[64];
    uint64_t timestamps[MAX_TIMESTAMPS_PER_IP];
    size_t count;
    uint64_t last_seen_ms;
} IpRateEntry;

struct RateLimiter {
    IpRateEntry entries[MAX_RATE_LIMIT_IPS];
};

uint64_t accounts_get_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t base_offset_ms = 100ULL * 24ULL * 3600ULL * 1000ULL;
    return base_offset_ms + (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint64_t get_now_ms(void)
{
    return accounts_get_now_ms();
}

/* -------------------- Validation Functions -------------------- */

bool accounts_validate_email(const char *email)
{
    if (!email) return false;
    size_t len = strlen(email);
    if (len < 3 || len > 254) return false;

    const char *at = strchr(email, '@');
    if (!at) return false;
    if (at == email) return false; /* empty local part */
    if (*(at + 1) == '\0') return false; /* empty domain */
    if (strchr(at + 1, '@')) return false; /* multiple @ */

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)email[i];
        if (c < 33 || c > 126) return false; /* no spaces, control chars, non-ASCII */
        if (c >= 'A' && c <= 'Z') return false; /* lowercase ASCII only */
        /* Allowed ASCII characters in email */
        if (isalnum(c) || c == '.' || c == '_' || c == '-' || c == '+' || c == '@' || c == '%' || c == '~') {
            continue;
        }
        return false;
    }

    return true;
}

bool accounts_validate_password(const char *password)
{
    if (!password) return false;
    size_t len = strlen(password);
    return len >= 6 && len <= 128;
}

bool accounts_validate_name(const char *name)
{
    if (!name) return false;
    size_t byte_len = strlen(name);
    if (byte_len < 1 || byte_len > 64) return false;

    size_t cp_count = 0;
    const unsigned char *s = (const unsigned char *)name;

    while (*s) {
        unsigned char b0 = *s;
        size_t seq_len = 0;

        if (b0 <= 0x7F) {
            /* 1-byte ASCII */
            if (b0 < 32 || b0 == 127) return false; /* control characters forbidden */
            seq_len = 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            /* 2-byte sequence */
            if (b0 < 0xC2) return false; /* overlong */
            unsigned char b1 = s[1];
            if ((b1 & 0xC0) != 0x80) return false;
            seq_len = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            /* 3-byte sequence */
            unsigned char b1 = s[1];
            unsigned char b2 = s[2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            if (b0 == 0xE0 && b1 < 0xA0) return false; /* overlong */
            if (b0 == 0xED && b1 >= 0xA0) return false; /* surrogate */
            seq_len = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            /* 4-byte sequence */
            unsigned char b1 = s[1];
            unsigned char b2 = s[2];
            unsigned char b3 = s[3];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return false;
            if (b0 == 0xF0 && b1 < 0x90) return false; /* overlong */
            if (b0 == 0xF4 && b1 > 0x8F) return false; /* > U+10FFFF */
            seq_len = 4;
        } else {
            return false;
        }

        cp_count++;
        s += seq_len;
    }

    return cp_count >= 1 && cp_count <= 16;
}

bool accounts_validate_avatar(const char *avatar)
{
    if (!avatar || avatar[0] == '\0') return false;

    static const char *const WHITELIST[] = {
        "fox", "panda", "bear", "owl", "lion", "tiger", "rabbit", "koala",
        "boy", "girl", "bun", "afro", "nerd", "beard", "cap", "punk", "grandpa",
        "blonde", "pigtails", "cat", "frog", "robot", "bunny", "ghost", "alien",
        "sun", "cool"
    };
    static const size_t COUNT = sizeof(WHITELIST) / sizeof(WHITELIST[0]);

    for (size_t i = 0; i < COUNT; i++) {
        if (strcmp(avatar, WHITELIST[i]) == 0) return true;
    }

    return false;
}

/* -------------------- Token & Tag Helpers -------------------- */

void accounts_generate_token(char out_token[65])
{
    unsigned char bytes[32];
    randombytes_buf(bytes, sizeof(bytes));
    sodium_bin2hex(out_token, 65, bytes, sizeof(bytes));
}

void accounts_format_tag(UserId user_id, char out_tag[32])
{
    snprintf(out_tag, 32, "UNO#%llu", (unsigned long long)user_id);
}

static void accounts_send_error(App *app, uint32_t slot_idx, int32_t req_id, const char *code, const char *msg)
{
    if (slot_idx >= APP_MAX_CONNS) return;
    AppConnSlot *slot = &app->slots[slot_idx];
    if (!slot->active) return;

    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "code", json_object_new_string(code));
    json_object_object_add(payload, "message", json_object_new_string(msg));

    Envelope resp = {1, req_id, "ERROR", payload};
    conn_queue(&slot->conn, &resp);
    json_object_put(payload);
    conn_flush(&slot->conn);
}

/* -------------------- Sessions Lifecycle -------------------- */

Status sessions_init(App *app)
{
    if (!app) return BAD_REQUEST;
    if (!app->sessions) {
        app->sessions = calloc(1, sizeof(Sessions));
        if (!app->sessions) return STORAGE_ERROR;
    }
    if (!app->rate_limiter) {
        app->rate_limiter = calloc(1, sizeof(RateLimiter));
        if (!app->rate_limiter) {
            free(app->sessions);
            app->sessions = NULL;
            return STORAGE_ERROR;
        }
    }
    return OK;
}

void sessions_cleanup(App *app)
{
    if (!app) return;
    if (app->sessions) {
        free(app->sessions);
        app->sessions = NULL;
    }
    if (app->rate_limiter) {
        free(app->rate_limiter);
        app->rate_limiter = NULL;
    }
}

Status sessions_bind(App *app, ConnId conn_id, UserId user_id, const char *token)
{
    if (!app || !app->sessions || !token) return BAD_REQUEST;

    uint64_t now = get_now_ms();
    Sessions *s = app->sessions;

    /* Check if user already has an active session */
    SessionEntry *slot_entry = NULL;
    SessionEntry *free_entry = NULL;

    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (s->entries[i].in_use && s->entries[i].user_id == user_id) {
            slot_entry = &s->entries[i];
            break;
        }
        if (!s->entries[i].in_use && !free_entry) {
            free_entry = &s->entries[i];
        }
    }

    SessionEntry *entry = slot_entry ? slot_entry : free_entry;
    if (!entry) {
        /* Evict oldest unused or disconnected entry */
        uint64_t oldest = UINT64_MAX;
        for (size_t i = 0; i < MAX_SESSIONS; i++) {
            if (!s->entries[i].connected && s->entries[i].created_at_ms < oldest) {
                oldest = s->entries[i].created_at_ms;
                entry = &s->entries[i];
            }
        }
    }

    if (!entry) return BUSY;

    entry->in_use = true;
    entry->user_id = user_id;
    strncpy(entry->token, token, sizeof(entry->token) - 1);
    entry->conn_id = conn_id;
    entry->connected = true;
    entry->created_at_ms = now;

    return OK;
}

Status sessions_user(App *app, ConnId conn_id, UserId *out)
{
    if (!app || !app->sessions || !out) return BAD_REQUEST;
    if (conn_id.slot >= APP_MAX_CONNS) return UNAUTHENTICATED;

    AppConnSlot *slot = &app->slots[conn_id.slot];
    if (!slot->active || slot->generation != conn_id.generation) {
        return UNAUTHENTICATED;
    }

    Sessions *s = app->sessions;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (s->entries[i].in_use && s->entries[i].connected &&
            s->entries[i].conn_id.slot == conn_id.slot &&
            s->entries[i].conn_id.generation == conn_id.generation) {
            *out = s->entries[i].user_id;
            return OK;
        }
    }

    return UNAUTHENTICATED;
}

Status sessions_find_conn(App *app, UserId user_id, ConnId *out_conn)
{
    if (!app || !app->sessions || !out_conn) return BAD_REQUEST;
    Sessions *s = app->sessions;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (s->entries[i].in_use && s->entries[i].connected && s->entries[i].user_id == user_id) {
            *out_conn = s->entries[i].conn_id;
            return OK;
        }
    }
    return NOT_FOUND;
}

enum {
    TOKEN_HEX_LENGTH = 64,
    /* Spec: a session token is valid for 24 hours after LOGIN. */
    SESSION_TTL_MS = 24 * 3600 * 1000
};

/* The entry a RESUME token names, or NULL; an expired entry is forgotten. */
static SessionEntry *find_live_token(Sessions *sessions, const char *token)
{
    if (!token || strlen(token) != TOKEN_HEX_LENGTH) return NULL;
    for (size_t index = 0; index < MAX_SESSIONS; index++) {
        SessionEntry *entry = &sessions->entries[index];
        if (!entry->in_use || strcmp(entry->token, token) != 0) continue;
        uint64_t now = get_now_ms();
        if (now > entry->created_at_ms && now - entry->created_at_ms > SESSION_TTL_MS) {
            memset(entry, 0, sizeof(*entry));
            return NULL;
        }
        return entry;
    }
    return NULL;
}

Status sessions_resume(App *app, ConnId conn_id, const char *token)
{
    if (!app || !app->sessions || !token) return BAD_REQUEST;
    if (conn_id.slot >= APP_MAX_CONNS) return UNAUTHENTICATED;

    AppConnSlot *slot = &app->slots[conn_id.slot];
    if (!slot->active || slot->generation != conn_id.generation) {
        return UNAUTHENTICATED;
    }

    SessionEntry *entry = find_live_token(app->sessions, token);
    if (!entry) return UNAUTHENTICATED;
    /* Found matching session. Reassign to new conn_id */
    entry->conn_id = conn_id;
    entry->connected = true;
    return OK;
}

/* One socket carries one account. Binding a different account takes the
 * current one off the socket like a LOGOUT, so its seat, room and party are
 * cleaned up now; otherwise both would share the socket and only one would
 * be cleaned up when it closes. */
static void release_other_account(App *app, ConnId conn, UserId incoming)
{
    UserId current = 0;
    if (sessions_user(app, conn, &current) != OK || current == 0) return;
    if (current == incoming) return;
    app_user_leave(app, conn, current);
    sessions_logout(app, conn);
}

void sessions_disconnect(App *app, ConnId conn_id)
{
    if (!app || !app->sessions) return;

    Sessions *s = app->sessions;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (s->entries[i].in_use &&
            s->entries[i].conn_id.slot == conn_id.slot &&
            s->entries[i].conn_id.generation == conn_id.generation) {
            s->entries[i].connected = false;
        }
    }
}

void sessions_logout(App *app, ConnId conn_id)
{
    if (!app || !app->sessions) return;

    Sessions *s = app->sessions;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (s->entries[i].in_use &&
            s->entries[i].conn_id.slot == conn_id.slot &&
            s->entries[i].conn_id.generation == conn_id.generation) {
            memset(&s->entries[i], 0, sizeof(s->entries[i]));
        }
    }
}

/* -------------------- Rate Limiter -------------------- */

bool accounts_rate_limit_check(App *app, const char *ip)
{
    if (!app || !app->rate_limiter || !ip || ip[0] == '\0') return true;
    if (app->auth_limit <= 0) return true;

    uint64_t now_ms = get_now_ms();
    RateLimiter *rl = app->rate_limiter;

    IpRateEntry *entry = NULL;
    IpRateEntry *free_entry = NULL;

    for (size_t i = 0; i < MAX_RATE_LIMIT_IPS; i++) {
        if (rl->entries[i].ip[0] != '\0' && strcmp(rl->entries[i].ip, ip) == 0) {
            entry = &rl->entries[i];
            break;
        }
        if (rl->entries[i].ip[0] == '\0' && !free_entry) {
            free_entry = &rl->entries[i];
        }
    }

    if (!entry) {
        if (!free_entry) {
            /* Evict LRU */
            uint64_t oldest = UINT64_MAX;
            for (size_t i = 0; i < MAX_RATE_LIMIT_IPS; i++) {
                if (rl->entries[i].last_seen_ms < oldest) {
                    oldest = rl->entries[i].last_seen_ms;
                    free_entry = &rl->entries[i];
                }
            }
        }
        entry = free_entry;
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->ip, ip, sizeof(entry->ip) - 1);
    }

    /* Evict timestamps older than 60 seconds (60000 ms) */
    size_t valid_count = 0;
    for (size_t k = 0; k < entry->count; k++) {
        if (now_ms >= entry->timestamps[k] && now_ms - entry->timestamps[k] < 60000) {
            entry->timestamps[valid_count++] = entry->timestamps[k];
        }
    }
    entry->count = valid_count;

    if (entry->count >= (size_t)app->auth_limit) {
        return false; /* Limit exceeded */
    }

    if (entry->count < MAX_TIMESTAMPS_PER_IP) {
        entry->timestamps[entry->count++] = now_ms;
    }
    entry->last_seen_ms = now_ms;

    return true;
}

/* -------------------- Envelope Handlers -------------------- */

Status accounts_handle(App *app, ConnId conn_id, const Envelope *e)
{
    if (!app || !e) return BAD_REQUEST;
    if (conn_id.slot >= APP_MAX_CONNS) return BAD_REQUEST;

    AppConnSlot *slot = &app->slots[conn_id.slot];
    if (!slot->active || slot->generation != conn_id.generation) {
        return BAD_REQUEST;
    }

    const char *ip = slot->client_ip;

    if (strcmp(e->type, "REGISTER") == 0) {
        if (!accounts_rate_limit_check(app, ip)) {
            accounts_send_error(app, conn_id.slot, e->id, "RATE_LIMITED", "Rate limit exceeded");
            return OK;
        }

        struct json_object *j_email = NULL, *j_pass = NULL, *j_name = NULL, *j_avatar = NULL;
        if (!e->payload ||
            !json_object_object_get_ex(e->payload, "email", &j_email) ||
            !json_object_object_get_ex(e->payload, "password", &j_pass) ||
            !json_object_object_get_ex(e->payload, "name", &j_name) ||
            !json_object_object_get_ex(e->payload, "avatar", &j_avatar)) {
            accounts_send_error(app, conn_id.slot, e->id, "BAD_REQUEST", "Missing required fields");
            return OK;
        }

        const char *email = json_object_get_string(j_email);
        const char *pass = json_object_get_string(j_pass);
        const char *name = json_object_get_string(j_name);
        const char *avatar = json_object_get_string(j_avatar);

        if (!accounts_validate_email(email) ||
            !accounts_validate_password(pass) ||
            !accounts_validate_name(name) ||
            !accounts_validate_avatar(avatar)) {
            accounts_send_error(app, conn_id.slot, e->id, "BAD_REQUEST", "Invalid registration input");
            return OK;
        }

        AuthJob job;
        memset(&job, 0, sizeof(job));
        job.kind = AUTH_REGISTER;
        job.conn_id = conn_id;
        job.req_id = e->id;
        strncpy(job.email, email, sizeof(job.email) - 1);
        strncpy(job.password, pass, sizeof(job.password) - 1);
        strncpy(job.name, name, sizeof(job.name) - 1);
        strncpy(job.avatar, avatar, sizeof(job.avatar) - 1);

        Status s = auth_submit(app->auth_worker, &job);
        if (s != OK) {
            accounts_send_error(app, conn_id.slot, e->id, "BUSY", "Server is busy");
        }
        return OK;
    }

    if (strcmp(e->type, "LOGIN") == 0) {
        if (!accounts_rate_limit_check(app, ip)) {
            accounts_send_error(app, conn_id.slot, e->id, "RATE_LIMITED", "Rate limit exceeded");
            return OK;
        }

        struct json_object *j_email = NULL, *j_pass = NULL;
        if (!e->payload ||
            !json_object_object_get_ex(e->payload, "email", &j_email) ||
            !json_object_object_get_ex(e->payload, "password", &j_pass)) {
            accounts_send_error(app, conn_id.slot, e->id, "BAD_REQUEST", "Missing email or password");
            return OK;
        }

        const char *email = json_object_get_string(j_email);
        const char *pass = json_object_get_string(j_pass);

        if (!accounts_validate_email(email) || !accounts_validate_password(pass)) {
            accounts_send_error(app, conn_id.slot, e->id, "BAD_REQUEST", "Invalid login input format");
            return OK;
        }

        AuthJob job;
        memset(&job, 0, sizeof(job));
        job.kind = AUTH_LOGIN;
        job.conn_id = conn_id;
        job.req_id = e->id;
        strncpy(job.email, email, sizeof(job.email) - 1);
        strncpy(job.password, pass, sizeof(job.password) - 1);

        Account acc;
        Status st = store_account_get(app->store, email, &acc);
        if (st == OK) {
            job.user_exists = true;
            strncpy(job.hash, acc.hash, sizeof(job.hash) - 1);
            job.account = acc;
        } else {
            job.user_exists = false;
        }

        Status s = auth_submit(app->auth_worker, &job);
        if (s != OK) {
            accounts_send_error(app, conn_id.slot, e->id, "BUSY", "Server is busy");
        }
        return OK;
    }

    if (strcmp(e->type, "RESUME") == 0) {
        struct json_object *j_token = NULL;
        if (!e->payload || !json_object_object_get_ex(e->payload, "token", &j_token)) {
            accounts_send_error(app, conn_id.slot, e->id, "BAD_REQUEST", "Missing token");
            return OK;
        }

        const char *token = json_object_get_string(j_token);
        /* Look the token up first: an invalid one must not cost the socket
         * the account it already carries. */
        const SessionEntry *target = find_live_token(app->sessions, token);
        if (target) release_other_account(app, conn_id, target->user_id);
        Status st = sessions_resume(app, conn_id, token);
        if (st == OK) {
            UserId uid = 0;
            sessions_user(app, conn_id, &uid);

            Account acc;
            memset(&acc, 0, sizeof(acc));
            store_account_get_by_id(app->store, uid, &acc);

            /* The client already holds the token it just sent; echoing it adds
             * nothing and only widens where the secret appears on the wire. */
            struct json_object *payload = json_object_new_object();

            struct json_object *me = json_object_new_object();
            char uid_str[32], tag_str[32];
            snprintf(uid_str, sizeof(uid_str), "%llu", (unsigned long long)uid);
            accounts_format_tag(uid, tag_str);
            json_object_object_add(me, "userId", json_object_new_string(uid_str));
            json_object_object_add(me, "tag", json_object_new_string(tag_str));
            json_object_object_add(me, "name", json_object_new_string(acc.name));
            json_object_object_add(me, "avatar", json_object_new_string(acc.avatar));
            json_object_object_add(me, "elo", json_object_new_int(acc.elo));

            json_object_object_add(payload, "me", me);

            Envelope resp = {1, e->id, "OK", payload};
            conn_queue(&slot->conn, &resp);
            json_object_put(payload);
            conn_flush(&slot->conn);

            /* After OK so the client sees the session before its GAME_STATE.
             * NOT_FOUND just means the user is not in a match. */
            (void)matchesConnected(app->matches, uid, true, app_now_ms());
            /* Like LOGIN: the session moved to a socket that has seen no
             * room, party or queue snapshot yet. */
            rooms_sync_user(app, uid);
            queue_sync_user(app, uid);
            social_presence(app, uid);
        } else {
            accounts_send_error(app, conn_id.slot, e->id, "UNAUTHENTICATED", "Invalid or expired token");
        }
        return OK;
    }

    if (strcmp(e->type, "LOGOUT") == 0) {
        UserId uid = 0;
        /* Spec §5: outside a match the user leaves queue, invites, party and
         * waiting room at once. This runs while the session still maps
         * conn -> user, so the leaver also gets the closing snapshots and
         * friends see the account offline. */
        if (sessions_user(app, conn_id, &uid) == OK && uid != 0) {
            app_user_leave(app, conn_id, uid);
        }
        sessions_logout(app, conn_id);
        struct json_object *payload = json_object_new_object();
        Envelope resp = {1, e->id, "OK", payload};
        conn_queue(&slot->conn, &resp);
        json_object_put(payload);
        conn_flush(&slot->conn);
        return OK;
    }

    return BAD_REQUEST;
}

void auth_drain(App *app)
{
    if (!app || app->auth_pipe_read < 0) return;

    AuthResult res;
    while (1) {
        ssize_t n = read(app->auth_pipe_read, &res, sizeof(res));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
        if (n < (ssize_t)sizeof(res)) break;

        /* Verify connection slot and generation */
        uint32_t slot_idx = res.conn_id.slot;
        if (slot_idx >= APP_MAX_CONNS) continue;

        AppConnSlot *slot = &app->slots[slot_idx];
        if (!slot->active || slot->generation != res.conn_id.generation) {
            /* Client disconnected or slot reassigned. Safely drop auth result! */
            continue;
        }

        if (res.kind == AUTH_REGISTER) {
            if (res.status == OK) {
                AccountCreate ac;
                memset(&ac, 0, sizeof(ac));
                strncpy(ac.email, res.account.email, sizeof(ac.email) - 1);
                strncpy(ac.hash, res.account.hash, sizeof(ac.hash) - 1);
                strncpy(ac.name, res.account.name, sizeof(ac.name) - 1);
                strncpy(ac.avatar, res.account.avatar, sizeof(ac.avatar) - 1);

                UserId new_uid = 0;
                Status st = store_account_create(app->store, &ac, &new_uid);
                if (st == OK) {
                    struct json_object *payload = json_object_new_object();
                    char uid_str[32];
                    snprintf(uid_str, sizeof(uid_str), "%llu", (unsigned long long)new_uid);
                    json_object_object_add(payload, "userId", json_object_new_string(uid_str));

                    Envelope resp = {1, res.req_id, "OK", payload};
                    conn_queue(&slot->conn, &resp);
                    json_object_put(payload);
                    conn_flush(&slot->conn);
                } else if (st == BAD_REQUEST) {
                    accounts_send_error(app, slot_idx, res.req_id, "BAD_REQUEST", "Email already registered");
                } else {
                    accounts_send_error(app, slot_idx, res.req_id, "STORAGE_ERROR", "Database storage failure");
                }
            } else {
                accounts_send_error(app, slot_idx, res.req_id, "STORAGE_ERROR", "Password hashing failed");
            }
        } else if (res.kind == AUTH_LOGIN) {
            if (res.status == OK) {
                release_other_account(app, res.conn_id, res.user_id);
                /* Disconnect any existing connection for this user */
                ConnId old_conn;
                if (sessions_find_conn(app, res.user_id, &old_conn) == OK) {
                    if (old_conn.slot != res.conn_id.slot && old_conn.slot < APP_MAX_CONNS) {
                        /* Detach the session first so app_close_slot sees an
                         * anonymous socket: the user is moving, not leaving, so
                         * no offline presence (which aborts a match with no
                         * other connected human) and no room/party exit. */
                        sessions_disconnect(app, old_conn);
                        app_close_slot(app, old_conn.slot);
                    }
                }

                /* Bind session to current connection */
                sessions_bind(app, res.conn_id, res.user_id, res.token);

                struct json_object *payload = json_object_new_object();
                json_object_object_add(payload, "token", json_object_new_string(res.token));

                struct json_object *me = json_object_new_object();
                char uid_str[32], tag_str[32];
                snprintf(uid_str, sizeof(uid_str), "%llu", (unsigned long long)res.user_id);
                accounts_format_tag(res.user_id, tag_str);
                json_object_object_add(me, "userId", json_object_new_string(uid_str));
                json_object_object_add(me, "tag", json_object_new_string(tag_str));
                json_object_object_add(me, "name", json_object_new_string(res.account.name));
                json_object_object_add(me, "avatar", json_object_new_string(res.account.avatar));
                json_object_object_add(me, "elo", json_object_new_int(res.account.elo));

                json_object_object_add(payload, "me", me);

                Envelope resp = {1, res.req_id, "OK", payload};
                conn_queue(&slot->conn, &resp);
                json_object_put(payload);
                conn_flush(&slot->conn);

                /* A re-login reclaims the seat. If the seat never went offline
                 * the coordinator still sends this connection a fresh view.
                 * NOT_FOUND just means the user is not in a match. */
                (void)matchesConnected(app->matches, res.user_id, true, app_now_ms());
                /* A re-login keeps room and party membership, but this socket has
                 * never seen their snapshots. */
                rooms_sync_user(app, res.user_id);
                queue_sync_user(app, res.user_id);
                /* Also hands this socket its first FRIEND_STATE. */
                social_presence(app, res.user_id);
            } else {
                accounts_send_error(app, slot_idx, res.req_id, "UNAUTHENTICATED", "Invalid email or password");
            }
        }
    }
}
