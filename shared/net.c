#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

Status conn_init(Conn *c, int fd, uint32_t slot, uint32_t generation)
{
    if (!c) return BAD_REQUEST;
    c->fd = fd;
    c->slot = slot;
    c->generation = generation;
    c->out_len = 0;
    c->out_pos = 0;
    c->closing = false;
    frame_reader_reset(&c->reader);

    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
    return OK;
}

Status conn_read(Conn *c, FrameCallback cb, void *ctx)
{
    if (!c || c->fd < 0 || !cb) return BAD_REQUEST;
    if (c->closing) return RATE_LIMITED;

    uint8_t buf[16384];
    while (1) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return OK;
            }
            c->closing = true;
            return BAD_REQUEST;
        }
        if (n == 0) {
            c->closing = true;
            return BAD_REQUEST;
        }
        Status s = frame_feed(&c->reader, buf, (size_t)n, cb, ctx);
        if (s != OK) {
            c->closing = true;
            return s;
        }
    }
}

Status conn_queue(Conn *c, const Envelope *e)
{
    if (!c || c->fd < 0 || !e) return BAD_REQUEST;
    if (c->closing) return RATE_LIMITED;

    if (c->out_pos > 0) {
        if (c->out_len > c->out_pos) {
            memmove(c->out_buf, c->out_buf + c->out_pos, c->out_len - c->out_pos);
            c->out_len -= c->out_pos;
        } else {
            c->out_len = 0;
        }
        c->out_pos = 0;
    }

    uint8_t tmp[FRAME_CAPACITY];
    size_t written = 0;
    Status s = frame_encode(e, tmp, sizeof(tmp), &written);
    if (s != OK) return s;

    if (c->out_len + written > CONN_OUT_BUF_SIZE) {
        c->closing = true;
        return RATE_LIMITED;
    }

    memcpy(c->out_buf + c->out_len, tmp, written);
    c->out_len += written;
    return OK;
}

Status conn_flush(Conn *c)
{
    if (!c || c->fd < 0) return BAD_REQUEST;

    while (c->out_len > c->out_pos) {
        size_t to_send = c->out_len - c->out_pos;
        ssize_t sent = send(c->fd, c->out_buf + c->out_pos, to_send, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return OK;
            }
            c->closing = true;
            return BAD_REQUEST;
        }
        if (sent == 0) {
            c->closing = true;
            return BAD_REQUEST;
        }
        c->out_pos += (size_t)sent;
    }

    c->out_pos = 0;
    c->out_len = 0;
    return OK;
}

void conn_close(Conn *c)
{
    if (!c) return;
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->out_len = 0;
    c->out_pos = 0;
    c->closing = false;
    c->slot = 0;
    c->generation = 0;
    frame_reader_reset(&c->reader);
}
