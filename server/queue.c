#define _POSIX_C_SOURCE 200809L
#include "queue.h"
#include "accounts.h"
#include "app.h"
#include "server/game/match.h"
#include "rooms.h"
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    QUEUE_MODE_COUNT = DRAFT + 1,
    TEAM_COUNT = 2,
    TEAM_SIZE = 2,
    ID_TEXT_SIZE = 32 /* fits the decimal form of any 64-bit id */
};

static const char *const MODE_NAMES[QUEUE_MODE_COUNT] = {"classic", "2v2", "side", "draft"};

/* Team seat layouts from the spec: 2v2 alternates seats, side keeps neighbours together. */
static const Seat TWO_V_TWO_TEAMS[TEAM_COUNT][TEAM_SIZE] = {{0, 2}, {1, 3}};
static const Seat SIDE_TEAMS[TEAM_COUNT][TEAM_SIZE] = {{0, 1}, {2, 3}};

typedef struct {
    QueueEntry entries[QUEUE_MAX_ENTRIES]; /* FIFO: index 0 waited longest */
    size_t count;
    uint64_t revision;  /* bumped on every change so clients drop stale QUEUE_STATE */
    bool retryPending;  /* a match start failed; queue_tick retries after retryAtMs */
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

Status queue_init(Queue **outQueue) {
    if (!outQueue) return BAD_REQUEST;
    *outQueue = calloc(1, sizeof(Queue));
    return *outQueue ? OK : STORAGE_ERROR;
}

void queue_destroy(Queue *queue) {
    free(queue);
}

bool queue_contains(const Queue *queue, UserId user) {
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
    /* Spec: parties exist only to stay on one team, so solo modes reject them. */
    if (entry->count == TEAM_SIZE && !isTeamMode(mode)) return INVALID_ACTION;
    return OK;
}

Status queue_add(Queue *queue, Mode mode, const QueueEntry *entry) {
    if (!queue || !entry || !validMode(mode)) return BAD_REQUEST;
    Status status = validateEntry(mode, entry);
    if (status != OK) return status;
    for (size_t index = 0; index < entry->count; index++) {
        if (queue_contains(queue, entry->users[index])) return BUSY;
    }
    ModeQueue *modeQueue = &queue->modes[mode];
    if (modeQueue->count >= QUEUE_MAX_ENTRIES) return SERVER_FULL;
    QueueEntry copy = {{0, 0}, entry->count};
    memcpy(copy.users, entry->users, entry->count * sizeof copy.users[0]);
    modeQueue->entries[modeQueue->count++] = copy;
    modeQueue->revision++;
    return OK;
}

/* memmove keeps the remaining entries in arrival order. */
static void eraseAt(ModeQueue *modeQueue, size_t index) {
    size_t tail = modeQueue->count - index - 1;
    memmove(&modeQueue->entries[index], &modeQueue->entries[index + 1],
            tail * sizeof modeQueue->entries[0]);
    modeQueue->count--;
    modeQueue->revision++;
}

bool queue_remove(Queue *queue, UserId user, Mode *outMode, QueueEntry *removed) {
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

static void place_seats(const ModeQueue *modeQueue, Mode mode, const size_t picked[],
                        size_t pickedCount, UserId seats[QUEUE_MATCH_SEATS]) {
    bool taken[QUEUE_MATCH_SEATS] = {false};
    bool teamMode = isTeamMode(mode);
    const Seat (*teams)[TEAM_SIZE] = mode == SIDE ? SIDE_TEAMS : TWO_V_TWO_TEAMS;
    size_t nextTeam = 0;
    memset(seats, 0, QUEUE_MATCH_SEATS * sizeof seats[0]);
    /* Parties first so each one lands on a whole team before solos fill gaps. */
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

/* Depth-first search in index order, so the first set that reaches exactly
 * four players is the earliest one by FIFO index. A plain greedy scan would
 * stall on [party, solo, party]: party + solo leaves no room for the second
 * party, although the two parties can play each other (spec §5). Depth is at
 * most four entries, so the search stays small even with a full queue. */
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

bool queue_pick(const Queue *queue, Mode mode, UserId outSeats[QUEUE_MATCH_SEATS],
                size_t outIndexes[QUEUE_MATCH_SEATS], size_t *outEntryCount) {
    if (!queue || !validMode(mode) || !outSeats || !outIndexes || !outEntryCount) return false;
    const ModeQueue *modeQueue = &queue->modes[mode];
    size_t pickedCount = 0;
    *outEntryCount = 0;
    if (!findEarliestSet(modeQueue, 0, 0, outIndexes, &pickedCount)) return false;
    *outEntryCount = pickedCount;
    place_seats(modeQueue, mode, outIndexes, pickedCount, outSeats);
    return true;
}

void queue_take(Queue *queue, Mode mode, const size_t indexes[], size_t count) {
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
 * Server glue: everything below runs on the poll loop and talks to clients.
 * ------------------------------------------------------------------------- */

/* Wire ids are decimal strings so 64-bit ids survive JSON number precision. */
static json_object *userIdJson(UserId user) {
    char idText[ID_TEXT_SIZE];
    snprintf(idText, sizeof idText, "%llu", (unsigned long long)user);
    return json_object_new_string(idText);
}

static json_object *playerJson(App *app, UserId user) {
    Account account;
    memset(&account, 0, sizeof account);
    json_object *player = json_object_new_object();
    json_object_object_add(player, "userId", userIdJson(user));
    /* A failed lookup still lists the seat; the name shows up on the next snapshot. */
    if (!app->store || store_account_get_by_id(app->store, user, &account) != OK) {
        snprintf(account.name, sizeof account.name, "Player%llu", (unsigned long long)user);
    }
    json_object_object_add(player, "name", json_object_new_string(account.name));
    json_object_object_add(player, "avatar", json_object_new_string(account.avatar));
    return player;
}

static json_object *activeStateJson(App *app, Mode mode) {
    const ModeQueue *modeQueue = &app->queue->modes[mode];
    json_object *payload = json_object_new_object();
    json_object *players = json_object_new_array();
    json_object_object_add(payload, "mode", json_object_new_string(MODE_NAMES[mode]));
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

/* Users who left the queue (or were matched, then matchId is set) get active:false. */
static void notifyInactive(App *app, Mode mode, const UserId users[], size_t count,
                            const char *matchId) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "mode", json_object_new_string(MODE_NAMES[mode]));
    json_object_object_add(payload, "active", json_object_new_boolean(false));
    if (matchId) json_object_object_add(payload, "matchId", json_object_new_string(matchId));
    json_object_object_add(payload, "revision",
                           json_object_new_uint64(app->queue->modes[mode].revision));
    for (size_t index = 0; index < count; index++) {
        /* Offline users simply miss it; they are pruned from the queue anyway. */
        (void)app_push(app, users[index], "QUEUE_STATE", payload);
    }
    json_object_put(payload);
}

static void pushToMode(App *app, Mode mode, json_object *payload) {
    const ModeQueue *modeQueue = &app->queue->modes[mode];
    for (size_t index = 0; index < modeQueue->count; index++) {
        const QueueEntry *entry = &modeQueue->entries[index];
        for (size_t member = 0; member < entry->count; member++) {
            (void)app_push(app, entry->users[member], "QUEUE_STATE", payload);
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
    return sessions_find_conn(app, user, &conn) == OK;
}

static UserId firstOfflineSeat(App *app, const UserId seats[QUEUE_MATCH_SEATS]) {
    for (size_t seat = 0; seat < QUEUE_MATCH_SEATS; seat++) {
        if (!isOnline(app, seats[seat])) return seats[seat];
    }
    return 0;
}

/* Start as many ranked matches as the mode can fill; on failure keep everyone
 * queued and let queue_tick retry, since the cause (storage busy, all match
 * slots taken) is usually transient. */
static void tryStart(App *app, Mode mode, uint64_t nowMs) {
    ModeQueue *modeQueue = &app->queue->modes[mode];
    UserId seats[QUEUE_MATCH_SEATS];
    size_t picked[QUEUE_MATCH_SEATS];
    size_t pickedCount = 0;
    bool started = false;
    modeQueue->retryPending = false;
    while (queue_pick(app->queue, mode, seats, picked, &pickedCount)) {
        /* A LOGOUT earlier in this poll iteration has not been pruned yet;
         * drop that entry (telling its members) and pick again. */
        UserId offline = firstOfflineSeat(app, seats);
        if (offline) {
            queue_user_gone(app, offline);
            continue;
        }
        MatchId matchId = {0};
        if (rooms_make_match(app, mode, seats, &matchId) != OK) {
            modeQueue->retryPending = true;
            modeQueue->retryAtMs = nowMs + QUEUE_RETRY_MS;
            break;
        }
        queue_take(app->queue, mode, picked, pickedCount);
        notifyInactive(app, mode, seats, QUEUE_MATCH_SEATS, matchId);
        started = true;
    }
    if (started) broadcastMode(app, mode);
}

void queue_user_gone(App *app, UserId user) {
    if (!app || !app->queue) return;
    Mode mode = CLASSIC;
    QueueEntry removed;
    if (!queue_remove(app->queue, user, &mode, &removed)) return;
    /* Spec: one member leaving drops the whole party entry, so both are told. */
    notifyInactive(app, mode, removed.users, removed.count, NULL);
    broadcastMode(app, mode);
}

void queue_sync_user(App *app, UserId user) {
    if (!app || !app->queue || user == 0) return;
    for (int mode = 0; mode < QUEUE_MODE_COUNT; mode++) {
        const ModeQueue *modeQueue = &app->queue->modes[mode];
        for (size_t index = 0; index < modeQueue->count; index++) {
            if (!entryHas(&modeQueue->entries[index], user)) continue;
            /* Only the new socket needs it; the others already hold this revision. */
            json_object *state = activeStateJson(app, (Mode)mode);
            (void)app_push(app, user, "QUEUE_STATE", state);
            json_object_put(state);
            return;
        }
    }
}

/* LOGOUT keeps the socket but drops the session; such users cannot play. */
static UserId firstOffline(App *app, const QueueEntry *entry) {
    for (size_t member = 0; member < entry->count; member++) {
        if (!isOnline(app, entry->users[member])) return entry->users[member];
    }
    return 0;
}

void queue_tick(App *app, uint64_t nowMs) {
    if (!app || !app->queue) return;
    for (int mode = 0; mode < QUEUE_MODE_COUNT; mode++) {
        ModeQueue *modeQueue = &app->queue->modes[mode];
        size_t index = 0;
        while (index < modeQueue->count) {
            UserId offline = firstOffline(app, &modeQueue->entries[index]);
            /* Removal shifts the next entry into this index, so do not advance. */
            if (offline) queue_user_gone(app, offline);
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
    const char *modeName = json_object_get_string(modeObj);
    for (int mode = 0; mode < QUEUE_MODE_COUNT; mode++) {
        if (strcmp(modeName, MODE_NAMES[mode]) == 0) {
            *outMode = (Mode)mode;
            return true;
        }
    }
    return false;
}

/* Spec: no user may sit in a room, a match and the queue at the same time. */
static bool isFree(App *app, UserId user) {
    return isOnline(app, user) &&
           rooms_find_by_user(app->rooms, user) == NULL &&
           !matchesContains(app->matches, user) &&
           !queue_contains(app->queue, user);
}

/* The party comes from the session, never from the client, so a member cannot
 * queue someone who is not in their party. */
static Status buildEntry(App *app, UserId user, Mode mode, QueueEntry *entry) {
    memset(entry, 0, sizeof *entry);
    Party *party = party_find_by_user(app->rooms, user);
    if (party) {
        if (party->mode != mode) return INVALID_ACTION;
        entry->count = party->member_count;
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
        app_reply_status(app, conn, envelope->id, BAD_REQUEST);
        return;
    }
    QueueEntry entry;
    Status status = buildEntry(app, user, mode, &entry);
    if (status == OK) status = queue_add(app->queue, mode, &entry);
    if (status != OK) {
        app_reply_status(app, conn, envelope->id, status);
        return;
    }
    /* Reply before any match starts so the client sees active:true, then the
     * higher-revision active:false that carries the matchId. */
    json_object *state = activeStateJson(app, mode);
    json_object *reply = json_object_new_object();
    json_object_object_add(reply, "queue", json_object_get(state));
    app_reply_ok(app, conn, envelope->id, reply);
    json_object_put(reply);
    pushToMode(app, mode, state);
    json_object_put(state);
    tryStart(app, mode, app_now_ms());
}

Status queue_handle(App *app, ConnId conn, const Envelope *envelope) {
    if (!app || !envelope || !envelope->type) return BAD_REQUEST;
    UserId user = 0;
    if (sessions_user(app, conn, &user) != OK || user == 0) {
        app_reply_status(app, conn, envelope->id, UNAUTHENTICATED);
        return OK;
    }
    if (!app->queue || !app->rooms) {
        app_reply_status(app, conn, envelope->id, BUSY);
        return OK;
    }
    if (strcmp(envelope->type, "QUEUE_JOIN") == 0) {
        handleJoin(app, conn, envelope, user);
        return OK;
    }
    if (strcmp(envelope->type, "QUEUE_LEAVE") == 0) {
        /* Leaving when not queued is a no-op, like ROOM_LEAVE. */
        queue_user_gone(app, user);
        app_reply_status(app, conn, envelope->id, OK);
        return OK;
    }
    app_reply_status(app, conn, envelope->id, BAD_REQUEST);
    return OK;
}
