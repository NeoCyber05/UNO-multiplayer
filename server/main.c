#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *bind_ip = "0.0.0.0";
    uint16_t port = 9090;
    const char *db_path = "uno.db";
    int auth_limit = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bind_ip = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(argv[i], "--auth-limit") == 0 && i + 1 < argc) {
            auth_limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--bind <ip>] [--port <port>] [--db <path>] [--auth-limit <num>]\n", argv[0]);
            return 0;
        }
    }

    return app_run(bind_ip, port, db_path, auth_limit);
}
