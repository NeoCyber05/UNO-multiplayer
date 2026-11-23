#ifndef UNO_NET_H
#define UNO_NET_H

#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    CONN_OUT_BUF_SIZE = 262144 /* 256 KiB */
};

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

Status conn_init(Conn *c, int fd, uint32_t slot, uint32_t generation);
Status conn_read(Conn *c, FrameCallback cb, void *ctx);
Status conn_queue(Conn *c, const Envelope *e);
Status conn_flush(Conn *c);
void conn_close(Conn *c);

#endif
