#include "bot.h"

#include <limits.h>
#include <string.h>

/* Reservoir sampling gives equal-scoring candidates an equal chance. */
static bool prefer(int score, int *best, unsigned *ties, uint64_t *rng) {
    if (score < *best) return false;
    if (score > *best) {
        *best = score;
        *ties = 1;
        return true;
    }
    return game_random(rng) % ++*ties == 0;
}

static int score_play(const GameView *v, const Card *c, BotLevel level, int same_color) {
    if (level == EASY) return 0;
    const PlayerView *me = &v->players[v->viewerSeat];
    Seat next = (Seat)((4 + v->turn + v->direction) % 4);
    const PlayerView *target = &v->players[next];
    bool friendly = me->team >= 0 && me->team == target->team;
    bool threat = target->cardCount <= 2;
    int score = c->color == v->currentColor ? 2 : 0;
    switch (c->value) {
    case DISCARD_ALL: score += 3 * same_color; break;
    case DRAW_TWO:
    case SKIP: score += friendly ? -8 : threat ? 12 : 6; break;
    case REVERSE: score += 2; break;
    case WILD: score -= 4; break;
    case WILD_FOUR: score += friendly ? -16 : threat ? 14 : -2; break;
    default: break;
    }
    if (level == HARD) {
        int shed = c->value == DISCARD_ALL ? same_color : 1;
        if (shed == me->cardCount) return 1000;
        score += 2 * shed;
        bool blocks = c->value == SKIP || c->value == DRAW_TWO ||
                      c->value == WILD_FOUR || c->value == REVERSE;
        if (blocks && friendly) score -= 40;
        if (blocks && !friendly && threat) score += 40;
    }
    return score;
}

Status bot_choose(const GameView *v, BotLevel level, uint64_t *rng, Intent *out) {
    if (!v || !rng || !out || v->viewerSeat >= 4 || (unsigned)level > HARD) return BAD_REQUEST;
    Intent a = {.turnNo=v->turnNo,.draftTurn=v->draft.turn};
    memcpy(a.matchId,v->matchId,sizeof a.matchId);
    if (v->canSayUno) {
        a.kind = SAY_UNO;
    } else if (v->phase == FINISHED) return INVALID_ACTION;
    else if (v->viewerSeat != v->turn) return NOT_YOUR_TURN;
    else if (v->phase == DRAFTING) {
        int best = INT_MIN;
        unsigned ties = 0;
        a.kind = DRAFT_PICK;
        for (int i=0; i<4; i++) {
            if (v->draft.offers[i].occupied && prefer(0,&best,&ties,rng)) a.index = i;
        }
        if (!ties) return INVALID_ACTION;
    } else {
        const Pile *h = &v->players[v->viewerSeat].hand;
        int counts[WILD_COLOR+1] = {0};
        for (int i=0; i<h->count; i++) counts[h->cards[i].color]++;
        int best = INT_MIN, pick = -1;
        unsigned ties = 0;
        for (int i=0; i<h->count; i++) {
            if (!v->legal[i]) continue;
            const Card *c = &h->cards[i];
            if (prefer(score_play(v,c,level,counts[c->color]),&best,&ties,rng)) pick = i;
        }
        if (pick >= 0) {
            Card c = h->cards[pick];
            a.kind = PLAY;
            memcpy(a.cardId,c.id,sizeof a.cardId);
            if (c.color == WILD_COLOR) {
                best = INT_MIN;
                ties = 0;
                for (Color k=RED; k<=BLUE; k++)
                    if (prefer(level == EASY ? 0 : counts[k],&best,&ties,rng)) a.color = k;
            }
        } else if (v->canPass) a.kind = PASS;
        else if (v->canDraw) a.kind = DRAW;
        else return INVALID_ACTION;
    }
    *out = a;
    return OK;
}
