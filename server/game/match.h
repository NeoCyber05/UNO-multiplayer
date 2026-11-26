#ifndef UNO_MATCH_H
#define UNO_MATCH_H

#include "server/lobby/rooms.h"

/* Callback mượn đối số trong lúc được gọi và tránh sửa trạng thái bộ điều phối. */
typedef void (*MatchPush)(void *context, UserId userId, const char *type, json_object *payload);
typedef void (*MatchEnded)(void *context, const char *roomId);

/**
 * @brief Tạo bộ điều phối trận do vòng lặp poll của máy chủ quản lý.
 * @param[in] store Kho dữ liệu đã mở, phải còn hiệu lực suốt đời bộ điều phối.
 * @param[in] push Callback xếp thông báo gửi đến tài khoản đã xác thực.
 * @param[in] ended Callback tùy chọn để cập nhật phòng sau khi lưu kết quả.
 * @param[in] context Ngữ cảnh do bên gọi giữ và truyền cho các callback.
 * @return Bộ điều phối cần giải phóng bằng matchesDestroy, hoặc NULL khi thất bại.
 */
Matches *matchesCreate(Store *store, MatchPush push, MatchEnded ended, void *context);

/**
 * @brief Giải phóng bộ điều phối khi vòng lặp poll đã ngừng dùng nó.
 * @param[in] matches Bộ điều phối do bên gọi sở hữu, có thể là NULL.
 */
void matchesDestroy(Matches *matches);

/**
 * @brief Lưu trận mới trước khi gửi trạng thái ban đầu cho người chơi.
 * @param[in,out] matches Bộ điều phối do vòng lặp poll quản lý.
 * @param[in] room Cấu hình ghế của phòng. Chỉ trận không xếp hạng mới cho ghế bot.
 * @param[in] nowMs Thời điểm hiện tại theo đồng hồ đơn điệu.
 * @param[out] outMatchId Nhận mã trận mới nếu thành công.
 * @return OK khi thành công hoặc mã lỗi kiểm tra dữ liệu, sức chứa hay lưu trữ.
 */
Status matchesStart(Matches *matches, const RoomStart *room, uint64_t nowMs, MatchId *outMatchId);

/**
 * @brief Kiểm tra tài khoản còn gắn với trận đang diễn ra hoặc đang chờ lưu kết quả.
 * @param[in] matches Bộ điều phối do vòng lặp poll quản lý.
 * @param[in] userId Mã tài khoản cần kiểm tra.
 * @return true nếu trận vẫn giữ ghế của tài khoản, kể cả khi người chơi đã rời trận.
 */
bool matchesContains(const Matches *matches, UserId userId);

/**
 * @brief Cập nhật kết nối của ghế khi phiên ngắt hoặc LOGIN/RESUME thành công.
 * @param[in,out] matches Bộ điều phối do vòng lặp poll quản lý.
 * @param[in] userId Mã tài khoản lấy từ phiên đã xác thực.
 * @param[in] connected true nếu phiên có kết nối, false nếu vừa ngắt.
 * @param[in] nowMs Thời điểm hiện tại theo đồng hồ đơn điệu.
 * @return OK nếu xử lý được, BAD_REQUEST nếu thiếu bộ điều phối, NOT_FOUND nếu
 *         không còn trận, hoặc FORBIDDEN nếu người chơi đã gửi MATCH_LEAVE.
 */
Status matchesConnected(Matches *matches, UserId userId, bool connected, uint64_t nowMs);

/**
 * @brief Xử lý lệnh theo danh tính đã xác thực và trạng thái ván hiện tại.
 * @param[in,out] matches Bộ điều phối do vòng lặp poll quản lý.
 * @param[in] userId Mã tài khoản gắn với phiên gửi lệnh.
 * @param[in] request Thông điệp mượn trong lúc xử lý.
 * @param[in] nowMs Thời điểm dùng để xử lý lượt hết hạn trước khi xét lệnh.
 * @return OK khi thành công hoặc mã trạng thái dùng trong phản hồi ERROR.
 */
Status matchesHandle(Matches *matches, UserId userId, const Envelope *request, uint64_t nowMs);

/**
 * @brief Xử lý lượt hết hạn, nước đi của bot và lần thử lưu đã đến hạn.
 * @param[in,out] matches Bộ điều phối do vòng lặp poll quản lý, bỏ qua nếu NULL.
 * @param[in] nowMs Thời điểm hiện tại theo cùng đồng hồ với matchesStart.
 */
void matchesTick(Matches *matches, uint64_t nowMs);

#endif
