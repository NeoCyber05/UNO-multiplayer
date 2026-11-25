#include "server/game.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static EventBatch events;
static const PlayerSpec PLAYERS[4] = {{0}};

static void countCards(const Pile *pile, int seen[CARD_COUNT + 1]) {
    assert(pile->count >= 0 && pile->count <= CARD_COUNT);
    for (int i = 0; i < pile->count; i++) {
        int id;
        assert(sscanf(pile->cards[i].id, "c%d", &id) == 1 && id >= 1 && id <= CARD_COUNT);
        assert(++seen[id] == 1);
    }
}

static void conservation(const Game *game) {
    int seen[CARD_COUNT + 1] = {0};
    countCards(&game->deck, seen);
    countCards(&game->discard, seen);
    for (int seat = 0; seat < 4; seat++)
        countCards(&game->hands[seat], seen);
    for (int i = 1; i <= CARD_COUNT; i++)
        assert(seen[i] == 1);
}

static Status act(Game *game, Seat seat, IntentKind kind, const char *id, Color color) {
    Intent intent = {.kind = kind, .turnNo = game->turnNo, .color = color};
    if (id) snprintf(intent.cardId, sizeof intent.cardId, "%s", id);
    return gameApply(game, seat, &intent, &events);
}

static Game setup(Value value) {
    Game game;
    assert(gameInit(&game, CLASSIC, PLAYERS, 1) == OK);
    game.hands[0] = (Pile){.count = 2, .cards = {{"c1", RED, value}, {"c2", BLUE, ONE}}};
    game.discard = (Pile){.count = 1, .cards = {{"c3", RED, ZERO}}};
    game.currentColor = RED;
    return game;
}

static void testCardEffects(void) {
    Value values[] = {SKIP, REVERSE, DRAW_TWO, WILD_FOUR};
    int targets[] = {2, 3, 2, 2}, draws[] = {0, 0, 2, 4};
    for (int i = 0; i < 4; i++) {
        Game game = setup(values[i]);
        Color choice = NO_COLOR;
        if (values[i] == WILD_FOUR) {
            game.hands[0].cards[0].color = WILD_COLOR;
            choice = GREEN;
        }
        assert(act(&game, 0, PLAY, "c1", choice) == OK);
        assert(game.turn == targets[i] && game.hands[1].count == 7 + draws[i]);
        assert(game.currentColor == (choice ? choice : RED));
        assert(game.direction == (values[i] == REVERSE ? -1 : 1));
        assert(game.stats[0].plus4 == (values[i] == WILD_FOUR));
    }
    Game game = setup(DISCARD_ALL);
    game.hands[0].cards[game.hands[0].count++] = (Card){"c4", RED, DRAW_TWO};
    assert(act(&game, 0, SAY_UNO, NULL, NO_COLOR) == OK);
    assert(act(&game, 0, PLAY, "c1", NO_COLOR) == OK);
    assert(game.hands[0].count == 1 && game.stats[0].played == 2 && game.hands[1].count == 7);
    assert(game.discard.count == 3 && !strcmp(game.discard.cards[2].id, "c1"));
    game = setup(DRAW_TWO);
    game.hands[0].count = 1;
    assert(act(&game, 0, PLAY, "c1", NO_COLOR) == OK);
    assert(game.phase == FINISHED && game.winnerSeat == 0 && game.hands[1].count == 7);
}

static void testInvalidAndDraw(void) {
    Game game = setup(ONE), before = game;
    assert(act(&game, 1, PLAY, "c1", NO_COLOR) == NOT_YOUR_TURN);
    assert(act(&game, 4, DRAW, NULL, NO_COLOR) != OK);
    assert(act(&game, 0, PLAY, "c2", NO_COLOR) == INVALID_ACTION);
    assert(act(&game, 0, PLAY, "c99", NO_COLOR) == INVALID_ACTION);
    Intent stale = {.kind = PLAY, .turnNo = game.turnNo - 1};
    assert(gameApply(&game, 0, &stale, &events) == STALE_TURN);
    assert(act(&game, 0, PASS, NULL, NO_COLOR) == INVALID_ACTION);
    assert(!memcmp(&before, &game, sizeof game));
    game.hands[0].cards[0] = (Card){"c1", WILD_COLOR, WILD_FOUR};
    before = game;
    assert(act(&game, 0, PLAY, "c1", NO_COLOR) == INVALID_ACTION);
    assert(act(&game, 0, PLAY, "c1", WILD_COLOR) == INVALID_ACTION);
    assert(!memcmp(&before, &game, sizeof game));

    game.deck.cards[game.deck.count - 1] = (Card){"c5", RED, TWO};
    assert(act(&game, 0, DRAW, NULL, NO_COLOR) == OK);
    assert(game.turnNo == before.turnNo && game.turn == 0);
    before = game;
    assert(act(&game, 0, PLAY, "c1", BLUE) == INVALID_ACTION);
    assert(act(&game, 0, DRAW, NULL, NO_COLOR) == INVALID_ACTION);
    assert(!memcmp(&before, &game, sizeof game));
    assert(act(&game, 0, PLAY, "c5", NO_COLOR) == OK && game.turn == 1);
    game = before;
    assert(act(&game, 0, PASS, NULL, NO_COLOR) == OK && game.turn == 1);
    game = before;
    assert(gameTimeout(&game, game.turnNo, &events) == OK && game.hands[0].count == 3);
    Game afterTimeout = game;
    assert(gameTimeout(&game, before.turnNo, &events) == STALE_TURN);
    assert(!memcmp(&afterTimeout, &game, sizeof game));

    game = setup(ONE);
    game.deck.count = 0;
    game.discard.cards[game.discard.count++] = (Card){"c4", RED, ONE};
    assert(act(&game, 0, DRAW, NULL, NO_COLOR) == OK && game.hands[0].count == 3);
    assert(game.discard.count == 1 && !strcmp(game.discard.cards[0].id, "c4"));
    assert(act(&game, 0, PASS, NULL, NO_COLOR) == OK);
    assert(act(&game, 1, DRAW, NULL, NO_COLOR) == OK && game.turn == 2 && game.hands[1].count == 7);

    game = setup(WILD_FOUR);
    game.hands[0].cards[0].color = WILD_COLOR;
    game.deck.count = 0;
    assert(act(&game, 0, PLAY, "c1", GREEN) == OK && game.hands[1].count == 8);
    assert(game.turn == 2 && game.discard.count == 1 && !strcmp(game.discard.cards[0].id, "c1"));
}

static void testUno(void) {
    Game game = setup(ONE);
    assert(act(&game, 0, PLAY, "c1", NO_COLOR) == OK);
    assert(game.hands[0].count == 3 && game.stats[0].penalties == 1);
    assert(!gameCanSayUno(&game, 0));
    game = setup(ONE);
    assert(act(&game, 0, SAY_UNO, NULL, NO_COLOR) == OK);
    assert(act(&game, 0, PLAY, "c1", NO_COLOR) == OK);
    assert(game.hands[0].count == 1 && game.stats[0].penalties == 0);

    game = setup(ONE);
    game.currentColor = GREEN;
    assert(!gameCanSayUno(&game, 0));
    assert(act(&game, 0, SAY_UNO, NULL, NO_COLOR) == INVALID_ACTION && game.stats[0].unoCalls == 0);

    game = setup(DISCARD_ALL);
    game.hands[0].cards[1] = (Card){"c2", RED, ONE};
    strcpy(game.drawnCardId, "c1");
    assert(!gameCanSayUno(&game, 0));
    assert(act(&game, 0, PLAY, "c1", NO_COLOR) == OK && game.phase == FINISHED);

    game = setup(DISCARD_ALL);
    game.hands[0].cards[game.hands[0].count++] = (Card){"c5", RED, TWO};
    assert(gameCanSayUno(&game, 0));
    assert(act(&game, 0, SAY_UNO, NULL, NO_COLOR) == OK);
    assert(act(&game, 0, PLAY, "c1", NO_COLOR) == OK && game.hands[0].count == 1);

    for (int timeout = 0; timeout <= 1; timeout++) {
        game = setup(ONE);
        game.hands[0].count = 1;
        game.deck.cards[game.deck.count - 1] = (Card){"c5", RED, TWO};
        assert(act(&game, 0, DRAW, NULL, NO_COLOR) == OK && game.hands[0].count == 2);
        assert(act(&game, 0, SAY_UNO, NULL, NO_COLOR) == OK);
        if (timeout)
            assert(gameTimeout(&game, game.turnNo, &events) == OK);
        else
            assert(act(&game, 0, PASS, NULL, NO_COLOR) == OK);
        while (game.turn != 0)
            assert(gameTimeout(&game, game.turnNo, &events) == OK);
        assert(gameCanSayUno(&game, 0));
        assert(act(&game, 0, PLAY, "c1", NO_COLOR) == OK);
        assert(game.hands[0].count == 3 && game.stats[0].penalties == 1);
    }
}

int main(void) {
    Game game, same;
    assert(gameInit(&game, CLASSIC, PLAYERS, 42) == OK);
    assert(gameInit(&same, CLASSIC, PLAYERS, 42) == OK && !memcmp(&game, &same, sizeof game));
    for (int seat = 0; seat < 4; seat++)
        assert(game.hands[seat].count == 7);
    assert(game.discard.count == 1 && game.discard.cards[0].value <= NINE);
    for (int i = 0; i < 1000; i++) {
        conservation(&game);
        if (game.phase == FINISHED) assert(gameInit(&game, CLASSIC, PLAYERS, (uint64_t)i) == OK);
        Seat seat = game.turn;
        int k = 0;
        while (k < game.hands[seat].count && !gamePlayable(&game, seat, &game.hands[seat].cards[k]))
            k++;
        if (k < game.hands[seat].count) {
            Card card = game.hands[seat].cards[k];
            assert(act(&game, seat, PLAY, card.id, card.color == WILD_COLOR ? RED : NO_COLOR) ==
                   OK);
        } else
            assert(act(&game, seat, DRAW, NULL, NO_COLOR) == OK);
        assert(events.count > 0 && events.events[events.count - 1].revision == game.revision);
    }
    conservation(&game);
    testCardEffects();
    testInvalidAndDraw();
    testUno();
    puts("Classic rules, immediate UNO penalty and conservation passed");
}
