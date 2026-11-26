#ifndef UNO_STORE_H
#define UNO_STORE_H

#include "shared/contracts.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>

enum {
    /* Email tối đa 254 byte (giới hạn của SMTP) cộng NUL. */
    ACCOUNT_EMAIL_SIZE = 255,
    /* Bằng crypto_pwhash_STRBYTES: chuỗi Argon2id kể cả NUL không vượt 128 byte. */
    PASSWORD_HASH_SIZE = 128
};

typedef struct {
    char email[ACCOUNT_EMAIL_SIZE];
    char hash[PASSWORD_HASH_SIZE];
    char name[PLAYER_NAME_SIZE];
    char avatar[AVATAR_ID_SIZE];
} AccountCreate;

typedef struct {
    UserId id;
    char email[ACCOUNT_EMAIL_SIZE];
    char hash[PASSWORD_HASH_SIZE];
    char name[PLAYER_NAME_SIZE];
    char avatar[AVATAR_ID_SIZE];
    int elo;
} Account;

/**
 * @brief Mở (hoặc tạo) database và chuẩn bị cho vòng poll.
 *
 * Bật khóa ngoại, WAL và thời gian chờ khóa 100 ms, áp schema nhúng cùng các
 * bước migrate (theo PRAGMA user_version), đánh dấu ABORTED các trận còn ACTIVE
 * do crash, rồi biên dịch sẵn mọi câu lệnh để các lần gọi sau không parse lại SQL.
 *
 * @param[out] out Nhận Store; đóng bằng storeClose.
 * @param[in] path Đường dẫn file hoặc ":memory:".
 * @return OK, BAD_REQUEST khi đối số NULL, hoặc STORAGE_ERROR.
 */
Status storeOpen(Store **out, const char *path);

/** @brief Hủy mọi câu lệnh, đóng database và giải phóng @p store; NULL được bỏ qua. */
void storeClose(Store *store);

/**
 * @brief Thêm một trận ACTIVE cùng bốn ghế trong một giao dịch.
 *
 * Gọi lặp an toàn: gọi lại với cùng mã, chế độ, cờ ranked và người chơi thì trả
 * OK mà không ghi gì.
 *
 * @return OK; BAD_REQUEST khi thiếu đối số hoặc mã rỗng; INVALID_ACTION khi mã đã
 *         tồn tại với dữ liệu khác hoặc không còn ACTIVE; STORAGE_ERROR khi đọc
 *         lỗi hoặc ghi lỗi (đã rollback).
 */
Status storeMatchStart(Store *store, const MatchId *matchId,
                       const PlayerSpec players[MATCH_SEAT_COUNT], Mode mode, bool ranked);

/**
 * @brief Nối một lô sự kiện trận trong một giao dịch.
 *
 * Sự kiện có seq đã tồn tại với cùng nội dung được bỏ qua, nên gửi lại một lô
 * là vô hại.
 *
 * @return OK (cả khi lô rỗng); NOT_FOUND khi không có trận; INVALID_ACTION khi
 *         seq đã lưu có nội dung khác; STORAGE_ERROR khi SQLite lỗi (đã rollback).
 */
Status storeEvents(Store *store, const MatchId *matchId, const EventBatch *batch);

/**
 * @brief Chốt trận: trạng thái, các ghế, ELO, số liệu hồ sơ và huy hiệu trong
 *        một giao dịch.
 *
 * Gọi lặp an toàn theo mã trận: chốt lại một trận đã FINALIZED với cùng kết quả
 * trả OK và không đổi gì, nên ELO và số liệu chỉ được cộng tối đa một lần kể cả
 * khi caller gọi lại sau crash hoặc timeout.
 *
 * @param[in] result Kết quả cuối; eloAfter phải được điền sẵn (ratingApply).
 * @param[in] deltas Thay đổi hồ sơ từng ghế từ profileCompute, hoặc NULL.
 * @return OK; BAD_REQUEST; NOT_FOUND khi không có trận; INVALID_ACTION khi trận đã
 *         bị hủy hoặc đã chốt với kết quả khác; STORAGE_ERROR sau khi rollback.
 */
Status storeFinalize(Store *store, const MatchResult *result,
                     const ProfileDelta deltas[MATCH_SEAT_COUNT]);

/**
 * @brief Ghi một trận cùng các ghế và sự kiện ra file JSON (hỗ trợ debug/demo).
 * @param[in] path File đích; bị ghi đè.
 * @return OK, BAD_REQUEST khi đối số NULL, NOT_FOUND khi không có trận, hoặc
 *         STORAGE_ERROR.
 */
Status storeExportMatch(Store *store, const MatchId *matchId, const char *path);

/**
 * @brief Đánh dấu trận đang ACTIVE thành ABORTED và ghi sự kiện hủy cùng lúc.
 * @param[in] store Store đang mở.
 * @param[in] id Trận cần hủy.
 * @param[in] event Sự kiện hủy nối vào nhật ký trận (ví dụ MATCH_ABORTED).
 * @param[in] durationMs Thời lượng đã chơi, lưu cùng trận bị hủy.
 * @return OK khi trận giờ là ABORTED, kể cả lần gọi lại trên trận đã ABORTED
 *         (không ghi thêm gì); BAD_REQUEST khi đối số NULL; NOT_FOUND khi không
 *         có trận; INVALID_ACTION khi trận đã FINALIZED; STORAGE_ERROR khi
 *         SQLite lỗi (trận vẫn ACTIVE).
 */
Status storeAbort(Store *store, const MatchId *id, const MatchEvent *event, uint64_t durationMs);

enum { STORE_LEADERBOARD_MAX = 20 };

typedef struct {
    UserId userId;
    char name[PLAYER_NAME_SIZE], avatar[AVATAR_ID_SIZE];
    int elo;
} LeaderboardEntry;

/**
 * @brief Đọc các tài khoản ELO cao nhất.
 * @param[in] store Store đang mở.
 * @param[out] out Nhận tối đa STORE_LEADERBOARD_MAX hàng, ELO cao nhất trước;
 *                 cùng ELO thì mã tài khoản nhỏ hơn đứng trước.
 * @param[out] count Số hàng đã ghi vào out.
 * @return OK khi thành công (count có thể bằng 0); BAD_REQUEST khi đối số NULL;
 *         STORAGE_ERROR khi SQLite lỗi (count giữ nguyên).
 */
Status storeLeaderboard(Store *store, LeaderboardEntry out[STORE_LEADERBOARD_MAX], size_t *count);

enum {
    STORE_FRIEND_MAX = 64,
    STORE_HISTORY_MAX = 20,
    STORE_MODE_COUNT = 4 /* CLASSIC, TWO_V_TWO, SIDE, DRAFT */
};

/* Quan hệ theo góc nhìn của tài khoản xin danh sách: bảng friends lưu
 * (user_id = người gửi, friend_id = người nhận), nên chiều của hàng đang chờ cho
 * biết ai còn phải trả lời. */
typedef enum { FRIEND_NONE, FRIEND_PENDING_OUT, FRIEND_PENDING_IN, FRIEND_ACCEPTED } FriendLink;

typedef struct {
    UserId userId;
    char name[PLAYER_NAME_SIZE], avatar[AVATAR_ID_SIZE];
    int elo;
    FriendLink link;
} FriendRow;

/**
 * @brief Gửi lời mời kết bạn từ một tài khoản tới tài khoản khác.
 * @param[in] store Store đang mở.
 * @param[in] from Tài khoản gửi lời mời.
 * @param[in] to Tài khoản nhận lời mời.
 * @return OK khi đã lưu lời mời chờ from -> to; BAD_REQUEST khi store NULL hoặc
 *         from == to; NOT_FOUND khi tài khoản to không tồn tại; INVALID_ACTION
 *         khi hai tài khoản đã có quan hệ theo bất kỳ chiều nào (chờ hoặc đã
 *         chấp nhận); STORAGE_ERROR khi SQLite lỗi (không ghi gì).
 */
Status storeFriendRequest(Store *store, UserId from, UserId to);

/**
 * @brief Chấp nhận hoặc từ chối lời mời requester gửi tới me.
 * @param[in] store Store đang mở.
 * @param[in] me Tài khoản đã nhận lời mời.
 * @param[in] requester Tài khoản đã gửi lời mời.
 * @param[in] accept true đánh dấu đã chấp nhận, false xóa lời mời để có thể gửi lại sau.
 * @return OK khi đã chấp nhận hoặc xóa; BAD_REQUEST khi store NULL; NOT_FOUND khi
 *         không có lời mời chờ requester -> me; STORAGE_ERROR khi SQLite lỗi
 *         (lời mời giữ nguyên).
 */
Status storeFriendReply(Store *store, UserId me, UserId requester, bool accept);

/**
 * @brief Liệt kê mọi bạn bè và lời mời đang chờ liên quan tới một tài khoản.
 * @param[in] store Store đang mở.
 * @param[in] me Tài khoản cần liệt kê.
 * @param[out] out Nhận tối đa STORE_FRIEND_MAX hàng: bạn đã chấp nhận trước, rồi
 *                 tới lời mời chờ, mỗi nhóm xếp theo mã tài khoản, để một loạt
 *                 lời mời không che mất bạn thật. Mỗi hàng là danh tính công khai
 *                 của người kia và quan hệ nhìn từ me.
 * @param[out] count Số hàng đã ghi vào out.
 * @return OK khi thành công (count có thể bằng 0); BAD_REQUEST khi đối số NULL;
 *         STORAGE_ERROR khi SQLite lỗi (count giữ nguyên).
 */
Status storeFriendList(Store *store, UserId me, FriendRow out[STORE_FRIEND_MAX], size_t *count);

/* endedAt tính bằng giây Unix, cùng đồng hồ với matches.created_at. */
typedef struct {
    char matchId[MATCH_ID_SIZE];
    Mode mode;
    bool won;
    int eloDelta;
    int64_t endedAt;
} HistoryRow;

/* Chỉ gồm hồ sơ công khai: không có email, hash mật khẩu hay token, nên caller có
 * thể gửi nguyên struct cho bất kỳ người xem nào. */
typedef struct {
    UserId userId;
    char name[PLAYER_NAME_SIZE], avatar[AVATAR_ID_SIZE];
    int elo;
    ProfileStats stats;
    uint32_t badges;                                             /* OR các bit từ achievements */
    struct { uint32_t matches, wins; } modes[STORE_MODE_COUNT]; /* chỉ trận đã chốt */
    HistoryRow history[STORE_HISTORY_MAX];
    size_t historyCount;                                         /* mới nhất trước */
} Profile;

/**
 * @brief Đọc hồ sơ công khai, tổng theo chế độ và lịch sử gần đây của một tài khoản.
 * @param[in] store Store đang mở.
 * @param[in] id Tài khoản cần đọc.
 * @param[out] out Hồ sơ đã điền; thống kê theo chế độ và lịch sử chỉ tính trận
 *                 FINALIZED, và người đã rời trận không bao giờ được tính thắng.
 * @return OK khi thành công; BAD_REQUEST khi đối số NULL; NOT_FOUND khi tài khoản
 *         không tồn tại; STORAGE_ERROR khi SQLite lỗi (out có thể đã ghi một phần).
 */
Status storeProfileGet(Store *store, UserId id, Profile *out);

/**
 * @brief Đổi tên hiển thị và avatar của một tài khoản.
 * @param[in] store Store đang mở.
 * @param[in] id Tài khoản cần sửa.
 * @param[in] name Tên mới, đã qua accountsValidateName.
 * @param[in] avatar Mã avatar mới, đã qua accountsValidateAvatar.
 * @return OK khi đã cập nhật; BAD_REQUEST khi đối số NULL; NOT_FOUND khi tài khoản
 *         không tồn tại; STORAGE_ERROR khi SQLite lỗi (tài khoản giữ nguyên).
 */
Status storeProfileUpdate(Store *store, UserId id, const char *name, const char *avatar);

/**
 * @brief Tạo tài khoản cùng hàng profile_stats ban đầu trong một giao dịch.
 * @param[out] outId Nhận mã tài khoản mới khi thành công.
 * @return OK; BAD_REQUEST khi đối số NULL hoặc email đã được dùng; STORAGE_ERROR
 *         khi SQLite lỗi (không ghi gì).
 */
Status storeAccountCreate(Store *store, const AccountCreate *newAccount, UserId *outId);

/**
 * @brief Đọc tài khoản theo email (kể cả hash mật khẩu, chỉ dùng phía server).
 * @return OK, BAD_REQUEST khi đối số NULL, NOT_FOUND khi không có, hoặc STORAGE_ERROR.
 */
Status storeAccountGet(Store *store, const char *email, Account *out);

/**
 * @brief Đọc tài khoản theo mã (kể cả hash mật khẩu, chỉ dùng phía server).
 * @return OK, BAD_REQUEST khi đối số NULL, NOT_FOUND khi không có, hoặc STORAGE_ERROR.
 */
Status storeAccountGetById(Store *store, UserId id, Account *out);

/**
 * @brief Đọc số liệu tích lũy trong profile_stats của một tài khoản.
 * @return OK, BAD_REQUEST khi đối số NULL, NOT_FOUND khi chưa có hàng, hoặc STORAGE_ERROR.
 */
Status storeProfileStats(Store *store, UserId userId, ProfileStats *out);

/** @brief Handle SQLite bên dưới cho caller cần truy vấn riêng; Store vẫn sở hữu handle. */
sqlite3 *storeGetDb(Store *store);

#endif
