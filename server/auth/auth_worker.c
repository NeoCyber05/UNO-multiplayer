#define _POSIX_C_SOURCE 200809L
#include "auth_worker.h"
#include "server/auth/accounts.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    /* Số job tối đa chờ băm; vượt quá thì REGISTER/LOGIN nhận BUSY. */
    AUTH_QUEUE_CAPACITY = 8,
    /* Spec: mọi tài khoản mới bắt đầu với ELO 1500. */
    INITIAL_ELO = 1500,
    /* Thời gian tối đa chờ pipe ghi được trở lại. Có giới hạn để lúc tắt server
     * (luồng poll đã ngừng đọc pipe) authWorkerDestroy không bị treo khi join. */
    PIPE_WRITABLE_TIMEOUT_MS = 1000
};

/* Mật khẩu giả chỉ dùng để tính hash giả lúc khởi động. */
static const char DUMMY_PASSWORD[] = "dummy_password_xyz";

/* Ring buffer các job chờ xử lý, dùng chung giữa luồng poll (đẩy vào) và một
 * luồng băm (lấy ra); mọi trường bên dưới `lock` đều được `lock` bảo vệ. */
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
    char dummyHash[PASSWORD_HASH_SIZE];
};

/**
 * @brief Kiểm tra kết quả một lời gọi pthread đồng bộ hóa.
 *
 * Lock/unlock/wait trên mutex mặc định chỉ lỗi khi trạng thái chương trình đã
 * hỏng (mutex chưa khởi tạo, unlock sai luồng); chạy tiếp lúc đó sẽ làm hỏng
 * hàng đợi, nên dừng hẳn tiến trình.
 */
static void requireThreadCall(int result, const char *what) {
    if (result == 0) return;
    fprintf(stderr, "auth worker: %s failed: %s\n", what, strerror(result));
    abort();
}

/**
 * @brief Chờ và lấy job kế tiếp khỏi hàng đợi.
 *
 * Ô vừa lấy ra được xóa ngay để mật khẩu dạng rõ không nằm lại trong hàng đợi.
 *
 * @param[out] outJob Nhận bản sao job.
 * @return false khi worker đã dừng và hàng đợi rỗng (luồng nên kết thúc).
 */
static bool dequeueJob(AuthWorker *worker, AuthJob *outJob) {
    requireThreadCall(pthread_mutex_lock(&worker->lock), "pthread_mutex_lock");
    while (worker->running && worker->count == 0) {
        requireThreadCall(pthread_cond_wait(&worker->cond, &worker->lock), "pthread_cond_wait");
    }
    bool hasJob = worker->count > 0;
    if (hasJob) {
        AuthJob *slot = &worker->queue[worker->head];
        *outJob = *slot;
        sodium_memzero(slot->password, sizeof(slot->password));
        worker->head = (worker->head + 1) % AUTH_QUEUE_CAPACITY;
        worker->count--;
    }
    requireThreadCall(pthread_mutex_unlock(&worker->lock), "pthread_mutex_unlock");
    return hasJob;
}

/* REGISTER: băm mật khẩu và chuẩn bị dữ liệu tài khoản để luồng poll ghi vào DB. */
static void hashNewAccount(const AuthJob *job, AuthResult *result) {
    if (crypto_pwhash_str(result->account.hash, job->password, strlen(job->password),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        result->status = STORAGE_ERROR;
        return;
    }
    result->status = OK;
    snprintf(result->account.email, sizeof(result->account.email), "%s", job->email);
    snprintf(result->account.name, sizeof(result->account.name), "%s", job->name);
    snprintf(result->account.avatar, sizeof(result->account.avatar), "%s", job->avatar);
    result->account.elo = INITIAL_ELO;
}

/* LOGIN: đối chiếu mật khẩu với hash đã lưu và cấp token phiên khi khớp. */
static void verifyLogin(const AuthWorker *worker, const AuthJob *job, AuthResult *result) {
    result->status = UNAUTHENTICATED;
    if (!job->userExists) {
        /* Email không tồn tại vẫn tốn một lần verify, để thời gian phản hồi
         * không tiết lộ email nào đã đăng ký. volatile ngăn compiler bỏ lời gọi. */
        if (worker->dummyHash[0] != '\0') {
            volatile int dummyResult = crypto_pwhash_str_verify(
                worker->dummyHash, job->password, strlen(job->password));
            (void)dummyResult;
        }
        return;
    }
    if (crypto_pwhash_str_verify(job->hash, job->password, strlen(job->password)) != 0) return;
    result->status = OK;
    result->userId = job->account.id;
    result->account = job->account;
    accountsGenerateToken(result->token);
}

/**
 * @brief Chờ đầu ghi của pipe sẵn sàng; luồng worker được phép chặn, luồng poll thì không.
 * @return false khi poll() lỗi hoặc hết PIPE_WRITABLE_TIMEOUT_MS mà pipe vẫn đầy.
 */
static bool waitPipeWritable(int fd) {
    struct pollfd pollFd = {fd, POLLOUT, 0};
    int readyCount;
    do {
        readyCount = poll(&pollFd, 1, PIPE_WRITABLE_TIMEOUT_MS);
    } while (readyCount < 0 && errno == EINTR);
    if (readyCount < 0) perror("auth worker: poll");
    if (readyCount == 0) {
        fprintf(stderr, "auth worker: notify pipe stayed full, result dropped\n");
    }
    return readyCount > 0;
}

/**
 * @brief Giao kết quả cho luồng poll qua notify pipe.
 *
 * Một bản ghi nhỏ hơn PIPE_BUF nên write() là nguyên tử và authDrain luôn đọc
 * nguyên bản ghi. Pipe đầy (EAGAIN) thì chờ POLLOUT rồi ghi lại; chỉ bỏ kết quả
 * khi pipe đầy quá PIPE_WRITABLE_TIMEOUT_MS.
 */
static void writeResult(int fd, const AuthResult *result) {
    const uint8_t *resultBytes = (const uint8_t *)result;
    size_t bytesWritten = 0;
    while (bytesWritten < sizeof(*result)) {
        ssize_t written = write(fd, resultBytes + bytesWritten, sizeof(*result) - bytesWritten);
        if (written >= 0) {
            bytesWritten += (size_t)written;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!waitPipeWritable(fd)) return;
        } else if (errno != EINTR) {
            perror("auth worker: write");
            return;
        }
    }
}

/* Băm mật khẩu Argon2 tốn hàng chục mili giây theo thiết kế, nên chạy ở luồng
 * riêng này thay vì luồng poll, nơi nó sẽ làm treo mọi client. */
static void *authWorkerThread(void *argument) {
    AuthWorker *worker = (AuthWorker *)argument;
    AuthJob job;
    while (dequeueJob(worker, &job)) {
        AuthResult result;
        memset(&result, 0, sizeof(result));
        result.connId = job.connId;
        result.reqId = job.reqId;
        result.kind = job.kind;
        if (job.kind == AUTH_REGISTER) {
            hashNewAccount(&job, &result);
        } else if (job.kind == AUTH_LOGIN) {
            verifyLogin(worker, &job, &result);
        }
        sodium_memzero(job.password, sizeof(job.password));
        writeResult(worker->notifyPipeFd, &result);
    }
    return NULL;
}

/* Khởi tạo mutex, condition variable và luồng; dọn lại phần đã tạo nếu lỗi. */
static Status startWorkerThread(AuthWorker *worker) {
    if (pthread_mutex_init(&worker->lock, NULL) != 0) return STORAGE_ERROR;
    if (pthread_cond_init(&worker->cond, NULL) != 0) {
        requireThreadCall(pthread_mutex_destroy(&worker->lock), "pthread_mutex_destroy");
        return STORAGE_ERROR;
    }
    if (pthread_create(&worker->thread, NULL, authWorkerThread, worker) != 0) {
        requireThreadCall(pthread_cond_destroy(&worker->cond), "pthread_cond_destroy");
        requireThreadCall(pthread_mutex_destroy(&worker->lock), "pthread_mutex_destroy");
        return STORAGE_ERROR;
    }
    return OK;
}

Status authWorkerInit(AuthWorker **outWorker, int notifyPipeFd) {
    if (!outWorker || notifyPipeFd < 0) return BAD_REQUEST;
    if (sodium_init() < 0) return STORAGE_ERROR;

    AuthWorker *worker = calloc(1, sizeof(AuthWorker));
    if (!worker) return STORAGE_ERROR;
    worker->notifyPipeFd = notifyPipeFd;
    worker->running = true;

    /* Tính sẵn hash giả để LOGIN sai email tốn thời gian như LOGIN thật. */
    if (crypto_pwhash_str(worker->dummyHash, DUMMY_PASSWORD, sizeof(DUMMY_PASSWORD) - 1,
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        worker->dummyHash[0] = '\0';
    }
    if (startWorkerThread(worker) != OK) {
        free(worker);
        return STORAGE_ERROR;
    }
    *outWorker = worker;
    return OK;
}

Status authSubmit(AuthWorker *worker, const AuthJob *job) {
    if (!worker || !job) return BAD_REQUEST;

    requireThreadCall(pthread_mutex_lock(&worker->lock), "pthread_mutex_lock");
    bool accepted = worker->running && worker->count < AUTH_QUEUE_CAPACITY;
    if (accepted) {
        worker->queue[worker->tail] = *job;
        worker->tail = (worker->tail + 1) % AUTH_QUEUE_CAPACITY;
        worker->count++;
        requireThreadCall(pthread_cond_signal(&worker->cond), "pthread_cond_signal");
    }
    requireThreadCall(pthread_mutex_unlock(&worker->lock), "pthread_mutex_unlock");
    return accepted ? OK : BUSY;
}

void authWorkerDestroy(AuthWorker *worker) {
    if (!worker) return;

    requireThreadCall(pthread_mutex_lock(&worker->lock), "pthread_mutex_lock");
    worker->running = false;
    requireThreadCall(pthread_cond_broadcast(&worker->cond), "pthread_cond_broadcast");
    requireThreadCall(pthread_mutex_unlock(&worker->lock), "pthread_mutex_unlock");
    requireThreadCall(pthread_join(worker->thread, NULL), "pthread_join");

    /* Xóa mọi mật khẩu có thể còn sót trong hàng đợi trước khi trả bộ nhớ. */
    for (size_t i = 0; i < AUTH_QUEUE_CAPACITY; i++) {
        sodium_memzero(worker->queue[i].password, sizeof(worker->queue[i].password));
    }
    requireThreadCall(pthread_cond_destroy(&worker->cond), "pthread_cond_destroy");
    requireThreadCall(pthread_mutex_destroy(&worker->lock), "pthread_mutex_destroy");
    free(worker);
}
