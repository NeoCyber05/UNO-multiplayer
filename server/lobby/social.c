#define _POSIX_C_SOURCE 200809L
#include "social.h"
#include "server/auth/accounts.h"
#include "server/game/match.h"
#include "server/lobby/queue.h"
#include "server/game/rating.h"
#include "server/lobby/rooms.h"
#include "server/storage/store.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    ID_TEXT_SIZE = 32, /* fits the decimal form of any 64-bit id */
    TAG_TEXT_SIZE = 32, /* buffer size accountsFormatTag writes */
    MS_PER_SECOND = 1000,
    DECIMAL_BASE = 10
};

static const char TAG_PREFIX[] = "UNO#";

/* Same order as Mode and Profile.modes. */
static const char *const MODE_KEYS[STORE_MODE_COUNT] = {"classic", "2v2", "side", "draft"};

static const char *const PRESENCE_KEYS[] = {"offline", "idle", "online", "in_match"};

/* Spec §6 badge names, one per BadgeBit. */
static const struct { BadgeBit bit; const char *key; } BADGE_KEYS[] = {
    {BADGE_FIRST_WIN, "first-win"}, {BADGE_WIN_100, "win-100"},
    {BADGE_STREAK_10, "streak-10"}, {BADGE_PLUS4, "plus4"},
    {BADGE_UNO_MASTER, "uno-master"}, {BADGE_TEAM, "team"},
    {BADGE_MATCHES, "matches-500"}, {BADGE_DIAMOND, "diamond"}
};

/* ---------------------------------------------------------------------------
 * Parsing
 * ------------------------------------------------------------------------- */

/* Canonical decimal only: the wire never carries signs, spaces or padding, and
 * anything above INT64_MAX cannot be an SQLite rowid. */
static bool parseIdText(const char *text, UserId *outUserId) {
    if (!text || text[0] < '1' || text[0] > '9') return false;
    uint64_t value = 0;
    for (const char *cursor = text; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') return false;
        uint64_t digit = (uint64_t)(*cursor - '0');
        if (value > ((uint64_t)INT64_MAX - digit) / DECIMAL_BASE) return false;
        value = value * DECIMAL_BASE + digit;
    }
    *outUserId = (UserId)value;
    return true;
}

bool socialParseTag(const char *tag, UserId *outUserId) {
    if (!tag || !outUserId) return false;
    size_t prefixLength = sizeof TAG_PREFIX - 1;
    if (strncmp(tag, TAG_PREFIX, prefixLength) != 0) return false;
    return parseIdText(tag + prefixLength, outUserId);
}

/* User ids travel as decimal strings; a plain positive JSON integer is also
 * accepted, like INVITE_SEND does. */
static bool parseUserIdValue(json_object *value, UserId *outUserId) {
    if (json_object_is_type(value, json_type_string)) {
        return parseIdText(json_object_get_string(value), outUserId);
    }
    if (json_object_is_type(value, json_type_int)) {
        int64_t number = json_object_get_int64(value);
        if (number <= 0) return false;
        *outUserId = (UserId)number;
        return true;
    }
    return false;
}

static const char *stringField(const Envelope *envelope, const char *key) {
    json_object *value = NULL;
    if (!envelope->payload || !json_object_object_get_ex(envelope->payload, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    return json_object_get_string(value);
}

/* ---------------------------------------------------------------------------
 * Presence
 * ------------------------------------------------------------------------- */

Presence socialPresenceOf(App *app, UserId user) {
    if (!app || user == 0) return PRESENCE_OFFLINE;
    ConnId conn;
    if (sessionsFindConn(app, user, &conn) != OK) return PRESENCE_OFFLINE;
    if (matchesContains(app->matches, user)) return PRESENCE_IN_MATCH;
    if (roomsFindByUser(app->rooms, user) || partyFindByUser(app->rooms, user) ||
        queueContains(app->queue, user)) {
        return PRESENCE_ONLINE;
    }
    return PRESENCE_IDLE;
}

const char *socialPresenceKey(Presence presence) {
    size_t keyCount = sizeof PRESENCE_KEYS / sizeof PRESENCE_KEYS[0];
    if ((size_t)presence >= keyCount) return PRESENCE_KEYS[PRESENCE_OFFLINE];
    return PRESENCE_KEYS[presence];
}

/* ---------------------------------------------------------------------------
 * JSON builders
 * ------------------------------------------------------------------------- */

/* Wire ids are decimal strings so 64-bit ids survive JSON number precision. */
static json_object *userIdJson(UserId user) {
    char idText[ID_TEXT_SIZE];
    snprintf(idText, sizeof idText, "%llu", (unsigned long long)user);
    return json_object_new_string(idText);
}

static json_object *tagJson(UserId user) {
    char tag[TAG_TEXT_SIZE];
    accountsFormatTag(user, tag);
    return json_object_new_string(tag);
}

/* Public identity shared by friend rows, leaderboard entries and profiles;
 * nothing private (email, hash, token) ever goes through here. */
static void addIdentity(json_object *object, UserId user, const char *name,
                         const char *avatar, int elo) {
    json_object_object_add(object, "userId", userIdJson(user));
    json_object_object_add(object, "tag", tagJson(user));
    json_object_object_add(object, "name", json_object_new_string(name));
    json_object_object_add(object, "avatar", json_object_new_string(avatar));
    json_object_object_add(object, "elo", json_object_new_int(elo));
}

static json_object *friendRowJson(App *app, const FriendRow *row) {
    json_object *object = json_object_new_object();
    bool pending = row->link != FRIEND_ACCEPTED;
    bool incoming = row->link == FRIEND_PENDING_IN;
    /* Spec: presence is shown to friends only; a stranger who merely sent or
     * received a request cannot watch the other account come and go. */
    Presence presence = pending ? PRESENCE_OFFLINE : socialPresenceOf(app, row->userId);
    addIdentity(object, row->userId, row->name, row->avatar, row->elo);
    json_object_object_add(object, "presence",
                           json_object_new_string(socialPresenceKey(presence)));
    json_object_object_add(object, "online",
                           json_object_new_boolean(presence != PRESENCE_OFFLINE));
    json_object_object_add(object, "pending", json_object_new_boolean(pending));
    json_object_object_add(object, "incoming", json_object_new_boolean(incoming));
    /* FRIEND_REPLY names the request by its sender, so only the receiver needs it. */
    if (incoming) json_object_object_add(object, "requestId", userIdJson(row->userId));
    return object;
}

static json_object *friendsArrayJson(App *app, const FriendRow rows[], size_t count) {
    json_object *friends = json_object_new_array();
    for (size_t index = 0; index < count; index++) {
        json_object_array_add(friends, friendRowJson(app, &rows[index]));
    }
    return friends;
}

/* FRIEND_LIST OK and FRIEND_STATE share this {friends:[...]} shape. */
static Status friendStateJson(App *app, UserId user, json_object **outJson) {
    FriendRow rows[STORE_FRIEND_MAX];
    size_t count = 0;
    Status status = storeFriendList(app->store, user, rows, &count);
    if (status != OK) return status;
    *outJson = json_object_new_object();
    json_object_object_add(*outJson, "friends", friendsArrayJson(app, rows, count));
    return OK;
}

static json_object *leaderboardEntryJson(const LeaderboardEntry *entry) {
    json_object *object = json_object_new_object();
    addIdentity(object, entry->userId, entry->name, entry->avatar, entry->elo);
    json_object_object_add(object, "rank",
                           json_object_new_string(ratingRankKey(ratingRank(entry->elo))));
    return object;
}

static Status leaderboardJson(App *app, json_object **outJson) {
    LeaderboardEntry entries[STORE_LEADERBOARD_MAX];
    size_t count = 0;
    Status status = storeLeaderboard(app->store, entries, &count);
    if (status != OK) return status;
    *outJson = json_object_new_array();
    for (size_t index = 0; index < count; index++) {
        json_object_array_add(*outJson, leaderboardEntryJson(&entries[index]));
    }
    return OK;
}

static json_object *statsJson(const ProfileStats *stats) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "matches", json_object_new_int64(stats->matches));
    json_object_object_add(object, "wins", json_object_new_int64(stats->wins));
    json_object_object_add(object, "teamWins", json_object_new_int64(stats->teamWins));
    json_object_object_add(object, "plus4", json_object_new_int64(stats->plus4));
    json_object_object_add(object, "unoCalls", json_object_new_int64(stats->unoCalls));
    json_object_object_add(object, "streak", json_object_new_int(stats->streak));
    json_object_object_add(object, "bestStreak", json_object_new_int64(stats->bestStreak));
    json_object_object_add(object, "peakElo", json_object_new_int(stats->peakElo));
    return object;
}

static json_object *badgesJson(uint32_t badges) {
    json_object *keys = json_object_new_array();
    size_t badgeCount = sizeof BADGE_KEYS / sizeof BADGE_KEYS[0];
    for (size_t index = 0; index < badgeCount; index++) {
        if (badges & BADGE_KEYS[index].bit) {
            json_object_array_add(keys, json_object_new_string(BADGE_KEYS[index].key));
        }
    }
    return keys;
}

static json_object *modesJson(const Profile *profile) {
    json_object *modes = json_object_new_array();
    for (size_t mode = 0; mode < STORE_MODE_COUNT; mode++) {
        json_object *object = json_object_new_object();
        json_object_object_add(object, "key", json_object_new_string(MODE_KEYS[mode]));
        json_object_object_add(object, "matches",
                               json_object_new_int64(profile->modes[mode].matches));
        json_object_object_add(object, "wins", json_object_new_int64(profile->modes[mode].wins));
        json_object_array_add(modes, object);
    }
    return modes;
}

static const char *modeKey(Mode mode) {
    return (size_t)mode < STORE_MODE_COUNT ? MODE_KEYS[mode] : MODE_KEYS[CLASSIC];
}

static json_object *historyJson(const Profile *profile) {
    json_object *history = json_object_new_array();
    for (size_t index = 0; index < profile->historyCount; index++) {
        const HistoryRow *row = &profile->history[index];
        json_object *object = json_object_new_object();
        json_object_object_add(object, "matchId", json_object_new_string(row->matchId));
        json_object_object_add(object, "mode", json_object_new_string(modeKey(row->mode)));
        json_object_object_add(object, "won", json_object_new_boolean(row->won));
        json_object_object_add(object, "delta", json_object_new_int(row->eloDelta));
        /* The store keeps Unix seconds; the wire uses epoch milliseconds like serverTimeMs. */
        json_object_object_add(object, "when",
                               json_object_new_int64(row->endedAt * MS_PER_SECOND));
        json_object_array_add(history, object);
    }
    return history;
}

static json_object *profileJson(const Profile *profile) {
    json_object *object = json_object_new_object();
    addIdentity(object, profile->userId, profile->name, profile->avatar, profile->elo);
    json_object_object_add(object, "rank",
                           json_object_new_string(ratingRankKey(ratingRank(profile->elo))));
    json_object_object_add(object, "stats", statsJson(&profile->stats));
    json_object_object_add(object, "badges", badgesJson(profile->badges));
    json_object_object_add(object, "modes", modesJson(profile));
    json_object_object_add(object, "history", historyJson(profile));
    return object;
}

/* ---------------------------------------------------------------------------
 * Pushes
 * ------------------------------------------------------------------------- */

static void pushFriendState(App *app, UserId user) {
    /* Skip the store read for someone who cannot receive the push anyway. */
    if (socialPresenceOf(app, user) == PRESENCE_OFFLINE) return;
    json_object *state = NULL;
    /* A failed read only costs this snapshot; the next change sends another. */
    if (friendStateJson(app, user, &state) != OK) return;
    (void)appPush(app, user, "FRIEND_STATE", state);
    json_object_put(state);
}

void socialPresence(App *app, UserId user) {
    if (!app || !app->store || user == 0) return;
    FriendRow rows[STORE_FRIEND_MAX];
    size_t count = 0;
    if (storeFriendList(app->store, user, rows, &count) != OK) return;

    json_object *own = json_object_new_object();
    json_object_object_add(own, "friends", friendsArrayJson(app, rows, count));
    /* NOT_FOUND just means u itself is offline; the friends still hear about it. */
    (void)appPush(app, user, "FRIEND_STATE", own);
    json_object_put(own);

    /* Each friend gets their own whole list: FRIEND_STATE replaces it client-side. */
    for (size_t index = 0; index < count; index++) {
        if (rows[index].link == FRIEND_ACCEPTED) pushFriendState(app, rows[index].userId);
    }
}

/* ---------------------------------------------------------------------------
 * Request handlers
 * ------------------------------------------------------------------------- */

static void replyBuilt(App *app, ConnId conn, int32_t id, Status status, json_object *payload) {
    if (status != OK) {
        appReplyStatus(app, conn, id, status);
        return;
    }
    appReplyOk(app, conn, id, payload);
    json_object_put(payload);
}

static void handleFriendList(App *app, ConnId conn, const Envelope *envelope, UserId me) {
    json_object *state = NULL;
    Status status = friendStateJson(app, me, &state);
    replyBuilt(app, conn, envelope->id, status, state);
}

static void handleFriendRequest(App *app, ConnId conn, const Envelope *envelope, UserId me) {
    UserId target = 0;
    if (!socialParseTag(stringField(envelope, "tag"), &target) || target == me) {
        appReplyStatus(app, conn, envelope->id, BAD_REQUEST);
        return;
    }
    /* NOT_FOUND (no such account) and INVALID_ACTION (already linked) come from the store. */
    Status status = storeFriendRequest(app->store, me, target);
    appReplyStatus(app, conn, envelope->id, status);
    if (status != OK) return;
    pushFriendState(app, me);
    pushFriendState(app, target);
}

static void handleFriendReply(App *app, ConnId conn, const Envelope *envelope, UserId me) {
    json_object *requestObj = NULL;
    json_object *acceptObj = NULL;
    UserId requester = 0;
    if (!envelope->payload ||
        !json_object_object_get_ex(envelope->payload, "requestId", &requestObj) ||
        !json_object_object_get_ex(envelope->payload, "accept", &acceptObj) ||
        !json_object_is_type(acceptObj, json_type_boolean) ||
        !parseUserIdValue(requestObj, &requester)) {
        appReplyStatus(app, conn, envelope->id, BAD_REQUEST);
        return;
    }
    Status status = storeFriendReply(app->store, me, requester,
                                       json_object_get_boolean(acceptObj));
    appReplyStatus(app, conn, envelope->id, status);
    if (status != OK) return;
    pushFriendState(app, me);
    pushFriendState(app, requester);
}

static Status loadProfileJson(App *app, UserId user, json_object **outJson) {
    Profile profile;
    Status status = storeProfileGet(app->store, user, &profile);
    if (status != OK) return status;
    *outJson = profileJson(&profile);
    return OK;
}

static void replyProfile(App *app, ConnId conn, int32_t id, json_object *profile) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "profile", profile);
    appReplyOk(app, conn, id, payload);
    json_object_put(payload);
}

static void handleProfileGet(App *app, ConnId conn, const Envelope *envelope, UserId me) {
    UserId target = me;
    json_object *userObj = NULL;
    if (envelope->payload && json_object_object_get_ex(envelope->payload, "userId", &userObj) &&
        !parseUserIdValue(userObj, &target)) {
        appReplyStatus(app, conn, envelope->id, BAD_REQUEST);
        return;
    }
    json_object *profile = NULL;
    Status status = loadProfileJson(app, target, &profile);
    if (status != OK) {
        appReplyStatus(app, conn, envelope->id, status);
        return;
    }
    replyProfile(app, conn, envelope->id, profile);
}

static void handleProfileUpdate(App *app, ConnId conn, const Envelope *envelope, UserId me) {
    json_object *userObj = NULL;
    if (envelope->payload && json_object_object_get_ex(envelope->payload, "userId", &userObj)) {
        UserId target = 0;
        if (!parseUserIdValue(userObj, &target)) {
            appReplyStatus(app, conn, envelope->id, BAD_REQUEST);
            return;
        }
        /* The account to edit always comes from the session, never the payload. */
        if (target != me) {
            appReplyStatus(app, conn, envelope->id, FORBIDDEN);
            return;
        }
    }
    const char *name = stringField(envelope, "name");
    const char *avatar = stringField(envelope, "avatar");
    if (!accountsValidateName(name) || !accountsValidateAvatar(avatar)) {
        appReplyStatus(app, conn, envelope->id, BAD_REQUEST);
        return;
    }
    Status status = storeProfileUpdate(app->store, me, name, avatar);
    if (status != OK) {
        /* Nothing changed, so nobody is told anything. */
        appReplyStatus(app, conn, envelope->id, status);
        return;
    }

    /* The rename is committed: rooms and friends must see it even if the
     * profile read for the reply fails below. */
    roomsUpdateIdentity(app, me, name, avatar);
    socialPresence(app, me);

    json_object *profile = NULL;
    status = loadProfileJson(app, me, &profile);
    if (status != OK) {
        appReplyStatus(app, conn, envelope->id, status);
        return;
    }
    (void)appPush(app, me, "PROFILE_STATE", profile);
    replyProfile(app, conn, envelope->id, profile);
}

static void handleLeaderboard(App *app, ConnId conn, const Envelope *envelope) {
    json_object *entries = NULL;
    Status status = leaderboardJson(app, &entries);
    if (status != OK) {
        appReplyStatus(app, conn, envelope->id, status);
        return;
    }
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "entries", entries);
    appReplyOk(app, conn, envelope->id, payload);
    json_object_put(payload);
}

static void handleLobby(App *app, ConnId conn, const Envelope *envelope, UserId me) {
    json_object *payload = NULL;
    Status status = friendStateJson(app, me, &payload);
    if (status != OK) {
        appReplyStatus(app, conn, envelope->id, status);
        return;
    }
    json_object *entries = NULL;
    status = leaderboardJson(app, &entries);
    if (status != OK) {
        json_object_put(payload);
        appReplyStatus(app, conn, envelope->id, status);
        return;
    }
    json_object_object_add(payload, "leaderboard", entries);
    appReplyOk(app, conn, envelope->id, payload);
    json_object_put(payload);
}

Status socialHandle(App *app, ConnId conn, const Envelope *envelope) {
    if (!app || !envelope || !envelope->type) return BAD_REQUEST;
    UserId me = 0;
    if (sessionsUser(app, conn, &me) != OK || me == 0) {
        appReplyStatus(app, conn, envelope->id, UNAUTHENTICATED);
        return OK;
    }
    if (!app->store) {
        appReplyStatus(app, conn, envelope->id, STORAGE_ERROR);
        return OK;
    }
    if (strcmp(envelope->type, "FRIEND_LIST") == 0) handleFriendList(app, conn, envelope, me);
    else if (strcmp(envelope->type,
                    "FRIEND_REQUEST") == 0) handleFriendRequest(app, conn, envelope, me);
    else if (strcmp(envelope->type,
                    "FRIEND_REPLY") == 0) handleFriendReply(app, conn, envelope, me);
    else if (strcmp(envelope->type, "PROFILE_GET") == 0) handleProfileGet(app, conn, envelope, me);
    else if (strcmp(envelope->type,
                    "PROFILE_UPDATE") == 0) handleProfileUpdate(app, conn, envelope, me);
    else if (strcmp(envelope->type, "LEADERBOARD_GET") == 0) handleLeaderboard(app, conn, envelope);
    else if (strcmp(envelope->type, "LOBBY_GET") == 0) handleLobby(app, conn, envelope, me);
    else appReplyStatus(app, conn, envelope->id, BAD_REQUEST);
    return OK;
}
