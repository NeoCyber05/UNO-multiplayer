#ifndef UNO_ACCOUNTS_H
#define UNO_ACCOUNTS_H

#include "server/app.h"
#include "shared/contracts.h"
#include "shared/protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Validation helpers: each returns true only for input the spec accepts. */

/** @brief Lowercase ASCII address, 3..254 bytes, exactly one '@' with text on both sides. */
bool accountsValidateEmail(const char *email);
/** @brief 6..128 bytes; content is not restricted because it is only hashed. */
bool accountsValidatePassword(const char *password);
/** @brief Well-formed UTF-8, 1..64 bytes and 1..16 code points, no control characters. */
bool accountsValidateName(const char *name);
/** @brief One of the whitelisted avatar ids (fox, panda, ...). */
bool accountsValidateAvatar(const char *avatar);

/**
 * @brief Fill @p outToken with 32 random bytes from libsodium as 64 hex chars.
 * @param[out] outToken Buffer of 65 bytes, NUL-terminated on return.
 */
void accountsGenerateToken(char outToken[65]);

/**
 * @brief Format the public friend tag "UNO#<userId>".
 * @param[in] userId Account id.
 * @param[out] outTag Buffer of 32 bytes.
 */
void accountsFormatTag(UserId userId, char outTag[32]);

/**
 * @brief Milliseconds from CLOCK_MONOTONIC plus a fixed offset, used for
 *        session age and the auth rate limit. Not comparable with appNowMs.
 */
uint64_t accountsGetNowMs(void);

#define MAX_SESSIONS 256

/* A login session. It outlives its socket (connected = false) so RESUME can
 * move it to a new connection within the 24 h token lifetime. */
typedef struct {
    bool inUse;
    UserId userId;
    char token[65];
    ConnId connId;
    bool connected;
    uint64_t createdAtMs;
} SessionEntry;

struct Sessions {
    SessionEntry entries[MAX_SESSIONS];
};

/**
 * @brief Allocate the session table and the per-IP rate limiter.
 * @return OK, BAD_REQUEST for NULL @p app, or STORAGE_ERROR when out of memory.
 */
Status sessionsInit(App *app);

/** @brief Free the session table and rate limiter; safe to call twice. */
void sessionsCleanup(App *app);

/**
 * @brief Attach a freshly logged-in account to a connection.
 *
 * Reuses the account's existing entry, else a free one, else evicts the
 * oldest disconnected entry.
 *
 * @param[in] connId Connection that completed LOGIN.
 * @param[in] userId Account id.
 * @param[in] token New session token; copied.
 * @return OK, BAD_REQUEST for invalid arguments, or BUSY when every entry is
 *         connected.
 */
Status sessionsBind(App *app, ConnId connId, UserId userId, const char *token);

/**
 * @brief Find the account currently bound to a live connection.
 * @param[out] outUserId Receives the account id on success.
 * @return OK, or UNAUTHENTICATED when the slot is closed, reused (generation
 *         mismatch) or carries no session.
 */
Status sessionsUser(App *app, ConnId connId, UserId *outUserId);

/**
 * @brief Find the connection a logged-in account is using.
 * @param[out] outConn Receives the connection on success.
 * @return OK, or NOT_FOUND when the account has no connected session.
 */
Status sessionsFindConn(App *app, UserId userId, ConnId *outConn);

/**
 * @brief Move a session to a new connection by its token (RESUME).
 * @param[in] connId The new connection.
 * @param[in] token 64-hex-char token from LOGIN; an expired one is forgotten.
 * @return OK, or UNAUTHENTICATED for an unknown/expired token or a dead slot.
 */
Status sessionsResume(App *app, ConnId connId, const char *token);

/** @brief Mark the session on @p connId disconnected but keep it for RESUME. */
void sessionsDisconnect(App *app, ConnId connId);

/** @brief Forget the session on @p connId entirely (LOGOUT or re-login). */
void sessionsLogout(App *app, ConnId connId);

/**
 * @brief Count one auth request from @p ip against its sliding one-minute window.
 * @return true if the request may proceed; always true when the limit is off
 *         (authLimit <= 0) or @p ip is empty.
 */
bool accountsRateLimitCheck(App *app, const char *ip);

/**
 * @brief Handle REGISTER, LOGIN, RESUME and LOGOUT.
 *
 * REGISTER and LOGIN are only validated here and then queued on the auth
 * worker; their replies are sent later by authDrain. RESUME and LOGOUT
 * reply at once. The session token only ever goes back in the LOGIN reply.
 *
 * @param[in] connId Connection that sent @p envelope.
 * @param[in] envelope Request; its payload is borrowed.
 * @return OK once a reply was sent or queued, BAD_REQUEST for another opcode
 *         or a dead connection.
 */
Status accountsHandle(App *app, ConnId connId, const Envelope *envelope);

/**
 * @brief Read every finished auth job from the worker pipe and reply.
 *
 * Results for a connection that closed or was reused meanwhile are dropped.
 * A successful LOGIN closes the account's previous socket, binds the session
 * and resyncs room, queue and presence for the new socket.
 */
void authDrain(App *app);

#endif
