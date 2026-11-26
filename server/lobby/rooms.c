#define _POSIX_C_SOURCE 200809L
#include "rooms.h"
#include "server/app.h"
#include "server/auth/accounts.h"
#include "server/game/match.h"
#include "server/lobby/lobby_common.h"
#include "server/lobby/queue.h"
#include "server/storage/store.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

enum {
    QUEUE_ROOM_PREFIX = 'Q',
    BOT_LEVEL_COUNT = HARD + 1,
    /* UUID phiên bản 4 (RFC 4122): 16 byte, nibble phiên bản ở byte 6, variant ở byte 8. */
    UUID_BYTES = 16,
    UUID_VERSION_BYTE = 6,
    UUID_VERSION_MASK = 0x0F,
    UUID_VERSION_4 = 0x40,
    UUID_VARIANT_BYTE = 8,
    UUID_VARIANT_MASK = 0x3F,
    UUID_VARIANT_RFC4122 = 0x80
};

/* 36 ký hiệu cho mã phòng: dễ đọc to, khoảng 2,2 tỷ mã 6 ký tự. */
static const char CODE_ALPHABET[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
#define CODE_ALPHABET_SIZE (sizeof(CODE_ALPHABET) - 1)

static const char *const BOT_LEVEL_NAMES[BOT_LEVEL_COUNT] = {"easy", "normal", "hard"};

/* Một request phòng đang xử lý, kèm danh tính người gửi dùng cho ghế và lời mời. */
typedef struct {
    App *app;
    Rooms *rooms;
    ConnId connId;
    const Envelope *envelope;
    UserId userId;
    Account account;
} RoomRequest;

/* Thông báo lỗi riêng cho từng Status mà một thao tác phòng có thể trả về. */
typedef struct {
    Status status;
    const char *message;
} StatusMessage;

/* ----------------------------- Gửi phản hồi ----------------------------- */

/* Slot của kết nối nếu vẫn là đúng socket đó (cùng generation), ngược lại NULL. */
static AppConnSlot *liveSlot(App *app, ConnId connId) {
    if (connId.slot >= APP_MAX_CONNS) return NULL;
    AppConnSlot *slot = &app->slots[connId.slot];
    return slot->active && slot->generation == connId.generation ? slot : NULL;
}

/* Gửi một envelope tới kết nối rồi giải phóng payload. Lỗi gửi chỉ bật cờ closing;
 * vòng poll sẽ đóng kết nối sau. */
static void sendToConn(App *app, ConnId connId, int32_t requestId, const char *type,
                       json_object *payload) {
    AppConnSlot *slot = liveSlot(app, connId);
    if (slot) {
        Envelope response = {PROTOCOL_VERSION, requestId, type, payload};
        if (connQueue(&slot->conn, &response) == OK) (void)connFlush(&slot->conn);
    }
    json_object_put(payload);
}

static void replyError(const RoomRequest *request, const char *code, const char *message) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "code", json_object_new_string(code));
    json_object_object_add(payload, "message", json_object_new_string(message));
    sendToConn(request->app, request->connId, request->envelope->id, "ERROR", payload);
}

/* Trả OK; @p payload NULL nghĩa là payload rỗng. Hàm này chiếm quyền sở hữu payload. */
static void replyOk(const RoomRequest *request, json_object *payload) {
    sendToConn(request->app, request->connId, request->envelope->id, "OK",
               payload ? payload : json_object_new_object());
}

/* Đổi Status lỗi thành ERROR: Status có trong bảng dùng mã và thông báo của nó,
 * còn lại là INVALID_ACTION với @p fallback. */
static void replyStatusError(const RoomRequest *request, Status status,
                             const StatusMessage messages[], size_t count,
                             const char *fallback) {
    for (size_t i = 0; i < count; i++) {
        if (messages[i].status == status) {
            replyError(request, statusCode(status), messages[i].message);
            return;
        }
    }
    replyError(request, "INVALID_ACTION", fallback);
}

/* Trường @p key của payload, hoặc NULL khi không có payload/trường. */
static json_object *payloadField(const Envelope *envelope, const char *key) {
    json_object *value = NULL;
    if (!envelope->payload || !json_object_object_get_ex(envelope->payload, key, &value)) {
        return NULL;
    }
    return value;
}

/* ------------------------------ Tiện ích ghế ------------------------------ */

static bool validSeat(int seat) {
    return seat >= 0 && seat < ROOM_MAX_SEATS;
}

static bool isHumanSeat(const RoomSeat *seat) {
    return seat->occupied && !seat->isBot;
}

static bool isHost(const Room *room, UserId userId) {
    return room->seats[room->hostSeat].userId == userId;
}

/* Ghế của người thật @p userId trong phòng, hoặc -1. */
static int humanSeatOf(const Room *room, UserId userId) {
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (isHumanSeat(&room->seats[seat]) && room->seats[seat].userId == userId) return seat;
    }
    return -1;
}

static int lowestEmptySeat(const Room *room) {
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (!room->seats[seat].occupied) return seat;
    }
    return -1;
}

/* Ghế người thật có số nhỏ nhất, hoặc -1 khi phòng không còn người thật. */
static int lowestHumanSeat(const Room *room) {
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (isHumanSeat(&room->seats[seat])) return seat;
    }
    return -1;
}

/* Đặt người thật vào ghế; tên/avatar NULL dùng giá trị mặc định. */
static void seatHuman(RoomSeat *seat, UserId userId, const char *name, const char *avatar,
                      const char *defaultName) {
    memset(seat, 0, sizeof(*seat));
    seat->occupied = true;
    seat->userId = userId;
    snprintf(seat->name, sizeof(seat->name), "%s", name ? name : defaultName);
    snprintf(seat->avatar, sizeof(seat->avatar), "%s", avatar ? avatar : "fox");
}

/**
 * @brief Phòng mà @p requesterId đang làm host.
 * @param[out] outStatus NOT_FOUND khi không ở phòng nào, FORBIDDEN khi không phải host.
 * @return Phòng, hoặc NULL kèm @p outStatus.
 */
static Room *hostedRoom(Rooms *rooms, UserId requesterId, Status *outStatus) {
    Room *room = roomsFindByUser(rooms, requesterId);
    if (!room) {
        *outStatus = NOT_FOUND;
        return NULL;
    }
    if (!isHost(room, requesterId)) {
        *outStatus = FORBIDDEN;
        return NULL;
    }
    return room;
}

/* ------------------------------ Mã ngẫu nhiên ------------------------------ */

/* "Q" + 5 chữ số dành cho trận hàng chờ; phòng mang mã đó sẽ bị roomsMatchEnded
 * nhả nhầm khi trận hàng chờ cùng mã kết thúc. */
static bool isQueueRoomId(const char *code) {
    if (code[0] != QUEUE_ROOM_PREFIX) return false;
    for (int i = 1; i < ROOM_CODE_LENGTH; i++) {
        if (code[i] < '0' || code[i] > '9') return false;
    }
    return true;
}

/* UUID phiên bản 4 ngẫu nhiên cho mã lời mời, để không thể đoán được. */
static void generateUuid(char outUuid[INVITE_ID_SIZE]) {
    uint8_t bytes[UUID_BYTES];
    randombytes_buf(bytes, sizeof(bytes));
    bytes[UUID_VERSION_BYTE] = (bytes[UUID_VERSION_BYTE] & UUID_VERSION_MASK) | UUID_VERSION_4;
    bytes[UUID_VARIANT_BYTE] =
        (bytes[UUID_VARIANT_BYTE] & UUID_VARIANT_MASK) | UUID_VARIANT_RFC4122;
    snprintf(outUuid, INVITE_ID_SIZE,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
             bytes[15]);
}

/* Mã đã thuộc về phòng đang mở khác (bỏ qua chính buffer đang được điền). */
static bool codeInUse(const Rooms *rooms, const char *candidate, const char *ownBuffer) {
    for (int i = 0; i < MAX_ROOMS; i++) {
        const Room *other = &rooms->rooms[i];
        if (other->active && other->code != ownBuffer && strcmp(other->code, candidate) == 0) {
            return true;
        }
    }
    return false;
}

void roomsGenerateCode(Rooms *rooms, char outCode[ROOM_CODE_SIZE]) {
    char candidate[ROOM_CODE_SIZE];
    do {
        uint8_t codeBytes[ROOM_CODE_LENGTH];
        randombytes_buf(codeBytes, sizeof(codeBytes));
        for (int i = 0; i < ROOM_CODE_LENGTH; i++) {
            candidate[i] = CODE_ALPHABET[codeBytes[i] % CODE_ALPHABET_SIZE];
        }
        candidate[ROOM_CODE_LENGTH] = '\0';
    } while (isQueueRoomId(candidate) || (rooms && codeInUse(rooms, candidate, outCode)));
    memcpy(outCode, candidate, ROOM_CODE_SIZE);
}

/* ------------------------------ Đọc tham số ------------------------------ */

/* Chế độ dạng số (0..3) hoặc tên; thiếu trường thì mặc định CLASSIC. */
static bool parseMode(json_object *valueJson, Mode *outMode) {
    if (!valueJson) {
        *outMode = CLASSIC;
        return true;
    }
    if (json_object_is_type(valueJson, json_type_int)) {
        int modeNumber = json_object_get_int(valueJson);
        if (modeNumber < 0 || modeNumber >= LOBBY_MODE_COUNT) return false;
        *outMode = (Mode)modeNumber;
        return true;
    }
    if (!json_object_is_type(valueJson, json_type_string)) return false;
    const char *text = json_object_get_string(valueJson);
    if (strcasecmp(text, "classic") == 0) *outMode = CLASSIC;
    else if (strcasecmp(text, "2v2") == 0 || strcasecmp(text, "two_v_two") == 0) {
        *outMode = TWO_V_TWO;
    }
    else if (strcasecmp(text, "side") == 0) *outMode = SIDE;
    else if (strcasecmp(text, "draft") == 0) *outMode = DRAFT;
    else return false;
    return true;
}

/* Party chỉ xếp hàng ở chế độ đồng đội, nên INVITE_SEND phải ghi rõ "2v2" hoặc "side". */
static bool parsePartyMode(const Envelope *envelope, Mode *outMode) {
    json_object *modeJson = payloadField(envelope, "mode");
    if (!json_object_is_type(modeJson, json_type_string)) return false;
    Mode mode = CLASSIC;
    if (!lobbyParseModeName(json_object_get_string(modeJson), &mode)) return false;
    if (mode != TWO_V_TWO && mode != SIDE) return false;
    *outMode = mode;
    return true;
}

/* Mức bot dạng số (0..2) hoặc tên; "medium" là tên khác của "normal". */
static bool parseBotLevel(json_object *valueJson, BotLevel *outLevel) {
    if (json_object_is_type(valueJson, json_type_int)) {
        int levelNumber = json_object_get_int(valueJson);
        if (levelNumber < 0 || levelNumber >= BOT_LEVEL_COUNT) return false;
        *outLevel = (BotLevel)levelNumber;
        return true;
    }
    if (!json_object_is_type(valueJson, json_type_string)) return false;
    const char *text = json_object_get_string(valueJson);
    if (strcasecmp(text, "easy") == 0) *outLevel = EASY;
    else if (strcasecmp(text, "normal") == 0 || strcasecmp(text, "medium") == 0) *outLevel = NORMAL;
    else if (strcasecmp(text, "hard") == 0) *outLevel = HARD;
    else return false;
    return true;
}

/* Người đang có ghế trong trận vẫn bị giữ chỗ tới khi trận chốt, kể cả sau ROOM_LEAVE. */
static bool rejectIfInMatch(const RoomRequest *request) {
    if (!matchesContains(request->app->matches, request->userId)) return false;
    replyError(request, "BUSY", "User is in a match");
    return true;
}

/* Spec: người đang chờ ghép trận không thể đồng thời ngồi phòng hay ở party. */
static bool rejectIfQueued(const RoomRequest *request) {
    if (!queueContains(request->app->queue, request->userId)) return false;
    replyError(request, "BUSY", "User is in matchmaking");
    return true;
}

/* --------------------------- Thao tác trên phòng --------------------------- */

Status roomsInit(Rooms **outRooms, App *app, OnMatchStart onStart) {
    if (!outRooms) return BAD_REQUEST;
    Rooms *rooms = calloc(1, sizeof(Rooms));
    if (!rooms) return STORAGE_ERROR;
    rooms->app = app;
    rooms->onStart = onStart;
    rooms->nextPartyId = 1;
    rooms->nextInviteId = 1;
    *outRooms = rooms;
    return OK;
}

void roomsDestroy(Rooms *rooms) {
    free(rooms);
}

Room *roomsFindByCode(Rooms *rooms, const char *code) {
    if (!rooms || !code) return NULL;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms->rooms[i].active && strcasecmp(rooms->rooms[i].code, code) == 0) {
            return &rooms->rooms[i];
        }
    }
    return NULL;
}

Room *roomsFindByUser(Rooms *rooms, UserId userId) {
    if (!rooms || userId == 0) return NULL;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms->rooms[i].active && humanSeatOf(&rooms->rooms[i], userId) >= 0) {
            return &rooms->rooms[i];
        }
    }
    return NULL;
}

static Room *freeRoom(Rooms *rooms) {
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (!rooms->rooms[i].active) return &rooms->rooms[i];
    }
    return NULL;
}

Status roomsCreate(Rooms *rooms, UserId hostId, const char *name, const char *avatar, Mode mode,
                   char outCode[ROOM_CODE_SIZE]) {
    if (!rooms || hostId == 0) return BAD_REQUEST;
    if (roomsFindByUser(rooms, hostId) != NULL) return BUSY;
    Room *room = freeRoom(rooms);
    if (!room) return SERVER_FULL;

    memset(room, 0, sizeof(*room));
    roomsGenerateCode(rooms, room->code);
    room->active = true;
    room->mode = mode;
    room->ranked = false;
    room->hostSeat = 0;
    room->revision = 1;
    room->status = ROOM_STATUS_WAITING;
    seatHuman(&room->seats[0], hostId, name, avatar, "Host");
    if (outCode) snprintf(outCode, ROOM_CODE_SIZE, "%s", room->code);
    return OK;
}

Status roomsJoin(Rooms *rooms, UserId userId, const char *name, const char *avatar,
                 const char *code, Seat *outSeat) {
    if (!rooms || userId == 0 || !code) return BAD_REQUEST;
    if (roomsFindByUser(rooms, userId) != NULL) return BUSY;
    Room *room = roomsFindByCode(rooms, code);
    if (!room) return NOT_FOUND;
    int emptySeat = lowestEmptySeat(room);
    if (emptySeat < 0) return ROOM_FULL;

    seatHuman(&room->seats[emptySeat], userId, name, avatar, "Player");
    room->revision++;
    if (outSeat) *outSeat = (Seat)emptySeat;
    return OK;
}

Status roomsLeave(Rooms *rooms, UserId userId) {
    if (!rooms || userId == 0) return OK;
    Room *room = roomsFindByUser(rooms, userId);
    if (!room) return OK;
    int seat = humanSeatOf(room, userId);
    if (seat < 0) return OK;

    memset(&room->seats[seat], 0, sizeof(room->seats[seat]));
    room->revision++;
    int nextHost = lowestHumanSeat(room);
    if (nextHost < 0) {
        /* Người thật cuối cùng đã rời: đóng phòng. */
        memset(room, 0, sizeof(*room));
    } else if (seat == room->hostSeat) {
        room->hostSeat = (Seat)nextHost;
    }
    return OK;
}

Status roomsSetBot(Rooms *rooms, UserId requesterId, int seat, BotLevel level) {
    if (!rooms || requesterId == 0 || !validSeat(seat)) return BAD_REQUEST;
    Status status = OK;
    Room *room = hostedRoom(rooms, requesterId, &status);
    if (!room) return status;
    /* Host không tự biến mình thành bot, và bot không được thay người thật. */
    if (seat == room->hostSeat || isHumanSeat(&room->seats[seat])) return INVALID_ACTION;

    RoomSeat *botSeat = &room->seats[seat];
    botSeat->occupied = true;
    botSeat->isBot = true;
    botSeat->userId = 0;
    botSeat->botLevel = level;
    snprintf(botSeat->name, sizeof(botSeat->name), "Bot %u", (unsigned)(seat + 1));
    snprintf(botSeat->avatar, sizeof(botSeat->avatar), "%s", "robot");
    room->revision++;
    return OK;
}

Status roomsKick(Rooms *rooms, UserId requesterId, int seat) {
    if (!rooms || requesterId == 0 || !validSeat(seat)) return BAD_REQUEST;
    Status status = OK;
    Room *room = hostedRoom(rooms, requesterId, &status);
    if (!room) return status;
    /* Host không tự kick mình, và không có gì để kick ở ghế trống. */
    if (seat == room->hostSeat || !room->seats[seat].occupied) return INVALID_ACTION;
    memset(&room->seats[seat], 0, sizeof(room->seats[seat]));
    room->revision++;
    return OK;
}

Status roomsSetMode(Rooms *rooms, UserId requesterId, Mode mode) {
    if (!rooms || requesterId == 0) return BAD_REQUEST;
    Status status = OK;
    Room *room = hostedRoom(rooms, requesterId, &status);
    if (!room) return status;
    room->mode = mode;
    room->revision++;
    return OK;
}

/* Chép bốn ghế của phòng sang RoomStart mà coordinator dùng để mở trận. */
static void fillRoomStart(RoomStart *roomStart, const Room *room) {
    memset(roomStart, 0, sizeof(*roomStart));
    snprintf(roomStart->roomId, sizeof(roomStart->roomId), "%s", room->code);
    roomStart->mode = room->mode;
    roomStart->ranked = room->ranked;
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        PlayerSpec *player = &roomStart->players[i];
        player->userId = room->seats[i].userId;
        snprintf(player->name, sizeof(player->name), "%s", room->seats[i].name);
        snprintf(player->avatar, sizeof(player->avatar), "%s", room->seats[i].avatar);
        player->isBot = room->seats[i].isBot;
        player->botLevel = room->seats[i].botLevel;
    }
}

Status roomsStart(App *app, const char *roomId) {
    if (!app || !app->rooms || !roomId) return BAD_REQUEST;
    Rooms *rooms = app->rooms;
    Room *room = roomsFindByCode(rooms, roomId);
    if (!room) return NOT_FOUND;
    if (room->status != ROOM_STATUS_WAITING || lowestEmptySeat(room) >= 0) return INVALID_ACTION;
    if (!rooms->onStart) return BUSY;

    RoomStart roomStart;
    fillRoomStart(&roomStart, room);
    MatchId matchId = {0};
    Status status = rooms->onStart(app, &roomStart, &matchId);
    /* Callback lỗi thì phòng giữ nguyên trạng thái chờ. */
    if (status == OK) room->status = ROOM_STATUS_IN_MATCH;
    return status;
}

/* Phát ROOM_STATE cho phòng mang mã @p code nếu phòng đó vẫn còn. */
static void broadcastRoomByCode(App *app, const char *code) {
    Room *room = roomsFindByCode(app->rooms, code);
    if (room) roomsBroadcastState(app, room);
}

void roomsDisconnect(App *app, UserId userId, uint64_t nowMs) {
    (void)nowMs;
    if (!app || !app->rooms || userId == 0) return;
    Rooms *rooms = app->rooms;
    partyLeave(rooms, userId);

    /* Hủy mọi lời mời mà user đã gửi hoặc đang nhận. */
    for (int i = 0; i < MAX_INVITES; i++) {
        Invite *invite = &rooms->invites[i];
        if (invite->active && (invite->senderId == userId || invite->recipientId == userId)) {
            invite->active = false;
        }
    }
    Room *room = roomsFindByUser(rooms, userId);
    if (room && room->status == ROOM_STATUS_WAITING) {
        char code[ROOM_CODE_SIZE];
        snprintf(code, sizeof(code), "%s", room->code);
        roomsLeave(rooms, userId);
        broadcastRoomByCode(app, code);
    }
}

Status roomsMakeMatch(App *app, Mode mode, const UserId users[ROOM_MAX_SEATS],
                      MatchId *outMatchId) {
    if (!app || !app->rooms || !users || !outMatchId) return BAD_REQUEST;
    Rooms *rooms = app->rooms;
    if (!rooms->onStart) return BUSY;
    if (!app->store) return STORAGE_ERROR;

    RoomStart roomStart;
    memset(&roomStart, 0, sizeof(roomStart));
    unsigned serial = (unsigned)(rooms->nextQueueRoom % QUEUE_ROOM_SERIAL_LIMIT);
    snprintf(roomStart.roomId, sizeof(roomStart.roomId), "%c%05u", QUEUE_ROOM_PREFIX, serial);
    roomStart.mode = mode;
    /* Spec: chỉ trận ghép từ hàng chờ với bốn người thật mới ranked; không có bot. */
    roomStart.ranked = true;
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (users[seat] == 0) return BAD_REQUEST;
        Account account;
        Status lookup = storeAccountGetById(app->store, users[seat], &account);
        if (lookup != OK) return lookup;
        PlayerSpec *player = &roomStart.players[seat];
        player->userId = users[seat];
        snprintf(player->name, sizeof(player->name), "%s", account.name);
        snprintf(player->avatar, sizeof(player->avatar), "%s", account.avatar);
    }
    /* Coordinator tự lưu trận và tự push GAME_STATE. */
    Status started = rooms->onStart(app, &roomStart, outMatchId);
    if (started == OK) rooms->nextQueueRoom++;
    return started;
}

/* ------------------------------ Snapshot JSON ------------------------------ */

static json_object *seatStateJson(const RoomSeat *seat, int seatIndex) {
    json_object *seatJson = json_object_new_object();
    json_object_object_add(seatJson, "seat", json_object_new_int(seatIndex));
    json_object_object_add(seatJson, "isBot", json_object_new_boolean(seat->isBot));
    if (!seat->isBot) json_object_object_add(seatJson, "userId", lobbyUserIdJson(seat->userId));
    json_object_object_add(seatJson, "name", json_object_new_string(seat->name));
    json_object_object_add(seatJson, "avatar", json_object_new_string(seat->avatar));
    if (seat->isBot) {
        json_object_object_add(seatJson, "botLevel",
                               json_object_new_string(BOT_LEVEL_NAMES[seat->botLevel]));
    }
    return seatJson;
}

/* Một hàm dựng duy nhất cho mọi ROOM_STATE, để broadcast, kick và đăng nhập lại khớp nhau. */
static json_object *roomStateJson(const Room *room) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "revision", json_object_new_int64(room->revision));
    json_object_object_add(payload, "roomId", json_object_new_string(room->code));
    json_object_object_add(payload, "code", json_object_new_string(room->code));
    json_object_object_add(payload, "mode", json_object_new_string(lobbyModeName(room->mode)));
    json_object_object_add(payload, "ranked", json_object_new_boolean(room->ranked));
    json_object_object_add(payload, "hostSeat", json_object_new_int(room->hostSeat));
    json_object *seatsJson = json_object_new_array();
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        const RoomSeat *seat = &room->seats[i];
        /* Ghế trống là null để client giữ đúng vị trí bốn ghế. */
        json_object_array_add(seatsJson, seat->occupied ? seatStateJson(seat, i) : NULL);
    }
    json_object_object_add(payload, "seats", seatsJson);
    return payload;
}

static void pushToSeatedHumans(App *app, const Room *room, json_object *payload) {
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        /* Thành viên offline lỡ bản push này; LOGIN đồng bộ lại qua roomsSyncUser. */
        if (isHumanSeat(&room->seats[i])) {
            (void)appPush(app, room->seats[i].userId, "ROOM_STATE", payload);
        }
    }
}

Status roomsBroadcastState(App *app, const Room *room) {
    if (!app || !room) return BAD_REQUEST;
    json_object *payload = roomStateJson(room);
    pushToSeatedHumans(app, room, payload);
    json_object_put(payload);
    return OK;
}

void roomsUpdateIdentity(App *app, UserId userId, const char *name, const char *avatar) {
    if (!app || !app->rooms || userId == 0 || !name || !avatar) return;
    Room *room = roomsFindByUser(app->rooms, userId);
    /* matchesStart đã chép các ghế; trận đang chạy giữ bản của riêng nó. */
    if (!room || room->status != ROOM_STATUS_WAITING) return;
    int seatIndex = humanSeatOf(room, userId);
    if (seatIndex < 0) return;
    RoomSeat *seat = &room->seats[seatIndex];
    snprintf(seat->name, sizeof(seat->name), "%s", name);
    snprintf(seat->avatar, sizeof(seat->avatar), "%s", avatar);
    room->revision++;
    roomsBroadcastState(app, room);
}

void roomsMatchEnded(App *app, const char *roomId) {
    if (!app || !app->rooms || !roomId || roomId[0] == '\0') return;
    Room *room = roomsFindByCode(app->rooms, roomId);
    /* Nếu mọi người thật đã rời giữa trận, mã này có thể đã thuộc một phòng chờ mới. */
    if (!room || room->status != ROOM_STATUS_IN_MATCH) return;
    /* Xóa ô phòng là nhả mọi ghế cùng lúc; thành viên đã nhận MATCH_RESULT. */
    memset(room, 0, sizeof(*room));
}

/* --------------------------------- Party --------------------------------- */

Party *partyFindByUser(Rooms *rooms, UserId userId) {
    if (!rooms || userId == 0) return NULL;
    for (int i = 0; i < MAX_PARTIES; i++) {
        Party *party = &rooms->parties[i];
        if (!party->active) continue;
        for (size_t member = 0; member < party->memberCount; member++) {
            if (party->members[member] == userId) return party;
        }
    }
    return NULL;
}

static json_object *partyStateJson(const Party *party) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "partyId", json_object_new_int((int)party->partyId));
    json_object_object_add(payload, "leaderId", lobbyUserIdJson(party->leaderId));
    json_object_object_add(payload, "revision", json_object_new_int64(party->revision));
    json_object_object_add(payload, "mode", json_object_new_string(lobbyModeName(party->mode)));
    json_object *membersJson = json_object_new_array();
    for (size_t i = 0; i < party->memberCount; i++) {
        json_object_array_add(membersJson, lobbyUserIdJson(party->members[i]));
    }
    json_object_object_add(payload, "members", membersJson);
    return payload;
}

static void broadcastPartyState(App *app, const Party *party) {
    json_object *payload = partyStateJson(party);
    for (size_t i = 0; i < party->memberCount; i++) {
        (void)appPush(app, party->members[i], "PARTY_STATE", payload);
    }
    json_object_put(payload);
}

void roomsSyncUser(App *app, UserId userId) {
    if (!app || !app->rooms || userId == 0) return;
    Room *room = roomsFindByUser(app->rooms, userId);
    if (room) {
        json_object *roomState = roomStateJson(room);
        (void)appPush(app, userId, "ROOM_STATE", roomState);
        json_object_put(roomState);
    }
    Party *party = partyFindByUser(app->rooms, userId);
    if (party) {
        json_object *partyState = partyStateJson(party);
        (void)appPush(app, userId, "PARTY_STATE", partyState);
        json_object_put(partyState);
    }
}

/* PARTY_STATE của party vừa giải tán: không còn leader ("0") và không còn thành viên. */
static json_object *dissolvedPartyJson(const Party *party) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "partyId", json_object_new_int((int)party->partyId));
    json_object_object_add(payload, "leaderId", json_object_new_string("0"));
    json_object_object_add(payload, "revision", json_object_new_int64(party->revision + 1));
    json_object_object_add(payload, "mode", json_object_new_string(lobbyModeName(party->mode)));
    json_object_object_add(payload, "members", json_object_new_array());
    return payload;
}

Status partyLeave(Rooms *rooms, UserId userId) {
    if (!rooms || userId == 0) return OK;
    Party *party = partyFindByUser(rooms, userId);
    if (!party) return OK;
    if (rooms->app) {
        json_object *payload = dissolvedPartyJson(party);
        for (size_t i = 0; i < party->memberCount; i++) {
            (void)appPush(rooms->app, party->members[i], "PARTY_STATE", payload);
        }
        json_object_put(payload);
    }
    /* Party tối đa hai người: một người rời thì giải tán cả party. */
    memset(party, 0, sizeof(*party));
    return OK;
}

/* Party mới gồm người mời (làm leader) và người nhận lời; NULL khi hết ô. */
static Party *createParty(Rooms *rooms, UserId leaderId, UserId memberId, Mode mode) {
    for (int i = 0; i < MAX_PARTIES; i++) {
        Party *party = &rooms->parties[i];
        if (party->active) continue;
        party->active = true;
        party->partyId = rooms->nextPartyId++;
        party->leaderId = leaderId;
        party->members[0] = leaderId;
        party->members[1] = memberId;
        party->memberCount = PARTY_MAX_MEMBERS;
        party->mode = mode;
        party->revision = 1;
        return party;
    }
    return NULL;
}

/* -------------------------------- Lời mời -------------------------------- */

static Invite *findInvite(Rooms *rooms, const char *inviteId) {
    for (int i = 0; i < MAX_INVITES; i++) {
        Invite *invite = &rooms->invites[i];
        if (invite->active && strcmp(invite->inviteId, inviteId) == 0) return invite;
    }
    return NULL;
}

/* Ô cho lời mời mới: ô trống hoặc ô có lời mời đã hết hạn. */
static Invite *freeInviteSlot(Rooms *rooms, uint64_t nowMs) {
    for (int i = 0; i < MAX_INVITES; i++) {
        Invite *slot = &rooms->invites[i];
        bool expired = nowMs > slot->createdAtMs &&
                       nowMs - slot->createdAtMs > INVITE_TIMEOUT_MS;
        if (!slot->active || expired) return slot;
    }
    return NULL;
}

Status roomsInviteCreate(Rooms *rooms, UserId senderId, UserId recipientId, const char *kind,
                         const char *roomCode, char outInviteId[INVITE_ID_SIZE]) {
    if (!rooms || senderId == 0 || recipientId == 0) return BAD_REQUEST;
    uint64_t nowMs = accountsGetNowMs();
    Invite *invite = freeInviteSlot(rooms, nowMs);
    if (!invite) return SERVER_FULL;

    memset(invite, 0, sizeof(*invite));
    invite->active = true;
    generateUuid(invite->inviteId);
    invite->senderId = senderId;
    invite->recipientId = recipientId;
    if (kind) snprintf(invite->kind, sizeof(invite->kind), "%s", kind);
    if (roomCode) snprintf(invite->roomCode, sizeof(invite->roomCode), "%s", roomCode);
    invite->createdAtMs = nowMs;
    if (outInviteId) snprintf(outInviteId, INVITE_ID_SIZE, "%s", invite->inviteId);
    return OK;
}

/* ------------------------- Handler từng lệnh ROOM_* ------------------------- */

/* Payload OK chung khi user vừa có ghế trong phòng. */
static json_object *seatedReplyJson(const char *code, int seat) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "roomId", json_object_new_string(code));
    json_object_object_add(payload, "code", json_object_new_string(code));
    json_object_object_add(payload, "seat", json_object_new_int(seat));
    return payload;
}

static void handleRoomCreate(RoomRequest *request) {
    if (rejectIfInMatch(request) || rejectIfQueued(request)) return;
    Mode mode = CLASSIC;
    json_object *modeJson = payloadField(request->envelope, "mode");
    /* Chế độ sai thì giữ CLASSIC thay vì từ chối, như hành vi cũ của client. */
    if (modeJson) (void)parseMode(modeJson, &mode);

    char code[ROOM_CODE_SIZE] = {0};
    Status status = roomsCreate(request->rooms, request->userId, request->account.name,
                                request->account.avatar, mode, code);
    if (status != OK) {
        static const StatusMessage MESSAGES[] = {
            {BUSY, "User already in a room"}, {SERVER_FULL, "Server room capacity reached"}};
        replyStatusError(request, status, MESSAGES, ARRAY_LENGTH(MESSAGES),
                         "Could not create room");
        return;
    }
    json_object *payload = seatedReplyJson(code, 0);
    json_object_object_add(payload, "mode", json_object_new_string(lobbyModeName(mode)));
    replyOk(request, payload);
    broadcastRoomByCode(request->app, code);
}

/* Ghép request vào phòng @p code; dùng chung cho ROOM_JOIN và nhận lời mời phòng. */
static Status joinRoom(RoomRequest *request, const char *code, Seat *outSeat) {
    return roomsJoin(request->rooms, request->userId, request->account.name,
                     request->account.avatar, code, outSeat);
}

static const StatusMessage JOIN_MESSAGES[] = {
    {BUSY, "User already in a room"}, {NOT_FOUND, "Room not found"}, {ROOM_FULL, "Room is full"}};

static void handleRoomJoin(RoomRequest *request) {
    json_object *codeJson = payloadField(request->envelope, "code");
    if (!codeJson) {
        replyError(request, "BAD_REQUEST", "Room code is required");
        return;
    }
    if (rejectIfInMatch(request) || rejectIfQueued(request)) return;
    const char *code = json_object_get_string(codeJson);
    Seat seat = 0;
    Status status = joinRoom(request, code, &seat);
    if (status != OK) {
        replyStatusError(request, status, JOIN_MESSAGES, ARRAY_LENGTH(JOIN_MESSAGES),
                         "Could not join room");
        return;
    }
    replyOk(request, seatedReplyJson(code, seat));
    broadcastRoomByCode(request->app, code);
}

static void handleRoomLeave(RoomRequest *request) {
    Room *room = roomsFindByUser(request->rooms, request->userId);
    if (!room) {
        replyOk(request, NULL);
        return;
    }
    /* Chép mã trước khi rời: người cuối rời đi thì ô phòng bị xóa trắng. */
    char code[ROOM_CODE_SIZE];
    snprintf(code, sizeof(code), "%s", room->code);
    roomsLeave(request->rooms, request->userId);
    replyOk(request, NULL);
    broadcastRoomByCode(request->app, code);
}

/* Thông báo cho các lỗi chung của thao tác chỉ host mới được làm. */
static void replyHostOnlyError(RoomRequest *request, Status status, const char *forbidden,
                               const char *fallback) {
    const StatusMessage messages[] = {{FORBIDDEN, forbidden}, {NOT_FOUND, "Room not found"}};
    replyStatusError(request, status, messages, ARRAY_LENGTH(messages), fallback);
}

/* Phát lại ROOM_STATE cho phòng mà người gửi đang ngồi. */
static void broadcastRequesterRoom(RoomRequest *request) {
    Room *room = roomsFindByUser(request->rooms, request->userId);
    if (room) roomsBroadcastState(request->app, room);
}

static void handleRoomMode(RoomRequest *request) {
    json_object *modeJson = payloadField(request->envelope, "mode");
    if (!modeJson) {
        replyError(request, "BAD_REQUEST", "Mode is required");
        return;
    }
    Mode mode = CLASSIC;
    if (!parseMode(modeJson, &mode)) {
        replyError(request, "BAD_REQUEST", "Invalid mode");
        return;
    }
    Status status = roomsSetMode(request->rooms, request->userId, mode);
    if (status != OK) {
        replyHostOnlyError(request, status, "Only host can change room mode", "Could not set mode");
        return;
    }
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "mode", json_object_new_string(lobbyModeName(mode)));
    replyOk(request, payload);
    broadcastRequesterRoom(request);
}

/* Đọc trường "seat" và kiểm tra nằm trong 0..3; trả lời lỗi và trả false nếu không. */
static bool readSeatParam(RoomRequest *request, json_object *seatJson, int *outSeat) {
    *outSeat = json_object_get_int(seatJson);
    if (validSeat(*outSeat)) return true;
    replyError(request, "BAD_REQUEST", "Seat must be between 0 and 3");
    return false;
}

static void handleRoomBot(RoomRequest *request) {
    json_object *seatJson = payloadField(request->envelope, "seat");
    json_object *levelJson = payloadField(request->envelope, "level");
    if (!seatJson || !levelJson) {
        replyError(request, "BAD_REQUEST", "Seat and level are required");
        return;
    }
    int seat = 0;
    if (!readSeatParam(request, seatJson, &seat)) return;
    BotLevel level = EASY;
    if (!parseBotLevel(levelJson, &level)) {
        replyError(request, "BAD_REQUEST", "Invalid bot level");
        return;
    }
    Status status = roomsSetBot(request->rooms, request->userId, seat, level);
    if (status != OK) {
        replyHostOnlyError(request, status, "Only host can configure bots",
                           "Could not configure bot");
        return;
    }
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "seat", json_object_new_int(seat));
    json_object_object_add(payload, "level", json_object_new_string(BOT_LEVEL_NAMES[level]));
    replyOk(request, payload);
    broadcastRequesterRoom(request);
}

/* Người thật đang ngồi ghế @p seat trong phòng của host, hoặc 0. */
static UserId humanAtSeat(Rooms *rooms, UserId hostId, int seat) {
    Room *room = roomsFindByUser(rooms, hostId);
    if (!room || !isHumanSeat(&room->seats[seat])) return 0;
    return room->seats[seat].userId;
}

static void handleRoomKick(RoomRequest *request) {
    json_object *seatJson = payloadField(request->envelope, "seat");
    if (!seatJson) {
        replyError(request, "BAD_REQUEST", "Seat is required");
        return;
    }
    int seat = 0;
    if (!readSeatParam(request, seatJson, &seat)) return;
    /* roomsKick xóa ghế, nên phải nhớ trước ai đang ngồi đó để báo cho họ. */
    UserId kickedUser = humanAtSeat(request->rooms, request->userId, seat);
    Status status = roomsKick(request->rooms, request->userId, seat);
    if (status != OK) {
        replyHostOnlyError(request, status, "Only host can kick members", "Could not kick member");
        return;
    }
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "seat", json_object_new_int(seat));
    replyOk(request, payload);

    Room *room = roomsFindByUser(request->rooms, request->userId);
    if (!room) return;
    /* Người bị kick không còn ngồi nên broadcast bỏ qua họ; cùng snapshot đó
     * báo cho client của họ biết đã rời phòng. */
    json_object *state = roomStateJson(room);
    pushToSeatedHumans(request->app, room, state);
    if (kickedUser) (void)appPush(request->app, kickedUser, "ROOM_STATE", state);
    json_object_put(state);
}

static void handleRoomStart(RoomRequest *request) {
    Room *room = roomsFindByUser(request->rooms, request->userId);
    if (!room) {
        replyError(request, "NOT_FOUND", "Room not found");
        return;
    }
    if (!isHost(room, request->userId)) {
        replyError(request, "FORBIDDEN", "Only host can start room");
        return;
    }
    Status status = roomsStart(request->app, room->code);
    if (status == BUSY) {
        replyError(request, "BUSY", "Match engine not available");
    } else if (status != OK) {
        replyError(request, "INVALID_ACTION", "Cannot start room: all 4 seats must be filled");
    } else {
        replyOk(request, NULL);
    }
}

/* -------------------------- INVITE_SEND / REPLY -------------------------- */

/* Kiểm tra lời mời vào phòng; điền @p outRoomCode khi hợp lệ. */
static bool checkRoomInvite(RoomRequest *request, UserId target, char outRoomCode[ROOM_CODE_SIZE]) {
    Room *room = roomsFindByUser(request->rooms, request->userId);
    if (!room) {
        replyError(request, "INVALID_ACTION", "You are not in a room");
        return false;
    }
    if (lowestEmptySeat(room) < 0) {
        replyError(request, "ROOM_FULL", "Room is full");
        return false;
    }
    if (roomsFindByUser(request->rooms, target) != NULL) {
        replyError(request, "BUSY", "Target user is already in a room");
        return false;
    }
    snprintf(outRoomCode, ROOM_CODE_SIZE, "%s", room->code);
    return true;
}

static bool checkPartyInvite(RoomRequest *request, UserId target) {
    Party *party = partyFindByUser(request->rooms, request->userId);
    if (party && party->memberCount >= PARTY_MAX_MEMBERS) {
        replyError(request, "BUSY", "Party is full");
        return false;
    }
    if (partyFindByUser(request->rooms, target) != NULL) {
        replyError(request, "BUSY", "Target user is already in a party");
        return false;
    }
    return true;
}

/* Đọc user được mời và loại lời mời; trả lời lỗi và trả false nếu sai. */
static bool readInviteTarget(RoomRequest *request, UserId *outTarget, bool *outParty,
                             Mode *outPartyMode) {
    json_object *userJson = payloadField(request->envelope, "userId");
    json_object *kindJson = payloadField(request->envelope, "kind");
    if (!userJson || !kindJson) {
        replyError(request, "BAD_REQUEST", "userId and kind required");
        return false;
    }
    if (!lobbyParseUserIdJson(userJson, outTarget) || *outTarget == request->userId) {
        replyError(request, "BAD_REQUEST", "Invalid target user");
        return false;
    }
    const char *kind = json_object_get_string(kindJson);
    if (strcasecmp(kind, "room") != 0 && strcasecmp(kind, "party") != 0) {
        replyError(request, "BAD_REQUEST", "Kind must be room or party");
        return false;
    }
    *outParty = strcasecmp(kind, "party") == 0;
    if (*outParty && !parsePartyMode(request->envelope, outPartyMode)) {
        replyError(request, "BAD_REQUEST", "Party mode must be 2v2 or side");
        return false;
    }
    return true;
}

/* Push INVITE cho người được mời; người đó đã được kiểm tra là đang online. */
static void pushInvite(RoomRequest *request, const Invite *invite, bool party) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "inviteId", json_object_new_string(invite->inviteId));
    json_object_object_add(payload, "senderId", lobbyUserIdJson(request->userId));
    json_object_object_add(payload, "senderName", json_object_new_string(request->account.name));
    json_object_object_add(payload, "kind", json_object_new_string(invite->kind));
    if (invite->roomCode[0]) {
        json_object_object_add(payload, "roomId", json_object_new_string(invite->roomCode));
    }
    if (party) {
        const char *modeName = lobbyModeName(invite->mode);
        json_object_object_add(payload, "mode", json_object_new_string(modeName));
    }
    (void)appPush(request->app, invite->recipientId, "INVITE", payload);
    json_object_put(payload);
}

static void handleInviteSend(RoomRequest *request) {
    UserId target = 0;
    bool party = false;
    Mode partyMode = TWO_V_TWO;
    if (!readInviteTarget(request, &target, &party, &partyMode)) return;
    ConnId recipientConn;
    if (sessionsFindConn(request->app, target, &recipientConn) != OK) {
        replyError(request, "NOT_FOUND", "User is offline");
        return;
    }
    char roomCode[ROOM_CODE_SIZE] = {0};
    if (party ? !checkPartyInvite(request, target) : !checkRoomInvite(request, target, roomCode)) {
        return;
    }
    char inviteId[INVITE_ID_SIZE];
    const char *kind = json_object_get_string(payloadField(request->envelope, "kind"));
    Status created = roomsInviteCreate(request->rooms, request->userId, target, kind, roomCode,
                                       inviteId);
    if (created != OK) {
        replyError(request, "SERVER_FULL", "Too many active invites");
        return;
    }
    Invite *invite = findInvite(request->rooms, inviteId);
    if (!invite) {
        replyError(request, "STORAGE_ERROR", "Failed to allocate invite");
        return;
    }
    invite->mode = partyMode;
    pushInvite(request, invite, party);
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "inviteId", json_object_new_string(invite->inviteId));
    replyOk(request, payload);
}

/* Nhận lời mời vào phòng. Lỗi do phòng (mất, đầy) làm lời mời hết hiệu lực; lỗi
 * do người nhận đang ở phòng khác thì giữ lời mời để nhận lại sau. */
static void acceptRoomInvite(RoomRequest *request, Invite *invite) {
    if (roomsFindByUser(request->rooms, request->userId) != NULL) {
        replyError(request, "BUSY", "Already in a room");
        return;
    }
    Seat seat = 0;
    Status status = joinRoom(request, invite->roomCode, &seat);
    if (status == BUSY) {
        replyError(request, "BUSY", "Already in a room");
        return;
    }
    invite->active = false;
    if (status != OK) {
        replyStatusError(request, status, JOIN_MESSAGES, ARRAY_LENGTH(JOIN_MESSAGES),
                         "Could not join room");
        return;
    }
    replyOk(request, seatedReplyJson(invite->roomCode, seat));
    broadcastRoomByCode(request->app, invite->roomCode);
}

static void acceptPartyInvite(RoomRequest *request, Invite *invite) {
    if (partyFindByUser(request->rooms, request->userId) != NULL) {
        replyError(request, "BUSY", "Already in a party");
        return;
    }
    Party *party = partyFindByUser(request->rooms, invite->senderId);
    if (!party) {
        party = createParty(request->rooms, invite->senderId, request->userId, invite->mode);
    } else if (party->memberCount < PARTY_MAX_MEMBERS) {
        party->members[party->memberCount++] = request->userId;
        party->revision++;
    } else {
        invite->active = false;
        replyError(request, "BUSY", "Party is full");
        return;
    }
    invite->active = false;
    replyOk(request, NULL);
    if (party) broadcastPartyState(request->app, party);
}

/* Lời mời đang chờ gửi tới chính người gửi request, hoặc NULL (đã trả lời lỗi). */
static Invite *pendingInviteFor(RoomRequest *request, const char *inviteId) {
    Invite *invite = findInvite(request->rooms, inviteId);
    if (!invite || invite->recipientId != request->userId) {
        replyError(request, "NOT_FOUND", "Invite not found");
        return NULL;
    }
    if (accountsGetNowMs() - invite->createdAtMs > INVITE_TIMEOUT_MS) {
        invite->active = false;
        replyError(request, "NOT_FOUND", "Invite has expired");
        return NULL;
    }
    return invite;
}

static void handleInviteReply(RoomRequest *request) {
    json_object *idJson = payloadField(request->envelope, "inviteId");
    json_object *acceptJson = payloadField(request->envelope, "accept");
    if (!idJson || !acceptJson) {
        replyError(request, "BAD_REQUEST", "inviteId and accept required");
        return;
    }
    Invite *invite = pendingInviteFor(request, json_object_get_string(idJson));
    if (!invite) return;
    if (!json_object_get_boolean(acceptJson)) {
        invite->active = false;
        replyOk(request, NULL);
        return;
    }
    /* Nhận lời: lời mời vẫn chờ nếu bị từ chối tạm, để nhận lại được sau trận. */
    if (rejectIfInMatch(request) || rejectIfQueued(request)) return;
    bool partyInvite = strcasecmp(invite->kind, "party") == 0;
    /* Một mục party luôn chứa đủ hai người, nên người mời đang xếp hàng một mình
     * cũng không thể có thêm bạn cùng đội. */
    if (partyInvite && queueContains(request->app->queue, invite->senderId)) {
        replyError(request, "BUSY", "Inviter is in matchmaking");
        return;
    }
    if (strcasecmp(invite->kind, "room") == 0) acceptRoomInvite(request, invite);
    else acceptPartyInvite(request, invite);
}

static void handlePartyLeave(RoomRequest *request) {
    /* Mục party trong hàng chờ không thể sống lâu hơn party; mục solo thì giữ. */
    if (partyFindByUser(request->rooms, request->userId)) {
        queueUserGone(request->app, request->userId);
    }
    partyLeave(request->rooms, request->userId);
    replyOk(request, NULL);
}

/* ------------------------------- Điều phối ------------------------------- */

typedef void (*RoomHandler)(RoomRequest *request);

static const struct {
    const char *type;
    RoomHandler handler;
} ROOM_HANDLERS[] = {
    {"ROOM_CREATE", handleRoomCreate}, {"ROOM_JOIN", handleRoomJoin},
    {"ROOM_LEAVE", handleRoomLeave}, {"ROOM_MODE", handleRoomMode},
    {"ROOM_BOT", handleRoomBot}, {"ROOM_KICK", handleRoomKick},
    {"ROOM_START", handleRoomStart}, {"INVITE_SEND", handleInviteSend},
    {"INVITE_REPLY", handleInviteReply}, {"PARTY_LEAVE", handlePartyLeave},
};

/* Tên/avatar của người gửi; không đọc được hồ sơ thì dùng "Player<id>" và "fox". */
static void loadRequesterIdentity(App *app, UserId userId, Account *outAccount) {
    memset(outAccount, 0, sizeof(*outAccount));
    if (app->store) (void)storeAccountGetById(app->store, userId, outAccount);
    if (outAccount->name[0] == '\0') {
        lobbyFallbackName(userId, outAccount->name, sizeof(outAccount->name));
        snprintf(outAccount->avatar, sizeof(outAccount->avatar), "%s", "fox");
    }
}

Status roomsHandle(App *app, ConnId connId, const Envelope *envelope) {
    if (!app || !app->rooms || !envelope) return BAD_REQUEST;
    RoomRequest request = {app, app->rooms, connId, envelope, 0, {0}};
    if (sessionsUser(app, connId, &request.userId) != OK) {
        replyError(&request, "UNAUTHENTICATED", "Authentication required");
        return OK;
    }
    loadRequesterIdentity(app, request.userId, &request.account);
    for (size_t i = 0; i < ARRAY_LENGTH(ROOM_HANDLERS); i++) {
        if (strcmp(envelope->type, ROOM_HANDLERS[i].type) == 0) {
            ROOM_HANDLERS[i].handler(&request);
            return OK;
        }
    }
    replyError(&request, "BAD_REQUEST", "Unknown command");
    return OK;
}
