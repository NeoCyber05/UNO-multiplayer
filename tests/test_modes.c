#include "server/game.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static EventBatch events;
static const PlayerSpec players[4] = {{0}};

static void teams(Mode mode, const int expected[4]) {
    Game g;
    assert(game_init(&g,mode,players,0) == OK && g.mode == mode);
    assert(!memcmp(g.team,expected,sizeof g.team));
    for (Seat s=0; s<4; s++) {
        Game end = g;
        end.turn = s;
        end.hands[s].count = 1;
        Card c = end.hands[s].cards[0];
        end.currentColor = c.color;
        Intent a = {.kind=PLAY, .turnNo=end.turnNo, .color=c.color == WILD_COLOR ? RED : NO_COLOR};
        strcpy(a.cardId,c.id);
        assert(game_apply(&end,s,&a,&events) == OK && end.phase == FINISHED);
        assert(end.winnerSeat == s && end.winnerTeam == expected[s]);
    }
}

static void mark(Card c, bool seen[113]) {
    int id = 0;
    assert(sscanf(c.id,"c%d",&id) == 1 && id >= 1 && id <= 112 && !seen[id]);
    seen[id] = true;
}

static void inventory(const Game *g) {
    bool seen[113] = {false};
    const Pile *piles[] = {&g->deck,&g->discard,&g->hands[0],&g->hands[1],&g->hands[2],&g->hands[3]};
    for (unsigned i=0; i<6; i++)
        for (int k=0; k<piles[i]->count; k++) mark(piles[i]->cards[k],seen);
    for (int k=0; k<4; k++)
        if (g->draft.offers[k].occupied) mark(g->draft.offers[k].card,seen);
    for (int k=1; k<=112; k++) assert(seen[k]);
}

int main(void) {
    teams(TWO_V_TWO,(int[]){0,1,0,1});
    teams(SIDE,(int[]){0,0,1,1});
    Game g;
    assert(game_init(&g,DRAFT,players,42) == OK && g.phase == DRAFTING);
    for (Seat s=0; s<4; s++) assert(g.hands[s].count == 4);
    for (int t=0; t<16; t++) {
        Seat s = (Seat)((t / 4 + t % 4) % 4);
        assert(g.draft.turn == (uint32_t)t && g.turn == s);
        inventory(&g);
        Game before = g;
        Intent a = {.kind=PLAY,.turnNo=g.turnNo};
        assert(game_apply(&g,s,&a,&events) == INVALID_ACTION);
        assert(game_timeout(&g,g.turnNo,&events) == INVALID_ACTION);
        assert(game_draft_timeout(&g,t+1,&events) == STALE_TURN);
        a = (Intent){.kind=DRAFT_PICK,.draftTurn=t+1,.index=0};
        assert(game_apply(&g,s,&a,&events) == STALE_TURN);
        a.draftTurn = t;
        assert(game_apply(&g,(s+1)%4,&a,&events) == NOT_YOUR_TURN);
        a.index = -1;
        assert(game_apply(&g,s,&a,&events) == INVALID_ACTION);
        int low = 0, high = 3;
        while (!g.draft.offers[low].occupied) low++;
        while (!g.draft.offers[high].occupied) high--;
        for (int i=0; i<4; i++) {
            if (!g.draft.offers[i].occupied) {
                a.index = i;
                assert(game_apply(&g,s,&a,&events) == INVALID_ACTION);
            }
        }
        assert(!memcmp(&before,&g,sizeof g));
        a.index = high;
        Card picked = g.draft.offers[t%2 ? low : high].card;
        if (t%2) assert(game_draft_timeout(&g,t,&events) == OK);
        else assert(game_apply(&g,s,&a,&events) == OK);
        assert(g.hands[s].count == before.hands[s].count+1);
        assert(!strcmp(g.hands[s].cards[g.hands[s].count-1].id,picked.id));
    }
    inventory(&g);
    assert(g.phase == PLAYING && g.turn == 0 && g.turnNo == 1);
    for (Seat s=0; s<4; s++) assert(g.hands[s].count == 8);
    assert(game_draft_timeout(&g,15,&events) != OK);
    puts("Absolute teams and 16 draft picks passed");
}
