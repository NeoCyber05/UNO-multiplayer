#ifndef UNO_ACCOUNTS_H
#define UNO_ACCOUNTS_H

#include "server/app.h"
#include "shared/contracts.h"
#include "shared/protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Validation helper functions */
bool accounts_validate_email(const char *email);
bool accounts_validate_password(const char *password);
bool accounts_validate_name(const char *name);
bool accounts_validate_avatar(const char *avatar);

/* Token and tag helpers */
void accounts_generate_token(char out_token[65]);
void accounts_format_tag(UserId user_id, char out_tag[32]);
uint64_t accounts_get_now_ms(void);

#define MAX_SESSIONS 256

typedef struct {
    bool in_use;
    UserId user_id;
    char token[65];
    ConnId conn_id;
    bool connected;
    uint64_t created_at_ms;
} SessionEntry;

struct Sessions {
    SessionEntry entries[MAX_SESSIONS];
};

/* Sessions and rate limiter lifecycle */
Status sessions_init(App *app);
void sessions_cleanup(App *app);

Status sessions_bind(App *app, ConnId conn_id, UserId user_id, const char *token);
Status sessions_user(App *app, ConnId conn_id, UserId *out);
Status sessions_find_conn(App *app, UserId user_id, ConnId *out_conn);
Status sessions_resume(App *app, ConnId conn_id, const char *token);
void sessions_disconnect(App *app, ConnId conn_id);
void sessions_logout(App *app, ConnId conn_id);

/* Rate limit check */
bool accounts_rate_limit_check(App *app, const char *ip);

/* Request handler & pipe drain */
Status accounts_handle(App *app, ConnId conn_id, const Envelope *e);
void auth_drain(App *app);

#endif
