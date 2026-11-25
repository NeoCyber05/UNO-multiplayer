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

Status client_connect(Client **out, const char *host, uint16_t port)
{
    if (!out || !host || port == 0) return BAD_REQUEST;
    *out = NULL;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo *res = NULL;
    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || !res) {
        return NOT_FOUND;
    }

    int fd = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        int ret = connect(fd, p->ai_addr, p->ai_addrlen);
        if (ret == 0) {
            break;
        }

        if (ret < 0 && errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 3000);
            if (pr > 0 && (pfd.revents & POLLOUT)) {
                int sock_err = 0;
                socklen_t len = sizeof(sock_err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) == 0 && sock_err == 0) {
                    break;
                }
            }
        }

        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        return BAD_REQUEST;
    }

    Client *c = calloc(1, sizeof(Client));
    if (!c) {
        close(fd);
        return BUSY;
    }

    conn_init(&c->conn, fd, 0, 0);
    c->connected = true;
    *out = c;
    return OK;
}

static Status lookup_first(const char *host, const char *service, int flags, ClientAddress *out)
{
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

Status client_resolve(const char *host, uint16_t port, ClientAddress *out)
{
    if (!host || port == 0 || !out) return BAD_REQUEST;

    char service[16];
    snprintf(service, sizeof(service), "%u", port);

    /* Numeric first: an IP from the command line must never touch DNS. */
    if (lookup_first(host, service, AI_NUMERICHOST | AI_NUMERICSERV, out) == OK) {
        return OK;
    }
    /* Only for host names, and only once per process (blocking lookup). */
    return lookup_first(host, service, AI_NUMERICSERV | AI_ADDRCONFIG, out);
}

Status client_connect_start(const ClientAddress *address, int *fd_out, bool *in_progress)
{
    if (!address || !fd_out || !in_progress || address->length == 0) return BAD_REQUEST;
    *fd_out = -1;
    *in_progress = false;

    const struct sockaddr *target = (const struct sockaddr *)&address->storage;
    int fd = socket(target->sa_family, SOCK_STREAM, 0);
    if (fd < 0) return BUSY;

    /* Non-blocking before connect() so the call returns at once with
     * EINPROGRESS instead of waiting for the handshake. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return BUSY;
    }

    if (connect(fd, target, address->length) == 0) {
        *fd_out = fd;
        return OK;
    }
    /* EINTR on a non-blocking connect also continues in the background. */
    if (errno == EINPROGRESS || errno == EINTR) {
        *fd_out = fd;
        *in_progress = true;
        return OK;
    }
    close(fd);
    return BAD_REQUEST;
}

Status client_connect_finish(int fd, Client **out)
{
    if (out) *out = NULL;
    if (fd < 0 || !out) return BAD_REQUEST;

    /* The pending connect's outcome is only visible through SO_ERROR. */
    int sockError = 0;
    socklen_t length = sizeof(sockError);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockError, &length) != 0 || sockError != 0) {
        close(fd);
        return BAD_REQUEST;
    }

    Client *client = calloc(1, sizeof(Client));
    if (!client) {
        close(fd);
        return BUSY;
    }
    if (conn_init(&client->conn, fd, 0, 0) != OK) {
        close(fd);
        free(client);
        return BAD_REQUEST;
    }
    client->connected = true;
    *out = client;
    return OK;
}

void client_connect_abort(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

Status client_send(Client *client, const Envelope *e)
{
    if (!client || !client->connected || client->conn.fd < 0 || !e) return BAD_REQUEST;
    Status s = conn_queue(&client->conn, e);
    if (s != OK) return s;
    Status ret = conn_flush(&client->conn);
    if (client->on_pending_cb) {
        client->on_pending_cb(client->pending_ctx, client->conn.out_len > client->conn.out_pos);
    }
    return ret;
}

Status client_flush(Client *client)
{
    if (!client || !client->connected || client->conn.fd < 0) return BAD_REQUEST;
    Status s = conn_flush(&client->conn);
    if (client->on_pending_cb) {
        client->on_pending_cb(client->pending_ctx, client->conn.out_len > client->conn.out_pos);
    }
    return s;
}

void client_set_pending_callback(Client *client, ClientPendingCallback cb, void *ctx)
{
    if (!client) return;
    client->on_pending_cb = cb;
    client->pending_ctx = ctx;
}

Status client_read(Client *client, FrameCallback cb, void *ctx)
{
    if (!client || !client->connected || client->conn.fd < 0) return BAD_REQUEST;
    return conn_read(&client->conn, cb, ctx);
}

int client_fd(const Client *client)
{
    return (client && client->connected) ? client->conn.fd : -1;
}

void client_close(Client *client)
{
    if (!client) return;
    conn_close(&client->conn);
    client->connected = false;
    free(client);
}
