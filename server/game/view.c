#include "view.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

Status viewBuild(const Game *game, Seat viewer, GameView *view) {
    if (!game || !view || viewer >= SEAT_COUNT) return BAD_REQUEST;
    *view = (GameView){.viewerSeat = viewer,
                       .turn = game->turn,
                       .mode = game->mode,
                       .phase = game->phase,
                       .direction = game->direction,
                       .drawCount = game->deck.count,
                       .winnerSeat = game->winnerSeat,
                       .winnerTeam = game->winnerTeam,
                       .turnNo = game->turnNo,
                       .revision = game->revision,
                       .currentColor = game->currentColor,
                       .topCard = game->discard.cards[game->discard.count - 1],
                       .canSayUno = gameCanSayUno(game, viewer)};
    if (game->phase == DRAFTING) view->draft = game->draft;
    if (viewer == game->turn && game->phase == PLAYING) {
        view->canPass = game->drawnCardId[0] != 0;
        view->canDraw = !view->canPass;
        for (int k = 0; k < game->hands[viewer].count; k++)
            view->legal[k] = gamePlayable(game, viewer, &game->hands[viewer].cards[k]);
    }
    for (Seat seat = 0; seat < SEAT_COUNT; seat++) {
        PlayerView *player = &view->players[seat];
        player->player = game->players[seat];
        player->team = game->team[seat];
        /* Người xem biết số lá của đối thủ để theo dõi ván, còn tay bài được giữ kín. */
        player->cardCount = game->hands[seat].count;
        player->handVisible =
            seat == viewer || (player->team >= 0 && player->team == game->team[viewer]);
        if (player->handVisible) {
            player->hand.count = player->cardCount;
            memcpy(player->hand.cards, game->hands[seat].cards,
                   (size_t)player->cardCount * sizeof(Card));
        }
    }
    return OK;
}

static const char *const COLOR_NAMES[] = {"", "red", "yellow", "green", "blue", "wild"};
static const char *const CARD_VALUE_NAMES[] = {
    "0", "1", "2",    "3",       "4",     "5",           "6",    "7",
    "8", "9", "skip", "reverse", "draw2", "discard_all", "wild", "wild4"};
static const char *const MODE_NAMES[] = {"classic", "2v2", "side", "draft"};
static const char *const PHASE_NAMES[] = {"PLAYING", "FINISHED", "DRAFT"};
static const char *const BOT_LEVEL_NAMES[] = {"easy", "normal", "hard"};

/* Hai hàm dưới nhận quyền sở hữu child và giải phóng nếu không thêm được vào JSON. */
static bool addField(json_object *obj, const char *key, json_object *child) {
    if (obj && child && json_object_object_add(obj, key, child) == 0) return true;
    json_object_put(child);
    return false;
}

static bool appendItem(json_object *array, json_object *child) {
    if (array && child && json_object_array_add(array, child) == 0) return true;
    json_object_put(child);
    return false;
}

static json_object *cardJson(const Card *card) {
    json_object *obj = json_object_new_object();
    if (addField(obj, "id", json_object_new_string(card->id)) &&
        addField(obj, "color", json_object_new_string(COLOR_NAMES[card->color])) &&
        addField(obj, "value", json_object_new_string(CARD_VALUE_NAMES[card->value])))
        return obj;
    json_object_put(obj);
    return NULL;
}

static json_object *playerJson(const PlayerView *player, Seat seat) {
    json_object *obj = json_object_new_object();
    char id[21];
    snprintf(id, sizeof id, "%" PRIu64, player->player.userId);
    if (!addField(obj, "seat", json_object_new_int(seat)) ||
        !addField(obj, "userId", json_object_new_string(id)) ||
        !addField(obj, "name", json_object_new_string(player->player.name)) ||
        !addField(obj, "avatar", json_object_new_string(player->player.avatar)) ||
        !addField(obj, "team", json_object_new_int(player->team)) ||
        !addField(obj, "cardCount", json_object_new_int(player->cardCount)) ||
        !addField(obj, "connected", json_object_new_boolean(player->connected)) ||
        !addField(obj, "isBot", json_object_new_boolean(player->player.isBot)))
        goto fail;
    if (player->player.isBot) {
        if (!addField(obj, "botLevel",
                      json_object_new_string(BOT_LEVEL_NAMES[player->player.botLevel])))
            goto fail;
    } else if (json_object_object_add(obj, "botLevel", NULL) < 0)
        goto fail;
    if (player->handVisible) {
        json_object *hand = json_object_new_array();
        if (!addField(obj, "hand", hand)) goto fail;
        for (int i = 0; i < player->hand.count; i++)
            if (!appendItem(hand, cardJson(&player->hand.cards[i]))) goto fail;
    }
    return obj;
fail:
    json_object_put(obj);
    return NULL;
}

Status viewJson(const GameView *view, json_object **out) {
    if (!out) return BAD_REQUEST;
    *out = NULL;
    if (!view || view->viewerSeat >= SEAT_COUNT) return BAD_REQUEST;
    json_object *root = json_object_new_object();
    if (!addField(root, "matchId", json_object_new_string(view->matchId)) ||
        !addField(root, "revision", json_object_new_uint64(view->revision)) ||
        !addField(root, "viewerSeat", json_object_new_int(view->viewerSeat)) ||
        !addField(root, "mode", json_object_new_string(MODE_NAMES[view->mode])) ||
        !addField(root, "teams",
                  json_object_new_boolean(view->mode == SIDE || view->mode == TWO_V_TWO)) ||
        !addField(root, "turn", json_object_new_int(view->turn)) ||
        !addField(root, "turnNo", json_object_new_uint64(view->turnNo)) ||
        !addField(root, "direction", json_object_new_int(view->direction)) ||
        !addField(root, "currentColor", json_object_new_string(COLOR_NAMES[view->currentColor])) ||
        !addField(root, "topCard", cardJson(&view->topCard)) ||
        !addField(root, "drawCount", json_object_new_int(view->drawCount)) ||
        !addField(root, "phase", json_object_new_string(PHASE_NAMES[view->phase])) ||
        !addField(root, "deadline", json_object_new_uint64(view->deadline)) ||
        !addField(root, "serverNowMs", json_object_new_uint64(view->serverNowMs)) ||
        !addField(root, "durationMs", json_object_new_uint64(view->durationMs)) ||
        !addField(root, "canDraw", json_object_new_boolean(view->canDraw)) ||
        !addField(root, "canPass", json_object_new_boolean(view->canPass)) ||
        !addField(root, "canSayUno", json_object_new_boolean(view->canSayUno)))
        goto fail;
    json_object *seats = json_object_new_array();
    if (!addField(root, "players", seats)) goto fail;
    for (Seat seat = 0; seat < SEAT_COUNT; seat++)
        if (!appendItem(seats, playerJson(&view->players[seat], seat))) goto fail;
    json_object *ids = json_object_new_array();
    if (!addField(root, "legalCardIds", ids)) goto fail;
    const Pile *hand = &view->players[view->viewerSeat].hand;
    for (int i = 0; i < hand->count; i++)
        if (view->legal[i] && !appendItem(ids, json_object_new_string(hand->cards[i].id)))
            goto fail;
    json_object *offers = json_object_new_array();
    if (!addField(root, "offers", offers)) goto fail;
    if (view->phase == DRAFTING) {
        if (!addField(root, "draftTurn", json_object_new_uint64(view->draft.turn))) goto fail;
        /* Giữ null ở ô đã lấy để chỉ số client gửi vẫn khớp với ô trên máy chủ. */
        for (int i = 0; i < DRAFT_OFFER_COUNT; i++) {
            if (view->draft.offers[i].occupied) {
                if (!appendItem(offers, cardJson(&view->draft.offers[i].card))) goto fail;
            } else if (json_object_array_add(offers, NULL) < 0)
                goto fail;
        }
    } else if (json_object_object_add(root, "draftTurn", NULL) < 0)
        goto fail;
    if (view->phase == FINISHED) {
        json_object *winner = json_object_new_object();
        if (!addField(root, "winner", winner) ||
            !addField(winner, "seat", json_object_new_int(view->winnerSeat)) ||
            !addField(winner, "team", json_object_new_int(view->winnerTeam)))
            goto fail;
    } else if (json_object_object_add(root, "winner", NULL) < 0)
        goto fail;
    *out = root;
    return OK;
fail:
    json_object_put(root);
    return BUSY;
}
