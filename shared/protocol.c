#include "protocol.h"
#include <string.h>

static bool digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

/* json-c 0.17 accepts leading zeros and fractions without digits in strict mode. */
static size_t number_length(const uint8_t *p, size_t n)
{
    size_t i = p[0] == '-';
    if (i == n || !digit(p[i])) return 0;
    if (p[i++] == '0') {
        if (i < n && digit(p[i])) return 0;
    } else {
        while (i < n && digit(p[i])) i++;
    }
    if (i < n && p[i] == '.') {
        if (++i == n || !digit(p[i])) return 0;
        while (i < n && digit(p[i])) i++;
    }
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        if (i < n && (p[i] == '+' || p[i] == '-')) i++;
        if (i == n || !digit(p[i])) return 0;
        while (i < n && digit(p[i])) i++;
    }
    return i;
}

/* json-c's strict mode still accepts raw control characters and empty
 * containers one level deeper than the deepest allowed scalar. */
static bool valid_body_bytes(const uint8_t *p, size_t len)
{
    bool quoted = false, escaped = false;
    int depth = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = p[i];
        if (quoted) {
            if (c < 0x20) return false;
            if (escaped) {
                if (len - i >= 5 && memcmp(p + i, "u0000", 5) == 0) return false;
                escaped = false;
            }
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{' || c == '[') {
            if (++depth > 16) return false;
        } else if (c == '}' || c == ']') {
            depth--;
        } else if (c == '-' || digit(c)) {
            size_t width = number_length(p + i, len - i);
            if (!width) return false;
            i += width - 1;
        } else if (c == 0 || c == 'N' || c == 'I') {
            /* json-c accepts NaN and Infinity even with JSON_TOKENER_STRICT. */
            return false;
        }
        if (c < 0xc0) continue;
        if (i + 1 == len || c < 0xc2 || c > 0xf4) return false;
        uint8_t next = p[i + 1];
        if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) ||
            (c == 0xf0 && next < 0x90) || (c == 0xf4 && next > 0x8f)) return false;
    }
    return true;
}

static Status decode(const uint8_t *body, size_t len, FrameCallback cb, void *ctx)
{
    if (body[len - 1] != '}' || !valid_body_bytes(body, len))
        return BAD_REQUEST;

    /* json-c needs an additional stack entry for a scalar at depth 16. */
    struct json_tokener *tok = json_tokener_new_ex(17);
    if (!tok) return BUSY;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *root = json_tokener_parse_ex(tok, (const char *)body, (int)len);
    bool parsed = json_tokener_get_error(tok) == json_tokener_success &&
                  json_tokener_get_parse_end(tok) == len;
    json_tokener_free(tok);

    Status status = BAD_REQUEST;
    if (parsed && json_object_is_type(root, json_type_object)) {
        struct json_object *v = json_object_object_get(root, "v");
        struct json_object *id = json_object_object_get(root, "id");
        struct json_object *type = json_object_object_get(root, "type");
        struct json_object *payload = json_object_object_get(root, "payload");
        if (json_object_is_type(v, json_type_int) && json_object_get_int64(v) == 1 &&
            json_object_is_type(id, json_type_int) && json_object_get_int64(id) >= 0 &&
            json_object_get_int64(id) <= INT32_MAX && json_object_is_type(type, json_type_string) &&
            json_object_get_string_len(type) > 0 &&
            strlen(json_object_get_string(type)) == (size_t)json_object_get_string_len(type) &&
            json_object_is_type(payload, json_type_object)) {
            Envelope e = {1, json_object_get_int(id), json_object_get_string(type), payload};
            status = cb ? cb(&e, ctx) : OK;
        }
    }
    json_object_put(root);
    return status;
}

void frame_reader_reset(FrameReader *r)
{
    r->used = 0;
}

Status frame_feed(FrameReader *r, const uint8_t *data, size_t len, FrameCallback cb, void *ctx)
{
    if (!r || (!data && len) || !cb) return BAD_REQUEST;
    while (len) {
        if (r->used < 4) {
            size_t take = 4 - r->used;
            if (take > len) take = len;
            memcpy(r->buffer + r->used, data, take);
            r->used += take;
            data += take;
            len -= take;
            if (r->used < 4) return OK;
        }
        const uint8_t *p = r->buffer;
        uint32_t n = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | p[3];
        if (!n || n > FRAME_MAX_JSON) return BAD_REQUEST;
        size_t target = 4 + n;
        size_t take = target - r->used;
        if (take > len) take = len;
        memcpy(r->buffer + r->used, data, take);
        r->used += take;
        data += take;
        len -= take;
        if (r->used != target) continue;
        r->used = 0;
        Status s = decode(r->buffer + 4, n, cb, ctx);
        if (s != OK) return s;
    }
    return OK;
}

Status frame_encode(const Envelope *e, uint8_t *out, size_t cap, size_t *written)
{
    if (!written) return BAD_REQUEST;
    *written = 0;
    if (!e || !out || cap < 4 || e->v != 1 || e->id < 0 || !e->type || !*e->type ||
        !json_object_is_type(e->payload, json_type_object)) return BAD_REQUEST;

    struct json_object *root = json_object_new_object();
    if (!root) return BUSY;
    struct json_object *fields[] = {json_object_new_int(1), json_object_new_int(e->id),
                                    json_object_new_string(e->type), json_object_get(e->payload)};
    const char *keys[] = {"v", "id", "type", "payload"};
    Status s = BUSY;
    for (size_t i = 0; i < 4; i++) {
        if (!fields[i] || json_object_object_add(root, keys[i], fields[i]) < 0) {
            for (; i < 4; i++) json_object_put(fields[i]);
            goto done;
        }
    }
    size_t n = 0;
    const char *json = json_object_to_json_string_length(root, JSON_C_TO_STRING_PLAIN, &n);
    if (!json) goto done;
    s = BAD_REQUEST;
    if (n == 0 || n > FRAME_MAX_JSON || n > cap - 4) goto done;
    s = decode((const uint8_t *)json, n, NULL, NULL);
    if (s == OK) {
        out[0] = (uint8_t)(n >> 24);
        out[1] = (uint8_t)(n >> 16);
        out[2] = (uint8_t)(n >> 8);
        out[3] = (uint8_t)n;
        memcpy(out + 4, json, n);
        *written = 4 + n;
    }
done:
    json_object_put(root);
    return s;
}
