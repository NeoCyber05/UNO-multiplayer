#include "server/app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DEFAULT_SERVER_PORT = 9090,
    /* Giới hạn số yêu cầu REGISTER và LOGIN mỗi phút theo địa chỉ IP. */
    DEFAULT_AUTH_LIMIT = 5
};

/* Lắng nghe trên mọi giao diện IPv4 để máy trong mạng LAN kết nối được. */
static const char DEFAULT_BIND_IP[] = "0.0.0.0";

/**
 * @brief Đọc cấu hình dòng lệnh và khởi chạy máy chủ UNO.
 * @param[in] argc Số đối số, tính cả tên chương trình.
 * @param[in] argv Các đối số ghi đè cấu hình mặc định.
 * @return 0 khi in hướng dẫn hoặc máy chủ thoát vòng lặp, 1 nếu khởi động thất bại.
 */
int main(int argc, char **argv) {
    const char *bindIp = DEFAULT_BIND_IP;
    uint16_t port = DEFAULT_SERVER_PORT;
    const char *dbPath = "uno.db";
    int authLimit = DEFAULT_AUTH_LIMIT;
    int heartbeatMs = APP_DEFAULT_HEARTBEAT_MS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bindIp = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            dbPath = argv[++i];
        } else if (strcmp(argv[i], "--auth-limit") == 0 && i + 1 < argc) {
            authLimit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--heartbeat-ms") == 0 && i + 1 < argc) {
            /* Cho phép kiểm thử heartbeat nhanh hơn. appRun dùng mặc định nếu giá trị <= 0. */
            heartbeatMs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--bind <ip>] [--port <port>] [--db <path>] "
                   "[--auth-limit <num>] [--heartbeat-ms <ms>]\n", argv[0]);
            return 0;
        }
    }

    return appRun(bindIp, port, dbPath, authLimit, heartbeatMs);
}
