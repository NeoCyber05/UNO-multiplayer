#include "server/game/bot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static EventBatch events;
static const PlayerSpec PLAYERS[4] = {{0}};

static Intent choose(Game *game, BotLevel level, uint64_t *randomState) {
    GameView view;
    assert(viewBuild(game, game->turn, &view) == OK);
    uint64_t copy = *randomState;
    Intent intent, repeatedIntent;
    assert(botChoose(&view, level, randomState, &intent) == OK);
    assert(botChoose(&view, level, &copy, &repeatedIntent) == OK && *randomState == copy);
    assert(intent.kind == repeatedIntent.kind && intent.index == repeatedIntent.index &&
           intent.color == repeatedIntent.color && !strcmp(intent.cardId, repeatedIntent.cardId));
    return intent;
}

static void testTactics(void) {
    Game game;
    uint64_t randomState = 7;
    assert(gameInit(&game, SIDE, PLAYERS, 3) == OK);
    game.currentColor = RED;
    game.hands[0] =
        (Pile){.count = 3, .cards = {{"c1", RED, ONE}, {"c2", RED, DRAW_TWO}, {"c3", BLUE, TWO}}};
    game.hands[1].count = 1;
    Intent intent = choose(&game, HARD, &randomState);
    assert(intent.kind == PLAY && !strcmp(intent.cardId, "c1"));
    game.direction = -1;
    game.hands[3].count = 1;
    intent = choose(&game, HARD, &randomState);
    assert(intent.kind == PLAY && !strcmp(intent.cardId, "c2"));
    game.hands[0].cards[0] = (Card){"c1", RED, DISCARD_ALL};
    game.hands[0].cards[2].color = RED;
    intent = choose(&game, HARD, &randomState);
    assert(intent.kind == PLAY && !strcmp(intent.cardId, "c1"));

    game.currentColor = GREEN;
    game.discard.cards[0].value = THREE;
    assert(choose(&game, NORMAL, &randomState).kind == DRAW);
    GameView view;
    assert(viewBuild(&game, 0, &view) == OK);
    view.canDraw = false;
    view.canPass = true;
    assert(botChoose(&view, EASY, &randomState, &intent) == OK && intent.kind == PASS);
    assert(viewBuild(&game, 1, &view) == OK && botChoose(&view, HARD, &randomState, &intent) != OK);
}

int main(void) {
    testTactics();
    bool uno = false, wild = false, draft = false, draw = false;
    for (BotLevel level = EASY; level <= HARD; level++) {
        for (uint64_t seed = 0; seed < 100; seed++) {
            Game game;
            uint64_t randomState = seed;
            assert(gameInit(&game, (Mode)(seed % 4), PLAYERS, seed) == OK);
            int steps = 0;
            while (game.phase != FINISHED && steps++ < 2000) {
                Intent intent = choose(&game, level, &randomState);
                assert(gameApply(&game, game.turn, &intent, &events) == OK);
                uno |= intent.kind == SAY_UNO;
                draft |= intent.kind == DRAFT_PICK;
                wild |= intent.kind == PLAY && intent.color != NO_COLOR;
                draw |= intent.kind == DRAW;
            }
            assert(game.phase == FINISHED);
        }
    }
    assert(uno && wild && draft && draw);
    puts("Three bot levels completed 300 seeded matches");
}
