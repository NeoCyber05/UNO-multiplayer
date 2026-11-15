#ifndef UNO_AUTH_WORKER_H
#define UNO_AUTH_WORKER_H

#include "shared/contracts.h"
#include "server/store.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AUTH_REGISTER,
    AUTH_LOGIN
} AuthJobKind;

typedef struct {
    AuthJobKind kind;
    ConnId conn_id;
    int32_t req_id;
    char email[255];
    char password[129];
    char name[65];
    char avatar[32];
    char hash[128];
    Account account;
    bool user_exists;
} AuthJob;

typedef struct {
    ConnId conn_id;
    int32_t req_id;
    Status status;
    UserId user_id;
    char token[65];
    Account account;
    AuthJobKind kind;
} AuthResult;

typedef struct AuthWorker AuthWorker;

Status auth_worker_init(AuthWorker **out, int notify_pipe_fd);
Status auth_submit(AuthWorker *w, const AuthJob *job);
void auth_worker_destroy(AuthWorker *w);

#endif
