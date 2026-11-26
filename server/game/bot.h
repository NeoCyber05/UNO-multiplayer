#ifndef UNO_BOT_H
#define UNO_BOT_H

#include "server/game/view.h"

/**
 * @brief Chọn nước đi cho bot dựa trên thông tin mà ghế đó được phép thấy.
 * @param[in] view Trạng thái của ghế bot và các hành động hợp lệ.
 * @param[in] level Độ khó dùng để chấm điểm các lựa chọn.
 * @param[in,out] randomState Trạng thái dùng khi chọn ngẫu nhiên hoặc phân xử đồng điểm.
 * @param[out] out Nhận hành động đã chọn khi thành công.
 * @return OK nếu chọn được, BAD_REQUEST nếu đầu vào sai, NOT_YOUR_TURN nếu
 *         chưa tới lượt, hoặc INVALID_ACTION nếu không có hành động hợp lệ.
 */
Status botChoose(const GameView *view, BotLevel level, uint64_t *randomState, Intent *out);

#endif
