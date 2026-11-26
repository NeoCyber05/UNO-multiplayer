#include "server/auth/accounts.h"
#include "server/app.h"
#include "server/storage/store.h"
#include "shared/contracts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void testEmailValidation(void) {
    /* Valid emails: lowercase ASCII, contains @, local and domain >= 1, length 3..254 */
    assert(accountsValidateEmail("test@example.com") == true);
    assert(accountsValidateEmail("user.name+tag@sub.domain.org") == true);
    assert(accountsValidateEmail("a@b.c") == true);
    assert(accountsValidateEmail("u_123-x@uno.vn") == true);

    /* Invalid emails */
    assert(accountsValidateEmail(NULL) == false);
    assert(accountsValidateEmail("") == false);
    assert(accountsValidateEmail("a@") == false);
    assert(accountsValidateEmail("@b.com") == false);
    assert(accountsValidateEmail("testexample.com") == false); /* missing @ */
    assert(accountsValidateEmail("Test@example.com") == false); /* uppercase */
    assert(accountsValidateEmail("USER@DOMAIN.COM") == false); /* uppercase */
    assert(accountsValidateEmail("te st@example.com") == false); /* space */
    assert(accountsValidateEmail("test@exa mple.com") == false); /* space */
    assert(accountsValidateEmail("nguyễn@example.com") == false); /* non-ASCII */

    /* Length boundaries */
    assert(accountsValidateEmail("a@b") == true); /* length 3 */
    assert(accountsValidateEmail("ab") == false); /* length 2 */

    char longEmail[300];
    memset(longEmail, 'a', 250);
    strcpy(longEmail + 250, "@b.com"); /* 256 bytes > 254 */
    assert(accountsValidateEmail(longEmail) == false);

    memset(longEmail, 'a', 248);
    strcpy(longEmail + 248, "@b.c"); /* exactly 252 bytes <= 254 */
    assert(accountsValidateEmail(longEmail) == true);
}

static void testPasswordValidation(void) {
    /* Valid: length 6..128 */
    assert(accountsValidatePassword("123456") == true);
    assert(accountsValidatePassword("P@ssw0rd!#%^&*()") == true);

    char pass128[129];
    memset(pass128, 'x', 128);
    pass128[128] = '\0';
    assert(accountsValidatePassword(pass128) == true);

    /* Invalid */
    assert(accountsValidatePassword(NULL) == false);
    assert(accountsValidatePassword("") == false);
    assert(accountsValidatePassword("12345") == false); /* length 5 < 6 */

    char pass129[130];
    memset(pass129, 'x', 129);
    pass129[129] = '\0';
    assert(accountsValidatePassword(pass129) == false); /* length 129 > 128 */
}

static void testNameValidation(void) {
    /* Valid: UTF-8 1..16 code points, max 64 bytes */
    assert(accountsValidateName("Alice") == true);
    assert(accountsValidateName("A") == true);
    assert(accountsValidateName("Nguyễn Văn A") == true); /* 12 code points */
    assert(accountsValidateName("Trần Thị Hương") == true); /* 14 code points */

    /* 16 code points */
    assert(accountsValidateName("1234567890123456") == true);
    /* 16 Vietnamese code points */
    assert(accountsValidateName("Một hai ba bốn n") == true);

    /* Invalid: 17 code points */
    assert(accountsValidateName("12345678901234567") == false);
    assert(accountsValidateName("Một hai ba bốn nă") == false); /* 17 code points */
    assert(accountsValidateName("Một hai ba bốn năm") == false); /* 18 code points */

    /* Invalid: empty */
    assert(accountsValidateName(NULL) == false);
    assert(accountsValidateName("") == false);

    /* Invalid: control characters */
    assert(accountsValidateName("Alice\n") == false);
    assert(accountsValidateName("Bob\t") == false);

    /* Invalid: broken UTF-8 byte sequences */
    char badUtf8[] = {'A', 'l', (char)0xC3, '\0'}; /* incomplete 2-byte sequence */
    assert(accountsValidateName(badUtf8) == false);

    char badUtf8Second[] = {'A', (char)0xFF, 'B', '\0'}; /* invalid byte */
    assert(accountsValidateName(badUtf8Second) == false);
}

static void testAvatarValidation(void) {
    /* Whitelist from spec: fox, panda, bear, owl, lion, tiger, rabbit, koala */
    assert(accountsValidateAvatar("fox") == true);
    assert(accountsValidateAvatar("panda") == true);
    assert(accountsValidateAvatar("bear") == true);
    assert(accountsValidateAvatar("owl") == true);
    assert(accountsValidateAvatar("lion") == true);
    assert(accountsValidateAvatar("tiger") == true);
    assert(accountsValidateAvatar("rabbit") == true);
    assert(accountsValidateAvatar("koala") == true);

    /* Invalid avatars */
    assert(accountsValidateAvatar(NULL) == false);
    assert(accountsValidateAvatar("") == false);
    assert(accountsValidateAvatar("dog") == false);
    assert(accountsValidateAvatar("dragon") == false);
    assert(accountsValidateAvatar("cat_with_long_name_invalid") == false);
}

static void testTokenGeneration(void) {
    char tokens[20][65];
    for (int i = 0; i < 20; i++) {
        accountsGenerateToken(tokens[i]);
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
    accountsFormatTag(1, tag);
    assert(strcmp(tag, "UNO#1") == 0);

    accountsFormatTag(42, tag);
    assert(strcmp(tag, "UNO#42") == 0);

    accountsFormatTag(100500, tag);
    assert(strcmp(tag, "UNO#100500") == 0);
}

static void testSessionLifecycle(void) {
    App *app = calloc(1, sizeof(App));
    assert(app != NULL);
    assert(appInit(app, "127.0.0.1", 0, ":memory:", 5) == OK);

    /* Allocate slot 0 with generation 1 */
    app->slots[0].active = true;
    app->slots[0].generation = 1;

    ConnId conn1 = {0, 1};
    UserId outUserId = 0;

    /* Before authentication, sessionsUser returns UNAUTHENTICATED */
    assert(sessionsUser(app, conn1, &outUserId) == UNAUTHENTICATED);

    /* Bind session to conn1 */
    char token1[65];
    accountsGenerateToken(token1);
    assert(sessionsBind(app, conn1, 42, token1) == OK);

    /* Now sessionsUser returns UserId 42 */
    assert(sessionsUser(app, conn1, &outUserId) == OK);
    assert(outUserId == 42);

    /* Stale generation conn returns UNAUTHENTICATED */
    ConnId staleConn = {0, 99};
    assert(sessionsUser(app, staleConn, &outUserId) == UNAUTHENTICATED);

    /* Resume from a new connection (slot 1, generation 1) using token1 */
    app->slots[1].active = true;
    app->slots[1].generation = 1;
    ConnId conn2 = {1, 1};

    /* Resume with wrong token fails */
    const char *invalidToken = "invalid_token_0000000000000000000000000000000000000000000000000000000";
    assert(sessionsResume(app, conn2, invalidToken) == UNAUTHENTICATED);

    /* Resume with valid token1 succeeds */
    assert(sessionsResume(app, conn2, token1) == OK);
    outUserId = 0;
    assert(sessionsUser(app, conn2, &outUserId) == OK);
    assert(outUserId == 42);

    /* The previous connection conn1 should be unbound after new login/resume */
    assert(sessionsUser(app, conn1, &outUserId) == UNAUTHENTICATED);

    /* Disconnect conn2 */
    sessionsDisconnect(app, conn2);
    assert(sessionsUser(app, conn2, &outUserId) == UNAUTHENTICATED);

    /* But token1 can still be resumed after disconnect! */
    app->slots[2].active = true;
    app->slots[2].generation = 1;
    ConnId conn3 = {2, 1};
    assert(sessionsResume(app, conn3, token1) == OK);
    assert(sessionsUser(app, conn3, &outUserId) == OK);
    assert(outUserId == 42);

    /* Logout explicitly revokes token */
    sessionsLogout(app, conn3);
    assert(sessionsUser(app, conn3, &outUserId) == UNAUTHENTICATED);

    /* Resuming with revoked token now fails */
    assert(sessionsResume(app, conn3, token1) == UNAUTHENTICATED);

    appCleanup(app);
    free(app);
}

static void testToken24hExpiration(void) {
    App *app = calloc(1, sizeof(App));
    assert(app != NULL);
    assert(appInit(app, "127.0.0.1", 0, ":memory:", 5) == OK);

    app->slots[0].active = true;
    app->slots[0].generation = 1;
    ConnId conn1 = {0, 1};

    char token1[65];
    accountsGenerateToken(token1);
    assert(sessionsBind(app, conn1, 42, token1) == OK);

    /* Find the session entry for token1 */
    int sessionIndex = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (app->sessions->entries[i].inUse && strcmp(app->sessions->entries[i].token,
                                                       token1) == 0) {
            sessionIndex = i;
            break;
        }
    }
    assert(sessionIndex >= 0);

    /* Disconnect conn1 */
    sessionsDisconnect(app, conn1);

    /* Setup conn2 to attempt resume */
    app->slots[1].active = true;
    app->slots[1].generation = 1;
    ConnId conn2 = {1, 1};

    /* 1. Test within 24h (23 hours ago) */
    uint64_t now = app->sessions->entries[sessionIndex].createdAtMs;
    app->sessions->entries[sessionIndex].createdAtMs = now - 23ULL * 3600ULL * 1000ULL;

    assert(sessionsResume(app, conn2, token1) == OK);
    UserId outUserId = 0;
    assert(sessionsUser(app, conn2, &outUserId) == OK);
    assert(outUserId == 42);

    /* Disconnect conn2 */
    sessionsDisconnect(app, conn2);

    /* Setup conn3 to attempt resume */
    app->slots[2].active = true;
    app->slots[2].generation = 1;
    ConnId conn3 = {2, 1};

    /* 2. Test over 24h (24 hours and 1 second ago) */
    now = app->sessions->entries[sessionIndex].createdAtMs;
    app->sessions->entries[sessionIndex].createdAtMs = now - 24ULL * 3600ULL * 1000ULL - 1000ULL;

    assert(sessionsResume(app, conn3, token1) == UNAUTHENTICATED);
    /* Verify slot is cleared */
    assert(app->sessions->entries[sessionIndex].inUse == false);
    assert(sessionsUser(app, conn3, &outUserId) == UNAUTHENTICATED);

    appCleanup(app);
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
