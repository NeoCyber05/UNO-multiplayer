#ifndef UNO_TCP_CLIENT_H
#define UNO_TCP_CLIENT_H

#include "shared/contracts.h"
#include "shared/net.h"

Status client_connect(Client **out, const char *host, uint16_t port);
Status client_send(Client *client, const Envelope *e);
Status client_poll(Client *client, FrameCallback cb, void *ctx, int timeout_ms);
Status client_read(Client *client, FrameCallback cb, void *ctx);
int client_fd(const Client *client);
void client_close(Client *client);

#endif
