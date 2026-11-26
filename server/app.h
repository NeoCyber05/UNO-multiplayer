#ifndef UNO_APP_H
#define UNO_APP_H

#include "server/lobby/rooms.h"
#include "shared/contracts.h"
#include "shared/net.h"

enum {
    APP_MAX_CONNS = 64,
    /* Client gửi PING mỗi 5 giây. Sau 15 giây không nhận frame mới, máy chủ đóng kết nối. */
    APP_DEFAULT_HEARTBEAT_MS = 15000,
    /* Dự phòng nhiều bản ghi hơn giới hạn 16 trận chạy đồng thời. */
    APP_MATCH_RECORDS = 32
};

/* Callback kết thúc chỉ mang mã phòng. Giữ danh sách tài khoản ở đây để
 * biết cần cập nhật trạng thái hiện diện của ai sau trận. */
typedef struct {
    bool active;
    char roomId[sizeof(((RoomStart *)0)->roomId)];
    UserId users[ROOM_MAX_SEATS]; /* Ghế bot có mã tài khoản bằng 0. */
} AppMatchRecord;

typedef struct {
    Conn conn;
    bool active;
    /* Phân biệt các kết nối từng dùng chung một ô, kể cả khi fd bị tái sử dụng. */
    uint32_t generation;
    char clientIp[64];
    uint64_t rateLastMs;
    double rateTokens;
    int32_t lastRequestId; /* Mã yêu cầu phải tăng nghiêm ngặt trong mỗi kết nối. */
    uint64_t lastFrameMs;  /* Thời điểm đơn điệu của frame được chấp nhận gần nhất. */
} AppConnSlot;

typedef struct AuthWorker AuthWorker;
typedef struct Sessions Sessions;
typedef struct RateLimiter RateLimiter;

struct App {
    int listenFd;
    uint16_t boundPort;
    char bindIp[64];
    char dbPath[256];
    int authLimit;
    bool running;
    Store *store;
    AppConnSlot slots[APP_MAX_CONNS];
    AuthWorker *authWorker;
    int authPipeRead;
    int authPipeWrite;
    Sessions *sessions;
    RateLimiter *rateLimiter;
    Rooms *rooms;
    Queue *queue;
    Matches *matches;
    int heartbeatMs;
    AppMatchRecord matchRecords[APP_MATCH_RECORDS];
    /* Chỉ có giá trị khi appUserLeave chạy để callback hủy trận bỏ qua
     * thông báo trùng cho tài khoản sắp ngắt phiên. */
    UserId leavingUser;
};

/**
 * @brief Khởi tạo trạng thái máy chủ và mở socket lắng nghe không chặn.
 *
 * Chuẩn bị luồng xác thực, pipe báo kết quả và bộ xử lý tín hiệu.
 * appRun mở cơ sở dữ liệu và bộ điều phối sau để kiểm thử có thể cấp bản riêng.
 *
 * @param[out] app Trạng thái máy chủ, được xóa về 0 trước khi khởi tạo.
 * @param[in] bindIp Địa chỉ IPv4 dạng thập phân có dấu chấm, NULL dùng "0.0.0.0".
 * @param[in] port Cổng TCP theo thứ tự byte của máy chủ. Dùng 0 để hệ điều hành
 *                 chọn cổng và lưu cổng thực tế vào boundPort.
 * @param[in] dbPath Đường dẫn SQLite, NULL dùng "uno.db".
 * @param[in] authLimit Số yêu cầu đăng ký hoặc đăng nhập mỗi phút cho một IP.
 * @return OK khi khởi tạo xong, BAD_REQUEST nếu thiếu app hoặc địa chỉ sai,
 *         BUSY nếu socket(), bind() hay listen() lỗi, hoặc mã lỗi từ queueInit.
 */
Status appInit(App *app, const char *bindIp, uint16_t port, const char *dbPath, int authLimit);

/**
 * @brief Chạy vòng lặp poll và thu hồi tài nguyên khi vòng lặp kết thúc.
 * @param[in] bindIp Địa chỉ IPv4 dùng để lắng nghe.
 * @param[in] port Cổng TCP. Dùng 0 để hệ điều hành chọn cổng, đọc từ dòng "PORT n".
 * @param[in] dbPath Đường dẫn SQLite hoặc ":memory:" để dùng bộ nhớ.
 * @param[in] authLimit Số yêu cầu đăng ký hoặc đăng nhập mỗi phút cho một IP.
 * @param[in] heartbeatMs Số mili giây chờ frame được chấp nhận. Giá trị <= 0 dùng mặc định.
 * @return 0 sau khi thoát vòng lặp, 1 nếu khởi động thất bại.
 */
int appRun(const char *bindIp, uint16_t port, const char *dbPath, int authLimit,
            int heartbeatMs);
/**
 * @brief Yêu cầu vòng lặp poll dừng sau lượt xử lý hiện tại.
 * @param[in,out] app Trạng thái máy chủ. Bỏ qua nếu là NULL.
 */
void appStop(App *app);

/**
 * @brief Đóng kết nối và giải phóng tài nguyên theo thứ tự phụ thuộc.
 *
 * Có thể gọi với App mới khởi tạo một phần. Xóa cờ running trước để tránh gửi
 * cập nhật hiện diện trong lúc đóng các kết nối.
 *
 * @param[in,out] app Trạng thái cần dọn. Bên gọi quản lý vùng nhớ của App.
 */
void appCleanup(App *app);

/**
 * @brief Ngắt phiên và đóng socket để ô kết nối có thể dùng lại.
 *
 * Tăng generation để mã kết nối cũ không trỏ nhầm tới socket dùng lại ô này.
 *
 * @param[in,out] app Trạng thái do vòng lặp poll quản lý.
 * @param[in] slotIndex Chỉ số trong slots. Bỏ qua ô rỗng hoặc chỉ số ngoài phạm vi.
 */
void appCloseSlot(App *app, int slotIndex);

/**
 * @brief Báo SERVER_FULL và đóng socket mới vì đã hết ô kết nối.
 *
 * Gửi một frame ERROR có id 0 bằng send() với MSG_NOSIGNAL để tránh SIGPIPE.
 * Chỉ thử gửi một lần. Lỗi gửi hoặc gửi thiếu byte được ghi ra stderr để
 * stdout giữ riêng dòng "PORT n".
 *
 * @param[in] connectionSocket Socket từ accept(), được đóng trong hàm.
 * @return OK nếu đã chuyển đủ frame cho hệ điều hành, BAD_REQUEST nếu lỗi
 *         mã hóa, lỗi gửi hoặc chỉ gửi được một phần.
 */
Status appRejectConnection(int connectionSocket);

/**
 * @brief Gỡ tài khoản khi socket đóng, người dùng LOGOUT hoặc đổi tài khoản.
 *
 * Xử lý hàng chờ, lời mời, nhóm và ghế phòng trước khi ngắt phiên để còn đường
 * gửi các thông báo liên quan. Trận giữ ghế cho RESUME hoặc LOGIN nếu vẫn còn
 * người khác kết nối. Sau khi ngắt phiên mới báo trạng thái hiện diện cho bạn bè.
 * Bên gọi quyết định xóa phiên qua sessionsLogout hay giữ lại cho RESUME.
 *
 * @param[in,out] app Trạng thái do vòng lặp poll quản lý.
 * @param[in] conn Mã kết nối tài khoản đang rời.
 * @param[in] user Mã tài khoản gắn với kết nối. Bỏ qua mã 0.
 */
void appUserLeave(App *app, ConnId conn, UserId user);

/**
 * @brief Dùng một token để kiểm tra kết nối còn hạn mức gửi yêu cầu.
 *
 * Bộ đếm hồi 0,03 token mỗi mili giây và giữ tối đa 60 token. Client có thể
 * gửi dồn 60 yêu cầu khi đầy rồi duy trì 30 yêu cầu mỗi giây.
 *
 * @param[in,out] slot Ô kết nối có hạn mức cần cập nhật.
 * @param[in] nowMs Thời điểm đơn điệu lấy từ appNowMs.
 * @return true nếu còn token, false nếu hết hạn mức hoặc thiếu ô kết nối.
 */
bool appConnCheckRateLimit(AppConnSlot *slot, uint64_t nowMs);

/**
 * @brief Đọc đồng hồ dùng cho vòng lặp poll, heartbeat và thời hạn lượt.
 * @return Mili giây từ CLOCK_MONOTONIC để tránh ảnh hưởng khi chỉnh giờ hệ thống.
 *         Trả về 0 nếu không đọc được đồng hồ.
 */
uint64_t appNowMs(void);

/**
 * @brief Xếp và thử gửi thông báo chủ động có id 0 đến kết nối của tài khoản.
 * @param[in,out] app Trạng thái do vòng lặp poll quản lý.
 * @param[in] user Mã tài khoản cần nhận thông báo.
 * @param[in] type Loại thông báo, ví dụ "GAME_STATE".
 * @param[in] payload Đối tượng JSON được mượn trong lúc gọi.
 * @return OK nếu đã xếp gửi, kể cả khi còn byte chờ POLLOUT. Trả về NOT_FOUND
 *         nếu mất kết nối, BAD_REQUEST nếu đầu vào sai hoặc mã lỗi khi gửi.
 */
Status appPush(App *app, UserId user, const char *type, json_object *payload);

/**
 * @brief Trả lời yêu cầu bằng trạng thái để client ghép phản hồi theo mã yêu cầu.
 * @param[in,out] app Trạng thái do vòng lặp poll quản lý.
 * @param[in] conn Mã kết nối gửi yêu cầu. Bỏ qua nếu kết nối đã đóng.
 * @param[in] id Mã yêu cầu được gửi lại cho client.
 * @param[in] status OK gửi payload rỗng. Giá trị khác gửi ERROR có code và message.
 */
void appReplyStatus(App *app, ConnId conn, int32_t id, Status status);

/**
 * @brief Gửi phản hồi OK kèm dữ liệu kết quả cho yêu cầu đã xử lý.
 * @param[in,out] app Trạng thái do vòng lặp poll quản lý.
 * @param[in] conn Mã kết nối gửi yêu cầu. Bỏ qua nếu kết nối đã đóng.
 * @param[in] id Mã yêu cầu được gửi lại cho client.
 * @param[in] payload Đối tượng JSON do bên gọi giữ quyền sở hữu.
 */
void appReplyOk(App *app, ConnId conn, int32_t id, json_object *payload);

#endif
