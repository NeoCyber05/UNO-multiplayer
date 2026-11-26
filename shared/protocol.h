#ifndef UNO_PROTOCOL_H
#define UNO_PROTOCOL_H

#include "shared/contracts.h"
#include <json-c/json.h>

/* Một frame gồm 4 byte độ dài (big-endian) rồi tới đúng chừng ấy byte JSON. */
enum { FRAME_MAX_JSON = 65536, FRAME_CAPACITY = 4 + FRAME_MAX_JSON };

enum {
    /* Giá trị trường "v" của mọi envelope UNOP/1. */
    PROTOCOL_VERSION = 1,
    /* Push từ server luôn mang id 0 để client không ghép nhầm với request nào. */
    PUSH_ENVELOPE_ID = 0
};

/* Giữ tối đa một frame còn dở giữa các lần đọc từ socket dạng luồng. */
typedef struct {
    uint8_t buffer[FRAME_CAPACITY];
    size_t used;
} FrameReader;

/* Một thông điệp UNOP/1: {"v":1,"id":n,"type":"...","payload":{...}}. */
typedef struct {
    int v;
    int32_t id;
    const char *type;
    struct json_object *payload;
} Envelope;

/* Nhận một envelope đã giải mã; trả khác OK thì frameFeed dừng lại. */
typedef Status (*FrameCallback)(const Envelope *, void *);

/**
 * @brief Bỏ frame còn dở, ví dụ khi một kết nối được dùng lại.
 * @param[out] reader Bộ đọc cần xóa trạng thái.
 */
void frameReaderReset(FrameReader *reader);

/**
 * @brief Nối các byte vừa nhận và giải mã mọi frame đã đủ.
 *
 * Byte có thể đến bị cắt nhỏ hoặc dính nhau tùy ý; frame chưa đủ được giữ lại
 * trong @p reader cho lần gọi sau.
 *
 * @param[in,out] reader Bộ đệm ghép frame của riêng từng kết nối.
 * @param[in] data Các byte vừa nhận (được phép NULL khi @p length là 0).
 * @param[in] length Số byte trong @p data.
 * @param[in] onFrame Được gọi đúng thứ tự cho mỗi frame đủ. Envelope và payload
 *            thuộc bộ giải mã và chỉ hợp lệ trong lúc callback chạy.
 * @param[in] context Chuyển nguyên cho @p onFrame.
 * @return OK khi đã dùng hết byte; BAD_REQUEST khi tiền tố độ dài bằng 0 hoặc quá
 *         lớn, hoặc envelope sai; nếu không là kết quả khác OK đầu tiên của
 *         callback. Sau khi lỗi, caller nên đóng kết nối.
 */
Status frameFeed(FrameReader *reader, const uint8_t *data, size_t length, FrameCallback onFrame,
                 void *context);

/**
 * @brief Tuần tự hóa một envelope thành một frame có tiền tố độ dài.
 * @param[in] envelope Thông điệp cần mã hóa; payload không bị chiếm quyền sở hữu.
 * @param[out] out Buffer do caller sở hữu.
 * @param[in] capacity Kích thước @p out; FRAME_CAPACITY luôn đủ cho frame hợp lệ.
 * @param[out] written Kích thước frame khi thành công, 0 nếu thất bại.
 * @return OK; BAD_REQUEST khi envelope sai hoặc frame không vừa buffer; BUSY khi
 *         json-c không cấp phát được bộ nhớ.
 */
Status frameEncode(const Envelope *envelope, uint8_t *out, size_t capacity, size_t *written);

/**
 * @brief Đổi Status thành chuỗi mã lỗi dùng trong payload của ERROR.
 * @param[in] status Giá trị Status bất kỳ, kể cả giá trị nằm ngoài enum.
 * @return Chuỗi tĩnh trùng tên enum (OK -> "OK", STALE_REQUEST ->
 *         "STALE_REQUEST", ...). Giá trị ngoài enum trả "BAD_REQUEST" để một
 *         status hỏng không bao giờ lọt ra ngoài wire.
 */
const char *statusCode(Status status);

#endif
