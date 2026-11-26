#define _POSIX_C_SOURCE 200809L
#include "auth_worker.h"
#include "accounts.h"
#include <errno.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AUTH_QUEUE_CAPACITY 8

enum {
    /* Spec: every new account starts at ELO 1500. */
    INITIAL_ELO = 1500
};

/* Ring buffer of pending jobs shared by the poll thread (producer) and one
 * hashing thread (consumer); every field below `lock` is guarded by it. */
struct AuthWorker {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int notifyPipeFd;
    bool running;
    AuthJob queue[AUTH_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    char dummyHash[128];
};

/* Argon2 hashing takes tens of milliseconds by design, so it runs here
 * instead of on the poll thread, which would otherwise stall every client. */
static void *authWorkerThread(void *argument) {
    AuthWorker *worker = (AuthWorker *)argument;

    while (1) {
        AuthJob job;
        pthread_mutex_lock(&worker->lock);
        while (worker->running && worker->count == 0) {
            pthread_cond_wait(&worker->cond, &worker->lock);
        }

        if (!worker->running && worker->count == 0) {
            pthread_mutex_unlock(&worker->lock);
            break;
        }

        job = worker->queue[worker->head];
        worker->head = (worker->head + 1) % AUTH_QUEUE_CAPACITY;
        worker->count--;
        pthread_mutex_unlock(&worker->lock);

        AuthResult result;
        memset(&result, 0, sizeof(result));
        result.conn_id = job.conn_id;
        result.req_id = job.req_id;
        result.kind = job.kind;

        if (job.kind == AUTH_REGISTER) {
            if (crypto_pwhash_str(result.account.hash, job.password, strlen(job.password),
                                  crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                  crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
                result.status = STORAGE_ERROR;
            } else {
                result.status = OK;
                strncpy(result.account.email, job.email, sizeof(result.account.email) - 1);
                strncpy(result.account.name, job.name, sizeof(result.account.name) - 1);
                strncpy(result.account.avatar, job.avatar, sizeof(result.account.avatar) - 1);
                result.account.elo = INITIAL_ELO;
            }
        } else if (job.kind == AUTH_LOGIN) {
            if (!job.user_exists) {
                /* An unknown email still pays for one verify, so response time
                 * does not reveal which emails are registered. volatile keeps
                 * the compiler from dropping the unused call. */
                if (worker->dummyHash[0] != '\0') {
                    volatile int dummyResult =
                        crypto_pwhash_str_verify(worker->dummyHash, job.password,
                                                 strlen(job.password));
                    (void)dummyResult;
                }
                result.status = UNAUTHENTICATED;
            } else {
                if (crypto_pwhash_str_verify(job.hash, job.password, strlen(job.password)) == 0) {
                    result.status = OK;
                    result.user_id = job.account.id;
                    result.account = job.account;

                    /* Generate secure 32-byte token and hex encode */
                    accounts_generate_token(result.token);
                } else {
                    result.status = UNAUTHENTICATED;
                }
            }
        }

        /* Always wipe plaintext password memory */
        sodium_memzero(job.password, sizeof(job.password));

        /* Hand the result to the poll thread. One record is below PIPE_BUF, so
         * the write is atomic and auth_drain always reads whole records.
         * FIXME: on a full non-blocking pipe (EAGAIN) the result is dropped and
         * the client never gets a reply. Retry after poll(POLLOUT) on the pipe. */
        const uint8_t *resultBytes = (const uint8_t *)&result;
        size_t bytesWritten = 0;
        while (bytesWritten < sizeof(result)) {
            ssize_t written = write(worker->notifyPipeFd, resultBytes + bytesWritten,
                                    sizeof(result) - bytesWritten);
            if (written < 0) {
                if (errno == EINTR) continue;
                break;
            }
            bytesWritten += (size_t)written;
        }
    }

    return NULL;
}

Status auth_worker_init(AuthWorker **outWorker, int notifyPipeFd) {
    if (!outWorker || notifyPipeFd < 0) return BAD_REQUEST;

    if (sodium_init() < 0) {
        return STORAGE_ERROR;
    }

    AuthWorker *worker = calloc(1, sizeof(AuthWorker));
    if (!worker) return STORAGE_ERROR;

    worker->notifyPipeFd = notifyPipeFd;
    worker->running = true;
    worker->head = 0;
    worker->tail = 0;
    worker->count = 0;

    /* Pre-generate a dummy hash for constant-time failure response */
    if (crypto_pwhash_str(worker->dummyHash, "dummy_password_xyz", 18,
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        worker->dummyHash[0] = '\0';
    }

    if (pthread_mutex_init(&worker->lock, NULL) != 0) {
        free(worker);
        return STORAGE_ERROR;
    }

    if (pthread_cond_init(&worker->cond, NULL) != 0) {
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        return STORAGE_ERROR;
    }

    if (pthread_create(&worker->thread, NULL, authWorkerThread, worker) != 0) {
        pthread_cond_destroy(&worker->cond);
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        return STORAGE_ERROR;
    }

    *outWorker = worker;
    return OK;
}

Status auth_submit(AuthWorker *worker, const AuthJob *job) {
    if (!worker || !job) return BAD_REQUEST;

    pthread_mutex_lock(&worker->lock);
    if (!worker->running) {
        pthread_mutex_unlock(&worker->lock);
        return BUSY;
    }

    if (worker->count >= AUTH_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&worker->lock);
        return BUSY;
    }

    worker->queue[worker->tail] = *job;
    worker->tail = (worker->tail + 1) % AUTH_QUEUE_CAPACITY;
    worker->count++;

    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->lock);

    return OK;
}

void auth_worker_destroy(AuthWorker *worker) {
    if (!worker) return;

    pthread_mutex_lock(&worker->lock);
    worker->running = false;
    pthread_cond_broadcast(&worker->cond);
    pthread_mutex_unlock(&worker->lock);

    pthread_join(worker->thread, NULL);

    /* Zero out passwords remaining in queue */
    for (size_t i = 0; i < AUTH_QUEUE_CAPACITY; i++) {
        sodium_memzero(worker->queue[i].password, sizeof(worker->queue[i].password));
    }

    pthread_cond_destroy(&worker->cond);
    pthread_mutex_destroy(&worker->lock);
    free(worker);
}
