#include "server/lobby/social.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { SAMPLE_USER_ID = 42 };

static void testParseTag(void) {
    UserId userId = 0;
    assert(socialParseTag("UNO#42", &userId));
    assert(userId == SAMPLE_USER_ID);
    /* The largest id SQLite can store still parses. */
    assert(socialParseTag("UNO#9223372036854775807", &userId));
    assert(userId == 9223372036854775807ULL);

    const char *const rejected[] = {
        "UNO#", "uno#1", "UNO#-1", "UNO# 1", "UNO#99999999999999999999",
        "UNO#1 ", "UNO#+1", "UNO#0", "UNO#007", "UNO#9223372036854775808",
        "UNO#1a", " UNO#1", "UNO1", ""
    };
    for (size_t index = 0; index < sizeof rejected / sizeof rejected[0]; index++) {
        userId = SAMPLE_USER_ID;
        assert(!socialParseTag(rejected[index], &userId));
        /* A rejected tag never leaves a half-parsed id behind. */
        assert(userId == SAMPLE_USER_ID);
    }
    assert(!socialParseTag(NULL, &userId));
    assert(!socialParseTag("UNO#1", NULL));
}

static void testPresenceKeys(void) {
    assert(strcmp(socialPresenceKey(PRESENCE_OFFLINE), "offline") == 0);
    assert(strcmp(socialPresenceKey(PRESENCE_IDLE), "idle") == 0);
    assert(strcmp(socialPresenceKey(PRESENCE_ONLINE), "online") == 0);
    assert(strcmp(socialPresenceKey(PRESENCE_IN_MATCH), "in_match") == 0);
}

int main(void) {
    testParseTag();
    testPresenceKeys();
    puts("social: OK");
    return 0;
}
