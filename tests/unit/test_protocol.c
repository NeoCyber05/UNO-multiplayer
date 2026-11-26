#include "shared/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define HELLO "{\"v\":1,\"id\":1,\"type\":\"HELLO\",\"payload\":{}}"

static Status countHello(const Envelope *envelope, void *context) {
    assert(envelope->v == 1 && envelope->id == 1 && strcmp(envelope->type, "HELLO") == 0);
    assert(json_object_is_type(envelope->payload, json_type_object));
    ++*(int *)context;
    return OK;
}

static Status feedBody(const char *body, size_t length, FrameCallback onFrame, void *context) {
    FrameReader reader = {0};
    uint8_t prefix[] = {0, (uint8_t)(length >> 16), (uint8_t)(length >> 8), (uint8_t)length};
    Status status = frameFeed(&reader, prefix, sizeof prefix, onFrame, context);
    if (status != OK) return status;
    return frameFeed(&reader, (const uint8_t *)body, length, onFrame, context);
}

static void testFragmentedAndCoalesced(void) {
    Envelope hello = {1, 1, "HELLO", json_object_new_object()};
    uint8_t pair[256];
    size_t length;
    assert(frameEncode(&hello, pair, sizeof pair / 2, &length) == OK);
    assert(length == sizeof HELLO + 3 && pair[3] == length - 4);
    json_object_put(hello.payload);
    FrameReader reader;
    frameReaderReset(&reader);
    int callbacks = 0;
    for (size_t i = 0; i < length; i++)
        assert(frameFeed(&reader, pair + i, 1, countHello, &callbacks) == OK);
    memcpy(pair + length, pair, length);
    assert(frameFeed(&reader, pair, 2 * length, countHello, &callbacks) == OK);
    assert(callbacks == 3);
}

static bool rejectLength(size_t length) {
    FrameReader reader = {0};
    uint8_t prefix[] = {0, (uint8_t)(length >> 16), (uint8_t)(length >> 8), (uint8_t)length};
    return frameFeed(&reader, prefix, 4, countHello, NULL) == BAD_REQUEST;
}

static Status observed(const Envelope *envelope, void *context) {
    (void)envelope;
    ++*(int *)context;
    return OK;
}

static Status parse(const char *body, size_t length) {
    int calls = 0;
    Status status = feedBody(body, length, observed, &calls);
    assert(calls == (status == OK));
    return status;
}

static void testFrameLimits(void) {
    assert(rejectLength(0) && rejectLength(65537));
    char json[FRAME_MAX_JSON];
    memset(json, ' ', sizeof json);
    memcpy(json + sizeof json - strlen(HELLO), HELLO, strlen(HELLO));
    assert(parse(json, sizeof json) == OK);

    for (int depth = 16; depth <= 17; depth++) {
        strcpy(json, "{\"v\":1,\"id\":1,\"type\":\"HELLO\",\"payload\":{\"x\":");
        for (int i = 2; i < depth; i++) strcat(json, "[");
        strcat(json, "0");
        for (int i = 2; i < depth; i++) strcat(json, "]");
        strcat(json, "}}");
        assert(parse(json, strlen(json)) == (depth == 16 ? OK : BAD_REQUEST));
        char *leaf = strrchr(json, '0');
        memmove(leaf, leaf + 1, strlen(leaf));
        assert(parse(json, strlen(json)) == (depth == 16 ? OK : BAD_REQUEST));
    }
}

static void testInvalidJson(void) {
    const char *invalid[] = {
        HELLO "x", HELLO "{}", "null", "[]", "{}",
        "{\"v\":2,\"id\":1,\"type\":\"A\",\"payload\":{}}",
        "{\"v\":1,\"id\":2147483648,\"type\":\"A\",\"payload\":{}}",
        "{\"v\":1,\"id\":-1,\"type\":\"A\",\"payload\":{}}",
        "{\"v\":1,\"id\":\"1\",\"type\":\"A\",\"payload\":{}}",
        "{\"v\":1,\"id\":1,\"type\":\"A\",\"payload\":[]}",
        "{\"v\":1,\"id\":1,\"type\":\"A\\u0000B\",\"payload\":{}}",
        "{\"v\\u0000x\":1,\"id\":1,\"type\":\"A\",\"payload\":{}}",
        "{\"v\":1,\"id\":1,\"type\":\"A\nB\",\"payload\":{}}",
        "{\"v\":1,\"id\":1,\"type\":\"A\",\"payload\":{\"n\":NaN}}",
        "{\"v\":1,\"id\":1,\"type\":\"A\",\"payload\":{\"n\":1.}}",
        "{\"v\":1,\"id\":1,\"type\":\"A\",\"payload\":{\"n\":-01}}",
        "{\"v\":1,\"id\":1,\"type\":\"A\",\"payload\":{\"n\":1e+}}",
        "{\"v\":1,\"id\":1,\"type\":\"\xe0\x80\x80\",\"payload\":{}}",
        "{\"v\":1,\"id\":1,\"type\":\"\xff\",\"payload\":{}}"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
        assert(parse(invalid[i], strlen(invalid[i])) == BAD_REQUEST);
    assert(parse(HELLO, sizeof HELLO) == BAD_REQUEST);
    const char literal[] = "{\"v\":1,\"id\":1,\"type\":\"A\",\"payload\":{\"x\":\"\\\\u0000\"}}";
    assert(parse(literal, strlen(literal)) == OK);
    const char number[] = "{\"v\":1,\"id\":1,\"type\":\"A\",\"payload\":{\"x\":-0.01e+02}}";
    assert(parse(number, strlen(number)) == OK);
}

static Status matchFixture(const Envelope *envelope, void *context) {
    struct json_object *fixture = context;
    assert(envelope->v == json_object_get_int(json_object_object_get(fixture, "v")));
    assert(envelope->id == json_object_get_int(json_object_object_get(fixture, "id")));
    const char *expectedType = json_object_get_string(json_object_object_get(fixture, "type"));
    assert(strcmp(envelope->type, expectedType) == 0);
    assert(json_object_equal(envelope->payload, json_object_object_get(fixture, "payload")));
    return BUSY; /* also verify callback errors reach the caller */
}

static void testFixtures(void) {
    struct json_object *rows = json_object_from_file("tests/fixtures/requests.json");
    assert(rows && json_object_array_length(rows) >= 4);
    for (size_t i = 0; i < json_object_array_length(rows); i++) {
        struct json_object *fixture = json_object_array_get_idx(rows, i);
        Envelope envelope = {json_object_get_int(json_object_object_get(fixture, "v")),
                      json_object_get_int(json_object_object_get(fixture, "id")),
                      json_object_get_string(json_object_object_get(fixture, "type")),
                      json_object_object_get(fixture, "payload")};
        uint8_t out[FRAME_CAPACITY];
        size_t length;
        assert(frameEncode(&envelope, out, 4, &length) == BAD_REQUEST && length == 0);
        assert(frameEncode(&envelope, out, sizeof out, &length) == OK);
        FrameReader reader = {0};
        assert(frameFeed(&reader, out, length, matchFixture, fixture) == BUSY);
    }
    json_object_put(rows);
}

static void testStatusCode(void) {
    assert(strcmp(statusCode(OK), "OK") == 0);
    assert(strcmp(statusCode(STALE_REQUEST), "STALE_REQUEST") == 0);
    assert(strcmp(statusCode(SERVER_FULL), "SERVER_FULL") == 0);
    assert(strcmp(statusCode((Status)99), "BAD_REQUEST") == 0);
}

int main(void) {
    testFragmentedAndCoalesced();
    testFrameLimits();
    testInvalidJson();
    testFixtures();
    testStatusCode();
    puts("protocol: OK");
    return 0;
}
