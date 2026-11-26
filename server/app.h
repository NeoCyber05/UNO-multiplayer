#ifndef UNO_APP_H
#define UNO_APP_H

#include "server/lobby/rooms.h"
#include "shared/contracts.h"
#include "shared/net.h"

enum {
    APP_MAX_CONNS = 64,
    /* Spec: clients PING every 5 s; a connection silent for 15 s is dead. */
    APP_DEFAULT_HEARTBEAT_MS = 15000,
    /* Above the coordinator's 16 match slots, so a record is always free. */
    APP_MATCH_RECORDS = 32
};

/* Humans seated in a running match. The end callback only carries the room id,
 * so this is how the App knows whose presence to refresh afterwards. */
typedef struct {
    bool active;
    char roomId[sizeof(((RoomStart *)0)->roomId)];
    UserId users[ROOM_MAX_SEATS]; /* 0 for a bot seat */
} AppMatchRecord;

typedef struct {
    Conn conn;
    bool active;
    uint32_t generation;
    char clientIp[64];
    uint64_t rateLastMs;
    double rateTokens;
    int32_t lastRequestId; /* request ids must strictly increase per connection */
    uint64_t lastFrameMs;  /* monotonic time of the last accepted frame */
} AppConnSlot;

typedef struct AuthWorker AuthWorker;
typedef struct Sessions Sessions;
typedef struct RateLimiter RateLimiter;

struct App {
    int listenFd;
    uint16_t boundPort;
    char bindIp[64];
    char dbPath[256];
    int authLimit;
    bool running;
    Store *store;
    AppConnSlot slots[APP_MAX_CONNS];
    AuthWorker *authWorker;
    int authPipeRead;
    int authPipeWrite;
    Sessions *sessions;
    RateLimiter *rateLimiter;
    Rooms *rooms;
    Queue *queue;
    Matches *matches;
    int heartbeatMs;
    AppMatchRecord matchRecords[APP_MATCH_RECORDS];
    /* Set only while appUserLeave runs: its own presence push replaces the
     * one a match abort would send for this user. */
    UserId leavingUser;
};

/**
 * @brief Prepare server state and open the non-blocking listening socket.
 *
 * Also starts the auth worker thread with its wake-up pipe, ignores SIGPIPE and
 * installs the SIGINT/SIGTERM handler. The store and match coordinator are
 * opened later by appRun, so tests can inject their own.
 *
 * @param[out] app State to initialize; zeroed first.
 * @param[in] bindIp IPv4 address in dotted form, or NULL for "0.0.0.0".
 * @param[in] port TCP port in host byte order; 0 picks an ephemeral port
 *            stored in `boundPort`.
 * @param[in] dbPath SQLite path, or NULL for "uno.db".
 * @param[in] authLimit Auth requests allowed per IP per minute.
 * @return OK; BAD_REQUEST for an unparsable @p bindIp; BUSY when socket(),
 *         bind() or listen() fails (the socket is closed again), or an error
 *         from queueInit.
 */
Status appInit(App *app, const char *bindIp, uint16_t port, const char *dbPath, int authLimit);

/**
 * @brief Run the server poll loop until SIGINT/SIGTERM.
 * @param[in] bindIp IPv4 address to listen on.
 * @param[in] port TCP port, 0 for an ephemeral port printed as "PORT n".
 * @param[in] dbPath SQLite path or ":memory:".
 * @param[in] authLimit Auth requests allowed per IP per minute.
 * @param[in] heartbeatMs Idle time after which a connection is closed.
 * @return 0 after a clean shutdown, 1 if startup failed.
 */
int appRun(const char *bindIp, uint16_t port, const char *dbPath, int authLimit,
            int heartbeatMs);
/**
 * @brief Ask the poll loop to exit after the current iteration.
 * @param[in,out] app Server state; NULL is ignored.
 */
void appStop(App *app);

/**
 * @brief Close every connection, the listening socket and the auth pipe, then
 *        free queue, rooms, sessions, matches and store (in that order).
 *
 * Safe on a partially initialized App. Presence pushes are suppressed because
 * `running` is cleared first.
 *
 * @param[in,out] app Server state; the App struct itself is not freed.
 */
void appCleanup(App *app);

/**
 * @brief Detach the session, close the socket and free the slot for reuse.
 *
 * The slot generation is bumped so ConnIds and frames for the old socket are
 * rejected afterwards. Safe to call on an inactive slot.
 *
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] slotIndex Index into `slots`; out-of-range values are ignored.
 */
void appCloseSlot(App *app, int slotIndex);

/**
 * @brief Refuse a freshly accepted connection because every slot is taken.
 *
 * Sends one ERROR frame {code: "SERVER_FULL"} with id 0 and closes the socket.
 * The attempt is best effort: a single send() with MSG_NOSIGNAL, no retry, so
 * it never raises SIGPIPE and blocks at most once. A failed or short send is
 * logged to stderr (stdout carries the "PORT n" line).
 *
 * @param[in] connectionSocket Socket returned by accept(); always closed here.
 * @return OK when the whole frame was handed to the kernel, BAD_REQUEST when
 *         encoding failed or send() failed or wrote only part of the frame.
 */
Status appRejectConnection(int connectionSocket);

/**
 * @brief Take a user off a connection: socket close, LOGOUT, or another
 *        account binding the same socket.
 *
 * Spec §5: the match seat goes offline (aborting the match when no other
 * human is connected), the queue entry and pending invites are dropped, the
 * party dissolves and a waiting room loses the seat; a running match keeps
 * the seat for RESUME or LOGIN. Pushes for those changes still reach the user
 * because the session is detached (connected = false) only afterwards.
 * Friends then get one presence update per user whose presence changed.
 * The caller decides whether the detached session is also forgotten
 * (sessionsLogout) or kept for RESUME.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] conn Connection the user is leaving.
 * @param[in] user Account bound to @p conn; 0 is ignored.
 */
void appUserLeave(App *app, ConnId conn, UserId user);

/**
 * @brief Spend one token from the connection's request bucket.
 *
 * The bucket refills at 0.03 tokens/ms and holds at most 60, so a client may
 * burst 60 requests and then sustain 30 per second.
 *
 * @param[in,out] slot Connection whose bucket is updated.
 * @param[in] nowMs Monotonic time from appNowMs.
 * @return true if the request may run, false if it must get RATE_LIMITED.
 */
bool appConnCheckRateLimit(AppConnSlot *slot, uint64_t nowMs);

/**
 * @brief Read the clock shared by the poll loop, heartbeat and match timers.
 * @return Milliseconds from CLOCK_MONOTONIC, immune to wall-clock jumps.
 */
uint64_t appNowMs(void);

/**
 * @brief Queue and flush a server push (envelope id 0) to a user's live connection.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] user Account to notify.
 * @param[in] type Push opcode, e.g. "GAME_STATE".
 * @param[in] payload JSON object borrowed for the call; ownership stays with the caller.
 * @return OK when queued, NOT_FOUND if the user has no connected session,
 *         BAD_REQUEST on invalid arguments, or the connQueue/connFlush error.
 */
Status appPush(App *app, UserId user, const char *type, json_object *payload);

/**
 * @brief Answer a request with OK {} or ERROR {code, message} derived from a Status.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] conn Connection that sent the request; ignored if already closed.
 * @param[in] id Request id echoed back to the client.
 * @param[in] status OK sends an empty OK; any other value sends ERROR with statusCode(status).
 */
void appReplyStatus(App *app, ConnId conn, int32_t id, Status status);

/**
 * @brief Answer a request with OK and a result object.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] conn Connection that sent the request; ignored if already closed.
 * @param[in] id Request id echoed back to the client.
 * @param[in] payload JSON object borrowed for the call; ownership stays with the caller.
 */
void appReplyOk(App *app, ConnId conn, int32_t id, json_object *payload);

#endif
