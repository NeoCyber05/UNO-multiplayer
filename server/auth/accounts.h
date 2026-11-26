#ifndef UNO_ACCOUNTS_H
#define UNO_ACCOUNTS_H

#include "server/app.h"
#include "server/auth/auth_worker.h"
#include "shared/contracts.h"
#include "shared/protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    /* "UNO#" cộng tối đa 20 chữ số của UserId 64 bit và NUL. */
    USER_TAG_SIZE = 32,
    /* Số phiên đăng nhập giữ cùng lúc, kể cả phiên đã rớt mạng chờ RESUME. */
    MAX_SESSIONS = 256
};

/* Các hàm kiểm tra đầu vào: chỉ trả true với dữ liệu mà spec chấp nhận. */

/** @brief Email ASCII chữ thường, 3..254 byte, đúng một '@' và có chữ ở hai phía. */
bool accountsValidateEmail(const char *email);
/** @brief Mật khẩu 6..128 byte; không giới hạn nội dung vì chỉ dùng để băm. */
bool accountsValidatePassword(const char *password);
/** @brief Tên UTF-8 hợp lệ, 1..64 byte và 1..16 ký tự, không có ký tự điều khiển. */
bool accountsValidateName(const char *name);
/** @brief Một trong các mã avatar nằm trong danh sách cho phép (fox, panda, ...). */
bool accountsValidateAvatar(const char *avatar);

/**
 * @brief Sinh token phiên: 32 byte ngẫu nhiên của libsodium viết thành 64 ký tự hex.
 * @param[out] outToken Buffer SESSION_TOKEN_SIZE byte, kết thúc bằng NUL.
 */
void accountsGenerateToken(char outToken[SESSION_TOKEN_SIZE]);

/**
 * @brief Tạo tag kết bạn công khai dạng "UNO#<userId>".
 * @param[in] userId Mã tài khoản.
 * @param[out] outTag Buffer USER_TAG_SIZE byte.
 */
void accountsFormatTag(UserId userId, char outTag[USER_TAG_SIZE]);

/**
 * @brief Số mili giây từ CLOCK_MONOTONIC cộng một độ lệch cố định, dùng cho tuổi
 *        phiên và giới hạn tần suất xác thực. Không so sánh được với appNowMs.
 */
uint64_t accountsGetNowMs(void);

/* Một phiên đăng nhập. Phiên sống lâu hơn socket của nó (connected = false) để
 * RESUME chuyển được sang kết nối mới trong 24 giờ hiệu lực của token. */
typedef struct {
    bool inUse;
    UserId userId;
    char token[SESSION_TOKEN_SIZE];
    ConnId connId;
    bool connected;
    uint64_t createdAtMs;
} SessionEntry;

struct Sessions {
    SessionEntry entries[MAX_SESSIONS];
};

/**
 * @brief Cấp phát bảng phiên và bộ giới hạn tần suất theo IP.
 * @return OK, BAD_REQUEST khi @p app là NULL, hoặc STORAGE_ERROR khi hết bộ nhớ.
 */
Status sessionsInit(App *app);

/** @brief Giải phóng bảng phiên và bộ giới hạn tần suất; gọi hai lần vẫn an toàn. */
void sessionsCleanup(App *app);

/**
 * @brief Gắn tài khoản vừa đăng nhập vào một kết nối.
 *
 * Dùng lại phiên sẵn có của tài khoản, nếu không thì một ô trống, nếu không
 * nữa thì thay phiên đã rớt mạng cũ nhất.
 *
 * @param[in] connId Kết nối vừa LOGIN thành công.
 * @param[in] userId Mã tài khoản.
 * @param[in] token Token phiên mới; được chép lại.
 * @return OK, BAD_REQUEST khi đối số sai, hoặc BUSY khi mọi phiên đều đang kết nối.
 */
Status sessionsBind(App *app, ConnId connId, UserId userId, const char *token);

/**
 * @brief Tìm tài khoản đang gắn với một kết nối còn sống.
 * @param[out] outUserId Nhận mã tài khoản khi thành công.
 * @return OK, hoặc UNAUTHENTICATED khi slot đã đóng, đã được dùng lại (khác
 *         generation) hoặc chưa có phiên.
 */
Status sessionsUser(App *app, ConnId connId, UserId *outUserId);

/**
 * @brief Tìm kết nối mà một tài khoản đang dùng.
 * @param[out] outConn Nhận kết nối khi thành công.
 * @return OK, hoặc NOT_FOUND khi tài khoản không có phiên đang kết nối.
 */
Status sessionsFindConn(App *app, UserId userId, ConnId *outConn);

/**
 * @brief Chuyển một phiên sang kết nối mới bằng token (RESUME).
 * @param[in] connId Kết nối mới.
 * @param[in] token Token 64 ký tự hex từ LOGIN; token hết hạn bị xóa.
 * @return OK, hoặc UNAUTHENTICATED khi token lạ/hết hạn hoặc slot đã chết.
 */
Status sessionsResume(App *app, ConnId connId, const char *token);

/** @brief Đánh dấu phiên trên @p connId là đã rớt mạng nhưng giữ lại cho RESUME. */
void sessionsDisconnect(App *app, ConnId connId);

/** @brief Xóa hẳn phiên trên @p connId (LOGOUT hoặc đăng nhập tài khoản khác). */
void sessionsLogout(App *app, ConnId connId);

/**
 * @brief Tính một request xác thực từ @p ip vào cửa sổ trượt một phút của IP đó.
 * @return true nếu request được phép chạy; luôn true khi tắt giới hạn
 *         (authLimit <= 0) hoặc @p ip rỗng.
 */
bool accountsRateLimitCheck(App *app, const char *ip);

/**
 * @brief Xử lý REGISTER, LOGIN, RESUME và LOGOUT.
 *
 * REGISTER và LOGIN chỉ được kiểm tra ở đây rồi đưa sang auth worker; phản hồi
 * do authDrain gửi sau. RESUME và LOGOUT trả lời ngay. Token phiên chỉ xuất hiện
 * trong phản hồi của LOGIN.
 *
 * @param[in] connId Kết nối đã gửi @p envelope.
 * @param[in] envelope Request; payload chỉ được mượn.
 * @return OK khi đã gửi hoặc xếp hàng phản hồi, BAD_REQUEST với opcode khác hoặc
 *         kết nối đã chết.
 */
Status accountsHandle(App *app, ConnId connId, const Envelope *envelope);

/**
 * @brief Đọc mọi job xác thực đã xong từ pipe của worker rồi trả lời client.
 *
 * Kết quả của kết nối đã đóng hoặc đã bị dùng lại trong lúc chờ bị bỏ qua.
 * LOGIN thành công sẽ đóng socket cũ của tài khoản, gắn phiên và đồng bộ lại
 * phòng, hàng chờ và trạng thái online cho socket mới.
 */
void authDrain(App *app);

#endif
