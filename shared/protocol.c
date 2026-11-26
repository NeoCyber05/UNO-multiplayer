#include "protocol.h"
#include <string.h>

enum {
    /* Every frame starts with the JSON body length as a 4-byte big-endian integer. */
    LENGTH_PREFIX_SIZE = 4,
    /* Deepest object/array nesting a client may send. */
    MAX_JSON_DEPTH = 16
};

static bool isDigit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

/**
 * @brief Measure one JSON number at the start of @p text.
 *
 * json-c 0.17 accepts leading zeros and fractions without digits even in strict
 * mode, so numbers are checked against the JSON grammar here first.
 *
 * @param[in] text Bytes starting at '-' or a digit.
 * @param[in] length Bytes available from @p text.
 * @return Length of the number, or 0 when it is not valid JSON.
 */
static size_t numberLength(const uint8_t *text, size_t length) {
    size_t i = text[0] == '-';
    if (i == length || !isDigit(text[i])) return 0;
    if (text[i++] == '0') {
        if (i < length && isDigit(text[i])) return 0;
    } else {
        while (i < length && isDigit(text[i])) i++;
    }
    if (i < length && text[i] == '.') {
        if (++i == length || !isDigit(text[i])) return 0;
        while (i < length && isDigit(text[i])) i++;
    }
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        if (i < length && (text[i] == '+' || text[i] == '-')) i++;
        if (i == length || !isDigit(text[i])) return 0;
        while (i < length && isDigit(text[i])) i++;
    }
    return i;
}

/**
 * @brief Reject byte patterns that json-c's strict mode lets through.
 *
 * json-c still accepts raw control characters, "\u0000", NaN/Infinity, empty
 * containers one level deeper than the deepest allowed scalar, and some
 * overlong or surrogate UTF-8 sequences.
 *
 * @param[in] body Frame body without the length prefix.
 * @param[in] length Body size in bytes.
 * @return true when the body may be handed to json-c.
 */
static bool validBodyBytes(const uint8_t *body, size_t length) {
    bool quoted = false, escaped = false;
    int depth = 0;
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = body[i];
        if (quoted) {
            if (byte < 0x20) return false;
            if (escaped) {
                if (length - i >= 5 && memcmp(body + i, "u0000", 5) == 0) return false;
                escaped = false;
            } else if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                quoted = false;
            }
        } else if (byte == '"') {
            quoted = true;
        } else if (byte == '{' || byte == '[') {
            if (++depth > MAX_JSON_DEPTH) return false;
        } else if (byte == '}' || byte == ']') {
            depth--;
        } else if (byte == '-' || isDigit(byte)) {
            size_t width = numberLength(body + i, length - i);
            if (!width) return false;
            i += width - 1;
        } else if (byte == 0 || byte == 'N' || byte == 'I') {
            /* json-c accepts NaN and Infinity even with JSON_TOKENER_STRICT. */
            return false;
        }
        if (byte < 0xc0) continue;
        if (i + 1 == length || byte < 0xc2 || byte > 0xf4) return false;
        uint8_t next = body[i + 1];
        if ((byte == 0xe0 && next < 0xa0) || (byte == 0xed && next >= 0xa0) ||
            (byte == 0xf0 && next < 0x90) || (byte == 0xf4 && next > 0x8f)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Parse one frame body and hand the envelope to @p onFrame.
 * @param[in] body JSON bytes; must be non-empty.
 * @param[in] length Body size in bytes.
 * @param[in] onFrame Receives the envelope, or NULL to only validate.
 * @param[in] context Passed through to @p onFrame.
 * @return BAD_REQUEST for an invalid envelope, BUSY when json-c cannot allocate,
 *         otherwise the callback's result (OK when @p onFrame is NULL).
 *         The envelope and its payload are only valid during the callback.
 */
static Status decode(const uint8_t *body, size_t length, FrameCallback onFrame, void *context) {
    if (body[length - 1] != '}' || !validBodyBytes(body, length)) return BAD_REQUEST;

    /* json-c needs an additional stack entry for a scalar at the deepest level. */
    struct json_tokener *tokener = json_tokener_new_ex(MAX_JSON_DEPTH + 1);
    if (!tokener) return BUSY;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *root = json_tokener_parse_ex(tokener, (const char *)body, (int)length);
    bool parsed = json_tokener_get_error(tokener) == json_tokener_success &&
                  json_tokener_get_parse_end(tokener) == length;
    json_tokener_free(tokener);

    Status status = BAD_REQUEST;
    if (parsed && json_object_is_type(root, json_type_object)) {
        struct json_object *version = json_object_object_get(root, "v");
        struct json_object *id = json_object_object_get(root, "id");
        struct json_object *type = json_object_object_get(root, "type");
        struct json_object *payload = json_object_object_get(root, "payload");
        if (json_object_is_type(version, json_type_int) && json_object_get_int64(version) == 1 &&
            json_object_is_type(id, json_type_int) && json_object_get_int64(id) >= 0 &&
            json_object_get_int64(id) <= INT32_MAX && json_object_is_type(type, json_type_string) &&
            json_object_get_string_len(type) > 0 &&
            strlen(json_object_get_string(type)) == (size_t)json_object_get_string_len(type) &&
            json_object_is_type(payload, json_type_object)) {
            Envelope envelope = {1, json_object_get_int(id), json_object_get_string(type), payload};
            status = onFrame ? onFrame(&envelope, context) : OK;
        }
    }
    json_object_put(root);
    return status;
}

void frame_reader_reset(FrameReader *reader) {
    reader->used = 0;
}

Status frame_feed(FrameReader *reader, const uint8_t *data, size_t length, FrameCallback onFrame,
                  void *context) {
    if (!reader || (!data && length) || !onFrame) return BAD_REQUEST;
    while (length) {
        /* TCP may split the length prefix itself, so it is buffered byte by byte. */
        if (reader->used < LENGTH_PREFIX_SIZE) {
            size_t take = LENGTH_PREFIX_SIZE - reader->used;
            if (take > length) take = length;
            memcpy(reader->buffer + reader->used, data, take);
            reader->used += take;
            data += take;
            length -= take;
            if (reader->used < LENGTH_PREFIX_SIZE) return OK;
        }
        /* The prefix is big-endian (network byte order) regardless of the host. */
        const uint8_t *prefix = reader->buffer;
        uint32_t bodyLength = ((uint32_t)prefix[0] << 24) | ((uint32_t)prefix[1] << 16) |
                              ((uint32_t)prefix[2] << 8) | prefix[3];
        if (!bodyLength || bodyLength > FRAME_MAX_JSON) return BAD_REQUEST;
        size_t target = LENGTH_PREFIX_SIZE + bodyLength;
        size_t take = target - reader->used;
        if (take > length) take = length;
        memcpy(reader->buffer + reader->used, data, take);
        reader->used += take;
        data += take;
        length -= take;
        if (reader->used != target) continue;
        reader->used = 0;
        Status status = decode(reader->buffer + LENGTH_PREFIX_SIZE, bodyLength, onFrame, context);
        if (status != OK) return status;
    }
    return OK;
}

Status frame_encode(const Envelope *envelope, uint8_t *out, size_t capacity, size_t *written) {
    if (!written) return BAD_REQUEST;
    *written = 0;
    if (!envelope || !out || capacity < LENGTH_PREFIX_SIZE || envelope->v != 1 ||
        envelope->id < 0 || !envelope->type || !*envelope->type ||
        !json_object_is_type(envelope->payload, json_type_object)) {
        return BAD_REQUEST;
    }

    struct json_object *root = json_object_new_object();
    if (!root) return BUSY;
    /* json_object_get adds a reference so root never frees the caller's payload. */
    struct json_object *fields[] = {json_object_new_int(1), json_object_new_int(envelope->id),
                                    json_object_new_string(envelope->type),
                                    json_object_get(envelope->payload)};
    const char *keys[] = {"v", "id", "type", "payload"};
    Status status = BUSY;
    for (size_t i = 0; i < 4; i++) {
        if (!fields[i] || json_object_object_add(root, keys[i], fields[i]) < 0) {
            for (; i < 4; i++) json_object_put(fields[i]);
            goto done;
        }
    }
    size_t jsonLength = 0;
    const char *json = json_object_to_json_string_length(root, JSON_C_TO_STRING_PLAIN, &jsonLength);
    if (!json) goto done;
    status = BAD_REQUEST;
    if (jsonLength == 0 || jsonLength > FRAME_MAX_JSON ||
        jsonLength > capacity - LENGTH_PREFIX_SIZE) {
        goto done;
    }
    /* Run the encoded body through the decoder so the server never sends a frame
     * that a client following the same rules would reject. */
    status = decode((const uint8_t *)json, jsonLength, NULL, NULL);
    if (status == OK) {
        out[0] = (uint8_t)(jsonLength >> 24);
        out[1] = (uint8_t)(jsonLength >> 16);
        out[2] = (uint8_t)(jsonLength >> 8);
        out[3] = (uint8_t)jsonLength;
        memcpy(out + LENGTH_PREFIX_SIZE, json, jsonLength);
        *written = LENGTH_PREFIX_SIZE + jsonLength;
    }
done:
    json_object_put(root);
    return status;
}

/* Indexed by Status; the static assert keeps it in step with contracts.h. */
static const char *const STATUS_NAMES[] = {
    "OK", "BAD_REQUEST", "UNAUTHENTICATED", "FORBIDDEN", "BUSY", "NOT_FOUND",
    "ROOM_FULL", "INVALID_ACTION", "NOT_YOUR_TURN", "STALE_TURN",
    "STALE_REQUEST", "RATE_LIMITED", "STORAGE_ERROR", "SERVER_FULL"
};
_Static_assert(sizeof STATUS_NAMES / sizeof STATUS_NAMES[0] == SERVER_FULL + 1,
               "STATUS_NAMES must list every Status in enum order");

const char *status_code(Status status) {
    size_t statusCount = sizeof STATUS_NAMES / sizeof STATUS_NAMES[0];
    if ((size_t)status >= statusCount) return "BAD_REQUEST";
    return STATUS_NAMES[status];
}
