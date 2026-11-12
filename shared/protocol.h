#ifndef UNO_PROTOCOL_H
#define UNO_PROTOCOL_H

#include "contracts.h"
#include <json-c/json.h>

enum { FRAME_MAX_JSON = 65536, FRAME_CAPACITY = 4 + FRAME_MAX_JSON };

typedef struct {
    uint8_t buffer[FRAME_CAPACITY];
    size_t used;
} FrameReader;

typedef struct {
    int v;
    int32_t id;
    const char *type;
    struct json_object *payload;
} Envelope;

typedef Status (*FrameCallback)(const Envelope *, void *);

void frame_reader_reset(FrameReader *r);
Status frame_feed(FrameReader *r, const uint8_t *data, size_t len, FrameCallback cb, void *ctx);
Status frame_encode(const Envelope *e, uint8_t *out, size_t cap, size_t *written);

#endif
