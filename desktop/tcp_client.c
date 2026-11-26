#define _POSIX_C_SOURCE 200809L
#include "tcp_client.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    /* How long the blocking clientConnect waits for each address's handshake. */
    CONNECT_TIMEOUT_MS = 3000
};

Status clientConnect(Client **out, const char *host, uint16_t port) {
    if (!out || !host || port == 0) return BAD_REQUEST;
    *out = NULL;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portText[16];
    snprintf(portText, sizeof(portText), "%u", port);

    struct addrinfo *addressList = NULL;
    int lookupError = getaddrinfo(host, portText, &hints, &addressList);
    if (lookupError != 0 || !addressList) {
        return NOT_FOUND;
    }

    // Try each resolved address (IPv4 or IPv6) until one accepts the connection.
    int connectionSocket = -1;
    for (struct addrinfo *candidate = addressList; candidate != NULL;
         candidate = candidate->ai_next) {
        connectionSocket = socket(candidate->ai_family, candidate->ai_socktype,
                                  candidate->ai_protocol);
        if (connectionSocket < 0) continue;

        /* Non-blocking connect plus poll() bounds the wait by CONNECT_TIMEOUT_MS
         * instead of the kernel's much longer SYN retry timeout.
         * FIXME: fcntl(F_SETFL) failure is ignored, so connect() could block.
         * Close this socket and try the next address instead. */
        int flags = fcntl(connectionSocket, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(connectionSocket, F_SETFL, flags | O_NONBLOCK);
        }

        int connectResult = connect(connectionSocket, candidate->ai_addr, candidate->ai_addrlen);
        if (connectResult == 0) {
            break;
        }

        if (connectResult < 0 && errno == EINPROGRESS) {
            struct pollfd pollFd;
            pollFd.fd = connectionSocket;
            pollFd.events = POLLOUT;
            pollFd.revents = 0;
            int pollResult = poll(&pollFd, 1, CONNECT_TIMEOUT_MS);
            if (pollResult > 0 && (pollFd.revents & POLLOUT)) {
                /* Writable only means the handshake ended; SO_ERROR says how. */
                int socketError = 0;
                socklen_t length = sizeof(socketError);
                if (getsockopt(connectionSocket, SOL_SOCKET, SO_ERROR, &socketError,
                               &length) == 0 && socketError == 0) {
                    break;
                }
            }
        }

        close(connectionSocket);
        connectionSocket = -1;
    }
    freeaddrinfo(addressList);

    if (connectionSocket < 0) {
        return BAD_REQUEST;
    }

    Client *client = calloc(1, sizeof(Client));
    if (!client) {
        close(connectionSocket);
        return BUSY;
    }

    connInit(&client->conn, connectionSocket, 0, 0);
    client->connected = true;
    *out = client;
    return OK;
}

static Status lookupFirstAddress(const char *host, const char *service, int flags,
                                 ClientAddress *out) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = flags;

    struct addrinfo *results = NULL;
    if (getaddrinfo(host, service, &hints, &results) != 0 || results == NULL) {
        return NOT_FOUND;
    }
    Status status = NOT_FOUND;
    if (results->ai_addrlen <= sizeof(out->storage)) {
        memset(out, 0, sizeof(*out));
        memcpy(&out->storage, results->ai_addr, results->ai_addrlen);
        out->length = results->ai_addrlen;
        status = OK;
    }
    freeaddrinfo(results);
    return status;
}

Status clientResolve(const char *host, uint16_t port, ClientAddress *out) {
    if (!host || port == 0 || !out) return BAD_REQUEST;

    char service[16];
    snprintf(service, sizeof(service), "%u", port);

    /* Numeric first: an IP from the command line must never touch DNS. */
    if (lookupFirstAddress(host, service, AI_NUMERICHOST | AI_NUMERICSERV, out) == OK) {
        return OK;
    }
    /* Only for host names, and only once per process (blocking lookup). */
    return lookupFirstAddress(host, service, AI_NUMERICSERV | AI_ADDRCONFIG, out);
}

Status clientConnectStart(const ClientAddress *address, int *outFd, bool *outInProgress) {
    if (!address || !outFd || !outInProgress || address->length == 0) return BAD_REQUEST;
    *outFd = -1;
    *outInProgress = false;

    const struct sockaddr *target = (const struct sockaddr *)&address->storage;
    int connectionSocket = socket(target->sa_family, SOCK_STREAM, 0);
    if (connectionSocket < 0) return BUSY;

    /* Non-blocking before connect() so the call returns at once with
     * EINPROGRESS instead of waiting for the handshake. */
    int flags = fcntl(connectionSocket, F_GETFL, 0);
    if (flags < 0 || fcntl(connectionSocket, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(connectionSocket);
        return BUSY;
    }

    if (connect(connectionSocket, target, address->length) == 0) {
        *outFd = connectionSocket;
        return OK;
    }
    /* EINTR on a non-blocking connect also continues in the background. */
    if (errno == EINPROGRESS || errno == EINTR) {
        *outFd = connectionSocket;
        *outInProgress = true;
        return OK;
    }
    close(connectionSocket);
    return BAD_REQUEST;
}

Status clientConnectFinish(int connectionSocket, Client **out) {
    if (out) *out = NULL;
    if (connectionSocket < 0 || !out) return BAD_REQUEST;

    /* The pending connect's outcome is only visible through SO_ERROR. */
    int sockError = 0;
    socklen_t length = sizeof(sockError);
    if (getsockopt(connectionSocket, SOL_SOCKET, SO_ERROR, &sockError,
                   &length) != 0 || sockError != 0) {
        close(connectionSocket);
        return BAD_REQUEST;
    }

    Client *client = calloc(1, sizeof(Client));
    if (!client) {
        close(connectionSocket);
        return BUSY;
    }
    if (connInit(&client->conn, connectionSocket, 0, 0) != OK) {
        close(connectionSocket);
        free(client);
        return BAD_REQUEST;
    }
    client->connected = true;
    *out = client;
    return OK;
}

void clientConnectAbort(int connectionSocket) {
    if (connectionSocket >= 0) {
        close(connectionSocket);
    }
}

Status clientSend(Client *client, const Envelope *envelope) {
    if (!client || !client->connected || client->conn.fd < 0 || !envelope) return BAD_REQUEST;
    Status status = connQueue(&client->conn, envelope);
    if (status != OK) return status;
    Status flushStatus = connFlush(&client->conn);
    if (client->onPendingCb) {
        client->onPendingCb(client->pendingCtx, client->conn.outLen > client->conn.outPos);
    }
    return flushStatus;
}

Status clientFlush(Client *client) {
    if (!client || !client->connected || client->conn.fd < 0) return BAD_REQUEST;
    Status status = connFlush(&client->conn);
    if (client->onPendingCb) {
        client->onPendingCb(client->pendingCtx, client->conn.outLen > client->conn.outPos);
    }
    return status;
}

void clientSetPendingCallback(Client *client, ClientPendingCallback callback, void *context) {
    if (!client) return;
    client->onPendingCb = callback;
    client->pendingCtx = context;
}

Status clientRead(Client *client, FrameCallback callback, void *context) {
    if (!client || !client->connected || client->conn.fd < 0) return BAD_REQUEST;
    return connRead(&client->conn, callback, context);
}

int clientFd(const Client *client) {
    return (client && client->connected) ? client->conn.fd : -1;
}

void clientClose(Client *client) {
    if (!client) return;
    connClose(&client->conn);
    client->connected = false;
    free(client);
}
