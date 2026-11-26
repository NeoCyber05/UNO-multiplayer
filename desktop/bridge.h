#ifndef UNO_BRIDGE_H
#define UNO_BRIDGE_H

#include "desktop/net_session.h"
#include "desktop/tcp_client.h"
#include "shared/contracts.h"
#include "shared/protocol.h"
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stddef.h>

typedef void (*BridgeDoneCallback)(void *context);
typedef void (*BridgeDisconnectCallback)(void *context);

/** @brief Directory served under uno://app/; ignored when NULL or empty (copied). */
void bridge_set_assets_dir(const char *assetsDir);

/** @brief Current asset directory; "./client/dist" until set. */
const char *bridge_get_assets_dir(void);

/**
 * @brief Set who handles a SPIKE_DONE message (desktop smoke test).
 * @param[in] callback Callback, or NULL to quit the GTK main loop instead.
 * @param[in] context Passed to @p callback unchanged.
 */
void bridge_set_done_callback(BridgeDoneCallback callback, void *context);

/**
 * @brief Set who is told when sending a JS request broke the connection.
 *
 * Called from bridge_dispatch after client_send fails and the client can no
 * longer be used (peer gone, write error, output buffer overflow), so the
 * host can tear down and reconnect at once. The callback may close and free
 * the client. A frame that merely fails to encode does not trigger it.
 * @param[in] callback Callback, or NULL to disable.
 * @param[in] context Passed to @p callback unchanged.
 */
void bridge_set_disconnect_callback(BridgeDisconnectCallback callback, void *context);

/**
 * @brief Register the uno:// scheme and the "uno" script message handler.
 *
 * JS requests are mapped through @p session and sent on whatever client
 * @p clientSlot points at when each message arrives, so the host can swap
 * clients on reconnect without re-registering. A request that cannot be sent
 * because the session is not connected is answered at once with
 * {v:1,id:<js id>,type:"ERROR",payload:{code:"DISCONNECTED",message}};
 * requests are never queued, so nothing is replayed after a reconnect. Any
 * other rejection of a request with a positive id is answered the same way
 * with the Status name as code (e.g. "BAD_REQUEST") and a fixed message.
 * @param[in] webView View whose JS talks to native; must outlive the bridge.
 * @param[in] session Session used to map ids; must outlive @p webView.
 * @param[in] clientSlot Address of the host's current client variable
 *            (the variable may be NULL while disconnected).
 * @return OK when registered; BAD_REQUEST when an argument is NULL.
 */
Status bridge_init(WebKitWebView *webView, NetSession *session, Client **clientSlot);

/**
 * @brief Validate one JS message and send it to the server.
 *
 * The JS id is replaced by a per-connection wire id from ns_outgoing; the
 * response is mapped back by ns_incoming. SPIKE_DONE (smoke test) is handled
 * locally and never sent. RESUME is not accepted from JS.
 * @param[in,out] session Session that allocates the wire id.
 * @param[in] client Connection to send on; NULL skips sending (tests).
 * @param[in] jsonText Message text from JS, not necessarily NUL-terminated.
 * @param[in] length Length of @p jsonText in bytes.
 * @param[out] outJsId Set to the JS id once it has been parsed, else 0, so
 *             the caller can answer the request with an ERROR. May be NULL.
 * @return OK when sent (or handled locally);
 *         BAD_REQUEST when the message is malformed, its type is not allowed
 *         from JS, its id is not positive, or @p session is NULL;
 *         BUSY when the session is not connected (or resuming) or has no free
 *         request slot; any other Status is a send failure from client_send
 *         (if it broke the link, the disconnect callback has already run and
 *         @p client may have been freed).
 */
Status bridge_dispatch(NetSession *session, Client *client, const char *jsonText, size_t length,
                       int32_t *outJsId);

/** @brief Deliver @p envelope to JS as a "uno:message" event; see bridge_format_emit_script. */
void bridge_emit(WebKitWebView *webView, const Envelope *envelope);

/**
 * @brief Build the JS that dispatches @p envelope as a "uno:message" event.
 *
 * A "token" key is removed from the payload first: the session token never
 * reaches JS, not even inside a LOGIN reply.
 *
 * @return A g_malloc'd script the caller frees with g_free, or NULL.
 */
char *bridge_format_emit_script(const Envelope *envelope);

/**
 * @brief Map a uno:// URI to a file below @p assetsDir.
 *
 * The path is percent-decoded first, then rejected if it contains "..", a
 * backslash, a NUL byte or starts with '/', so a page can never read files
 * outside the asset directory. An empty path serves index.html.
 *
 * @param[out] outPath Buffer of @p maxLength bytes for "<assetsDir>/<path>".
 * @return OK, or BAD_REQUEST for a rejected URI or a path that does not fit.
 */
Status bridge_resolve_path(const char *assetsDir, const char *uri, char *outPath, size_t maxLength);

/** @brief Content type for a file name by extension; application/octet-stream if unknown. */
const char *bridge_mime_type(const char *path);

/**
 * @brief Serve uno:// from the asset directory; safe to call more than once.
 * @param[in] context Web context, or NULL for the default one.
 * @param[in] assetsDir Unused; the directory is read through bridge_get_assets_dir.
 */
void bridge_register_uri_scheme(WebKitWebContext *context, const char *assetsDir);

#endif
