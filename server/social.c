#define _POSIX_C_SOURCE 200809L
#include "social.h"
#include "accounts.h"
#include "server/game/match.h"
#include "queue.h"
#include "rating.h"
#include "rooms.h"
#include "store.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    ID_TEXT_SIZE = 32, /* fits the decimal form of any 64-bit id */
    TAG_TEXT_SIZE = 32, /* buffer size accounts_format_tag writes */
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
static bool parse_id_text(const char *text, UserId *out) {
    if (!text || text[0] < '1' || text[0] > '9') return false;
    uint64_t value = 0;
    for (const char *cursor = text; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') return false;
        uint64_t digit = (uint64_t)(*cursor - '0');
        if (value > ((uint64_t)INT64_MAX - digit) / DECIMAL_BASE) return false;
        value = value * DECIMAL_BASE + digit;
    }
    *out = (UserId)value;
    return true;
}

bool social_parse_tag(const char *tag, UserId *out) {
    if (!tag || !out) return false;
    size_t prefixLength = sizeof TAG_PREFIX - 1;
    if (strncmp(tag, TAG_PREFIX, prefixLength) != 0) return false;
    return parse_id_text(tag + prefixLength, out);
}

/* User ids travel as decimal strings; a plain positive JSON integer is also
 * accepted, like INVITE_SEND does. */
static bool parse_user_id_value(json_object *value, UserId *out) {
    if (json_object_is_type(value, json_type_string)) {
        return parse_id_text(json_object_get_string(value), out);
    }
    if (json_object_is_type(value, json_type_int)) {
        int64_t number = json_object_get_int64(value);
        if (number <= 0) return false;
        *out = (UserId)number;
        return true;
    }
    return false;
}

static const char *string_field(const Envelope *e, const char *key) {
    json_object *value = NULL;
    if (!e->payload || !json_object_object_get_ex(e->payload, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    return json_object_get_string(value);
}

/* ---------------------------------------------------------------------------
 * Presence
 * ------------------------------------------------------------------------- */

Presence social_presence_of(App *app, UserId u) {
    if (!app || u == 0) return PRESENCE_OFFLINE;
    ConnId conn;
    if (sessions_find_conn(app, u, &conn) != OK) return PRESENCE_OFFLINE;
    if (matchesContains(app->matches, u)) return PRESENCE_IN_MATCH;
    if (rooms_find_by_user(app->rooms, u) || party_find_by_user(app->rooms, u) ||
        queue_contains(app->queue, u)) {
        return PRESENCE_ONLINE;
    }
    return PRESENCE_IDLE;
}

const char *social_presence_key(Presence p) {
    size_t keyCount = sizeof PRESENCE_KEYS / sizeof PRESENCE_KEYS[0];
    if ((size_t)p >= keyCount) return PRESENCE_KEYS[PRESENCE_OFFLINE];
    return PRESENCE_KEYS[p];
}

/* ---------------------------------------------------------------------------
 * JSON builders
 * ------------------------------------------------------------------------- */

/* Wire ids are decimal strings so 64-bit ids survive JSON number precision. */
static json_object *user_id_json(UserId user) {
    char idText[ID_TEXT_SIZE];
    snprintf(idText, sizeof idText, "%llu", (unsigned long long)user);
    return json_object_new_string(idText);
}

static json_object *tag_json(UserId user) {
    char tag[TAG_TEXT_SIZE];
    accounts_format_tag(user, tag);
    return json_object_new_string(tag);
}

/* Public identity shared by friend rows, leaderboard entries and profiles;
 * nothing private (email, hash, token) ever goes through here. */
static void add_identity(json_object *object, UserId user, const char *name,
                         const char *avatar, int elo) {
    json_object_object_add(object, "userId", user_id_json(user));
    json_object_object_add(object, "tag", tag_json(user));
    json_object_object_add(object, "name", json_object_new_string(name));
    json_object_object_add(object, "avatar", json_object_new_string(avatar));
    json_object_object_add(object, "elo", json_object_new_int(elo));
}

static json_object *friend_row_json(App *app, const FriendRow *row) {
    json_object *object = json_object_new_object();
    bool pending = row->link != FRIEND_ACCEPTED;
    bool incoming = row->link == FRIEND_PENDING_IN;
    /* Spec: presence is shown to friends only; a stranger who merely sent or
     * received a request cannot watch the other account come and go. */
    Presence presence = pending ? PRESENCE_OFFLINE : social_presence_of(app, row->userId);
    add_identity(object, row->userId, row->name, row->avatar, row->elo);
    json_object_object_add(object, "presence",
                           json_object_new_string(social_presence_key(presence)));
    json_object_object_add(object, "online",
                           json_object_new_boolean(presence != PRESENCE_OFFLINE));
    json_object_object_add(object, "pending", json_object_new_boolean(pending));
    json_object_object_add(object, "incoming", json_object_new_boolean(incoming));
    /* FRIEND_REPLY names the request by its sender, so only the receiver needs it. */
    if (incoming) json_object_object_add(object, "requestId", user_id_json(row->userId));
    return object;
}

static json_object *friends_array_json(App *app, const FriendRow rows[], size_t count) {
    json_object *friends = json_object_new_array();
    for (size_t index = 0; index < count; index++) {
        json_object_array_add(friends, friend_row_json(app, &rows[index]));
    }
    return friends;
}

/* FRIEND_LIST OK and FRIEND_STATE share this {friends:[...]} shape. */
static Status friend_state_json(App *app, UserId user, json_object **out) {
    FriendRow rows[STORE_FRIEND_MAX];
    size_t count = 0;
    Status status = store_friend_list(app->store, user, rows, &count);
    if (status != OK) return status;
    *out = json_object_new_object();
    json_object_object_add(*out, "friends", friends_array_json(app, rows, count));
    return OK;
}

static json_object *leaderboard_entry_json(const LeaderboardEntry *entry) {
    json_object *object = json_object_new_object();
    add_identity(object, entry->userId, entry->name, entry->avatar, entry->elo);
    json_object_object_add(object, "rank",
                           json_object_new_string(rating_rank_key(rating_rank(entry->elo))));
    return object;
}

static Status leaderboard_json(App *app, json_object **out) {
    LeaderboardEntry entries[STORE_LEADERBOARD_MAX];
    size_t count = 0;
    Status status = store_leaderboard(app->store, entries, &count);
    if (status != OK) return status;
    *out = json_object_new_array();
    for (size_t index = 0; index < count; index++) {
        json_object_array_add(*out, leaderboard_entry_json(&entries[index]));
    }
    return OK;
}

static json_object *stats_json(const ProfileStats *stats) {
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

static json_object *badges_json(uint32_t badges) {
    json_object *keys = json_object_new_array();
    size_t badgeCount = sizeof BADGE_KEYS / sizeof BADGE_KEYS[0];
    for (size_t index = 0; index < badgeCount; index++) {
        if (badges & BADGE_KEYS[index].bit) {
            json_object_array_add(keys, json_object_new_string(BADGE_KEYS[index].key));
        }
    }
    return keys;
}

static json_object *modes_json(const Profile *profile) {
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

static const char *mode_key(Mode mode) {
    return (size_t)mode < STORE_MODE_COUNT ? MODE_KEYS[mode] : MODE_KEYS[CLASSIC];
}

static json_object *history_json(const Profile *profile) {
    json_object *history = json_object_new_array();
    for (size_t index = 0; index < profile->historyCount; index++) {
        const HistoryRow *row = &profile->history[index];
        json_object *object = json_object_new_object();
        json_object_object_add(object, "matchId", json_object_new_string(row->matchId));
        json_object_object_add(object, "mode", json_object_new_string(mode_key(row->mode)));
        json_object_object_add(object, "won", json_object_new_boolean(row->won));
        json_object_object_add(object, "delta", json_object_new_int(row->eloDelta));
        /* The store keeps Unix seconds; the wire uses epoch milliseconds like serverTimeMs. */
        json_object_object_add(object, "when",
                               json_object_new_int64(row->endedAt * MS_PER_SECOND));
        json_object_array_add(history, object);
    }
    return history;
}

static json_object *profile_json(const Profile *profile) {
    json_object *object = json_object_new_object();
    add_identity(object, profile->userId, profile->name, profile->avatar, profile->elo);
    json_object_object_add(object, "rank",
                           json_object_new_string(rating_rank_key(rating_rank(profile->elo))));
    json_object_object_add(object, "stats", stats_json(&profile->stats));
    json_object_object_add(object, "badges", badges_json(profile->badges));
    json_object_object_add(object, "modes", modes_json(profile));
    json_object_object_add(object, "history", history_json(profile));
    return object;
}

/* ---------------------------------------------------------------------------
 * Pushes
 * ------------------------------------------------------------------------- */

static void push_friend_state(App *app, UserId user) {
    /* Skip the store read for someone who cannot receive the push anyway. */
    if (social_presence_of(app, user) == PRESENCE_OFFLINE) return;
    json_object *state = NULL;
    /* A failed read only costs this snapshot; the next change sends another. */
    if (friend_state_json(app, user, &state) != OK) return;
    (void)app_push(app, user, "FRIEND_STATE", state);
    json_object_put(state);
}

void social_presence(App *app, UserId u) {
    if (!app || !app->store || u == 0) return;
    FriendRow rows[STORE_FRIEND_MAX];
    size_t count = 0;
    if (store_friend_list(app->store, u, rows, &count) != OK) return;

    json_object *own = json_object_new_object();
    json_object_object_add(own, "friends", friends_array_json(app, rows, count));
    /* NOT_FOUND just means u itself is offline; the friends still hear about it. */
    (void)app_push(app, u, "FRIEND_STATE", own);
    json_object_put(own);

    /* Each friend gets their own whole list: FRIEND_STATE replaces it client-side. */
    for (size_t index = 0; index < count; index++) {
        if (rows[index].link == FRIEND_ACCEPTED) push_friend_state(app, rows[index].userId);
    }
}

/* ---------------------------------------------------------------------------
 * Request handlers
 * ------------------------------------------------------------------------- */

static void reply_built(App *app, ConnId conn, int32_t id, Status status, json_object *payload) {
    if (status != OK) {
        app_reply_status(app, conn, id, status);
        return;
    }
    app_reply_ok(app, conn, id, payload);
    json_object_put(payload);
}

static void handle_friend_list(App *app, ConnId conn, const Envelope *e, UserId me) {
    json_object *state = NULL;
    Status status = friend_state_json(app, me, &state);
    reply_built(app, conn, e->id, status, state);
}

static void handle_friend_request(App *app, ConnId conn, const Envelope *e, UserId me) {
    UserId target = 0;
    if (!social_parse_tag(string_field(e, "tag"), &target) || target == me) {
        app_reply_status(app, conn, e->id, BAD_REQUEST);
        return;
    }
    /* NOT_FOUND (no such account) and INVALID_ACTION (already linked) come from the store. */
    Status status = store_friend_request(app->store, me, target);
    app_reply_status(app, conn, e->id, status);
    if (status != OK) return;
    push_friend_state(app, me);
    push_friend_state(app, target);
}

static void handle_friend_reply(App *app, ConnId conn, const Envelope *e, UserId me) {
    json_object *requestObj = NULL;
    json_object *acceptObj = NULL;
    UserId requester = 0;
    if (!e->payload ||
        !json_object_object_get_ex(e->payload, "requestId", &requestObj) ||
        !json_object_object_get_ex(e->payload, "accept", &acceptObj) ||
        !json_object_is_type(acceptObj, json_type_boolean) ||
        !parse_user_id_value(requestObj, &requester)) {
        app_reply_status(app, conn, e->id, BAD_REQUEST);
        return;
    }
    Status status = store_friend_reply(app->store, me, requester,
                                       json_object_get_boolean(acceptObj));
    app_reply_status(app, conn, e->id, status);
    if (status != OK) return;
    push_friend_state(app, me);
    push_friend_state(app, requester);
}

static Status load_profile_json(App *app, UserId user, json_object **out) {
    Profile profile;
    Status status = store_profile_get(app->store, user, &profile);
    if (status != OK) return status;
    *out = profile_json(&profile);
    return OK;
}

static void reply_profile(App *app, ConnId conn, int32_t id, json_object *profile) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "profile", profile);
    app_reply_ok(app, conn, id, payload);
    json_object_put(payload);
}

static void handle_profile_get(App *app, ConnId conn, const Envelope *e, UserId me) {
    UserId target = me;
    json_object *userObj = NULL;
    if (e->payload && json_object_object_get_ex(e->payload, "userId", &userObj) &&
        !parse_user_id_value(userObj, &target)) {
        app_reply_status(app, conn, e->id, BAD_REQUEST);
        return;
    }
    json_object *profile = NULL;
    Status status = load_profile_json(app, target, &profile);
    if (status != OK) {
        app_reply_status(app, conn, e->id, status);
        return;
    }
    reply_profile(app, conn, e->id, profile);
}

static void handle_profile_update(App *app, ConnId conn, const Envelope *e, UserId me) {
    json_object *userObj = NULL;
    if (e->payload && json_object_object_get_ex(e->payload, "userId", &userObj)) {
        UserId target = 0;
        if (!parse_user_id_value(userObj, &target)) {
            app_reply_status(app, conn, e->id, BAD_REQUEST);
            return;
        }
        /* The account to edit always comes from the session, never the payload. */
        if (target != me) {
            app_reply_status(app, conn, e->id, FORBIDDEN);
            return;
        }
    }
    const char *name = string_field(e, "name");
    const char *avatar = string_field(e, "avatar");
    if (!accounts_validate_name(name) || !accounts_validate_avatar(avatar)) {
        app_reply_status(app, conn, e->id, BAD_REQUEST);
        return;
    }
    Status status = store_profile_update(app->store, me, name, avatar);
    if (status != OK) {
        /* Nothing changed, so nobody is told anything. */
        app_reply_status(app, conn, e->id, status);
        return;
    }

    /* The rename is committed: rooms and friends must see it even if the
     * profile read for the reply fails below. */
    rooms_update_identity(app, me, name, avatar);
    social_presence(app, me);

    json_object *profile = NULL;
    status = load_profile_json(app, me, &profile);
    if (status != OK) {
        app_reply_status(app, conn, e->id, status);
        return;
    }
    (void)app_push(app, me, "PROFILE_STATE", profile);
    reply_profile(app, conn, e->id, profile);
}

static void handle_leaderboard(App *app, ConnId conn, const Envelope *e) {
    json_object *entries = NULL;
    Status status = leaderboard_json(app, &entries);
    if (status != OK) {
        app_reply_status(app, conn, e->id, status);
        return;
    }
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "entries", entries);
    app_reply_ok(app, conn, e->id, payload);
    json_object_put(payload);
}

static void handle_lobby(App *app, ConnId conn, const Envelope *e, UserId me) {
    json_object *payload = NULL;
    Status status = friend_state_json(app, me, &payload);
    if (status != OK) {
        app_reply_status(app, conn, e->id, status);
        return;
    }
    json_object *entries = NULL;
    status = leaderboard_json(app, &entries);
    if (status != OK) {
        json_object_put(payload);
        app_reply_status(app, conn, e->id, status);
        return;
    }
    json_object_object_add(payload, "leaderboard", entries);
    app_reply_ok(app, conn, e->id, payload);
    json_object_put(payload);
}

Status social_handle(App *app, ConnId conn, const Envelope *e) {
    if (!app || !e || !e->type) return BAD_REQUEST;
    UserId me = 0;
    if (sessions_user(app, conn, &me) != OK || me == 0) {
        app_reply_status(app, conn, e->id, UNAUTHENTICATED);
        return OK;
    }
    if (!app->store) {
        app_reply_status(app, conn, e->id, STORAGE_ERROR);
        return OK;
    }
    if (strcmp(e->type, "FRIEND_LIST") == 0) handle_friend_list(app, conn, e, me);
    else if (strcmp(e->type, "FRIEND_REQUEST") == 0) handle_friend_request(app, conn, e, me);
    else if (strcmp(e->type, "FRIEND_REPLY") == 0) handle_friend_reply(app, conn, e, me);
    else if (strcmp(e->type, "PROFILE_GET") == 0) handle_profile_get(app, conn, e, me);
    else if (strcmp(e->type, "PROFILE_UPDATE") == 0) handle_profile_update(app, conn, e, me);
    else if (strcmp(e->type, "LEADERBOARD_GET") == 0) handle_leaderboard(app, conn, e);
    else if (strcmp(e->type, "LOBBY_GET") == 0) handle_lobby(app, conn, e, me);
    else app_reply_status(app, conn, e->id, BAD_REQUEST);
    return OK;
}
