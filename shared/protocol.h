#ifndef UNO_PROTOCOL_H
#define UNO_PROTOCOL_H

#include "contracts.h"
#include <json-c/json.h>

/* A frame is a 4-byte big-endian body length followed by that many JSON bytes. */
enum { FRAME_MAX_JSON = 65536, FRAME_CAPACITY = 4 + FRAME_MAX_JSON };

/* Holds at most one partial frame between reads from a stream socket. */
typedef struct {
    uint8_t buffer[FRAME_CAPACITY];
    size_t used;
} FrameReader;

/* One UNOP/1 message: {"v":1,"id":n,"type":"...","payload":{...}}. */
typedef struct {
    int v;
    int32_t id;
    const char *type;
    struct json_object *payload;
} Envelope;

/* Receives a decoded envelope; a non-OK result stops frameFeed. */
typedef Status (*FrameCallback)(const Envelope *, void *);

/**
 * @brief Drop any partial frame, e.g. after a connection is reused.
 * @param[out] reader Reader to clear.
 */
void frameReaderReset(FrameReader *reader);

/**
 * @brief Append received bytes and decode every frame they complete.
 *
 * Bytes may arrive split or coalesced in any way; a frame that is still
 * incomplete stays in @p reader for the next call.
 *
 * @param[in,out] reader Per-connection reassembly buffer.
 * @param[in] data Bytes just received (may be NULL when @p length is 0).
 * @param[in] length Number of bytes in @p data.
 * @param[in] onFrame Called once per complete frame, in order. The envelope and
 *            its payload are owned by the decoder and only valid during the call.
 * @param[in] context Passed through to @p onFrame.
 * @return OK after consuming all bytes; BAD_REQUEST for a zero or oversized
 *         length prefix or an invalid envelope; otherwise the first non-OK
 *         callback result. After an error the connection should be closed.
 */
Status frameFeed(FrameReader *reader, const uint8_t *data, size_t length, FrameCallback onFrame,
                  void *context);

/**
 * @brief Serialize an envelope into one length-prefixed frame.
 * @param[in] envelope Message to encode; the payload is not consumed.
 * @param[out] out Caller-owned buffer.
 * @param[in] capacity Size of @p out; FRAME_CAPACITY always fits a valid frame.
 * @param[out] written Frame size on success, 0 otherwise.
 * @return OK, BAD_REQUEST for an invalid envelope or a frame that does not fit,
 *         or BUSY when json-c cannot allocate.
 */
Status frameEncode(const Envelope *envelope, uint8_t *out, size_t capacity, size_t *written);

/**
 * @brief Map a Status to the wire error code string used in ERROR payloads.
 * @param[in] status Any Status value, including ones outside the enum.
 * @return A static string equal to the enum name (OK -> "OK",
 *         STALE_REQUEST -> "STALE_REQUEST", ...). A value outside the enum
 *         returns "BAD_REQUEST" so a corrupted status never leaks to the wire.
 */
const char *statusCode(Status status);

#endif
