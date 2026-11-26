#ifndef UNO_SOCIAL_H
#define UNO_SOCIAL_H

#include "server/app.h"
#include "shared/contracts.h"
#include "shared/protocol.h"
#include <stdbool.h>

/* Spec: presence được suy ra từ kết nối và hoạt động, không bao giờ từ điều client
 * tự khai. */
typedef enum { PRESENCE_OFFLINE, PRESENCE_IDLE, PRESENCE_ONLINE, PRESENCE_IN_MATCH } Presence;

/**
 * @brief Suy ra presence của user từ trạng thái server.
 * @param[in] app Trạng thái server do vòng poll sở hữu.
 * @param[in] user Tài khoản cần xem.
 * @return PRESENCE_OFFLINE khi không có phiên đang kết nối; PRESENCE_IN_MATCH khi
 *         matchesContains đang giữ ghế; PRESENCE_ONLINE khi ở phòng, hàng chờ hoặc
 *         party; còn lại PRESENCE_IDLE.
 */
Presence socialPresenceOf(App *app, UserId user);

/**
 * @brief Khóa trên wire của một giá trị presence.
 * @param[in] presence Giá trị presence.
 * @return Chuỗi tĩnh "offline", "idle", "online" hoặc "in_match"; giá trị ngoài
 *         enum trả "offline".
 */
const char *socialPresenceKey(Presence presence);

/**
 * @brief Đọc tag kết bạn công khai có đúng dạng "UNO#<id>".
 *
 * Chỉ nhận đúng chuỗi chuẩn mà accountsFormatTag sinh ra: không khoảng trắng, không
 * dấu, không số 0 ở đầu, không mã 0 và không vượt INT64_MAX (mã SQLite là số có
 * dấu 64 bit).
 * @param[in] tag Chuỗi từ client.
 * @param[out] outUserId Nhận mã khi thành công; giữ nguyên nếu thất bại.
 * @return true khi tag hợp lệ, false nếu không (kể cả khi đối số NULL).
 */
bool socialParseTag(const char *tag, UserId *outUserId);

/**
 * @brief Xử lý FRIEND_LIST, FRIEND_REQUEST, FRIEND_REPLY, PROFILE_GET,
 *        PROFILE_UPDATE, LEADERBOARD_GET và LOBBY_GET cho tài khoản của phiên.
 * @param[in,out] app Trạng thái server do vòng poll sở hữu.
 * @param[in] conn Kết nối đã gửi request.
 * @param[in] envelope Request; chỉ mượn trong lúc gọi.
 * @return OK khi đã trả lời (lỗi được gửi dạng ERROR: UNAUTHENTICATED, BAD_REQUEST,
 *         FORBIDDEN, NOT_FOUND, INVALID_ACTION, STORAGE_ERROR), BAD_REQUEST khi đối
 *         số NULL.
 */
Status socialHandle(App *app, ConnId conn, const Envelope *envelope);

/**
 * @brief Push FRIEND_STATE mới cho user và cho từng người bạn đã chấp nhận đang online.
 *
 * Gọi sau mọi thay đổi presence hoặc danh tính công khai của user. Người nhận
 * offline lỡ bản push này; lần LOGIN sau họ nhận danh sách mới.
 * @param[in,out] app Trạng thái server do vòng poll sở hữu.
 * @param[in] user Tài khoản vừa đổi presence hoặc danh tính.
 */
void socialPresence(App *app, UserId user);

#endif
