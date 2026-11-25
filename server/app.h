#ifndef UNO_APP_H
#define UNO_APP_H

#include "shared/contracts.h"
#include "shared/net.h"

enum {
    APP_MAX_CONNS = 64,
    /* Spec: clients PING every 5 s; a connection silent for 15 s is dead. */
    APP_DEFAULT_HEARTBEAT_MS = 15000
};

typedef struct {
    Conn conn;
    bool active;
    uint32_t generation;
    char client_ip[64];
    uint64_t rate_last_ms;
    double rate_tokens;
    int32_t last_request_id; /* request ids must strictly increase per connection */
    uint64_t last_frame_ms;  /* monotonic time of the last accepted frame */
} AppConnSlot;

typedef struct AuthWorker AuthWorker;
typedef struct Sessions Sessions;
typedef struct RateLimiter RateLimiter;

struct App {
    int listen_fd;
    uint16_t bound_port;
    char bind_ip[64];
    char db_path[256];
    int auth_limit;
    bool running;
    Store *store;
    AppConnSlot slots[APP_MAX_CONNS];
    AuthWorker *auth_worker;
    int auth_pipe_read;
    int auth_pipe_write;
    Sessions *sessions;
    RateLimiter *rate_limiter;
    Rooms *rooms;
    Queue *queue;
    Matches *matches;
    int heartbeat_ms;
};

Status app_init(App *app, const char *bind_ip, uint16_t port, const char *db_path, int auth_limit);
/**
 * @brief Run the server poll loop until SIGINT/SIGTERM.
 * @param[in] bind_ip IPv4 address to listen on.
 * @param[in] port TCP port, 0 for an ephemeral port printed as "PORT n".
 * @param[in] db_path SQLite path or ":memory:".
 * @param[in] auth_limit Auth requests allowed per IP per minute.
 * @param[in] heartbeat_ms Idle time after which a connection is closed.
 * @return 0 after a clean shutdown, 1 if startup failed.
 */
int app_run(const char *bind_ip, uint16_t port, const char *db_path, int auth_limit,
            int heartbeat_ms);
void app_stop(App *app);
void app_cleanup(App *app);
void app_close_slot(App *app, int slot_idx);
bool app_conn_check_rate_limit(AppConnSlot *slot, uint64_t now_ms);

/**
 * @brief Read the clock shared by the poll loop, heartbeat and match timers.
 * @return Milliseconds from CLOCK_MONOTONIC, immune to wall-clock jumps.
 */
uint64_t app_now_ms(void);

/**
 * @brief Queue and flush a server push (envelope id 0) to a user's live connection.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] user Account to notify.
 * @param[in] type Push opcode, e.g. "GAME_STATE".
 * @param[in] payload JSON object borrowed for the call; ownership stays with the caller.
 * @return OK when queued, NOT_FOUND if the user has no connected session,
 *         BAD_REQUEST on invalid arguments, or the conn_queue/conn_flush error.
 */
Status app_push(App *app, UserId user, const char *type, json_object *payload);

/**
 * @brief Answer a request with OK {} or ERROR {code, message} derived from a Status.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] conn Connection that sent the request; ignored if already closed.
 * @param[in] id Request id echoed back to the client.
 * @param[in] st OK sends an empty OK; any other value sends ERROR with status_code(st).
 */
void app_reply_status(App *app, ConnId conn, int32_t id, Status st);

/**
 * @brief Answer a request with OK and a result object.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] conn Connection that sent the request; ignored if already closed.
 * @param[in] id Request id echoed back to the client.
 * @param[in] payload JSON object borrowed for the call; ownership stays with the caller.
 */
void app_reply_ok(App *app, ConnId conn, int32_t id, json_object *payload);

#endif
