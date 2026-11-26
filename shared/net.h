#ifndef UNO_NET_H
#define UNO_NET_H

#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    /* Unsent bytes allowed per connection before it counts as a slow consumer. */
    CONN_OUT_BUF_SIZE = 262144 /* 256 KiB */
};

/* One non-blocking TCP stream with its frame reader and pending output. */
typedef struct {
    int fd;
    uint32_t slot;
    uint32_t generation;
    FrameReader reader;
    uint8_t out_buf[CONN_OUT_BUF_SIZE];
    size_t out_len;
    size_t out_pos;
    bool closing;
} Conn;

/**
 * @brief Take ownership of a connected socket and switch it to non-blocking.
 * @param[out] connection Connection to initialize.
 * @param[in] fd Connected socket, or -1 for an unconnected placeholder.
 * @param[in] slot Logical slot index; not the fd.
 * @param[in] generation Slot reuse counter so stale references can be detected.
 * @return OK, or BAD_REQUEST when @p connection is NULL.
 */
Status conn_init(Conn *connection, int fd, uint32_t slot, uint32_t generation);

/**
 * @brief Read until the socket would block and decode every complete frame.
 * @param[in,out] connection Connection with a readable socket.
 * @param[in] onFrame Called for each frame; see frame_feed for envelope lifetime.
 * @param[in] context Passed through to @p onFrame.
 * @return OK once recv() reports EAGAIN/EWOULDBLOCK. BAD_REQUEST on EOF
 *         (recv() returned 0) or a socket error; a framing error or a non-OK
 *         callback result is returned as is; all of these set `closing`.
 *         RATE_LIMITED without reading when the connection is already
 *         `closing`, and BAD_REQUEST for a NULL argument or a closed fd.
 */
Status conn_read(Conn *connection, FrameCallback onFrame, void *context);

/**
 * @brief Encode an envelope and append it to the pending output.
 *
 * Nothing is sent here; call conn_flush or wait for POLLOUT.
 *
 * @param[in,out] connection Destination connection.
 * @param[in] envelope Message to queue; the payload is not consumed.
 * @return OK, an encode error, or RATE_LIMITED when the output buffer would
 *         exceed CONN_OUT_BUF_SIZE (the connection is then marked `closing`).
 */
Status conn_queue(Conn *connection, const Envelope *envelope);

/**
 * @brief Send pending output until it is empty or the socket would block.
 *
 * send() may accept fewer bytes than offered; the rest stays queued and
 * `out_pos` records how much the kernel has accepted so far.
 *
 * @param[in,out] connection Connection with pending output.
 * @return OK when everything was sent or the socket would block (check
 *         `out_len > out_pos` to know whether to wait for POLLOUT).
 *         BAD_REQUEST on a socket error, which also sets `closing`.
 */
Status conn_flush(Conn *connection);

/**
 * @brief Close the socket and reset the connection; safe to call twice.
 * @param[in,out] connection Connection to close; the fd is closed at most once.
 */
void conn_close(Conn *connection);

#endif
