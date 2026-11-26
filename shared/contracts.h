#ifndef UNO_CONTRACTS_H
#define UNO_CONTRACTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Result of every operation. The enum names double as wire error codes
 * (see statusCode), so the order must not change. */
typedef enum {
    OK, BAD_REQUEST, UNAUTHENTICATED, FORBIDDEN, BUSY, NOT_FOUND, ROOM_FULL,
    INVALID_ACTION, NOT_YOUR_TURN, STALE_TURN, STALE_REQUEST, RATE_LIMITED,
    STORAGE_ERROR, SERVER_FULL
} Status;

typedef uint64_t UserId;
typedef uint8_t Seat;
typedef char MatchId[37];
/* Logical connection handle: slot index plus reuse counter, never the socket fd. */
typedef struct { uint32_t slot, generation; } ConnId;
typedef enum { CLASSIC, TWO_V_TWO, SIDE, DRAFT } Mode;
typedef enum { EASY, NORMAL, HARD } BotLevel;

typedef struct {
    UserId userId;
    char name[65], avatar[32];
    BotLevel botLevel;
    bool isBot;
} PlayerSpec;

typedef struct {
    uint64_t seq, revision;
    int actor; /* -1 for server events */
    char type[32], payload[2048]; /* JSON object; fits a list of all 112 card IDs */
} MatchEvent;

typedef struct {
    size_t count;
    MatchEvent events[32];
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
    int winnerSeat, winnerTeam; /* -1 if absent */
    uint64_t durationMs;
    PlayerResult players[4];
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
