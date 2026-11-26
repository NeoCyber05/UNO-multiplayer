#ifndef UNO_NET_H
#define UNO_NET_H

#include "shared/protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    /* Số byte chưa gửi tối đa cho mỗi kết nối trước khi coi là client đọc quá chậm. */
    CONN_OUT_BUF_SIZE = 262144 /* 256 KiB */
};

/* Một luồng TCP non-blocking kèm bộ ghép frame và phần dữ liệu chờ gửi. */
typedef struct {
    int fd;
    uint32_t slot;
    uint32_t generation;
    FrameReader reader;
    uint8_t outBuf[CONN_OUT_BUF_SIZE];
    size_t outLen;
    size_t outPos;
    bool closing;
} Conn;

/**
 * @brief Nhận quyền sở hữu socket đã kết nối và chuyển nó sang non-blocking.
 * @param[out] connection Kết nối cần khởi tạo.
 * @param[in] fd Socket đã kết nối, hoặc -1 cho chỗ trống chưa có kết nối.
 * @param[in] slot Chỉ số slot logic; không phải fd.
 * @param[in] generation Bộ đếm tái sử dụng slot để phát hiện tham chiếu cũ.
 * @return OK; BAD_REQUEST khi @p connection là NULL hoặc fcntl() không đặt được
 *         O_NONBLOCK. Khi fcntl() lỗi, struct vẫn được khởi tạo đủ nên caller
 *         phải gọi connClose để đóng socket.
 */
Status connInit(Conn *connection, int fd, uint32_t slot, uint32_t generation);

/**
 * @brief Đọc cho tới khi socket báo hết dữ liệu và giải mã mọi frame đầy đủ.
 * @param[in,out] connection Kết nối có socket đang đọc được.
 * @param[in] onFrame Được gọi cho mỗi frame; xem frameFeed về vòng đời envelope.
 * @param[in] context Chuyển nguyên cho @p onFrame.
 * @return OK khi recv() báo EAGAIN/EWOULDBLOCK. BAD_REQUEST khi gặp EOF (recv()
 *         trả 0) hoặc lỗi socket; lỗi frame hoặc kết quả khác OK của callback
 *         được trả nguyên; mọi trường hợp này đều bật `closing`. Trả
 *         RATE_LIMITED mà không đọc khi kết nối đã `closing`, và BAD_REQUEST
 *         khi đối số NULL hoặc fd đã đóng.
 */
Status connRead(Conn *connection, FrameCallback onFrame, void *context);

/**
 * @brief Mã hóa envelope và nối vào phần dữ liệu chờ gửi.
 *
 * Hàm không gửi gì; hãy gọi connFlush hoặc chờ POLLOUT.
 *
 * @param[in,out] connection Kết nối đích.
 * @param[in] envelope Thông điệp cần xếp hàng; payload không bị chiếm quyền sở hữu.
 * @return OK, lỗi mã hóa, hoặc RATE_LIMITED khi buffer gửi sẽ vượt
 *         CONN_OUT_BUF_SIZE (kết nối khi đó bị đánh dấu `closing`).
 */
Status connQueue(Conn *connection, const Envelope *envelope);

/**
 * @brief Gửi phần dữ liệu chờ cho tới khi hết hoặc socket báo sẽ chặn.
 *
 * send() có thể nhận ít byte hơn số được đưa; phần còn lại vẫn nằm trong hàng
 * đợi và `outPos` ghi lại số byte kernel đã nhận.
 *
 * @param[in,out] connection Kết nối có dữ liệu chờ gửi.
 * @return OK khi đã gửi hết hoặc socket sẽ chặn (xem `outLen > outPos` để biết
 *         có cần chờ POLLOUT không). BAD_REQUEST khi lỗi socket, đồng thời bật
 *         `closing`.
 */
Status connFlush(Conn *connection);

/**
 * @brief Đóng socket và đặt lại kết nối; gọi hai lần vẫn an toàn.
 * @param[in,out] connection Kết nối cần đóng; fd chỉ bị đóng tối đa một lần.
 */
void connClose(Conn *connection);

#endif
