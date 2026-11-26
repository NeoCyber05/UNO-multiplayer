#define _POSIX_C_SOURCE 200809L
#include "rooms.h"
#include "accounts.h"
#include "app.h"
#include "server/game/match.h"
#include "queue.h"
#include "store.h"
#include <arpa/inet.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static uint64_t monotonicNowMs(void) {
    return accounts_get_now_ms();
}

static const char *const MODE_NAMES[] = {"classic", "2v2", "side", "draft"};

enum {
    ID_TEXT_SIZE = 32, /* fits the decimal form of any 64-bit id */
    QUEUE_ROOM_PREFIX = 'Q',
    ROOM_CODE_LENGTH = 6
};

/* "Q" + 5 digits is reserved for queue matches; a room with such a code would
 * be freed by rooms_match_ended when the queue match of the same id ends. */
static bool isQueueRoomId(const char *code) {
    if (code[0] != QUEUE_ROOM_PREFIX) return false;
    for (int i = 1; i < ROOM_CODE_LENGTH; i++) {
        if (code[i] < '0' || code[i] > '9') return false;
    }
    return true;
}

static void sendError(App *app, ConnId connId, int32_t requestId, const char *code,
                      const char *message) {
    if (connId.slot >= APP_MAX_CONNS) return;
    AppConnSlot *slot = &app->slots[connId.slot];
    if (!slot->active || slot->generation != connId.generation) return;

    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "code", json_object_new_string(code));
    json_object_object_add(payload, "message", json_object_new_string(message));

    Envelope response = {1, requestId, "ERROR", payload};
    conn_queue(&slot->conn, &response);
    json_object_put(payload);
    conn_flush(&slot->conn);
}

static void sendOk(App *app, ConnId connId, int32_t requestId, struct json_object *payload) {
    if (connId.slot >= APP_MAX_CONNS) return;
    AppConnSlot *slot = &app->slots[connId.slot];
    if (!slot->active || slot->generation != connId.generation) return;

    Envelope response = {1, requestId, "OK", payload ? payload : json_object_new_object()};
    conn_queue(&slot->conn, &response);
    if (!payload) json_object_put(response.payload);
    conn_flush(&slot->conn);
}

/* Random RFC 4122 version-4 UUID text for invite ids, so ids cannot be guessed. */
static void generateUuid(char outUuid[37]) {
    uint8_t randomBytes[16];
    randombytes_buf(randomBytes, sizeof(randomBytes));
    randomBytes[6] = (randomBytes[6] & 0x0f) | 0x40; /* version 4 */
    randomBytes[8] = (randomBytes[8] & 0x3f) | 0x80; /* variant 1 */
    snprintf(outUuid, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             randomBytes[0], randomBytes[1], randomBytes[2], randomBytes[3],
             randomBytes[4], randomBytes[5], randomBytes[6], randomBytes[7],
             randomBytes[8], randomBytes[9], randomBytes[10], randomBytes[11],
             randomBytes[12], randomBytes[13], randomBytes[14], randomBytes[15]);
}

void rooms_generate_code(Rooms *rooms, char outCode[7]) {
    /* 6 characters from 36 symbols: about 2.2 billion codes, short enough to
     * read out loud. Collisions with live rooms or queue ids are retried. */
    static const char codeAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char candidate[7];
    while (1) {
        uint8_t codeBytes[6];
        randombytes_buf(codeBytes, sizeof(codeBytes));
        for (int i = 0; i < 6; i++) {
            candidate[i] = codeAlphabet[codeBytes[i] % 36];
        }
        candidate[6] = '\0';

        bool collision = isQueueRoomId(candidate);
        if (rooms && !collision) {
            for (int i = 0; i < MAX_ROOMS; i++) {
                const Room *other = &rooms->rooms[i];
                if (other->active && other->code != outCode &&
                    strcmp(other->code, candidate) == 0) {
                    collision = true;
                    break;
                }
            }
        }
        if (!collision) {
            memcpy(outCode, candidate, 7);
            break;
        }
    }
}

static bool parseMode(struct json_object *valueJson, Mode *outMode) {
    if (!valueJson) {
        *outMode = CLASSIC;
        return true;
    }
    if (json_object_is_type(valueJson, json_type_int)) {
        int modeNumber = json_object_get_int(valueJson);
        if (modeNumber >= 0 && modeNumber <= 3) {
            *outMode = (Mode)modeNumber;
            return true;
        }
        return false;
    }
    if (json_object_is_type(valueJson, json_type_string)) {
        const char *text = json_object_get_string(valueJson);
        if (strcasecmp(text, "classic") == 0) { *outMode = CLASSIC; return true; }
        if (strcasecmp(text, "2v2") == 0 || strcasecmp(text, "two_v_two") == 0) {
            *outMode = TWO_V_TWO;
            return true;
        }
        if (strcasecmp(text, "side") == 0) { *outMode = SIDE; return true; }
        if (strcasecmp(text, "draft") == 0) { *outMode = DRAFT; return true; }
        return false;
    }
    return false;
}

/* Parties only queue for team modes, so INVITE_SEND must name one explicitly. */
static bool parsePartyMode(struct json_object *payload, Mode *outMode) {
    struct json_object *modeObj = NULL;
    if (!json_object_object_get_ex(payload, "mode", &modeObj) ||
        !json_object_is_type(modeObj, json_type_string)) {
        return false;
    }
    const char *modeName = json_object_get_string(modeObj);
    if (strcmp(modeName, MODE_NAMES[TWO_V_TWO]) == 0) { *outMode = TWO_V_TWO; return true; }
    if (strcmp(modeName, MODE_NAMES[SIDE]) == 0) { *outMode = SIDE; return true; }
    return false;
}

/* A seated player stays reserved until the match finalizes, even after ROOM_LEAVE. */
static bool rejectIfInMatch(App *app, ConnId connId, int32_t requestId, UserId userId) {
    if (!matchesContains(app->matches, userId)) return false;
    sendError(app, connId, requestId, "BUSY", "User is in a match");
    return true;
}

/* Spec: a user waiting in matchmaking cannot also sit in a room or party. */
static bool rejectIfQueued(App *app, ConnId connId, int32_t requestId, UserId userId) {
    if (!queue_contains(app->queue, userId)) return false;
    sendError(app, connId, requestId, "BUSY", "User is in matchmaking");
    return true;
}

static bool parseBotLevel(struct json_object *valueJson, BotLevel *outLevel) {
    if (!valueJson) return false;
    if (json_object_is_type(valueJson, json_type_int)) {
        int levelNumber = json_object_get_int(valueJson);
        if (levelNumber >= 0 && levelNumber <= 2) {
            *outLevel = (BotLevel)levelNumber;
            return true;
        }
        return false;
    }
    if (json_object_is_type(valueJson, json_type_string)) {
        const char *text = json_object_get_string(valueJson);
        if (strcasecmp(text, "easy") == 0) { *outLevel = EASY; return true; }
        if (strcasecmp(text, "normal") == 0 || strcasecmp(text, "medium") == 0) {
            *outLevel = NORMAL;
            return true;
        }
        if (strcasecmp(text, "hard") == 0) { *outLevel = HARD; return true; }
        return false;
    }
    return false;
}

Status rooms_init(Rooms **outRooms, App *app, OnMatchStart onStart) {
    if (!outRooms) return BAD_REQUEST;
    Rooms *rooms = calloc(1, sizeof(Rooms));
    if (!rooms) return STORAGE_ERROR;
    rooms->app = app;
    rooms->on_start = onStart;
    rooms->next_party_id = 1;
    rooms->next_invite_id = 1;
    *outRooms = rooms;
    return OK;
}

void rooms_destroy(Rooms *rooms) {
    if (!rooms) return;
    free(rooms);
}

Room *rooms_find_by_code(Rooms *rooms, const char *code) {
    if (!rooms || !code) return NULL;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms->rooms[i].active && strcasecmp(rooms->rooms[i].code, code) == 0) {
            return &rooms->rooms[i];
        }
    }
    return NULL;
}

Room *rooms_find_by_user(Rooms *rooms, UserId userId) {
    if (!rooms || userId == 0) return NULL;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms->rooms[i].active) {
            for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
                if (rooms->rooms[i].seats[seat].occupied && !rooms->rooms[i].seats[seat].is_bot &&
                    rooms->rooms[i].seats[seat].user_id == userId) {
                    return &rooms->rooms[i];
                }
            }
        }
    }
    return NULL;
}

Status rooms_create(Rooms *rooms, UserId hostId, const char *name, const char *avatar, Mode mode,
                    char outCode[7]) {
    if (!rooms || hostId == 0) return BAD_REQUEST;

    if (rooms_find_by_user(rooms, hostId) != NULL) return BUSY;

    /* Find empty room slot */
    int slot = -1;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (!rooms->rooms[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return SERVER_FULL;

    Room *room = &rooms->rooms[slot];
    memset(room, 0, sizeof(*room));
    rooms_generate_code(rooms, room->code);
    room->active = true;
    room->mode = mode;
    room->ranked = false;
    room->host_seat = 0;
    room->revision = 1;
    room->status = ROOM_STATUS_WAITING;

    room->seats[0].occupied = true;
    room->seats[0].is_bot = false;
    room->seats[0].user_id = hostId;
    strncpy(room->seats[0].name, name ? name : "Host", sizeof(room->seats[0].name) - 1);
    strncpy(room->seats[0].avatar, avatar ? avatar : "fox", sizeof(room->seats[0].avatar) - 1);

    if (outCode) {
        strcpy(outCode, room->code);
    }
    return OK;
}

Status rooms_join(Rooms *rooms, UserId userId, const char *name, const char *avatar,
                  const char *code, Seat *outSeat) {
    if (!rooms || userId == 0 || !code) return BAD_REQUEST;

    if (rooms_find_by_user(rooms, userId) != NULL) return BUSY;

    Room *room = rooms_find_by_code(rooms, code);
    if (!room) return NOT_FOUND;

    /* Find lowest-numbered empty seat */
    int emptySeat = -1;
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (!room->seats[i].occupied) {
            emptySeat = i;
            break;
        }
    }
    if (emptySeat < 0) return ROOM_FULL;

    room->seats[emptySeat].occupied = true;
    room->seats[emptySeat].is_bot = false;
    room->seats[emptySeat].user_id = userId;
    strncpy(room->seats[emptySeat].name, name ? name : "Player",
            sizeof(room->seats[emptySeat].name) - 1);
    strncpy(room->seats[emptySeat].avatar, avatar ? avatar : "fox",
            sizeof(room->seats[emptySeat].avatar) - 1);
    room->revision++;

    if (outSeat) *outSeat = (Seat)emptySeat;
    return OK;
}

Status rooms_leave(Rooms *rooms, UserId userId) {
    if (!rooms || userId == 0) return OK;

    Room *room = rooms_find_by_user(rooms, userId);
    if (!room) return OK;

    int seatIndex = -1;
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (room->seats[i].occupied && !room->seats[i].is_bot && room->seats[i].user_id == userId) {
            seatIndex = i;
            break;
        }
    }
    if (seatIndex < 0) return OK;

    memset(&room->seats[seatIndex], 0, sizeof(room->seats[seatIndex]));
    room->revision++;

    /* Check remaining humans */
    int humanCount = 0;
    int lowestHumanSeat = -1;
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (room->seats[i].occupied && !room->seats[i].is_bot) {
            humanCount++;
            if (lowestHumanSeat < 0) lowestHumanSeat = i;
        }
    }

    if (humanCount == 0) {
        /* Last human left: room is destroyed */
        memset(room, 0, sizeof(*room));
        return OK;
    }

    /* If the host left, migrate host to remaining human with lowest seat */
    if (seatIndex == room->host_seat) {
        room->host_seat = (Seat)lowestHumanSeat;
    }

    return OK;
}

Status rooms_set_bot(Rooms *rooms, UserId requesterId, int seat, BotLevel level) {
    if (!rooms || requesterId == 0) return BAD_REQUEST;
    if (seat < 0 || seat >= ROOM_MAX_SEATS) return BAD_REQUEST;

    Room *room = rooms_find_by_user(rooms, requesterId);
    if (!room) return NOT_FOUND;

    /* Must be host */
    if (room->seats[room->host_seat].user_id != requesterId) return FORBIDDEN;

    /* Host cannot turn themselves into a bot */
    if (seat == room->host_seat) return INVALID_ACTION;

    /* Cannot overwrite an existing human */
    if (room->seats[seat].occupied && !room->seats[seat].is_bot) return INVALID_ACTION;

    room->seats[seat].occupied = true;
    room->seats[seat].is_bot = true;
    room->seats[seat].user_id = 0;
    room->seats[seat].bot_level = level;
    snprintf(room->seats[seat].name, sizeof(room->seats[seat].name), "Bot %u",
             (unsigned)(seat + 1));
    strcpy(room->seats[seat].avatar, "robot");
    room->revision++;

    return OK;
}

Status rooms_kick(Rooms *rooms, UserId requesterId, int seat) {
    if (!rooms || requesterId == 0) return BAD_REQUEST;
    if (seat < 0 || seat >= ROOM_MAX_SEATS) return BAD_REQUEST;

    Room *room = rooms_find_by_user(rooms, requesterId);
    if (!room) return NOT_FOUND;

    /* Must be host */
    if (room->seats[room->host_seat].user_id != requesterId) return FORBIDDEN;

    /* Cannot kick self */
    if (seat == room->host_seat) return INVALID_ACTION;

    /* Must be occupied */
    if (!room->seats[seat].occupied) return INVALID_ACTION;

    memset(&room->seats[seat], 0, sizeof(room->seats[seat]));
    room->revision++;

    return OK;
}

Status rooms_set_mode(Rooms *rooms, UserId requesterId, Mode mode) {
    if (!rooms || requesterId == 0) return BAD_REQUEST;

    Room *room = rooms_find_by_user(rooms, requesterId);
    if (!room) return NOT_FOUND;

    /* Must be host */
    if (room->seats[room->host_seat].user_id != requesterId) return FORBIDDEN;

    room->mode = mode;
    room->revision++;
    return OK;
}

Status rooms_start(App *app, const char *roomId) {
    if (!app || !app->rooms || !roomId) return BAD_REQUEST;
    Rooms *rooms = app->rooms;

    Room *room = rooms_find_by_code(rooms, roomId);
    if (!room) return NOT_FOUND;

    if (room->status != ROOM_STATUS_WAITING) return INVALID_ACTION;

    /* Check all 4 seats occupied */
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (!room->seats[i].occupied) return INVALID_ACTION;
    }

    if (!rooms->on_start) return BUSY;

    RoomStart roomStart;
    memset(&roomStart, 0, sizeof(roomStart));
    strncpy(roomStart.roomId, room->code, sizeof(roomStart.roomId) - 1);
    roomStart.mode = room->mode;
    roomStart.ranked = room->ranked;
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        roomStart.players[i].userId = room->seats[i].user_id;
        strncpy(roomStart.players[i].name, room->seats[i].name,
                sizeof(roomStart.players[i].name) - 1);
        strncpy(roomStart.players[i].avatar, room->seats[i].avatar,
                sizeof(roomStart.players[i].avatar) - 1);
        roomStart.players[i].isBot = room->seats[i].is_bot;
        roomStart.players[i].botLevel = room->seats[i].bot_level;
    }

    MatchId outMatchId = {0};
    Status status = rooms->on_start(app, &roomStart, &outMatchId);
    if (status != OK) {
        /* Callback failed: room remains intact without mutating to in-match */
        return status;
    }

    room->status = ROOM_STATUS_IN_MATCH;
    return OK;
}

void rooms_disconnect(App *app, UserId userId, uint64_t nowMs) {
    (void)nowMs;
    if (!app || !app->rooms || userId == 0) return;
    Rooms *rooms = app->rooms;

    party_leave(rooms, userId);

    /* Cancel any pending invites involving this user */
    for (int i = 0; i < MAX_INVITES; i++) {
        if (rooms->invites[i].active &&
            (rooms->invites[i].sender_id == userId || rooms->invites[i].recipient_id == userId)) {
            rooms->invites[i].active = false;
        }
    }

    Room *room = rooms_find_by_user(rooms, userId);
    if (room && room->status == ROOM_STATUS_WAITING) {
        char code[7];
        strcpy(code, room->code);
        rooms_leave(rooms, userId);
        Room *after = rooms_find_by_code(rooms, code);
        if (after) {
            rooms_broadcast_state(app, after);
        }
    }
}

Status rooms_make_match(App *app, Mode mode, const UserId users[4], MatchId *outMatchId) {
    if (!app || !app->rooms || !users || !outMatchId) return BAD_REQUEST;
    Rooms *rooms = app->rooms;
    if (!rooms->on_start) return BUSY;
    if (!app->store) return STORAGE_ERROR;

    RoomStart roomStart;
    memset(&roomStart, 0, sizeof(roomStart));
    unsigned serial = (unsigned)(rooms->next_queue_room % QUEUE_ROOM_SERIAL_LIMIT);
    snprintf(roomStart.roomId, sizeof(roomStart.roomId), "%c%05u", QUEUE_ROOM_PREFIX, serial);
    roomStart.mode = mode;
    /* Spec: only matchmaking with four humans is ranked; no bot ever fills a seat. */
    roomStart.ranked = true;
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (users[seat] == 0) return BAD_REQUEST;
        Account account;
        Status lookup = store_account_get_by_id(app->store, users[seat], &account);
        if (lookup != OK) return lookup;
        PlayerSpec *player = &roomStart.players[seat];
        player->userId = users[seat];
        strncpy(player->name, account.name, sizeof(player->name) - 1);
        strncpy(player->avatar, account.avatar, sizeof(player->avatar) - 1);
    }

    /* The coordinator persists the match and pushes GAME_STATE itself. */
    Status started = rooms->on_start(app, &roomStart, outMatchId);
    if (started == OK) rooms->next_queue_room++;
    return started;
}

/* Wire ids are decimal strings so 64-bit ids survive JSON number precision. */
static json_object *userIdJson(UserId userId) {
    char idText[ID_TEXT_SIZE];
    snprintf(idText, sizeof(idText), "%llu", (unsigned long long)userId);
    return json_object_new_string(idText);
}

/* One builder for every ROOM_STATE so broadcast, kick and re-login agree. */
static json_object *roomStateJson(const Room *room) {
    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "revision", json_object_new_int64(room->revision));
    json_object_object_add(payload, "roomId", json_object_new_string(room->code));
    json_object_object_add(payload, "code", json_object_new_string(room->code));
    json_object_object_add(payload, "mode", json_object_new_string(MODE_NAMES[room->mode]));
    json_object_object_add(payload, "ranked", json_object_new_boolean(room->ranked));
    json_object_object_add(payload, "hostSeat", json_object_new_int(room->host_seat));

    struct json_object *seatsJson = json_object_new_array();
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (!room->seats[i].occupied) {
            json_object_array_add(seatsJson, NULL);
        } else {
            struct json_object *seatStateJson = json_object_new_object();
            json_object_object_add(seatStateJson, "seat", json_object_new_int(i));
            json_object_object_add(seatStateJson, "isBot",
                                   json_object_new_boolean(room->seats[i].is_bot));
            if (room->seats[i].is_bot) {
                json_object_object_add(seatStateJson, "name",
                                       json_object_new_string(room->seats[i].name));
                json_object_object_add(seatStateJson, "avatar",
                                       json_object_new_string(room->seats[i].avatar));
                const char *levelNames[] = {"easy", "normal", "hard"};
                const char *levelName = levelNames[room->seats[i].bot_level];
                json_object_object_add(seatStateJson, "botLevel",
                                       json_object_new_string(levelName));
            } else {
                json_object_object_add(seatStateJson, "userId", userIdJson(room->seats[i].user_id));
                json_object_object_add(seatStateJson, "name",
                                       json_object_new_string(room->seats[i].name));
                json_object_object_add(seatStateJson, "avatar",
                                       json_object_new_string(room->seats[i].avatar));
            }
            json_object_array_add(seatsJson, seatStateJson);
        }
    }
    json_object_object_add(payload, "seats", seatsJson);
    return payload;
}

static void pushToSeatedHumans(App *app, const Room *room, json_object *payload) {
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (room->seats[i].occupied && !room->seats[i].is_bot) {
            /* Offline members miss the push; LOGIN resyncs them via rooms_sync_user. */
            (void)app_push(app, room->seats[i].user_id, "ROOM_STATE", payload);
        }
    }
}

Status rooms_broadcast_state(App *app, const Room *room) {
    if (!app || !room) return BAD_REQUEST;
    json_object *payload = roomStateJson(room);
    pushToSeatedHumans(app, room, payload);
    json_object_put(payload);
    return OK;
}

void rooms_update_identity(App *app, UserId userId, const char *name, const char *avatar) {
    if (!app || !app->rooms || userId == 0 || !name || !avatar) return;
    Room *room = rooms_find_by_user(app->rooms, userId);
    /* matchesStart already copied the seats; a live match keeps its own copy. */
    if (!room || room->status != ROOM_STATUS_WAITING) return;
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        RoomSeat *seat = &room->seats[i];
        if (!seat->occupied || seat->is_bot || seat->user_id != userId) continue;
        snprintf(seat->name, sizeof(seat->name), "%s", name);
        snprintf(seat->avatar, sizeof(seat->avatar), "%s", avatar);
        room->revision++;
        rooms_broadcast_state(app, room);
        return;
    }
}

void rooms_match_ended(App *app, const char *roomId) {
    if (!app || !app->rooms || !roomId || roomId[0] == '\0') return;
    Room *room = rooms_find_by_code(app->rooms, roomId);
    /* If every human left mid-match the code may since name a new waiting room. */
    if (!room || room->status != ROOM_STATUS_IN_MATCH) return;
    /* Clearing the slot frees every seat at once; members already got MATCH_RESULT. */
    memset(room, 0, sizeof(*room));
}

Party *party_find_by_user(Rooms *rooms, UserId userId) {
    if (!rooms || userId == 0) return NULL;
    for (int i = 0; i < MAX_PARTIES; i++) {
        if (rooms->parties[i].active) {
            for (size_t modeNumber = 0; modeNumber < rooms->parties[i].member_count; modeNumber++) {
                if (rooms->parties[i].members[modeNumber] == userId) {
                    return &rooms->parties[i];
                }
            }
        }
    }
    return NULL;
}

static json_object *partyStateJson(const Party *party) {
    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "partyId", json_object_new_int((int)party->party_id));
    json_object_object_add(payload, "leaderId", userIdJson(party->leader_id));
    json_object_object_add(payload, "revision", json_object_new_int64(party->revision));
    json_object_object_add(payload, "mode", json_object_new_string(MODE_NAMES[party->mode]));

    struct json_object *membersJson = json_object_new_array();
    for (size_t i = 0; i < party->member_count; i++) {
        json_object_array_add(membersJson, userIdJson(party->members[i]));
    }
    json_object_object_add(payload, "members", membersJson);
    return payload;
}

static void broadcastPartyState(App *app, const Party *party) {
    if (!app || !party) return;

    json_object *payload = partyStateJson(party);
    for (size_t i = 0; i < party->member_count; i++) {
        (void)app_push(app, party->members[i], "PARTY_STATE", payload);
    }
    json_object_put(payload);
}

void rooms_sync_user(App *app, UserId userId) {
    if (!app || !app->rooms || userId == 0) return;
    Room *room = rooms_find_by_user(app->rooms, userId);
    if (room) {
        json_object *roomState = roomStateJson(room);
        (void)app_push(app, userId, "ROOM_STATE", roomState);
        json_object_put(roomState);
    }
    Party *party = party_find_by_user(app->rooms, userId);
    if (party) {
        json_object *partyState = partyStateJson(party);
        (void)app_push(app, userId, "PARTY_STATE", partyState);
        json_object_put(partyState);
    }
}

Status party_leave(Rooms *rooms, UserId userId) {
    if (!rooms || userId == 0) return OK;
    Party *party = party_find_by_user(rooms, userId);
    if (!party) return OK;

    if (rooms->app) {
        UserId oldMembers[PARTY_MAX_MEMBERS];
        size_t oldCount = party->member_count;
        for (size_t i = 0; i < oldCount; i++) {
            oldMembers[i] = party->members[i];
        }
        uint32_t partyId = party->party_id;
        uint64_t revision = party->revision + 1;

        struct json_object *payload = json_object_new_object();
        json_object_object_add(payload, "partyId", json_object_new_int((int)partyId));
        json_object_object_add(payload, "leaderId", json_object_new_string("0"));
        json_object_object_add(payload, "revision", json_object_new_int64(revision));
        json_object_object_add(payload, "mode", json_object_new_string(MODE_NAMES[party->mode]));
        json_object_object_add(payload, "members", json_object_new_array());

        Envelope pushEnvelope = {1, 0, "PARTY_STATE", payload};
        for (size_t i = 0; i < oldCount; i++) {
            ConnId connId;
            if (sessions_find_conn(rooms->app, oldMembers[i], &connId) == OK) {
                if (connId.slot < APP_MAX_CONNS) {
                    AppConnSlot *recipientSlot = &rooms->app->slots[connId.slot];
                    if (recipientSlot->active && recipientSlot->generation == connId.generation) {
                        conn_queue(&recipientSlot->conn, &pushEnvelope);
                        conn_flush(&recipientSlot->conn);
                    }
                }
            }
        }
        json_object_put(payload);
    }

    /* Party max 2 members. If someone leaves, dissolve party */
    memset(party, 0, sizeof(*party));
    return OK;
}

Status rooms_invite_create(Rooms *rooms, UserId senderId, UserId recipientId, const char *kind,
                           const char *roomCode, char outInviteId[37]) {
    if (!rooms || senderId == 0 || recipientId == 0) return BAD_REQUEST;

    uint64_t nowMs = monotonicNowMs();
    int inviteSlot = -1;
    for (int i = 0; i < MAX_INVITES; i++) {
        const Invite *slot = &rooms->invites[i];
        bool expired = nowMs > slot->created_at_ms &&
                       nowMs - slot->created_at_ms > INVITE_TIMEOUT_MS;
        if (!slot->active || expired) {
            inviteSlot = i;
            break;
        }
    }
    if (inviteSlot < 0) {
        return SERVER_FULL;
    }

    Invite *invite = &rooms->invites[inviteSlot];
    memset(invite, 0, sizeof(*invite));
    invite->active = true;
    generateUuid(invite->invite_id);
    invite->sender_id = senderId;
    invite->recipient_id = recipientId;
    if (kind) strncpy(invite->kind, kind, sizeof(invite->kind) - 1);
    if (roomCode) strncpy(invite->room_code, roomCode, sizeof(invite->room_code) - 1);
    invite->created_at_ms = nowMs;

    if (outInviteId) {
        strcpy(outInviteId, invite->invite_id);
    }
    return OK;
}

Status rooms_handle(App *app, ConnId connId, const Envelope *envelope) {
    if (!app || !app->rooms || !envelope) return BAD_REQUEST;
    Rooms *rooms = app->rooms;

    UserId userId = 0;
    if (sessions_user(app, connId, &userId) != OK) {
        sendError(app, connId, envelope->id, "UNAUTHENTICATED", "Authentication required");
        return OK;
    }

    Account account;
    memset(&account, 0, sizeof(account));
    if (app->store) {
        store_account_get_by_id(app->store, userId, &account);
    }
    if (account.name[0] == '\0') {
        snprintf(account.name, sizeof(account.name), "Player%llu", (unsigned long long)userId);
        strcpy(account.avatar, "fox");
    }

    if (strcmp(envelope->type, "ROOM_CREATE") == 0) {
        if (rejectIfInMatch(app, connId, envelope->id, userId)) return OK;
        if (rejectIfQueued(app, connId, envelope->id, userId)) return OK;
        Mode mode = CLASSIC;
        struct json_object *modeJson = NULL;
        if (envelope->payload && json_object_object_get_ex(envelope->payload, "mode", &modeJson)) {
            parseMode(modeJson, &mode);
        }

        char code[7] = {0};
        Status status = rooms_create(rooms, userId, account.name, account.avatar, mode, code);
        if (status == BUSY) {
            sendError(app, connId, envelope->id, "BUSY", "User already in a room");
            return OK;
        }
        if (status == SERVER_FULL) {
            sendError(app, connId, envelope->id, "SERVER_FULL", "Server room capacity reached");
            return OK;
        }
        if (status != OK) {
            sendError(app, connId, envelope->id, "INVALID_ACTION", "Could not create room");
            return OK;
        }

        struct json_object *responsePayload = json_object_new_object();
        json_object_object_add(responsePayload, "roomId", json_object_new_string(code));
        json_object_object_add(responsePayload, "code", json_object_new_string(code));
        json_object_object_add(responsePayload, "mode", json_object_new_string(MODE_NAMES[mode]));
        json_object_object_add(responsePayload, "seat", json_object_new_int(0));
        sendOk(app, connId, envelope->id, responsePayload);

        Room *room = rooms_find_by_code(rooms, code);
        if (room) rooms_broadcast_state(app, room);
        return OK;
    }

    if (strcmp(envelope->type, "ROOM_JOIN") == 0) {
        struct json_object *codeJson = NULL;
        if (!envelope->payload || !json_object_object_get_ex(envelope->payload, "code",
                                                             &codeJson)) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Room code is required");
            return OK;
        }
        if (rejectIfInMatch(app, connId, envelope->id, userId)) return OK;
        if (rejectIfQueued(app, connId, envelope->id, userId)) return OK;
        const char *code = json_object_get_string(codeJson);
        Seat seat = 0;
        Status status = rooms_join(rooms, userId, account.name, account.avatar, code, &seat);
        if (status == BUSY) {
            sendError(app, connId, envelope->id, "BUSY", "User already in a room");
            return OK;
        }
        if (status == NOT_FOUND) {
            sendError(app, connId, envelope->id, "NOT_FOUND", "Room not found");
            return OK;
        }
        if (status == ROOM_FULL) {
            sendError(app, connId, envelope->id, "ROOM_FULL", "Room is full");
            return OK;
        }
        if (status != OK) {
            sendError(app, connId, envelope->id, "INVALID_ACTION", "Could not join room");
            return OK;
        }

        struct json_object *responsePayload = json_object_new_object();
        json_object_object_add(responsePayload, "roomId", json_object_new_string(code));
        json_object_object_add(responsePayload, "code", json_object_new_string(code));
        json_object_object_add(responsePayload, "seat", json_object_new_int(seat));
        sendOk(app, connId, envelope->id, responsePayload);

        Room *room = rooms_find_by_code(rooms, code);
        if (room) rooms_broadcast_state(app, room);
        return OK;
    }

    if (strcmp(envelope->type, "ROOM_LEAVE") == 0) {
        Room *room = rooms_find_by_user(rooms, userId);
        if (!room) {
            sendOk(app, connId, envelope->id, NULL);
            return OK;
        }
        char code[7];
        strcpy(code, room->code);
        rooms_leave(rooms, userId);
        sendOk(app, connId, envelope->id, NULL);

        Room *after = rooms_find_by_code(rooms, code);
        if (after) {
            rooms_broadcast_state(app, after);
        }
        return OK;
    }

    if (strcmp(envelope->type, "ROOM_MODE") == 0) {
        struct json_object *modeJson = NULL;
        if (!envelope->payload || !json_object_object_get_ex(envelope->payload, "mode",
                                                             &modeJson)) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Mode is required");
            return OK;
        }
        Mode mode = CLASSIC;
        if (!parseMode(modeJson, &mode)) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Invalid mode");
            return OK;
        }
        Status status = rooms_set_mode(rooms, userId, mode);
        if (status == FORBIDDEN) {
            sendError(app, connId, envelope->id, "FORBIDDEN", "Only host can change room mode");
            return OK;
        }
        if (status == NOT_FOUND) {
            sendError(app, connId, envelope->id, "NOT_FOUND", "Room not found");
            return OK;
        }
        if (status != OK) {
            sendError(app, connId, envelope->id, "INVALID_ACTION", "Could not set mode");
            return OK;
        }

        struct json_object *responsePayload = json_object_new_object();
        json_object_object_add(responsePayload, "mode", json_object_new_string(MODE_NAMES[mode]));
        sendOk(app, connId, envelope->id, responsePayload);

        Room *room = rooms_find_by_user(rooms, userId);
        if (room) rooms_broadcast_state(app, room);
        return OK;
    }

    if (strcmp(envelope->type, "ROOM_BOT") == 0) {
        struct json_object *seatJson = NULL, *levelJson = NULL;
        if (!envelope->payload ||
            !json_object_object_get_ex(envelope->payload, "seat", &seatJson) ||
            !json_object_object_get_ex(envelope->payload, "level", &levelJson)) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Seat and level are required");
            return OK;
        }
        int seatValue = json_object_get_int(seatJson);
        if (seatValue < 0 || seatValue >= ROOM_MAX_SEATS) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Seat must be between 0 and 3");
            return OK;
        }
        BotLevel level = EASY;
        if (!parseBotLevel(levelJson, &level)) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Invalid bot level");
            return OK;
        }

        Status status = rooms_set_bot(rooms, userId, seatValue, level);
        if (status == FORBIDDEN) {
            sendError(app, connId, envelope->id, "FORBIDDEN", "Only host can configure bots");
            return OK;
        }
        if (status == NOT_FOUND) {
            sendError(app, connId, envelope->id, "NOT_FOUND", "Room not found");
            return OK;
        }
        if (status != OK) {
            sendError(app, connId, envelope->id, "INVALID_ACTION", "Could not configure bot");
            return OK;
        }

        struct json_object *responsePayload = json_object_new_object();
        json_object_object_add(responsePayload, "seat", json_object_new_int(seatValue));
        const char *levelNames[] = {"easy", "normal", "hard"};
        json_object_object_add(responsePayload, "level", json_object_new_string(levelNames[level]));
        sendOk(app, connId, envelope->id, responsePayload);

        Room *room = rooms_find_by_user(rooms, userId);
        if (room) rooms_broadcast_state(app, room);
        return OK;
    }

    if (strcmp(envelope->type, "ROOM_KICK") == 0) {
        struct json_object *seatJson = NULL;
        if (!envelope->payload || !json_object_object_get_ex(envelope->payload, "seat",
                                                             &seatJson)) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Seat is required");
            return OK;
        }
        int seatValue = json_object_get_int(seatJson);
        if (seatValue < 0 || seatValue >= ROOM_MAX_SEATS) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Seat must be between 0 and 3");
            return OK;
        }
        /* rooms_kick wipes the seat, so remember who sat there to notify them. */
        UserId kickedUser = 0;
        Room *before = rooms_find_by_user(rooms, userId);
        if (before && before->seats[seatValue].occupied && !before->seats[seatValue].is_bot) {
            kickedUser = before->seats[seatValue].user_id;
        }
        Status status = rooms_kick(rooms, userId, seatValue);
        if (status == FORBIDDEN) {
            sendError(app, connId, envelope->id, "FORBIDDEN", "Only host can kick members");
            return OK;
        }
        if (status == NOT_FOUND) {
            sendError(app, connId, envelope->id, "NOT_FOUND", "Room not found");
            return OK;
        }
        if (status != OK) {
            sendError(app, connId, envelope->id, "INVALID_ACTION", "Could not kick member");
            return OK;
        }

        struct json_object *responsePayload = json_object_new_object();
        json_object_object_add(responsePayload, "seat", json_object_new_int(seatValue));
        sendOk(app, connId, envelope->id, responsePayload);

        Room *room = rooms_find_by_user(rooms, userId);
        if (room) {
            /* The kicked user is no longer seated, so the broadcast skips them;
             * the same snapshot tells their client it left the room. */
            json_object *state = roomStateJson(room);
            pushToSeatedHumans(app, room, state);
            if (kickedUser) (void)app_push(app, kickedUser, "ROOM_STATE", state);
            json_object_put(state);
        }
        return OK;
    }

    if (strcmp(envelope->type, "ROOM_START") == 0) {
        Room *room = rooms_find_by_user(rooms, userId);
        if (!room) {
            sendError(app, connId, envelope->id, "NOT_FOUND", "Room not found");
            return OK;
        }
        if (room->seats[room->host_seat].user_id != userId) {
            sendError(app, connId, envelope->id, "FORBIDDEN", "Only host can start room");
            return OK;
        }

        Status status = rooms_start(app, room->code);
        if (status == BUSY) {
            sendError(app, connId, envelope->id, "BUSY", "Match engine not available");
            return OK;
        }
        if (status != OK) {
            sendError(app, connId, envelope->id, "INVALID_ACTION",
                      "Cannot start room: all 4 seats must be filled");
            return OK;
        }

        sendOk(app, connId, envelope->id, NULL);
        return OK;
    }

    if (strcmp(envelope->type, "INVITE_SEND") == 0) {
        struct json_object *userJson = NULL, *kindJson = NULL;
        if (!envelope->payload ||
            !json_object_object_get_ex(envelope->payload, "userId", &userJson) ||
            !json_object_object_get_ex(envelope->payload, "kind", &kindJson)) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "userId and kind required");
            return OK;
        }

        UserId targetUserId = 0;
        if (json_object_is_type(userJson, json_type_string)) {
            targetUserId = (UserId)strtoull(json_object_get_string(userJson), NULL, 10);
        } else if (json_object_is_type(userJson, json_type_int)) {
            targetUserId = (UserId)json_object_get_int64(userJson);
        }

        if (targetUserId == 0 || targetUserId == userId) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Invalid target user");
            return OK;
        }

        const char *kind = json_object_get_string(kindJson);
        if (strcasecmp(kind, "room") != 0 && strcasecmp(kind, "party") != 0) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Kind must be room or party");
            return OK;
        }
        bool isParty = strcasecmp(kind, "party") == 0;
        Mode partyMode = TWO_V_TWO;
        if (isParty && !parsePartyMode(envelope->payload, &partyMode)) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "Party mode must be 2v2 or side");
            return OK;
        }

        /* Check recipient is online */
        ConnId recipientConn;
        if (sessions_find_conn(app, targetUserId, &recipientConn) != OK) {
            sendError(app, connId, envelope->id, "NOT_FOUND", "User is offline");
            return OK;
        }

        char roomCode[7] = {0};
        if (strcasecmp(kind, "room") == 0) {
            Room *room = rooms_find_by_user(rooms, userId);
            if (!room) {
                sendError(app, connId, envelope->id, "INVALID_ACTION", "You are not in a room");
                return OK;
            }
            int occupiedSeats = 0;
            for (int i = 0; i < ROOM_MAX_SEATS; i++) {
                if (room->seats[i].occupied) occupiedSeats++;
            }
            if (occupiedSeats >= ROOM_MAX_SEATS) {
                sendError(app, connId, envelope->id, "ROOM_FULL", "Room is full");
                return OK;
            }
            if (rooms_find_by_user(rooms, targetUserId) != NULL) {
                sendError(app, connId, envelope->id, "BUSY", "Target user is already in a room");
                return OK;
            }
            strcpy(roomCode, room->code);
        } else {
            /* Party */
            Party *party = party_find_by_user(rooms, userId);
            if (party && party->member_count >= PARTY_MAX_MEMBERS) {
                sendError(app, connId, envelope->id, "BUSY", "Party is full");
                return OK;
            }
            if (party_find_by_user(rooms, targetUserId) != NULL) {
                sendError(app, connId, envelope->id, "BUSY", "Target user is already in a party");
                return OK;
            }
        }

        /* Create invite */
        char inviteIdBuffer[37];
        Status inviteStatus = rooms_invite_create(rooms, userId, targetUserId, kind, roomCode,
                                                  inviteIdBuffer);
        if (inviteStatus != OK) {
            sendError(app, connId, envelope->id, "SERVER_FULL", "Too many active invites");
            return OK;
        }
        Invite *invite = NULL;
        for (int i = 0; i < MAX_INVITES; i++) {
            if (rooms->invites[i].active && strcmp(rooms->invites[i].invite_id,
                                                   inviteIdBuffer) == 0) {
                invite = &rooms->invites[i];
                break;
            }
        }
        if (!invite) {
            sendError(app, connId, envelope->id, "STORAGE_ERROR", "Failed to allocate invite");
            return OK;
        }
        invite->mode = partyMode;

        /* Push INVITE to recipient */
        struct json_object *pushPayload = json_object_new_object();
        json_object_object_add(pushPayload, "inviteId", json_object_new_string(invite->invite_id));
        char senderText[32];
        snprintf(senderText, sizeof(senderText), "%llu", (unsigned long long)userId);
        json_object_object_add(pushPayload, "senderId", json_object_new_string(senderText));
        json_object_object_add(pushPayload, "senderName", json_object_new_string(account.name));
        json_object_object_add(pushPayload, "kind", json_object_new_string(invite->kind));
        if (roomCode[0]) {
            json_object_object_add(pushPayload, "roomId", json_object_new_string(roomCode));
        }
        if (isParty) {
            json_object_object_add(pushPayload, "mode",
                                   json_object_new_string(MODE_NAMES[invite->mode]));
        }

        Envelope pushEnvelope = {1, 0, "INVITE", pushPayload};
        if (recipientConn.slot < APP_MAX_CONNS) {
            AppConnSlot *recipientSlot = &app->slots[recipientConn.slot];
            if (recipientSlot->active && recipientSlot->generation == recipientConn.generation) {
                conn_queue(&recipientSlot->conn, &pushEnvelope);
                conn_flush(&recipientSlot->conn);
            }
        }
        json_object_put(pushPayload);

        /* Send OK to sender */
        struct json_object *responsePayload = json_object_new_object();
        json_object_object_add(responsePayload, "inviteId",
                               json_object_new_string(invite->invite_id));
        sendOk(app, connId, envelope->id, responsePayload);
        return OK;
    }

    if (strcmp(envelope->type, "INVITE_REPLY") == 0) {
        struct json_object *idJson = NULL, *acceptJson = NULL;
        if (!envelope->payload ||
            !json_object_object_get_ex(envelope->payload, "inviteId", &idJson) ||
            !json_object_object_get_ex(envelope->payload, "accept", &acceptJson)) {
            sendError(app, connId, envelope->id, "BAD_REQUEST", "inviteId and accept required");
            return OK;
        }

        const char *inviteId = json_object_get_string(idJson);
        bool accept = json_object_get_boolean(acceptJson);

        Invite *invite = NULL;
        for (int i = 0; i < MAX_INVITES; i++) {
            if (rooms->invites[i].active && strcmp(rooms->invites[i].invite_id, inviteId) == 0) {
                invite = &rooms->invites[i];
                break;
            }
        }

        if (!invite || invite->recipient_id != userId) {
            sendError(app, connId, envelope->id, "NOT_FOUND", "Invite not found");
            return OK;
        }

        uint64_t now = monotonicNowMs();
        if (now - invite->created_at_ms > INVITE_TIMEOUT_MS) {
            invite->active = false;
            sendError(app, connId, envelope->id, "NOT_FOUND", "Invite has expired");
            return OK;
        }

        if (!accept) {
            invite->active = false;
            sendOk(app, connId, envelope->id, NULL);
            return OK;
        }

        /* Accept invite; the invite stays pending so it can be accepted after the match. */
        if (rejectIfInMatch(app, connId, envelope->id, userId)) return OK;
        if (rejectIfQueued(app, connId, envelope->id, userId)) return OK;
        /* A party entry always holds both members, so a solo-queued inviter
         * cannot gain a partner either. */
        if (strcasecmp(invite->kind,
                       "party") == 0 && queue_contains(app->queue, invite->sender_id)) {
            sendError(app, connId, envelope->id, "BUSY", "Inviter is in matchmaking");
            return OK;
        }
        if (strcasecmp(invite->kind, "room") == 0) {
            if (rooms_find_by_user(rooms, userId) != NULL) {
                sendError(app, connId, envelope->id, "BUSY", "Already in a room");
                return OK;
            }
            Seat seat = 0;
            Status status = rooms_join(rooms, userId, account.name, account.avatar,
                                       invite->room_code, &seat);
            if (status == BUSY) {
                sendError(app, connId, envelope->id, "BUSY", "Already in a room");
                return OK;
            }
            if (status == NOT_FOUND) {
                invite->active = false;
                sendError(app, connId, envelope->id, "NOT_FOUND", "Room not found");
                return OK;
            }
            if (status == ROOM_FULL) {
                invite->active = false;
                sendError(app, connId, envelope->id, "ROOM_FULL", "Room is full");
                return OK;
            }
            if (status != OK) {
                invite->active = false;
                sendError(app, connId, envelope->id, "INVALID_ACTION", "Could not join room");
                return OK;
            }

            char code[7];
            strcpy(code, invite->room_code);
            invite->active = false;

            struct json_object *responsePayload = json_object_new_object();
            json_object_object_add(responsePayload, "roomId", json_object_new_string(code));
            json_object_object_add(responsePayload, "code", json_object_new_string(code));
            json_object_object_add(responsePayload, "seat", json_object_new_int(seat));
            sendOk(app, connId, envelope->id, responsePayload);

            Room *room = rooms_find_by_code(rooms, code);
            if (room) rooms_broadcast_state(app, room);
            return OK;
        } else {
            /* Party invite accepted */
            if (party_find_by_user(rooms, userId) != NULL) {
                sendError(app, connId, envelope->id, "BUSY", "Already in a party");
                return OK;
            }
            Party *party = party_find_by_user(rooms, invite->sender_id);
            if (!party) {
                /* Create new party */
                int partySlot = -1;
                for (int i = 0; i < MAX_PARTIES; i++) {
                    if (!rooms->parties[i].active) {
                        partySlot = i;
                        break;
                    }
                }
                if (partySlot >= 0) {
                    party = &rooms->parties[partySlot];
                    party->active = true;
                    party->party_id = rooms->next_party_id++;
                    party->leader_id = invite->sender_id;
                    party->members[0] = invite->sender_id;
                    party->members[1] = userId;
                    party->member_count = 2;
                    party->mode = invite->mode;
                    party->revision = 1;
                }
            } else {
                if (party->member_count < PARTY_MAX_MEMBERS) {
                    party->members[party->member_count++] = userId;
                    party->revision++;
                } else {
                    invite->active = false;
                    sendError(app, connId, envelope->id, "BUSY", "Party is full");
                    return OK;
                }
            }

            invite->active = false;
            sendOk(app, connId, envelope->id, NULL);

            if (party) broadcastPartyState(app, party);
            return OK;
        }
    }

    if (strcmp(envelope->type, "PARTY_LEAVE") == 0) {
        /* The queued party entry cannot outlive the party; a solo entry stays. */
        if (party_find_by_user(rooms, userId)) queue_user_gone(app, userId);
        party_leave(rooms, userId);
        sendOk(app, connId, envelope->id, NULL);
        return OK;
    }

    sendError(app, connId, envelope->id, "BAD_REQUEST", "Unknown command");
    return OK;
}
