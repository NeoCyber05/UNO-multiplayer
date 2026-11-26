#include "shared/net.h"
#include "desktop/tcp_client.h"
#include "shared/protocol.h"
#include "server/app.h"
#include <assert.h>
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    assert(flags >= 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

typedef struct {
    int count;
    int lastId;
    char lastType[32];
} TestReceiver;

static Status onFrameReceived(const Envelope *envelope, void *context) {
    TestReceiver *receiver = (TestReceiver *)context;
    receiver->count++;
    receiver->lastId = envelope->id;
    strncpy(receiver->lastType, envelope->type, sizeof(receiver->lastType) - 1);
    receiver->lastType[sizeof(receiver->lastType) - 1] = '\0';
    return OK;
}

static void testPartialWriteAndFlush(void) {
    int socketPair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, socketPair) == 0);
    assert(setNonBlocking(socketPair[0]) == 0);
    assert(setNonBlocking(socketPair[1]) == 0);

    /* Set small send buffer on socketPair[0] and small recv buffer on socketPair[1] */
    int bufferSize = 2048;
    assert(setsockopt(socketPair[0], SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) == 0);
    assert(setsockopt(socketPair[1], SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)) == 0);

    Conn connection;
    assert(conn_init(&connection, socketPair[0], 0, 1) == OK);

    /* Construct an envelope with ~8KB payload to force partial write */
    struct json_object *payload = json_object_new_object();
    char bigString[8192];
    memset(bigString, 'X', sizeof(bigString) - 1);
    bigString[sizeof(bigString) - 1] = '\0';
    json_object_object_add(payload, "bulk", json_object_new_string(bigString));

    Envelope envelope = {1, 42, "HELLO", payload};
    assert(conn_queue(&connection, &envelope) == OK);
    json_object_put(payload);

    assert(connection.out_len > 8000);
    assert(connection.out_pos == 0);

    /* Flush once: should write until kernel buffer is full and hit EAGAIN */
    Status status = conn_flush(&connection);
    assert(status == OK);
    assert(connection.out_pos > 0);
    assert(connection.out_pos < connection.out_len);

    /* Read some bytes from socketPair[1] */
    uint8_t receiveBuffer[16384];
    size_t totalReceived = 0;
    while (1) {
        ssize_t byteCount = recv(socketPair[1], receiveBuffer + totalReceived,
                                 sizeof(receiveBuffer) - totalReceived, 0);
        if (byteCount < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            assert(0 && "recv error");
        }
        if (byteCount == 0) break;
        totalReceived += (size_t)byteCount;
    }
    assert(totalReceived == connection.out_pos);

    /* Flush again to send remaining data */
    status = conn_flush(&connection);
    assert(status == OK);

    /* If still remaining, drain and flush until fully sent */
    while (connection.out_pos < connection.out_len) {
        ssize_t byteCount = recv(socketPair[1], receiveBuffer + totalReceived,
                                 sizeof(receiveBuffer) - totalReceived, 0);
        if (byteCount > 0) totalReceived += (size_t)byteCount;
        status = conn_flush(&connection);
        assert(status == OK);
    }
    assert(connection.out_len == 0);
    assert(connection.out_pos == 0);

    /* Drain any remaining bytes from receiver */
    while (1) {
        ssize_t byteCount = recv(socketPair[1], receiveBuffer + totalReceived,
                                 sizeof(receiveBuffer) - totalReceived, 0);
        if (byteCount <= 0) break;
        totalReceived += (size_t)byteCount;
    }

    /* Verify receiver got the full envelope */
    FrameReader reader;
    frame_reader_reset(&reader);
    TestReceiver receiver = {0};
    assert(frame_feed(&reader, receiveBuffer, totalReceived, onFrameReceived, &receiver) == OK);
    assert(receiver.count == 1);
    assert(receiver.lastId == 42);
    assert(strcmp(receiver.lastType, "HELLO") == 0);

    conn_close(&connection);
    close(socketPair[1]);
}

static void testFragmentedRead(void) {
    int socketPair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, socketPair) == 0);
    assert(setNonBlocking(socketPair[0]) == 0);
    assert(setNonBlocking(socketPair[1]) == 0);

    Conn connection;
    assert(conn_init(&connection, socketPair[0], 0, 1) == OK);

    /* Encode two envelopes into a buffer */
    struct json_object *firstPayload = json_object_new_object();
    Envelope firstEnvelope = {1, 1, "PING", firstPayload};
    uint8_t firstFrame[256];
    size_t firstLength = 0;
    assert(frame_encode(&firstEnvelope, firstFrame, sizeof(firstFrame), &firstLength) == OK);
    json_object_put(firstPayload);

    struct json_object *secondPayload = json_object_new_object();
    json_object_object_add(secondPayload, "val", json_object_new_int(99));
    Envelope secondEnvelope = {1, 2, "HELLO", secondPayload};
    uint8_t secondFrame[256];
    size_t secondLength = 0;
    assert(frame_encode(&secondEnvelope, secondFrame, sizeof(secondFrame), &secondLength) == OK);
    json_object_put(secondPayload);

    uint8_t combined[512];
    memcpy(combined, firstFrame, firstLength);
    memcpy(combined + firstLength, secondFrame, secondLength);
    size_t totalLength = firstLength + secondLength;

    TestReceiver receiver = {0};

    /* Send in irregular chunk sizes: 1, 3, 2, 7... bytes */
    size_t sentTotal = 0;
    size_t chunkSizes[] = {1, 3, 2, 7, 5, 4, 11, 1, 6};
    size_t chunkIndex = 0;
    while (sentTotal < totalLength) {
        size_t chunk = chunkSizes[chunkIndex % (sizeof(chunkSizes) / sizeof(chunkSizes[0]))];
        if (chunk > totalLength - sentTotal) chunk = totalLength - sentTotal;
        ssize_t byteCount = send(socketPair[1], combined + sentTotal, chunk, 0);
        assert(byteCount == (ssize_t)chunk);
        sentTotal += chunk;
        chunkIndex++;

        /* Read whatever is available into conn */
        Status status = conn_read(&connection, onFrameReceived, &receiver);
        assert(status == OK);
    }

    assert(receiver.count == 2);
    assert(receiver.lastId == 2);
    assert(strcmp(receiver.lastType, "HELLO") == 0);

    conn_close(&connection);
    close(socketPair[1]);
}

static void testSlowConsumerThreshold(void) {
    int socketPair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, socketPair) == 0);
    assert(setNonBlocking(socketPair[0]) == 0);
    assert(setNonBlocking(socketPair[1]) == 0);

    Conn connection;
    assert(conn_init(&connection, socketPair[0], 0, 1) == OK);

    /* Construct payload of ~32KB */
    struct json_object *payload = json_object_new_object();
    char largeText[32000];
    memset(largeText, 'Z', sizeof(largeText) - 1);
    largeText[sizeof(largeText) - 1] = '\0';
    json_object_object_add(payload, "data", json_object_new_string(largeText));
    Envelope envelope = {1, 1, "PING", payload};

    Status lastStatus = OK;
    int queuedCount = 0;
    for (int i = 0; i < 20; i++) {
        lastStatus = conn_queue(&connection, &envelope);
        if (lastStatus != OK) break;
        queuedCount++;
    }
    json_object_put(payload);

    /* Output buffer size is 256 KiB = 262144 bytes.
     * Queuing ~32KB repeatedly should trigger RATE_LIMITED before 10 frames. */
    assert(lastStatus == RATE_LIMITED);
    assert(connection.closing == true);
    assert(connection.out_len <= CONN_OUT_BUF_SIZE);

    conn_close(&connection);
    close(socketPair[1]);
}

static void testConnCloseIdempotent(void) {
    Conn connection;
    assert(conn_init(&connection, -1, 0, 1) == OK);
    conn_close(&connection);
    conn_close(&connection);
    assert(connection.fd == -1);
}

static void testClientConnectFail(void) {
    Client *client = NULL;
    /* Connecting to a non-listening port should return error, not hang */
    Status status = client_connect(&client, "127.0.0.1", 1);
    assert(status != OK);
    assert(client == NULL);

    /* Invalid hostname */
    status = client_connect(&client, "this.host.does.not.exist.invalid.local", 9090);
    assert(status != OK);
    assert(client == NULL);
}

static void testClientResolve(void) {
    ClientAddress address;
    assert(client_resolve(NULL, 9090, &address) == BAD_REQUEST);
    assert(client_resolve("127.0.0.1", 0, &address) == BAD_REQUEST);
    assert(client_resolve("127.0.0.1", 9090, NULL) == BAD_REQUEST);

    /* A numeric address resolves without DNS. */
    assert(client_resolve("127.0.0.1", 9090, &address) == OK);
    assert(address.length > 0);

    assert(client_resolve("this.host.does.not.exist.invalid.local", 9090, &address) == NOT_FOUND);
}

/* Listening socket on an ephemeral loopback port; returns the fd. */
static int openListener(uint16_t *outPort) {
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenFd >= 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(listenFd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listenFd, 4) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listenFd, (struct sockaddr *)&address, &length) == 0);
    *outPort = ntohs(address.sin_port);
    return listenFd;
}

/* Wait for a pending connect the way the GTK host does: writable or error. */
static void waitWritable(int fd) {
    struct pollfd pollFd = {fd, POLLOUT, 0};
    assert(poll(&pollFd, 1, 2000) == 1);
}

static void testClientConnectStartFinish(void) {
    int fd = -1;
    bool inProgress = false;
    Client *client = NULL;
    assert(client_connect_start(NULL, &fd, &inProgress) == BAD_REQUEST);
    assert(client_connect_finish(-1, &client) == BAD_REQUEST);
    assert(client == NULL);

    uint16_t port = 0;
    int listenFd = openListener(&port);
    ClientAddress address;
    assert(client_resolve("127.0.0.1", port, &address) == OK);

    /* Success: the start never blocks; finish turns the fd into a Client. */
    assert(client_connect_start(&address, &fd, &inProgress) == OK);
    assert(fd >= 0);
    assert((fcntl(fd, F_GETFL, 0) & O_NONBLOCK) != 0);
    if (inProgress) {
        waitWritable(fd);
    }
    assert(client_connect_finish(fd, &client) == OK);
    assert(client != NULL);
    assert(client_fd(client) == fd);
    client_close(client);
    client = NULL;

    /* Refused: either start fails at once or finish reports SO_ERROR. */
    close(listenFd);
    Status started = client_connect_start(&address, &fd, &inProgress);
    if (started == OK) {
        assert(inProgress);
        waitWritable(fd);
        assert(client_connect_finish(fd, &client) != OK);
        assert(client == NULL);
    } else {
        assert(started == BAD_REQUEST);
    }

    /* Abort closes a pending attempt's fd. */
    listenFd = openListener(&port);
    assert(client_resolve("127.0.0.1", port, &address) == OK);
    assert(client_connect_start(&address, &fd, &inProgress) == OK);
    client_connect_abort(fd);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    close(listenFd);
}

static void testTokenBucketRateLimiter(void) {
    AppConnSlot slot;
    memset(&slot, 0, sizeof(slot));
    uint64_t nowMs = 1000000;
    slot.rate_last_ms = nowMs;
    slot.rate_tokens = 60.0;

    /* Burst cap: 60 requests in the same millisecond should succeed */
    for (int i = 0; i < 60; i++) {
        assert(app_conn_check_rate_limit(&slot, nowMs) == true);
    }

    /* 61st request in the same millisecond should be rejected */
    assert(app_conn_check_rate_limit(&slot, nowMs) == false);

    /* After 100ms, refill: 100ms * 0.03 = 3 tokens */
    nowMs += 100;
    for (int i = 0; i < 3; i++) {
        assert(app_conn_check_rate_limit(&slot, nowMs) == true);
    }
    /* 4th request after refill should be rejected */
    assert(app_conn_check_rate_limit(&slot, nowMs) == false);

    /* Burst cap clamp: after 10000ms (+300 tokens), should be capped at 60 */
    nowMs += 10000;
    for (int i = 0; i < 60; i++) {
        assert(app_conn_check_rate_limit(&slot, nowMs) == true);
    }
    /* 61st request should be rejected */
    assert(app_conn_check_rate_limit(&slot, nowMs) == false);
}

/* Copy of the one envelope a rejected client receives; the decoder frees its own. */
typedef struct {
    int count;
    int version;
    int32_t id;
    char type[16];
    char code[32];
    char message[64];
} RejectFrame;

static void copyPayloadString(json_object *payload, const char *key, char *out, size_t size) {
    json_object *value = NULL;
    assert(json_object_object_get_ex(payload, key, &value));
    snprintf(out, size, "%s", json_object_get_string(value));
}

static Status onRejectFrame(const Envelope *envelope, void *context) {
    RejectFrame *frame = (RejectFrame *)context;
    frame->count++;
    frame->version = envelope->v;
    frame->id = envelope->id;
    snprintf(frame->type, sizeof(frame->type), "%s", envelope->type);
    copyPayloadString(envelope->payload, "code", frame->code, sizeof(frame->code));
    copyPayloadString(envelope->payload, "message", frame->message, sizeof(frame->message));
    return OK;
}

static void testRejectConnectionSendsServerFull(void) {
    int socketPair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, socketPair) == 0);
    assert(appRejectConnection(socketPair[0]) == OK);
    /* The call owns the socket and must have closed it. */
    assert(fcntl(socketPair[0], F_GETFD) == -1 && errno == EBADF);

    /* Read until EOF: the server side is closed, so recv() ends with 0. */
    uint8_t receiveBuffer[512];
    size_t totalReceived = 0;
    while (totalReceived < sizeof(receiveBuffer)) {
        ssize_t byteCount = recv(socketPair[1], receiveBuffer + totalReceived,
                                 sizeof(receiveBuffer) - totalReceived, 0);
        assert(byteCount >= 0);
        if (byteCount == 0) break;
        totalReceived += (size_t)byteCount;
    }

    FrameReader reader;
    frame_reader_reset(&reader);
    RejectFrame frame = {0};
    assert(frame_feed(&reader, receiveBuffer, totalReceived, onRejectFrame, &frame) == OK);
    assert(reader.used == 0);
    assert(frame.count == 1);
    assert(frame.version == 1 && frame.id == 0);
    assert(strcmp(frame.type, "ERROR") == 0);
    assert(strcmp(frame.code, "SERVER_FULL") == 0);
    assert(strcmp(frame.message, "Server connection limit reached") == 0);
    close(socketPair[1]);
}

static void testRejectConnectionReportsBrokenPeer(void) {
    int socketPair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, socketPair) == 0);
    close(socketPair[1]);
    /* MSG_NOSIGNAL: EPIPE comes back as an error instead of killing the test. */
    assert(appRejectConnection(socketPair[0]) == BAD_REQUEST);
    assert(fcntl(socketPair[0], F_GETFD) == -1 && errno == EBADF);
}

int main(void) {
    testTokenBucketRateLimiter();
    testPartialWriteAndFlush();
    testFragmentedRead();
    testSlowConsumerThreshold();
    testConnCloseIdempotent();
    testClientConnectFail();
    testClientResolve();
    testClientConnectStartFinish();
    testRejectConnectionSendsServerFull();
    testRejectConnectionReportsBrokenPeer();
    puts("net: OK");
    return 0;
}
