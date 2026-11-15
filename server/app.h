#ifndef UNO_APP_H
#define UNO_APP_H

#include "shared/contracts.h"
#include "shared/net.h"

enum {
    APP_MAX_CONNS = 64
};

typedef struct {
    Conn conn;
    bool active;
    uint32_t generation;
    char client_ip[64];
    uint64_t rate_last_ms;
    double rate_tokens;
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
};

Status app_init(App *app, const char *bind_ip, uint16_t port, const char *db_path, int auth_limit);
int app_run(const char *bind_ip, uint16_t port, const char *db_path, int auth_limit);
void app_stop(App *app);
void app_cleanup(App *app);
void app_close_slot(App *app, int slot_idx);
bool app_conn_check_rate_limit(AppConnSlot *slot, uint64_t now_ms);

#endif
