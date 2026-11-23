#ifndef UNO_TCP_CLIENT_H
#define UNO_TCP_CLIENT_H

#include "shared/contracts.h"
#include "shared/net.h"

typedef void (*ClientPendingCallback)(void *ctx, bool has_pending);

typedef struct Client {
    Conn conn;
    bool connected;
    ClientPendingCallback on_pending_cb;
    void *pending_ctx;
} Client;

Status client_connect(Client **out, const char *host, uint16_t port);
Status client_send(Client *client, const Envelope *e);
Status client_read(Client *client, FrameCallback cb, void *ctx);
Status client_flush(Client *client);
void client_set_pending_callback(Client *client, ClientPendingCallback cb, void *ctx);
int client_fd(const Client *client);
void client_close(Client *client);

#endif
