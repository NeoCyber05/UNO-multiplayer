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
 * @return The new session (release with ns_destroy), or NULL on out of memory.
 */
NetSession *ns_create(void);

/**
 * @brief Free a session and its token.
 * @param[in] s Session to free; NULL is ignored.
 */
void ns_destroy(NetSession *s);

/**
 * @brief Record that a new TCP connection is up.
 *
 * Wire ids restart at 1 because the server enforces strictly increasing ids
 * per connection; pending requests of the old connection are forgotten and
 * the reconnect backoff is reset. State becomes NS_RESUMING when a token is
 * held (the host must then send RESUME via ns_internal), otherwise
 * NS_CONNECTED.
 * @param[in,out] s Session; NULL is ignored.
 */
void ns_on_connected(NetSession *s);

/**
 * @brief Record that the TCP connection dropped.
 *
 * Pending requests are forgotten (their responses can never arrive); the
 * token is kept so the next connection can RESUME.
 * @param[in,out] s Session; NULL is ignored.
 */
void ns_on_disconnected(NetSession *s);

/**
 * @brief Current connection state.
 * @param[in] s Session.
 * @return The state; NS_DISCONNECTED when @p s is NULL.
 */
NsState ns_state(const NetSession *s);

/**
 * @brief Allocate a wire id for a request coming from JS.
 * @param[in,out] s Session.
 * @param[in] js_id Id the JS side chose; echoed back by ns_incoming.
 * @param[in] type Request type; LOGIN/LOGOUT responses update the token.
 * @param[out] wire_id Id to put on the wire envelope.
 * @return OK when the request was registered;
 *         BAD_REQUEST when an argument is NULL or @p type is "RESUME"
 *         (only the native side may resume, since JS never sees the token);
 *         BUSY when the state is not NS_CONNECTED, all NS_MAX_PENDING slots
 *         are in use, or the connection ran out of wire ids.
 */
Status ns_outgoing(NetSession *s, int32_t js_id, const char *type, int32_t *wire_id);

/**
 * @brief Allocate a wire id for a request the native side sends itself.
 *
 * The response to this id is consumed natively and never forwarded to JS.
 * @param[in,out] s Session.
 * @param[in] type "PING" or "RESUME"; a RESUME response completes resuming.
 * @return The wire id (> 0), or 0 when nothing may be sent: NULL argument,
 *         NS_DISCONNECTED/NS_CONNECTING, table full, or ids exhausted.
 */
int32_t ns_internal(NetSession *s, const char *type);

/**
 * @brief Classify an envelope received from the server.
 *
 * Pushes (id 0) are forwarded with js_id 0. A response to a JS request is
 * forwarded with the original JS id; LOGIN OK stores payload.token and
 * LOGOUT OK clears it. A response to a native request or an unknown id is
 * dropped. A RESUME OK moves to NS_CONNECTED; a RESUME failure also moves to
 * NS_CONNECTED but clears the token, because the server no longer knows it.
 * @param[in,out] s Session.
 * @param[in] e Received envelope; not retained.
 * @param[out] js_id JS id to emit with (0 for pushes and dropped frames).
 * @param[out] conn_state Set to a new json_object owned by the caller
 *             (release with json_object_put) when the host must emit a
 *             connection-state event: {status:"connected", me} after RESUME
 *             OK, {status:"connected", sessionExpired:true} after a RESUME
 *             failure. Set to NULL when there is nothing to emit. May be NULL.
 * @return NS_FORWARD when the host must emit @p e to JS with @p js_id;
 *         NS_DROP when it must not.
 */
NsVerdict ns_incoming(NetSession *s, const Envelope *e, int32_t *js_id, json_object **conn_state);

/**
 * @brief Whether a session token is held.
 * @param[in] s Session.
 * @return true when a token from LOGIN is held; false otherwise or for NULL.
 */
bool ns_has_token(const NetSession *s);

/**
 * @brief The held token, for building the native RESUME request only.
 * @param[in] s Session.
 * @return The token string owned by the session (invalidated by the next
 *         LOGIN/LOGOUT/RESUME failure or ns_destroy), or NULL when none.
 */
const char *ns_token(const NetSession *s);

/**
 * @brief Delay before the next reconnect attempt.
 *
 * Doubles on each call so a down server is not hammered, capped so the user
 * is not left waiting long once it is back; ns_on_connected resets it.
 * @param[in,out] s Session.
 * @return 1000, 2000, 4000, 8000, 8000, ... milliseconds; 0 when @p s is NULL.
 */
uint32_t ns_next_backoff_ms(NetSession *s);

#endif
