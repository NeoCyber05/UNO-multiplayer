#ifndef UNO_PROFILE_H
#define UNO_PROFILE_H

#include "shared/contracts.h"

enum { PROFILE_SEAT_COUNT = 4 };

/**
 * @brief Tính cập nhật hồ sơ để lưu cùng kết quả trong một giao dịch.
 * @param[in] store Kho dữ liệu dùng để đọc thống kê hiện tại.
 * @param[in] result Kết quả đã chốt hoặc đã hủy. Nếu có ghế bot (userId bằng 0),
 *                   trận đã chốt chỉ cộng lượt tham gia và xét huy hiệu theo số trận.
 * @param[out] out Nhận một bản cập nhật cho mỗi ghế khi thành công.
 * @return OK, BAD_REQUEST nếu đối số sai hoặc trận còn diễn ra, hoặc lỗi đọc dữ liệu.
 *
 * Trận bị hủy trả về các bản cập nhật rỗng. Giao dịch chốt chỉ áp dụng một lần mỗi trận.
 */
Status profileCompute(Store *store, const MatchResult *result,
                      ProfileDelta out[PROFILE_SEAT_COUNT]);

#endif
