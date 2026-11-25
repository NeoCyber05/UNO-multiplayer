#include "match.h"

#include "bot.h"
#include "profile.h"
#include "rating.h"
#include "store.h"

#include <inttypes.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    WILD_CARD_POINTS = 50,
    ACTION_CARD_POINTS = 20,
    MAX_MATCHES = 16,
    TURN_MS = 20000,
    BOT_DELAY_MS = 700,
    STORAGE_RETRY_MS = 1000,
    QUICK_CHAT_COUNT = 8
};

typedef struct {
    Game game;
    MatchResult result;
    char roomId[sizeof(((RoomStart *)0)->roomId)];
    bool connected[SEAT_COUNT];
    bool active, played, finishing;
    uint64_t playStartedAt, deadline, botAt, botRng, retryAt, pausedAt, finishRevision;
} Match;

struct Matches {
    Store *store;
    MatchPush push;
    MatchEnded ended;
    void *context;
    Match slots[MAX_MATCHES];
};

Matches *matchesCreate(Store *store, MatchPush push, MatchEnded ended, void *context) {
    if (!store || !push || sodium_init() < 0) return NULL;
    Matches *matches = calloc(1, sizeof *matches);
    if (matches) {
        matches->store = store;
        matches->push = push;
        matches->ended = ended;
        matches->context = context;
    }
    return matches;
}

void matchesDestroy(Matches *matches) {
    free(matches);
}

static int seatOf(const Match *match, UserId userId) {
    if (userId) {
        for (int seat = 0; seat < SEAT_COUNT; seat++) {
            if (match->result.players[seat].userId == userId) return seat;
        }
    }
    return -1;
}

static Match *findByUser(Matches *matches, UserId userId) {
    for (int index = 0; index < MAX_MATCHES; index++) {
        Match *match = &matches->slots[index];
        if (match->active && seatOf(match, userId) >= 0) return match;
    }
    return NULL;
}

bool matchesContains(const Matches *matches, UserId userId) {
    if (!matches || !userId) return false;
    for (int index = 0; index < MAX_MATCHES; index++) {
        const Match *match = &matches->slots[index];
        if (match->active && seatOf(match, userId) >= 0) return true;
    }
    return false;
}

static uint64_t durationAt(const Match *match, uint64_t nowMs) {
    return match->played ? nowMs - match->playStartedAt : 0;
}

static void sendView(Matches *matches, const Match *match, Seat seat, uint64_t nowMs) {
    GameView view;
    json_object *payload = NULL;
    if (viewBuild(&match->game, seat, &view) != OK) return;
    memcpy(view.matchId, match->result.matchId, sizeof view.matchId);
    view.deadline = match->deadline;
    if (match->finishing)
        view.deadline = 0;
    else if (match->retryAt) {
        uint64_t remaining =
            match->deadline > match->pausedAt ? match->deadline - match->pausedAt : 0;
        view.deadline = nowMs + remaining;
    }
    view.serverNowMs = nowMs;
    view.durationMs = match->finishing ? match->result.durationMs : durationAt(match, nowMs);
    for (Seat index = 0; index < SEAT_COUNT; index++) {
        view.players[index].connected = match->connected[index];
    }
    if (viewJson(&view, &payload) != OK) return;
    json_object_object_add(payload, "paused", json_object_new_boolean(match->retryAt != 0));
    json_object_object_add(payload, "unoCalls",
                           json_object_new_uint64(match->game.stats[seat].unoCalls));
    matches->push(matches->context, match->result.players[seat].userId, "GAME_STATE", payload);
    json_object_put(payload);
}

static void broadcastView(Matches *matches, const Match *match, uint64_t nowMs) {
    for (Seat seat = 0; seat < SEAT_COUNT; seat++) {
        if (match->connected[seat]) sendView(matches, match, seat, nowMs);
    }
}

static void broadcast(Matches *matches, const Match *match, const char *type,
                      json_object *payload) {
    for (Seat seat = 0; seat < SEAT_COUNT; seat++) {
        if (match->connected[seat]) {
            matches->push(matches->context, match->result.players[seat].userId, type, payload);
        }
    }
}

static void scheduleTurn(Match *match, uint64_t nowMs) {
    match->deadline = nowMs + TURN_MS;
    match->botAt = nowMs + BOT_DELAY_MS;
    if (match->game.phase == PLAYING && !match->played) {
        match->played = true;
        match->playStartedAt = nowMs;
    }
}

static void generateId(MatchId *out) {
    unsigned char bytes[16];
    char hex[sizeof bytes * 2 + 1];
    randombytes_buf(bytes, sizeof bytes);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    sodium_bin2hex(hex, sizeof hex, bytes, sizeof bytes);
    snprintf(*out, sizeof *out, "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16,
             hex + 20);
}

Status matchesStart(Matches *matches, const RoomStart *room, uint64_t nowMs, MatchId *outMatchId) {
    if (!matches || !room || !outMatchId) return BAD_REQUEST;
    Match *slot = NULL;
    for (int index = 0; index < MAX_MATCHES; index++) {
        if (!matches->slots[index].active) {
            slot = &matches->slots[index];
            break;
        }
    }
    if (!slot) return SERVER_FULL;

    Match next = {.active = true};
    uint64_t seed;
    randombytes_buf(&seed, sizeof seed);
    randombytes_buf(&next.botRng, sizeof next.botRng);
    Status status = gameInit(&next.game, room->mode, room->players, seed);
    if (status != OK) return status;
    bool hasHuman = false;
    for (Seat seat = 0; seat < SEAT_COUNT; seat++) {
        UserId userId = room->players[seat].userId;
        if (room->players[seat].isBot) {
            // Ranked ELO is only meaningful between humans.
            if (room->ranked || userId || (unsigned)room->players[seat].botLevel > HARD) {
                return BAD_REQUEST;
            }
            continue;
        }
        if (!userId || seatOf(&next, userId) >= 0) return BAD_REQUEST;
        if (findByUser(matches, userId)) return BUSY;
        Account account;
        status = store_account_get_by_id(matches->store, userId, &account);
        if (status != OK) return status;
        next.result.players[seat].userId = userId;
        next.result.players[seat].eloBefore = account.elo;
        next.result.players[seat].eloAfter = account.elo;
        next.connected[seat] = true;
        hasHuman = true;
    }
    if (!hasHuman) return BAD_REQUEST;
    next.result.mode = room->mode;
    next.result.ranked = room->ranked;
    next.result.status = MATCH_ACTIVE;
    memcpy(next.roomId, room->roomId, sizeof next.roomId);
    generateId(&next.result.matchId);
    status = store_match_start(matches->store, (const MatchId *)&next.result.matchId, room->players,
                               room->mode, room->ranked);
    if (status != OK) return status;
    scheduleTurn(&next, nowMs);
    *slot = next;
    memcpy(*outMatchId, next.result.matchId, sizeof *outMatchId);
    broadcastView(matches, slot, nowMs);
    return OK;
}

static int handPoints(const Pile *hand) {
    int points = 0;
    for (int index = 0; index < hand->count; index++) {
        Value value = hand->cards[index].value;
        if (value <= NINE)
            points += value;
        else
            points += value >= WILD ? WILD_CARD_POINTS : ACTION_CARD_POINTS;
    }
    return points;
}

static void beginFinish(Match *match, MatchStatus status, uint64_t nowMs) {
    match->finishing = true;
    match->finishRevision = match->game.revision;
    match->result.status = status;
    match->result.durationMs = durationAt(match, nowMs);
    match->result.winnerSeat = match->game.winnerSeat;
    match->result.winnerTeam = match->game.winnerTeam;
    for (Seat seat = 0; seat < SEAT_COUNT; seat++) {
        PlayerResult *result = &match->result.players[seat];
        result->team = match->game.team[seat];
        result->points = handPoints(&match->game.hands[seat]);
        result->stats = match->game.stats[seat];
    }
    if (status == MATCH_FINALIZED) {
        // MATCH_LEAVE turns a human seat into a bot, but the leaver is still rated.
        bool isBot[SEAT_COUNT];
        for (Seat seat = 0; seat < SEAT_COUNT; seat++) {
            isBot[seat] = match->result.players[seat].userId == 0;
        }
        rating_apply(&match->result, isBot);
    }
}

static json_object *resultJson(const Match *match) {
    static const char *const MODE_NAMES[] = {"classic", "2v2", "side", "draft"};
    json_object *payload = json_object_new_object();
    json_object *players = json_object_new_array();
    json_object_object_add(payload, "matchId", json_object_new_string(match->result.matchId));
    json_object_object_add(payload, "mode", json_object_new_string(MODE_NAMES[match->game.mode]));
    json_object_object_add(payload, "ranked", json_object_new_boolean(match->result.ranked));
    json_object_object_add(payload, "winnerSeat", json_object_new_int(match->result.winnerSeat));
    json_object_object_add(payload, "winnerTeam", json_object_new_int(match->result.winnerTeam));
    json_object_object_add(payload, "durationMs", json_object_new_uint64(match->result.durationMs));
    json_object_object_add(payload, "players", players);
    for (Seat seat = 0; seat < SEAT_COUNT; seat++) {
        const PlayerResult *result = &match->result.players[seat];
        const PlayerSpec *player = &match->game.players[seat];
        json_object *row = json_object_new_object();
        json_object *stats = json_object_new_object();
        char userId[21];
        snprintf(userId, sizeof userId, "%" PRIu64, result->userId);
        json_object_object_add(row, "seat", json_object_new_int(seat));
        json_object_object_add(row, "userId", json_object_new_string(userId));
        json_object_object_add(row, "name", json_object_new_string(player->name));
        json_object_object_add(row, "avatar", json_object_new_string(player->avatar));
        json_object_object_add(row, "isBot", json_object_new_boolean(player->isBot));
        json_object_object_add(row, "team", json_object_new_int(result->team));
        json_object_object_add(row, "cardsLeft",
                               json_object_new_int(match->game.hands[seat].count));
        json_object_object_add(row, "points", json_object_new_int(result->points));
        json_object_object_add(row, "eloBefore", json_object_new_int(result->eloBefore));
        json_object_object_add(row, "eloAfter", json_object_new_int(result->eloAfter));
        json_object_object_add(row, "stats", stats);
        json_object_object_add(stats, "played", json_object_new_uint64(result->stats.played));
        json_object_object_add(stats, "attacks", json_object_new_uint64(result->stats.attacks));
        json_object_object_add(stats, "unoCalls", json_object_new_uint64(result->stats.unoCalls));
        json_object_array_add(players, row);
    }
    return payload;
}

static Status storeAbort(Store *store, const Match *match) {
    MatchEvent event = {.seq = match->game.eventSeq + 1,
                        .revision = match->finishRevision,
                        .actor = -1,
                        .type = "MATCH_ABORTED",
                        .payload = "{}"};
    return store_abort(store, (const MatchId *)&match->result.matchId, &event,
                       match->result.durationMs);
}

static Status pauseMatch(Matches *matches, Match *match, uint64_t nowMs) {
    if (!match->retryAt) {
        match->pausedAt = nowMs;
        match->retryAt = nowMs + STORAGE_RETRY_MS;
        match->game.revision++;
        if (!match->finishing) broadcastView(matches, match, nowMs);
    } else {
        match->retryAt = nowMs + STORAGE_RETRY_MS;
    }
    return STORAGE_ERROR;
}

static Status finishMatch(Matches *matches, Match *match, uint64_t nowMs) {
    bool aborted = match->result.status == MATCH_ABORTED;
    Status status;
    if (aborted) {
        status = storeAbort(matches->store, match);
    } else {
        // Single-threaded poll loop: stats read here cannot change before store_finalize commits.
        ProfileDelta deltas[PROFILE_SEAT_COUNT];
        status = profileCompute(matches->store, &match->result, deltas);
        if (status == OK) status = store_finalize(matches->store, &match->result, deltas);
    }
    if (status != OK) return pauseMatch(matches, match, nowMs);

    match->retryAt = 0;
    if (!aborted) {
        broadcastView(matches, match, nowMs);
        json_object *payload = resultJson(match);
        broadcast(matches, match, "MATCH_RESULT", payload);
        json_object_put(payload);
    }
    match->active = false;
    if (matches->ended) matches->ended(matches->context, match->roomId);
    return OK;
}

static void resumeClock(Match *match, uint64_t nowMs) {
    if (!match->retryAt) return;
    uint64_t pausedFor = nowMs - match->pausedAt;
    match->deadline += pausedFor;
    match->botAt += pausedFor;
    match->retryAt = 0;
}

static void retryPausedTurn(Matches *matches, Match *match, uint64_t nowMs) {
    sqlite3 *db = store_get_db(matches->store);
    // Probe for a writer without replaying a command the client saw fail.
    if (sqlite3_exec(db, "BEGIN IMMEDIATE; ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
        match->retryAt = nowMs + STORAGE_RETRY_MS;
        return;
    }
    resumeClock(match, nowMs);
    match->game.revision++;
    broadcastView(matches, match, nowMs);
}

static Status applyEvents(Matches *matches, Match *match, const Game *candidate,
                          const EventBatch *events, uint64_t nowMs) {
    if (store_events(matches->store, (const MatchId *)&match->result.matchId, events) != OK) {
        return pauseMatch(matches, match, nowMs);
    }
    bool turnChanged = candidate->turnNo != match->game.turnNo ||
                       candidate->phase != match->game.phase ||
                       candidate->draft.turn != match->game.draft.turn;
    resumeClock(match, nowMs);
    match->game = *candidate;
    if (candidate->phase == FINISHED) {
        beginFinish(match, MATCH_FINALIZED, nowMs);
        return finishMatch(matches, match, nowMs);
    }
    if (turnChanged) scheduleTurn(match, nowMs);
    broadcastView(matches, match, nowMs);
    return OK;
}

static bool hasConnectedHuman(const Match *match) {
    for (Seat seat = 0; seat < SEAT_COUNT; seat++) {
        if (match->connected[seat]) return true;
    }
    return false;
}

static void tickMatch(Matches *matches, Match *match, uint64_t nowMs) {
    if (nowMs < match->retryAt) return;
    if (match->finishing) {
        finishMatch(matches, match, nowMs);
        return;
    }
    Seat turn = match->game.turn;
    uint64_t effectiveNow = match->retryAt ? match->pausedAt : nowMs;
    bool expired = effectiveNow >= match->deadline;
    bool botReady = match->game.players[turn].isBot && effectiveNow >= match->botAt;
    if (!expired && !botReady) {
        if (match->retryAt) retryPausedTurn(matches, match, nowMs);
        return;
    }

    Game candidate = match->game;
    EventBatch events;
    Status status;
    uint64_t botRng = match->botRng;
    if (expired) {
        status = candidate.phase == DRAFTING
                     ? gameDraftTimeout(&candidate, candidate.draft.turn, &events)
                     : gameTimeout(&candidate, candidate.turnNo, &events);
    } else {
        GameView view;
        Intent intent;
        status = viewBuild(&candidate, turn, &view);
        if (status == OK)
            status = botChoose(&view, candidate.players[turn].botLevel, &botRng, &intent);
        if (status == OK) status = gameApply(&candidate, turn, &intent, &events);
    }
    if (status == OK && applyEvents(matches, match, &candidate, &events, nowMs) == OK) {
        match->botRng = botRng;
        match->botAt = nowMs + BOT_DELAY_MS;
    }
}

void matchesTick(Matches *matches, uint64_t nowMs) {
    if (!matches) return;
    for (int index = 0; index < MAX_MATCHES; index++) {
        Match *match = &matches->slots[index];
        if (match->active) tickMatch(matches, match, nowMs);
    }
}

static void updatePresence(Matches *matches, Match *match, Seat seat, bool connected,
                           uint64_t nowMs) {
    match->connected[seat] = connected;
    match->game.revision++;
    if (!match->finishing && !hasConnectedHuman(match)) {
        beginFinish(match, MATCH_ABORTED, nowMs);
        finishMatch(matches, match, nowMs);
    } else if (!match->finishing) {
        broadcastView(matches, match, nowMs);
    }
}

Status matchesConnected(Matches *matches, UserId userId, bool connected, uint64_t nowMs) {
    if (!matches) return BAD_REQUEST;
    Match *match = findByUser(matches, userId);
    if (!match) return NOT_FOUND;
    Seat seat = (Seat)seatOf(match, userId);
    if (match->result.players[seat].left) return FORBIDDEN;
    if (connected && !match->finishing) tickMatch(matches, match, nowMs);
    if (!match->active) return NOT_FOUND;
    if (connected == match->connected[seat]) {
        if (connected && !match->finishing) sendView(matches, match, seat, nowMs);
    } else {
        updatePresence(matches, match, seat, connected, nowMs);
    }
    return OK;
}

static bool readInt(json_object *payload, const char *key, int64_t min, int64_t max, int64_t *out) {
    json_object *field;
    if (!json_object_object_get_ex(payload, key, &field) ||
        !json_object_is_type(field, json_type_int))
        return false;
    *out = json_object_get_int64(field);
    return *out >= min && *out <= max;
}

static const char *readString(json_object *payload, const char *key, size_t maxLength) {
    json_object *field;
    if (!json_object_object_get_ex(payload, key, &field) ||
        !json_object_is_type(field, json_type_string))
        return NULL;
    const char *value = json_object_get_string(field);
    size_t length = (size_t)json_object_get_string_len(field);
    return length && length <= maxLength && strlen(value) == length ? value : NULL;
}

static bool readIntent(const Envelope *request, Intent *intent) {
    static const char *const INTENT_TYPES[] = {"PLAY",       "DRAW",        "PASS", "SAY_UNO",
                                               "DRAFT_PICK", "MATCH_LEAVE", "CHAT"};
    int kind;
    for (kind = PLAY; kind <= CHAT; kind++) {
        if (!strcmp(request->type, INTENT_TYPES[kind])) break;
    }
    if (kind > CHAT) return false;
    intent->kind = (IntentKind)kind;
    int64_t value;
    if (kind == DRAFT_PICK) {
        if (!readInt(request->payload, "draftTurn", 0, UINT32_MAX, &value)) return false;
        intent->draftTurn = (uint32_t)value;
        if (!readInt(request->payload, "index", 0, DRAFT_OFFER_COUNT - 1, &value)) return false;
        intent->index = (int)value;
    } else if (kind == CHAT) {
        if (!readInt(request->payload, "chatId", 0, QUICK_CHAT_COUNT - 1, &value)) return false;
        intent->chatId = (int)value;
    } else if (kind <= PASS) {
        if (!readInt(request->payload, "turnNo", 1, UINT32_MAX, &value)) return false;
        intent->turnNo = (uint32_t)value;
    }
    if (kind == PLAY) {
        const char *cardId = readString(request->payload, "cardId", sizeof intent->cardId - 1);
        if (!cardId) return false;
        strcpy(intent->cardId, cardId);
        if (json_object_object_get_ex(request->payload, "color", NULL)) {
            const char *color = readString(request->payload, "color", 6);
            if (!color) return false;
            static const char *const COLOR_NAMES[] = {"", "red", "yellow", "green", "blue"};
            for (Color index = RED; index <= BLUE; index++) {
                if (!strcmp(color, COLOR_NAMES[index])) intent->color = index;
            }
            if (intent->color == NO_COLOR) return false;
        }
    }
    return true;
}

static Status leaveMatch(Matches *matches, Match *match, Seat seat, uint64_t nowMs) {
    Game candidate = match->game;
    EventBatch events = {.count = 1};
    events.events[0] = (MatchEvent){.seq = ++candidate.eventSeq,
                                    .revision = ++candidate.revision,
                                    .actor = seat,
                                    .type = "MATCH_LEAVE",
                                    .payload = "{}"};
    if (store_events(matches->store, (const MatchId *)&match->result.matchId, &events) != OK) {
        return pauseMatch(matches, match, nowMs);
    }
    resumeClock(match, nowMs);
    candidate.players[seat].isBot = true;
    candidate.players[seat].botLevel = NORMAL;
    match->game = candidate;
    match->result.players[seat].left = true;
    if (seat == candidate.turn) match->botAt = nowMs + BOT_DELAY_MS;
    updatePresence(matches, match, seat, false, nowMs);
    return OK;
}

Status matchesHandle(Matches *matches, UserId userId, const Envelope *request, uint64_t nowMs) {
    if (!matches || !request || !request->type || !request->payload) return BAD_REQUEST;
    if (!userId) return UNAUTHENTICATED;
    Intent intent = {0};
    const char *matchId = readString(request->payload, "matchId", sizeof(MatchId) - 1);
    if (!matchId || json_object_object_get_ex(request->payload, "actor", NULL) ||
        json_object_object_get_ex(request->payload, "seat", NULL) || !readIntent(request, &intent))
        return BAD_REQUEST;

    Match *match = findByUser(matches, userId);
    if (!match || strcmp(matchId, match->result.matchId)) return NOT_FOUND;
    Seat seat = (Seat)seatOf(match, userId);
    if (!match->connected[seat]) return FORBIDDEN;
    tickMatch(matches, match, nowMs);
    if (!match->active || match->finishing) return INVALID_ACTION;
    if (nowMs < match->retryAt) return STORAGE_ERROR;
    if (intent.kind == LEAVE) return leaveMatch(matches, match, seat, nowMs);
    if (intent.kind == CHAT) {
        json_object *payload = json_object_new_object();
        json_object_object_add(payload, "matchId", json_object_new_string(matchId));
        json_object_object_add(payload, "seat", json_object_new_int(seat));
        json_object_object_add(payload, "chatId", json_object_new_int(intent.chatId));
        broadcast(matches, match, "GAME_CHAT", payload);
        json_object_put(payload);
        return OK;
    }
    Game candidate = match->game;
    EventBatch events;
    Status status = gameApply(&candidate, seat, &intent, &events);
    return status == OK ? applyEvents(matches, match, &candidate, &events, nowMs) : status;
}
