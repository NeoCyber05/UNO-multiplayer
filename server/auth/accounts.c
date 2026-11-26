#define _POSIX_C_SOURCE 200809L
#include "accounts.h"
#include "server/auth/auth_worker.h"
#include "server/game/match.h"
#include "server/lobby/queue.h"
#include "server/lobby/rooms.h"
#include "server/lobby/social.h"
#include "server/storage/store.h"
#include <ctype.h>
#include <errno.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    /* Số IP và số lần thử mỗi IP mà bộ giới hạn tần suất nhớ được. */
    MAX_RATE_LIMIT_IPS = 256,
    MAX_TIMESTAMPS_PER_IP = 128,
    IP_TEXT_SIZE = 64,
    EMAIL_MIN_LENGTH = 3,
    EMAIL_MAX_LENGTH = 254,
    PASSWORD_MIN_LENGTH = 6,
    PASSWORD_MAX_LENGTH = 128,
    NAME_MAX_BYTES = 64,
    NAME_MAX_CODE_POINTS = 16,
    /* Cửa sổ trượt của giới hạn xác thực theo IP: một phút. */
    AUTH_RATE_WINDOW_MS = 60000,
    /* Email chỉ gồm ký tự ASCII nhìn thấy được: '!' (33) tới '~' (126). */
    FIRST_VISIBLE_ASCII = 33,
    LAST_VISIBLE_ASCII = 126,
    /* Tên được phép có khoảng trắng nhưng không có ký tự điều khiển. */
    FIRST_PRINTABLE_ASCII = 32,
    ASCII_DELETE = 127,
    LAST_ASCII = 0x7F,
    /* Mặt nạ/tiền tố nhận diện độ dài chuỗi UTF-8 theo byte dẫn (RFC 3629). */
    UTF8_TWO_BYTE_MASK = 0xE0,
    UTF8_TWO_BYTE_PREFIX = 0xC0,
    UTF8_THREE_BYTE_MASK = 0xF0,
    UTF8_THREE_BYTE_PREFIX = 0xE0,
    UTF8_FOUR_BYTE_MASK = 0xF8,
    UTF8_FOUR_BYTE_PREFIX = 0xF0,
    UTF8_CONTINUATION_MASK = 0xC0,
    UTF8_CONTINUATION_PREFIX = 0x80,
    /* Các mốc loại chuỗi overlong, surrogate và mã vượt U+10FFFF. */
    UTF8_MIN_TWO_BYTE_LEAD = 0xC2,
    UTF8_OVERLONG_THREE_LEAD = 0xE0,
    UTF8_THREE_BYTE_MIN_SECOND = 0xA0,
    UTF8_SURROGATE_LEAD = 0xED,
    UTF8_SURROGATE_MIN_SECOND = 0xA0,
    UTF8_OVERLONG_FOUR_LEAD = 0xF0,
    UTF8_FOUR_BYTE_MIN_SECOND = 0x90,
    UTF8_MAX_LEAD = 0xF4,
    UTF8_MAX_LEAD_MAX_SECOND = 0x8F,
    UTF8_MAX_SEQUENCE = 4,
    /* UserId 64 bit viết thập phân tối đa 20 chữ số. */
    USER_ID_TEXT_SIZE = 32,
    TOKEN_HEX_LENGTH = SESSION_TOKEN_SIZE - 1,
    MS_PER_SECOND = 1000,
    NS_PER_MS = 1000000,
    SECONDS_PER_HOUR = 3600,
    HOURS_PER_DAY = 24,
    /* Spec: token phiên có hiệu lực 24 giờ kể từ LOGIN. */
    SESSION_TTL_HOURS = 24,
    /* Độ lệch thêm vào đồng hồ monotonic; xem accountsGetNowMs. */
    CLOCK_OFFSET_DAYS = 100
};

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

/* Vị trí các trường trong mảng giá trị đọc từ payload REGISTER/LOGIN. */
enum { FIELD_EMAIL, FIELD_PASSWORD, FIELD_NAME, FIELD_AVATAR };

static const uint64_t MS_PER_HOUR = (uint64_t)SECONDS_PER_HOUR * MS_PER_SECOND;

/* Ký tự đặc biệt được phép trong email ngoài chữ và số. */
static const char EMAIL_SYMBOLS[] = "._-+@%~";

/* Các mã avatar mà client có hình vẽ. */
static const char *const AVATAR_WHITELIST[] = {
    "fox", "panda", "bear", "owl", "lion", "tiger", "rabbit", "koala",
    "boy", "girl", "bun", "afro", "nerd", "beard", "cap", "punk", "grandpa",
    "blonde", "pigtails", "cat", "frog", "robot", "bunny", "ghost", "alien",
    "sun", "cool"
};

/* Các lần xác thực từ một IP trong cửa sổ trượt. */
typedef struct {
    char ip[IP_TEXT_SIZE];
    uint64_t timestamps[MAX_TIMESTAMPS_PER_IP];
    size_t count;
    uint64_t lastSeenMs;
} IpRateEntry;

/* Bảng cố định; khi đầy thì thay IP lâu nhất không thấy. */
struct RateLimiter {
    IpRateEntry entries[MAX_RATE_LIMIT_IPS];
};

uint64_t accountsGetNowMs(void) {
    /* Cộng thêm 100 ngày để mọi mốc thời gian thật đều lớn hơn hẳn 0, giá trị mà
     * các ô phiên và ô giới hạn tần suất vừa bị xóa đang giữ. CLOCK_MONOTONIC bắt
     * đầu gần 0 lúc khởi động máy và không nhảy khi đổi giờ hệ thống. */
    const uint64_t offsetMs = (uint64_t)CLOCK_OFFSET_DAYS * HOURS_PER_DAY * MS_PER_HOUR;
    struct timespec monotonicTime;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonicTime) != 0) {
        /* CLOCK_MONOTONIC không thể lỗi trên Linux; vẫn trả giá trị xác định. */
        perror("clock_gettime");
        return offsetMs;
    }
    return offsetMs + (uint64_t)monotonicTime.tv_sec * MS_PER_SECOND +
           (uint64_t)(monotonicTime.tv_nsec / NS_PER_MS);
}

/* ------------------------- Kiểm tra đầu vào ------------------------- */

/* Ký tự hợp lệ trong email: ASCII nhìn thấy được, không phải chữ hoa. */
static bool isEmailCharacter(unsigned char character) {
    if (character < FIRST_VISIBLE_ASCII || character > LAST_VISIBLE_ASCII) return false;
    if (character >= 'A' && character <= 'Z') return false;
    /* character không bao giờ là NUL ở đây nên strchr không khớp nhầm ký tự kết thúc. */
    return isalnum(character) || strchr(EMAIL_SYMBOLS, character) != NULL;
}

bool accountsValidateEmail(const char *email) {
    if (!email) return false;
    size_t length = strlen(email);
    if (length < EMAIL_MIN_LENGTH || length > EMAIL_MAX_LENGTH) return false;

    /* Đúng một '@', có phần tên phía trước và tên miền phía sau. */
    const char *atSign = strchr(email, '@');
    if (!atSign || atSign == email || atSign[1] == '\0') return false;
    if (strchr(atSign + 1, '@')) return false;

    for (size_t i = 0; i < length; i++) {
        if (!isEmailCharacter((unsigned char)email[i])) return false;
    }
    return true;
}

bool accountsValidatePassword(const char *password) {
    if (!password) return false;
    size_t length = strlen(password);
    return length >= PASSWORD_MIN_LENGTH && length <= PASSWORD_MAX_LENGTH;
}

static bool isUtf8Continuation(unsigned char byte) {
    return (byte & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION_PREFIX;
}

/* Độ dài chuỗi UTF-8 theo byte dẫn, hoặc 0 khi byte dẫn không hợp lệ. */
static size_t utf8LeadLength(unsigned char lead) {
    if ((lead & UTF8_TWO_BYTE_MASK) == UTF8_TWO_BYTE_PREFIX) {
        return lead < UTF8_MIN_TWO_BYTE_LEAD ? 0 : 2;
    }
    if ((lead & UTF8_THREE_BYTE_MASK) == UTF8_THREE_BYTE_PREFIX) return 3;
    if ((lead & UTF8_FOUR_BYTE_MASK) == UTF8_FOUR_BYTE_PREFIX) return UTF8_MAX_SEQUENCE;
    return 0;
}

/**
 * @brief Đo một ký tự UTF-8 bắt đầu tại @p cursor.
 *
 * Byte nối được kiểm tra lần lượt và dừng ở byte sai đầu tiên, nên không bao giờ
 * đọc quá ký tự NUL kết thúc chuỗi.
 *
 * @return Số byte của ký tự, hoặc 0 khi là ký tự điều khiển, chuỗi overlong,
 *         surrogate, mã vượt U+10FFFF hay chuỗi bị cắt cụt.
 */
static size_t utf8SequenceLength(const unsigned char *cursor) {
    unsigned char lead = cursor[0];
    if (lead <= LAST_ASCII) {
        return (lead < FIRST_PRINTABLE_ASCII || lead == ASCII_DELETE) ? 0 : 1;
    }
    size_t length = utf8LeadLength(lead);
    if (length == 0) return 0;
    for (size_t i = 1; i < length; i++) {
        if (!isUtf8Continuation(cursor[i])) return 0;
    }
    unsigned char second = cursor[1];
    if (lead == UTF8_OVERLONG_THREE_LEAD && second < UTF8_THREE_BYTE_MIN_SECOND) return 0;
    if (lead == UTF8_SURROGATE_LEAD && second >= UTF8_SURROGATE_MIN_SECOND) return 0;
    if (lead == UTF8_OVERLONG_FOUR_LEAD && second < UTF8_FOUR_BYTE_MIN_SECOND) return 0;
    if (lead == UTF8_MAX_LEAD && second > UTF8_MAX_LEAD_MAX_SECOND) return 0;
    return length;
}

bool accountsValidateName(const char *name) {
    if (!name) return false;
    size_t byteLength = strlen(name);
    if (byteLength < 1 || byteLength > NAME_MAX_BYTES) return false;

    size_t codePointCount = 0;
    for (const unsigned char *cursor = (const unsigned char *)name; *cursor;) {
        size_t sequenceLength = utf8SequenceLength(cursor);
        if (sequenceLength == 0) return false;
        codePointCount++;
        cursor += sequenceLength;
    }
    return codePointCount >= 1 && codePointCount <= NAME_MAX_CODE_POINTS;
}

bool accountsValidateAvatar(const char *avatar) {
    if (!avatar || avatar[0] == '\0') return false;
    for (size_t i = 0; i < ARRAY_LENGTH(AVATAR_WHITELIST); i++) {
        if (strcmp(avatar, AVATAR_WHITELIST[i]) == 0) return true;
    }
    return false;
}

/* ----------------------- Token, tag và phản hồi ----------------------- */

void accountsGenerateToken(char outToken[SESSION_TOKEN_SIZE]) {
    unsigned char bytes[SESSION_TOKEN_BYTES];
    randombytes_buf(bytes, sizeof(bytes));
    sodium_bin2hex(outToken, SESSION_TOKEN_SIZE, bytes, sizeof(bytes));
}

void accountsFormatTag(UserId userId, char outTag[USER_TAG_SIZE]) {
    snprintf(outTag, USER_TAG_SIZE, "UNO#%llu", (unsigned long long)userId);
}

/* UserId gửi dạng chuỗi vì số 64 bit vượt độ chính xác của number trong JS. */
static void formatUserId(UserId userId, char outText[USER_ID_TEXT_SIZE]) {
    snprintf(outText, USER_ID_TEXT_SIZE, "%llu", (unsigned long long)userId);
}

/* Slot của kết nối nếu nó vẫn là đúng socket đó (cùng generation), ngược lại NULL. */
static AppConnSlot *liveSlot(App *app, ConnId connId) {
    if (connId.slot >= APP_MAX_CONNS) return NULL;
    AppConnSlot *slot = &app->slots[connId.slot];
    if (!slot->active || slot->generation != connId.generation) return NULL;
    return slot;
}

/**
 * @brief Gửi một phản hồi tới slot rồi giải phóng payload.
 *
 * Lỗi xếp hàng/gửi chỉ bật cờ closing của kết nối; vòng poll sẽ đóng nó sau, nên
 * ở đây không cần xử lý thêm.
 *
 * @param[in] payload Object JSON; hàm này chiếm quyền sở hữu.
 */
static void replyToSlot(AppConnSlot *slot, int32_t requestId, const char *type,
                        json_object *payload) {
    Envelope response = {PROTOCOL_VERSION, requestId, type, payload};
    Status queued = connQueue(&slot->conn, &response);
    json_object_put(payload);
    if (queued == OK) (void)connFlush(&slot->conn);
}

/* Trả ERROR {code, message} thẳng vào một slot đang hoạt động. */
static void sendAccountError(App *app, uint32_t slotIndex, int32_t requestId, const char *code,
                             const char *message) {
    if (slotIndex >= APP_MAX_CONNS || !app->slots[slotIndex].active) return;
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "code", json_object_new_string(code));
    json_object_object_add(payload, "message", json_object_new_string(message));
    replyToSlot(&app->slots[slotIndex], requestId, "ERROR", payload);
}

/* Object "me" công khai của tài khoản; không chứa email, hash hay token. */
static json_object *meJson(UserId userId, const char *name, const char *avatar, int elo) {
    char userIdText[USER_ID_TEXT_SIZE];
    char tagText[USER_TAG_SIZE];
    formatUserId(userId, userIdText);
    accountsFormatTag(userId, tagText);
    json_object *me = json_object_new_object();
    json_object_object_add(me, "userId", json_object_new_string(userIdText));
    json_object_object_add(me, "tag", json_object_new_string(tagText));
    json_object_object_add(me, "name", json_object_new_string(name));
    json_object_object_add(me, "avatar", json_object_new_string(avatar));
    json_object_object_add(me, "elo", json_object_new_int(elo));
    return me;
}

/**
 * @brief Gửi lại trạng thái cho socket vừa nhận phiên (LOGIN hoặc RESUME).
 *
 * Gọi sau phản hồi OK để client thấy phiên trước GAME_STATE. Socket mới chưa
 * từng nhận snapshot phòng, party hay hàng chờ, và cần FRIEND_STATE đầu tiên.
 */
static void syncUserOnNewSocket(App *app, UserId userId) {
    /* NOT_FOUND chỉ có nghĩa là user không ở trong trận nào. */
    (void)matchesConnected(app->matches, userId, true, appNowMs());
    roomsSyncUser(app, userId);
    queueSyncUser(app, userId);
    socialPresence(app, userId);
}

/* ------------------------------- Phiên ------------------------------- */

Status sessionsInit(App *app) {
    if (!app) return BAD_REQUEST;
    if (!app->sessions) {
        app->sessions = calloc(1, sizeof(Sessions));
        if (!app->sessions) return STORAGE_ERROR;
    }
    if (!app->rateLimiter) {
        app->rateLimiter = calloc(1, sizeof(RateLimiter));
        if (!app->rateLimiter) {
            free(app->sessions);
            app->sessions = NULL;
            return STORAGE_ERROR;
        }
    }
    return OK;
}

void sessionsCleanup(App *app) {
    if (!app) return;
    free(app->sessions);
    app->sessions = NULL;
    free(app->rateLimiter);
    app->rateLimiter = NULL;
}

/* Phiên đang dùng có gắn với đúng kết nối này (cùng slot và generation) không. */
static bool sessionOnConn(const SessionEntry *entry, ConnId connId) {
    return entry->inUse && entry->connId.slot == connId.slot &&
           entry->connId.generation == connId.generation;
}

/* Phiên đã rớt mạng tạo sớm nhất, dùng để nhường chỗ khi bảng phiên đầy. */
static SessionEntry *oldestDisconnectedSession(Sessions *sessions) {
    SessionEntry *oldest = NULL;
    uint64_t oldestMs = UINT64_MAX;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        SessionEntry *entry = &sessions->entries[i];
        if (!entry->connected && entry->createdAtMs < oldestMs) {
            oldestMs = entry->createdAtMs;
            oldest = entry;
        }
    }
    return oldest;
}

/* Ô phiên cho @p userId: phiên cũ của chính user, ô trống, hoặc phiên rớt mạng cũ nhất. */
static SessionEntry *findSessionSlot(Sessions *sessions, UserId userId) {
    SessionEntry *freeEntry = NULL;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        SessionEntry *entry = &sessions->entries[i];
        if (entry->inUse && entry->userId == userId) return entry;
        if (!entry->inUse && !freeEntry) freeEntry = entry;
    }
    return freeEntry ? freeEntry : oldestDisconnectedSession(sessions);
}

Status sessionsBind(App *app, ConnId connId, UserId userId, const char *token) {
    if (!app || !app->sessions || !token) return BAD_REQUEST;
    SessionEntry *entry = findSessionSlot(app->sessions, userId);
    if (!entry) return BUSY;

    entry->inUse = true;
    entry->userId = userId;
    snprintf(entry->token, sizeof(entry->token), "%s", token);
    entry->connId = connId;
    entry->connected = true;
    entry->createdAtMs = accountsGetNowMs();
    return OK;
}

Status sessionsUser(App *app, ConnId connId, UserId *outUserId) {
    if (!app || !app->sessions || !outUserId) return BAD_REQUEST;
    if (!liveSlot(app, connId)) return UNAUTHENTICATED;

    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        const SessionEntry *entry = &app->sessions->entries[i];
        if (entry->connected && sessionOnConn(entry, connId)) {
            *outUserId = entry->userId;
            return OK;
        }
    }
    return UNAUTHENTICATED;
}

Status sessionsFindConn(App *app, UserId userId, ConnId *outConn) {
    if (!app || !app->sessions || !outConn) return BAD_REQUEST;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        const SessionEntry *entry = &app->sessions->entries[i];
        if (entry->inUse && entry->connected && entry->userId == userId) {
            *outConn = entry->connId;
            return OK;
        }
    }
    return NOT_FOUND;
}

/* Phiên mà token RESUME trỏ tới, hoặc NULL; phiên hết hạn bị xóa luôn. */
static SessionEntry *findLiveToken(Sessions *sessions, const char *token) {
    if (!token || strlen(token) != TOKEN_HEX_LENGTH) return NULL;
    const uint64_t ttlMs = (uint64_t)SESSION_TTL_HOURS * MS_PER_HOUR;
    for (size_t index = 0; index < MAX_SESSIONS; index++) {
        SessionEntry *entry = &sessions->entries[index];
        if (!entry->inUse || strcmp(entry->token, token) != 0) continue;
        uint64_t nowMs = accountsGetNowMs();
        if (nowMs > entry->createdAtMs && nowMs - entry->createdAtMs > ttlMs) {
            memset(entry, 0, sizeof(*entry));
            return NULL;
        }
        return entry;
    }
    return NULL;
}

Status sessionsResume(App *app, ConnId connId, const char *token) {
    if (!app || !app->sessions || !token) return BAD_REQUEST;
    if (!liveSlot(app, connId)) return UNAUTHENTICATED;

    SessionEntry *entry = findLiveToken(app->sessions, token);
    if (!entry) return UNAUTHENTICATED;
    entry->connId = connId;
    entry->connected = true;
    return OK;
}

/* Mỗi socket chỉ mang một tài khoản. Gắn tài khoản khác thì tài khoản hiện tại
 * rời socket như khi LOGOUT, để ghế, phòng và party của nó được dọn ngay; nếu
 * không, hai tài khoản dùng chung socket và chỉ một bị dọn khi socket đóng. */
static void releaseOtherAccount(App *app, ConnId conn, UserId incoming) {
    UserId current = 0;
    if (sessionsUser(app, conn, &current) != OK || current == 0) return;
    if (current == incoming) return;
    appUserLeave(app, conn, current);
    sessionsLogout(app, conn);
}

void sessionsDisconnect(App *app, ConnId connId) {
    if (!app || !app->sessions) return;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        SessionEntry *entry = &app->sessions->entries[i];
        if (sessionOnConn(entry, connId)) entry->connected = false;
    }
}

void sessionsLogout(App *app, ConnId connId) {
    if (!app || !app->sessions) return;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        SessionEntry *entry = &app->sessions->entries[i];
        if (sessionOnConn(entry, connId)) memset(entry, 0, sizeof(*entry));
    }
}

/* ----------------------- Giới hạn tần suất theo IP ----------------------- */

/* Ô của @p ip trong bảng; chưa có thì lấy ô trống, bảng đầy thì thay IP cũ nhất. */
static IpRateEntry *claimRateEntry(RateLimiter *rateLimiter, const char *ip) {
    IpRateEntry *freeEntry = NULL;
    for (size_t i = 0; i < MAX_RATE_LIMIT_IPS; i++) {
        IpRateEntry *entry = &rateLimiter->entries[i];
        if (entry->ip[0] != '\0' && strcmp(entry->ip, ip) == 0) return entry;
        if (entry->ip[0] == '\0' && !freeEntry) freeEntry = entry;
    }
    if (!freeEntry) {
        uint64_t oldestMs = UINT64_MAX;
        for (size_t i = 0; i < MAX_RATE_LIMIT_IPS; i++) {
            if (rateLimiter->entries[i].lastSeenMs < oldestMs) {
                oldestMs = rateLimiter->entries[i].lastSeenMs;
                freeEntry = &rateLimiter->entries[i];
            }
        }
    }
    memset(freeEntry, 0, sizeof(*freeEntry));
    snprintf(freeEntry->ip, sizeof(freeEntry->ip), "%s", ip);
    return freeEntry;
}

/* Bỏ các lần thử nằm ngoài cửa sổ để chỉ phút gần nhất được tính. */
static void dropExpiredAttempts(IpRateEntry *entry, uint64_t nowMs) {
    size_t validCount = 0;
    for (size_t k = 0; k < entry->count; k++) {
        uint64_t attemptMs = entry->timestamps[k];
        if (nowMs >= attemptMs && nowMs - attemptMs < AUTH_RATE_WINDOW_MS) {
            entry->timestamps[validCount++] = attemptMs;
        }
    }
    entry->count = validCount;
}

bool accountsRateLimitCheck(App *app, const char *ip) {
    if (!app || !app->rateLimiter || !ip || ip[0] == '\0') return true;
    if (app->authLimit <= 0) return true;

    uint64_t nowMs = accountsGetNowMs();
    IpRateEntry *entry = claimRateEntry(app->rateLimiter, ip);
    dropExpiredAttempts(entry, nowMs);
    if (entry->count >= (size_t)app->authLimit) return false;

    if (entry->count < MAX_TIMESTAMPS_PER_IP) entry->timestamps[entry->count++] = nowMs;
    entry->lastSeenMs = nowMs;
    return true;
}

/* -------------------------- Xử lý envelope -------------------------- */

/**
 * @brief Lấy các trường chuỗi bắt buộc từ payload.
 * @param[out] outValues Nhận chuỗi; chỉ hợp lệ khi payload còn sống.
 * @return false khi payload NULL hoặc thiếu một trường bất kỳ.
 */
static bool readStringFields(json_object *payload, const char *const keys[], size_t count,
                             const char *outValues[]) {
    if (!payload) return false;
    for (size_t i = 0; i < count; i++) {
        json_object *value = NULL;
        if (!json_object_object_get_ex(payload, keys[i], &value)) return false;
        outValues[i] = json_object_get_string(value);
    }
    return true;
}

/* Điền phần chung của mọi job xác thực. */
static void prepareAuthJob(AuthJob *job, AuthJobKind kind, ConnId connId, int32_t requestId,
                           const char *email, const char *password) {
    memset(job, 0, sizeof(*job));
    job->kind = kind;
    job->connId = connId;
    job->reqId = requestId;
    snprintf(job->email, sizeof(job->email), "%s", email);
    snprintf(job->password, sizeof(job->password), "%s", password);
}

/* Đưa job sang worker rồi xóa mật khẩu khỏi bản trên stack của caller. */
static void submitAuthJob(App *app, ConnId connId, int32_t requestId, AuthJob *job) {
    Status submitted = authSubmit(app->authWorker, job);
    sodium_memzero(job->password, sizeof(job->password));
    if (submitted != OK) sendAccountError(app, connId.slot, requestId, "BUSY", "Server is busy");
}

static Status handleRegister(App *app, ConnId connId, const char *ip, const Envelope *envelope) {
    if (!accountsRateLimitCheck(app, ip)) {
        sendAccountError(app, connId.slot, envelope->id, "RATE_LIMITED", "Rate limit exceeded");
        return OK;
    }
    static const char *const KEYS[] = {"email", "password", "name", "avatar"};
    const char *values[ARRAY_LENGTH(KEYS)];
    if (!readStringFields(envelope->payload, KEYS, ARRAY_LENGTH(KEYS), values)) {
        sendAccountError(app, connId.slot, envelope->id, "BAD_REQUEST", "Missing required fields");
        return OK;
    }
    if (!accountsValidateEmail(values[FIELD_EMAIL]) ||
        !accountsValidatePassword(values[FIELD_PASSWORD]) ||
        !accountsValidateName(values[FIELD_NAME]) ||
        !accountsValidateAvatar(values[FIELD_AVATAR])) {
        sendAccountError(app, connId.slot, envelope->id, "BAD_REQUEST",
                         "Invalid registration input");
        return OK;
    }
    AuthJob job;
    prepareAuthJob(&job, AUTH_REGISTER, connId, envelope->id, values[FIELD_EMAIL],
                   values[FIELD_PASSWORD]);
    snprintf(job.name, sizeof(job.name), "%s", values[FIELD_NAME]);
    snprintf(job.avatar, sizeof(job.avatar), "%s", values[FIELD_AVATAR]);
    submitAuthJob(app, connId, envelope->id, &job);
    return OK;
}

static Status handleLogin(App *app, ConnId connId, const char *ip, const Envelope *envelope) {
    if (!accountsRateLimitCheck(app, ip)) {
        sendAccountError(app, connId.slot, envelope->id, "RATE_LIMITED", "Rate limit exceeded");
        return OK;
    }
    static const char *const KEYS[] = {"email", "password"};
    const char *values[ARRAY_LENGTH(KEYS)];
    if (!readStringFields(envelope->payload, KEYS, ARRAY_LENGTH(KEYS), values)) {
        sendAccountError(app, connId.slot, envelope->id, "BAD_REQUEST",
                         "Missing email or password");
        return OK;
    }
    if (!accountsValidateEmail(values[FIELD_EMAIL]) ||
        !accountsValidatePassword(values[FIELD_PASSWORD])) {
        sendAccountError(app, connId.slot, envelope->id, "BAD_REQUEST",
                         "Invalid login input format");
        return OK;
    }
    AuthJob job;
    prepareAuthJob(&job, AUTH_LOGIN, connId, envelope->id, values[FIELD_EMAIL],
                   values[FIELD_PASSWORD]);
    /* Tra tài khoản ở luồng poll (SQLite chỉ dùng từ luồng này); worker chỉ verify. */
    Account account;
    job.userExists = storeAccountGet(app->store, values[FIELD_EMAIL], &account) == OK;
    if (job.userExists) {
        snprintf(job.hash, sizeof(job.hash), "%s", account.hash);
        job.account = account;
    }
    submitAuthJob(app, connId, envelope->id, &job);
    return OK;
}

/* Phản hồi OK của RESUME. Client đã có token vừa gửi; gửi lại chỉ làm bí mật
 * xuất hiện thêm trên wire, nên payload chỉ có "me". */
static void replyResumed(App *app, AppConnSlot *slot, int32_t requestId, UserId userId) {
    Account account;
    memset(&account, 0, sizeof(account));
    if (storeAccountGetById(app->store, userId, &account) != OK) {
        /* Phiên vẫn hợp lệ; chỉ thiếu hồ sơ, client sẽ nhận tên/avatar rỗng. */
        fprintf(stderr, "accounts: profile lookup failed for user %llu\n",
                (unsigned long long)userId);
    }
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "me",
                           meJson(userId, account.name, account.avatar, account.elo));
    replyToSlot(slot, requestId, "OK", payload);
}

static Status handleResume(App *app, ConnId connId, AppConnSlot *slot,
                           const Envelope *envelope) {
    static const char *const KEYS[] = {"token"};
    const char *token = NULL;
    if (!readStringFields(envelope->payload, KEYS, ARRAY_LENGTH(KEYS), &token)) {
        sendAccountError(app, connId.slot, envelope->id, "BAD_REQUEST", "Missing token");
        return OK;
    }
    /* Tra token trước: token sai không được làm socket mất tài khoản đang có. */
    const SessionEntry *target = findLiveToken(app->sessions, token);
    if (target) releaseOtherAccount(app, connId, target->userId);
    UserId userId = 0;
    if (sessionsResume(app, connId, token) != OK || sessionsUser(app, connId, &userId) != OK) {
        sendAccountError(app, connId.slot, envelope->id, "UNAUTHENTICATED",
                         "Invalid or expired token");
        return OK;
    }
    replyResumed(app, slot, envelope->id, userId);
    syncUserOnNewSocket(app, userId);
    return OK;
}

static Status handleLogout(App *app, ConnId connId, AppConnSlot *slot,
                           const Envelope *envelope) {
    UserId userId = 0;
    /* Spec §5: ngoài trận, user rời hàng chờ, lời mời, party và phòng chờ ngay.
     * Việc này chạy khi phiên vẫn còn ánh xạ conn -> user, để người rời cũng
     * nhận snapshot cuối và bạn bè thấy tài khoản offline. */
    if (sessionsUser(app, connId, &userId) == OK && userId != 0) {
        appUserLeave(app, connId, userId);
    }
    sessionsLogout(app, connId);
    replyToSlot(slot, envelope->id, "OK", json_object_new_object());
    return OK;
}

Status accountsHandle(App *app, ConnId connId, const Envelope *envelope) {
    if (!app || !envelope) return BAD_REQUEST;
    AppConnSlot *slot = liveSlot(app, connId);
    if (!slot) return BAD_REQUEST;

    const char *type = envelope->type;
    if (strcmp(type, "REGISTER") == 0) return handleRegister(app, connId, slot->clientIp, envelope);
    if (strcmp(type, "LOGIN") == 0) return handleLogin(app, connId, slot->clientIp, envelope);
    if (strcmp(type, "RESUME") == 0) return handleResume(app, connId, slot, envelope);
    if (strcmp(type, "LOGOUT") == 0) return handleLogout(app, connId, slot, envelope);
    return BAD_REQUEST;
}

/* --------------------- Nhận kết quả từ auth worker --------------------- */

/**
 * @brief Đọc một bản ghi kết quả từ pipe non-blocking của worker.
 *
 * Worker ghi nguyên bản ghi, mỗi bản ghi nhỏ hơn PIPE_BUF, nên read() trên pipe
 * không bao giờ trả về nửa bản ghi.
 *
 * @return true khi đọc được một bản ghi; false khi pipe đã cạn (EAGAIN), đầu ghi
 *         đã đóng (read() trả 0) hoặc read() lỗi.
 */
static bool readAuthResult(int pipeFd, AuthResult *outResult) {
    while (1) {
        ssize_t bytesRead = read(pipeFd, outResult, sizeof(*outResult));
        if (bytesRead == (ssize_t)sizeof(*outResult)) return true;
        if (bytesRead < 0 && errno == EINTR) continue;
        if (bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK) perror("authDrain: read");
        return false;
    }
}

static void finishRegister(App *app, AppConnSlot *slot, const AuthResult *result) {
    uint32_t slotIndex = result->connId.slot;
    if (result->status != OK) {
        sendAccountError(app, slotIndex, result->reqId, "STORAGE_ERROR", "Password hashing failed");
        return;
    }
    AccountCreate accountCreate;
    memset(&accountCreate, 0, sizeof(accountCreate));
    snprintf(accountCreate.email, sizeof(accountCreate.email), "%s", result->account.email);
    snprintf(accountCreate.hash, sizeof(accountCreate.hash), "%s", result->account.hash);
    snprintf(accountCreate.name, sizeof(accountCreate.name), "%s", result->account.name);
    snprintf(accountCreate.avatar, sizeof(accountCreate.avatar), "%s", result->account.avatar);

    UserId newUserId = 0;
    Status status = storeAccountCreate(app->store, &accountCreate, &newUserId);
    if (status == BAD_REQUEST) {
        sendAccountError(app, slotIndex, result->reqId, "BAD_REQUEST", "Email already registered");
    } else if (status != OK) {
        sendAccountError(app, slotIndex, result->reqId, "STORAGE_ERROR",
                         "Database storage failure");
    } else {
        char userIdText[USER_ID_TEXT_SIZE];
        formatUserId(newUserId, userIdText);
        json_object *payload = json_object_new_object();
        json_object_object_add(payload, "userId", json_object_new_string(userIdText));
        replyToSlot(slot, result->reqId, "OK", payload);
    }
}

/* Tài khoản đăng nhập lại từ socket khác: tách phiên trước để appCloseSlot thấy
 * socket cũ là vô danh. User đang chuyển chỗ chứ không rời đi, nên không phát
 * offline (vốn sẽ hủy trận khi không còn người khác) và không rời phòng/party. */
static void closePreviousSocket(App *app, const AuthResult *result) {
    ConnId previousConn;
    if (sessionsFindConn(app, result->userId, &previousConn) != OK) return;
    bool otherSocket = previousConn.slot != result->connId.slot;
    if (otherSocket && previousConn.slot < APP_MAX_CONNS) {
        sessionsDisconnect(app, previousConn);
        appCloseSlot(app, (int)previousConn.slot);
    }
}

static void finishLogin(App *app, AppConnSlot *slot, const AuthResult *result) {
    if (result->status != OK) {
        sendAccountError(app, result->connId.slot, result->reqId, "UNAUTHENTICATED",
                         "Invalid email or password");
        return;
    }
    releaseOtherAccount(app, result->connId, result->userId);
    closePreviousSocket(app, result);
    if (sessionsBind(app, result->connId, result->userId, result->token) != OK) {
        sendAccountError(app, result->connId.slot, result->reqId, "BUSY", "Server is busy");
        return;
    }
    const Account *account = &result->account;
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "token", json_object_new_string(result->token));
    json_object_object_add(payload, "me",
                           meJson(result->userId, account->name, account->avatar, account->elo));
    replyToSlot(slot, result->reqId, "OK", payload);
    /* Đăng nhập lại giành lại ghế; nếu ghế chưa từng offline, coordinator vẫn
     * gửi view mới cho kết nối này. */
    syncUserOnNewSocket(app, result->userId);
}

void authDrain(App *app) {
    if (!app || app->authPipeRead < 0) return;

    AuthResult result;
    while (readAuthResult(app->authPipeRead, &result)) {
        /* Client đã ngắt hoặc slot đã được dùng lại: bỏ qua kết quả cũ. */
        AppConnSlot *slot = liveSlot(app, result.connId);
        if (!slot) continue;
        if (result.kind == AUTH_REGISTER) {
            finishRegister(app, slot, &result);
        } else if (result.kind == AUTH_LOGIN) {
            finishLogin(app, slot, &result);
        }
    }
}
