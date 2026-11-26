#ifndef UNO_TCP_CLIENT_H
#define UNO_TCP_CLIENT_H

#include "shared/contracts.h"
#include "shared/net.h"
#include <sys/socket.h>

/* Told after every send/flush whether output is still queued, so the GUI can
 * watch the socket for writability only while it has something to send. */
typedef void (*ClientPendingCallback)(void *context, bool hasPending);

/* The desktop's single connection to the server. */
typedef struct Client {
    Conn conn;
    bool connected;
    ClientPendingCallback onPendingCb;
    void *pendingCtx;
} Client;

/* A resolved server address, reused for every reconnect attempt. */
typedef struct {
    struct sockaddr_storage storage;
    socklen_t length;
} ClientAddress;

/* Blocking connect (resolves and waits up to 3 s); kept for tools and tests.
 * A GUI must use clientResolve + clientConnectStart/finish instead. */
Status clientConnect(Client **out, const char *host, uint16_t port);

/**
 * @brief Resolve a host once, so later connect attempts need no DNS.
 *
 * A numeric IPv4/IPv6 address is parsed without any lookup. Otherwise a
 * normal (blocking) lookup runs; call this once at startup, never from a
 * reconnect path, because a DNS timeout would freeze the caller.
 * @param[in] host Numeric address or host name.
 * @param[in] port TCP port (> 0).
 * @param[out] out First usable address.
 * @return OK when resolved; BAD_REQUEST when an argument is NULL or the port
 *         is 0; NOT_FOUND when the name cannot be resolved.
 */
Status clientResolve(const char *host, uint16_t port, ClientAddress *out);

/**
 * @brief Begin a connect that never blocks.
 * @param[in] address Address from clientResolve.
 * @param[out] outFd Non-blocking socket of the attempt, owned by the caller
 *             until clientConnectFinish or clientConnectAbort.
 * @param[out] outInProgress true when the result is pending: wait for the fd
 *             to become writable (or report an error), then call
 *             clientConnectFinish. false when already connected.
 * @return OK when the attempt is underway or done;
 *         BAD_REQUEST when an argument is NULL or connect() failed at once
 *         (e.g. refused or unreachable; no fd is left open);
 *         BUSY when no socket could be created.
 */
Status clientConnectStart(const ClientAddress *address, int *outFd, bool *outInProgress);

/**
 * @brief Complete a connect started by clientConnectStart.
 *
 * Reads SO_ERROR to learn whether the pending connect succeeded.
 * @param[in] connectionSocket Socket from clientConnectStart; ownership passes
 *            to this call (it ends up in the Client or is closed).
 * @param[out] out The connected client on OK, otherwise NULL.
 * @return OK when connected; BAD_REQUEST when @p connectionSocket is negative, @p out is
 *         NULL or the connect failed; BUSY when out of memory.
 */
Status clientConnectFinish(int connectionSocket, Client **out);

/**
 * @brief Cancel a pending connect (timeout or shutdown) and close its fd.
 * @param[in] connectionSocket Socket from clientConnectStart; negative is ignored.
 */
void clientConnectAbort(int connectionSocket);

/**
 * @brief Queue an envelope and try to send it at once.
 *
 * Whatever the kernel does not take now stays queued; call clientFlush when
 * the socket becomes writable. The pending callback reports which case it was.
 *
 * @return OK, an encode/queue error, or BAD_REQUEST when not connected or the
 *         socket broke (the client is then `closing`).
 */
Status clientSend(Client *client, const Envelope *envelope);

/**
 * @brief Read everything available and decode complete frames; never blocks.
 * @return OK, or an error after which the connection should be dropped
 *         (BAD_REQUEST on EOF or a socket error); see connRead.
 */
Status clientRead(Client *client, FrameCallback callback, void *context);

/** @brief Send queued output until done or the socket would block; see connFlush. */
Status clientFlush(Client *client);

/** @brief Register the callback that reports whether output is still queued. */
void clientSetPendingCallback(Client *client, ClientPendingCallback callback, void *context);

/** @brief Socket of a connected client for poll/GIO watches, or -1. */
int clientFd(const Client *client);

/** @brief Close the socket and free the client; NULL is ignored. */
void clientClose(Client *client);

#endif
