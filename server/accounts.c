#define _POSIX_C_SOURCE 200809L
#include "accounts.h"
#include "auth_worker.h"
#include "server/game/match.h"
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

enum {
    EMAIL_MIN_LENGTH = 3,
    EMAIL_MAX_LENGTH = 254,
    PASSWORD_MIN_LENGTH = 6,
    PASSWORD_MAX_LENGTH = 128,
    NAME_MAX_BYTES = 64,
    NAME_MAX_CODE_POINTS = 16,
    /* Sliding window for the per-IP auth limit. */
    AUTH_RATE_WINDOW_MS = 60000
};

/* Auth attempts seen from one client IP inside the sliding window. */
typedef struct {
    char ip[64];
    uint64_t timestamps[MAX_TIMESTAMPS_PER_IP];
    size_t count;
    uint64_t lastSeenMs;
} IpRateEntry;

/* Fixed table; the least recently seen IP is evicted when it is full. */
struct RateLimiter {
    IpRateEntry entries[MAX_RATE_LIMIT_IPS];
};

uint64_t accounts_get_now_ms(void) {
    struct timespec monotonicTime;
    clock_gettime(CLOCK_MONOTONIC, &monotonicTime);
    /* Offset by 100 days so every real timestamp sits far above 0, the value
     * zeroed session and rate-limit entries hold. CLOCK_MONOTONIC starts near
     * zero at boot and never jumps with wall-clock changes. */
    const uint64_t baseOffsetMs = 100ULL * 24ULL * 3600ULL * 1000ULL;
    return baseOffsetMs + (uint64_t)monotonicTime.tv_sec * 1000 +
           (uint64_t)(monotonicTime.tv_nsec / 1000000);
}

static uint64_t monotonicNowMs(void) {
    return accounts_get_now_ms();
}

/* -------------------- Validation Functions -------------------- */

bool accounts_validate_email(const char *email) {
    if (!email) return false;
    size_t length = strlen(email);
    if (length < EMAIL_MIN_LENGTH || length > EMAIL_MAX_LENGTH) return false;

    const char *atSign = strchr(email, '@');
    if (!atSign) return false;
    if (atSign == email) return false; /* empty local part */
    if (*(atSign + 1) == '\0') return false; /* empty domain */
    if (strchr(atSign + 1, '@')) return false; /* multiple @ */

    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char)email[i];
        /* No spaces, control characters or non-ASCII bytes. */
        if (character < 33 || character > 126) return false;
        if (character >= 'A' && character <= 'Z') return false; /* lowercase ASCII only */
        /* Allowed ASCII characters in email */
        if (isalnum(character) || strchr("._-+@%~", character)) {
            continue;
        }
        return false;
    }

    return true;
}

bool accounts_validate_password(const char *password) {
    if (!password) return false;
    size_t length = strlen(password);
    return length >= PASSWORD_MIN_LENGTH && length <= PASSWORD_MAX_LENGTH;
}

bool accounts_validate_name(const char *name) {
    if (!name) return false;
    size_t byteLength = strlen(name);
    if (byteLength < 1 || byteLength > NAME_MAX_BYTES) return false;

    size_t codePointCount = 0;
    const unsigned char *cursor = (const unsigned char *)name;

    while (*cursor) {
        unsigned char b0 = *cursor;
        size_t sequenceLength = 0;

        if (b0 <= 0x7F) {
            /* 1-byte ASCII */
            if (b0 < 32 || b0 == 127) return false; /* control characters forbidden */
            sequenceLength = 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            /* 2-byte sequence */
            if (b0 < 0xC2) return false; /* overlong */
            unsigned char b1 = cursor[1];
            if ((b1 & 0xC0) != 0x80) return false;
            sequenceLength = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            /* 3-byte sequence */
            unsigned char b1 = cursor[1];
            unsigned char b2 = cursor[2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            if (b0 == 0xE0 && b1 < 0xA0) return false; /* overlong */
            if (b0 == 0xED && b1 >= 0xA0) return false; /* surrogate */
            sequenceLength = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            /* 4-byte sequence */
            unsigned char b1 = cursor[1];
            unsigned char b2 = cursor[2];
            unsigned char b3 = cursor[3];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return false;
            if (b0 == 0xF0 && b1 < 0x90) return false; /* overlong */
            if (b0 == 0xF4 && b1 > 0x8F) return false; /* > U+10FFFF */
            sequenceLength = 4;
        } else {
            return false;
        }

        codePointCount++;
        cursor += sequenceLength;
    }

    return codePointCount >= 1 && codePointCount <= NAME_MAX_CODE_POINTS;
}

bool accounts_validate_avatar(const char *avatar) {
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

void accounts_generate_token(char outToken[65]) {
    unsigned char bytes[32];
    randombytes_buf(bytes, sizeof(bytes));
    sodium_bin2hex(outToken, 65, bytes, sizeof(bytes));
}

void accounts_format_tag(UserId userId, char outTag[32]) {
    snprintf(outTag, 32, "UNO#%llu", (unsigned long long)userId);
}

/* Reply ERROR {code, message} straight to a slot. A send failure only marks the
 * connection closing; the poll loop tears it down afterwards. */
static void sendAccountError(App *app, uint32_t slotIndex, int32_t requestId, const char *code,
                             const char *message) {
    if (slotIndex >= APP_MAX_CONNS) return;
    AppConnSlot *slot = &app->slots[slotIndex];
    if (!slot->active) return;

    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "code", json_object_new_string(code));
    json_object_object_add(payload, "message", json_object_new_string(message));

    Envelope response = {1, requestId, "ERROR", payload};
    conn_queue(&slot->conn, &response);
    json_object_put(payload);
    conn_flush(&slot->conn);
}

/* -------------------- Sessions Lifecycle -------------------- */

Status sessions_init(App *app) {
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

void sessions_cleanup(App *app) {
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

Status sessions_bind(App *app, ConnId connId, UserId userId, const char *token) {
    if (!app || !app->sessions || !token) return BAD_REQUEST;

    uint64_t now = monotonicNowMs();
    Sessions *sessions = app->sessions;

    /* Check if user already has an active session */
    SessionEntry *existingEntry = NULL;
    SessionEntry *freeEntry = NULL;

    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions->entries[i].in_use && sessions->entries[i].user_id == userId) {
            existingEntry = &sessions->entries[i];
            break;
        }
        if (!sessions->entries[i].in_use && !freeEntry) {
            freeEntry = &sessions->entries[i];
        }
    }

    SessionEntry *entry = existingEntry ? existingEntry : freeEntry;
    if (!entry) {
        /* Evict oldest unused or disconnected entry */
        uint64_t oldest = UINT64_MAX;
        for (size_t i = 0; i < MAX_SESSIONS; i++) {
            if (!sessions->entries[i].connected && sessions->entries[i].created_at_ms < oldest) {
                oldest = sessions->entries[i].created_at_ms;
                entry = &sessions->entries[i];
            }
        }
    }

    if (!entry) return BUSY;

    entry->in_use = true;
    entry->user_id = userId;
    strncpy(entry->token, token, sizeof(entry->token) - 1);
    entry->conn_id = connId;
    entry->connected = true;
    entry->created_at_ms = now;

    return OK;
}

Status sessions_user(App *app, ConnId connId, UserId *outUserId) {
    if (!app || !app->sessions || !outUserId) return BAD_REQUEST;
    if (connId.slot >= APP_MAX_CONNS) return UNAUTHENTICATED;

    AppConnSlot *slot = &app->slots[connId.slot];
    if (!slot->active || slot->generation != connId.generation) {
        return UNAUTHENTICATED;
    }

    Sessions *sessions = app->sessions;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions->entries[i].in_use && sessions->entries[i].connected &&
            sessions->entries[i].conn_id.slot == connId.slot &&
            sessions->entries[i].conn_id.generation == connId.generation) {
            *outUserId = sessions->entries[i].user_id;
            return OK;
        }
    }

    return UNAUTHENTICATED;
}

Status sessions_find_conn(App *app, UserId userId, ConnId *outConn) {
    if (!app || !app->sessions || !outConn) return BAD_REQUEST;
    Sessions *sessions = app->sessions;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        const SessionEntry *entry = &sessions->entries[i];
        if (entry->in_use && entry->connected && entry->user_id == userId) {
            *outConn = entry->conn_id;
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
static SessionEntry *findLiveToken(Sessions *sessions, const char *token) {
    if (!token || strlen(token) != TOKEN_HEX_LENGTH) return NULL;
    for (size_t index = 0; index < MAX_SESSIONS; index++) {
        SessionEntry *entry = &sessions->entries[index];
        if (!entry->in_use || strcmp(entry->token, token) != 0) continue;
        uint64_t now = monotonicNowMs();
        if (now > entry->created_at_ms && now - entry->created_at_ms > SESSION_TTL_MS) {
            memset(entry, 0, sizeof(*entry));
            return NULL;
        }
        return entry;
    }
    return NULL;
}

Status sessions_resume(App *app, ConnId connId, const char *token) {
    if (!app || !app->sessions || !token) return BAD_REQUEST;
    if (connId.slot >= APP_MAX_CONNS) return UNAUTHENTICATED;

    AppConnSlot *slot = &app->slots[connId.slot];
    if (!slot->active || slot->generation != connId.generation) {
        return UNAUTHENTICATED;
    }

    SessionEntry *entry = findLiveToken(app->sessions, token);
    if (!entry) return UNAUTHENTICATED;
    /* Found matching session. Reassign to new connId */
    entry->conn_id = connId;
    entry->connected = true;
    return OK;
}

/* One socket carries one account. Binding a different account takes the
 * current one off the socket like a LOGOUT, so its seat, room and party are
 * cleaned up now; otherwise both would share the socket and only one would
 * be cleaned up when it closes. */
static void releaseOtherAccount(App *app, ConnId conn, UserId incoming) {
    UserId current = 0;
    if (sessions_user(app, conn, &current) != OK || current == 0) return;
    if (current == incoming) return;
    app_user_leave(app, conn, current);
    sessions_logout(app, conn);
}

void sessions_disconnect(App *app, ConnId connId) {
    if (!app || !app->sessions) return;

    Sessions *sessions = app->sessions;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions->entries[i].in_use &&
            sessions->entries[i].conn_id.slot == connId.slot &&
            sessions->entries[i].conn_id.generation == connId.generation) {
            sessions->entries[i].connected = false;
        }
    }
}

void sessions_logout(App *app, ConnId connId) {
    if (!app || !app->sessions) return;

    Sessions *sessions = app->sessions;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions->entries[i].in_use &&
            sessions->entries[i].conn_id.slot == connId.slot &&
            sessions->entries[i].conn_id.generation == connId.generation) {
            memset(&sessions->entries[i], 0, sizeof(sessions->entries[i]));
        }
    }
}

/* -------------------- Rate Limiter -------------------- */

bool accounts_rate_limit_check(App *app, const char *ip) {
    if (!app || !app->rate_limiter || !ip || ip[0] == '\0') return true;
    if (app->auth_limit <= 0) return true;

    uint64_t nowMs = monotonicNowMs();
    RateLimiter *rateLimiter = app->rate_limiter;

    IpRateEntry *entry = NULL;
    IpRateEntry *freeEntry = NULL;

    for (size_t i = 0; i < MAX_RATE_LIMIT_IPS; i++) {
        if (rateLimiter->entries[i].ip[0] != '\0' && strcmp(rateLimiter->entries[i].ip, ip) == 0) {
            entry = &rateLimiter->entries[i];
            break;
        }
        if (rateLimiter->entries[i].ip[0] == '\0' && !freeEntry) {
            freeEntry = &rateLimiter->entries[i];
        }
    }

    if (!entry) {
        if (!freeEntry) {
            /* Evict LRU */
            uint64_t oldest = UINT64_MAX;
            for (size_t i = 0; i < MAX_RATE_LIMIT_IPS; i++) {
                if (rateLimiter->entries[i].lastSeenMs < oldest) {
                    oldest = rateLimiter->entries[i].lastSeenMs;
                    freeEntry = &rateLimiter->entries[i];
                }
            }
        }
        entry = freeEntry;
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->ip, ip, sizeof(entry->ip) - 1);
    }

    /* Drop attempts older than the window so only the last minute counts. */
    size_t validCount = 0;
    for (size_t k = 0; k < entry->count; k++) {
        if (nowMs >= entry->timestamps[k] && nowMs - entry->timestamps[k] < AUTH_RATE_WINDOW_MS) {
            entry->timestamps[validCount++] = entry->timestamps[k];
        }
    }
    entry->count = validCount;

    if (entry->count >= (size_t)app->auth_limit) {
        return false; /* Limit exceeded */
    }

    if (entry->count < MAX_TIMESTAMPS_PER_IP) {
        entry->timestamps[entry->count++] = nowMs;
    }
    entry->lastSeenMs = nowMs;

    return true;
}

/* -------------------- Envelope Handlers -------------------- */

Status accounts_handle(App *app, ConnId connId, const Envelope *envelope) {
    if (!app || !envelope) return BAD_REQUEST;
    if (connId.slot >= APP_MAX_CONNS) return BAD_REQUEST;

    AppConnSlot *slot = &app->slots[connId.slot];
    if (!slot->active || slot->generation != connId.generation) {
        return BAD_REQUEST;
    }

    const char *ip = slot->client_ip;

    if (strcmp(envelope->type, "REGISTER") == 0) {
        if (!accounts_rate_limit_check(app, ip)) {
            sendAccountError(app, connId.slot, envelope->id, "RATE_LIMITED", "Rate limit exceeded");
            return OK;
        }

        struct json_object *emailJson = NULL, *passwordJson = NULL;
        struct json_object *nameJson = NULL, *avatarJson = NULL;
        if (!envelope->payload ||
            !json_object_object_get_ex(envelope->payload, "email", &emailJson) ||
            !json_object_object_get_ex(envelope->payload, "password", &passwordJson) ||
            !json_object_object_get_ex(envelope->payload, "name", &nameJson) ||
            !json_object_object_get_ex(envelope->payload, "avatar", &avatarJson)) {
            sendAccountError(app, connId.slot, envelope->id, "BAD_REQUEST",
                             "Missing required fields");
            return OK;
        }

        const char *email = json_object_get_string(emailJson);
        const char *password = json_object_get_string(passwordJson);
        const char *name = json_object_get_string(nameJson);
        const char *avatar = json_object_get_string(avatarJson);

        if (!accounts_validate_email(email) ||
            !accounts_validate_password(password) ||
            !accounts_validate_name(name) ||
            !accounts_validate_avatar(avatar)) {
            sendAccountError(app, connId.slot, envelope->id, "BAD_REQUEST",
                             "Invalid registration input");
            return OK;
        }

        AuthJob job;
        memset(&job, 0, sizeof(job));
        job.kind = AUTH_REGISTER;
        job.conn_id = connId;
        job.req_id = envelope->id;
        strncpy(job.email, email, sizeof(job.email) - 1);
        strncpy(job.password, password, sizeof(job.password) - 1);
        strncpy(job.name, name, sizeof(job.name) - 1);
        strncpy(job.avatar, avatar, sizeof(job.avatar) - 1);

        Status submitted = auth_submit(app->auth_worker, &job);
        if (submitted != OK) {
            sendAccountError(app, connId.slot, envelope->id, "BUSY", "Server is busy");
        }
        return OK;
    }

    if (strcmp(envelope->type, "LOGIN") == 0) {
        if (!accounts_rate_limit_check(app, ip)) {
            sendAccountError(app, connId.slot, envelope->id, "RATE_LIMITED", "Rate limit exceeded");
            return OK;
        }

        struct json_object *emailJson = NULL, *passwordJson = NULL;
        if (!envelope->payload ||
            !json_object_object_get_ex(envelope->payload, "email", &emailJson) ||
            !json_object_object_get_ex(envelope->payload, "password", &passwordJson)) {
            sendAccountError(app, connId.slot, envelope->id, "BAD_REQUEST",
                             "Missing email or password");
            return OK;
        }

        const char *email = json_object_get_string(emailJson);
        const char *password = json_object_get_string(passwordJson);

        if (!accounts_validate_email(email) || !accounts_validate_password(password)) {
            sendAccountError(app, connId.slot, envelope->id, "BAD_REQUEST",
                             "Invalid login input format");
            return OK;
        }

        AuthJob job;
        memset(&job, 0, sizeof(job));
        job.kind = AUTH_LOGIN;
        job.conn_id = connId;
        job.req_id = envelope->id;
        strncpy(job.email, email, sizeof(job.email) - 1);
        strncpy(job.password, password, sizeof(job.password) - 1);

        Account account;
        Status status = store_account_get(app->store, email, &account);
        if (status == OK) {
            job.user_exists = true;
            strncpy(job.hash, account.hash, sizeof(job.hash) - 1);
            job.account = account;
        } else {
            job.user_exists = false;
        }

        Status submitted = auth_submit(app->auth_worker, &job);
        if (submitted != OK) {
            sendAccountError(app, connId.slot, envelope->id, "BUSY", "Server is busy");
        }
        return OK;
    }

    if (strcmp(envelope->type, "RESUME") == 0) {
        struct json_object *tokenJson = NULL;
        if (!envelope->payload || !json_object_object_get_ex(envelope->payload, "token",
                                                             &tokenJson)) {
            sendAccountError(app, connId.slot, envelope->id, "BAD_REQUEST", "Missing token");
            return OK;
        }

        const char *token = json_object_get_string(tokenJson);
        /* Look the token up first: an invalid one must not cost the socket
         * the account it already carries. */
        const SessionEntry *target = findLiveToken(app->sessions, token);
        if (target) releaseOtherAccount(app, connId, target->user_id);
        Status status = sessions_resume(app, connId, token);
        if (status == OK) {
            UserId userId = 0;
            sessions_user(app, connId, &userId);

            Account account;
            memset(&account, 0, sizeof(account));
            store_account_get_by_id(app->store, userId, &account);

            /* The client already holds the token it just sent; echoing it adds
             * nothing and only widens where the secret appears on the wire. */
            struct json_object *payload = json_object_new_object();

            struct json_object *me = json_object_new_object();
            char userIdText[32], tagText[32];
            snprintf(userIdText, sizeof(userIdText), "%llu", (unsigned long long)userId);
            accounts_format_tag(userId, tagText);
            json_object_object_add(me, "userId", json_object_new_string(userIdText));
            json_object_object_add(me, "tag", json_object_new_string(tagText));
            json_object_object_add(me, "name", json_object_new_string(account.name));
            json_object_object_add(me, "avatar", json_object_new_string(account.avatar));
            json_object_object_add(me, "elo", json_object_new_int(account.elo));

            json_object_object_add(payload, "me", me);

            Envelope response = {1, envelope->id, "OK", payload};
            conn_queue(&slot->conn, &response);
            json_object_put(payload);
            conn_flush(&slot->conn);

            /* After OK so the client sees the session before its GAME_STATE.
             * NOT_FOUND just means the user is not in a match. */
            (void)matchesConnected(app->matches, userId, true, app_now_ms());
            /* Like LOGIN: the session moved to a socket that has seen no
             * room, party or queue snapshot yet. */
            rooms_sync_user(app, userId);
            queue_sync_user(app, userId);
            social_presence(app, userId);
        } else {
            sendAccountError(app, connId.slot, envelope->id, "UNAUTHENTICATED",
                             "Invalid or expired token");
        }
        return OK;
    }

    if (strcmp(envelope->type, "LOGOUT") == 0) {
        UserId userId = 0;
        /* Spec §5: outside a match the user leaves queue, invites, party and
         * waiting room at once. This runs while the session still maps
         * conn -> user, so the leaver also gets the closing snapshots and
         * friends see the account offline. */
        if (sessions_user(app, connId, &userId) == OK && userId != 0) {
            app_user_leave(app, connId, userId);
        }
        sessions_logout(app, connId);
        struct json_object *payload = json_object_new_object();
        Envelope response = {1, envelope->id, "OK", payload};
        conn_queue(&slot->conn, &response);
        json_object_put(payload);
        conn_flush(&slot->conn);
        return OK;
    }

    return BAD_REQUEST;
}

void auth_drain(App *app) {
    if (!app || app->auth_pipe_read < 0) return;

    AuthResult result;
    while (1) {
        /* The worker writes whole AuthResult records, each smaller than
         * PIPE_BUF, so a pipe read never returns half of one. */
        ssize_t bytesRead = read(app->auth_pipe_read, &result, sizeof(result));
        if (bytesRead < 0) {
            if (errno == EINTR) continue;
            /* Pipe is non-blocking: no more finished jobs for now. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
        /* 0 means the write end closed (worker gone); stop either way. */
        if (bytesRead < (ssize_t)sizeof(result)) break;

        /* Verify connection slot and generation */
        uint32_t slotIndex = result.conn_id.slot;
        if (slotIndex >= APP_MAX_CONNS) continue;

        AppConnSlot *slot = &app->slots[slotIndex];
        if (!slot->active || slot->generation != result.conn_id.generation) {
            /* Client disconnected or slot reassigned. Safely drop auth result! */
            continue;
        }

        if (result.kind == AUTH_REGISTER) {
            if (result.status == OK) {
                AccountCreate accountCreate;
                memset(&accountCreate, 0, sizeof(accountCreate));
                strncpy(accountCreate.email, result.account.email, sizeof(accountCreate.email) - 1);
                strncpy(accountCreate.hash, result.account.hash, sizeof(accountCreate.hash) - 1);
                strncpy(accountCreate.name, result.account.name, sizeof(accountCreate.name) - 1);
                strncpy(accountCreate.avatar, result.account.avatar,
                        sizeof(accountCreate.avatar) - 1);

                UserId newUserId = 0;
                Status status = store_account_create(app->store, &accountCreate, &newUserId);
                if (status == OK) {
                    struct json_object *payload = json_object_new_object();
                    char userIdText[32];
                    snprintf(userIdText, sizeof(userIdText), "%llu", (unsigned long long)newUserId);
                    json_object_object_add(payload, "userId", json_object_new_string(userIdText));

                    Envelope response = {1, result.req_id, "OK", payload};
                    conn_queue(&slot->conn, &response);
                    json_object_put(payload);
                    conn_flush(&slot->conn);
                } else if (status == BAD_REQUEST) {
                    sendAccountError(app, slotIndex, result.req_id, "BAD_REQUEST",
                                     "Email already registered");
                } else {
                    sendAccountError(app, slotIndex, result.req_id, "STORAGE_ERROR",
                                     "Database storage failure");
                }
            } else {
                sendAccountError(app, slotIndex, result.req_id, "STORAGE_ERROR",
                                 "Password hashing failed");
            }
        } else if (result.kind == AUTH_LOGIN) {
            if (result.status == OK) {
                releaseOtherAccount(app, result.conn_id, result.user_id);
                /* Disconnect any existing connection for this user */
                ConnId previousConn;
                if (sessions_find_conn(app, result.user_id, &previousConn) == OK) {
                    bool otherSocket = previousConn.slot != result.conn_id.slot;
                    if (otherSocket && previousConn.slot < APP_MAX_CONNS) {
                        /* Detach the session first so app_close_slot sees an
                         * anonymous socket: the user is moving, not leaving, so
                         * no offline presence (which aborts a match with no
                         * other connected human) and no room/party exit. */
                        sessions_disconnect(app, previousConn);
                        app_close_slot(app, previousConn.slot);
                    }
                }

                /* Bind session to current connection */
                sessions_bind(app, result.conn_id, result.user_id, result.token);

                struct json_object *payload = json_object_new_object();
                json_object_object_add(payload, "token", json_object_new_string(result.token));

                struct json_object *me = json_object_new_object();
                char userIdText[32], tagText[32];
                snprintf(userIdText, sizeof(userIdText), "%llu",
                         (unsigned long long)result.user_id);
                accounts_format_tag(result.user_id, tagText);
                json_object_object_add(me, "userId", json_object_new_string(userIdText));
                json_object_object_add(me, "tag", json_object_new_string(tagText));
                json_object_object_add(me, "name", json_object_new_string(result.account.name));
                json_object_object_add(me, "avatar", json_object_new_string(result.account.avatar));
                json_object_object_add(me, "elo", json_object_new_int(result.account.elo));

                json_object_object_add(payload, "me", me);

                Envelope response = {1, result.req_id, "OK", payload};
                conn_queue(&slot->conn, &response);
                json_object_put(payload);
                conn_flush(&slot->conn);

                /* A re-login reclaims the seat. If the seat never went offline
                 * the coordinator still sends this connection a fresh view.
                 * NOT_FOUND just means the user is not in a match. */
                (void)matchesConnected(app->matches, result.user_id, true, app_now_ms());
                /* A re-login keeps room and party membership, but this socket has
                 * never seen their snapshots. */
                rooms_sync_user(app, result.user_id);
                queue_sync_user(app, result.user_id);
                /* Also hands this socket its first FRIEND_STATE. */
                social_presence(app, result.user_id);
            } else {
                sendAccountError(app, slotIndex, result.req_id, "UNAUTHENTICATED",
                                 "Invalid email or password");
            }
        }
    }
}
