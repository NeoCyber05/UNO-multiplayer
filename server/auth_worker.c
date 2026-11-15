#define _POSIX_C_SOURCE 200809L
#include "auth_worker.h"
#include <errno.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AUTH_QUEUE_CAPACITY 8

struct AuthWorker {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int notify_pipe_fd;
    bool running;
    AuthJob queue[AUTH_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    char dummy_hash[128];
};

static void *auth_worker_thread(void *arg)
{
    AuthWorker *w = (AuthWorker *)arg;

    while (1) {
        AuthJob job;
        pthread_mutex_lock(&w->lock);
        while (w->running && w->count == 0) {
            pthread_cond_wait(&w->cond, &w->lock);
        }

        if (!w->running && w->count == 0) {
            pthread_mutex_unlock(&w->lock);
            break;
        }

        job = w->queue[w->head];
        w->head = (w->head + 1) % AUTH_QUEUE_CAPACITY;
        w->count--;
        pthread_mutex_unlock(&w->lock);

        AuthResult res;
        memset(&res, 0, sizeof(res));
        res.conn_id = job.conn_id;
        res.req_id = job.req_id;
        res.kind = job.kind;

        if (job.kind == AUTH_REGISTER) {
            if (crypto_pwhash_str(res.account.hash, job.password, strlen(job.password),
                                  crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                  crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
                res.status = STORAGE_ERROR;
            } else {
                res.status = OK;
                strncpy(res.account.email, job.email, sizeof(res.account.email) - 1);
                strncpy(res.account.name, job.name, sizeof(res.account.name) - 1);
                strncpy(res.account.avatar, job.avatar, sizeof(res.account.avatar) - 1);
                res.account.elo = 1500;
            }
        } else if (job.kind == AUTH_LOGIN) {
            if (!job.user_exists) {
                /* Timing attack mitigation: run dummy verify against pre-generated dummy hash */
                if (w->dummy_hash[0] != '\0') {
                    volatile int dummy_ret = crypto_pwhash_str_verify(w->dummy_hash, job.password, strlen(job.password));
                    (void)dummy_ret;
                }
                res.status = UNAUTHENTICATED;
            } else {
                if (crypto_pwhash_str_verify(job.hash, job.password, strlen(job.password)) == 0) {
                    res.status = OK;
                    res.user_id = job.account.id;
                    res.account = job.account;

                    /* Generate secure 32-byte token and hex encode */
                    unsigned char token_bytes[32];
                    randombytes_buf(token_bytes, sizeof(token_bytes));
                    sodium_bin2hex(res.token, sizeof(res.token), token_bytes, sizeof(token_bytes));
                } else {
                    res.status = UNAUTHENTICATED;
                }
            }
        }

        /* Always wipe plaintext password memory */
        sodium_memzero(job.password, sizeof(job.password));

        /* Send result through notify pipe */
        const uint8_t *ptr = (const uint8_t *)&res;
        size_t total = 0;
        while (total < sizeof(res)) {
            ssize_t n = write(w->notify_pipe_fd, ptr + total, sizeof(res) - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            total += (size_t)n;
        }
    }

    return NULL;
}

Status auth_worker_init(AuthWorker **out, int notify_pipe_fd)
{
    if (!out || notify_pipe_fd < 0) return BAD_REQUEST;

    if (sodium_init() < 0) {
        return STORAGE_ERROR;
    }

    AuthWorker *w = calloc(1, sizeof(AuthWorker));
    if (!w) return STORAGE_ERROR;

    w->notify_pipe_fd = notify_pipe_fd;
    w->running = true;
    w->head = 0;
    w->tail = 0;
    w->count = 0;

    /* Pre-generate a dummy hash for constant-time failure response */
    if (crypto_pwhash_str(w->dummy_hash, "dummy_password_xyz", 18,
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        w->dummy_hash[0] = '\0';
    }

    if (pthread_mutex_init(&w->lock, NULL) != 0) {
        free(w);
        return STORAGE_ERROR;
    }

    if (pthread_cond_init(&w->cond, NULL) != 0) {
        pthread_mutex_destroy(&w->lock);
        free(w);
        return STORAGE_ERROR;
    }

    if (pthread_create(&w->thread, NULL, auth_worker_thread, w) != 0) {
        pthread_cond_destroy(&w->cond);
        pthread_mutex_destroy(&w->lock);
        free(w);
        return STORAGE_ERROR;
    }

    *out = w;
    return OK;
}

Status auth_submit(AuthWorker *w, const AuthJob *job)
{
    if (!w || !job) return BAD_REQUEST;

    pthread_mutex_lock(&w->lock);
    if (!w->running) {
        pthread_mutex_unlock(&w->lock);
        return BUSY;
    }

    if (w->count >= AUTH_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&w->lock);
        return BUSY;
    }

    w->queue[w->tail] = *job;
    w->tail = (w->tail + 1) % AUTH_QUEUE_CAPACITY;
    w->count++;

    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);

    return OK;
}

void auth_worker_destroy(AuthWorker *w)
{
    if (!w) return;

    pthread_mutex_lock(&w->lock);
    w->running = false;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);

    pthread_join(w->thread, NULL);

    /* Zero out passwords remaining in queue */
    for (size_t i = 0; i < AUTH_QUEUE_CAPACITY; i++) {
        sodium_memzero(w->queue[i].password, sizeof(w->queue[i].password));
    }

    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->lock);
    free(w);
}
