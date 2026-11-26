#include "desktop/bridge.h"
#include "desktop/net_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* A session that accepts JS requests, as after connecting without a token. */
static NetSession *connectedSession(void) {
    NetSession *session = nsCreate();
    assert(session != NULL);
    nsOnConnected(session);
    assert(nsState(session) == NS_CONNECTED);
    return session;
}

static void testBridgeDispatchValidation(void) {
    NetSession *session = connectedSession();
    int32_t jsId = -1;

    /* Null or empty */
    assert(bridgeDispatch(session, NULL, NULL, 0, &jsId) == BAD_REQUEST);
    assert(bridgeDispatch(session, NULL, "", 0, &jsId) == BAD_REQUEST);

    /* Malformed JSON */
    const char *badJson = "{\"v\": 1, \"id\":";
    assert(bridgeDispatch(session, NULL, badJson, strlen(badJson), &jsId) == BAD_REQUEST);

    /* Valid JSON, but not an object */
    const char *notObj1 = "123";
    assert(bridgeDispatch(session, NULL, notObj1, strlen(notObj1), &jsId) == BAD_REQUEST);
    const char *notObj2 = "[\"HELLO\"]";
    assert(bridgeDispatch(session, NULL, notObj2, strlen(notObj2), &jsId) == BAD_REQUEST);
    const char *notObj3 = "\"hello\"";
    assert(bridgeDispatch(session, NULL, notObj3, strlen(notObj3), &jsId) == BAD_REQUEST);

    /* Missing or wrong 'v' */
    const char *noV = "{\"id\": 1, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, noV, strlen(noV), &jsId) == BAD_REQUEST);
    const char *wrongV = "{\"v\": 2, \"id\": 1, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, wrongV, strlen(wrongV), &jsId) == BAD_REQUEST);

    /* Missing 'id' */
    const char *noId = "{\"v\": 1, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, noId, strlen(noId), &jsId) == BAD_REQUEST);

    /* Missing or wrong 'type' */
    const char *noType = "{\"v\": 1, \"id\": 1, \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, noType, strlen(noType), &jsId) == BAD_REQUEST);
    const char *emptyType = "{\"v\": 1, \"id\": 1, \"type\": \"\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, emptyType, strlen(emptyType), &jsId) == BAD_REQUEST);
    const char *numericType = "{\"v\": 1, \"id\": 1, \"type\": 123, \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, numericType, strlen(numericType), &jsId) == BAD_REQUEST);

    /* Payload not an object */
    const char *arrayPayload = "{\"v\": 1, \"id\": 1, \"type\": \"HELLO\", \"payload\": []}";
    assert(bridgeDispatch(session, NULL, arrayPayload, strlen(arrayPayload),
                           &jsId) == BAD_REQUEST);
    const char *numPayload = "{\"v\": 1, \"id\": 1, \"type\": \"HELLO\", \"payload\": 42}";
    assert(bridgeDispatch(session, NULL, numPayload, strlen(numPayload), &jsId) == BAD_REQUEST);
    const char *noPayload = "{\"v\": 1, \"id\": 1, \"type\": \"HELLO\"}";
    assert(bridgeDispatch(session, NULL, noPayload, strlen(noPayload), &jsId) == BAD_REQUEST);

    /* Unknown message type not in whitelist */
    const char *unknownType = "{\"v\": 1, \"id\": 1, \"type\": \"UNKNOWN_MSG\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, unknownType, strlen(unknownType), &jsId) == BAD_REQUEST);
    const char *maliciousType = "{\"v\": 1, \"id\": 1, \"type\": \"DROP_TABLE\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, maliciousType, strlen(maliciousType),
                           &jsId) == BAD_REQUEST);

    /* Valid messages in whitelist */
    const char *validHello = "{\"v\": 1, \"id\": 1, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, validHello, strlen(validHello), &jsId) == OK);

    const char *validPing = "{\"v\": 1, \"id\": 2, \"type\": \"PING\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, validPing, strlen(validPing), &jsId) == OK);

    const char *validSpikeDone =
        "{\"v\": 1, \"id\": 999, \"type\": \"SPIKE_DONE\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, validSpikeDone, strlen(validSpikeDone), &jsId) == OK);

    /* Without a session nothing can be mapped to a wire id. */
    assert(bridgeDispatch(NULL, NULL, validHello, strlen(validHello), &jsId) == BAD_REQUEST);

    /* JS ids must be positive: 0 is reserved for server pushes. */
    const char *zeroId = "{\"v\": 1, \"id\": 0, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, zeroId, strlen(zeroId), &jsId) == BAD_REQUEST);

    nsDestroy(session);
}

static void testJsResumeBlocked(void) {
    NetSession *session = connectedSession();
    int32_t jsId = -1;

    /* Only native code holds the token, so JS must never send RESUME. */
    const char *resume =
        "{\"v\": 1, \"id\": 4, \"type\": \"RESUME\", \"payload\": {\"token\": \"x\"}}";
    assert(bridgeDispatch(session, NULL, resume, strlen(resume), &jsId) == BAD_REQUEST);

    nsDestroy(session);
}

static void testDispatchWhileDisconnected(void) {
    NetSession *session = connectedSession();
    nsOnDisconnected(session);

    /* The caller needs the JS id to answer with ERROR DISCONNECTED. */
    int32_t jsId = -1;
    const char *hello = "{\"v\": 1, \"id\": 9, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, hello, strlen(hello), &jsId) == BUSY);
    assert(jsId == 9);

    nsDestroy(session);
}

static int g_disconnect_reports = 0;

static void countDisconnect(void *ctx) {
    (void)ctx;
    g_disconnect_reports++;
}

static void testSendFailureReportsDisconnect(void) {
    /* A client whose peer is gone: the send fails with EPIPE. */
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    Client *client = calloc(1, sizeof(*client));
    assert(client != NULL);
    assert(connInit(&client->conn, fds[0], 0, 0) == OK);
    client->connected = true;
    assert(close(fds[1]) == 0);

    NetSession *session = connectedSession();
    bridgeSetDisconnectCallback(countDisconnect, NULL);
    g_disconnect_reports = 0;

    /* The host must hear about it now, not one heartbeat later. */
    int32_t jsId = -1;
    const char *hello = "{\"v\": 1, \"id\": 3, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridgeDispatch(session, client, hello, strlen(hello), &jsId) != OK);
    assert(jsId == 3);
    assert(g_disconnect_reports == 1);

    /* A request rejected before sending is not a connection problem. */
    const char *unknown = "{\"v\": 1, \"id\": 4, \"type\": \"NOPE\", \"payload\": {}}";
    assert(bridgeDispatch(session, client, unknown, strlen(unknown), &jsId) == BAD_REQUEST);
    assert(g_disconnect_reports == 1);

    bridgeSetDisconnectCallback(NULL, NULL);
    clientClose(client);
    nsDestroy(session);
}

static void testDispatchWhileResuming(void) {
    NetSession *session = nsCreate();
    int32_t wireId = 0;

    /* Hold a token so the next connection starts in NS_RESUMING. */
    nsOnConnected(session);
    assert(nsOutgoing(session, 1, "LOGIN", &wireId) == OK);
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "token", json_object_new_string("tok"));
    Envelope loginOk = {1, wireId, "OK", payload};
    int32_t forwardedId = 0;
    assert(nsIncoming(session, &loginOk, &forwardedId, NULL) == NS_FORWARD);
    json_object_put(payload);
    nsOnDisconnected(session);
    nsOnConnected(session);
    assert(nsState(session) == NS_RESUMING);

    int32_t jsId = -1;
    const char *lobby = "{\"v\": 1, \"id\": 12, \"type\": \"LOBBY_GET\", \"payload\": {}}";
    assert(bridgeDispatch(session, NULL, lobby, strlen(lobby), &jsId) == BUSY);
    assert(jsId == 12);

    nsDestroy(session);
}

static void testSchemePathTraversal(void) {
    char outPath[512];

    /* Valid paths */
    assert(bridgeResolvePath("tests/fixtures", "uno://app/index.html", outPath,
                               sizeof(outPath)) == OK);
    assert(strcmp(outPath, "tests/fixtures/index.html") == 0);

    assert(bridgeResolvePath("tests/fixtures", "uno://app/spike_test.html", outPath,
                               sizeof(outPath)) == OK);
    assert(strcmp(outPath, "tests/fixtures/spike_test.html") == 0);

    assert(bridgeResolvePath("tests/fixtures", "uno://app/", outPath, sizeof(outPath)) == OK);
    assert(strcmp(outPath, "tests/fixtures/index.html") == 0);

    assert(bridgeResolvePath("tests/fixtures", "uno://app", outPath, sizeof(outPath)) == OK);
    assert(strcmp(outPath, "tests/fixtures/index.html") == 0);

    assert(bridgeResolvePath("tests/fixtures", "/index.html", outPath, sizeof(outPath)) == OK);
    assert(strcmp(outPath, "tests/fixtures/index.html") == 0);

    assert(bridgeResolvePath("client/dist", "uno://app/assets/main.js", outPath,
                               sizeof(outPath)) == OK);
    assert(strcmp(outPath, "client/dist/assets/main.js") == 0);

    /* Path traversal rejection */
    assert(bridgeResolvePath("tests/fixtures", "uno://app/../secret.txt", outPath,
                               sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "uno://app/..%2fsecret.txt", outPath,
                               sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "uno://app/..%2Fsecret.txt", outPath,
                               sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "uno://app/%2e%2e/secret.txt", outPath,
                               sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "uno://app/%2e%2e%2fsecret.txt", outPath,
                               sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "uno://app/%2E%2E%2Fsecret.txt", outPath,
                               sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "uno://app/foo/../../etc/passwd", outPath,
                               sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "uno://app/foo/%2e%2e/%2e%2e/etc/passwd", outPath,
                               sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "uno://app/something/..", outPath,
                               sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "../Makefile", outPath, sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "..%2fMakefile", outPath, sizeof(outPath)) != OK);
    assert(bridgeResolvePath("tests/fixtures", "uno://app/%00hidden.html", outPath,
                               sizeof(outPath)) != OK);
}

static void testMimeTypes(void) {
    assert(strcmp(bridgeMimeType("index.html"), "text/html") == 0);
    assert(strcmp(bridgeMimeType("app.js"), "application/javascript") == 0);
    assert(strcmp(bridgeMimeType("style.css"), "text/css") == 0);
    assert(strcmp(bridgeMimeType("icon.png"), "image/png") == 0);
    assert(strcmp(bridgeMimeType("data.json"), "application/json") == 0);
    assert(strcmp(bridgeMimeType("unknown.xyz"), "application/octet-stream") == 0);
}

static void testBridgeOpcodeWhitelist(void) {
    NetSession *session = connectedSession();
    int32_t jsId = -1;

    /* Valid spec §3 opcodes (+ SPIKE_DONE for smoke test) must return OK */
    static const char *const validOpcodes[] = {
        "LOGIN", "REGISTER", "ROOM_MODE", "LOGOUT", "QUEUE_JOIN",
        "PLAY", "DRAW", "PASS", "SAY_UNO", "DRAFT_PICK", "CHAT",
        "HELLO", "PING", "PROFILE_GET", "PROFILE_UPDATE",
        "LOBBY_GET", "FRIEND_REQUEST", "FRIEND_REPLY", "FRIEND_LIST",
        "LEADERBOARD_GET", "ROOM_CREATE", "ROOM_JOIN", "ROOM_LEAVE",
        "ROOM_BOT", "ROOM_KICK", "ROOM_START", "INVITE_SEND",
        "INVITE_REPLY", "PARTY_LEAVE", "QUEUE_LEAVE", "MATCH_LEAVE",
        "SPIKE_DONE", NULL
    };

    char buf[256];
    for (int i = 0; validOpcodes[i] != NULL; i++) {
        snprintf(buf, sizeof(buf), "{\"v\": 1, \"id\": %d, \"type\": \"%s\", \"payload\": {}}",
                 i + 1, validOpcodes[i]);
        assert(bridgeDispatch(session, NULL, buf, strlen(buf), &jsId) == OK);
    }

    /* Invalid or obsolete opcodes must return BAD_REQUEST */
    static const char *const invalidOpcodes[] = {
        "AUTH_LOGIN", "AUTH_REGISTER", "AUTH_GUEST", "AUTH_LOGOUT",
        "INVITE_RESPOND", "GAME_ACTION", "PARTY_INVITE", "MATCH_CONFIRM",
        "REMATCH_REQUEST", "MATCH_SURRENDER", "CHAT_SEND", "RESUME", NULL
    };

    for (int i = 0; invalidOpcodes[i] != NULL; i++) {
        snprintf(buf, sizeof(buf), "{\"v\": 1, \"id\": %d, \"type\": \"%s\", \"payload\": {}}",
                 i + 100, invalidOpcodes[i]);
        assert(bridgeDispatch(session, NULL, buf, strlen(buf), &jsId) == BAD_REQUEST);
    }

    nsDestroy(session);
}

static void testBridgeEmitStripsToken(void) {
    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "token", json_object_new_string("secret_token_123"));
    json_object_object_add(payload, "user", json_object_new_string("alice"));

    Envelope envelope;
    envelope.v = 1;
    envelope.id = 42;
    envelope.type = "OK";
    envelope.payload = payload;

    char *script = bridgeFormatEmitScript(&envelope);
    assert(script != NULL);
    assert(strstr(script, "secret_token_123") == NULL);
    assert(strstr(script, "\"token\"") == NULL);
    assert(strstr(script, "alice") != NULL);

    g_free(script);
    json_object_put(payload);
}

int main(void) {
    testBridgeDispatchValidation();
    testJsResumeBlocked();
    testDispatchWhileDisconnected();
    testDispatchWhileResuming();
    testSendFailureReportsDisconnect();
    testBridgeOpcodeWhitelist();
    testBridgeEmitStripsToken();
    testSchemePathTraversal();
    testMimeTypes();
    printf("bridge: OK\n");
    return 0;
}
