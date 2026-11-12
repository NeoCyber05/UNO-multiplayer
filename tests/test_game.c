#include "server/game.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static EventBatch events;
static const PlayerSpec players[4] = {{0}};

static void count_cards(const Pile *pile, int seen[113]) {
    assert(pile->count >= 0 && pile->count <= 112);
    for (int i = 0; i < pile->count; i++) {
        int id;
        assert(sscanf(pile->cards[i].id, "c%d", &id) == 1 && id >= 1 && id <= 112);
        assert(++seen[id] == 1);
    }
}

static void conservation(const Game *g) {
    int seen[113] = {0};
    count_cards(&g->deck, seen);
    count_cards(&g->discard, seen);
    for (int s = 0; s < 4; s++) count_cards(&g->hands[s], seen);
    for (int i = 1; i <= 112; i++) assert(seen[i] == 1);
}

static Status act(Game *g, Seat s, IntentKind kind, const char *id, Color color) {
    Intent a = {.kind=kind, .turnNo=g->turnNo, .color=color};
    if (id) snprintf(a.cardId, sizeof a.cardId, "%s", id);
    return game_apply(g, s, &a, &events);
}

static Game setup(Value value) {
    Game g;
    assert(game_init(&g, CLASSIC, players, 1) == OK);
    g.hands[0] = (Pile){.count=2, .cards={{"c1", RED, value}, {"c2", BLUE, ONE}}};
    g.discard = (Pile){.count=1, .cards={{"c3", RED, ZERO}}};
    g.currentColor = RED;
    return g;
}

static void effects(void) {
    Value values[] = {SKIP, REVERSE, DRAW_TWO, WILD_FOUR};
    int targets[] = {2,3,2,2}, draws[] = {0,0,2,4};
    for (int i = 0; i < 4; i++) {
        Game g = setup(values[i]);
        Color choice = NO_COLOR;
        if (values[i] == WILD_FOUR) {
            g.hands[0].cards[0].color = WILD_COLOR;
            choice = GREEN;
        }
        assert(act(&g,0,PLAY,"c1",choice) == OK);
        assert(g.turn == targets[i] && g.hands[1].count == 7 + draws[i]);
        assert(g.currentColor == (choice ? choice : RED));
        assert(g.direction == (values[i] == REVERSE ? -1 : 1));
        assert(g.stats[0].plus4 == (values[i] == WILD_FOUR));
    }
    Game g = setup(DISCARD_ALL);
    g.hands[0].cards[g.hands[0].count++] = (Card){"c4", RED, DRAW_TWO};
    assert(act(&g,0,PLAY,"c1",NO_COLOR) == OK);
    assert(g.hands[0].count == 1 && g.stats[0].played == 2 && g.hands[1].count == 7);
    assert(g.discard.count == 3 && !strcmp(g.discard.cards[2].id,"c1"));
    g = setup(DRAW_TWO);
    g.hands[0].count = 1;
    assert(act(&g,0,PLAY,"c1",NO_COLOR) == OK);
    assert(g.phase == FINISHED && g.winnerSeat == 0 && g.hands[1].count == 7);
}

static void invalid_and_draw(void) {
    Game g = setup(ONE), before = g;
    assert(act(&g,1,PLAY,"c1",NO_COLOR) == NOT_YOUR_TURN);
    assert(act(&g,4,DRAW,NULL,NO_COLOR) != OK);
    assert(act(&g,0,PLAY,"c2",NO_COLOR) == INVALID_ACTION);
    assert(act(&g,0,PLAY,"c99",NO_COLOR) == INVALID_ACTION);
    Intent stale = {.kind=PLAY,.turnNo=g.turnNo-1};
    assert(game_apply(&g,0,&stale,&events) == STALE_TURN);
    assert(act(&g,0,PASS,NULL,NO_COLOR) == INVALID_ACTION);
    assert(!memcmp(&before,&g,sizeof g));
    g.hands[0].cards[0] = (Card){"c1",WILD_COLOR,WILD_FOUR};
    before = g;
    assert(act(&g,0,PLAY,"c1",NO_COLOR) == INVALID_ACTION);
    assert(act(&g,0,PLAY,"c1",WILD_COLOR) == INVALID_ACTION);
    assert(!memcmp(&before,&g,sizeof g));

    g.deck.cards[g.deck.count-1] = (Card){"c5",RED,TWO};
    assert(act(&g,0,DRAW,NULL,NO_COLOR) == OK);
    assert(g.turnNo == before.turnNo && g.turn == 0);
    before = g;
    assert(act(&g,0,PLAY,"c1",BLUE) == INVALID_ACTION);
    assert(act(&g,0,DRAW,NULL,NO_COLOR) == INVALID_ACTION);
    assert(!memcmp(&before,&g,sizeof g));
    assert(act(&g,0,PLAY,"c5",NO_COLOR) == OK && g.turn == 1);
    g = before;
    assert(act(&g,0,PASS,NULL,NO_COLOR) == OK && g.turn == 1);
    g = before;
    assert(game_timeout(&g,g.turnNo,&events) == OK && g.hands[0].count == 3);

    g = setup(ONE);
    g.deck.count = 0;
    g.discard.cards[g.discard.count++] = (Card){"c4",RED,ONE};
    assert(act(&g,0,DRAW,NULL,NO_COLOR) == OK && g.hands[0].count == 3);
    assert(g.discard.count == 1 && !strcmp(g.discard.cards[0].id,"c4"));
    assert(act(&g,0,PASS,NULL,NO_COLOR) == OK);
    assert(act(&g,1,DRAW,NULL,NO_COLOR) == OK && g.turn == 2 && g.hands[1].count == 7);
}

static void uno(void) {
    Game g = setup(ONE);
    g.hands[1] = (Pile){.count=2,.cards={{"c5",RED,TWO},{"c6",BLUE,ONE}}};
    assert(act(&g,0,PLAY,"c1",NO_COLOR) == OK);
    uint32_t generation = g.watch[0].generation;
    assert(act(&g,1,PLAY,"c5",NO_COLOR) == OK && g.watch[0].active && g.watch[1].active);
    assert(act(&g,1,SAY_UNO,NULL,NO_COLOR) == OK && !g.watch[1].active && g.watch[0].active);
    assert(game_uno_penalty(&g,0,generation,&events) == OK && g.hands[0].count == 3);
    Game before = g;
    assert(game_uno_penalty(&g,0,generation,&events) != OK && !memcmp(&before,&g,sizeof g));
    g = setup(ONE);
    assert(act(&g,0,SAY_UNO,NULL,NO_COLOR) == OK);
    assert(act(&g,0,PLAY,"c1",NO_COLOR) == OK && !g.watch[0].active);
}

int main(void) {
    Game g, same;
    assert(game_init(&g,CLASSIC,players,42) == OK);
    assert(game_init(&same,CLASSIC,players,42) == OK && !memcmp(&g,&same,sizeof g));
    for (int s=0; s<4; s++) assert(g.hands[s].count == 7);
    assert(g.discard.count == 1 && g.discard.cards[0].value <= NINE);
    for (int i=0; i<1000; i++) {
        conservation(&g);
        Seat s = g.turn;
        if (g.phase == FINISHED) {
            assert(game_init(&g,CLASSIC,players,(uint64_t)i) == OK);
            continue;
        }
        int k = 0;
        while (k < g.hands[s].count && !game_playable(&g,s,&g.hands[s].cards[k])) k++;
        if (k < g.hands[s].count) {
            Card c = g.hands[s].cards[k];
            assert(act(&g,s,PLAY,c.id,c.color == WILD_COLOR ? RED : NO_COLOR) == OK);
        } else assert(act(&g,s,DRAW,NULL,NO_COLOR) == OK);
        assert(events.count > 0 && events.events[events.count-1].revision == g.revision);
    }
    conservation(&g);
    effects();
    invalid_and_draw();
    uno();
    puts("Classic rules, UNO watches and conservation passed");
}
