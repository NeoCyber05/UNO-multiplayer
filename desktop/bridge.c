#define _POSIX_C_SOURCE 200809L
#include "bridge.h"
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static char g_assets_dir[512] = "./client/dist";
static BridgeDoneCallback g_done_cb = NULL;
static void *g_done_ctx = NULL;
static bool g_scheme_registered = false;

void bridge_set_assets_dir(const char *assets_dir)
{
    if (assets_dir && *assets_dir) {
        strncpy(g_assets_dir, assets_dir, sizeof(g_assets_dir) - 1);
        g_assets_dir[sizeof(g_assets_dir) - 1] = '\0';
    }
}

const char *bridge_get_assets_dir(void)
{
    return g_assets_dir;
}

void bridge_set_done_callback(BridgeDoneCallback cb, void *ctx)
{
    g_done_cb = cb;
    g_done_ctx = ctx;
}

static bool is_whitelisted_type(const char *type)
{
    if (!type || !*type) return false;
    static const char *const whitelist[] = {
        "HELLO",
        "PING",
        "SPIKE_DONE",
        "AUTH_REGISTER",
        "AUTH_LOGIN",
        "AUTH_GUEST",
        "AUTH_LOGOUT",
        "PROFILE_GET",
        "PROFILE_UPDATE",
        "ROOM_CREATE",
        "ROOM_JOIN",
        "ROOM_LEAVE",
        "ROOM_BOT",
        "ROOM_KICK",
        "ROOM_START",
        "INVITE_SEND",
        "INVITE_RESPOND",
        "PARTY_INVITE",
        "PARTY_LEAVE",
        "GAME_ACTION",
        "MATCH_CONFIRM",
        "REMATCH_REQUEST",
        "MATCH_SURRENDER",
        "CHAT_SEND",
        NULL
    };
    for (int i = 0; whitelist[i] != NULL; i++) {
        if (strcmp(type, whitelist[i]) == 0) return true;
    }
    return false;
}

Status bridge_dispatch(Client *client, const char *json_str, size_t len)
{
    if (!json_str || len == 0) return BAD_REQUEST;

    struct json_tokener *tok = json_tokener_new();
    if (!tok) return BUSY;

    struct json_object *root = json_tokener_parse_ex(tok, json_str, (int)len);
    enum json_tokener_error jerr = json_tokener_get_error(tok);
    json_tokener_free(tok);

    if (jerr != json_tokener_success || !root) {
        if (root) json_object_put(root);
        return BAD_REQUEST;
    }

    if (!json_object_is_type(root, json_type_object)) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    struct json_object *v_obj = NULL;
    struct json_object *id_obj = NULL;
    struct json_object *type_obj = NULL;
    struct json_object *payload_obj = NULL;

    if (!json_object_object_get_ex(root, "v", &v_obj) ||
        !json_object_object_get_ex(root, "id", &id_obj) ||
        !json_object_object_get_ex(root, "type", &type_obj) ||
        !json_object_object_get_ex(root, "payload", &payload_obj)) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    if (!json_object_is_type(v_obj, json_type_int) ||
        !json_object_is_type(id_obj, json_type_int) ||
        !json_object_is_type(type_obj, json_type_string) ||
        !json_object_is_type(payload_obj, json_type_object)) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    int v = json_object_get_int(v_obj);
    if (v != 1) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    int32_t id = (int32_t)json_object_get_int(id_obj);
    const char *type = json_object_get_string(type_obj);
    if (!type || strlen(type) == 0) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    if (!is_whitelisted_type(type)) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    if (strcmp(type, "SPIKE_DONE") == 0) {
        if (g_done_cb) {
            g_done_cb(g_done_ctx);
        } else if (gtk_main_level() > 0) {
            gtk_main_quit();
        }
        json_object_put(root);
        return OK;
    }

    Envelope env;
    env.v = v;
    env.id = id;
    env.type = type;
    env.payload = payload_obj;

    Status s = OK;
    if (client != NULL) {
        s = client_send(client, &env);
    }

    json_object_put(root);
    return s;
}

void bridge_emit(WebKitWebView *web_view, const Envelope *e)
{
    if (!web_view || !e) return;

    struct json_object *root = json_object_new_object();
    if (!root) return;

    json_object_object_add(root, "v", json_object_new_int(e->v));
    json_object_object_add(root, "id", json_object_new_int(e->id));
    json_object_object_add(root, "type", json_object_new_string(e->type ? e->type : ""));

    if (e->payload) {
        json_object_get(e->payload);
        json_object_object_add(root, "payload", e->payload);
    } else {
        json_object_object_add(root, "payload", json_object_new_object());
    }

    const char *json_str = json_object_to_json_string(root);
    gchar *script = g_strdup_printf("window.dispatchEvent(new CustomEvent('uno:message', {detail: %s}));", json_str);
    json_object_put(root);

    webkit_web_view_evaluate_javascript(web_view, script, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(script);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static Status url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        if (src[i] == '%') {
            if (src[i + 1] == '\0' || src[i + 2] == '\0') {
                return BAD_REQUEST;
            }
            int h1 = hex_val(src[i + 1]);
            int h2 = hex_val(src[i + 2]);
            if (h1 < 0 || h2 < 0) {
                return BAD_REQUEST;
            }
            char ch = (char)((h1 << 4) | h2);
            if (ch == '\0') {
                /* Reject null byte injection */
                return BAD_REQUEST;
            }
            if (j + 1 >= dst_len) return BAD_REQUEST;
            dst[j++] = ch;
            i += 2;
        } else {
            if (j + 1 >= dst_len) return BAD_REQUEST;
            dst[j++] = src[i];
        }
    }
    if (j >= dst_len) return BAD_REQUEST;
    dst[j] = '\0';
    return OK;
}

Status bridge_resolve_path(const char *assets_dir, const char *uri, char *out_path, size_t max_len)
{
    if (!assets_dir || !uri || !out_path || max_len == 0) return BAD_REQUEST;

    const char *p = uri;
    if (strncmp(p, "uno://app/", 10) == 0) {
        p += 10;
    } else if (strncmp(p, "uno://app", 9) == 0) {
        p += 9;
        if (*p == '/') p++;
    } else if (strncmp(p, "uno:", 4) == 0) {
        p += 4;
        while (*p == '/') p++;
    } else if (*p == '/') {
        while (*p == '/') p++;
    }

    char decoded[512];
    if (url_decode(p, decoded, sizeof(decoded)) != OK) {
        return BAD_REQUEST;
    }

    /* Reject backslashes or absolute paths */
    if (strchr(decoded, '\\') != NULL || decoded[0] == '/') {
        return BAD_REQUEST;
    }

    /* Check for path traversal components */
    if (strstr(decoded, "..") != NULL) {
        return BAD_REQUEST;
    }

    const char *rel_path = decoded;
    while (*rel_path == '/') rel_path++;

    if (*rel_path == '\0') {
        rel_path = "index.html";
    }

    int n = snprintf(out_path, max_len, "%s/%s", assets_dir, rel_path);
    if (n < 0 || (size_t)n >= max_len) {
        return BAD_REQUEST;
    }

    return OK;
}

const char *bridge_mime_type(const char *path)
{
    if (!path) return "application/octet-stream";
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";

    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html";
    if (strcasecmp(dot, ".js") == 0 || strcasecmp(dot, ".mjs") == 0) return "application/javascript";
    if (strcasecmp(dot, ".css") == 0) return "text/css";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(dot, ".wasm") == 0) return "application/wasm";

    return "application/octet-stream";
}

static void on_uri_scheme_request(WebKitURISchemeRequest *request, gpointer user_data)
{
    const char *assets_dir = (const char *)user_data;
    if (!assets_dir) assets_dir = bridge_get_assets_dir();

    const char *uri = webkit_uri_scheme_request_get_uri(request);
    char file_path[512];
    Status s = bridge_resolve_path(assets_dir, uri, file_path, sizeof(file_path));
    if (s != OK) {
        GError *err = g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_NOENT, "Invalid URI or access denied");
        webkit_uri_scheme_request_finish_error(request, err);
        g_error_free(err);
        return;
    }

    /* Fallback: if index.html requested but does not exist, check if spike_test.html exists */
    if (access(file_path, R_OK) != 0) {
        const char *base = strrchr(file_path, '/');
        if (base && strcmp(base + 1, "index.html") == 0) {
            char alt_path[512];
            snprintf(alt_path, sizeof(alt_path), "%s/spike_test.html", assets_dir);
            if (access(alt_path, R_OK) == 0) {
                strncpy(file_path, alt_path, sizeof(file_path) - 1);
                file_path[sizeof(file_path) - 1] = '\0';
            }
        }
    }

    GFile *file = g_file_new_for_path(file_path);
    GError *err = NULL;
    GFileInputStream *stream = g_file_read(file, NULL, &err);
    if (!stream) {
        webkit_uri_scheme_request_finish_error(request, err ? err : g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_NOENT, "File not found"));
        if (err) g_error_free(err);
        g_object_unref(file);
        return;
    }

    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    gint64 size = info ? g_file_info_get_size(info) : -1;
    if (info) g_object_unref(info);

    const char *mime_type = bridge_mime_type(file_path);
    webkit_uri_scheme_request_finish(request, G_INPUT_STREAM(stream), size, mime_type);

    g_object_unref(stream);
    g_object_unref(file);
}

void bridge_register_uri_scheme(WebKitWebContext *context, const char *assets_dir)
{
    (void)assets_dir;
    if (g_scheme_registered) return;
    if (!context) context = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(context, "uno",
                                           on_uri_scheme_request,
                                           (gpointer)bridge_get_assets_dir(), NULL);
    g_scheme_registered = true;
}

typedef struct {
    WebKitWebView *web_view;
    Client *client;
} BridgeState;

static void on_script_message_uno(WebKitUserContentManager *manager,
                                   WebKitJavascriptResult *result,
                                   gpointer user_data)
{
    (void)manager;
    BridgeState *state = (BridgeState *)user_data;
    JSCValue *val = webkit_javascript_result_get_js_value(result);
    if (!val) return;

    char *json_str = jsc_value_to_string(val);
    if (!json_str) return;

    bridge_dispatch(state ? state->client : NULL, json_str, strlen(json_str));
    g_free(json_str);
}

Status bridge_init(WebKitWebView *web_view, Client *client)
{
    if (!web_view) return BAD_REQUEST;

    WebKitWebContext *context = webkit_web_view_get_context(web_view);
    bridge_register_uri_scheme(context, bridge_get_assets_dir());

    WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager(web_view);
    BridgeState *state = g_new0(BridgeState, 1);
    state->web_view = web_view;
    state->client = client;

    g_object_set_data_full(G_OBJECT(web_view), "bridge_state", state, g_free);

    g_signal_connect(manager, "script-message-received::uno",
                     G_CALLBACK(on_script_message_uno), state);
    webkit_user_content_manager_register_script_message_handler(manager, "uno");

    return OK;
}
