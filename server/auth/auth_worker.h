#ifndef UNO_AUTH_WORKER_H
#define UNO_AUTH_WORKER_H

#include "shared/contracts.h"
#include "server/storage/store.h"
#include <stdbool.h>
#include <stdint.h>

enum {
    /* Token phiên là 32 byte ngẫu nhiên viết dạng hex: 64 ký tự cộng NUL. */
    SESSION_TOKEN_BYTES = 32,
    HEX_DIGITS_PER_BYTE = 2,
    SESSION_TOKEN_SIZE = HEX_DIGITS_PER_BYTE * SESSION_TOKEN_BYTES + 1,
    /* Mật khẩu tối đa 128 byte cộng NUL. */
    AUTH_PASSWORD_SIZE = 129
};

typedef enum {
    AUTH_REGISTER,
    AUTH_LOGIN
} AuthJobKind;

/* Một request được chép nguyên vào hàng đợi của worker. Mật khẩu dạng rõ bị xóa
 * ở cả ba nơi từng giữ nó: bản caller (accountsHandle), ô trong hàng đợi (lúc
 * lấy ra) và bản cục bộ của worker (sau khi băm/kiểm tra). */
typedef struct {
    AuthJobKind kind;
    ConnId connId;
    int32_t reqId;
    char email[ACCOUNT_EMAIL_SIZE];
    char password[AUTH_PASSWORD_SIZE];
    char name[PLAYER_NAME_SIZE];
    char avatar[AVATAR_ID_SIZE];
    char hash[PASSWORD_HASH_SIZE];
    Account account;
    bool userExists;
} AuthJob;

/* Được ghi nguyên khối vào notify pipe; đủ nhỏ để một lần write() là nguyên tử. */
typedef struct {
    ConnId connId;
    int32_t reqId;
    Status status;
    UserId userId;
    char token[SESSION_TOKEN_SIZE];
    Account account;
    AuthJobKind kind;
} AuthResult;

typedef struct AuthWorker AuthWorker;

/**
 * @brief Khởi động luồng băm mật khẩu.
 *
 * Đồng thời tính sẵn một hash Argon2 giả để LOGIN với email không tồn tại tốn
 * thời gian ngang với một tài khoản thật.
 *
 * @param[out] outWorker Nhận worker khi thành công.
 * @param[in] notifyPipeFd Đầu ghi non-blocking của pipe mà appRun poll; caller
 *            vẫn sở hữu fd này.
 * @return OK, BAD_REQUEST khi đối số sai, hoặc STORAGE_ERROR khi khởi tạo
 *         libsodium hay pthread thất bại.
 */
Status authWorkerInit(AuthWorker **outWorker, int notifyPipeFd);

/**
 * @brief Xếp một job REGISTER hoặc LOGIN mà không chặn luồng poll.
 * @param[in,out] worker Worker đang chạy.
 * @param[in] job Được chép vào hàng đợi; caller có thể dùng lại ngay.
 * @return OK, BAD_REQUEST khi đối số NULL, hoặc BUSY khi hàng đợi (tối đa
 *         AUTH_QUEUE_CAPACITY job) đã đầy hay worker đang dừng.
 */
Status authSubmit(AuthWorker *worker, const AuthJob *job);

/**
 * @brief Dừng và join luồng, xóa mật khẩu còn trong hàng đợi rồi giải phóng worker.
 * @param[in] worker Worker cần hủy; NULL được bỏ qua. Các job còn trong hàng
 *            đợi được xử lý xong trước, nên lệnh này có thể chặn vài vòng băm.
 */
void authWorkerDestroy(AuthWorker *worker);

#endif
