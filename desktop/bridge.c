#define _POSIX_C_SOURCE 200809L
#include "bridge.h"
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static char gAssetsDir[512] = "./client/dist";
static BridgeDoneCallback gDoneCallback = NULL;
static void *gDoneContext = NULL;
static BridgeDisconnectCallback gDisconnectCallback = NULL;
static void *gDisconnectContext = NULL;
static bool gSchemeRegistered = false;

void bridgeSetAssetsDir(const char *assetsDir) {
    if (assetsDir && *assetsDir) {
        strncpy(gAssetsDir, assetsDir, sizeof(gAssetsDir) - 1);
        gAssetsDir[sizeof(gAssetsDir) - 1] = '\0';
    }
}

const char *bridgeGetAssetsDir(void) {
    return gAssetsDir;
}

void bridgeSetDoneCallback(BridgeDoneCallback callback, void *context) {
    gDoneCallback = callback;
    gDoneContext = context;
}

void bridgeSetDisconnectCallback(BridgeDisconnectCallback callback, void *context) {
    gDisconnectCallback = callback;
    gDisconnectContext = context;
}

/* A send can fail because the frame is unusable (connection still fine) or
 * because the socket broke; only the latter makes the client unusable. */
static bool clientIsBroken(const Client *client) {
    return !client->connected || client->conn.fd < 0 || client->conn.closing;
}

static bool isWhitelistedType(const char *type) {
    if (!type || !*type) return false;
    /* RESUME is deliberately absent: only native code holds the token. */
    static const char *const whitelist[] = {
        "HELLO",
        "PING",
        "LOGOUT",
        "REGISTER",
        "LOGIN",
        "PROFILE_GET",
        "PROFILE_UPDATE",
        "LOBBY_GET",
        "FRIEND_REQUEST",
        "FRIEND_REPLY",
        "FRIEND_LIST",
        "LEADERBOARD_GET",
        "ROOM_CREATE",
        "ROOM_JOIN",
        "ROOM_LEAVE",
        "ROOM_BOT",
        "ROOM_KICK",
        "ROOM_MODE",
        "ROOM_START",
        "INVITE_SEND",
        "INVITE_REPLY",
        "PARTY_LEAVE",
        "QUEUE_JOIN",
        "QUEUE_LEAVE",
        "PLAY",
        "DRAW",
        "PASS",
        "SAY_UNO",
        "DRAFT_PICK",
        "MATCH_LEAVE",
        "CHAT",
        "SPIKE_DONE",
        NULL
    };
    for (int i = 0; whitelist[i] != NULL; i++) {
        if (strcmp(type, whitelist[i]) == 0) return true;
    }
    return false;
}

Status bridgeDispatch(NetSession *session, Client *client, const char *jsonText, size_t length,
                       int32_t *outJsId)
{
    if (outJsId) *outJsId = 0;
    if (!jsonText || length == 0) return BAD_REQUEST;

    struct json_tokener *tokener = json_tokener_new();
    if (!tokener) return BUSY;

    struct json_object *root = json_tokener_parse_ex(tokener, jsonText, (int)length);
    enum json_tokener_error parseError = json_tokener_get_error(tokener);
    json_tokener_free(tokener);

    if (parseError != json_tokener_success || !root) {
        if (root) json_object_put(root);
        return BAD_REQUEST;
    }

    if (!json_object_is_type(root, json_type_object)) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    struct json_object *versionJson = NULL;
    struct json_object *idJson = NULL;
    struct json_object *typeJson = NULL;
    struct json_object *payloadJson = NULL;

    if (!json_object_object_get_ex(root, "v", &versionJson) ||
        !json_object_object_get_ex(root, "id", &idJson) ||
        !json_object_object_get_ex(root, "type", &typeJson) ||
        !json_object_object_get_ex(root, "payload", &payloadJson)) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    if (!json_object_is_type(versionJson, json_type_int) ||
        !json_object_is_type(idJson, json_type_int) ||
        !json_object_is_type(typeJson, json_type_string) ||
        !json_object_is_type(payloadJson, json_type_object)) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    int version = json_object_get_int(versionJson);
    if (version != 1) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    int32_t id = (int32_t)json_object_get_int(idJson);
    if (outJsId) *outJsId = id;
    const char *type = json_object_get_string(typeJson);
    if (!type || strlen(type) == 0) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    if (!isWhitelistedType(type)) {
        json_object_put(root);
        return BAD_REQUEST;
    }

    if (strcmp(type, "SPIKE_DONE") == 0) {
        if (gDoneCallback) {
            gDoneCallback(gDoneContext);
        } else if (gtk_main_level() > 0) {
            gtk_main_quit();
        }
        json_object_put(root);
        return OK;
    }

    /* The server requires strictly increasing ids per connection, which JS
     * ids cannot guarantee across reconnects, so each request gets a fresh
     * wire id and nsIncoming maps the response back to the JS id. */
    int32_t wireId = 0;
    Status status = nsOutgoing(session, id, type, &wireId);
    if (status != OK) {
        json_object_put(root);
        return status;
    }

    Envelope envelope;
    envelope.v = version;
    envelope.id = wireId;
    envelope.type = type;
    envelope.payload = payloadJson;

    if (client != NULL) {
        status = clientSend(client, &envelope);
    }
    json_object_put(root);

    /* Report a broken link now instead of one heartbeat later. The callback
     * may free @p client, so it is not touched afterwards. */
    if (status != OK && client != NULL && clientIsBroken(client) && gDisconnectCallback) {
        gDisconnectCallback(gDisconnectContext);
    }
    return status;
}

char *bridgeFormatEmitScript(const Envelope *envelope) {
    if (!envelope) return NULL;

    struct json_object *root = json_object_new_object();
    if (!root) return NULL;

    json_object_object_add(root, "v", json_object_new_int(envelope->v));
    json_object_object_add(root, "id", json_object_new_int(envelope->id));
    json_object_object_add(root, "type",
                           json_object_new_string(envelope->type ? envelope->type : ""));

    struct json_object *payloadToEmit = NULL;
    if (envelope->payload && json_object_is_type(envelope->payload, json_type_object)) {
        if (json_object_object_get(envelope->payload, "token")) {
            payloadToEmit = json_object_new_object();
            json_object_object_foreach(envelope->payload, key, value) {
                if (strcmp(key, "token") != 0) {
                    json_object_get(value);
                    json_object_object_add(payloadToEmit, key, value);
                }
            }
        } else {
            json_object_get(envelope->payload);
            payloadToEmit = envelope->payload;
        }
    } else {
        payloadToEmit = json_object_new_object();
    }
    json_object_object_add(root, "payload", payloadToEmit);

    const char *jsonText = json_object_to_json_string(root);
    gchar *script = g_strdup_printf(
        "window.dispatchEvent(new CustomEvent('uno:message', {detail: %s}));",
        jsonText ? jsonText : "{}");
    json_object_put(root);
    return script;
}

void bridgeEmit(WebKitWebView *webView, const Envelope *envelope) {
    if (!webView || !envelope) return;

    gchar *script = bridgeFormatEmitScript(envelope);
    if (!script) return;

    webkit_web_view_evaluate_javascript(webView, script, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(script);
}

static int hexValue(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static Status urlDecode(const char *source, char *destination, size_t destinationSize) {
    size_t written = 0;
    for (size_t i = 0; source[i] != '\0'; i++) {
        if (source[i] == '%') {
            if (source[i + 1] == '\0' || source[i + 2] == '\0') {
                return BAD_REQUEST;
            }
            int highNibble = hexValue(source[i + 1]);
            int lowNibble = hexValue(source[i + 2]);
            if (highNibble < 0 || lowNibble < 0) {
                return BAD_REQUEST;
            }
            char decodedByte = (char)((highNibble << 4) | lowNibble);
            if (decodedByte == '\0') {
                /* Reject null byte injection */
                return BAD_REQUEST;
            }
            if (written + 1 >= destinationSize) return BAD_REQUEST;
            destination[written++] = decodedByte;
            i += 2;
        } else {
            if (written + 1 >= destinationSize) return BAD_REQUEST;
            destination[written++] = source[i];
        }
    }
    if (written >= destinationSize) return BAD_REQUEST;
    destination[written] = '\0';
    return OK;
}

Status bridgeResolvePath(const char *assetsDir, const char *uri, char *outPath,
                           size_t maxLength) {
    if (!assetsDir || !uri || !outPath || maxLength == 0) return BAD_REQUEST;

    const char *cursor = uri;
    if (strncmp(cursor, "uno://app/", 10) == 0) {
        cursor += 10;
    } else if (strncmp(cursor, "uno://app", 9) == 0) {
        cursor += 9;
        if (*cursor == '/') cursor++;
    } else if (strncmp(cursor, "uno:", 4) == 0) {
        cursor += 4;
        while (*cursor == '/') cursor++;
    } else if (*cursor == '/') {
        while (*cursor == '/') cursor++;
    }

    char decoded[512];
    if (urlDecode(cursor, decoded, sizeof(decoded)) != OK) {
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

    const char *relativePath = decoded;
    while (*relativePath == '/') relativePath++;

    if (*relativePath == '\0') {
        relativePath = "index.html";
    }

    int pathLength = snprintf(outPath, maxLength, "%s/%s", assetsDir, relativePath);
    if (pathLength < 0 || (size_t)pathLength >= maxLength) {
        return BAD_REQUEST;
    }

    return OK;
}

const char *bridgeMimeType(const char *path) {
    if (!path) return "application/octet-stream";
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";

    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html";
    if (strcasecmp(dot, ".js") == 0 || strcasecmp(dot, ".mjs") == 0) {
        return "application/javascript";
    }
    if (strcasecmp(dot, ".css") == 0) return "text/css";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(dot, ".wasm") == 0) return "application/wasm";

    return "application/octet-stream";
}

static void onUriSchemeRequest(WebKitURISchemeRequest *request, gpointer userData) {
    const char *assetsDir = (const char *)userData;
    if (!assetsDir) assetsDir = bridgeGetAssetsDir();

    const char *uri = webkit_uri_scheme_request_get_uri(request);
    char filePath[512];
    Status status = bridgeResolvePath(assetsDir, uri, filePath, sizeof(filePath));
    if (status != OK) {
        GError *error = g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_NOENT,
                                            "Invalid URI or access denied");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    GFile *file = g_file_new_for_path(filePath);
    GError *error = NULL;
    GFileInputStream *stream = g_file_read(file, NULL, &error);
    if (!stream) {
        GError *reply = error ? error
                              : g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_NOENT,
                                                    "File not found");
        webkit_uri_scheme_request_finish_error(request, reply);
        if (error) g_error_free(error);
        g_object_unref(file);
        return;
    }

    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    gint64 size = info ? g_file_info_get_size(info) : -1;
    if (info) g_object_unref(info);

    const char *mimeType = bridgeMimeType(filePath);
    webkit_uri_scheme_request_finish(request, G_INPUT_STREAM(stream), size, mimeType);

    g_object_unref(stream);
    g_object_unref(file);
}

void bridgeRegisterUriScheme(WebKitWebContext *context, const char *assetsDir) {
    (void)assetsDir;
    if (gSchemeRegistered) return;
    if (!context) context = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(context, "uno",
                                           onUriSchemeRequest,
                                           (gpointer)bridgeGetAssetsDir(), NULL);
    gSchemeRegistered = true;
}

/* Texts for the ERROR replies the bridge makes itself, without the server. */
#define DISCONNECTED_MESSAGE "Not connected to the server; the request was not sent"
#define REJECTED_MESSAGE "Request rejected before reaching the server"

typedef struct {
    WebKitWebView *webView;
    NetSession *session;
    Client **clientSlot; /* host-owned; points at NULL while disconnected */
} BridgeState;

/* Answer a request natively so JS settles it now instead of after its own
 * response timeout. */
static void emitRequestError(WebKitWebView *webView, int32_t jsId, const char *code,
                               const char *message)
{
    struct json_object *payload = json_object_new_object();
    if (!payload) return;
    /* Protocol: ERROR always carries both code and message, local ones too. */
    json_object_object_add(payload, "code", json_object_new_string(code));
    json_object_object_add(payload, "message", json_object_new_string(message));
    Envelope error = {1, jsId, "ERROR", payload};
    bridgeEmit(webView, &error);
    json_object_put(payload);
}

static void onScriptMessageUno(WebKitUserContentManager *manager,
                                   WebKitJavascriptResult *result,
                                   gpointer userData)
{
    (void)manager;
    BridgeState *state = (BridgeState *)userData;
    if (!state) return;
    JSCValue *value = webkit_javascript_result_get_js_value(result);
    if (!value) return;

    char *jsonText = jsc_value_to_string(value);
    if (!jsonText) return;

    int32_t jsId = 0;
    Client *client = *state->clientSlot;
    Status status = bridgeDispatch(state->session, client, jsonText, strlen(jsonText), &jsId);
    g_free(jsonText);

    /* Without a positive id JS has nothing to match an answer against. */
    if (status == OK || jsId <= 0) return;

    /* Not connected (or still resuming, or the send just broke the link) is
     * the reconnect case: the request is dropped, never queued, so it cannot
     * be replayed later. */
    const char *code = statusCode(status);
    const char *message = REJECTED_MESSAGE;
    if (nsState(state->session) != NS_CONNECTED) {
        code = "DISCONNECTED";
        message = DISCONNECTED_MESSAGE;
    }
    emitRequestError(state->webView, jsId, code, message);
}

Status bridgeInit(WebKitWebView *webView, NetSession *session, Client **clientSlot) {
    if (!webView || !session || !clientSlot) return BAD_REQUEST;

    WebKitWebContext *context = webkit_web_view_get_context(webView);
    bridgeRegisterUriScheme(context, bridgeGetAssetsDir());

    WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager(webView);
    BridgeState *state = g_new0(BridgeState, 1);
    state->webView = webView;
    state->session = session;
    state->clientSlot = clientSlot;

    g_object_set_data_full(G_OBJECT(webView), "bridge_state", state, g_free);

    g_signal_connect(manager, "script-message-received::uno",
                     G_CALLBACK(onScriptMessageUno), state);
    webkit_user_content_manager_register_script_message_handler(manager, "uno");

    return OK;
}
