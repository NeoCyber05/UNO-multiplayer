#ifndef UNO_LOBBY_COMMON_H
#define UNO_LOBBY_COMMON_H

#include "shared/contracts.h"
#include "shared/protocol.h"
#include <stdbool.h>
#include <stddef.h>

enum {
    /* Số chế độ chơi: CLASSIC, TWO_V_TWO, SIDE, DRAFT. */
    LOBBY_MODE_COUNT = DRAFT + 1,
    /* UserId 64 bit viết thập phân tối đa 20 chữ số cộng NUL. */
    LOBBY_USER_ID_TEXT_SIZE = 32
};

/**
 * @brief Tên chế độ dùng trên wire ("classic", "2v2", "side", "draft").
 * @return Chuỗi tĩnh; giá trị ngoài enum trả "classic".
 */
const char *lobbyModeName(Mode mode);

/**
 * @brief Đổi tên chế độ trên wire (phân biệt hoa thường) thành Mode.
 * @param[out] outMode Nhận chế độ khi tên hợp lệ; giữ nguyên nếu không.
 * @return true khi @p name là một trong bốn tên chế độ.
 */
bool lobbyParseModeName(const char *name, Mode *outMode);

/**
 * @brief Mã user dạng chuỗi thập phân, vì số 64 bit vượt độ chính xác number của JSON.
 * @return Object JSON mới mà caller sở hữu.
 */
json_object *lobbyUserIdJson(UserId user);

/**
 * @brief Đọc mã user từ JSON: chuỗi thập phân chuẩn hoặc số nguyên dương.
 *
 * Chuỗi chỉ được gồm chữ số, không dấu, không khoảng trắng, không số 0 ở đầu và
 * không vượt INT64_MAX (giới hạn rowid của SQLite).
 *
 * @param[out] outUser Nhận mã khi hợp lệ; giữ nguyên nếu không.
 * @return true khi @p value là một mã user hợp lệ.
 */
bool lobbyParseUserIdJson(json_object *value, UserId *outUser);

/**
 * @brief Đọc mã user từ chuỗi thập phân chuẩn (quy tắc như lobbyParseUserIdJson).
 * @return true khi @p text hợp lệ.
 */
bool lobbyParseUserIdText(const char *text, UserId *outUser);

/**
 * @brief Tên hiển thị tạm "Player<id>" khi không đọc được hồ sơ từ Store.
 * @param[out] outName Buffer nhận tên.
 * @param[in] size Kích thước @p outName.
 */
void lobbyFallbackName(UserId user, char *outName, size_t size);

#endif
