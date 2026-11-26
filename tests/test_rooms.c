#include "server/rooms.h"
#include "server/app.h"
#include "server/accounts.h"
#include "shared/contracts.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static Status startSucceeds(App *app, const RoomStart *roomStart, MatchId *outMatchId) {
    (void)app;
    (void)roomStart;
    strcpy(*outMatchId, "123e4567-e89b-12d3-a456-426614174000");
    return OK;
}

static Status startFails(App *app, const RoomStart *roomStart, MatchId *outMatchId) {
    (void)app;
    (void)roomStart;
    (void)outMatchId;
    return BAD_REQUEST;
}

static void testRoomCodeGeneration(void) {
    Rooms *rooms = NULL;
    assert(roomsInit(&rooms, NULL, NULL) == OK);

    /* Generate multiple codes and check format */
    for (int i = 0; i < 50; i++) {
        char code[7] = {0};
        roomsGenerateCode(rooms, code);
        assert(strlen(code) == 6);
        for (int j = 0; j < 6; j++) {
            char character = code[j];
            bool valid = (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9');
            assert(valid);
        }
    }

    /* Test collision retry logic */
    /* Set up an active room with code "XYZ123" */
    rooms->rooms[0].active = true;
    strcpy(rooms->rooms[0].code, "XYZ123");

    /* Generating a code should not return "XYZ123" */
    for (int i = 0; i < 50; i++) {
        char code[7] = {0};
        roomsGenerateCode(rooms, code);
        assert(strcmp(code, "XYZ123") != 0);
    }

    roomsDestroy(rooms);
}

static void testRoomLimits(void) {
    Rooms *rooms = NULL;
    assert(roomsInit(&rooms, NULL, NULL) == OK);

    /* Maximum 16 concurrent rooms */
    for (int i = 0; i < MAX_ROOMS; i++) {
        char code[7] = {0};
        UserId hostId = 1000 + i;
        Status status = roomsCreate(rooms, hostId, "Host", "fox", CLASSIC, code);
        assert(status == OK);
        assert(strlen(code) == 6);
    }

    /* 17th room creation must return SERVER_FULL */
    char code17[7] = {0};
    Status st17 = roomsCreate(rooms, 9999, "ExtraHost", "owl", CLASSIC, code17);
    assert(st17 == SERVER_FULL);

    roomsDestroy(rooms);
}

static void testSeatAssignment(void) {
    Rooms *rooms = NULL;
    assert(roomsInit(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(roomsCreate(rooms, 101, "Alice", "fox", CLASSIC, code) == OK);

    Room *room = roomsFindByCode(rooms, code);
    assert(room != NULL);
    assert(room->seats[0].occupied && room->seats[0].userId == 101);
    assert(room->hostSeat == 0);

    /* 2nd human joins -> seat 1 */
    Seat seat2 = 255;
    assert(roomsJoin(rooms, 102, "Bob", "bear", code, &seat2) == OK);
    assert(seat2 == 1);
    assert(room->seats[1].occupied && room->seats[1].userId == 102);

    /* 3rd human joins -> seat 2 */
    Seat seat3 = 255;
    assert(roomsJoin(rooms, 103, "Charlie", "cat", code, &seat3) == OK);
    assert(seat3 == 2);
    assert(room->seats[2].occupied && room->seats[2].userId == 103);

    /* 4th human joins -> seat 3 */
    Seat seat4 = 255;
    assert(roomsJoin(rooms, 104, "Dave", "dog", code, &seat4) == OK);
    assert(seat4 == 3);
    assert(room->seats[3].occupied && room->seats[3].userId == 104);

    /* 5th human joining returns ROOM_FULL */
    Seat seat5 = 255;
    Status st5 = roomsJoin(rooms, 105, "Eve", "panda", code, &seat5);
    assert(st5 == ROOM_FULL);

    roomsDestroy(rooms);
}

static void testHostPrivileges(void) {
    Rooms *rooms = NULL;
    assert(roomsInit(&rooms, NULL, startSucceeds) == OK);

    char code[7] = {0};
    assert(roomsCreate(rooms, 101, "Host", "fox", CLASSIC, code) == OK);
    Seat s2 = 0;
    assert(roomsJoin(rooms, 102, "Guest", "bear", code, &s2) == OK);

    /* Non-host (102) attempts host-only commands */
    assert(roomsSetMode(rooms, 102, TWO_V_TWO) == FORBIDDEN);
    assert(roomsSetBot(rooms, 102, 2, EASY) == FORBIDDEN);
    assert(roomsKick(rooms, 102, 0) == FORBIDDEN);

    /* Host attempts host commands -> allowed */
    assert(roomsSetMode(rooms, 101, TWO_V_TWO) == OK);
    Room *room = roomsFindByCode(rooms, code);
    assert(room->mode == TWO_V_TWO);

    assert(roomsSetBot(rooms, 101, 2, EASY) == OK);
    assert(room->seats[2].occupied && room->seats[2].isBot);

    /* Host kicks guest at seat 1 -> allowed */
    assert(roomsKick(rooms, 101, 1) == OK);
    assert(!room->seats[1].occupied);

    /* Host cannot kick self */
    assert(roomsKick(rooms, 101, 0) == INVALID_ACTION || roomsKick(rooms, 101, 0) == FORBIDDEN);

    roomsDestroy(rooms);
}

static void testBotConfiguration(void) {
    Rooms *rooms = NULL;
    assert(roomsInit(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(roomsCreate(rooms, 101, "Host", "fox", CLASSIC, code) == OK);

    Room *room = roomsFindByCode(rooms, code);

    /* Add EASY bot at seat 1 */
    assert(roomsSetBot(rooms, 101, 1, EASY) == OK);
    assert(room->seats[1].occupied && room->seats[1].isBot && room->seats[1].botLevel == EASY);

    /* Reconfigure bot at seat 1 to NORMAL */
    assert(roomsSetBot(rooms, 101, 1, NORMAL) == OK);
    assert(room->seats[1].occupied && room->seats[1].isBot && room->seats[1].botLevel == NORMAL);

    /* Add HARD bot at seat 2 */
    assert(roomsSetBot(rooms, 101, 2, HARD) == OK);
    assert(room->seats[2].occupied && room->seats[2].isBot && room->seats[2].botLevel == HARD);

    roomsDestroy(rooms);
}

static void testHostMigration(void) {
    Rooms *rooms = NULL;
    assert(roomsInit(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(roomsCreate(rooms, 201, "User1", "fox", CLASSIC, code) == OK);
    Seat s2, s3;
    assert(roomsJoin(rooms, 202, "User2", "bear", code, &s2) == OK);
    assert(roomsJoin(rooms, 203, "User3", "cat", code, &s3) == OK);
    assert(roomsSetBot(rooms, 201, 3, EASY) == OK);

    Room *room = roomsFindByCode(rooms, code);
    assert(room->hostSeat == 0);

    /* When host (User1) leaves before start, host migrates to the remaining human
     * with the lowest seat number (User2, seat 1). */
    assert(roomsLeave(rooms, 201) == OK);
    assert(!room->seats[0].occupied);
    assert(room->hostSeat == 1);
    assert(room->seats[1].occupied && room->seats[1].userId == 202);

    /* User2 leaves -> host migrates to User3 (seat 2) */
    assert(roomsLeave(rooms, 202) == OK);
    assert(!room->seats[1].occupied);
    assert(room->hostSeat == 2);
    assert(room->seats[2].occupied && room->seats[2].userId == 203);

    /* User3 (last human) leaves -> room is cleaned up even if bot was in seat 3 */
    assert(roomsLeave(rooms, 203) == OK);
    assert(roomsFindByCode(rooms, code) == NULL);

    roomsDestroy(rooms);
}

static void testRoomStart(void) {
    Rooms *rooms = NULL;
    App *dummyApp = calloc(1, sizeof(App));
    assert(dummyApp != NULL);
    assert(roomsInit(&rooms, dummyApp, startFails) == OK);
    dummyApp->rooms = rooms;

    char code[7] = {0};
    assert(roomsCreate(rooms, 301, "Host", "fox", CLASSIC, code) == OK);
    assert(roomsSetBot(rooms, 301, 1, EASY) == OK);
    assert(roomsSetBot(rooms, 301, 2, NORMAL) == OK);

    /* Not all 4 seats are filled (seat 3 empty) -> roomsStart must fail */
    assert(roomsStart(dummyApp, code) != OK);

    /* Fill seat 3 */
    assert(roomsSetBot(rooms, 301, 3, HARD) == OK);

    /* Non-host cannot start */
    /* roomsStart by roomId uses the room's host. But let's verify callback failure behavior: */
    /* startFails returns BAD_REQUEST -> roomsStart must fail, room remains intact */
    Room *room = roomsFindByCode(rooms, code);
    assert(room != NULL);
    assert(room->status == ROOM_STATUS_WAITING);

    Status status = roomsStart(dummyApp, code);
    assert(status == BAD_REQUEST);
    /* Room remains intact, active, not mutated to in-match */
    assert(room->active == true);
    assert(room->status == ROOM_STATUS_WAITING);

    /* Now set callback to success */
    rooms->onStart = startSucceeds;
    status = roomsStart(dummyApp, code);
    assert(status == OK);

    roomsDestroy(rooms);
    free(dummyApp);
}

static void testUserInRoomCannotCreateOrJoinAnother(void) {
    Rooms *rooms = NULL;
    assert(roomsInit(&rooms, NULL, NULL) == OK);

    char codeA[7] = {0};
    char codeB[7] = {0};
    assert(roomsCreate(rooms, 1, "User1", "fox", CLASSIC, codeA) == OK);
    assert(roomsCreate(rooms, 2, "User2", "bear", CLASSIC, codeB) == OK);

    /* User 1 is in Room A; calling roomsCreate must return BUSY */
    char codeC[7] = {0};
    assert(roomsCreate(rooms, 1, "User1", "fox", CLASSIC, codeC) == BUSY);

    /* User 1 is in Room A; calling roomsJoin into Room B must return BUSY */
    Seat seat = 0;
    assert(roomsJoin(rooms, 1, "User1", "fox", codeB, &seat) == BUSY);

    /* Room A still retains User 1 at seat 0 */
    Room *roomA = roomsFindByCode(rooms, codeA);
    assert(roomA != NULL);
    assert(roomA->seats[0].occupied);
    assert(roomA->seats[0].userId == 1);

    /* Room B does not have User 1 */
    Room *roomB = roomsFindByCode(rooms, codeB);
    assert(roomB != NULL);
    assert(roomB->seats[0].userId == 2);
    for (int i = 1; i < ROOM_MAX_SEATS; i++) {
        assert(!roomB->seats[i].occupied || roomB->seats[i].userId != 1);
    }

    roomsDestroy(rooms);
}

static void testRoomStartInvariants(void) {
    Rooms *rooms = NULL;
    App *dummyApp = calloc(1, sizeof(App));
    assert(dummyApp != NULL);
    assert(roomsInit(&rooms, dummyApp, NULL) == OK);
    dummyApp->rooms = rooms;

    char code[7] = {0};
    assert(roomsCreate(rooms, 301, "Host", "fox", CLASSIC, code) == OK);
    assert(roomsSetBot(rooms, 301, 1, EASY) == OK);
    assert(roomsSetBot(rooms, 301, 2, NORMAL) == OK);

    /* 1. Less than 4 seats occupied -> INVALID_ACTION */
    assert(roomsStart(dummyApp, code) == INVALID_ACTION);

    /* Fill 4th seat */
    assert(roomsSetBot(rooms, 301, 3, HARD) == OK);

    /* 2. 4 seats occupied but onStart == NULL -> BUSY */
    assert(roomsStart(dummyApp, code) == BUSY);

    /* 3. With onStart callback succeeding -> OK */
    rooms->onStart = startSucceeds;
    assert(roomsStart(dummyApp, code) == OK);

    Room *room = roomsFindByCode(rooms, code);
    assert(room != NULL);
    assert(room->status == ROOM_STATUS_IN_MATCH);

    /* 4. Room already IN_MATCH -> INVALID_ACTION */
    assert(roomsStart(dummyApp, code) == INVALID_ACTION);

    roomsDestroy(rooms);
    free(dummyApp);
}

static void testInvite30sSlotReclamation(void) {
    Rooms *rooms = NULL;
    assert(roomsInit(&rooms, NULL, NULL) == OK);

    /* Fill all 64 invite slots with expired invites (31 seconds ago) */
    uint64_t now = accountsGetNowMs();
    for (int i = 0; i < MAX_INVITES; i++) {
        rooms->invites[i].active = true;
        snprintf(rooms->invites[i].inviteId, sizeof(rooms->invites[i].inviteId), "inv-%03d", i);
        rooms->invites[i].senderId = 100 + (UserId)i;
        rooms->invites[i].recipientId = 200 + (UserId)i;
        strcpy(rooms->invites[i].kind, "party");
        rooms->invites[i].createdAtMs = now - 31000ULL;
    }

    /* Slot 0 is made active and fresh (< 30s) */
    rooms->invites[0].createdAtMs = now;

    /* Slot 1 is expired (created 31s ago) */
    rooms->invites[1].createdAtMs = now - 31000ULL;

    char outId[37] = {0};
    /* Creating an invite should reclaim an expired slot (e.g. slot 1),
     * NOT overwrite active slot 0. */
    assert(roomsInviteCreate(rooms, 999, 888, "party", NULL, outId) == OK);

    /* Verify slot 0 is STILL active and untouched */
    assert(rooms->invites[0].active == true);
    assert(rooms->invites[0].senderId == 100);
    assert(strcmp(rooms->invites[0].inviteId, "inv-000") == 0);

    /* Verify an expired slot was reclaimed */
    assert(rooms->invites[1].active == true);
    assert(rooms->invites[1].senderId == 999);
    assert(strcmp(rooms->invites[1].inviteId, outId) == 0);

    roomsDestroy(rooms);
}

static void testSeatBoundsValidation(void) {
    Rooms *rooms = NULL;
    assert(roomsInit(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(roomsCreate(rooms, 101, "Host", "fox", CLASSIC, code) == OK);

    assert(roomsSetBot(rooms, 101, -1, EASY) == BAD_REQUEST);
    assert(roomsSetBot(rooms, 101, 4, EASY) == BAD_REQUEST);
    assert(roomsSetBot(rooms, 101, 257, EASY) == BAD_REQUEST);

    assert(roomsKick(rooms, 101, -1) == BAD_REQUEST);
    assert(roomsKick(rooms, 101, 4) == BAD_REQUEST);
    assert(roomsKick(rooms, 101, 257) == BAD_REQUEST);

    roomsDestroy(rooms);
}

static void testMatchEndedFreesMembers(void) {
    enum { HOST_ID = 401, FIRST_GUEST_ID = 402 };
    App *app = calloc(1, sizeof(App));
    assert(app != NULL);
    Rooms *rooms = NULL;
    assert(roomsInit(&rooms, app, NULL) == OK);
    app->rooms = rooms;

    char code[7] = {0};
    assert(roomsCreate(rooms, HOST_ID, "Host", "fox", CLASSIC, code) == OK);
    for (int seat = 1; seat < ROOM_MAX_SEATS; seat++) {
        Seat joinedSeat = 0;
        UserId guestId = (UserId)(FIRST_GUEST_ID + seat - 1);
        assert(roomsJoin(rooms, guestId, "Guest", "bear", code, &joinedSeat) == OK);
    }
    Room *room = roomsFindByCode(rooms, code);
    assert(room != NULL);
    /* A waiting room that reuses a finished match's code must survive. */
    roomsMatchEnded(app, code);
    assert(roomsFindByUser(rooms, HOST_ID) == room);
    room->status = ROOM_STATUS_IN_MATCH;

    /* Empty or unknown ids are ignored and leave the live room alone. */
    roomsMatchEnded(app, "");
    roomsMatchEnded(app, "ZZZZZZ");
    assert(roomsFindByUser(rooms, HOST_ID) == room);

    roomsMatchEnded(app, code);
    assert(roomsFindByCode(rooms, code) == NULL);
    assert(roomsFindByUser(rooms, HOST_ID) == NULL);
    for (int seat = 1; seat < ROOM_MAX_SEATS; seat++) {
        UserId guestId = (UserId)(FIRST_GUEST_ID + seat - 1);
        assert(roomsFindByUser(rooms, guestId) == NULL);
    }

    /* Released members may host again. */
    char nextCode[7] = {0};
    assert(roomsCreate(rooms, HOST_ID, "Host", "fox", CLASSIC, nextCode) == OK);

    roomsDestroy(rooms);
    free(app);
}

int main(void) {
    testRoomCodeGeneration();
    testRoomLimits();
    testSeatAssignment();
    testHostPrivileges();
    testBotConfiguration();
    testHostMigration();
    testRoomStart();
    testUserInRoomCannotCreateOrJoinAnother();
    testRoomStartInvariants();
    testInvite30sSlotReclamation();
    testSeatBoundsValidation();
    testMatchEndedFreesMembers();

    printf("rooms: OK\n");
    return 0;
}
