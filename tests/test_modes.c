#include "server/game.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static EventBatch events;
static const PlayerSpec PLAYERS[4] = {{0}};

static void testTeams(Mode mode, const int expected[4]) {
    Game game;
    assert(gameInit(&game, mode, PLAYERS, 0) == OK && game.mode == mode);
    assert(!memcmp(game.team, expected, sizeof game.team));
    for (Seat seat = 0; seat < 4; seat++) {
        Game end = game;
        end.turn = seat;
        end.hands[seat].count = 1;
        Card card = end.hands[seat].cards[0];
        end.currentColor = card.color;
        Intent intent = {
            .kind = PLAY, .turnNo = end.turnNo, .color = card.color == WILD_COLOR ? RED : NO_COLOR};
        strcpy(intent.cardId, card.id);
        assert(gameApply(&end, seat, &intent, &events) == OK && end.phase == FINISHED);
        assert(end.winnerSeat == seat && end.winnerTeam == expected[seat]);
    }
}

static void mark(Card card, bool seen[CARD_COUNT + 1]) {
    int id = 0;
    assert(sscanf(card.id, "c%d", &id) == 1 && id >= 1 && id <= CARD_COUNT && !seen[id]);
    seen[id] = true;
}

static void inventory(const Game *game) {
    bool seen[CARD_COUNT + 1] = {false};
    const Pile *piles[] = {&game->deck,     &game->discard,  &game->hands[0],
                           &game->hands[1], &game->hands[2], &game->hands[3]};
    for (unsigned i = 0; i < 6; i++)
        for (int k = 0; k < piles[i]->count; k++)
            mark(piles[i]->cards[k], seen);
    for (int k = 0; k < 4; k++)
        if (game->draft.offers[k].occupied) mark(game->draft.offers[k].card, seen);
    for (int k = 1; k <= CARD_COUNT; k++)
        assert(seen[k]);
}

int main(void) {
    testTeams(TWO_V_TWO, (int[]){0, 1, 0, 1});
    testTeams(SIDE, (int[]){0, 0, 1, 1});
    Game game;
    assert(gameInit(&game, DRAFT, PLAYERS, 42) == OK && game.phase == DRAFTING);
    for (Seat seat = 0; seat < 4; seat++)
        assert(game.hands[seat].count == 4);
    for (int draftTurn = 0; draftTurn < 16; draftTurn++) {
        Seat seat = (Seat)((draftTurn / 4 + draftTurn % 4) % 4);
        assert(game.draft.turn == (uint32_t)draftTurn && game.turn == seat);
        inventory(&game);
        Game before = game;
        Intent intent = {.kind = PLAY, .turnNo = game.turnNo};
        assert(gameApply(&game, seat, &intent, &events) == INVALID_ACTION);
        assert(gameTimeout(&game, game.turnNo, &events) == INVALID_ACTION);
        assert(gameDraftTimeout(&game, draftTurn + 1, &events) == STALE_TURN);
        intent = (Intent){.kind = DRAFT_PICK, .draftTurn = draftTurn + 1, .index = 0};
        assert(gameApply(&game, seat, &intent, &events) == STALE_TURN);
        intent.draftTurn = draftTurn;
        assert(gameApply(&game, (seat + 1) % 4, &intent, &events) == NOT_YOUR_TURN);
        intent.index = -1;
        assert(gameApply(&game, seat, &intent, &events) == INVALID_ACTION);
        int low = 0, high = 3;
        while (!game.draft.offers[low].occupied)
            low++;
        while (!game.draft.offers[high].occupied)
            high--;
        for (int i = 0; i < 4; i++) {
            if (!game.draft.offers[i].occupied) {
                intent.index = i;
                assert(gameApply(&game, seat, &intent, &events) == INVALID_ACTION);
            }
        }
        assert(!memcmp(&before, &game, sizeof game));
        intent.index = high;
        Card picked = game.draft.offers[draftTurn % 2 ? low : high].card;
        if (draftTurn % 2)
            assert(gameDraftTimeout(&game, draftTurn, &events) == OK);
        else
            assert(gameApply(&game, seat, &intent, &events) == OK);
        assert(game.hands[seat].count == before.hands[seat].count + 1);
        assert(!strcmp(game.hands[seat].cards[game.hands[seat].count - 1].id, picked.id));
    }
    inventory(&game);
    assert(game.phase == PLAYING && game.turn == 0 && game.turnNo == 1);
    for (Seat seat = 0; seat < 4; seat++)
        assert(game.hands[seat].count == 8);
    assert(gameDraftTimeout(&game, 15, &events) != OK);
    puts("Absolute teams and 16 draft picks passed");
}
