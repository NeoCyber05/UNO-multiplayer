#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum {
    /* Mỗi lần recv() lấy tối đa chừng này byte rồi mới đưa vào frameFeed. */
    RECEIVE_BUFFER_SIZE = 16384
};

/**
 * @brief Chuyển socket sang chế độ non-blocking.
 *
 * Vòng poll phục vụ mọi client trên một luồng, nên một lệnh đọc/ghi blocking
 * trên một socket sẽ làm treo tất cả client khác.
 *
 * @param[in] fd Socket đã kết nối.
 * @return true khi cả F_GETFL và F_SETFL đều thành công.
 */
static bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl(F_GETFL)");
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl(F_SETFL)");
        return false;
    }
    return true;
}

/* Đánh dấu kết nối cần đóng và trả về lỗi để caller báo tiếp lên vòng poll. */
static Status markClosing(Conn *connection, Status reason) {
    connection->closing = true;
    return reason;
}

/* Dời phần chưa gửi về đầu buffer để vùng trống phía sau liền mạch. */
static void compactOutput(Conn *connection) {
    if (connection->outPos == 0) return;
    size_t pending = connection->outLen - connection->outPos;
    if (pending > 0) {
        memmove(connection->outBuf, connection->outBuf + connection->outPos, pending);
    }
    connection->outLen = pending;
    connection->outPos = 0;
}

Status connInit(Conn *connection, int fd, uint32_t slot, uint32_t generation) {
    if (!connection) return BAD_REQUEST;
    connection->fd = fd;
    connection->slot = slot;
    connection->generation = generation;
    connection->outLen = 0;
    connection->outPos = 0;
    connection->closing = false;
    frameReaderReset(&connection->reader);
    /* Struct đã khởi tạo đủ trước khi báo lỗi, nên caller vẫn gọi connClose được. */
    if (fd >= 0 && !setNonBlocking(fd)) return BAD_REQUEST;
    return OK;
}

Status connRead(Conn *connection, FrameCallback onFrame, void *context) {
    if (!connection || connection->fd < 0 || !onFrame) return BAD_REQUEST;
    /* Kết nối đã bị đánh dấu đóng thì không được xử lý thêm request nào. */
    if (connection->closing) return RATE_LIMITED;

    uint8_t receiveBuffer[RECEIVE_BUFFER_SIZE];
    while (1) {
        // Bước recv: đọc cho tới khi socket non-blocking báo hết dữ liệu.
        ssize_t bytesReceived = recv(connection->fd, receiveBuffer, sizeof(receiveBuffer), 0);
        if (bytesReceived < 0) {
            /* Tín hiệu cắt ngang trước khi có dữ liệu: chỉ cần gọi lại. */
            if (errno == EINTR) continue;
            /* Đã đọc hết phần đang có; chờ POLLIN lần sau. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) return OK;
            return markClosing(connection, BAD_REQUEST);
        }
        /* recv() trả 0 chỉ khi phía bên kia đã đóng kết nối (EOF). */
        if (bytesReceived == 0) return markClosing(connection, BAD_REQUEST);
        Status status = frameFeed(&connection->reader, receiveBuffer, (size_t)bytesReceived,
                                  onFrame, context);
        if (status != OK) return markClosing(connection, status);
    }
}

Status connQueue(Conn *connection, const Envelope *envelope) {
    if (!connection || connection->fd < 0 || !envelope) return BAD_REQUEST;
    if (connection->closing) return RATE_LIMITED;

    compactOutput(connection);
    uint8_t frameBuffer[FRAME_CAPACITY];
    size_t frameLength = 0;
    Status status = frameEncode(envelope, frameBuffer, sizeof(frameBuffer), &frameLength);
    if (status != OK) return status;

    /* Client ngừng đọc thì không được để bộ nhớ server phình ra vô hạn. */
    if (connection->outLen + frameLength > CONN_OUT_BUF_SIZE) {
        return markClosing(connection, RATE_LIMITED);
    }
    memcpy(connection->outBuf + connection->outLen, frameBuffer, frameLength);
    connection->outLen += frameLength;
    return OK;
}

Status connFlush(Conn *connection) {
    if (!connection || connection->fd < 0) return BAD_REQUEST;

    while (connection->outLen > connection->outPos) {
        // Bước send: gửi phần còn lại, chấp nhận kernel chỉ nhận một phần.
        size_t bytesToSend = connection->outLen - connection->outPos;
        /* MSG_NOSIGNAL biến việc ghi vào peer đã đóng thành EPIPE thay vì SIGPIPE. */
        ssize_t bytesSent = send(connection->fd, connection->outBuf + connection->outPos,
                                 bytesToSend, MSG_NOSIGNAL);
        if (bytesSent < 0) {
            if (errno == EINTR) continue;
            /* Buffer gửi của kernel đã đầy; phần còn lại đi ở POLLOUT kế tiếp. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) return OK;
            return markClosing(connection, BAD_REQUEST);
        }
        if (bytesSent == 0) return markClosing(connection, BAD_REQUEST);
        /* Gửi thiếu là chuyện bình thường với TCP: giữ vị trí rồi lặp tiếp. */
        connection->outPos += (size_t)bytesSent;
    }

    connection->outPos = 0;
    connection->outLen = 0;
    return OK;
}

void connClose(Conn *connection) {
    if (!connection) return;
    if (connection->fd >= 0) {
        // Bước close: trên Linux fd luôn được giải phóng kể cả khi close() báo lỗi,
        // nên chỉ ghi lại lỗi và không gọi lại để tránh đóng nhầm fd đã tái sử dụng.
        if (close(connection->fd) != 0) perror("close");
        connection->fd = -1;
    }
    connection->outLen = 0;
    connection->outPos = 0;
    connection->closing = false;
    connection->slot = 0;
    connection->generation = 0;
    frameReaderReset(&connection->reader);
}
