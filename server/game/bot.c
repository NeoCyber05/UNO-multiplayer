#include "bot.h"

#include <limits.h>
#include <string.h>

enum { WINNING_PLAY_SCORE = 1000 };

/* Reservoir sampling gives equal-scoring candidates an equal chance. */
static bool prefer(int score, int *best, unsigned *ties, uint64_t *randomState) {
    if (score < *best) return false;
    if (score > *best) {
        *best = score;
        *ties = 1;
        return true;
    }
    return gameRandom(randomState) % ++*ties == 0;
}

static int scorePlay(const GameView *view, const Card *card, BotLevel level, int sameColorCount) {
    if (level == EASY) return 0;
    const PlayerView *me = &view->players[view->viewerSeat];
    Seat nextSeat = (Seat)((SEAT_COUNT + view->turn + view->direction) % SEAT_COUNT);
    const PlayerView *target = &view->players[nextSeat];
    bool friendly = me->team >= 0 && me->team == target->team;
    bool threat = target->cardCount <= 2;
    int score = card->color == view->currentColor ? 2 : 0;
    switch (card->value) {
    case DISCARD_ALL:
        score += 3 * sameColorCount;
        break;
    case DRAW_TWO:
    case SKIP:
        score += friendly ? -8 : threat ? 12 : 6;
        break;
    case REVERSE:
        score += 2;
        break;
    case WILD:
        score -= 4;
        break;
    case WILD_FOUR:
        score += friendly ? -16 : threat ? 14 : -2;
        break;
    default:
        break;
    }
    if (level == HARD) {
        int shed = card->value == DISCARD_ALL ? sameColorCount : 1;
        if (shed == me->cardCount) return WINNING_PLAY_SCORE;
        score += 2 * shed;
        bool blocks = card->value == SKIP || card->value == DRAW_TWO || card->value == WILD_FOUR ||
                      card->value == REVERSE;
        if (blocks && friendly) score -= 40;
        if (blocks && !friendly && threat) score += 40;
    }
    return score;
}

Status botChoose(const GameView *view, BotLevel level, uint64_t *randomState, Intent *out) {
    if (!view || !randomState || !out || view->viewerSeat >= SEAT_COUNT || (unsigned)level > HARD)
        return BAD_REQUEST;
    Intent intent = {.turnNo = view->turnNo, .draftTurn = view->draft.turn};
    memcpy(intent.matchId, view->matchId, sizeof intent.matchId);
    if (view->canSayUno) {
        intent.kind = SAY_UNO;
    } else if (view->phase == FINISHED) {
        return INVALID_ACTION;
    } else if (view->viewerSeat != view->turn) {
        return NOT_YOUR_TURN;
    } else if (view->phase == DRAFTING) {
        int best = INT_MIN;
        unsigned ties = 0;
        intent.kind = DRAFT_PICK;
        for (int i = 0; i < DRAFT_OFFER_COUNT; i++) {
            if (view->draft.offers[i].occupied && prefer(0, &best, &ties, randomState))
                intent.index = i;
        }
        if (!ties) return INVALID_ACTION;
    } else {
        const Pile *hand = &view->players[view->viewerSeat].hand;
        int colorCounts[WILD_COLOR + 1] = {0};
        for (int i = 0; i < hand->count; i++)
            colorCounts[hand->cards[i].color]++;
        int best = INT_MIN, selectedIndex = -1;
        unsigned ties = 0;
        for (int i = 0; i < hand->count; i++) {
            if (!view->legal[i]) continue;
            const Card *card = &hand->cards[i];
            if (prefer(scorePlay(view, card, level, colorCounts[card->color]), &best, &ties,
                       randomState))
                selectedIndex = i;
        }
        if (selectedIndex >= 0) {
            Card card = hand->cards[selectedIndex];
            intent.kind = PLAY;
            memcpy(intent.cardId, card.id, sizeof intent.cardId);
            if (card.color == WILD_COLOR) {
                best = INT_MIN;
                ties = 0;
                for (Color color = RED; color <= BLUE; color++) {
                    if (prefer(level == EASY ? 0 : colorCounts[color], &best, &ties, randomState))
                        intent.color = color;
                }
            }
        } else if (view->canPass) {
            intent.kind = PASS;
        } else if (view->canDraw) {
            intent.kind = DRAW;
        } else {
            return INVALID_ACTION;
        }
    }
    *out = intent;
    return OK;
}
