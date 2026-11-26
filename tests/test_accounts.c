#include "server/accounts.h"
#include "server/app.h"
#include "server/store.h"
#include "shared/contracts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void testEmailValidation(void) {
    /* Valid emails: lowercase ASCII, contains @, local and domain >= 1, length 3..254 */
    assert(accounts_validate_email("test@example.com") == true);
    assert(accounts_validate_email("user.name+tag@sub.domain.org") == true);
    assert(accounts_validate_email("a@b.c") == true);
    assert(accounts_validate_email("u_123-x@uno.vn") == true);

    /* Invalid emails */
    assert(accounts_validate_email(NULL) == false);
    assert(accounts_validate_email("") == false);
    assert(accounts_validate_email("a@") == false);
    assert(accounts_validate_email("@b.com") == false);
    assert(accounts_validate_email("testexample.com") == false); /* missing @ */
    assert(accounts_validate_email("Test@example.com") == false); /* uppercase */
    assert(accounts_validate_email("USER@DOMAIN.COM") == false); /* uppercase */
    assert(accounts_validate_email("te st@example.com") == false); /* space */
    assert(accounts_validate_email("test@exa mple.com") == false); /* space */
    assert(accounts_validate_email("nguyễn@example.com") == false); /* non-ASCII */

    /* Length boundaries */
    assert(accounts_validate_email("a@b") == true); /* length 3 */
    assert(accounts_validate_email("ab") == false); /* length 2 */

    char longEmail[300];
    memset(longEmail, 'a', 250);
    strcpy(longEmail + 250, "@b.com"); /* 256 bytes > 254 */
    assert(accounts_validate_email(longEmail) == false);

    memset(longEmail, 'a', 248);
    strcpy(longEmail + 248, "@b.c"); /* exactly 252 bytes <= 254 */
    assert(accounts_validate_email(longEmail) == true);
}

static void testPasswordValidation(void) {
    /* Valid: length 6..128 */
    assert(accounts_validate_password("123456") == true);
    assert(accounts_validate_password("P@ssw0rd!#%^&*()") == true);

    char pass128[129];
    memset(pass128, 'x', 128);
    pass128[128] = '\0';
    assert(accounts_validate_password(pass128) == true);

    /* Invalid */
    assert(accounts_validate_password(NULL) == false);
    assert(accounts_validate_password("") == false);
    assert(accounts_validate_password("12345") == false); /* length 5 < 6 */

    char pass129[130];
    memset(pass129, 'x', 129);
    pass129[129] = '\0';
    assert(accounts_validate_password(pass129) == false); /* length 129 > 128 */
}

static void testNameValidation(void) {
    /* Valid: UTF-8 1..16 code points, max 64 bytes */
    assert(accounts_validate_name("Alice") == true);
    assert(accounts_validate_name("A") == true);
    assert(accounts_validate_name("Nguyễn Văn A") == true); /* 12 code points */
    assert(accounts_validate_name("Trần Thị Hương") == true); /* 14 code points */

    /* 16 code points */
    assert(accounts_validate_name("1234567890123456") == true);
    /* 16 Vietnamese code points */
    assert(accounts_validate_name("Một hai ba bốn n") == true);

    /* Invalid: 17 code points */
    assert(accounts_validate_name("12345678901234567") == false);
    assert(accounts_validate_name("Một hai ba bốn nă") == false); /* 17 code points */
    assert(accounts_validate_name("Một hai ba bốn năm") == false); /* 18 code points */

    /* Invalid: empty */
    assert(accounts_validate_name(NULL) == false);
    assert(accounts_validate_name("") == false);

    /* Invalid: control characters */
    assert(accounts_validate_name("Alice\n") == false);
    assert(accounts_validate_name("Bob\t") == false);

    /* Invalid: broken UTF-8 byte sequences */
    char badUtf8[] = {'A', 'l', (char)0xC3, '\0'}; /* incomplete 2-byte sequence */
    assert(accounts_validate_name(badUtf8) == false);

    char badUtf8Second[] = {'A', (char)0xFF, 'B', '\0'}; /* invalid byte */
    assert(accounts_validate_name(badUtf8Second) == false);
}

static void testAvatarValidation(void) {
    /* Whitelist from spec: fox, panda, bear, owl, lion, tiger, rabbit, koala */
    assert(accounts_validate_avatar("fox") == true);
    assert(accounts_validate_avatar("panda") == true);
    assert(accounts_validate_avatar("bear") == true);
    assert(accounts_validate_avatar("owl") == true);
    assert(accounts_validate_avatar("lion") == true);
    assert(accounts_validate_avatar("tiger") == true);
    assert(accounts_validate_avatar("rabbit") == true);
    assert(accounts_validate_avatar("koala") == true);

    /* Invalid avatars */
    assert(accounts_validate_avatar(NULL) == false);
    assert(accounts_validate_avatar("") == false);
    assert(accounts_validate_avatar("dog") == false);
    assert(accounts_validate_avatar("dragon") == false);
    assert(accounts_validate_avatar("cat_with_long_name_invalid") == false);
}

static void testTokenGeneration(void) {
    char tokens[20][65];
    for (int i = 0; i < 20; i++) {
        accounts_generate_token(tokens[i]);
        assert(strlen(tokens[i]) == 64);
        /* Check lowercase hex chars */
        for (int j = 0; j < 64; j++) {
            char character = tokens[i][j];
            bool isDigit = character >= '0' && character <= '9';
            assert(isDigit || (character >= 'a' && character <= 'f'));
        }
        /* Check uniqueness among generated tokens */
        for (int k = 0; k < i; k++) {
            assert(strcmp(tokens[i], tokens[k]) != 0);
        }
    }
}

static void testTagGeneration(void) {
    char tag[32];
    accounts_format_tag(1, tag);
    assert(strcmp(tag, "UNO#1") == 0);

    accounts_format_tag(42, tag);
    assert(strcmp(tag, "UNO#42") == 0);

    accounts_format_tag(100500, tag);
    assert(strcmp(tag, "UNO#100500") == 0);
}

static void testSessionLifecycle(void) {
    App *app = calloc(1, sizeof(App));
    assert(app != NULL);
    assert(app_init(app, "127.0.0.1", 0, ":memory:", 5) == OK);

    /* Allocate slot 0 with generation 1 */
    app->slots[0].active = true;
    app->slots[0].generation = 1;

    ConnId conn1 = {0, 1};
    UserId outUserId = 0;

    /* Before authentication, sessions_user returns UNAUTHENTICATED */
    assert(sessions_user(app, conn1, &outUserId) == UNAUTHENTICATED);

    /* Bind session to conn1 */
    char token1[65];
    accounts_generate_token(token1);
    assert(sessions_bind(app, conn1, 42, token1) == OK);

    /* Now sessions_user returns UserId 42 */
    assert(sessions_user(app, conn1, &outUserId) == OK);
    assert(outUserId == 42);

    /* Stale generation conn returns UNAUTHENTICATED */
    ConnId staleConn = {0, 99};
    assert(sessions_user(app, staleConn, &outUserId) == UNAUTHENTICATED);

    /* Resume from a new connection (slot 1, generation 1) using token1 */
    app->slots[1].active = true;
    app->slots[1].generation = 1;
    ConnId conn2 = {1, 1};

    /* Resume with wrong token fails */
    const char *invalidToken = "invalid_token_0000000000000000000000000000000000000000000000000000000";
    assert(sessions_resume(app, conn2, invalidToken) == UNAUTHENTICATED);

    /* Resume with valid token1 succeeds */
    assert(sessions_resume(app, conn2, token1) == OK);
    outUserId = 0;
    assert(sessions_user(app, conn2, &outUserId) == OK);
    assert(outUserId == 42);

    /* The previous connection conn1 should be unbound after new login/resume */
    assert(sessions_user(app, conn1, &outUserId) == UNAUTHENTICATED);

    /* Disconnect conn2 */
    sessions_disconnect(app, conn2);
    assert(sessions_user(app, conn2, &outUserId) == UNAUTHENTICATED);

    /* But token1 can still be resumed after disconnect! */
    app->slots[2].active = true;
    app->slots[2].generation = 1;
    ConnId conn3 = {2, 1};
    assert(sessions_resume(app, conn3, token1) == OK);
    assert(sessions_user(app, conn3, &outUserId) == OK);
    assert(outUserId == 42);

    /* Logout explicitly revokes token */
    sessions_logout(app, conn3);
    assert(sessions_user(app, conn3, &outUserId) == UNAUTHENTICATED);

    /* Resuming with revoked token now fails */
    assert(sessions_resume(app, conn3, token1) == UNAUTHENTICATED);

    app_cleanup(app);
    free(app);
}

static void testToken24hExpiration(void) {
    App *app = calloc(1, sizeof(App));
    assert(app != NULL);
    assert(app_init(app, "127.0.0.1", 0, ":memory:", 5) == OK);

    app->slots[0].active = true;
    app->slots[0].generation = 1;
    ConnId conn1 = {0, 1};

    char token1[65];
    accounts_generate_token(token1);
    assert(sessions_bind(app, conn1, 42, token1) == OK);

    /* Find the session entry for token1 */
    int sessionIndex = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (app->sessions->entries[i].in_use && strcmp(app->sessions->entries[i].token,
                                                       token1) == 0) {
            sessionIndex = i;
            break;
        }
    }
    assert(sessionIndex >= 0);

    /* Disconnect conn1 */
    sessions_disconnect(app, conn1);

    /* Setup conn2 to attempt resume */
    app->slots[1].active = true;
    app->slots[1].generation = 1;
    ConnId conn2 = {1, 1};

    /* 1. Test within 24h (23 hours ago) */
    uint64_t now = app->sessions->entries[sessionIndex].created_at_ms;
    app->sessions->entries[sessionIndex].created_at_ms = now - 23ULL * 3600ULL * 1000ULL;

    assert(sessions_resume(app, conn2, token1) == OK);
    UserId outUserId = 0;
    assert(sessions_user(app, conn2, &outUserId) == OK);
    assert(outUserId == 42);

    /* Disconnect conn2 */
    sessions_disconnect(app, conn2);

    /* Setup conn3 to attempt resume */
    app->slots[2].active = true;
    app->slots[2].generation = 1;
    ConnId conn3 = {2, 1};

    /* 2. Test over 24h (24 hours and 1 second ago) */
    now = app->sessions->entries[sessionIndex].created_at_ms;
    app->sessions->entries[sessionIndex].created_at_ms = now - 24ULL * 3600ULL * 1000ULL - 1000ULL;

    assert(sessions_resume(app, conn3, token1) == UNAUTHENTICATED);
    /* Verify slot is cleared */
    assert(app->sessions->entries[sessionIndex].in_use == false);
    assert(sessions_user(app, conn3, &outUserId) == UNAUTHENTICATED);

    app_cleanup(app);
    free(app);
}

int main(void) {
    testEmailValidation();
    testPasswordValidation();
    testNameValidation();
    testAvatarValidation();
    testTokenGeneration();
    testTagGeneration();
    testSessionLifecycle();
    testToken24hExpiration();

    puts("accounts: OK");
    return 0;
}
