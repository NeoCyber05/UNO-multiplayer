#include "server/game/view.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const PlayerSpec PLAYERS[] = {
    {.userId = 17, .name = "An", .avatar = "boy"},
    {.userId = 28, .name = "Lan", .avatar = "girl"},
    {.userId = 39, .name = "Minh", .avatar = "nerd"},
    {.isBot = true, .botLevel = HARD, .name = "Bot", .avatar = "bun"}};

static void allowCard(const Card *c, bool allowed[CARD_COUNT + 1]) {
    int n;
    assert(sscanf(c->id, "c%d", &n) == 1 && n >= 1 && n <= CARD_COUNT);
    allowed[n] = true;
}

static void checkTree(json_object *node, const bool allowed[CARD_COUNT + 1]) {
    if (json_object_is_type(node, json_type_object)) {
        json_object_object_foreach(node, key, value) {
            const char *PRIVATE_KEYS[] = {"deck",    "drawPile", "seed", "rng",
                                          "discard", "events",   "watch"};
            for (size_t i = 0; i < sizeof PRIVATE_KEYS / sizeof *PRIVATE_KEYS; i++)
                assert(strcmp(key, PRIVATE_KEYS[i]));
            checkTree(value, allowed);
        }
    } else if (json_object_is_type(node, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(node); i++)
            checkTree(json_object_array_get_idx(node, i), allowed);
    } else if (json_object_is_type(node, json_type_string)) {
        int n;
        char extra;
        if (sscanf(json_object_get_string(node), "c%d%c", &n, &extra) == 1)
            assert(n >= 1 && n <= CARD_COUNT && allowed[n]);
    }
}

static json_object *snapshot(const Game *game, Seat viewer) {
    GameView view;
    assert(viewBuild(game, viewer, &view) == OK);
    strcpy(view.matchId, "123e4567-e89b-12d3-a456-426614174000");
    view.serverNowMs = 10000;
    view.deadline = 30000;
    view.durationMs = game->phase == PLAYING ? 5000 : 0;
    for (Seat seat = 0; seat < 4; seat++)
        view.players[seat].connected = !PLAYERS[seat].isBot;
    json_object *wire;
    assert(viewJson(&view, &wire) == OK);
    json_object *parsed = json_tokener_parse(json_object_to_json_string(wire));
    json_object_put(wire);
    assert(parsed);
    assert(json_object_get_int(json_object_object_get(parsed, "viewerSeat")) == viewer);
    bool allowed[CARD_COUNT + 1] = {false};
    allowCard(&game->discard.cards[game->discard.count - 1], allowed);
    for (int i = 0; i < 4; i++)
        if (game->draft.offers[i].occupied) allowCard(&game->draft.offers[i].card, allowed);
    json_object *seats = json_object_object_get(parsed, "players");
    assert(json_object_array_length(seats) == 4);
    for (Seat seat = 0; seat < 4; seat++) {
        bool visible =
            seat == viewer || (game->team[seat] >= 0 && game->team[seat] == game->team[viewer]);
        json_object *player = json_object_array_get_idx(seats, seat), *hand;
        assert(json_object_get_int(json_object_object_get(player, "seat")) == seat);
        assert(json_object_get_int(json_object_object_get(player, "cardCount")) ==
               game->hands[seat].count);
        assert(json_object_object_get_ex(player, "hand", &hand) == visible);
        if (visible) {
            assert((int)json_object_array_length(hand) == game->hands[seat].count);
            for (int k = 0; k < game->hands[seat].count; k++) {
                const Card *c = &game->hands[seat].cards[k];
                json_object *card = json_object_array_get_idx(hand, k);
                assert(!strcmp(json_object_get_string(json_object_object_get(card, "id")), c->id));
                allowCard(c, allowed);
            }
        } else
            assert(view.players[seat].hand.count == 0);
    }
    checkTree(parsed, allowed);
    return parsed;
}

int main(int argc, char **argv) {
    static EventBatch events;
    json_object *fixtures = json_object_new_array();
    for (Mode mode = CLASSIC; mode <= DRAFT; mode++) {
        Game game;
        assert(gameInit(&game, mode, PLAYERS, 1234) == OK);
        for (int t = 0; t < 2; t++) {
            if (mode == DRAFT)
                assert(gameDraftTimeout(&game, game.draft.turn, &events) == OK);
            else
                assert(gameTimeout(&game, game.turnNo, &events) == OK);
        }
        assert(game.turn == 2);
        for (Seat viewer = 0; viewer < 4; viewer++) {
            json_object *wire = snapshot(&game, viewer);
            if (viewer == 2)
                json_object_array_add(fixtures, wire);
            else
                json_object_put(wire);
        }
    }
    if (argc == 2 && !strcmp(argv[1], "--print-fixtures"))
        puts(json_object_to_json_string_ext(fixtures, JSON_C_TO_STRING_PRETTY));
    else {
        json_object *saved = json_object_from_file("tests/fixtures/views.json");
        assert(saved && json_object_equal(saved, fixtures));
        json_object_put(saved);
        puts("Seat privacy and wire fixtures passed in all four modes");
    }
    json_object_put(fixtures);
}
