#ifndef UNO_AUTH_WORKER_H
#define UNO_AUTH_WORKER_H

#include "shared/contracts.h"
#include "server/storage/store.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AUTH_REGISTER,
    AUTH_LOGIN
} AuthJobKind;

/* One request copied by value into the worker queue. The worker wipes its own
 * copy of the plaintext password after hashing or verifying it.
 * FIXME: the queue slot it was copied from and the caller's stack AuthJob in
 * accountsHandle keep the plaintext until overwritten. Wipe the slot with
 * sodium_memzero under the lock when dequeuing, and wipe job.password in
 * accountsHandle right after authSubmit. */
typedef struct {
    AuthJobKind kind;
    ConnId connId;
    int32_t reqId;
    char email[255];
    char password[129];
    char name[65];
    char avatar[32];
    char hash[128];
    Account account;
    bool userExists;
} AuthJob;

/* Written whole to the notify pipe; small enough for an atomic pipe write. */
typedef struct {
    ConnId connId;
    int32_t reqId;
    Status status;
    UserId userId;
    char token[65];
    Account account;
    AuthJobKind kind;
} AuthResult;

typedef struct AuthWorker AuthWorker;

/**
 * @brief Start the hashing thread.
 *
 * Also precomputes a dummy Argon2 hash so a LOGIN for an unknown email costs
 * as much time as one for a real account.
 *
 * @param[out] outWorker Receives the worker on success.
 * @param[in] notifyPipeFd Non-blocking write end of the pipe appRun polls;
 *            stays owned by the caller.
 * @return OK, BAD_REQUEST for invalid arguments, or STORAGE_ERROR when
 *         libsodium or pthread setup fails.
 */
Status authWorkerInit(AuthWorker **outWorker, int notifyPipeFd);

/**
 * @brief Queue a REGISTER or LOGIN job without blocking the poll thread.
 * @param[in,out] worker Running worker.
 * @param[in] job Copied into the queue; the caller may reuse it at once.
 * @return OK, BAD_REQUEST for NULL arguments, or BUSY when the queue
 *         (AUTH_QUEUE_CAPACITY jobs) is full or the worker is stopping.
 */
Status authSubmit(AuthWorker *worker, const AuthJob *job);

/**
 * @brief Stop and join the thread, wipe queued passwords and free the worker.
 * @param[in] worker Worker to destroy; NULL is ignored. Jobs still queued are
 *            finished first, so the call can block for a few hash rounds.
 */
void authWorkerDestroy(AuthWorker *worker);

#endif
