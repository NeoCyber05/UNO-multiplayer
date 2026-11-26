#include "server/game/match.h"
#include "server/game/game.h"
#include "server/game/profile.h"
#include "server/store.h"

#include <assert.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    json_object *views[5];
    int results, chats, finished;
} Capture;

static uint64_t randomState = 1;

static const char *randomName(void) {
    return "match-test";
}

static uint32_t randomWord(void) {
    return (uint32_t)gameRandom(&randomState);
}

static void randomBuffer(void *buffer, size_t size) {
    unsigned char *cursor = buffer;
    while (size) {
        uint64_t word = gameRandom(&randomState);
        size_t length = size < sizeof word ? size : sizeof word;
        memcpy(cursor, &word, length);
        cursor += length;
        size -= length;
    }
}

static randombytes_implementation testRandom = {
    .implementation_name = randomName, .random = randomWord, .buf = randomBuffer};

static void ended(void *context, const char *roomId) {
    Capture *messages = context;
    assert(!strcmp(roomId, "ABC123"));
    messages->finished++;
}

static void capture(void *context, UserId userId, const char *type, json_object *payload) {
    Capture *messages = context;
    if (!strcmp(type, "MATCH_RESULT")) {
        assert(json_object_array_length(json_object_object_get(payload, "players")) == 4);
        messages->results++;
    }
    if (!strcmp(type, "GAME_CHAT")) {
        assert(json_object_get_int(json_object_object_get(payload, "chatId")) == 3);
        assert(json_object_get_int(json_object_object_get(payload, "seat")) == 1);
        messages->chats++;
    }
    if (strcmp(type, "GAME_STATE")) return;
    assert(userId > 0 && userId < 5);
    json_object_put(messages->views[userId]);
    messages->views[userId] = json_object_get(payload);
}

static json_object *field(json_object *value, const char *key) {
    return json_object_object_get(value, key);
}

static int number(json_object *value, const char *key) {
    assert(field(value, key));
    return json_object_get_int(field(value, key));
}

static Status sendPayload(Matches *matches, UserId userId, const char *type, const MatchId *matchId,
                          json_object *payload, uint64_t nowMs) {
    json_object_object_add(payload, "matchId", json_object_new_string(*matchId));
    Envelope request = {1, 1, type, payload};
    Status status = matchesHandle(matches, userId, &request, nowMs);
    json_object_put(payload);
    return status;
}

static Status send(Matches *matches, UserId userId, const char *type, const MatchId *matchId,
                   int turnNo, uint64_t nowMs) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "turnNo", json_object_new_int(turnNo));
    return sendPayload(matches, userId, type, matchId, payload, nowMs);
}

static void checkDrawDeadline(Matches *matches, const RoomStart *room, Capture *messages) {
    // Find a repeatable seed where DRAW leaves a playable card in the same turn.
    bool found = false;
    for (uint64_t trial = 1; trial <= 100; trial++) {
        uint64_t trialState = trial;
        Game game;
        assert(gameInit(&game, CLASSIC, room->players, gameRandom(&trialState)) == OK);
        Card drawn = game.deck.cards[game.deck.count - 1];
        if (!gamePlayable(&game, 0, &drawn)) continue;
        randomState = trial;
        found = true;
        break;
    }
    assert(found);
    MatchId matchId;
    const uint64_t startedAt = 700000;
    const uint64_t deadline = startedAt + 20000;
    assert(matchesStart(matches, room, startedAt, &matchId) == OK);
    assert(send(matches, 1, "DRAW", (const MatchId *)&matchId, 1, startedAt + 500) == OK);
    assert(json_object_get_boolean(field(messages->views[1], "canPass")));
    assert(number(messages->views[1], "deadline") == (int)deadline);
    assert(matchesConnected(matches, 2, false, startedAt + 600) == OK);
    matchesTick(matches, deadline - 1);
    assert(number(messages->views[1], "turnNo") == 1);
    matchesTick(matches, deadline);
    assert(number(messages->views[1], "turnNo") == 2);
    assert(number(json_object_array_get_idx(field(messages->views[1], "players"), 0),
                  "cardCount") == 8);
    for (UserId userId = 1; userId <= 4; userId++) {
        if (userId != 2) assert(matchesConnected(matches, userId, false, deadline + 1) == OK);
    }
}

static void checkDraftAndResult(Matches *matches, RoomStart room, Capture *messages) {
    room.mode = DRAFT;
    for (Seat seat = 0; seat < 4; seat++) {
        if (seat == 1) continue;
        room.players[seat].userId = 0;
        room.players[seat].isBot = true;
        room.players[seat].botLevel = HARD;
    }
    randomState = 42;
    MatchId matchId;
    uint64_t nowMs = 800000;
    assert(matchesStart(matches, &room, nowMs, &matchId) == OK);
    matchesTick(matches, nowMs + 699);
    assert(number(messages->views[2], "draftTurn") == 0);
    nowMs += 700;
    matchesTick(matches, nowMs);
    assert(number(messages->views[2], "draftTurn") == 1);

    json_object *spoof = json_object_new_object();
    json_object_object_add(spoof, "actor", json_object_new_int(0));
    json_object_object_add(spoof, "chatId", json_object_new_int(3));
    assert(sendPayload(matches, 2, "CHAT", (const MatchId *)&matchId, spoof, nowMs) == BAD_REQUEST);
    json_object *chat = json_object_new_object();
    json_object_object_add(chat, "seat", json_object_new_int(0));
    json_object_object_add(chat, "chatId", json_object_new_int(3));
    assert(sendPayload(matches, 2, "CHAT", (const MatchId *)&matchId, chat, nowMs) == BAD_REQUEST);
    chat = json_object_new_object();
    json_object_object_add(chat, "chatId", json_object_new_int(3));
    assert(sendPayload(matches, 2, "CHAT", (const MatchId *)&matchId, chat, nowMs) == OK);
    assert(messages->chats == 1);

    int finishedBefore = messages->finished;
    for (int step = 0; step < 2000 && !messages->results; step++) {
        json_object *view = messages->views[2];
        if (number(view, "turn") == 1) {
            json_object *payload = json_object_new_object();
            const char *type = "DRAW";
            if (!strcmp(json_object_get_string(field(view, "phase")), "DRAFT")) {
                json_object *offers = field(view, "offers");
                int index = 0;
                while (index < 4 && !json_object_array_get_idx(offers, index))
                    index++;
                assert(index < 4);
                type = "DRAFT_PICK";
                json_object_object_add(payload, "index", json_object_new_int(index));
                json_object_object_add(payload, "draftTurn",
                                       json_object_new_int(number(view, "draftTurn")));
            } else {
                json_object_object_add(payload, "turnNo",
                                       json_object_new_int(number(view, "turnNo")));
                json_object *legal = field(view, "legalCardIds");
                if (json_object_get_boolean(field(view, "canSayUno")))
                    type = "SAY_UNO";
                else if (json_object_array_length(legal)) {
                    type = "PLAY";
                    json_object *cardId = json_object_array_get_idx(legal, 0);
                    json_object *players = field(view, "players");
                    json_object *hand = field(json_object_array_get_idx(players, 1), "hand");
                    json_object_object_add(payload, "cardId", json_object_get(cardId));
                    for (size_t index = 0; index < json_object_array_length(hand); index++) {
                        json_object *card = json_object_array_get_idx(hand, index);
                        if (json_object_equal(cardId, field(card, "id")) &&
                            !strcmp(json_object_get_string(field(card, "color")), "wild")) {
                            json_object_object_add(payload, "color",
                                                   json_object_get(field(view, "currentColor")));
                        }
                    }
                } else if (json_object_get_boolean(field(view, "canPass")))
                    type = "PASS";
            }
            assert(sendPayload(matches, 2, type, (const MatchId *)&matchId, payload, nowMs) == OK);
        }
        nowMs += 700;
        matchesTick(matches, nowMs);
    }
    assert(messages->results == 1 && messages->finished == finishedBefore + 1);
    assert(!matchesContains(matches, 2));
}

enum {
    RANKED_START_ELO = 1500,
    RANKED_WIN_ELO = 1516,
    RANKED_LOSS_ELO = 1484,
    RANKED_MAX_STEPS = 4000,
    LEAVER_SEAT = 3
};

static int queryInt(sqlite3 *db, const char *sql, int64_t key) {
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &query, NULL) == SQLITE_OK);
    if (sqlite3_bind_parameter_count(query)) {
        assert(sqlite3_bind_int64(query, 1, key) == SQLITE_OK);
    }
    assert(sqlite3_step(query) == SQLITE_ROW);
    int value = sqlite3_column_int(query, 0);
    sqlite3_finalize(query);
    return value;
}

static void playClassicTurn(Matches *matches, const Capture *messages, UserId userId,
                            const MatchId *matchId, uint64_t nowMs) {
    json_object *view = messages->views[userId];
    json_object *payload = json_object_new_object();
    const char *type = "DRAW";
    json_object_object_add(payload, "turnNo", json_object_new_int(number(view, "turnNo")));
    json_object *legal = field(view, "legalCardIds");
    if (json_object_get_boolean(field(view, "canSayUno")))
        type = "SAY_UNO";
    else if (json_object_array_length(legal)) {
        type = "PLAY";
        json_object *cardId = json_object_array_get_idx(legal, 0);
        json_object *players = field(view, "players");
        json_object *hand = field(json_object_array_get_idx(players, (int)userId - 1), "hand");
        json_object_object_add(payload, "cardId", json_object_get(cardId));
        for (size_t index = 0; index < json_object_array_length(hand); index++) {
            json_object *card = json_object_array_get_idx(hand, index);
            if (json_object_equal(cardId, field(card, "id")) &&
                !strcmp(json_object_get_string(field(card, "color")), "wild")) {
                json_object_object_add(payload, "color",
                                       json_object_get(field(view, "currentColor")));
            }
        }
    } else if (json_object_get_boolean(field(view, "canPass")))
        type = "PASS";
    assert(sendPayload(matches, userId, type, matchId, payload, nowMs) == OK);
}

static void testRankedRoomWithBotRejected(Matches *matches, sqlite3 *db, RoomStart room) {
    int matchesBefore = queryInt(db, "SELECT count(*) FROM matches", 0);
    room.ranked = true;
    room.players[LEAVER_SEAT].userId = 0;
    room.players[LEAVER_SEAT].isBot = true;
    room.players[LEAVER_SEAT].botLevel = EASY;
    MatchId matchId = {0};
    assert(matchesStart(matches, &room, 900000, &matchId) == BAD_REQUEST);
    assert(queryInt(db, "SELECT count(*) FROM matches", 0) == matchesBefore);
    assert(!matchesContains(matches, 1));
}

static void testRankedFinishAppliesElo(Matches *matches, Store *store, RoomStart room,
                                       Capture *messages) {
    sqlite3 *db = storeGetDb(store);
    ProfileStats before[PROFILE_SEAT_COUNT];
    for (Seat seat = 0; seat < PROFILE_SEAT_COUNT; seat++) {
        assert(storeProfileStats(store, room.players[seat].userId, &before[seat]) == OK);
    }
    room.mode = CLASSIC;
    room.ranked = true;
    randomState = 7;
    MatchId matchId;
    uint64_t nowMs = 1000000;
    assert(matchesStart(matches, &room, nowMs, &matchId) == OK);
    assert(queryInt(db, "SELECT ranked FROM matches ORDER BY rowid DESC LIMIT 1", 0) == 1);

    // The leaver's seat is played by a bot but must still be rated as a human.
    UserId leaver = room.players[LEAVER_SEAT].userId;
    int turnNo = number(messages->views[1], "turnNo");
    assert(send(matches, leaver, "MATCH_LEAVE", (const MatchId *)&matchId, turnNo, nowMs) == OK);

    int resultsBefore = messages->results;
    int finishedBefore = messages->finished;
    for (int step = 0; step < RANKED_MAX_STEPS && messages->results == resultsBefore; step++) {
        Seat turn = (Seat)number(messages->views[1], "turn");
        if (turn != LEAVER_SEAT) {
            playClassicTurn(matches, messages, room.players[turn].userId, (const MatchId *)&matchId,
                            nowMs);
        }
        nowMs += 700;
        matchesTick(matches, nowMs);
    }
    assert(messages->results > resultsBefore && messages->finished == finishedBefore + 1);

    int winnerSeat = queryInt(db, "SELECT winner_seat FROM matches WHERE ranked = 1", 0);
    assert(winnerSeat >= 0 && winnerSeat < 4);
    for (Seat seat = 0; seat < PROFILE_SEAT_COUNT; seat++) {
        UserId userId = room.players[seat].userId;
        int expected = seat == winnerSeat ? RANKED_WIN_ELO : RANKED_LOSS_ELO;
        assert(queryInt(db, "SELECT elo FROM accounts WHERE id = ?", (int64_t)userId) == expected);
        assert(queryInt(db,
                        "SELECT elo_before FROM match_players WHERE seat = ? AND "
                        "match_id = (SELECT match_id FROM matches WHERE ranked = 1)",
                        seat) == RANKED_START_ELO);
        ProfileStats after;
        assert(storeProfileStats(store, userId, &after) == OK);
        bool won = seat == winnerSeat && seat != LEAVER_SEAT;
        assert(after.matches == before[seat].matches + 1);
        assert(after.wins == before[seat].wins + won);
        int streak = before[seat].streak;
        assert(after.streak ==
               (won ? (streak < 0 ? 1 : streak + 1) : (streak > 0 ? -1 : streak - 1)));
        assert(after.peakElo ==
               (expected > before[seat].peakElo ? expected : before[seat].peakElo));
    }
}

int main(void) {
    assert(randombytes_set_implementation(&testRandom) == 0);
    Store *store = NULL;
    assert(storeOpen(&store, ":memory:") == OK);
    Capture messages = {0};
    Matches *matches = matchesCreate(store, capture, ended, &messages);
    assert(matches);

    RoomStart room = {.mode = CLASSIC, .roomId = "ABC123"};
    for (Seat seat = 0; seat < 4; seat++) {
        AccountCreate account = {.name = "Player", .avatar = "boy", .hash = "test"};
        snprintf(account.email, sizeof account.email, "player%u@example.com", (unsigned)seat);
        assert(storeAccountCreate(store, &account, &room.players[seat].userId) == OK);
        strcpy(room.players[seat].name, account.name);
    }

    MatchId matchId = {0};
    assert(matchesStart(matches, &room, 1000, &matchId) == OK);
    assert(matchesStart(matches, &room, 1000, &matchId) == BUSY);
    assert(number(messages.views[1], "deadline") == 21000);
    json_object *players = field(messages.views[1], "players");
    assert(field(json_object_array_get_idx(players, 0), "hand"));
    assert(!field(json_object_array_get_idx(players, 1), "hand"));
    sqlite3 *db = storeGetDb(store);
    assert(sqlite3_exec(db, "PRAGMA query_only = ON", NULL, NULL, NULL) == SQLITE_OK);
    assert(send(matches, 1, "DRAW", (const MatchId *)&matchId, 1, 1200) == STORAGE_ERROR);
    assert(matchesConnected(matches, 2, false, 1300) == OK);
    matchesTick(matches, 2200);
    assert(json_object_get_boolean(field(messages.views[1], "paused")));
    assert(sqlite3_exec(db, "PRAGMA query_only = OFF", NULL, NULL, NULL) == SQLITE_OK);
    matchesTick(matches, 3199);
    assert(json_object_get_boolean(field(messages.views[1], "paused")));
    matchesTick(matches, 3200);
    assert(!json_object_get_boolean(field(messages.views[1], "paused")));
    assert(number(messages.views[1], "deadline") == 23000);
    assert(number(messages.views[1], "turnNo") == 1);
    assert(matchesConnected(matches, 2, true, 3300) == OK);
    assert(number(messages.views[2], "deadline") == 23000);
    assert(matchesConnected(matches, 1, false, 4000) == OK);
    matchesTick(matches, 22999);
    assert(number(messages.views[2], "turnNo") == 1);
    assert(sqlite3_exec(db, "PRAGMA query_only = ON", NULL, NULL, NULL) == SQLITE_OK);
    assert(send(matches, 2, "DRAW", (const MatchId *)&matchId, 1, 23000) == STORAGE_ERROR);
    assert(number(messages.views[2], "turnNo") == 1);
    assert(json_object_get_boolean(field(messages.views[2], "paused")));
    assert(sqlite3_exec(db, "PRAGMA query_only = OFF", NULL, NULL, NULL) == SQLITE_OK);
    matchesTick(matches, 23999);
    assert(number(messages.views[2], "turnNo") == 1);
    assert(send(matches, 2, "DRAW", (const MatchId *)&matchId, 1, 24000) == STALE_TURN);
    assert(number(messages.views[2], "turn") == 1);
    assert(!json_object_get_boolean(field(messages.views[2], "paused")));
    assert(matchesConnected(matches, 1, true, 24100) == OK);
    assert(number(messages.views[1], "viewerSeat") == 0);
    assert(matchesConnected(matches, 1, false, 24101) == OK);
    assert(matchesConnected(matches, 1, true, 621101) == OK);
    assert(number(messages.views[1], "turnNo") > 2);
    int turnNo = number(messages.views[1], "turnNo");
    assert(send(matches, 1, "UNO_PENALTY", (const MatchId *)&matchId, turnNo, 621102) ==
           BAD_REQUEST);
    assert(send(matches, 1, "MATCH_LEAVE", (const MatchId *)&matchId, turnNo, 621102) == OK);
    assert(matchesContains(matches, 1));
    assert(matchesConnected(matches, 1, true, 621103) == FORBIDDEN);
    for (UserId userId = 2; userId <= 4; userId++) {
        assert(matchesConnected(matches, userId, false, 621104) == OK);
    }
    assert(!matchesContains(matches, 1));
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches", -1, &query, NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == MATCH_ABORTED);
    sqlite3_finalize(query);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM match_events WHERE type = 'MATCH_ABORTED'",
                              -1, &query, NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 1);
    sqlite3_finalize(query);
    assert(messages.finished == 1 && messages.results == 0);
    checkDrawDeadline(matches, &room, &messages);
    checkDraftAndResult(matches, room, &messages);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM matches WHERE status = ?", -1, &query,
                              NULL) == SQLITE_OK);
    assert(sqlite3_bind_int(query, 1, MATCH_FINALIZED) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 1);
    sqlite3_finalize(query);
    testRankedRoomWithBotRejected(matches, db, room);
    testRankedFinishAppliesElo(matches, store, room, &messages);

    for (UserId userId = 1; userId <= 4; userId++)
        json_object_put(messages.views[userId]);
    matchesDestroy(matches);
    storeClose(store);
}
