#ifndef UNO_BRIDGE_H
#define UNO_BRIDGE_H

#include "desktop/tcp_client.h"
#include "shared/contracts.h"
#include "shared/protocol.h"
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stddef.h>

typedef void (*BridgeDoneCallback)(void *ctx);

void bridge_set_assets_dir(const char *assets_dir);
const char *bridge_get_assets_dir(void);
void bridge_set_done_callback(BridgeDoneCallback cb, void *ctx);

Status bridge_init(WebKitWebView *web_view, Client *client);
Status bridge_dispatch(Client *client, const char *json_str, size_t len);
void bridge_emit(WebKitWebView *web_view, const Envelope *e);

Status bridge_resolve_path(const char *assets_dir, const char *uri, char *out_path, size_t max_len);
const char *bridge_mime_type(const char *path);
void bridge_register_uri_scheme(WebKitWebContext *context, const char *assets_dir);

#endif
