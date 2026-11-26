#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "server/auth/accounts.h"
#include "server/auth/auth_worker.h"
#include "server/game/match.h"
#include "server/lobby/queue.h"
#include "server/lobby/rooms.h"
#include "server/lobby/social.h"
#include "server/storage/store.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
/* Nếu hệ thống thiếu cờ này, appInit vẫn bỏ qua SIGPIPE cho cả tiến trình. */
#define MSG_NOSIGNAL 0
#endif

/* sig_atomic_t cho phép bộ xử lý tín hiệu đặt cờ để vòng lặp đọc an toàn. */
static volatile sig_atomic_t gStopRequested = 0;

/* SIGINT và SIGTERM chỉ đặt cờ để việc thu hồi tài nguyên diễn ra ngoài
 * bộ xử lý tín hiệu. */
static void handleStopSignal(int signalNumber) {
    (void)signalNumber;
    gStopRequested = 1;
}

/* Đính kèm generation để onFrame loại frame thuộc kết nối cũ đã mất ô. */
typedef struct {
    App *app;
    uint32_t slotIndex;
    uint32_t generation;
} FrameContext;

enum {
    MS_PER_SECOND = 1000,
    NS_PER_MS = 1000000,
    /* Giới hạn hàng chờ kết nối mà hệ điều hành dùng trước accept(). */
    LISTEN_BACKLOG = 128,
    /* Giới hạn thời gian chờ poll để lượt và bot vẫn chạy khi mạng im lặng. */
    POLL_TIMEOUT_MS = 50,
    /* Bộ đệm riêng cho frame lỗi khi không thể cấp ô kết nối. */
    SERVER_FULL_FRAME_SIZE = 256
};

/* Mỗi kết nối hồi 30 token/giây để chặn gửi liên tục quá hạn mức. */
static const double RATE_TOKENS_PER_MS = 0.03;
static const double RATE_BURST_TOKENS = 60.0;

/* Các lệnh trận được xử lý với danh tính lấy từ phiên đã xác thực. */
static const char *const MATCH_COMMANDS[] = {
    "PLAY", "DRAW", "PASS", "SAY_UNO", "DRAFT_PICK", "MATCH_LEAVE", "CHAT"
};

static const char *const ROOM_COMMANDS[] = {
    "ROOM_CREATE", "ROOM_JOIN", "ROOM_LEAVE", "ROOM_BOT", "ROOM_KICK", "ROOM_MODE",
    "ROOM_START", "INVITE_SEND", "INVITE_REPLY", "PARTY_LEAVE"
};

static const char *const SOCIAL_COMMANDS[] = {
    "FRIEND_LIST", "FRIEND_REQUEST", "FRIEND_REPLY", "PROFILE_GET", "PROFILE_UPDATE",
    "LEADERBOARD_GET", "LOBBY_GET"
};

enum {
    /* Chỗ cho người ra lệnh, người trong phòng, thành viên nhóm và người mời. */
    PRESENCE_WATCH_MAX = 1 + ROOM_MAX_SEATS + PARTY_MAX_MEMBERS + 1
};

/* Chụp trạng thái trước lệnh để chỉ báo cho bạn bè khi có thay đổi. */
typedef struct {
    UserId users[PRESENCE_WATCH_MAX];
    Presence before[PRESENCE_WATCH_MAX];
    size_t count;
} PresenceWatch;

uint64_t appNowMs(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * MS_PER_SECOND + (uint64_t)(now.tv_nsec / NS_PER_MS);
}

static const char *statusMessage(Status status) {
    switch (status) {
    case BAD_REQUEST: return "Malformed or unsupported request";
    case UNAUTHENTICATED: return "Authentication required";
    case FORBIDDEN: return "Not allowed";
    case BUSY: return "Resource busy";
    case NOT_FOUND: return "Not found";
    case ROOM_FULL: return "Room is full";
    case INVALID_ACTION: return "Action not allowed now";
    case NOT_YOUR_TURN: return "Not your turn";
    case STALE_TURN: return "Turn already advanced";
    case STALE_REQUEST: return "Request id must increase";
    case RATE_LIMITED: return "Too many requests";
    case STORAGE_ERROR: return "Storage unavailable";
    case SERVER_FULL: return "Server is full";
    default: return "Request failed";
    }
}

/* So generation để kết quả xử lý muộn không bị gửi cho socket dùng lại ô. */
static AppConnSlot *liveSlot(App *app, ConnId conn) {
    if (conn.slot >= APP_MAX_CONNS) return NULL;
    AppConnSlot *slot = &app->slots[conn.slot];
    if (!slot->active || slot->generation != conn.generation) return NULL;
    return slot;
}

/* Giữ phần gửi thiếu trong bộ đệm đến POLLOUT. Lỗi I/O đánh dấu kết nối
 * để vòng lặp dọn sau, tránh đóng ô giữa lúc đang xử lý yêu cầu. */
static Status sendEnvelope(AppConnSlot *slot, const Envelope *envelope) {
    Status queued = connQueue(&slot->conn, envelope);
    if (queued != OK) return queued;
    return connFlush(&slot->conn);
}

Status appPush(App *app, UserId user, const char *type, json_object *payload) {
    if (!app || !user || !type || !payload) return BAD_REQUEST;
    ConnId conn;
    if (sessionsFindConn(app, user, &conn) != OK) return NOT_FOUND;
    AppConnSlot *slot = liveSlot(app, conn);
    if (!slot) return NOT_FOUND;
    /* Giao thức dành id 0 cho thông báo chủ động để client phân biệt với phản hồi. */
    Envelope push = {1, 0, type, payload};
    return sendEnvelope(slot, &push);
}

void appReplyStatus(App *app, ConnId conn, int32_t id, Status status) {
    if (!app) return;
    AppConnSlot *slot = liveSlot(app, conn);
    if (!slot) return;
    json_object *payload = json_object_new_object();
    if (!payload) return;
    if (status != OK) {
        json_object_object_add(payload, "code", json_object_new_string(statusCode(status)));
        json_object_object_add(payload, "message",
                               json_object_new_string(statusMessage(status)));
    }
    Envelope reply = {1, id, status == OK ? "OK" : "ERROR", payload};
    /* Lỗi I/O sẽ được vòng lặp xử lý qua cờ closing. */
    (void)sendEnvelope(slot, &reply);
    json_object_put(payload);
}

void appReplyOk(App *app, ConnId conn, int32_t id, json_object *payload) {
    if (!app || !payload) return;
    AppConnSlot *slot = liveSlot(app, conn);
    if (!slot) return;
    Envelope reply = {1, id, "OK", payload};
    (void)sendEnvelope(slot, &reply);
}

static void onMatchPush(void *context, UserId userId, const char *type,
                           json_object *payload) {
    /* Ghế mất kết nối nhận lại trạng thái hiện tại qua RESUME. */
    (void)appPush((App *)context, userId, type, payload);
}

static bool recordInMatch(const App *app, const AppMatchRecord *record) {
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (matchesContains(app->matches, record->users[seat])) return true;
    }
    return false;
}

static void recordMatch(App *app, const RoomStart *roomStart) {
    AppMatchRecord *slot = NULL;
    for (int index = 0; index < APP_MATCH_RECORDS && !slot; index++) {
        if (!app->matchRecords[index].active) slot = &app->matchRecords[index];
    }
    /* Nếu hết bản ghi trống, có thể dùng lại bản ghi của trận đã kết thúc. */
    for (int index = 0; index < APP_MATCH_RECORDS && !slot; index++) {
        if (!recordInMatch(app, &app->matchRecords[index])) {
            slot = &app->matchRecords[index];
        }
    }
    if (!slot) return;
    memset(slot, 0, sizeof *slot);
    slot->active = true;
    memcpy(slot->roomId, roomStart->roomId, sizeof slot->roomId);
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (!roomStart->players[seat].isBot) slot->users[seat] = roomStart->players[seat].userId;
    }
}

static void onMatchEnded(void *context, const char *roomId) {
    App *app = (App *)context;
    /* Bộ điều phối đã nhả trận để các thành viên có thể vào phòng ngay. */
    roomsMatchEnded(app, roomId);
    for (int index = 0; index < APP_MATCH_RECORDS; index++) {
        AppMatchRecord *record = &app->matchRecords[index];
        /* Giữ bản ghi còn tài khoản trong trận vì mã phòng có thể được tái sử dụng. */
        if (!record->active || strcmp(record->roomId, roomId) != 0 ||
            recordInMatch(app, record)) {
            continue;
        }
        record->active = false;
        /* Bỏ qua cập nhật hiện diện nếu máy chủ đang đóng tất cả kết nối. */
        if (!app->running) continue;
        for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
            UserId seated = record->users[seat];
            /* Phiên người đang rời vẫn còn gắn với socket. Đợi appUserLeave
             * ngắt phiên và gửi trạng thái offline để tránh báo online thoáng qua. */
            if (seated && seated != app->leavingUser) socialPresence(app, seated);
        }
    }
}

static Status onMatchStart(App *app, const RoomStart *roomStart, MatchId *outMatchId) {
    if (!app || !app->matches) return BUSY;
    Status started = matchesStart(app->matches, roomStart, appNowMs(), outMatchId);
    if (started != OK) return started;
    recordMatch(app, roomStart);
    /* Cả phòng và hàng chờ báo in_match tại đây để bạn bè không nhận tin trùng. */
    for (int seat = 0; seat < ROOM_MAX_SEATS; seat++) {
        if (!roomStart->players[seat].isBot) socialPresence(app, roomStart->players[seat].userId);
    }
    return OK;
}

static bool inList(const char *type, const char *const list[], size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(type, list[index]) == 0) return true;
    }
    return false;
}

static bool isMatchCommand(const char *type) {
    return inList(type, MATCH_COMMANDS, sizeof MATCH_COMMANDS / sizeof MATCH_COMMANDS[0]);
}

static bool isRoomCommand(const char *type) {
    return inList(type, ROOM_COMMANDS, sizeof ROOM_COMMANDS / sizeof ROOM_COMMANDS[0]);
}

static bool isSocialCommand(const char *type) {
    return inList(type, SOCIAL_COMMANDS, sizeof SOCIAL_COMMANDS / sizeof SOCIAL_COMMANDS[0]);
}

static void watchAdd(App *app, PresenceWatch *watch, UserId user) {
    if (user == 0 || watch->count >= PRESENCE_WATCH_MAX) return;
    for (size_t index = 0; index < watch->count; index++) {
        if (watch->users[index] == user) return;
    }
    watch->users[watch->count] = user;
    watch->before[watch->count] = socialPresenceOf(app, user);
    watch->count++;
}

/* Theo dõi người ra lệnh và những người có thể đổi trạng thái khi phòng
 * đổi thành viên, nhóm giải tán hoặc vào hàng chờ. */
static void watchBegin(App *app, PresenceWatch *watch, UserId actor) {
    memset(watch, 0, sizeof *watch);
    watchAdd(app, watch, actor);
    const Room *room = roomsFindByUser(app->rooms, actor);
    for (int seat = 0; room && seat < ROOM_MAX_SEATS; seat++) {
        if (room->seats[seat].occupied && !room->seats[seat].isBot) {
            watchAdd(app, watch, room->seats[seat].userId);
        }
    }
    const Party *party = partyFindByUser(app->rooms, actor);
    for (size_t member = 0; party && member < party->memberCount; member++) {
        watchAdd(app, watch, party->members[member]);
    }
}

/* Chấp nhận lời mời vào nhóm có thể đổi cả trạng thái của người mời. */
static void watchInviter(App *app, PresenceWatch *watch, const Envelope *envelope) {
    json_object *idObj = NULL;
    if (!envelope->payload || !json_object_object_get_ex(envelope->payload, "inviteId", &idObj) ||
        !json_object_is_type(idObj, json_type_string)) {
        return;
    }
    const char *inviteId = json_object_get_string(idObj);
    for (int index = 0; index < MAX_INVITES; index++) {
        const Invite *invite = &app->rooms->invites[index];
        if (invite->active && strcmp(invite->inviteId, inviteId) == 0) {
            watchAdd(app, watch, invite->senderId);
            return;
        }
    }
}

static void watchEnd(App *app, PresenceWatch *watch) {
    for (size_t index = 0; index < watch->count; index++) {
        Presence now = socialPresenceOf(app, watch->users[index]);
        /* onMatchStart đã báo in_match cho bạn bè, bỏ qua để tránh gửi trùng. */
        if (now != watch->before[index] && now != PRESENCE_IN_MATCH) {
            socialPresence(app, watch->users[index]);
        }
    }
}

/* Bao quanh lệnh phòng hoặc hàng chờ để so trạng thái trước và sau xử lý. */
static Status dispatchWatched(App *app, ConnId conn, const Envelope *envelope, bool roomCommand) {
    PresenceWatch watch;
    UserId actor = 0;
    memset(&watch, 0, sizeof watch);
    if (app->rooms && sessionsUser(app, conn, &actor) == OK && actor != 0) {
        watchBegin(app, &watch, actor);
        if (strcmp(envelope->type, "INVITE_REPLY") == 0) watchInviter(app, &watch, envelope);
    }
    Status result = roomCommand ? roomsHandle(app, conn, envelope)
                                : queueHandle(app, conn, envelope);
    watchEnd(app, &watch);
    return result;
}

bool appConnCheckRateLimit(AppConnSlot *slot, uint64_t nowMs) {
    if (!slot) return false;
    if (nowMs > slot->rateLastMs) {
        uint64_t elapsed = nowMs - slot->rateLastMs;
        slot->rateTokens += (double)elapsed * RATE_TOKENS_PER_MS;
        if (slot->rateTokens > RATE_BURST_TOKENS) {
            slot->rateTokens = RATE_BURST_TOKENS;
        }
        slot->rateLastMs = nowMs;
    }
    if (slot->rateTokens < 1.0) {
        return false;
    }
    slot->rateTokens -= 1.0;
    return true;
}

void appUserLeave(App *app, ConnId conn, UserId user) {
    if (!app || user == 0) return;
    PresenceWatch watch;
    memset(&watch, 0, sizeof watch);
    /* Không cần chụp trạng thái hiện diện khi máy chủ đang đóng toàn bộ kết nối. */
    if (app->running) watchBegin(app, &watch, user);
    uint64_t nowMs = appNowMs();
    /* Báo mất kết nối cho trận khi phiên còn gắn. Ghế trong trận đang chạy
     * được giữ để người chơi vào lại qua RESUME hoặc LOGIN. */
    app->leavingUser = user;
    (void)matchesConnected(app->matches, user, false, nowMs);
    app->leavingUser = 0;
    /* Rút cả nhóm khỏi hàng chờ khi còn tra được thành viên, trước khi giải tán. */
    queueUserGone(app, user);
    roomsDisconnect(app, user, nowMs);
    sessionsDisconnect(app, conn);
    /* Đợi ngắt phiên để trạng thái gửi cho bạn bè phản ánh việc mất kết nối. */
    watchEnd(app, &watch);
}

void appCloseSlot(App *app, int slotIndex) {
    if (slotIndex < 0 || slotIndex >= APP_MAX_CONNS) return;
    AppConnSlot *slot = &app->slots[slotIndex];
    if (!slot->active) return;
    ConnId conn = {(uint32_t)slotIndex, slot->generation};
    UserId userId = 0;
    if (sessionsUser(app, conn, &userId) == OK && userId != 0) {
        appUserLeave(app, conn, userId);
    }
    connClose(&slot->conn);
    slot->active = false;
    slot->generation++;
    slot->clientIp[0] = '\0';
    slot->rateLastMs = 0;
    slot->rateTokens = 0.0;
    slot->lastRequestId = 0;
    slot->lastFrameMs = 0;
}

/**
 * @brief Kiểm tra kết nối và hạn mức trước khi chuyển yêu cầu đến mô-đun xử lý.
 * @param[in] envelope Frame đã được giải mã.
 * @param[in] context Ngữ cảnh FrameContext chứa ô, generation và trạng thái máy chủ.
 * @return OK nếu vòng đọc có thể tiếp tục, kể cả khi đã trả một lỗi ứng dụng.
 *         Mã khác được chuyển cho connRead để đánh dấu đóng kết nối.
 */
static Status onFrame(const Envelope *envelope, void *context) {
    FrameContext *frameContext = (FrameContext *)context;
    App *app = frameContext->app;
    if (frameContext->slotIndex >= APP_MAX_CONNS) return BAD_REQUEST;
    AppConnSlot *slot = &app->slots[frameContext->slotIndex];
    if (!slot->active || slot->generation != frameContext->generation) {
        return BAD_REQUEST;
    }

    ConnId conn = {frameContext->slotIndex, frameContext->generation};
    uint64_t nowMs = appNowMs();
    if (!appConnCheckRateLimit(slot, nowMs)) {
        appReplyStatus(app, conn, envelope->id, RATE_LIMITED);
        return OK;
    }

    /* Từ chối id lặp hoặc nhỏ hơn id trước để tránh chạy lại yêu cầu cũ. */
    if (envelope->id <= slot->lastRequestId) {
        appReplyStatus(app, conn, envelope->id, STALE_REQUEST);
        return OK;
    }
    slot->lastRequestId = envelope->id;
    slot->lastFrameMs = nowMs;

    /* Xử lý lượt hết hạn trước để không nhận lệnh cho lượt đã hết giờ. */
    matchesTick(app->matches, nowMs);

    if (strcmp(envelope->type, "HELLO") == 0) {
        /* HELLO cần giờ thực để client hiển thị mốc thời gian. Bộ đếm thời hạn
         * nội bộ dùng CLOCK_MONOTONIC để không nhảy khi hệ thống chỉnh giờ. */
        struct timespec wallClock;
        clock_gettime(CLOCK_REALTIME, &wallClock);
        uint64_t serverTimeMs = (uint64_t)wallClock.tv_sec * MS_PER_SECOND +
                                (uint64_t)(wallClock.tv_nsec / NS_PER_MS);

        struct json_object *payload = json_object_new_object();
        json_object_object_add(payload, "serverTimeMs", json_object_new_int64(serverTimeMs));

        Envelope response = {1, envelope->id, "OK", payload};
        Status status = connQueue(&slot->conn, &response);
        json_object_put(payload);
        if (status != OK) return status;
        return connFlush(&slot->conn);
    }

    if (strcmp(envelope->type, "PING") == 0) {
        struct json_object *payload = json_object_new_object();
        Envelope response = {1, envelope->id, "OK", payload};
        Status status = connQueue(&slot->conn, &response);
        json_object_put(payload);
        if (status != OK) return status;
        return connFlush(&slot->conn);
    }

    if (strcmp(envelope->type, "REGISTER") == 0 ||
        strcmp(envelope->type, "LOGIN") == 0 ||
        strcmp(envelope->type, "RESUME") == 0 ||
        strcmp(envelope->type, "LOGOUT") == 0) {
        return accountsHandle(app, conn, envelope);
    }

    if (isRoomCommand(envelope->type)) {
        return dispatchWatched(app, conn, envelope, true);
    }

    if (strcmp(envelope->type, "QUEUE_JOIN") == 0 || strcmp(envelope->type, "QUEUE_LEAVE") == 0) {
        return dispatchWatched(app, conn, envelope, false);
    }

    if (isSocialCommand(envelope->type)) {
        return socialHandle(app, conn, envelope);
    }

    if (isMatchCommand(envelope->type)) {
        UserId userId = 0;
        if (sessionsUser(app, conn, &userId) != OK) {
            appReplyStatus(app, conn, envelope->id, UNAUTHENTICATED);
            return OK;
        }
        /* matchesHandle có thể gửi GAME_STATE trước phản hồi mang id yêu cầu. */
        Status result = matchesHandle(app->matches, userId, envelope, nowMs);
        appReplyStatus(app, conn, envelope->id, result);
        return OK;
    }

    /* TIMEOUT và UNO_PENALTY do máy chủ sinh ra. Yêu cầu kiểu này chỉ nhận
     * BAD_REQUEST để client vẫn có thể gửi lệnh hợp lệ tiếp theo. */
    appReplyStatus(app, conn, envelope->id, BAD_REQUEST);
    return OK;
}

Status appInit(App *app, const char *bindIp, uint16_t port, const char *dbPath, int authLimit) {
    if (!app) return BAD_REQUEST;
    memset(app, 0, sizeof(*app));
    app->listenFd = -1;
    app->authPipeRead = -1;
    app->authPipeWrite = -1;
    app->running = false;
    app->authLimit = authLimit;
    app->heartbeatMs = APP_DEFAULT_HEARTBEAT_MS;

    if (bindIp) {
        strncpy(app->bindIp, bindIp, sizeof(app->bindIp) - 1);
    } else {
        strcpy(app->bindIp, "0.0.0.0");
    }

    if (dbPath) {
        strncpy(app->dbPath, dbPath, sizeof(app->dbPath) - 1);
    } else {
        strcpy(app->dbPath, "uno.db");
    }

    sessionsInit(app);
    roomsInit(&app->rooms, app, onMatchStart);
    Status queueStatus = queueInit(&app->queue);
    if (queueStatus != OK) return queueStatus;

    /* Luồng xác thực ghi bản ghi AuthResult vào pipe để đánh thức poll().
     * Hai đầu dùng chế độ không chặn để một luồng không phải chờ luồng kia.
     * FIXME: Kiểm tra lỗi tạo pipe, đặt cờ và khởi tạo worker. Dọn tài nguyên
     * rồi báo lỗi khởi động nếu không thiết lập được luồng xác thực. */
    int authPipe[2];
    if (pipe(authPipe) == 0) {
        int flags = fcntl(authPipe[0], F_GETFL, 0);
        if (flags >= 0) fcntl(authPipe[0], F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(authPipe[1], F_GETFL, 0);
        if (flags >= 0) fcntl(authPipe[1], F_SETFL, flags | O_NONBLOCK);

        app->authPipeRead = authPipe[0];
        app->authPipeWrite = authPipe[1];

        authWorkerInit(&app->authWorker, app->authPipeWrite);
    }

    /* Bỏ qua SIGPIPE để mất kết nối trong lúc gửi không làm dừng cả máy chủ. */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction stopAction;
    memset(&stopAction, 0, sizeof(stopAction));
    stopAction.sa_handler = handleStopSignal;
    sigaction(SIGINT, &stopAction, NULL);
    sigaction(SIGTERM, &stopAction, NULL);

    // Bước 1. Tạo socket TCP/IPv4 dành riêng để nhận kết nối mới.
    int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket < 0) return BUSY;

    /* SO_REUSEADDR cho phép mở lại cổng khi kết nối cũ còn ở TIME_WAIT.
     * FIXME: Kiểm tra lỗi setsockopt và fcntl. Đóng socket và trả BUSY
     * nếu cấu hình lỗi, vì socket chặn có thể làm treo vòng accept. */
    int reuseAddress = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

    /* Dùng chế độ không chặn để accept() dừng ở EAGAIN khi hết hàng chờ. */
    int flags = fcntl(listenSocket, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(listenSocket, F_SETFL, flags | O_NONBLOCK);
    }

    // Bước 2. Gắn địa chỉ lắng nghe. htons() đổi cổng sang thứ tự byte mạng.
    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    if (inet_pton(AF_INET, app->bindIp, &serverAddress.sin_addr) <= 0) {
        close(listenSocket);
        return BAD_REQUEST;
    }

    if (bind(listenSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) < 0) {
        close(listenSocket);
        return BUSY;
    }

    // Bước 3. Bật lắng nghe để hệ điều hành xếp hàng kết nối cho appRun.
    if (listen(listenSocket, LISTEN_BACKLOG) < 0) {
        close(listenSocket);
        return BUSY;
    }

    /* Nếu truyền cổng 0, hệ điều hành chọn cổng. Đọc lại bằng getsockname()
     * rồi dùng ntohs() để appRun in số cổng mà kiểm thử có thể kết nối. */
    struct sockaddr_in boundAddress;
    socklen_t addressLength = sizeof(boundAddress);
    if (getsockname(listenSocket, (struct sockaddr *)&boundAddress, &addressLength) == 0) {
        app->boundPort = ntohs(boundAddress.sin_port);
    } else {
        app->boundPort = port;
    }

    app->listenFd = listenSocket;
    app->running = true;
    return OK;
}

void appStop(App *app) {
    if (app) app->running = false;
}

void appCleanup(App *app) {
    if (!app) return;
    /* Tắt cập nhật hiện diện trước khi appCloseSlot ngắt các phiên. */
    app->running = false;
    for (int i = 0; i < APP_MAX_CONNS; i++) {
        if (app->slots[i].active) {
            appCloseSlot(app, i);
        }
    }
    if (app->listenFd >= 0) {
        close(app->listenFd);
        app->listenFd = -1;
    }
    /* Dừng luồng xác thực trước khi đóng pipe mà nó có thể còn ghi vào. */
    if (app->authWorker) {
        authWorkerDestroy(app->authWorker);
        app->authWorker = NULL;
    }
    if (app->authPipeRead >= 0) {
        close(app->authPipeRead);
        app->authPipeRead = -1;
    }
    if (app->authPipeWrite >= 0) {
        close(app->authPipeWrite);
        app->authPipeWrite = -1;
    }
    /* Giữ hàng chờ và phòng đến khi xử lý xong mọi lượt ngắt phiên. */
    queueDestroy(app->queue);
    app->queue = NULL;
    if (app->rooms) {
        roomsDestroy(app->rooms);
        app->rooms = NULL;
    }
    sessionsCleanup(app);
    /* Bộ điều phối mượn kho dữ liệu nên cần giải phóng trước khi đóng kho. */
    matchesDestroy(app->matches);
    app->matches = NULL;
    if (app->store) {
        storeClose(app->store);
        app->store = NULL;
    }
    app->running = false;
}

Status appRejectConnection(int connectionSocket) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "code", json_object_new_string("SERVER_FULL"));
    const char *fullMessage = "Server connection limit reached";
    json_object_object_add(payload, "message", json_object_new_string(fullMessage));
    Envelope fullEnvelope = {1, 0, "ERROR", payload};
    uint8_t fullFrame[SERVER_FULL_FRAME_SIZE];
    size_t frameLength = 0;
    Status status = frameEncode(&fullEnvelope, fullFrame, sizeof(fullFrame), &frameLength);
    json_object_put(payload);

    if (status == OK) {
        /* Socket chưa qua connInit nên trên Linux vẫn ở chế độ chặn. Chỉ thử
         * gửi một lần cho kết nối bị từ chối, kể cả khi gửi thiếu byte. */
        ssize_t bytesSent = send(connectionSocket, fullFrame, frameLength, MSG_NOSIGNAL);
        if (bytesSent < 0) {
            fprintf(stderr, "SERVER_FULL reject: send failed: %s\n", strerror(errno));
            status = BAD_REQUEST;
        } else if ((size_t)bytesSent < frameLength) {
            fprintf(stderr, "SERVER_FULL reject: sent %zd of %zu bytes\n", bytesSent, frameLength);
            status = BAD_REQUEST;
        }
    } else {
        status = BAD_REQUEST;
    }
    close(connectionSocket);
    return status;
}

/* Báo SERVER_SHUTDOWN trước khi đóng socket để client biết lý do mất kết nối. */
static void announceShutdown(App *app) {
    json_object *payload = json_object_new_object();
    if (!payload) return;
    for (int index = 0; index < APP_MAX_CONNS; index++) {
        AppConnSlot *slot = &app->slots[index];
        UserId userId = 0;
        if (!slot->active) continue;
        ConnId conn = {(uint32_t)index, slot->generation};
        if (sessionsUser(app, conn, &userId) == OK && userId != 0) {
            (void)appPush(app, userId, "SERVER_SHUTDOWN", payload);
        }
    }
    json_object_put(payload);
}

/* Thời hạn heartbeat tính từ frame được chấp nhận gần nhất, kể cả ngoài PING. */
static void sweepIdleConnections(App *app, uint64_t nowMs) {
    uint64_t limitMs = (uint64_t)app->heartbeatMs;
    for (int index = 0; index < APP_MAX_CONNS; index++) {
        AppConnSlot *slot = &app->slots[index];
        if (!slot->active || nowMs < slot->lastFrameMs) continue;
        if (nowMs - slot->lastFrameMs >= limitMs) appCloseSlot(app, index);
    }
}

int appRun(const char *bindIp, uint16_t port, const char *dbPath, int authLimit,
            int heartbeatMs) {
    App *app = calloc(1, sizeof(App));
    if (!app) {
        fprintf(stderr, "Failed to allocate App memory\n");
        return 1;
    }

    Status status = appInit(app, bindIp, port, dbPath, authLimit);
    if (status != OK) {
        fprintf(stderr, "Failed to initialize server (error %d)\n", status);
        free(app);
        return 1;
    }
    if (heartbeatMs > 0) app->heartbeatMs = heartbeatMs;

    if (!app->store) {
        status = storeOpen(&app->store, app->dbPath);
        if (status != OK) {
            fprintf(stderr, "Failed to open database at %s (error %d)\n", app->dbPath, status);
            appCleanup(app);
            free(app);
            return 1;
        }
    }

    app->matches = matchesCreate(app->store, onMatchPush, onMatchEnded, app);
    if (!app->matches) {
        fprintf(stderr, "Failed to create match coordinator\n");
        appCleanup(app);
        free(app);
        return 1;
    }

    printf("PORT %u\n", app->boundPort);
    fflush(stdout);

    struct pollfd pollFds[2 + APP_MAX_CONNS];
    int slotByPollIndex[2 + APP_MAX_CONNS];

    while (app->running && !gStopRequested) {
        /* Chạy bộ đếm lượt và bot theo thời gian, kể cả khi không có dữ liệu mạng. */
        uint64_t tickMs = appNowMs();
        matchesTick(app->matches, tickMs);
        /* Hàng chờ thử mở lại trận bị lỗi và loại người đã rời phiên. */
        queueTick(app, tickMs);

        pollFds[0].fd = app->listenFd;
        pollFds[0].events = POLLIN;
        pollFds[0].revents = 0;

        pollFds[1].fd = app->authPipeRead;
        pollFds[1].events = (app->authPipeRead >= 0) ? POLLIN : 0;
        pollFds[1].revents = 0;

        int pollCount = 2;

        for (int i = 0; i < APP_MAX_CONNS; i++) {
            if (app->slots[i].active && app->slots[i].conn.fd >= 0) {
                pollFds[pollCount].fd = app->slots[i].conn.fd;
                pollFds[pollCount].events = POLLIN;
                /* Chỉ chờ POLLOUT khi có byte tồn để socket rảnh không đánh thức poll. */
                if (app->slots[i].conn.outLen > app->slots[i].conn.outPos) {
                    pollFds[pollCount].events |= POLLOUT;
                }
                pollFds[pollCount].revents = 0;
                slotByPollIndex[pollCount] = i;
                pollCount++;
            }
        }

        int readyCount = poll(pollFds, pollCount, POLL_TIMEOUT_MS);
        if (readyCount < 0) {
            /* EINTR quay lại đầu vòng để kiểm tra cờ dừng do tín hiệu đặt. */
            if (errno == EINTR) continue;
            break;
        }

        /* Áp dụng kết quả xác thực đã sẵn sàng trước khi xử lý lệnh mới của client. */
        if (pollFds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            authDrain(app);
        }

        // Bước 4. Nhận hết kết nối đang chờ trên socket lắng nghe.
        if (pollFds[0].revents & POLLIN) {
            while (1) {
                struct sockaddr_in clientAddress;
                socklen_t clientAddressLength = sizeof(clientAddress);
                int connectionSocket = accept(app->listenFd, (struct sockaddr *)&clientAddress,
                                              &clientAddressLength);
                if (connectionSocket < 0) {
                    if (errno == EINTR) continue;
                    /* EAGAIN hoặc EWOULDBLOCK báo hàng chờ hiện đã cạn. */
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    /* Lỗi khác kết thúc đợt nhận này để vòng poll tiếp tục phục vụ
                     * các socket đang hoạt động. */
                    break;
                }

                int freeSlot = -1;
                for (int i = 0; i < APP_MAX_CONNS; i++) {
                    if (!app->slots[i].active) {
                        freeSlot = i;
                        break;
                    }
                }

                if (freeSlot < 0) {
                    /* Hàm tự ghi lỗi và đóng socket để vòng nhận vẫn tiếp tục. */
                    (void)appRejectConnection(connectionSocket);
                    continue;
                }

                AppConnSlot *slot = &app->slots[freeSlot];
                slot->active = true;
                slot->generation++;
                /* Đổi địa chỉ sang chuỗi để áp hạn mức xác thực theo IP. */
                inet_ntop(AF_INET, &clientAddress.sin_addr, slot->clientIp,
                          sizeof(slot->clientIp));
                slot->rateLastMs = appNowMs();
                slot->rateTokens = RATE_BURST_TOKENS;
                slot->lastRequestId = 0;
                /* Cho kết nối mới đủ một khoảng heartbeat để gửi frame đầu tiên. */
                slot->lastFrameMs = slot->rateLastMs;
                /* Socket không chuyển được sang non-blocking sẽ làm treo vòng poll,
                 * nên đóng ngay thay vì phục vụ nó. */
                if (connInit(&slot->conn, connectionSocket, (uint32_t)freeSlot,
                             slot->generation) != OK) {
                    appCloseSlot(app, freeSlot);
                }
            }
        }

        for (int k = 2; k < pollCount; k++) {
            int slotIndex = slotByPollIndex[k];
            AppConnSlot *slot = &app->slots[slotIndex];
            if (!slot->active) continue;

            // Bước 5. Gửi phần còn tồn trước khi đọc, để nhường chỗ cho phản hồi mới.
            if (pollFds[k].revents & POLLOUT) {
                Status ioStatus = connFlush(&slot->conn);
                if (ioStatus != OK || slot->conn.closing) {
                    appCloseSlot(app, slotIndex);
                    continue;
                }
            }

            /* TCP có thể chia một frame thành nhiều lần recv(). connRead ghép
             * đủ frame rồi mới gọi onFrame và báo lỗi khi đầu kia đóng. */
            if (pollFds[k].revents & POLLIN) {
                FrameContext context = {app, (uint32_t)slotIndex, slot->generation};
                Status ioStatus = connRead(&slot->conn, onFrame, &context);
                if (ioStatus != OK || slot->conn.closing) {
                    appCloseSlot(app, slotIndex);
                    continue;
                }
            }

            // Bước 6. Đóng socket lỗi hoặc đã ngắt và thu hồi ô kết nối.
            if (pollFds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                appCloseSlot(app, slotIndex);
                continue;
            }
        }

        /* Lấy thêm kết quả xác thực hoàn tất trong lúc xử lý I/O của client. */
        authDrain(app);

        /* Những push phát sinh giữa vòng có thể đánh dấu client chậm để đóng ngay. */
        for (int i = 0; i < APP_MAX_CONNS; i++) {
            if (app->slots[i].active && app->slots[i].conn.closing) {
                appCloseSlot(app, i);
            }
        }

        sweepIdleConnections(app, appNowMs());
    }

    announceShutdown(app);
    appCleanup(app);
    free(app);
    return 0;
}
