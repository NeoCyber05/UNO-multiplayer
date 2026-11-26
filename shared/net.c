#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum {
    /* One recv() call drains up to this many bytes before frameFeed runs. */
    RECEIVE_BUFFER_SIZE = 16384
};

Status connInit(Conn *connection, int fd, uint32_t slot, uint32_t generation) {
    if (!connection) return BAD_REQUEST;
    connection->fd = fd;
    connection->slot = slot;
    connection->generation = generation;
    connection->outLen = 0;
    connection->outPos = 0;
    connection->closing = false;
    frameReaderReset(&connection->reader);

    /* The poll loop serves every client on one thread, so a blocking read or
     * write on one socket would stall all others. */
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            /* FIXME: fcntl(F_SETFL) failure is ignored, leaving a blocking socket in the
             * poll loop. Return BAD_REQUEST and let the caller close. */
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
    return OK;
}

Status connRead(Conn *connection, FrameCallback onFrame, void *context) {
    if (!connection || connection->fd < 0 || !onFrame) return BAD_REQUEST;
    /* A connection already marked for closing must not dispatch more requests. */
    if (connection->closing) return RATE_LIMITED;

    uint8_t receiveBuffer[RECEIVE_BUFFER_SIZE];
    while (1) {
        ssize_t bytesReceived = recv(connection->fd, receiveBuffer, sizeof(receiveBuffer), 0);
        if (bytesReceived < 0) {
            /* A signal interrupted the call before any data arrived; just retry. */
            if (errno == EINTR) continue;
            /* Non-blocking socket drained: wait for the next POLLIN. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return OK;
            }
            connection->closing = true;
            return BAD_REQUEST;
        }
        if (bytesReceived == 0) {
            /* recv() returns 0 only when the peer has closed its side. */
            connection->closing = true;
            return BAD_REQUEST;
        }
        Status status = frameFeed(&connection->reader, receiveBuffer, (size_t)bytesReceived,
                                   onFrame, context);
        if (status != OK) {
            connection->closing = true;
            return status;
        }
    }
}

Status connQueue(Conn *connection, const Envelope *envelope) {
    if (!connection || connection->fd < 0 || !envelope) return BAD_REQUEST;
    if (connection->closing) return RATE_LIMITED;

    /* Move the unsent tail to the front so the free space is contiguous. */
    if (connection->outPos > 0) {
        if (connection->outLen > connection->outPos) {
            memmove(connection->outBuf, connection->outBuf + connection->outPos,
                    connection->outLen - connection->outPos);
            connection->outLen -= connection->outPos;
        } else {
            connection->outLen = 0;
        }
        connection->outPos = 0;
    }

    uint8_t frameBuffer[FRAME_CAPACITY];
    size_t frameLength = 0;
    Status status = frameEncode(envelope, frameBuffer, sizeof(frameBuffer), &frameLength);
    if (status != OK) return status;

    /* A peer that stops reading must not grow server memory without bound. */
    if (connection->outLen + frameLength > CONN_OUT_BUF_SIZE) {
        connection->closing = true;
        return RATE_LIMITED;
    }

    memcpy(connection->outBuf + connection->outLen, frameBuffer, frameLength);
    connection->outLen += frameLength;
    return OK;
}

Status connFlush(Conn *connection) {
    if (!connection || connection->fd < 0) return BAD_REQUEST;

    while (connection->outLen > connection->outPos) {
        size_t bytesToSend = connection->outLen - connection->outPos;
        /* MSG_NOSIGNAL turns a write to a closed peer into EPIPE instead of SIGPIPE. */
        ssize_t bytesSent = send(connection->fd, connection->outBuf + connection->outPos,
                                 bytesToSend, MSG_NOSIGNAL);
        if (bytesSent < 0) {
            if (errno == EINTR) continue;
            /* Kernel send buffer is full; the rest goes out on the next POLLOUT. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return OK;
            }
            connection->closing = true;
            return BAD_REQUEST;
        }
        if (bytesSent == 0) {
            connection->closing = true;
            return BAD_REQUEST;
        }
        /* A partial send is normal for TCP; keep the position and loop. */
        connection->outPos += (size_t)bytesSent;
    }

    connection->outPos = 0;
    connection->outLen = 0;
    return OK;
}

void connClose(Conn *connection) {
    if (!connection) return;
    if (connection->fd >= 0) {
        close(connection->fd);
        connection->fd = -1;
    }
    connection->outLen = 0;
    connection->outPos = 0;
    connection->closing = false;
    connection->slot = 0;
    connection->generation = 0;
    frameReaderReset(&connection->reader);
}
