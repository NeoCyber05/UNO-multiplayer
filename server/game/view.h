#ifndef UNO_VIEW_H
#define UNO_VIEW_H

#include "server/game/game.h"
#include <json-c/json.h>

typedef struct {
    PlayerSpec player;
    int team, cardCount;
    bool connected, handVisible;
    Pile hand;
} PlayerView;

typedef struct {
    Seat viewerSeat, turn;
    Mode mode;
    Phase phase;
    int direction, drawCount, winnerSeat, winnerTeam;
    uint32_t turnNo;
    uint64_t revision;
    Color currentColor;
    Card topCard;
    DraftState draft;
    PlayerView players[SEAT_COUNT];
    bool legal[CARD_COUNT]; /* Indexed by position in the viewer's hand. */
    bool canDraw, canPass, canSayUno;
    /* The coordinator supplies match identity, clocks and connection status. */
    MatchId matchId;
    uint64_t deadline, serverNowMs, durationMs;
} GameView;

/**
 * @brief Build a snapshot with only the hands the viewer is allowed to see.
 * @param[in] game Current game state.
 * @param[in] viewer Seat for which to build the snapshot.
 * @param[out] view Receives the snapshot. The coordinator fills in clocks and presence.
 * @return OK on success or BAD_REQUEST for an invalid argument.
 */
Status viewBuild(const Game *game, Seat viewer, GameView *view);

/**
 * @brief Serialize the seat-specific GAME_STATE payload.
 * @param[in] view Snapshot for the intended viewer.
 * @param[out] out Receives one caller-owned JSON reference on success.
 * @return OK on success, BAD_REQUEST for an invalid argument, or BUSY on encoding failure.
 */
Status viewJson(const GameView *view, json_object **out);

#endif
