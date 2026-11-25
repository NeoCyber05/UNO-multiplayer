#ifndef UNO_GAME_H
#define UNO_GAME_H

#include "shared/contracts.h"

enum { CARD_COUNT = 112, SEAT_COUNT = 4, DRAFT_OFFER_COUNT = SEAT_COUNT };
typedef enum { NO_COLOR, RED, YELLOW, GREEN, BLUE, WILD_COLOR } Color;
typedef enum {
    ZERO,
    ONE,
    TWO,
    THREE,
    FOUR,
    FIVE,
    SIX,
    SEVEN,
    EIGHT,
    NINE,
    SKIP,
    REVERSE,
    DRAW_TWO,
    DISCARD_ALL,
    WILD,
    WILD_FOUR
} Value;
typedef struct {
    char id[5];
    Color color;
    Value value;
} Card;
typedef struct {
    int count;
    Card cards[CARD_COUNT];
} Pile;
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
typedef struct {
    bool occupied;
    Card card;
} Offer;
typedef struct {
    uint32_t turn;
    Offer offers[DRAFT_OFFER_COUNT];
} DraftState;
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
    bool unoDeclared[SEAT_COUNT];
    PlayerStats stats[SEAT_COUNT];
    uint64_t rng, revision, eventSeq;
} Game;

/**
 * @brief Initialize the game and deal the opening hands from a deterministic shuffle.
 * @param[out] game Game to initialize after player validation.
 * @param[in] mode Mode that determines teams and the initial phase.
 * @param[in] players Seat-indexed player specifications copied into the game.
 * @param[in] seed Initial shuffle state. Zero is a valid seed.
 * @return OK on success or BAD_REQUEST for invalid inputs.
 */
Status gameInit(Game *game, Mode mode, const PlayerSpec players[SEAT_COUNT], uint64_t seed);

/**
 * @brief Apply one validated move to the caller's candidate game.
 * @param[in,out] game Candidate state. The coordinator owns persistence and publication.
 * @param[in] actor Authenticated seat bound by the coordinator.
 * @param[in] intent Move and the turn or draft pick it applies to.
 * @param[out] out Receives the events produced by the move.
 * @return OK on success or a status describing the rejected move.
 */
Status gameApply(Game *game, Seat actor, const Intent *intent, EventBatch *out);

/**
 * @brief Advance an expired playing turn, drawing a card if the seat has yet to draw.
 * @param[in,out] game Candidate state in the playing phase.
 * @param[in] expectedTurn Turn number captured by the coordinator.
 * @param[out] out Receives the timeout and turn events.
 * @return OK on success, STALE_TURN when the turn changed, or an input or phase error.
 */
Status gameTimeout(Game *game, uint32_t expectedTurn, EventBatch *out);

/**
 * @brief Take the first occupied offer when a draft pick expires.
 * @param[in,out] game Candidate state in the draft phase.
 * @param[in] expectedPick Draft pick number captured by the coordinator.
 * @param[out] out Receives the draft events.
 * @return OK on success, STALE_TURN when the pick changed, or an input or phase error.
 */
Status gameDraftTimeout(Game *game, uint32_t expectedPick, EventBatch *out);

/**
 * @brief Check whether the active seat may play a card from its hand.
 * @param[in] game Current game state.
 * @param[in] actor Seat whose hand contains the card.
 * @param[in] card Card borrowed from that seat's hand.
 * @return True if the current turn and discard permit the play.
 */
bool gamePlayable(const Game *game, Seat actor, const Card *card);

/**
 * @brief Check whether the seat can declare UNO for a play that leaves one card.
 * @param[in] game Current game state.
 * @param[in] seat Seat considering the declaration.
 * @return True if an eligible play exists in the current turn.
 */
bool gameCanSayUno(const Game *game, Seat seat);

/**
 * @brief Generate a deterministic random word using SplitMix64.
 * @param[in,out] state State to advance. Any initial value is valid.
 * @return Next random word.
 */
uint64_t gameRandom(uint64_t *state);

#endif
