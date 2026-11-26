#ifndef UNO_RATING_H
#define UNO_RATING_H

#include "shared/contracts.h"

#include <stdbool.h>

enum {
    RATING_K_FACTOR = 32,
    RATING_SILVER_MIN = 1200,
    RATING_GOLD_MIN = 1400,
    RATING_DIAMOND_MIN = 1800
};

typedef enum { RANK_BRONZE, RANK_SILVER, RANK_GOLD, RANK_DIAMOND } Rank;

/**
 * @brief Tính mức thay đổi ELO của một người chơi hoặc một đội.
 * @param[in] own ELO cá nhân hoặc ELO trung bình của đội.
 * @param[in] opposition ELO trung bình của các đối thủ.
 * @param[in] won true nếu thắng (điểm 1), false nếu thua (điểm 0).
 * @return K nhân chênh lệch giữa điểm thực tế và kỳ vọng. lround làm tròn
 *         phần đúng nửa đơn vị theo hướng xa 0.
 */
int ratingDelta(double own, double opposition, bool won);

/**
 * @brief Xác định hạng theo ELO hiện tại.
 * @param[in] elo Điểm ELO cần xét.
 * @return RANK_BRONZE dưới 1200, RANK_SILVER từ 1200 đến 1399,
 *         RANK_GOLD từ 1400 đến 1799, RANK_DIAMOND từ 1800 trở lên.
 */
Rank ratingRank(int elo);

/**
 * @brief Lấy tên hạng theo định dạng giao thức.
 * @param[in] rank Hạng cần gửi.
 * @return Chuỗi tĩnh "bronze", "silver", "gold" hoặc "diamond".
 *         Giá trị ngoài phạm vi dùng "bronze".
 */
const char *ratingRankKey(Rank rank);

/**
 * @brief Điền ELO sau trận cho từng ghế dựa trên ELO trước trận.
 * @param[in,out] result Kết quả gồm ELO ban đầu, chế độ, đội và bên thắng.
 * @param[in] isBot Cờ bot theo ghế. Truyền NULL nếu mọi ghế là người chơi.
 *
 * Chỉ tính thay đổi cho người chơi trong trận xếp hạng đã chốt. Chế độ cá nhân
 * so với trung bình ba đối thủ. Chế độ đội so trung bình hai đội và tính cùng
 * một mức thay đổi cho đồng đội. Điểm sau cập nhật có sàn bằng 0.
 */
void ratingApply(MatchResult *result, const bool isBot[4]);

#endif
