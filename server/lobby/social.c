#define _POSIX_C_SOURCE 200809L
#include "social.h"
#include "server/auth/accounts.h"
#include "server/game/match.h"
#include "server/game/rating.h"
#include "server/lobby/lobby_common.h"
#include "server/lobby/queue.h"
#include "server/lobby/rooms.h"
#include "server/storage/store.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

enum { MS_PER_SECOND = 1000 };

static const char TAG_PREFIX[] = "UNO#";

static const char *const PRESENCE_KEYS[] = {"offline", "idle", "online", "in_match"};

/* Tên huy hiệu theo spec §6, mỗi BadgeBit một tên. */
static const struct { BadgeBit bit; const char *key; } BADGE_KEYS[] = {
    {BADGE_FIRST_WIN, "first-win"}, {BADGE_WIN_100, "win-100"},
    {BADGE_STREAK_10, "streak-10"}, {BADGE_PLUS4, "plus4"},
    {BADGE_UNO_MASTER, "uno-master"}, {BADGE_TEAM, "team"},
    {BADGE_MATCHES, "matches-500"}, {BADGE_DIAMOND, "diamond"}
};

/* ------------------------------- Đọc tham số ------------------------------- */

bool socialParseTag(const char *tag, UserId *outUserId) {
    if (!tag || !outUserId) return false;
    size_t prefixLength = sizeof TAG_PREFIX - 1;
    if (strncmp(tag, TAG_PREFIX, prefixLength) != 0) return false;
    return lobbyParseUserIdText(tag + prefixLength, outUserId);
}

/* Trường chuỗi của payload, hoặc NULL khi thiếu hay không phải chuỗi. */
static const char *stringField(const Envelope *envelope, const char *key) {
    json_object *value = NULL;
    if (!envelope->payload || !json_object_object_get_ex(envelope->payload, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    return json_object_get_string(value);
}

/* Trường JSON bất kỳ của payload, hoặc NULL. */
static json_object *payloadField(const Envelope *envelope, const char *key) {
    json_object *value = NULL;
    if (!envelope->payload || !json_object_object_get_ex(envelope->payload, key, &value)) {
        return NULL;
    }
    return value;
}

/* --------------------------------- Presence --------------------------------- */

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
    if ((size_t)presence >= ARRAY_LENGTH(PRESENCE_KEYS)) return PRESENCE_KEYS[PRESENCE_OFFLINE];
    return PRESENCE_KEYS[presence];
}

/* ------------------------------- Dựng JSON ------------------------------- */

static json_object *tagJson(UserId user) {
    char tag[USER_TAG_SIZE];
    accountsFormatTag(user, tag);
    return json_object_new_string(tag);
}

/* Danh tính công khai dùng chung cho hàng bạn bè, bảng xếp hạng và hồ sơ; không
 * thông tin riêng tư nào (email, hash, token) đi qua đây. */
static void addIdentity(json_object *object, UserId user, const char *name,
                        const char *avatar, int elo) {
    json_object_object_add(object, "userId", lobbyUserIdJson(user));
    json_object_object_add(object, "tag", tagJson(user));
    json_object_object_add(object, "name", json_object_new_string(name));
    json_object_object_add(object, "avatar", json_object_new_string(avatar));
    json_object_object_add(object, "elo", json_object_new_int(elo));
}

static json_object *friendRowJson(App *app, const FriendRow *row) {
    json_object *object = json_object_new_object();
    bool pending = row->link != FRIEND_ACCEPTED;
    bool incoming = row->link == FRIEND_PENDING_IN;
    /* Spec: presence chỉ hiện cho bạn bè; người lạ mới gửi hoặc nhận lời mời không
     * được theo dõi tài khoản kia lúc online/offline. */
    Presence presence = pending ? PRESENCE_OFFLINE : socialPresenceOf(app, row->userId);
    addIdentity(object, row->userId, row->name, row->avatar, row->elo);
    json_object_object_add(object, "presence",
                           json_object_new_string(socialPresenceKey(presence)));
    json_object_object_add(object, "online",
                           json_object_new_boolean(presence != PRESENCE_OFFLINE));
    json_object_object_add(object, "pending", json_object_new_boolean(pending));
    json_object_object_add(object, "incoming", json_object_new_boolean(incoming));
    /* FRIEND_REPLY gọi lời mời theo người gửi, nên chỉ phía nhận cần requestId. */
    if (incoming) json_object_object_add(object, "requestId", lobbyUserIdJson(row->userId));
    return object;
}

static json_object *friendsArrayJson(App *app, const FriendRow rows[], size_t count) {
    json_object *friends = json_object_new_array();
    for (size_t index = 0; index < count; index++) {
        json_object_array_add(friends, friendRowJson(app, &rows[index]));
    }
    return friends;
}

/* Phản hồi FRIEND_LIST và push FRIEND_STATE dùng chung dạng {friends:[...]}. */
static Status friendStateJson(App *app, UserId user, json_object **outJson) {
    FriendRow rows[STORE_FRIEND_MAX];
    size_t count = 0;
    Status status = storeFriendList(app->store, user, rows, &count);
    if (status != OK) return status;
    *outJson = json_object_new_object();
    json_object_object_add(*outJson, "friends", friendsArrayJson(app, rows, count));
    return OK;
}

static json_object *rankJson(int elo) {
    return json_object_new_string(ratingRankKey(ratingRank(elo)));
}

static json_object *leaderboardEntryJson(const LeaderboardEntry *entry) {
    json_object *object = json_object_new_object();
    addIdentity(object, entry->userId, entry->name, entry->avatar, entry->elo);
    json_object_object_add(object, "rank", rankJson(entry->elo));
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
    for (size_t index = 0; index < ARRAY_LENGTH(BADGE_KEYS); index++) {
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
        json_object_object_add(object, "key", json_object_new_string(lobbyModeName((Mode)mode)));
        json_object_object_add(object, "matches",
                               json_object_new_int64(profile->modes[mode].matches));
        json_object_object_add(object, "wins", json_object_new_int64(profile->modes[mode].wins));
        json_object_array_add(modes, object);
    }
    return modes;
}

static json_object *historyJson(const Profile *profile) {
    json_object *history = json_object_new_array();
    for (size_t index = 0; index < profile->historyCount; index++) {
        const HistoryRow *row = &profile->history[index];
        json_object *object = json_object_new_object();
        json_object_object_add(object, "matchId", json_object_new_string(row->matchId));
        json_object_object_add(object, "mode", json_object_new_string(lobbyModeName(row->mode)));
        json_object_object_add(object, "won", json_object_new_boolean(row->won));
        json_object_object_add(object, "delta", json_object_new_int(row->eloDelta));
        /* Store lưu giây Unix; wire dùng mili giây epoch giống serverTimeMs. */
        json_object_object_add(object, "when",
                               json_object_new_int64(row->endedAt * MS_PER_SECOND));
        json_object_array_add(history, object);
    }
    return history;
}

static json_object *profileJson(const Profile *profile) {
    json_object *object = json_object_new_object();
    addIdentity(object, profile->userId, profile->name, profile->avatar, profile->elo);
    json_object_object_add(object, "rank", rankJson(profile->elo));
    json_object_object_add(object, "stats", statsJson(&profile->stats));
    json_object_object_add(object, "badges", badgesJson(profile->badges));
    json_object_object_add(object, "modes", modesJson(profile));
    json_object_object_add(object, "history", historyJson(profile));
    return object;
}

/* ---------------------------------- Push ---------------------------------- */

static void pushFriendState(App *app, UserId user) {
    /* Không đọc Store cho người dù sao cũng không nhận được push. */
    if (socialPresenceOf(app, user) == PRESENCE_OFFLINE) return;
    json_object *state = NULL;
    /* Đọc lỗi chỉ mất bản snapshot này; lần thay đổi sau sẽ gửi bản khác. */
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
    /* NOT_FOUND chỉ có nghĩa là chính user đang offline; bạn bè vẫn được báo. */
    (void)appPush(app, user, "FRIEND_STATE", own);
    json_object_put(own);

    /* Mỗi người bạn nhận nguyên danh sách của họ: FRIEND_STATE thay thế toàn bộ ở client. */
    for (size_t index = 0; index < count; index++) {
        if (rows[index].link == FRIEND_ACCEPTED) pushFriendState(app, rows[index].userId);
    }
}

/* ------------------------------ Xử lý request ------------------------------ */

/* Một request social đang xử lý; @c me luôn lấy từ phiên, không từ payload. */
typedef struct {
    App *app;
    ConnId conn;
    const Envelope *envelope;
    UserId me;
} SocialRequest;

static void replyStatus(const SocialRequest *request, Status status) {
    appReplyStatus(request->app, request->conn, request->envelope->id, status);
}

/* Trả OK với payload đã dựng rồi giải phóng nó; status lỗi thì trả ERROR. */
static void replyBuilt(const SocialRequest *request, Status status, json_object *payload) {
    if (status != OK) {
        replyStatus(request, status);
        return;
    }
    appReplyOk(request->app, request->conn, request->envelope->id, payload);
    json_object_put(payload);
}

static void handleFriendList(const SocialRequest *request) {
    json_object *state = NULL;
    Status status = friendStateJson(request->app, request->me, &state);
    replyBuilt(request, status, state);
}

static void handleFriendRequest(const SocialRequest *request) {
    UserId target = 0;
    if (!socialParseTag(stringField(request->envelope, "tag"), &target) || target == request->me) {
        replyStatus(request, BAD_REQUEST);
        return;
    }
    /* NOT_FOUND (không có tài khoản) và INVALID_ACTION (đã có quan hệ) đến từ Store. */
    Status status = storeFriendRequest(request->app->store, request->me, target);
    replyStatus(request, status);
    if (status != OK) return;
    pushFriendState(request->app, request->me);
    pushFriendState(request->app, target);
}

static void handleFriendReply(const SocialRequest *request) {
    json_object *requestJson = payloadField(request->envelope, "requestId");
    json_object *acceptJson = payloadField(request->envelope, "accept");
    UserId requester = 0;
    if (!requestJson || !json_object_is_type(acceptJson, json_type_boolean) ||
        !lobbyParseUserIdJson(requestJson, &requester)) {
        replyStatus(request, BAD_REQUEST);
        return;
    }
    Status status = storeFriendReply(request->app->store, request->me, requester,
                                     json_object_get_boolean(acceptJson));
    replyStatus(request, status);
    if (status != OK) return;
    pushFriendState(request->app, request->me);
    pushFriendState(request->app, requester);
}

static Status loadProfileJson(App *app, UserId user, json_object **outJson) {
    Profile profile;
    Status status = storeProfileGet(app->store, user, &profile);
    if (status != OK) return status;
    *outJson = profileJson(&profile);
    return OK;
}

/* Bọc hồ sơ thành {profile:...} rồi trả OK; hàm chiếm quyền sở hữu @p profile. */
static void replyProfile(const SocialRequest *request, json_object *profile) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "profile", profile);
    replyBuilt(request, OK, payload);
}

static void handleProfileGet(const SocialRequest *request) {
    UserId target = request->me;
    json_object *userJson = payloadField(request->envelope, "userId");
    if (userJson && !lobbyParseUserIdJson(userJson, &target)) {
        replyStatus(request, BAD_REQUEST);
        return;
    }
    json_object *profile = NULL;
    Status status = loadProfileJson(request->app, target, &profile);
    if (status != OK) {
        replyStatus(request, status);
        return;
    }
    replyProfile(request, profile);
}

/**
 * @brief Kiểm tra trường "userId" tùy chọn của PROFILE_UPDATE.
 *
 * Tài khoản được sửa luôn lấy từ phiên; payload chỉ được nhắc lại đúng mã đó.
 *
 * @return OK, BAD_REQUEST khi mã sai dạng, FORBIDDEN khi là tài khoản khác.
 */
static Status checkProfileTarget(const SocialRequest *request) {
    json_object *userJson = payloadField(request->envelope, "userId");
    if (!userJson) return OK;
    UserId target = 0;
    if (!lobbyParseUserIdJson(userJson, &target)) return BAD_REQUEST;
    return target == request->me ? OK : FORBIDDEN;
}

static void handleProfileUpdate(const SocialRequest *request) {
    Status status = checkProfileTarget(request);
    const char *name = stringField(request->envelope, "name");
    const char *avatar = stringField(request->envelope, "avatar");
    if (status == OK && (!accountsValidateName(name) || !accountsValidateAvatar(avatar))) {
        status = BAD_REQUEST;
    }
    /* Không ghi được thì không có gì đổi, nên không báo cho ai. */
    if (status == OK) status = storeProfileUpdate(request->app->store, request->me, name, avatar);
    if (status != OK) {
        replyStatus(request, status);
        return;
    }
    /* Đổi tên đã commit: phòng và bạn bè phải thấy kể cả khi đọc hồ sơ để trả lời
     * bên dưới bị lỗi. */
    roomsUpdateIdentity(request->app, request->me, name, avatar);
    socialPresence(request->app, request->me);

    json_object *profile = NULL;
    status = loadProfileJson(request->app, request->me, &profile);
    if (status != OK) {
        replyStatus(request, status);
        return;
    }
    (void)appPush(request->app, request->me, "PROFILE_STATE", profile);
    replyProfile(request, profile);
}

static void handleLeaderboard(const SocialRequest *request) {
    json_object *entries = NULL;
    Status status = leaderboardJson(request->app, &entries);
    json_object *payload = NULL;
    if (status == OK) {
        payload = json_object_new_object();
        json_object_object_add(payload, "entries", entries);
    }
    replyBuilt(request, status, payload);
}

static void handleLobby(const SocialRequest *request) {
    json_object *payload = NULL;
    Status status = friendStateJson(request->app, request->me, &payload);
    json_object *entries = NULL;
    if (status == OK) status = leaderboardJson(request->app, &entries);
    if (status == OK) {
        json_object_object_add(payload, "leaderboard", entries);
    } else {
        /* friendStateJson có thể đã dựng payload trước khi bảng xếp hạng lỗi. */
        json_object_put(payload);
        payload = NULL;
    }
    replyBuilt(request, status, payload);
}

typedef void (*SocialHandler)(const SocialRequest *request);

static const struct {
    const char *type;
    SocialHandler handler;
} SOCIAL_HANDLERS[] = {
    {"FRIEND_LIST", handleFriendList}, {"FRIEND_REQUEST", handleFriendRequest},
    {"FRIEND_REPLY", handleFriendReply}, {"PROFILE_GET", handleProfileGet},
    {"PROFILE_UPDATE", handleProfileUpdate}, {"LEADERBOARD_GET", handleLeaderboard},
    {"LOBBY_GET", handleLobby},
};

Status socialHandle(App *app, ConnId conn, const Envelope *envelope) {
    if (!app || !envelope || !envelope->type) return BAD_REQUEST;
    SocialRequest request = {app, conn, envelope, 0};
    if (sessionsUser(app, conn, &request.me) != OK || request.me == 0) {
        replyStatus(&request, UNAUTHENTICATED);
        return OK;
    }
    if (!app->store) {
        replyStatus(&request, STORAGE_ERROR);
        return OK;
    }
    for (size_t i = 0; i < ARRAY_LENGTH(SOCIAL_HANDLERS); i++) {
        if (strcmp(envelope->type, SOCIAL_HANDLERS[i].type) == 0) {
            SOCIAL_HANDLERS[i].handler(&request);
            return OK;
        }
    }
    replyStatus(&request, BAD_REQUEST);
    return OK;
}
