#define _POSIX_C_SOURCE 200809L
#include "queue.h"
#include "server/auth/accounts.h"
#include "server/app.h"
#include "server/game/match.h"
#include "server/lobby/lobby_common.h"
#include "server/lobby/rooms.h"
#include "server/storage/store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    QUEUE_MODE_COUNT = LOBBY_MODE_COUNT,
    TEAM_COUNT = 2,
    TEAM_SIZE = 2
};

/* Bố trí ghế theo đội từ spec: 2v2 xen kẽ ghế, side xếp hai người cạnh nhau. */
static const Seat TWO_V_TWO_TEAMS[TEAM_COUNT][TEAM_SIZE] = {{0, 2}, {1, 3}};
static const Seat SIDE_TEAMS[TEAM_COUNT][TEAM_SIZE] = {{0, 1}, {2, 3}};

typedef struct {
    QueueEntry entries[QUEUE_MAX_ENTRIES]; /* FIFO: index 0 là mục chờ lâu nhất */
    size_t count;
    uint64_t revision;  /* tăng mỗi lần đổi để client bỏ QUEUE_STATE cũ */
    bool retryPending;  /* mở trận vừa thất bại; queueTick thử lại sau retryAtMs */
    uint64_t retryAtMs;
} ModeQueue;

struct Queue {
    ModeQueue modes[QUEUE_MODE_COUNT];
};

static bool validMode(Mode mode) {
    return (unsigned)mode < QUEUE_MODE_COUNT;
}

static bool isTeamMode(Mode mode) {
    return mode == TWO_V_TWO || mode == SIDE;
}

static bool entryHas(const QueueEntry *entry, UserId user) {
    for (size_t index = 0; index < entry->count; index++) {
        if (entry->users[index] == user) return true;
    }
    return false;
}

Status queueInit(Queue **outQueue) {
    if (!outQueue) return BAD_REQUEST;
    *outQueue = calloc(1, sizeof(Queue));
    return *outQueue ? OK : STORAGE_ERROR;
}

void queueDestroy(Queue *queue) {
    free(queue);
}

bool queueContains(const Queue *queue, UserId user) {
    if (!queue || user == 0) return false;
    for (int mode = 0; mode < QUEUE_MODE_COUNT; mode++) {
        const ModeQueue *modeQueue = &queue->modes[mode];
        for (size_t index = 0; index < modeQueue->count; index++) {
            if (entryHas(&modeQueue->entries[index], user)) return true;
        }
    }
    return false;
}

static Status validateEntry(Mode mode, const QueueEntry *entry) {
    if (entry->count < 1 || entry->count > QUEUE_ENTRY_MAX_USERS) return BAD_REQUEST;
    for (size_t index = 0; index < entry->count; index++) {
        if (entry->users[index] == 0) return BAD_REQUEST;
    }
    if (entry->count == TEAM_SIZE && entry->users[0] == entry->users[1]) return BAD_REQUEST;
    /* Spec: party chỉ tồn tại để chung một đội, nên chế độ đơn từ chối party. */
    if (entry->count == TEAM_SIZE && !isTeamMode(mode)) return INVALID_ACTION;
    return OK;
}

Status queueAdd(Queue *queue, Mode mode, const QueueEntry *entry) {
    if (!queue || !entry || !validMode(mode)) return BAD_REQUEST;
    Status status = validateEntry(mode, entry);
    if (status != OK) return status;
    for (size_t index = 0; index < entry->count; index++) {
        if (queueContains(queue, entry->users[index])) return BUSY;
    }
    ModeQueue *modeQueue = &queue->modes[mode];
    if (modeQueue->count >= QUEUE_MAX_ENTRIES) return SERVER_FULL;
    QueueEntry copy = {{0, 0}, entry->count};
    memcpy(copy.users, entry->users, entry->count * sizeof copy.users[0]);
    modeQueue->entries[modeQueue->count++] = copy;
    modeQueue->revision++;
    return OK;
}

/* memmove giữ các mục còn lại đúng thứ tự đến. */
static void eraseAt(ModeQueue *modeQueue, size_t index) {
    size_t tail = modeQueue->count - index - 1;
    memmove(&modeQueue->entries[index], &modeQueue->entries[index + 1],
            tail * sizeof modeQueue->entries[0]);
    modeQueue->count--;
    modeQueue->revision++;
}

bool queueRemove(Queue *queue, UserId user, Mode *outMode, QueueEntry *removed) {
    if (!queue || user == 0) return false;
    for (int mode = 0; mode < QUEUE_MODE_COUNT; mode++) {
        ModeQueue *modeQueue = &queue->modes[mode];
        for (size_t index = 0; index < modeQueue->count; index++) {
            if (!entryHas(&modeQueue->entries[index], user)) continue;
            if (outMode) *outMode = (Mode)mode;
            if (removed) *removed = modeQueue->entries[index];
            eraseAt(modeQueue, index);
            return true;
        }
    }
    return false;
}

static void placeSeats(const ModeQueue *modeQueue, Mode mode, const size_t picked[],
                        size_t pickedCount, UserId seats[QUEUE_MATCH_SEATS]) {
    bool taken[QUEUE_MATCH_SEATS] = {false};
    bool teamMode = isTeamMode(mode);
    const Seat (*teams)[TEAM_SIZE] = mode == SIDE ? SIDE_TEAMS : TWO_V_TWO_TEAMS;
    size_t nextTeam = 0;
    memset(seats, 0, QUEUE_MATCH_SEATS * sizeof seats[0]);
    /* Xếp party trước để mỗi party chiếm trọn một đội, rồi người chơi đơn lấp chỗ trống. */
    for (size_t pick = 0; teamMode && pick < pickedCount; pick++) {
        const QueueEntry *entry = &modeQueue->entries[picked[pick]];
        if (entry->count != TEAM_SIZE || nextTeam >= TEAM_COUNT) continue;
        for (size_t member = 0; member < TEAM_SIZE; member++) {
            Seat seat = teams[nextTeam][member];
            seats[seat] = entry->users[member];
            taken[seat] = true;
        }
        nextTeam++;
    }
    size_t nextSeat = 0;
    for (size_t pick = 0; pick < pickedCount; pick++) {
        const QueueEntry *entry = &modeQueue->entries[picked[pick]];
        if (teamMode && entry->count == TEAM_SIZE) continue;
        for (size_t member = 0; member < entry->count; member++) {
            while (nextSeat < QUEUE_MATCH_SEATS && taken[nextSeat]) nextSeat++;
            if (nextSeat >= QUEUE_MATCH_SEATS) return;
            seats[nextSeat] = entry->users[member];
            taken[nextSeat] = true;
        }
    }
}

/* Tìm theo chiều sâu đúng thứ tự index, nên tập đầu tiên đủ đúng bốn người là tập
 * sớm nhất theo FIFO. Quét tham lam sẽ kẹt với [party, solo, party]: party + solo
 * không còn chỗ cho party thứ hai, dù hai party có thể đấu với nhau (spec §5). Độ
 * sâu tối đa bốn mục nên phép tìm vẫn nhỏ kể cả khi hàng chờ đầy. */
static bool findEarliestSet(const ModeQueue *modeQueue, size_t start, size_t players,
                              size_t picked[QUEUE_MATCH_SEATS], size_t *pickedCount) {
    if (players == QUEUE_MATCH_SEATS) return true;
    for (size_t index = start; index < modeQueue->count; index++) {
        size_t entrySize = modeQueue->entries[index].count;
        if (players + entrySize > QUEUE_MATCH_SEATS) continue;
        picked[(*pickedCount)++] = index;
        if (findEarliestSet(modeQueue, index + 1, players + entrySize, picked, pickedCount)) {
            return true;
        }
        (*pickedCount)--;
    }
    return false;
}

bool queuePick(const Queue *queue, Mode mode, UserId outSeats[QUEUE_MATCH_SEATS],
                size_t outIndexes[QUEUE_MATCH_SEATS], size_t *outEntryCount) {
    if (!queue || !validMode(mode) || !outSeats || !outIndexes || !outEntryCount) return false;
    const ModeQueue *modeQueue = &queue->modes[mode];
    size_t pickedCount = 0;
    *outEntryCount = 0;
    if (!findEarliestSet(modeQueue, 0, 0, outIndexes, &pickedCount)) return false;
    *outEntryCount = pickedCount;
    placeSeats(modeQueue, mode, outIndexes, pickedCount, outSeats);
    return true;
}

void queueTake(Queue *queue, Mode mode, const size_t indexes[], size_t count) {
    if (!queue || !validMode(mode) || !indexes) return;
    ModeQueue *modeQueue = &queue->modes[mode];
    bool drop[QUEUE_MAX_ENTRIES] = {false};
    for (size_t index = 0; index < count; index++) {
        if (indexes[index] < modeQueue->count) drop[indexes[index]] = true;
    }
    size_t kept = 0;
    for (size_t index = 0; index < modeQueue->count; index++) {
        if (!drop[index]) modeQueue->entries[kept++] = modeQueue->entries[index];
    }
    if (kept != modeQueue->count) modeQueue->revision++;
    modeQueue->count = kept;
}

/* ---------------------------------------------------------------------------
 * Phần nối với server: mọi hàm dưới đây chạy trên vòng poll và nói chuyện với client.
 * ------------------------------------------------------------------------- */

static json_object *playerJson(App *app, UserId user) {
    Account account;
    memset(&account, 0, sizeof account);
    json_object *player = json_object_new_object();
    json_object_object_add(player, "userId", lobbyUserIdJson(user));
    /* Tra hồ sơ lỗi vẫn liệt kê người chơi; tên thật xuất hiện ở snapshot kế tiếp. */
    if (!app->store || storeAccountGetById(app->store, user, &account) != OK) {
        lobbyFallbackName(user, account.name, sizeof account.name);
    }
    json_object_object_add(player, "name", json_object_new_string(account.name));
    json_object_object_add(player, "avatar", json_object_new_string(account.avatar));
    return player;
}

static json_object *activeStateJson(App *app, Mode mode) {
    const ModeQueue *modeQueue = &app->queue->modes[mode];
    json_object *payload = json_object_new_object();
    json_object *players = json_object_new_array();
    json_object_object_add(payload, "mode", json_object_new_string(lobbyModeName(mode)));
    json_object_object_add(payload, "active", json_object_new_boolean(true));
    for (size_t index = 0; index < modeQueue->count; index++) {
        const QueueEntry *entry = &modeQueue->entries[index];
        for (size_t member = 0; member < entry->count; member++) {
            json_object_array_add(players, playerJson(app, entry->users[member]));
        }
    }
    json_object_object_add(payload, "players", players);
    json_object_object_add(payload, "revision", json_object_new_uint64(modeQueue->revision));
    return payload;
}

/* Người rời hàng chờ (hoặc vừa được ghép trận, khi đó có matchId) nhận active:false. */
static void notifyInactive(App *app, Mode mode, const UserId users[], size_t count,
                            const char *matchId) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "mode", json_object_new_string(lobbyModeName(mode)));
    json_object_object_add(payload, "active", json_object_new_boolean(false));
    if (matchId) json_object_object_add(payload, "matchId", json_object_new_string(matchId));
    json_object_object_add(payload, "revision",
                           json_object_new_uint64(app->queue->modes[mode].revision));
    for (size_t index = 0; index < count; index++) {
        /* Người offline lỡ bản push này; dù sao họ cũng sẽ bị loại khỏi hàng chờ. */
        (void)appPush(app, users[index], "QUEUE_STATE", payload);
    }
    json_object_put(payload);
}

static void pushToMode(App *app, Mode mode, json_object *payload) {
    const ModeQueue *modeQueue = &app->queue->modes[mode];
    for (size_t index = 0; index < modeQueue->count; index++) {
        const QueueEntry *entry = &modeQueue->entries[index];
        for (size_t member = 0; member < entry->count; member++) {
            (void)appPush(app, entry->users[member], "QUEUE_STATE", payload);
        }
    }
}

static void broadcastMode(App *app, Mode mode) {
    if (app->queue->modes[mode].count == 0) return;
    json_object *payload = activeStateJson(app, mode);
    pushToMode(app, mode, payload);
    json_object_put(payload);
}

static bool isOnline(App *app, UserId user) {
    ConnId conn;
    return sessionsFindConn(app, user, &conn) == OK;
}

static UserId firstOfflineSeat(App *app, const UserId seats[QUEUE_MATCH_SEATS]) {
    for (size_t seat = 0; seat < QUEUE_MATCH_SEATS; seat++) {
        if (!isOnline(app, seats[seat])) return seats[seat];
    }
    return 0;
}

/* Mở nhiều trận ranked nhất mà chế độ này đủ người; nếu thất bại thì giữ mọi người
 * trong hàng chờ và để queueTick thử lại, vì nguyên nhân (Store bận, hết ô trận)
 * thường chỉ là tạm thời. */
static void tryStart(App *app, Mode mode, uint64_t nowMs) {
    ModeQueue *modeQueue = &app->queue->modes[mode];
    UserId seats[QUEUE_MATCH_SEATS];
    size_t picked[QUEUE_MATCH_SEATS];
    size_t pickedCount = 0;
    bool started = false;
    modeQueue->retryPending = false;
    while (queuePick(app->queue, mode, seats, picked, &pickedCount)) {
        /* Một LOGOUT trước đó trong cùng vòng poll chưa được dọn; bỏ mục đó (báo cho
         * các thành viên) rồi chọn lại. */
        UserId offline = firstOfflineSeat(app, seats);
        if (offline) {
            queueUserGone(app, offline);
            continue;
        }
        MatchId matchId = {0};
        if (roomsMakeMatch(app, mode, seats, &matchId) != OK) {
            modeQueue->retryPending = true;
            modeQueue->retryAtMs = nowMs + QUEUE_RETRY_MS;
            break;
        }
        queueTake(app->queue, mode, picked, pickedCount);
        notifyInactive(app, mode, seats, QUEUE_MATCH_SEATS, matchId);
        started = true;
    }
    if (started) broadcastMode(app, mode);
}

void queueUserGone(App *app, UserId user) {
    if (!app || !app->queue) return;
    Mode mode = CLASSIC;
    QueueEntry removed;
    if (!queueRemove(app->queue, user, &mode, &removed)) return;
    /* Spec: một thành viên rời là bỏ cả mục party, nên cả hai đều được báo. */
    notifyInactive(app, mode, removed.users, removed.count, NULL);
    broadcastMode(app, mode);
}

void queueSyncUser(App *app, UserId user) {
    if (!app || !app->queue || user == 0) return;
    for (int mode = 0; mode < QUEUE_MODE_COUNT; mode++) {
        const ModeQueue *modeQueue = &app->queue->modes[mode];
        for (size_t index = 0; index < modeQueue->count; index++) {
            if (!entryHas(&modeQueue->entries[index], user)) continue;
            /* Chỉ socket mới cần bản này; các người khác đã có đúng revision này. */
            json_object *state = activeStateJson(app, (Mode)mode);
            (void)appPush(app, user, "QUEUE_STATE", state);
            json_object_put(state);
            return;
        }
    }
}

/* LOGOUT giữ socket nhưng bỏ phiên; người như vậy không thể chơi. */
static UserId firstOffline(App *app, const QueueEntry *entry) {
    for (size_t member = 0; member < entry->count; member++) {
        if (!isOnline(app, entry->users[member])) return entry->users[member];
    }
    return 0;
}

void queueTick(App *app, uint64_t nowMs) {
    if (!app || !app->queue) return;
    for (int mode = 0; mode < QUEUE_MODE_COUNT; mode++) {
        ModeQueue *modeQueue = &app->queue->modes[mode];
        size_t index = 0;
        while (index < modeQueue->count) {
            UserId offline = firstOffline(app, &modeQueue->entries[index]);
            /* Xóa mục sẽ dời mục kế tiếp vào đúng index này, nên không tăng index. */
            if (offline) queueUserGone(app, offline);
            else index++;
        }
        if (modeQueue->retryPending && nowMs >= modeQueue->retryAtMs) {
            tryStart(app, (Mode)mode, nowMs);
        }
    }
}

static bool parseQueueMode(json_object *payload, Mode *outMode) {
    json_object *modeObj = NULL;
    if (!payload || !json_object_object_get_ex(payload, "mode", &modeObj) ||
        !json_object_is_type(modeObj, json_type_string)) {
        return false;
    }
    return lobbyParseModeName(json_object_get_string(modeObj), outMode);
}

/* Spec: không ai được cùng lúc ngồi phòng, ở trong trận và chờ ghép trận. */
static bool isFree(App *app, UserId user) {
    return isOnline(app, user) &&
           roomsFindByUser(app->rooms, user) == NULL &&
           !matchesContains(app->matches, user) &&
           !queueContains(app->queue, user);
}

/* Party lấy từ phiên chứ không từ client, nên thành viên không thể xếp hàng giùm
 * người không ở trong party của mình. */
static Status buildEntry(App *app, UserId user, Mode mode, QueueEntry *entry) {
    memset(entry, 0, sizeof *entry);
    Party *party = partyFindByUser(app->rooms, user);
    if (party) {
        if (party->mode != mode) return INVALID_ACTION;
        entry->count = party->memberCount;
        memcpy(entry->users, party->members, entry->count * sizeof entry->users[0]);
    } else {
        entry->users[0] = user;
        entry->count = 1;
    }
    for (size_t member = 0; member < entry->count; member++) {
        if (!isFree(app, entry->users[member])) return BUSY;
    }
    return OK;
}

static void handleJoin(App *app, ConnId conn, const Envelope *envelope, UserId user) {
    Mode mode = CLASSIC;
    if (!parseQueueMode(envelope->payload, &mode)) {
        appReplyStatus(app, conn, envelope->id, BAD_REQUEST);
        return;
    }
    QueueEntry entry;
    Status status = buildEntry(app, user, mode, &entry);
    if (status == OK) status = queueAdd(app->queue, mode, &entry);
    if (status != OK) {
        appReplyStatus(app, conn, envelope->id, status);
        return;
    }
    /* Trả lời trước khi trận nào bắt đầu, để client thấy active:true rồi mới tới
     * active:false có revision cao hơn kèm matchId. */
    json_object *state = activeStateJson(app, mode);
    json_object *reply = json_object_new_object();
    json_object_object_add(reply, "queue", json_object_get(state));
    appReplyOk(app, conn, envelope->id, reply);
    json_object_put(reply);
    pushToMode(app, mode, state);
    json_object_put(state);
    tryStart(app, mode, appNowMs());
}

Status queueHandle(App *app, ConnId conn, const Envelope *envelope) {
    if (!app || !envelope || !envelope->type) return BAD_REQUEST;
    UserId user = 0;
    if (sessionsUser(app, conn, &user) != OK || user == 0) {
        appReplyStatus(app, conn, envelope->id, UNAUTHENTICATED);
        return OK;
    }
    if (!app->queue || !app->rooms) {
        appReplyStatus(app, conn, envelope->id, BUSY);
        return OK;
    }
    if (strcmp(envelope->type, "QUEUE_JOIN") == 0) {
        handleJoin(app, conn, envelope, user);
        return OK;
    }
    if (strcmp(envelope->type, "QUEUE_LEAVE") == 0) {
        /* Rời khi không ở hàng chờ thì không làm gì, giống ROOM_LEAVE. */
        queueUserGone(app, user);
        appReplyStatus(app, conn, envelope->id, OK);
        return OK;
    }
    appReplyStatus(app, conn, envelope->id, BAD_REQUEST);
    return OK;
}
