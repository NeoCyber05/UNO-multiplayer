#include "lobby_common.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { DECIMAL_BASE = 10 };

/* Cùng thứ tự với enum Mode và Profile.modes. */
static const char *const MODE_NAMES[LOBBY_MODE_COUNT] = {"classic", "2v2", "side", "draft"};

const char *lobbyModeName(Mode mode) {
    return (unsigned)mode < LOBBY_MODE_COUNT ? MODE_NAMES[mode] : MODE_NAMES[CLASSIC];
}

bool lobbyParseModeName(const char *name, Mode *outMode) {
    if (!name) return false;
    for (int mode = 0; mode < LOBBY_MODE_COUNT; mode++) {
        if (strcmp(name, MODE_NAMES[mode]) == 0) {
            *outMode = (Mode)mode;
            return true;
        }
    }
    return false;
}

json_object *lobbyUserIdJson(UserId user) {
    char idText[LOBBY_USER_ID_TEXT_SIZE];
    snprintf(idText, sizeof idText, "%llu", (unsigned long long)user);
    return json_object_new_string(idText);
}

/* Chỉ nhận dạng thập phân chuẩn: wire không bao giờ có dấu, khoảng trắng hay số 0
 * đệm, và giá trị trên INT64_MAX không thể là rowid của SQLite. */
bool lobbyParseUserIdText(const char *text, UserId *outUser) {
    if (!text || text[0] < '1' || text[0] > '9') return false;
    uint64_t value = 0;
    for (const char *cursor = text; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') return false;
        uint64_t digit = (uint64_t)(*cursor - '0');
        /* Kiểm tra trước khi nhân để phép tính không bao giờ tràn. */
        if (value > ((uint64_t)INT64_MAX - digit) / DECIMAL_BASE) return false;
        value = value * DECIMAL_BASE + digit;
    }
    *outUser = (UserId)value;
    return true;
}

bool lobbyParseUserIdJson(json_object *value, UserId *outUser) {
    if (json_object_is_type(value, json_type_string)) {
        return lobbyParseUserIdText(json_object_get_string(value), outUser);
    }
    if (json_object_is_type(value, json_type_int)) {
        int64_t number = json_object_get_int64(value);
        if (number <= 0) return false;
        *outUser = (UserId)number;
        return true;
    }
    return false;
}

void lobbyFallbackName(UserId user, char *outName, size_t size) {
    snprintf(outName, size, "Player%llu", (unsigned long long)user);
}
