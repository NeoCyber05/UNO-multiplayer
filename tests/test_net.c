#include "shared/net.h"
#include "desktop/tcp_client.h"
#include "shared/protocol.h"
#include "server/app.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    assert(flags >= 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

typedef struct {
    int count;
    int last_id;
    char last_type[32];
} TestReceiver;

static Status on_frame_received(const Envelope *e, void *ctx)
{
    TestReceiver *r = (TestReceiver *)ctx;
    r->count++;
    r->last_id = e->id;
    strncpy(r->last_type, e->type, sizeof(r->last_type) - 1);
    r->last_type[sizeof(r->last_type) - 1] = '\0';
    return OK;
}

static void test_partial_write_and_flush(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(set_nonblocking(sv[0]) == 0);
    assert(set_nonblocking(sv[1]) == 0);

    /* Set small send buffer on sv[0] and small recv buffer on sv[1] */
    int buf_size = 2048;
    assert(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size)) == 0);
    assert(setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size)) == 0);

    Conn c;
    assert(conn_init(&c, sv[0], 0, 1) == OK);

    /* Construct an envelope with ~8KB payload to force partial write */
    struct json_object *payload = json_object_new_object();
    char big_string[8192];
    memset(big_string, 'X', sizeof(big_string) - 1);
    big_string[sizeof(big_string) - 1] = '\0';
    json_object_object_add(payload, "bulk", json_object_new_string(big_string));

    Envelope env = {1, 42, "HELLO", payload};
    assert(conn_queue(&c, &env) == OK);
    json_object_put(payload);

    assert(c.out_len > 8000);
    assert(c.out_pos == 0);

    /* Flush once: should write until kernel buffer is full and hit EAGAIN */
    Status s = conn_flush(&c);
    assert(s == OK);
    assert(c.out_pos > 0);
    assert(c.out_pos < c.out_len);

    /* Read some bytes from sv[1] */
    uint8_t recv_buf[16384];
    size_t total_received = 0;
    while (1) {
        ssize_t n = recv(sv[1], recv_buf + total_received, sizeof(recv_buf) - total_received, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            assert(0 && "recv error");
        }
        if (n == 0) break;
        total_received += (size_t)n;
    }
    assert(total_received == c.out_pos);

    /* Flush again to send remaining data */
    s = conn_flush(&c);
    assert(s == OK);

    /* If still remaining, drain and flush until fully sent */
    while (c.out_pos < c.out_len) {
        ssize_t n = recv(sv[1], recv_buf + total_received, sizeof(recv_buf) - total_received, 0);
        if (n > 0) total_received += (size_t)n;
        s = conn_flush(&c);
        assert(s == OK);
    }
    assert(c.out_len == 0);
    assert(c.out_pos == 0);

    /* Drain any remaining bytes from receiver */
    while (1) {
        ssize_t n = recv(sv[1], recv_buf + total_received, sizeof(recv_buf) - total_received, 0);
        if (n <= 0) break;
        total_received += (size_t)n;
    }

    /* Verify receiver got the full envelope */
    FrameReader reader;
    frame_reader_reset(&reader);
    TestReceiver receiver = {0};
    assert(frame_feed(&reader, recv_buf, total_received, on_frame_received, &receiver) == OK);
    assert(receiver.count == 1);
    assert(receiver.last_id == 42);
    assert(strcmp(receiver.last_type, "HELLO") == 0);

    conn_close(&c);
    close(sv[1]);
}

static void test_fragmented_read(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(set_nonblocking(sv[0]) == 0);
    assert(set_nonblocking(sv[1]) == 0);

    Conn c;
    assert(conn_init(&c, sv[0], 0, 1) == OK);

    /* Encode two envelopes into a buffer */
    struct json_object *p1 = json_object_new_object();
    Envelope e1 = {1, 1, "PING", p1};
    uint8_t out1[256];
    size_t len1 = 0;
    assert(frame_encode(&e1, out1, sizeof(out1), &len1) == OK);
    json_object_put(p1);

    struct json_object *p2 = json_object_new_object();
    json_object_object_add(p2, "val", json_object_new_int(99));
    Envelope e2 = {1, 2, "HELLO", p2};
    uint8_t out2[256];
    size_t len2 = 0;
    assert(frame_encode(&e2, out2, sizeof(out2), &len2) == OK);
    json_object_put(p2);

    uint8_t combined[512];
    memcpy(combined, out1, len1);
    memcpy(combined + len1, out2, len2);
    size_t total_len = len1 + len2;

    TestReceiver receiver = {0};

    /* Send in irregular chunk sizes: 1, 3, 2, 7... bytes */
    size_t sent = 0;
    size_t chunk_sizes[] = {1, 3, 2, 7, 5, 4, 11, 1, 6};
    size_t chunk_idx = 0;
    while (sent < total_len) {
        size_t chunk = chunk_sizes[chunk_idx % (sizeof(chunk_sizes) / sizeof(chunk_sizes[0]))];
        if (chunk > total_len - sent) chunk = total_len - sent;
        ssize_t n = send(sv[1], combined + sent, chunk, 0);
        assert(n == (ssize_t)chunk);
        sent += chunk;
        chunk_idx++;

        /* Read whatever is available into conn */
        Status s = conn_read(&c, on_frame_received, &receiver);
        assert(s == OK);
    }

    assert(receiver.count == 2);
    assert(receiver.last_id == 2);
    assert(strcmp(receiver.last_type, "HELLO") == 0);

    conn_close(&c);
    close(sv[1]);
}

static void test_slow_consumer_threshold(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(set_nonblocking(sv[0]) == 0);
    assert(set_nonblocking(sv[1]) == 0);

    Conn c;
    assert(conn_init(&c, sv[0], 0, 1) == OK);

    /* Construct payload of ~32KB */
    struct json_object *payload = json_object_new_object();
    char buf[32000];
    memset(buf, 'Z', sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    json_object_object_add(payload, "data", json_object_new_string(buf));
    Envelope env = {1, 1, "PING", payload};

    Status last_status = OK;
    int queued_count = 0;
    for (int i = 0; i < 20; i++) {
        last_status = conn_queue(&c, &env);
        if (last_status != OK) break;
        queued_count++;
    }
    json_object_put(payload);

    /* Output buffer size is 256 KiB = 262144 bytes.
     * Queuing ~32KB repeatedly should trigger RATE_LIMITED before 10 frames. */
    assert(last_status == RATE_LIMITED);
    assert(c.closing == true);
    assert(c.out_len <= CONN_OUT_BUF_SIZE);

    conn_close(&c);
    close(sv[1]);
}

static void test_conn_close_idempotent(void)
{
    Conn c;
    assert(conn_init(&c, -1, 0, 1) == OK);
    conn_close(&c);
    conn_close(&c);
    assert(c.fd == -1);
}

static void test_client_connect_fail(void)
{
    Client *client = NULL;
    /* Connecting to a non-listening port should return error, not hang */
    Status s = client_connect(&client, "127.0.0.1", 1);
    assert(s != OK);
    assert(client == NULL);

    /* Invalid hostname */
    s = client_connect(&client, "this.host.does.not.exist.invalid.local", 9090);
    assert(s != OK);
    assert(client == NULL);
}

static void test_token_bucket_rate_limiter(void)
{
    AppConnSlot slot;
    memset(&slot, 0, sizeof(slot));
    uint64_t now_ms = 1000000;
    slot.rate_last_ms = now_ms;
    slot.rate_tokens = 60.0;

    /* Burst cap: 60 requests in the same millisecond should succeed */
    for (int i = 0; i < 60; i++) {
        assert(app_conn_check_rate_limit(&slot, now_ms) == true);
    }

    /* 61st request in the same millisecond should be rejected */
    assert(app_conn_check_rate_limit(&slot, now_ms) == false);

    /* After 100ms, refill: 100ms * 0.03 = 3 tokens */
    now_ms += 100;
    for (int i = 0; i < 3; i++) {
        assert(app_conn_check_rate_limit(&slot, now_ms) == true);
    }
    /* 4th request after refill should be rejected */
    assert(app_conn_check_rate_limit(&slot, now_ms) == false);

    /* Burst cap clamp: after 10000ms (+300 tokens), should be capped at 60 */
    now_ms += 10000;
    for (int i = 0; i < 60; i++) {
        assert(app_conn_check_rate_limit(&slot, now_ms) == true);
    }
    /* 61st request should be rejected */
    assert(app_conn_check_rate_limit(&slot, now_ms) == false);
}

int main(void)
{
    test_token_bucket_rate_limiter();
    test_partial_write_and_flush();
    test_fragmented_read();
    test_slow_consumer_threshold();
    test_conn_close_idempotent();
    test_client_connect_fail();
    puts("net: OK");
    return 0;
}
