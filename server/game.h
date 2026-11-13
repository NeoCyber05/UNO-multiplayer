#ifndef UNO_GAME_H
#define UNO_GAME_H

#include "shared/contracts.h"

enum { CARD_COUNT = 112, SEAT_COUNT = 4 };
typedef enum { NO_COLOR, RED, YELLOW, GREEN, BLUE, WILD_COLOR } Color;
typedef enum {
    ZERO, ONE, TWO, THREE, FOUR, FIVE, SIX, SEVEN, EIGHT, NINE,
    SKIP, REVERSE, DRAW_TWO, DISCARD_ALL, WILD, WILD_FOUR
} Value;
typedef struct { char id[5]; Color color; Value value; } Card;
typedef struct { int count; Card cards[CARD_COUNT]; } Pile;
typedef enum { PLAY, DRAW, PASS, SAY_UNO, DRAFT_PICK, LEAVE, CHAT } IntentKind;
typedef struct {
    IntentKind kind;
    MatchId matchId; /* Bound to a Game by the coordinator. */
    uint32_t turnNo, draftTurn;
    char cardId[5];
    Color color;
    int index, chatId;
} Intent;
typedef enum { PLAYING, FINISHED, DRAFTING } Phase;
typedef struct { bool occupied; Card card; } Offer;
typedef struct { uint32_t turn; Offer offers[4]; } DraftState;
typedef struct {
    Mode mode;
    Phase phase;
    PlayerSpec players[SEAT_COUNT];
    Pile deck, discard, hands[SEAT_COUNT];
    DraftState draft;
    Color currentColor;
    Seat turn;
    uint32_t turnNo;
    int direction, winnerSeat, winnerTeam, team[SEAT_COUNT];
    char drawnCardId[5]; /* Empty until DRAW returns a playable card. */
    struct { bool active, declared; uint32_t generation; } watch[SEAT_COUNT];
    PlayerStats stats[SEAT_COUNT];
    uint64_t rng, revision, eventSeq;
} Game;

Status game_init(Game *g, Mode mode, const PlayerSpec players[4], uint64_t seed);
Status game_apply(Game *g, Seat actor, const Intent *intent, EventBatch *out);
Status game_timeout(Game *g, uint32_t expected_turn, EventBatch *out);
Status game_draft_timeout(Game *g, uint32_t expected_pick, EventBatch *out);
Status game_uno_penalty(Game *g, Seat actor, uint32_t watch_generation, EventBatch *out);
/* card is borrowed from hands[actor]. */
bool game_playable(const Game *g, Seat actor, const Card *card);
bool game_can_say_uno(const Game *g, Seat actor);
uint64_t game_random(uint64_t *state);

#endif
