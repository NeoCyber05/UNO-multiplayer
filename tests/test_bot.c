#include "server/bot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static EventBatch events;
static const PlayerSpec players[4] = {{0}};

static Intent choose(Game *g, BotLevel level, uint64_t *rng) {
    GameView v;
    assert(view_build(g,g->turn,&v) == OK);
    uint64_t copy = *rng;
    Intent a, b;
    assert(bot_choose(&v,level,rng,&a) == OK);
    assert(bot_choose(&v,level,&copy,&b) == OK && *rng == copy);
    assert(a.kind == b.kind && a.index == b.index && a.color == b.color && !strcmp(a.cardId,b.cardId));
    return a;
}

static void tactics(void) {
    Game g;
    uint64_t rng = 7;
    assert(game_init(&g,SIDE,players,3) == OK);
    g.currentColor = RED;
    g.hands[0] = (Pile){.count=3,.cards={{"c1",RED,ONE},{"c2",RED,DRAW_TWO},{"c3",BLUE,TWO}}};
    g.hands[1].count = 1;
    Intent a = choose(&g,HARD,&rng);
    assert(a.kind == PLAY && !strcmp(a.cardId,"c1"));
    g.direction = -1;
    g.hands[3].count = 1;
    a = choose(&g,HARD,&rng);
    assert(a.kind == PLAY && !strcmp(a.cardId,"c2"));
    g.hands[0].cards[0] = (Card){"c1",RED,DISCARD_ALL};
    g.hands[0].cards[2].color = RED;
    a = choose(&g,HARD,&rng);
    assert(a.kind == PLAY && !strcmp(a.cardId,"c1"));

    g.currentColor = GREEN;
    g.discard.cards[0].value = THREE;
    assert(choose(&g,NORMAL,&rng).kind == DRAW);
    GameView v;
    assert(view_build(&g,0,&v) == OK);
    v.canDraw = false;
    v.canPass = true;
    assert(bot_choose(&v,EASY,&rng,&a) == OK && a.kind == PASS);
    assert(view_build(&g,1,&v) == OK && bot_choose(&v,HARD,&rng,&a) != OK);
}

int main(void) {
    tactics();
    bool uno = false, wild = false, draft = false, draw = false;
    for (BotLevel level=EASY; level<=HARD; level++) {
        for (uint64_t seed=0; seed<100; seed++) {
            Game g;
            uint64_t rng = seed;
            assert(game_init(&g,(Mode)(seed % 4),players,seed) == OK);
            int steps = 0;
            while (g.phase != FINISHED && steps++ < 2000) {
                Intent a = choose(&g,level,&rng);
                assert(game_apply(&g,g.turn,&a,&events) == OK);
                uno |= a.kind == SAY_UNO;
                draft |= a.kind == DRAFT_PICK;
                wild |= a.kind == PLAY && a.color != NO_COLOR;
                draw |= a.kind == DRAW;
            }
            assert(g.phase == FINISHED);
        }
    }
    assert(uno && wild && draft && draw);
    puts("Three bot levels completed 300 seeded matches");
}
