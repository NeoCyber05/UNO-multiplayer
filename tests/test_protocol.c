#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define HELLO "{\"v\":1,\"id\":1,\"type\":\"HELLO\",\"payload\":{}}"

static Status count_hello(const Envelope *e, void *ctx)
{
    assert(e->v == 1 && e->id == 1 && strcmp(e->type, "HELLO") == 0);
    assert(json_object_is_type(e->payload, json_type_object));
    ++*(int *)ctx;
    return OK;
}

static Status feed_body(const char *body, size_t n, FrameCallback cb, void *ctx)
{
    FrameReader r = {0};
    uint8_t prefix[] = {0, (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    Status s = frame_feed(&r, prefix, sizeof prefix, cb, ctx);
    return s == OK ? frame_feed(&r, (const uint8_t *)body, n, cb, ctx) : s;
}

static void test_fragmented_and_coalesced(void)
{
    Envelope hello = {1, 1, "HELLO", json_object_new_object()};
    uint8_t pair[256];
    size_t n;
    assert(frame_encode(&hello, pair, sizeof pair / 2, &n) == OK);
    assert(n == sizeof HELLO + 3 && pair[3] == n - 4);
    json_object_put(hello.payload);
    FrameReader r;
    frame_reader_reset(&r);
    int callbacks = 0;
    for (size_t i = 0; i < n; i++)
        assert(frame_feed(&r, pair + i, 1, count_hello, &callbacks) == OK);
    memcpy(pair + n, pair, n);
    assert(frame_feed(&r, pair, 2 * n, count_hello, &callbacks) == OK);
    assert(callbacks == 3);
}

static bool reject_length(size_t n)
{
    FrameReader r = {0};
    uint8_t prefix[] = {0, (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    return frame_feed(&r, prefix, 4, count_hello, NULL) == BAD_REQUEST;
}

static Status observed(const Envelope *e, void *ctx)
{
    (void)e;
    ++*(int *)ctx;
    return OK;
}

static Status parse(const char *s, size_t n)
{
    int calls = 0;
    Status status = feed_body(s, n, observed, &calls);
    assert(calls == (status == OK));
    return status;
}

static void test_frame_limits(void)
{
    assert(reject_length(0) && reject_length(65537));
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

static void test_invalid_json(void)
{
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

static Status match_fixture(const Envelope *e, void *ctx)
{
    struct json_object *j = ctx;
    assert(e->v == json_object_get_int(json_object_object_get(j, "v")));
    assert(e->id == json_object_get_int(json_object_object_get(j, "id")));
    assert(strcmp(e->type, json_object_get_string(json_object_object_get(j, "type"))) == 0);
    assert(json_object_equal(e->payload, json_object_object_get(j, "payload")));
    return BUSY; /* also verify callback errors reach the caller */
}

static void test_fixtures(void)
{
    struct json_object *rows = json_object_from_file("tests/fixtures/requests.json");
    assert(rows && json_object_array_length(rows) >= 4);
    for (size_t i = 0; i < json_object_array_length(rows); i++) {
        struct json_object *j = json_object_array_get_idx(rows, i);
        Envelope e = {json_object_get_int(json_object_object_get(j, "v")),
                      json_object_get_int(json_object_object_get(j, "id")),
                      json_object_get_string(json_object_object_get(j, "type")),
                      json_object_object_get(j, "payload")};
        uint8_t out[FRAME_CAPACITY];
        size_t n;
        assert(frame_encode(&e, out, 4, &n) == BAD_REQUEST && n == 0);
        assert(frame_encode(&e, out, sizeof out, &n) == OK);
        FrameReader r = {0};
        assert(frame_feed(&r, out, n, match_fixture, j) == BUSY);
    }
    json_object_put(rows);
}

int main(void)
{
    test_fragmented_and_coalesced();
    test_frame_limits();
    test_invalid_json();
    test_fixtures();
    puts("protocol: OK");
    return 0;
}
