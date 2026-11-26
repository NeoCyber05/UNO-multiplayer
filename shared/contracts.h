#ifndef UNO_CONTRACTS_H
#define UNO_CONTRACTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kết quả của mọi thao tác. Tên enum cũng là mã lỗi gửi trên wire (xem
 * statusCode), nên không được đổi thứ tự. */
typedef enum {
    OK, BAD_REQUEST, UNAUTHENTICATED, FORBIDDEN, BUSY, NOT_FOUND, ROOM_FULL,
    INVALID_ACTION, NOT_YOUR_TURN, STALE_TURN, STALE_REQUEST, RATE_LIMITED,
    STORAGE_ERROR, SERVER_FULL
} Status;

enum {
    /* UUID dạng chuỗi 36 ký tự cộng NUL. */
    MATCH_ID_SIZE = 37,
    /* Tên hiển thị tối đa 64 byte UTF-8 cộng NUL. */
    PLAYER_NAME_SIZE = 65,
    AVATAR_ID_SIZE = 32,
    EVENT_TYPE_SIZE = 32,
    /* Đủ chứa danh sách cả 112 mã lá bài trong một payload JSON. */
    EVENT_PAYLOAD_SIZE = 2048,
    EVENT_BATCH_CAPACITY = 32,
    MATCH_SEAT_COUNT = 4
};

typedef uint64_t UserId;
typedef uint8_t Seat;
typedef char MatchId[MATCH_ID_SIZE];
/* Định danh kết nối logic: chỉ số slot cộng bộ đếm tái sử dụng, không bao giờ là fd. */
typedef struct { uint32_t slot, generation; } ConnId;
typedef enum { CLASSIC, TWO_V_TWO, SIDE, DRAFT } Mode;
typedef enum { EASY, NORMAL, HARD } BotLevel;

typedef struct {
    UserId userId;
    char name[PLAYER_NAME_SIZE], avatar[AVATAR_ID_SIZE];
    BotLevel botLevel;
    bool isBot;
} PlayerSpec;

typedef struct {
    uint64_t seq, revision;
    int actor; /* -1 cho sự kiện do server sinh ra */
    char type[EVENT_TYPE_SIZE], payload[EVENT_PAYLOAD_SIZE]; /* payload là một object JSON */
} MatchEvent;

typedef struct {
    size_t count;
    MatchEvent events[EVENT_BATCH_CAPACITY];
} EventBatch;

typedef struct {
    uint32_t played, attacks, unoCalls, penalties, plus4;
} PlayerStats;

typedef struct {
    UserId userId;
    int team, points, eloBefore, eloAfter;
    bool left;
    PlayerStats stats;
} PlayerResult;

typedef enum { MATCH_ACTIVE, MATCH_FINALIZED, MATCH_ABORTED } MatchStatus;

typedef struct {
    MatchId matchId;
    Mode mode;
    bool ranked;
    MatchStatus status;
    int winnerSeat, winnerTeam; /* -1 khi không có */
    uint64_t durationMs;
    PlayerResult players[MATCH_SEAT_COUNT];
} MatchResult;

typedef enum {
    BADGE_FIRST_WIN  = 1u << 0, /* wins >= 1 */
    BADGE_WIN_100    = 1u << 1, /* wins >= 100 */
    BADGE_STREAK_10  = 1u << 2, /* bestStreak >= 10 */
    BADGE_PLUS4      = 1u << 3, /* plus4 >= 50 */
    BADGE_UNO_MASTER = 1u << 4, /* unoCalls >= 300 */
    BADGE_TEAM       = 1u << 5, /* teamWins >= 50 */
    BADGE_MATCHES    = 1u << 6, /* matches >= 500 */
    BADGE_DIAMOND    = 1u << 7  /* peakElo >= 1800 */
} BadgeBit;

typedef struct {
    uint32_t matches, wins, teamWins, plus4, unoCalls;
    int32_t  streak;      /* >0 chuỗi thắng, <0 chuỗi thua */
    uint32_t bestStreak;
    int32_t  peakElo;
} ProfileStats;

typedef struct {
    struct { uint32_t matches, wins, teamWins, plus4, unoCalls; } add; /* cộng dồn */
    int32_t  streak;      /* giá trị MỚI, ghi đè */
    uint32_t bestStreak;  /* giá trị MỚI, ghi đè */
    int32_t  peakElo;     /* giá trị MỚI, ghi đè */
    uint32_t unlocked;    /* BadgeBit mở lần đầu trong trận này */
} ProfileDelta;

typedef struct Store Store;
typedef struct App App;
typedef struct Rooms Rooms;
typedef struct Queue Queue;
typedef struct Matches Matches;
typedef struct Client Client;

#endif
