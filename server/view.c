#include "view.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

Status view_build(const Game *g, Seat viewer, GameView *v) {
    if (!g || !v || viewer >= 4) return BAD_REQUEST;
    *v = (GameView){
        .viewerSeat=viewer, .turn=g->turn, .mode=g->mode, .phase=g->phase,
        .direction=g->direction, .drawCount=g->deck.count, .winnerSeat=g->winnerSeat,
        .winnerTeam=g->winnerTeam, .turnNo=g->turnNo, .revision=g->revision,
        .currentColor=g->currentColor, .topCard=g->discard.cards[g->discard.count-1],
        .canSayUno=game_can_say_uno(g,viewer)
    };
    if (g->phase == DRAFTING) v->draft = g->draft;
    if (viewer == g->turn && g->phase == PLAYING) {
        v->canPass = g->drawnCardId[0] != 0;
        v->canDraw = !v->canPass;
        for (int k = 0; k < g->hands[viewer].count; k++)
            v->legal[k] = game_playable(g,viewer,&g->hands[viewer].cards[k]);
    }
    for (Seat s = 0; s < 4; s++) {
        PlayerView *p = &v->players[s];
        p->player = g->players[s];
        p->team = g->team[s];
        p->cardCount = g->hands[s].count;
        p->handVisible = s == viewer || (p->team >= 0 && p->team == g->team[viewer]);
        if (p->handVisible) {
            p->hand.count = p->cardCount;
            memcpy(p->hand.cards,g->hands[s].cards,(size_t)p->cardCount * sizeof(Card));
        }
    }
    return OK;
}

static const char *const colors[] = {"","red","yellow","green","blue","wild"};
static const char *const values[] = {"0","1","2","3","4","5","6","7","8","9",
    "skip","reverse","draw2","discard_all","wild","wild4"};
static const char *const modes[] = {"classic","2v2","side","draft"};
static const char *const phases[] = {"PLAYING","FINISHED","DRAFT"};
static const char *const levels[] = {"easy","normal","hard"};

/* Both helpers consume the child, including on allocation/insertion failure. */
static bool field(json_object *obj, const char *key, json_object *child) {
    if (obj && child && json_object_object_add(obj,key,child) == 0) return true;
    json_object_put(child);
    return false;
}

static bool append(json_object *array, json_object *child) {
    if (array && child && json_object_array_add(array,child) == 0) return true;
    json_object_put(child);
    return false;
}

static json_object *card_json(const Card *c) {
    json_object *obj = json_object_new_object();
    if (field(obj,"id",json_object_new_string(c->id)) &&
        field(obj,"color",json_object_new_string(colors[c->color])) &&
        field(obj,"value",json_object_new_string(values[c->value]))) return obj;
    json_object_put(obj);
    return NULL;
}

static json_object *player_json(const PlayerView *p, Seat seat) {
    json_object *obj = json_object_new_object();
    char id[21];
    snprintf(id,sizeof id,"%" PRIu64,p->player.userId);
    if (!field(obj,"seat",json_object_new_int(seat)) ||
        !field(obj,"userId",json_object_new_string(id)) ||
        !field(obj,"name",json_object_new_string(p->player.name)) ||
        !field(obj,"avatar",json_object_new_string(p->player.avatar)) ||
        !field(obj,"team",json_object_new_int(p->team)) ||
        !field(obj,"cardCount",json_object_new_int(p->cardCount)) ||
        !field(obj,"connected",json_object_new_boolean(p->connected)) ||
        !field(obj,"isBot",json_object_new_boolean(p->player.isBot))) goto fail;
    if (p->player.isBot) {
        if (!field(obj,"botLevel",json_object_new_string(levels[p->player.botLevel]))) goto fail;
    } else if (json_object_object_add(obj,"botLevel",NULL) < 0) goto fail;
    if (p->handVisible) {
        json_object *hand = json_object_new_array();
        if (!field(obj,"hand",hand)) goto fail;
        for (int i=0; i<p->hand.count; i++)
            if (!append(hand,card_json(&p->hand.cards[i]))) goto fail;
    }
    return obj;
fail:
    json_object_put(obj);
    return NULL;
}

Status view_json(const GameView *v, json_object **out) {
    if (!out) return BAD_REQUEST;
    *out = NULL;
    if (!v || v->viewerSeat >= 4) return BAD_REQUEST;
    json_object *root = json_object_new_object();
    if (!field(root,"matchId",json_object_new_string(v->matchId)) ||
        !field(root,"revision",json_object_new_uint64(v->revision)) ||
        !field(root,"viewerSeat",json_object_new_int(v->viewerSeat)) ||
        !field(root,"mode",json_object_new_string(modes[v->mode])) ||
        !field(root,"teams",json_object_new_boolean(v->mode == SIDE || v->mode == TWO_V_TWO)) ||
        !field(root,"turn",json_object_new_int(v->turn)) ||
        !field(root,"turnNo",json_object_new_uint64(v->turnNo)) ||
        !field(root,"direction",json_object_new_int(v->direction)) ||
        !field(root,"currentColor",json_object_new_string(colors[v->currentColor])) ||
        !field(root,"topCard",card_json(&v->topCard)) ||
        !field(root,"drawCount",json_object_new_int(v->drawCount)) ||
        !field(root,"phase",json_object_new_string(phases[v->phase])) ||
        !field(root,"deadline",json_object_new_uint64(v->deadline)) ||
        !field(root,"serverNowMs",json_object_new_uint64(v->serverNowMs)) ||
        !field(root,"durationMs",json_object_new_uint64(v->durationMs)) ||
        !field(root,"canDraw",json_object_new_boolean(v->canDraw)) ||
        !field(root,"canPass",json_object_new_boolean(v->canPass)) ||
        !field(root,"canSayUno",json_object_new_boolean(v->canSayUno))) goto fail;
    json_object *seats = json_object_new_array();
    if (!field(root,"players",seats)) goto fail;
    for (Seat s=0; s<4; s++)
        if (!append(seats,player_json(&v->players[s],s))) goto fail;
    json_object *ids = json_object_new_array();
    if (!field(root,"legalCardIds",ids)) goto fail;
    const Pile *hand = &v->players[v->viewerSeat].hand;
    for (int i=0; i<hand->count; i++)
        if (v->legal[i] && !append(ids,json_object_new_string(hand->cards[i].id))) goto fail;
    json_object *offers = json_object_new_array();
    if (!field(root,"offers",offers)) goto fail;
    if (v->phase == DRAFTING) {
        if (!field(root,"draftTurn",json_object_new_uint64(v->draft.turn))) goto fail;
        for (int i=0; i<4; i++) {
            if (v->draft.offers[i].occupied) {
                if (!append(offers,card_json(&v->draft.offers[i].card))) goto fail;
            } else if (json_object_array_add(offers,NULL) < 0) goto fail;
        }
    } else if (json_object_object_add(root,"draftTurn",NULL) < 0) goto fail;
    if (v->phase == FINISHED) {
        json_object *winner = json_object_new_object();
        if (!field(root,"winner",winner) ||
            !field(winner,"seat",json_object_new_int(v->winnerSeat)) ||
            !field(winner,"team",json_object_new_int(v->winnerTeam))) goto fail;
    } else if (json_object_object_add(root,"winner",NULL) < 0) goto fail;
    *out = root;
    return OK;
fail:
    json_object_put(root);
    return BUSY;
}
