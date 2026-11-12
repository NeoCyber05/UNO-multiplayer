#include "server/view.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const PlayerSpec players[] = {
    {.userId=17,.name="An",.avatar="boy"}, {.userId=28,.name="Lan",.avatar="girl"},
    {.userId=39,.name="Minh",.avatar="nerd"}, {.isBot=true,.botLevel=HARD,.name="Bot",.avatar="bun"}
};

static void allowed_card(const Card *c, bool allowed[113]) {
    int n;
    assert(sscanf(c->id,"c%d",&n) == 1 && n >= 1 && n <= 112);
    allowed[n] = true;
}

static void check_tree(json_object *node, const bool allowed[113]) {
    if (json_object_is_type(node,json_type_object)) {
        json_object_object_foreach(node,key,value) {
            const char *private_keys[] = {"deck","drawPile","seed","rng","discard","events","watch"};
            for (size_t i=0; i<sizeof private_keys/sizeof *private_keys; i++)
                assert(strcmp(key,private_keys[i]));
            check_tree(value,allowed);
        }
    } else if (json_object_is_type(node,json_type_array)) {
        for (size_t i=0; i<json_object_array_length(node); i++) check_tree(json_object_array_get_idx(node,i),allowed);
    } else if (json_object_is_type(node,json_type_string)) {
        int n;
        char extra;
        if (sscanf(json_object_get_string(node),"c%d%c",&n,&extra) == 1)
            assert(n>=1 && n<=112 && allowed[n]);
    }
}

static json_object *snapshot(const Game *g, Seat viewer) {
    GameView v;
    assert(view_build(g,viewer,&v) == OK);
    strcpy(v.matchId,"123e4567-e89b-12d3-a456-426614174000");
    v.serverNowMs = 10000;
    v.deadline = 30000;
    v.durationMs = g->phase == PLAYING ? 5000 : 0;
    for (Seat s=0; s<4; s++) v.players[s].connected = !players[s].isBot;
    json_object *wire;
    assert(view_json(&v,&wire) == OK);
    json_object *parsed = json_tokener_parse(json_object_to_json_string(wire));
    json_object_put(wire);
    assert(parsed);
    assert(json_object_get_int(json_object_object_get(parsed,"viewerSeat")) == viewer);
    bool allowed[113] = {false};
    allowed_card(&g->discard.cards[g->discard.count-1],allowed);
    for (int i=0; i<4; i++)
        if (g->draft.offers[i].occupied) allowed_card(&g->draft.offers[i].card,allowed);
    json_object *seats = json_object_object_get(parsed,"players");
    assert(json_object_array_length(seats) == 4);
    for (Seat s=0; s<4; s++) {
        bool visible = s == viewer || (g->team[s] >= 0 && g->team[s] == g->team[viewer]);
        json_object *p = json_object_array_get_idx(seats,s), *hand;
        assert(json_object_get_int(json_object_object_get(p,"seat")) == s);
        assert(json_object_get_int(json_object_object_get(p,"cardCount")) == g->hands[s].count);
        assert(json_object_object_get_ex(p,"hand",&hand) == visible);
        if (visible) {
            assert((int)json_object_array_length(hand) == g->hands[s].count);
            for (int k=0; k<g->hands[s].count; k++) {
                const Card *c = &g->hands[s].cards[k];
                json_object *card = json_object_array_get_idx(hand,k);
                assert(!strcmp(json_object_get_string(json_object_object_get(card,"id")),c->id));
                allowed_card(c,allowed);
            }
        } else assert(v.players[s].hand.count == 0);
    }
    check_tree(parsed,allowed);
    return parsed;
}

int main(int argc, char **argv) {
    static EventBatch events;
    json_object *fixtures = json_object_new_array();
    for (Mode m=CLASSIC; m<=DRAFT; m++) {
        Game g;
        assert(game_init(&g,m,players,1234) == OK);
        for (int t=0; t<2; t++) {
            if (m == DRAFT) assert(game_draft_timeout(&g,g.draft.turn,&events) == OK);
            else assert(game_timeout(&g,g.turnNo,&events) == OK);
        }
        assert(g.turn == 2);
        for (Seat viewer=0; viewer<4; viewer++) {
            json_object *wire = snapshot(&g,viewer);
            if (viewer == 2) json_object_array_add(fixtures,wire);
            else json_object_put(wire);
        }
    }
    if (argc == 2 && !strcmp(argv[1],"--print-fixtures"))
        puts(json_object_to_json_string_ext(fixtures,JSON_C_TO_STRING_PRETTY));
    else {
        json_object *saved = json_object_from_file("tests/fixtures/views.json");
        assert(saved && json_object_equal(saved,fixtures));
        json_object_put(saved);
        puts("Seat privacy and wire fixtures passed in all four modes");
    }
    json_object_put(fixtures);
}
