#include "game.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
    OPENING_HAND_SIZE = 7,
    DRAFT_OPENING_HAND_SIZE = 4,
    DRAFT_ROUND_COUNT = 4,
    WILD_COPIES_PER_VALUE = 4,
    DRAW_TWO_PENALTY = 2,
    WILD_FOUR_PENALTY = 4,
    MISSED_UNO_PENALTY = 2
};

uint64_t gameRandom(uint64_t *state) {
    uint64_t word = (*state += UINT64_C(0x9e3779b97f4a7c15));
    word = (word ^ (word >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    word = (word ^ (word >> 27)) * UINT64_C(0x94d049bb133111eb);
    return word ^ (word >> 31);
}

static Card popCard(Pile *pile) {
    return pile->cards[--pile->count];
}

static void pushCard(Pile *pile, Card card) {
    pile->cards[pile->count++] = card;
}

/* Fisher–Yates dùng trạng thái riêng của ván để cùng seed cho cùng thứ tự bài. */
static void shuffleDeck(Game *game) {
    for (int i = game->deck.count - 1; i > 0; i--) {
        int j = (int)(gameRandom(&game->rng) % (i + 1));
        Card card = game->deck.cards[i];
        game->deck.cards[i] = game->deck.cards[j];
        game->deck.cards[j] = card;
    }
}

static void addCard(Pile *deck, Color color, Value value) {
    Card card = {.color = color, .value = value};
    snprintf(card.id, sizeof card.id, "c%u", (unsigned char)(deck->count + 1));
    pushCard(deck, card);
}

static void dealOffers(Game *game) {
    for (int i = 0; i < DRAFT_OFFER_COUNT; i++)
        game->draft.offers[i] = (Offer){.occupied = true, .card = popCard(&game->deck)};
}

Status gameInit(Game *game, Mode mode, const PlayerSpec players[SEAT_COUNT], uint64_t seed) {
    if (!game || !players || (unsigned)mode > DRAFT) return BAD_REQUEST;
    for (int seat = 0; seat < SEAT_COUNT; seat++) {
        if (!memchr(players[seat].name, 0, sizeof players[seat].name) ||
            !memchr(players[seat].avatar, 0, sizeof players[seat].avatar) ||
            (unsigned)players[seat].botLevel > HARD)
            return BAD_REQUEST;
    }
    memset(game, 0, sizeof *game);
    game->mode = mode;
    game->phase = mode == DRAFT ? DRAFTING : PLAYING;
    game->rng = seed;
    game->direction = 1;
    game->revision = game->turnNo = 1;
    game->winnerSeat = game->winnerTeam = -1;
    memcpy(game->players, players, sizeof game->players);
    for (int seat = 0; seat < SEAT_COUNT; seat++) {
        game->team[seat] = -1;
        /* 2v2 ghép hai ghế đối diện. Side ghép hai ghế liền nhau. */
        if (mode == TWO_V_TWO) game->team[seat] = seat % 2;
        if (mode == SIDE) game->team[seat] = seat / 2;
    }
    for (Color color = RED; color <= BLUE; color++) {
        addCard(&game->deck, color, ZERO);
        for (Value value = ONE; value <= DRAW_TWO; value++) {
            addCard(&game->deck, color, value);
            addCard(&game->deck, color, value);
        }
        addCard(&game->deck, color, DISCARD_ALL);
    }
    for (int i = 0; i < WILD_COPIES_PER_VALUE; i++) {
        addCard(&game->deck, WILD_COLOR, WILD);
        addCard(&game->deck, WILD_COLOR, WILD_FOUR);
    }
    shuffleDeck(game);
    int openingHandSize = mode == DRAFT ? DRAFT_OPENING_HAND_SIZE : OPENING_HAND_SIZE;
    for (int round = 0; round < openingHandSize; round++) {
        for (Seat seat = 0; seat < SEAT_COUNT; seat++) {
            pushCard(&game->hands[seat], popCard(&game->deck));
        }
    }
    if (mode == DRAFT) dealOffers(game);
    // Mở bàn bằng lá số để tránh kích hoạt hiệu ứng trước lượt đầu tiên.
    int openingIndex = 0;
    while (game->deck.cards[openingIndex].value > NINE)
        openingIndex++;
    Card openingCard = game->deck.cards[openingIndex];
    game->deck.cards[openingIndex] = popCard(&game->deck);
    pushCard(&game->discard, openingCard);
    game->currentColor = openingCard.color;
    return OK;
}

/* Các sự kiện cùng nước đi dùng chung revision nhưng có số thứ tự riêng. */
static void recordEvent(Game *game, EventBatch *out, int actor, const char *type,
                        const char *format, ...) {
    assert(out->count < sizeof out->events / sizeof out->events[0]);
    MatchEvent *entry = &out->events[out->count++];
    entry->seq = ++game->eventSeq;
    entry->revision = game->revision;
    entry->actor = actor;
    snprintf(entry->type, sizeof entry->type, "%s", type);
    va_list args;
    va_start(args, format);
    vsnprintf(entry->payload, sizeof entry->payload, format, args);
    va_end(args);
}

static bool matchesTopCard(const Game *game, const Card *card) {
    return card->color == WILD_COLOR || card->color == game->currentColor ||
           card->value == game->discard.cards[game->discard.count - 1].value;
}

bool gamePlayable(const Game *game, Seat actor, const Card *card) {
    return game && card && actor < SEAT_COUNT && game->phase == PLAYING && game->turn == actor &&
           (!game->drawnCardId[0] || !strcmp(card->id, game->drawnCardId)) &&
           matchesTopCard(game, card);
}

bool gameCanSayUno(const Game *game, Seat seat) {
    if (!game || seat >= SEAT_COUNT || game->phase != PLAYING || seat != game->turn ||
        game->unoDeclared[seat])
        return false;

    const Pile *hand = &game->hands[seat];
    if (hand->count < 2) return false;

    // Lá bỏ cùng màu có thể đưa tay bài nhiều hơn hai lá về đúng một lá.
    int colorCounts[WILD_COLOR + 1] = {0};
    for (int index = 0; index < hand->count; index++) {
        colorCounts[hand->cards[index].color]++;
    }
    for (int index = 0; index < hand->count; index++) {
        const Card *card = &hand->cards[index];
        int removed = card->value == DISCARD_ALL ? colorCounts[card->color] : 1;
        if (hand->count - removed == 1 && gamePlayable(game, seat, card)) return true;
    }
    return false;
}

static void drawCards(Game *game, Seat seat, int count, EventBatch *out) {
    for (int i = 0; i < count; i++) {
        if (!game->deck.count) {
            if (game->discard.count == 1) break;
            /* Giữ lá trên cùng để luật so màu và số vẫn có lá đối chiếu. */
            Card top = popCard(&game->discard);
            game->deck = game->discard;
            game->discard.count = 0;
            pushCard(&game->discard, top);
            shuffleDeck(game);
        }
        Card card = popCard(&game->deck);
        pushCard(&game->hands[seat], card);
        game->unoDeclared[seat] = false;
        recordEvent(game, out, -1, "CARD_DRAWN", "{\"seat\":%u,\"cardId\":\"%s\"}", (unsigned)seat,
                    card.id);
    }
}

static Seat nextSeat(const Game *game, int steps) {
    return (Seat)((SEAT_COUNT + game->turn + steps * game->direction) % SEAT_COUNT);
}

static void advanceTurn(Game *game, int steps, EventBatch *out) {
    game->unoDeclared[game->turn] = false;
    game->turn = nextSeat(game, steps);
    game->turnNo++;
    game->drawnCardId[0] = 0;
    recordEvent(game, out, -1, "TURN", "{\"turn\":%u,\"turnNo\":%u,\"direction\":%d}",
                (unsigned)game->turn, game->turnNo, game->direction);
}

static void playCard(Game *game, Seat seat, Card card, Color chosen, EventBatch *out) {
    Pile *hand = &game->hands[seat];
    game->currentColor = card.color == WILD_COLOR ? chosen : card.color;
    int kept = 0, discarded = 0;
    /* Chừa đủ chỗ cho mọi mã lá bỏ kèm trong cùng sự kiện PLAY. */
    char extras[CARD_COUNT * 8 + 1] = "";
    size_t used = 0;
    for (int i = 0; i < hand->count; i++) {
        Card other = hand->cards[i];
        if (!strcmp(other.id, card.id)) continue;
        if (card.value == DISCARD_ALL && other.color == card.color) {
            pushCard(&game->discard, other);
            used += (size_t)snprintf(extras + used, sizeof extras - used, "%s\"%s\"",
                                     discarded ? "," : "", other.id);
            discarded++;
        } else
            hand->cards[kept++] = other;
    }
    hand->count = kept;
    pushCard(&game->discard, card);
    game->drawnCardId[0] = 0;
    PlayerStats *stats = &game->stats[seat];
    stats->played += 1 + discarded;
    stats->attacks += card.value == DRAW_TWO || card.value == WILD_FOUR;
    stats->plus4 += card.value == WILD_FOUR;
    recordEvent(game, out, seat, "PLAY",
                "{\"cardId\":\"%s\",\"color\":%d,\"discarded\":[%s],"
                "\"played\":%u,\"attacks\":%u,\"plus4\":%u}",
                card.id, game->currentColor, extras, stats->played, stats->attacks, stats->plus4);
    if (!kept) {
        game->phase = FINISHED;
        game->winnerSeat = seat;
        game->winnerTeam = game->team[seat];
        recordEvent(game, out, -1, "WINNER", "{\"seat\":%u,\"team\":%d}", (unsigned)seat,
                    game->winnerTeam);
        return;
    }
    if (card.value == REVERSE) game->direction = -game->direction;
    bool attack = card.value == DRAW_TWO || card.value == WILD_FOUR;
    if (attack)
        drawCards(game, nextSeat(game, 1),
                  card.value == DRAW_TWO ? DRAW_TWO_PENALTY : WILD_FOUR_PENALTY, out);
    /* UNO phải được hô trước khi đánh. Áp dụng phạt ngay trong nước đi này. */
    if (kept == 1 && !game->unoDeclared[seat]) {
        recordEvent(game, out, -1, "UNO_PENALTY", "{\"seat\":%u,\"penalties\":%u}", (unsigned)seat,
                    ++stats->penalties);
        drawCards(game, seat, MISSED_UNO_PENALTY, out);
    }
    advanceTurn(game, (attack || card.value == SKIP) ? 2 : 1, out);
}

static void draftPick(Game *game, int index, bool timeout, EventBatch *out) {
    Offer *offer = &game->draft.offers[index];
    game->revision++;
    pushCard(&game->hands[game->turn], offer->card);
    recordEvent(game, out, timeout ? -1 : (int)game->turn, "DRAFT_PICK",
                "{\"seat\":%u,\"draftTurn\":%u,\"index\":%d,\"cardId\":\"%s\"}",
                (unsigned)game->turn, game->draft.turn, index, offer->card.id);
    *offer = (Offer){0};
    uint32_t draftTurn = ++game->draft.turn;
    if (draftTurn == DRAFT_ROUND_COUNT * DRAFT_OFFER_COUNT) {
        game->phase = PLAYING;
        game->turn = 0;
        recordEvent(game, out, -1, "TURN", "{\"turn\":0,\"turnNo\":1,\"direction\":1}");
    } else {
        if (draftTurn % DRAFT_OFFER_COUNT == 0) dealOffers(game);
        /* Đổi ghế chọn đầu tiên qua mỗi vòng để chia đều lợi thế chọn trước. */
        game->turn =
            (Seat)((draftTurn / DRAFT_OFFER_COUNT + draftTurn % DRAFT_OFFER_COUNT) % SEAT_COUNT);
    }
}

Status gameDraftTimeout(Game *game, uint32_t expectedPick, EventBatch *out) {
    if (!game || !out) return BAD_REQUEST;
    out->count = 0;
    if (game->phase != DRAFTING) return INVALID_ACTION;
    if (expectedPick != game->draft.turn) return STALE_TURN;
    for (int i = 0; i < DRAFT_OFFER_COUNT; i++) {
        if (game->draft.offers[i].occupied) {
            draftPick(game, i, true, out);
            return OK;
        }
    }
    return INVALID_ACTION;
}

Status gameApply(Game *game, Seat seat, const Intent *intent, EventBatch *out) {
    if (!game || !intent || !out || seat >= SEAT_COUNT) return BAD_REQUEST;
    out->count = 0;
    if (game->phase == DRAFTING) {
        if (intent->kind != DRAFT_PICK) return INVALID_ACTION;
        if (intent->draftTurn != game->draft.turn) return STALE_TURN;
        if (seat != game->turn) return NOT_YOUR_TURN;
        if (intent->index < 0 || intent->index >= DRAFT_OFFER_COUNT ||
            !game->draft.offers[intent->index].occupied)
            return INVALID_ACTION;
        draftPick(game, intent->index, false, out);
        return OK;
    }
    if (game->phase != PLAYING) return INVALID_ACTION;
    /* SAY_UNO không mang turnNo. gameCanSayUno kiểm tra theo lượt hiện tại. */
    if (intent->kind == SAY_UNO) {
        if (!gameCanSayUno(game, seat)) return INVALID_ACTION;
        game->revision++;
        game->unoDeclared[seat] = true;
        recordEvent(game, out, seat, "SAY_UNO", "{\"unoCalls\":%u}", ++game->stats[seat].unoCalls);
        return OK;
    }
    if (intent->turnNo != game->turnNo) return STALE_TURN;
    if (seat != game->turn) return NOT_YOUR_TURN;
    Pile *hand = &game->hands[seat];
    switch (intent->kind) {
    case PLAY:
        for (int i = 0; i < hand->count; i++) {
            Card card = hand->cards[i];
            if (strncmp(card.id, intent->cardId, sizeof card.id)) continue;
            if (!gamePlayable(game, seat, &card) ||
                (card.color == WILD_COLOR
                     ? intent->color < RED || intent->color > BLUE
                     : intent->color != NO_COLOR && intent->color != card.color))
                return INVALID_ACTION;
            game->revision++;
            playCard(game, seat, card, intent->color, out);
            return OK;
        }
        return INVALID_ACTION;
    case DRAW: {
        if (game->drawnCardId[0]) return INVALID_ACTION;
        game->revision++;
        int before = hand->count;
        recordEvent(game, out, seat, "DRAW", "{}");
        drawCards(game, seat, 1, out);
        /* Chỉ giữ lượt nếu lá vừa rút đánh được. Người chơi có thể đánh lá đó hoặc bỏ qua. */
        if (hand->count > before && matchesTopCard(game, &hand->cards[before]))
            memcpy(game->drawnCardId, hand->cards[before].id, sizeof game->drawnCardId);
        else
            advanceTurn(game, 1, out);
        return OK;
    }
    case PASS:
        if (!game->drawnCardId[0]) return INVALID_ACTION;
        game->revision++;
        recordEvent(game, out, seat, "PASS", "{}");
        advanceTurn(game, 1, out);
        return OK;
    default:
        return INVALID_ACTION;
    }
}

Status gameTimeout(Game *game, uint32_t expectedTurn, EventBatch *out) {
    if (!game || !out) return BAD_REQUEST;
    out->count = 0;
    if (game->phase != PLAYING) return INVALID_ACTION;
    if (expectedTurn != game->turnNo) return STALE_TURN;
    game->revision++;
    recordEvent(game, out, -1, "TIMEOUT", "{\"seat\":%u}", (unsigned)game->turn);
    if (!game->drawnCardId[0]) drawCards(game, game->turn, 1, out);
    advanceTurn(game, 1, out);
    return OK;
}
