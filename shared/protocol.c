#include "protocol.h"
#include <string.h>

enum {
    /* Mỗi frame mở đầu bằng 4 byte độ dài phần JSON, theo thứ tự big-endian. */
    LENGTH_PREFIX_SIZE = 4,
    /* Độ sâu lồng object/array tối đa mà client được gửi. */
    MAX_JSON_DEPTH = 16,
    /* Số trường bắt buộc của envelope: v, id, type, payload. */
    ENVELOPE_FIELD_COUNT = 4,
    /* Byte nhỏ hơn 0x20 là ký tự điều khiển, JSON cấm xuất hiện thô trong chuỗi. */
    FIRST_PRINTABLE_BYTE = 0x20,
    /* Các mốc byte dẫn của UTF-8 (RFC 3629). */
    UTF8_FIRST_LEAD_BYTE = 0xC0,
    UTF8_MIN_VALID_LEAD = 0xC2,
    UTF8_MAX_VALID_LEAD = 0xF4,
    UTF8_THREE_BYTE_LEAD = 0xE0,
    UTF8_SURROGATE_LEAD = 0xED,
    UTF8_FOUR_BYTE_LEAD = 0xF0,
    /* Ngưỡng byte thứ hai: dưới/trên các mốc này là overlong, surrogate hoặc > U+10FFFF. */
    UTF8_THREE_BYTE_MIN_SECOND = 0xA0,
    UTF8_SURROGATE_MIN_SECOND = 0xA0,
    UTF8_FOUR_BYTE_MIN_SECOND = 0x90,
    UTF8_MAX_LEAD_MAX_SECOND = 0x8F,
    BITS_PER_BYTE = 8
};

/* Chuỗi escape "\u0000" (bỏ dấu '\' đã đọc) mà json-c vẫn chấp nhận. */
static const char NUL_ESCAPE[] = "u0000";
#define NUL_ESCAPE_LENGTH (sizeof(NUL_ESCAPE) - 1)

static bool isDigit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

/* Bỏ qua dãy chữ số liên tiếp bắt đầu tại index, trả về vị trí sau chữ số cuối. */
static size_t skipDigits(const uint8_t *text, size_t index, size_t length) {
    while (index < length && isDigit(text[index])) index++;
    return index;
}

/**
 * @brief Đo độ dài một số JSON nằm ở đầu @p text.
 *
 * json-c 0.17 vẫn nhận số có chữ số 0 ở đầu hoặc phần thập phân rỗng ngay cả ở
 * chế độ strict, nên số được kiểm tra theo ngữ pháp JSON tại đây trước.
 *
 * @param[in] text Dãy byte bắt đầu bằng '-' hoặc một chữ số.
 * @param[in] length Số byte còn lại tính từ @p text.
 * @return Độ dài của số, hoặc 0 khi đó không phải số JSON hợp lệ.
 */
static size_t numberLength(const uint8_t *text, size_t length) {
    size_t i = text[0] == '-';
    if (i == length || !isDigit(text[i])) return 0;
    if (text[i++] == '0') {
        if (i < length && isDigit(text[i])) return 0;
    } else {
        i = skipDigits(text, i, length);
    }
    if (i < length && text[i] == '.') {
        if (++i == length || !isDigit(text[i])) return 0;
        i = skipDigits(text, i, length);
    }
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        if (i < length && (text[i] == '+' || text[i] == '-')) i++;
        if (i == length || !isDigit(text[i])) return 0;
        i = skipDigits(text, i, length);
    }
    return i;
}

/**
 * @brief Kiểm tra byte dẫn UTF-8 và byte kế tiếp của nó.
 *
 * Loại các chuỗi overlong, surrogate và mã vượt U+10FFFF mà json-c bỏ sót.
 *
 * @param[in] lead Byte dẫn (>= 0xC0).
 * @param[in] next Byte ngay sau byte dẫn.
 * @return true khi cặp byte có thể mở đầu một ký tự UTF-8 hợp lệ.
 */
static bool validUtf8Lead(uint8_t lead, uint8_t next) {
    if (lead < UTF8_MIN_VALID_LEAD || lead > UTF8_MAX_VALID_LEAD) return false;
    if (lead == UTF8_THREE_BYTE_LEAD && next < UTF8_THREE_BYTE_MIN_SECOND) return false;
    if (lead == UTF8_SURROGATE_LEAD && next >= UTF8_SURROGATE_MIN_SECOND) return false;
    if (lead == UTF8_FOUR_BYTE_LEAD && next < UTF8_FOUR_BYTE_MIN_SECOND) return false;
    if (lead == UTF8_MAX_VALID_LEAD && next > UTF8_MAX_LEAD_MAX_SECOND) return false;
    return true;
}

/* Trạng thái khi quét thân JSON: đang trong chuỗi, vừa gặp '\' và độ sâu lồng. */
typedef struct {
    bool quoted;
    bool escaped;
    int depth;
} JsonScan;

/**
 * @brief Xử lý một byte nằm bên trong chuỗi JSON.
 * @return false khi gặp ký tự điều khiển thô hoặc escape "\u0000".
 */
static bool scanQuotedByte(JsonScan *scan, const uint8_t *body, size_t index, size_t length) {
    uint8_t byte = body[index];
    if (byte < FIRST_PRINTABLE_BYTE) return false;
    if (scan->escaped) {
        if (length - index >= NUL_ESCAPE_LENGTH &&
            memcmp(body + index, NUL_ESCAPE, NUL_ESCAPE_LENGTH) == 0) {
            return false;
        }
        scan->escaped = false;
    } else if (byte == '\\') {
        scan->escaped = true;
    } else if (byte == '"') {
        scan->quoted = false;
    }
    return true;
}

/**
 * @brief Xử lý một byte nằm ngoài chuỗi JSON.
 * @param[in,out] index Vị trí byte; được đẩy tới cuối số khi gặp một số.
 * @return false khi lồng quá sâu, số sai ngữ pháp, hoặc gặp NaN/Infinity/NUL.
 */
static bool scanPlainByte(JsonScan *scan, const uint8_t *body, size_t *index, size_t length) {
    uint8_t byte = body[*index];
    if (byte == '"') {
        scan->quoted = true;
    } else if (byte == '{' || byte == '[') {
        if (++scan->depth > MAX_JSON_DEPTH) return false;
    } else if (byte == '}' || byte == ']') {
        scan->depth--;
    } else if (byte == '-' || isDigit(byte)) {
        size_t width = numberLength(body + *index, length - *index);
        if (!width) return false;
        *index += width - 1;
    } else if (byte == 0 || byte == 'N' || byte == 'I') {
        /* json-c nhận NaN và Infinity kể cả khi bật JSON_TOKENER_STRICT. */
        return false;
    }
    return true;
}

/**
 * @brief Chặn các mẫu byte mà chế độ strict của json-c vẫn để lọt.
 *
 * json-c vẫn nhận ký tự điều khiển thô, "\u0000", NaN/Infinity, container rỗng
 * sâu hơn một mức so với scalar sâu nhất, cùng vài chuỗi UTF-8 overlong hoặc
 * surrogate.
 *
 * @param[in] body Thân frame, không gồm tiền tố độ dài.
 * @param[in] length Kích thước thân frame tính bằng byte.
 * @return true khi thân frame có thể giao cho json-c.
 */
static bool validBodyBytes(const uint8_t *body, size_t length) {
    JsonScan scan = {false, false, 0};
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = body[i];
        bool accepted = scan.quoted ? scanQuotedByte(&scan, body, i, length)
                                    : scanPlainByte(&scan, body, &i, length);
        if (!accepted) return false;
        if (byte < UTF8_FIRST_LEAD_BYTE) continue;
        if (i + 1 == length || !validUtf8Lead(byte, body[i + 1])) return false;
    }
    return true;
}

/**
 * @brief Kiểm tra bốn trường của envelope sau khi json-c đã parse.
 * @return true khi v == PROTOCOL_VERSION, 0 <= id <= INT32_MAX, type là chuỗi
 *         khác rỗng không chứa NUL, và payload là object.
 */
static bool validEnvelopeFields(struct json_object *version, struct json_object *id,
                                struct json_object *type, struct json_object *payload) {
    if (!json_object_is_type(version, json_type_int) ||
        json_object_get_int64(version) != PROTOCOL_VERSION) {
        return false;
    }
    if (!json_object_is_type(id, json_type_int) || json_object_get_int64(id) < 0 ||
        json_object_get_int64(id) > INT32_MAX) {
        return false;
    }
    if (!json_object_is_type(type, json_type_string) || json_object_get_string_len(type) <= 0) {
        return false;
    }
    /* strlen ngắn hơn độ dài json-c báo nghĩa là type có NUL nhúng bên trong. */
    size_t typeLength = (size_t)json_object_get_string_len(type);
    if (strlen(json_object_get_string(type)) != typeLength) return false;
    return json_object_is_type(payload, json_type_object);
}

/* Parse JSON ở chế độ strict; trả NULL khi lỗi hoặc còn byte thừa sau object. */
static struct json_object *parseStrictJson(const uint8_t *body, size_t length, bool *outBusy) {
    *outBusy = false;
    /* json-c cần thêm một mức stack cho scalar nằm ở độ sâu tối đa. */
    struct json_tokener *tokener = json_tokener_new_ex(MAX_JSON_DEPTH + 1);
    if (!tokener) {
        *outBusy = true;
        return NULL;
    }
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *root = json_tokener_parse_ex(tokener, (const char *)body, (int)length);
    bool parsed = json_tokener_get_error(tokener) == json_tokener_success &&
                  json_tokener_get_parse_end(tokener) == length;
    json_tokener_free(tokener);
    if (!parsed) {
        json_object_put(root);
        return NULL;
    }
    return root;
}

/**
 * @brief Parse thân một frame và giao envelope cho @p onFrame.
 * @param[in] body Các byte JSON; không được rỗng.
 * @param[in] length Kích thước thân frame tính bằng byte.
 * @param[in] onFrame Nhận envelope, hoặc NULL nếu chỉ cần kiểm tra hợp lệ.
 * @param[in] context Chuyển nguyên cho @p onFrame.
 * @return BAD_REQUEST khi envelope sai, BUSY khi json-c không cấp phát được,
 *         còn lại là kết quả của callback (OK khi @p onFrame là NULL).
 *         Envelope và payload chỉ còn hợp lệ trong lúc callback chạy.
 */
static Status decode(const uint8_t *body, size_t length, FrameCallback onFrame, void *context) {
    if (body[length - 1] != '}' || !validBodyBytes(body, length)) return BAD_REQUEST;

    bool busy = false;
    struct json_object *root = parseStrictJson(body, length, &busy);
    if (busy) return BUSY;

    Status status = BAD_REQUEST;
    if (root && json_object_is_type(root, json_type_object)) {
        struct json_object *version = json_object_object_get(root, "v");
        struct json_object *id = json_object_object_get(root, "id");
        struct json_object *type = json_object_object_get(root, "type");
        struct json_object *payload = json_object_object_get(root, "payload");
        if (validEnvelopeFields(version, id, type, payload)) {
            Envelope envelope = {PROTOCOL_VERSION, json_object_get_int(id),
                                 json_object_get_string(type), payload};
            status = onFrame ? onFrame(&envelope, context) : OK;
        }
    }
    json_object_put(root);
    return status;
}

/* Đọc tiền tố độ dài big-endian (network byte order), không phụ thuộc máy. */
static uint32_t readLengthPrefix(const uint8_t prefix[LENGTH_PREFIX_SIZE]) {
    uint32_t length = 0;
    for (size_t i = 0; i < LENGTH_PREFIX_SIZE; i++) {
        length = (length << BITS_PER_BYTE) | prefix[i];
    }
    return length;
}

/* Ghi độ dài theo big-endian để mọi client đọc giống nhau bất kể kiến trúc. */
static void writeLengthPrefix(uint8_t prefix[LENGTH_PREFIX_SIZE], uint32_t length) {
    /* Ghi từ byte cuối về đầu: byte thấp nhất nằm cuối cùng. */
    for (size_t i = LENGTH_PREFIX_SIZE; i-- > 0;) {
        prefix[i] = (uint8_t)length;
        length >>= BITS_PER_BYTE;
    }
}

/**
 * @brief Chép byte vừa nhận vào reader cho tới khi đủ @p target byte.
 * @param[in,out] data Con trỏ dữ liệu, được đẩy qua phần đã chép.
 * @param[in,out] length Số byte còn lại, được trừ đi phần đã chép.
 * @return true khi reader đã đủ @p target byte.
 */
static bool fillReader(FrameReader *reader, const uint8_t **data, size_t *length, size_t target) {
    size_t take = target - reader->used;
    if (take > *length) take = *length;
    memcpy(reader->buffer + reader->used, *data, take);
    reader->used += take;
    *data += take;
    *length -= take;
    return reader->used == target;
}

void frameReaderReset(FrameReader *reader) {
    reader->used = 0;
}

Status frameFeed(FrameReader *reader, const uint8_t *data, size_t length, FrameCallback onFrame,
                 void *context) {
    if (!reader || (!data && length) || !onFrame) return BAD_REQUEST;
    while (length) {
        /* TCP có thể cắt đôi chính tiền tố độ dài, nên tiền tố cũng được gom dần. */
        if (reader->used < LENGTH_PREFIX_SIZE &&
            !fillReader(reader, &data, &length, LENGTH_PREFIX_SIZE)) {
            return OK;
        }
        uint32_t bodyLength = readLengthPrefix(reader->buffer);
        if (!bodyLength || bodyLength > FRAME_MAX_JSON) return BAD_REQUEST;
        if (!fillReader(reader, &data, &length, LENGTH_PREFIX_SIZE + bodyLength)) continue;
        reader->used = 0;
        Status status = decode(reader->buffer + LENGTH_PREFIX_SIZE, bodyLength, onFrame, context);
        if (status != OK) return status;
    }
    return OK;
}

/* Envelope phải có v đúng phiên bản, id không âm, type khác rỗng, payload là object. */
static bool encodableEnvelope(const Envelope *envelope) {
    return envelope && envelope->v == PROTOCOL_VERSION && envelope->id >= 0 && envelope->type &&
           *envelope->type && json_object_is_type(envelope->payload, json_type_object);
}

/**
 * @brief Dựng object JSON {"v","id","type","payload"} cho một envelope.
 * @return Object mới mà caller phải json_object_put, hoặc NULL khi hết bộ nhớ.
 */
static struct json_object *buildEnvelopeJson(const Envelope *envelope) {
    struct json_object *root = json_object_new_object();
    if (!root) return NULL;
    /* json_object_get tăng reference để root không giải phóng payload của caller. */
    struct json_object *fields[ENVELOPE_FIELD_COUNT] = {
        json_object_new_int(PROTOCOL_VERSION), json_object_new_int(envelope->id),
        json_object_new_string(envelope->type), json_object_get(envelope->payload)};
    const char *keys[ENVELOPE_FIELD_COUNT] = {"v", "id", "type", "payload"};
    for (size_t i = 0; i < ENVELOPE_FIELD_COUNT; i++) {
        if (!fields[i] || json_object_object_add(root, keys[i], fields[i]) < 0) {
            /* Các trường chưa gắn vào root vẫn thuộc về hàm này nên phải tự giải phóng. */
            for (; i < ENVELOPE_FIELD_COUNT; i++) json_object_put(fields[i]);
            json_object_put(root);
            return NULL;
        }
    }
    return root;
}

Status frameEncode(const Envelope *envelope, uint8_t *out, size_t capacity, size_t *written) {
    if (!written) return BAD_REQUEST;
    *written = 0;
    if (!out || capacity < LENGTH_PREFIX_SIZE || !encodableEnvelope(envelope)) return BAD_REQUEST;

    struct json_object *root = buildEnvelopeJson(envelope);
    if (!root) return BUSY;
    size_t jsonLength = 0;
    const char *json = json_object_to_json_string_length(root, JSON_C_TO_STRING_PLAIN, &jsonLength);
    Status status = BUSY;
    if (json) {
        bool fits = jsonLength > 0 && jsonLength <= FRAME_MAX_JSON &&
                    jsonLength <= capacity - LENGTH_PREFIX_SIZE;
        /* Cho thân vừa mã hóa chạy lại qua bộ giải mã để server không bao giờ gửi
         * frame mà một client tuân thủ cùng quy tắc sẽ từ chối. */
        status = fits ? decode((const uint8_t *)json, jsonLength, NULL, NULL) : BAD_REQUEST;
    }
    if (status == OK) {
        writeLengthPrefix(out, (uint32_t)jsonLength);
        memcpy(out + LENGTH_PREFIX_SIZE, json, jsonLength);
        *written = LENGTH_PREFIX_SIZE + jsonLength;
    }
    json_object_put(root);
    return status;
}

/* Đánh index theo Status; static assert giữ bảng này khớp với contracts.h. */
static const char *const STATUS_NAMES[] = {
    "OK", "BAD_REQUEST", "UNAUTHENTICATED", "FORBIDDEN", "BUSY", "NOT_FOUND",
    "ROOM_FULL", "INVALID_ACTION", "NOT_YOUR_TURN", "STALE_TURN",
    "STALE_REQUEST", "RATE_LIMITED", "STORAGE_ERROR", "SERVER_FULL"
};
_Static_assert(sizeof STATUS_NAMES / sizeof STATUS_NAMES[0] == SERVER_FULL + 1,
               "STATUS_NAMES must list every Status in enum order");

const char *statusCode(Status status) {
    size_t statusCount = sizeof STATUS_NAMES / sizeof STATUS_NAMES[0];
    if ((size_t)status >= statusCount) return "BAD_REQUEST";
    return STATUS_NAMES[status];
}
