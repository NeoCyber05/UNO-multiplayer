#include "desktop/bridge.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_bridge_dispatch_validation(void)
{
    /* Null or empty */
    assert(bridge_dispatch(NULL, NULL, 0) == BAD_REQUEST);
    assert(bridge_dispatch(NULL, "", 0) == BAD_REQUEST);

    /* Malformed JSON */
    const char *bad_json = "{\"v\": 1, \"id\":";
    assert(bridge_dispatch(NULL, bad_json, strlen(bad_json)) == BAD_REQUEST);

    /* Valid JSON, but not an object */
    const char *not_obj1 = "123";
    assert(bridge_dispatch(NULL, not_obj1, strlen(not_obj1)) == BAD_REQUEST);
    const char *not_obj2 = "[\"HELLO\"]";
    assert(bridge_dispatch(NULL, not_obj2, strlen(not_obj2)) == BAD_REQUEST);
    const char *not_obj3 = "\"hello\"";
    assert(bridge_dispatch(NULL, not_obj3, strlen(not_obj3)) == BAD_REQUEST);

    /* Missing or wrong 'v' */
    const char *no_v = "{\"id\": 1, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridge_dispatch(NULL, no_v, strlen(no_v)) == BAD_REQUEST);
    const char *wrong_v = "{\"v\": 2, \"id\": 1, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridge_dispatch(NULL, wrong_v, strlen(wrong_v)) == BAD_REQUEST);

    /* Missing 'id' */
    const char *no_id = "{\"v\": 1, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridge_dispatch(NULL, no_id, strlen(no_id)) == BAD_REQUEST);

    /* Missing or wrong 'type' */
    const char *no_type = "{\"v\": 1, \"id\": 1, \"payload\": {}}";
    assert(bridge_dispatch(NULL, no_type, strlen(no_type)) == BAD_REQUEST);
    const char *empty_type = "{\"v\": 1, \"id\": 1, \"type\": \"\", \"payload\": {}}";
    assert(bridge_dispatch(NULL, empty_type, strlen(empty_type)) == BAD_REQUEST);
    const char *numeric_type = "{\"v\": 1, \"id\": 1, \"type\": 123, \"payload\": {}}";
    assert(bridge_dispatch(NULL, numeric_type, strlen(numeric_type)) == BAD_REQUEST);

    /* Payload not an object */
    const char *array_payload = "{\"v\": 1, \"id\": 1, \"type\": \"HELLO\", \"payload\": []}";
    assert(bridge_dispatch(NULL, array_payload, strlen(array_payload)) == BAD_REQUEST);
    const char *num_payload = "{\"v\": 1, \"id\": 1, \"type\": \"HELLO\", \"payload\": 42}";
    assert(bridge_dispatch(NULL, num_payload, strlen(num_payload)) == BAD_REQUEST);
    const char *no_payload = "{\"v\": 1, \"id\": 1, \"type\": \"HELLO\"}";
    assert(bridge_dispatch(NULL, no_payload, strlen(no_payload)) == BAD_REQUEST);

    /* Unknown message type not in whitelist */
    const char *unknown_type = "{\"v\": 1, \"id\": 1, \"type\": \"UNKNOWN_MSG\", \"payload\": {}}";
    assert(bridge_dispatch(NULL, unknown_type, strlen(unknown_type)) == BAD_REQUEST);
    const char *malicious_type = "{\"v\": 1, \"id\": 1, \"type\": \"DROP_TABLE\", \"payload\": {}}";
    assert(bridge_dispatch(NULL, malicious_type, strlen(malicious_type)) == BAD_REQUEST);

    /* Valid messages in whitelist */
    const char *valid_hello = "{\"v\": 1, \"id\": 1, \"type\": \"HELLO\", \"payload\": {}}";
    assert(bridge_dispatch(NULL, valid_hello, strlen(valid_hello)) == OK);

    const char *valid_ping = "{\"v\": 1, \"id\": 2, \"type\": \"PING\", \"payload\": {}}";
    assert(bridge_dispatch(NULL, valid_ping, strlen(valid_ping)) == OK);

    const char *valid_spike_done = "{\"v\": 1, \"id\": 999, \"type\": \"SPIKE_DONE\", \"payload\": {}}";
    assert(bridge_dispatch(NULL, valid_spike_done, strlen(valid_spike_done)) == OK);
}

static void test_scheme_path_traversal(void)
{
    char out_path[512];

    /* Valid paths */
    assert(bridge_resolve_path("tests/fixtures", "uno://app/index.html", out_path, sizeof(out_path)) == OK);
    assert(strcmp(out_path, "tests/fixtures/index.html") == 0);

    assert(bridge_resolve_path("tests/fixtures", "uno://app/spike_test.html", out_path, sizeof(out_path)) == OK);
    assert(strcmp(out_path, "tests/fixtures/spike_test.html") == 0);

    assert(bridge_resolve_path("tests/fixtures", "uno://app/", out_path, sizeof(out_path)) == OK);
    assert(strcmp(out_path, "tests/fixtures/index.html") == 0);

    assert(bridge_resolve_path("tests/fixtures", "uno://app", out_path, sizeof(out_path)) == OK);
    assert(strcmp(out_path, "tests/fixtures/index.html") == 0);

    assert(bridge_resolve_path("tests/fixtures", "/index.html", out_path, sizeof(out_path)) == OK);
    assert(strcmp(out_path, "tests/fixtures/index.html") == 0);

    assert(bridge_resolve_path("client/dist", "uno://app/assets/main.js", out_path, sizeof(out_path)) == OK);
    assert(strcmp(out_path, "client/dist/assets/main.js") == 0);

    /* Path traversal rejection */
    assert(bridge_resolve_path("tests/fixtures", "uno://app/../secret.txt", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "uno://app/..%2fsecret.txt", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "uno://app/..%2Fsecret.txt", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "uno://app/%2e%2e/secret.txt", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "uno://app/%2e%2e%2fsecret.txt", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "uno://app/%2E%2E%2Fsecret.txt", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "uno://app/foo/../../etc/passwd", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "uno://app/foo/%2e%2e/%2e%2e/etc/passwd", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "uno://app/something/..", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "../Makefile", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "..%2fMakefile", out_path, sizeof(out_path)) != OK);
    assert(bridge_resolve_path("tests/fixtures", "uno://app/%00hidden.html", out_path, sizeof(out_path)) != OK);
}

static void test_mime_types(void)
{
    assert(strcmp(bridge_mime_type("index.html"), "text/html") == 0);
    assert(strcmp(bridge_mime_type("app.js"), "application/javascript") == 0);
    assert(strcmp(bridge_mime_type("style.css"), "text/css") == 0);
    assert(strcmp(bridge_mime_type("icon.png"), "image/png") == 0);
    assert(strcmp(bridge_mime_type("data.json"), "application/json") == 0);
    assert(strcmp(bridge_mime_type("unknown.xyz"), "application/octet-stream") == 0);
}

static void test_bridge_opcode_whitelist(void)
{
    /* Valid spec §3 opcodes (+ SPIKE_DONE for smoke test) must return OK */
    static const char *const valid_opcodes[] = {
        "LOGIN", "REGISTER", "ROOM_MODE", "LOGOUT", "QUEUE_JOIN",
        "PLAY", "DRAW", "PASS", "SAY_UNO", "DRAFT_PICK", "CHAT",
        "HELLO", "PING", "RESUME", "PROFILE_GET", "PROFILE_UPDATE",
        "LOBBY_GET", "FRIEND_REQUEST", "FRIEND_REPLY", "FRIEND_LIST",
        "LEADERBOARD_GET", "ROOM_CREATE", "ROOM_JOIN", "ROOM_LEAVE",
        "ROOM_BOT", "ROOM_KICK", "ROOM_START", "INVITE_SEND",
        "INVITE_REPLY", "PARTY_LEAVE", "QUEUE_LEAVE", "MATCH_LEAVE",
        "SPIKE_DONE", NULL
    };

    char buf[256];
    for (int i = 0; valid_opcodes[i] != NULL; i++) {
        snprintf(buf, sizeof(buf), "{\"v\": 1, \"id\": %d, \"type\": \"%s\", \"payload\": {}}", i + 1, valid_opcodes[i]);
        assert(bridge_dispatch(NULL, buf, strlen(buf)) == OK);
    }

    /* Invalid or obsolete opcodes must return BAD_REQUEST */
    static const char *const invalid_opcodes[] = {
        "AUTH_LOGIN", "AUTH_REGISTER", "AUTH_GUEST", "AUTH_LOGOUT",
        "INVITE_RESPOND", "GAME_ACTION", "PARTY_INVITE", "MATCH_CONFIRM",
        "REMATCH_REQUEST", "MATCH_SURRENDER", "CHAT_SEND", NULL
    };

    for (int i = 0; invalid_opcodes[i] != NULL; i++) {
        snprintf(buf, sizeof(buf), "{\"v\": 1, \"id\": %d, \"type\": \"%s\", \"payload\": {}}", i + 100, invalid_opcodes[i]);
        assert(bridge_dispatch(NULL, buf, strlen(buf)) == BAD_REQUEST);
    }
}

static void test_bridge_emit_strips_token(void)
{
    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "token", json_object_new_string("secret_token_123"));
    json_object_object_add(payload, "user", json_object_new_string("alice"));

    Envelope e;
    e.v = 1;
    e.id = 42;
    e.type = "OK";
    e.payload = payload;

    char *script = bridge_format_emit_script(&e);
    assert(script != NULL);
    assert(strstr(script, "secret_token_123") == NULL);
    assert(strstr(script, "\"token\"") == NULL);
    assert(strstr(script, "alice") != NULL);

    g_free(script);
    json_object_put(payload);
}

int main(void)
{
    test_bridge_dispatch_validation();
    test_bridge_opcode_whitelist();
    test_bridge_emit_strips_token();
    test_scheme_path_traversal();
    test_mime_types();
    printf("bridge: OK\n");
    return 0;
}
