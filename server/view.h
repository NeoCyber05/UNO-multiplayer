#ifndef UNO_VIEW_H
#define UNO_VIEW_H

#include "game.h"
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
    PlayerView players[4];
    bool legal[CARD_COUNT]; /* Indexed by position in the viewer's hand. */
    bool canDraw, canPass, canSayUno;
    /* The coordinator supplies match identity, clocks and connection status. */
    MatchId matchId;
    uint64_t deadline, serverNowMs, durationMs;
} GameView;

Status view_build(const Game *g, Seat viewer, GameView *out);
/* On success the caller owns one reference to the GAME_STATE payload. */
Status view_json(const GameView *view, json_object **out);

#endif
