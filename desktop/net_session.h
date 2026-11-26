#ifndef UNO_NET_SESSION_H
#define UNO_NET_SESSION_H

#include "shared/contracts.h"
#include "shared/protocol.h"
#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Native session state for the desktop host, kept free of GTK so it can be
 * unit tested. It owns the session token (never exposed to JS), translates JS
 * request ids into per-connection wire ids, and computes reconnect backoff.
 */

enum {
    NS_MAX_PENDING = 256 /* in-flight requests awaiting a response */
};

typedef enum { NS_DISCONNECTED, NS_CONNECTING, NS_RESUMING, NS_CONNECTED } NsState;
typedef enum { NS_FORWARD, NS_DROP } NsVerdict;
typedef struct NetSession NetSession;

/**
 * @brief Allocate a session in NS_DISCONNECTED with no token.
 * @return The new session (release with nsDestroy), or NULL on out of memory.
 */
NetSession *nsCreate(void);

/**
 * @brief Wipe the token, then free the session.
 * @param[in] session Session to free; NULL is ignored.
 */
void nsDestroy(NetSession *session);

/**
 * @brief Record that a new TCP connection is up.
 *
 * Wire ids restart at 1 because the server enforces strictly increasing ids
 * per connection; pending requests of the old connection are forgotten.
 * State becomes NS_RESUMING when a token is
 * held (the host must then send RESUME via nsInternal), otherwise
 * NS_CONNECTED. The reconnect backoff is reset only on NS_CONNECTED; while
 * resuming it is kept until the RESUME answer (see nsIncoming).
 * @param[in,out] session Session; NULL is ignored.
 */
void nsOnConnected(NetSession *session);

/**
 * @brief Record that the TCP connection dropped.
 *
 * Pending requests are forgotten (their responses can never arrive); the
 * token is kept so the next connection can RESUME.
 * @param[in,out] session Session; NULL is ignored.
 */
void nsOnDisconnected(NetSession *session);

/**
 * @brief Current connection state.
 * @param[in] session Session.
 * @return The state; NS_DISCONNECTED when @p session is NULL.
 */
NsState nsState(const NetSession *session);

/**
 * @brief Allocate a wire id for a request coming from JS.
 * @param[in,out] session Session.
 * @param[in] jsId Id the JS side chose (> 0); echoed back by nsIncoming.
 * @param[in] type Request type; LOGIN/LOGOUT responses update the token.
 * @param[out] wireId Id to put on the wire envelope.
 * @return OK when the request was registered;
 *         BAD_REQUEST when an argument is NULL, @p jsId <= 0 (0 marks
 *         pushes) or @p type is "RESUME"
 *         (only the native side may resume, since JS never sees the token);
 *         BUSY when the state is not NS_CONNECTED, all NS_MAX_PENDING slots
 *         are in use, or the connection ran out of wire ids.
 */
Status nsOutgoing(NetSession *session, int32_t jsId, const char *type, int32_t *wireId);

/**
 * @brief Allocate a wire id for a request the native side sends itself.
 *
 * The response to this id is consumed natively and never forwarded to JS.
 * @param[in,out] session Session.
 * @param[in] type "PING" or "RESUME"; a RESUME response completes resuming.
 * @return The wire id (> 0), or 0 when nothing may be sent: NULL argument,
 *         NS_DISCONNECTED/NS_CONNECTING, table full, or ids exhausted.
 */
int32_t nsInternal(NetSession *session, const char *type);

/**
 * @brief Classify an envelope received from the server.
 *
 * Pushes (id 0) are forwarded with jsId 0. A response to a JS request is
 * forwarded with the original JS id; LOGIN OK stores payload.token and
 * LOGOUT OK clears it. A response to a native request or an unknown id is
 * dropped. A RESUME OK moves to NS_CONNECTED; a RESUME failure also moves to
 * NS_CONNECTED but clears the token, because the server no longer knows it.
 * Either answer resets the reconnect backoff.
 * @param[in,out] session Session.
 * @param[in] envelope Received envelope; not retained.
 * @param[out] jsId JS id to emit with (0 for pushes and dropped frames).
 * @param[out] outConnState Set to a new json_object owned by the caller
 *             (release with json_object_put) when the host must emit a
 *             connection-state event: {status:"connected", me} after RESUME
 *             OK, {status:"connected", sessionExpired:true} after a RESUME
 *             failure. Set to NULL when there is nothing to emit. May be NULL.
 * @return NS_FORWARD when the host must emit @p envelope to JS with @p jsId;
 *         NS_DROP when it must not.
 */
NsVerdict nsIncoming(NetSession *session, const Envelope *envelope, int32_t *jsId,
                      json_object **outConnState);

/**
 * @brief Whether a session token is held.
 * @param[in] session Session.
 * @return true when a token from LOGIN is held; false otherwise or for NULL.
 */
bool nsHasToken(const NetSession *session);

/**
 * @brief The held token, for building the native RESUME request only.
 * @param[in] session Session.
 * @return The token string owned by the session; its storage is wiped (not
 *         freed) by the next LOGIN/LOGOUT/RESUME failure and freed by
 *         nsDestroy. NULL when no token is held.
 */
const char *nsToken(const NetSession *session);

/**
 * @brief Delay before the next reconnect attempt.
 *
 * Doubles on each call so a down server is not hammered, capped so the user
 * is not left waiting long once it is back. It resets when the session
 * reaches NS_CONNECTED (connect without a token, or a RESUME answer).
 * @param[in,out] session Session.
 * @return 1000, 2000, 4000, 8000, 8000, ... milliseconds; 0 when @p session is NULL.
 */
uint32_t nsNextBackoffMs(NetSession *session);

#endif
