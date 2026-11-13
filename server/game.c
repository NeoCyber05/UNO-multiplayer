#include "game.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* SplitMix64 accepts every seed, including zero. */
uint64_t game_random(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static Card pop(Pile *p) { return p->cards[--p->count]; }
static void push(Pile *p, Card c) { p->cards[p->count++] = c; }

static void shuffle(Game *g) {
    for (int i = g->deck.count - 1; i > 0; i--) {
        int j = (int)(game_random(&g->rng) % (i + 1));
        Card c = g->deck.cards[i];
        g->deck.cards[i] = g->deck.cards[j];
        g->deck.cards[j] = c;
    }
}

static void add_card(Pile *deck, Color color, Value value) {
    Card c = {.color=color, .value=value};
    snprintf(c.id, sizeof c.id, "c%u", (unsigned char)(deck->count + 1));
    push(deck, c);
}

static void deal_offers(Game *g) {
    for (int i = 0; i < 4; i++)
        g->draft.offers[i] = (Offer){.occupied=true,.card=pop(&g->deck)};
}

Status game_init(Game *g, Mode mode, const PlayerSpec players[4], uint64_t seed) {
    if (!g || !players || (unsigned)mode > DRAFT) return BAD_REQUEST;
    for (int s = 0; s < 4; s++) {
        if (!memchr(players[s].name, 0, sizeof players[s].name) ||
            !memchr(players[s].avatar, 0, sizeof players[s].avatar) ||
            (unsigned)players[s].botLevel > HARD) return BAD_REQUEST;
    }
    memset(g, 0, sizeof *g);
    g->mode = mode;
    g->phase = mode == DRAFT ? DRAFTING : PLAYING;
    g->rng = seed;
    g->direction = 1;
    g->revision = g->turnNo = 1;
    g->winnerSeat = g->winnerTeam = -1;
    memcpy(g->players, players, sizeof g->players);
    for (int s = 0; s < 4; s++) {
        g->team[s] = -1;
        if (mode == TWO_V_TWO) g->team[s] = s % 2;
        if (mode == SIDE) g->team[s] = s / 2;
    }
    for (Color c = RED; c <= BLUE; c++) {
        add_card(&g->deck, c, ZERO);
        for (Value v = ONE; v <= DRAW_TWO; v++) {
            add_card(&g->deck, c, v);
            add_card(&g->deck, c, v);
        }
        add_card(&g->deck, c, DISCARD_ALL);
    }
    for (int i = 0; i < 4; i++) {
        add_card(&g->deck, WILD_COLOR, WILD);
        add_card(&g->deck, WILD_COLOR, WILD_FOUR);
    }
    shuffle(g);
    int opening = mode == DRAFT ? 4 : 7;
    for (int r = 0; r < opening; r++)
        for (Seat s = 0; s < 4; s++) push(&g->hands[s], pop(&g->deck));
    if (mode == DRAFT) deal_offers(g);
    int i = 0;
    while (g->deck.cards[i].value > NINE) i++;
    Card first = g->deck.cards[i];
    g->deck.cards[i] = pop(&g->deck);
    push(&g->discard, first);
    g->currentColor = first.color;
    return OK;
}

static void event(Game *g, EventBatch *out, int actor, const char *type, const char *format, ...) {
    assert(out->count < sizeof out->events / sizeof out->events[0]);
    MatchEvent *e = &out->events[out->count++];
    e->seq = ++g->eventSeq;
    e->revision = g->revision;
    e->actor = actor;
    snprintf(e->type, sizeof e->type, "%s", type);
    va_list args;
    va_start(args, format);
    vsnprintf(e->payload, sizeof e->payload, format, args);
    va_end(args);
}

static bool matches(const Game *g, const Card *c) {
    return c->color == WILD_COLOR || c->color == g->currentColor ||
           c->value == g->discard.cards[g->discard.count-1].value;
}

bool game_playable(const Game *g, Seat actor, const Card *c) {
    return g && c && actor < 4 && g->phase == PLAYING && g->turn == actor &&
           (!g->drawnCardId[0] || !strcmp(c->id, g->drawnCardId)) && matches(g,c);
}

bool game_can_say_uno(const Game *g, Seat s) {
    return g && s < 4 && g->phase == PLAYING && !g->watch[s].declared &&
           (g->watch[s].active || (s == g->turn && g->hands[s].count == 2));
}

static void give(Game *g, Seat s, int count, EventBatch *out) {
    for (int i = 0; i < count; i++) {
        if (!g->deck.count) {
            if (g->discard.count == 1) break;
            Card top = pop(&g->discard);
            g->deck = g->discard;
            g->discard.count = 0;
            push(&g->discard, top);
            shuffle(g);
        }
        Card c = pop(&g->deck);
        push(&g->hands[s], c);
        g->watch[s].active = g->watch[s].declared = false;
        event(g,out,-1,"CARD_DRAWN","{\"seat\":%u,\"cardId\":\"%s\"}",(unsigned)s,c.id);
    }
}

static Seat next(const Game *g, int steps) {
    return (Seat)((4 + g->turn + steps * g->direction) % 4);
}

static void advance(Game *g, int steps, EventBatch *out) {
    if (g->hands[g->turn].count > 1) g->watch[g->turn].declared = false;
    g->turn = next(g,steps);
    g->turnNo++;
    g->drawnCardId[0] = 0;
    event(g,out,-1,"TURN","{\"turn\":%u,\"turnNo\":%u,\"direction\":%d}",
          (unsigned)g->turn,g->turnNo,g->direction);
}

static void play(Game *g, Seat s, Card c, Color chosen, EventBatch *out) {
    Pile *h = &g->hands[s];
    g->currentColor = c.color == WILD_COLOR ? chosen : c.color;
    int kept = 0, discarded = 0;
    /* All extras fit in one event, even if a hand contains every card of its color. */
    char extras[CARD_COUNT * 8 + 1] = "";
    size_t used = 0;
    for (int i = 0; i < h->count; i++) {
        Card other = h->cards[i];
        if (!strcmp(other.id,c.id)) continue;
        if (c.value == DISCARD_ALL && other.color == c.color) {
            push(&g->discard,other);
            used += (size_t)snprintf(extras+used,sizeof extras-used,
                                     "%s\"%s\"", discarded ? "," : "",other.id);
            discarded++;
        } else h->cards[kept++] = other;
    }
    h->count = kept;
    push(&g->discard,c);
    g->drawnCardId[0] = 0;
    PlayerStats *stats = &g->stats[s];
    stats->played += 1 + discarded;
    stats->attacks += c.value == DRAW_TWO || c.value == WILD_FOUR;
    stats->plus4 += c.value == WILD_FOUR;
    event(g,out,s,"PLAY","{\"cardId\":\"%s\",\"color\":%d,\"discarded\":[%s],"
          "\"played\":%u,\"attacks\":%u,\"plus4\":%u}",c.id,g->currentColor,extras,
          stats->played,stats->attacks,stats->plus4);
    if (!kept) {
        g->phase = FINISHED;
        g->winnerSeat = s;
        g->winnerTeam = g->team[s];
        for (int i=0; i<4; i++) g->watch[i].active = false;
        event(g,out,-1,"WINNER","{\"seat\":%u,\"team\":%d}",(unsigned)s,g->winnerTeam);
        return;
    }
    if (kept == 1 && !g->watch[s].declared) {
        g->watch[s].active = true;
        g->watch[s].generation++;
    }
    if (c.value == REVERSE) g->direction = -g->direction;
    bool attack = c.value == DRAW_TWO || c.value == WILD_FOUR;
    if (attack) give(g,next(g,1),c.value == DRAW_TWO ? 2 : 4,out);
    advance(g,(attack || c.value == SKIP) ? 2 : 1,out);
}

static void draft_pick(Game *g, int index, bool timeout, EventBatch *out) {
    Offer *offer = &g->draft.offers[index];
    g->revision++;
    push(&g->hands[g->turn],offer->card);
    event(g,out,timeout ? -1 : (int)g->turn,"DRAFT_PICK",
          "{\"seat\":%u,\"draftTurn\":%u,\"index\":%d,\"cardId\":\"%s\"}",
          (unsigned)g->turn,g->draft.turn,index,offer->card.id);
    *offer = (Offer){0};
    uint32_t t = ++g->draft.turn;
    if (t == 16) {
        g->phase = PLAYING;
        g->turn = 0;
        event(g,out,-1,"TURN","{\"turn\":0,\"turnNo\":1,\"direction\":1}");
    } else {
        if (t % 4 == 0) deal_offers(g);
        g->turn = (Seat)((t / 4 + t % 4) % 4);
    }
}

Status game_draft_timeout(Game *g, uint32_t expected_pick, EventBatch *out) {
    if (!g || !out) return BAD_REQUEST;
    out->count = 0;
    if (g->phase != DRAFTING) return INVALID_ACTION;
    if (expected_pick != g->draft.turn) return STALE_TURN;
    for (int i = 0; i < 4; i++) {
        if (g->draft.offers[i].occupied) {
            draft_pick(g,i,true,out);
            return OK;
        }
    }
    return INVALID_ACTION;
}

Status game_apply(Game *g, Seat s, const Intent *a, EventBatch *out) {
    if (!g || !a || !out || s >= 4) return BAD_REQUEST;
    out->count = 0;
    if (g->phase == DRAFTING) {
        if (a->kind != DRAFT_PICK) return INVALID_ACTION;
        if (a->draftTurn != g->draft.turn) return STALE_TURN;
        if (s != g->turn) return NOT_YOUR_TURN;
        if (a->index < 0 || a->index >= 4 || !g->draft.offers[a->index].occupied)
            return INVALID_ACTION;
        draft_pick(g,a->index,false,out);
        return OK;
    }
    if (g->phase != PLAYING) return INVALID_ACTION;
    if (a->kind == SAY_UNO) {
        if (!game_can_say_uno(g,s)) return INVALID_ACTION;
        g->revision++;
        g->watch[s].declared = true;
        g->watch[s].active = false;
        event(g,out,s,"SAY_UNO","{\"unoCalls\":%u}",++g->stats[s].unoCalls);
        return OK;
    }
    if (a->turnNo != g->turnNo) return STALE_TURN;
    if (s != g->turn) return NOT_YOUR_TURN;
    Pile *h = &g->hands[s];
    switch (a->kind) {
    case PLAY:
        for (int i=0; i<h->count; i++) {
            Card c = h->cards[i];
            if (strncmp(c.id,a->cardId,sizeof c.id)) continue;
            if (!game_playable(g,s,&c) ||
                (c.color == WILD_COLOR ? a->color < RED || a->color > BLUE :
                 a->color != NO_COLOR && a->color != c.color)) return INVALID_ACTION;
            g->revision++;
            play(g,s,c,a->color,out);
            return OK;
        }
        return INVALID_ACTION;
    case DRAW: {
        if (g->drawnCardId[0]) return INVALID_ACTION;
        g->revision++;
        int before = h->count;
        event(g,out,s,"DRAW","{}");
        give(g,s,1,out);
        if (h->count > before && matches(g,&h->cards[before]))
            memcpy(g->drawnCardId,h->cards[before].id,sizeof g->drawnCardId);
        else advance(g,1,out);
        return OK;
    }
    case PASS:
        if (!g->drawnCardId[0]) return INVALID_ACTION;
        g->revision++;
        event(g,out,s,"PASS","{}");
        advance(g,1,out);
        return OK;
    default: return INVALID_ACTION;
    }
}

Status game_timeout(Game *g, uint32_t expected_turn, EventBatch *out) {
    if (!g || !out) return BAD_REQUEST;
    out->count = 0;
    if (g->phase != PLAYING) return INVALID_ACTION;
    if (expected_turn != g->turnNo) return STALE_TURN;
    g->revision++;
    event(g,out,-1,"TIMEOUT","{\"seat\":%u}",(unsigned)g->turn);
    if (!g->drawnCardId[0]) give(g,g->turn,1,out);
    advance(g,1,out);
    return OK;
}

Status game_uno_penalty(Game *g, Seat s, uint32_t generation, EventBatch *out) {
    if (!g || !out || s >= 4) return BAD_REQUEST;
    out->count = 0;
    if (g->phase != PLAYING || !g->watch[s].active ||
        g->watch[s].generation != generation) return INVALID_ACTION;
    g->revision++;
    g->watch[s].active = false;
    event(g,out,-1,"UNO_PENALTY","{\"seat\":%u,\"penalties\":%u}",(unsigned)s,++g->stats[s].penalties);
    give(g,s,2,out);
    return OK;
}
