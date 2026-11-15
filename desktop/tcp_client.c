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

struct Client {
    Conn conn;
    bool connected;
};

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

Status client_send(Client *client, const Envelope *e)
{
    if (!client || !client->connected || client->conn.fd < 0 || !e) return BAD_REQUEST;
    Status s = conn_queue(&client->conn, e);
    if (s != OK) return s;
    return conn_flush(&client->conn);
}

Status client_poll(Client *client, FrameCallback cb, void *ctx, int timeout_ms)
{
    if (!client || !client->connected || client->conn.fd < 0) return BAD_REQUEST;

    struct pollfd pfd;
    pfd.fd = client->conn.fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (client->conn.out_len > client->conn.out_pos) {
        pfd.events |= POLLOUT;
    }

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) return OK;
        return BAD_REQUEST;
    }
    if (ret == 0) return OK;

    if (pfd.revents & POLLOUT) {
        Status s = conn_flush(&client->conn);
        if (s != OK) return s;
    }

    if (pfd.revents & POLLIN) {
        Status s = conn_read(&client->conn, cb, ctx);
        if (s != OK) return s;
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        if (client->conn.closing) return BAD_REQUEST;
    }

    return OK;
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
