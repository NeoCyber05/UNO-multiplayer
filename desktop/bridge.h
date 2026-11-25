#ifndef UNO_BRIDGE_H
#define UNO_BRIDGE_H

#include "desktop/net_session.h"
#include "desktop/tcp_client.h"
#include "shared/contracts.h"
#include "shared/protocol.h"
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stddef.h>

typedef void (*BridgeDoneCallback)(void *ctx);
typedef void (*BridgeDisconnectCallback)(void *ctx);

void bridge_set_assets_dir(const char *assets_dir);
const char *bridge_get_assets_dir(void);
void bridge_set_done_callback(BridgeDoneCallback cb, void *ctx);

/**
 * @brief Set who is told when sending a JS request broke the connection.
 *
 * Called from bridge_dispatch after client_send fails and the client can no
 * longer be used (peer gone, write error, output buffer overflow), so the
 * host can tear down and reconnect at once. The callback may close and free
 * the client. A frame that merely fails to encode does not trigger it.
 * @param[in] cb Callback, or NULL to disable.
 * @param[in] ctx Passed to @p cb unchanged.
 */
void bridge_set_disconnect_callback(BridgeDisconnectCallback cb, void *ctx);

/**
 * @brief Register the uno:// scheme and the "uno" script message handler.
 *
 * JS requests are mapped through @p session and sent on whatever client
 * @p client_slot points at when each message arrives, so the host can swap
 * clients on reconnect without re-registering. A request that cannot be sent
 * because the session is not connected is answered at once with
 * {v:1,id:<js id>,type:"ERROR",payload:{code:"DISCONNECTED",message}};
 * requests are never queued, so nothing is replayed after a reconnect. Any
 * other rejection of a request with a positive id is answered the same way
 * with the Status name as code (e.g. "BAD_REQUEST") and a fixed message.
 * @param[in] web_view View whose JS talks to native; must outlive the bridge.
 * @param[in] session Session used to map ids; must outlive @p web_view.
 * @param[in] client_slot Address of the host's current client variable
 *            (the variable may be NULL while disconnected).
 * @return OK when registered; BAD_REQUEST when an argument is NULL.
 */
Status bridge_init(WebKitWebView *web_view, NetSession *session, Client **client_slot);

/**
 * @brief Validate one JS message and send it to the server.
 *
 * The JS id is replaced by a per-connection wire id from ns_outgoing; the
 * response is mapped back by ns_incoming. SPIKE_DONE (smoke test) is handled
 * locally and never sent. RESUME is not accepted from JS.
 * @param[in,out] session Session that allocates the wire id.
 * @param[in] client Connection to send on; NULL skips sending (tests).
 * @param[in] json_str Message text from JS, not necessarily NUL-terminated.
 * @param[in] len Length of @p json_str in bytes.
 * @param[out] js_id_out Set to the JS id once it has been parsed, else 0, so
 *             the caller can answer the request with an ERROR. May be NULL.
 * @return OK when sent (or handled locally);
 *         BAD_REQUEST when the message is malformed, its type is not allowed
 *         from JS, its id is not positive, or @p session is NULL;
 *         BUSY when the session is not connected (or resuming) or has no free
 *         request slot; any other Status is a send failure from client_send
 *         (if it broke the link, the disconnect callback has already run and
 *         @p client may have been freed).
 */
Status bridge_dispatch(NetSession *session, Client *client, const char *json_str, size_t len,
                       int32_t *js_id_out);

void bridge_emit(WebKitWebView *web_view, const Envelope *e);
char *bridge_format_emit_script(const Envelope *e);

Status bridge_resolve_path(const char *assets_dir, const char *uri, char *out_path, size_t max_len);
const char *bridge_mime_type(const char *path);
void bridge_register_uri_scheme(WebKitWebContext *context, const char *assets_dir);

#endif
