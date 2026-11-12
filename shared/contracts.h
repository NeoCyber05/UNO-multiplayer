#ifndef UNO_CONTRACTS_H
#define UNO_CONTRACTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OK, BAD_REQUEST, UNAUTHENTICATED, FORBIDDEN, BUSY, NOT_FOUND, ROOM_FULL,
    INVALID_ACTION, NOT_YOUR_TURN, STALE_TURN, STALE_REQUEST, RATE_LIMITED,
    STORAGE_ERROR, SERVER_FULL
} Status;

typedef uint64_t UserId;
typedef uint8_t Seat;
typedef char MatchId[37];
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

typedef struct {
    uint32_t matches, wins, plus4, unoCalls, teamWins;
    uint32_t streak, bestStreak, peakElo;
    struct {
        uint32_t firstWin, win100, streak10, plus4, unoMaster, team, matches500, diamond;
    } badges;
} ProfileDelta;

typedef struct Store Store;
typedef struct App App;
typedef struct Rooms Rooms;
typedef struct Queue Queue;
typedef struct Matches Matches;
typedef struct Client Client;

#endif
