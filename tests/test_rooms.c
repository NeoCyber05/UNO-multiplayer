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
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    /* Generate multiple codes and check format */
    for (int i = 0; i < 50; i++) {
        char code[7] = {0};
        rooms_generate_code(rooms, code);
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
        rooms_generate_code(rooms, code);
        assert(strcmp(code, "XYZ123") != 0);
    }

    rooms_destroy(rooms);
}

static void testRoomLimits(void) {
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    /* Maximum 16 concurrent rooms */
    for (int i = 0; i < MAX_ROOMS; i++) {
        char code[7] = {0};
        UserId hostId = 1000 + i;
        Status status = rooms_create(rooms, hostId, "Host", "fox", CLASSIC, code);
        assert(status == OK);
        assert(strlen(code) == 6);
    }

    /* 17th room creation must return SERVER_FULL */
    char code17[7] = {0};
    Status st17 = rooms_create(rooms, 9999, "ExtraHost", "owl", CLASSIC, code17);
    assert(st17 == SERVER_FULL);

    rooms_destroy(rooms);
}

static void testSeatAssignment(void) {
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(rooms_create(rooms, 101, "Alice", "fox", CLASSIC, code) == OK);

    Room *room = rooms_find_by_code(rooms, code);
    assert(room != NULL);
    assert(room->seats[0].occupied && room->seats[0].user_id == 101);
    assert(room->host_seat == 0);

    /* 2nd human joins -> seat 1 */
    Seat seat2 = 255;
    assert(rooms_join(rooms, 102, "Bob", "bear", code, &seat2) == OK);
    assert(seat2 == 1);
    assert(room->seats[1].occupied && room->seats[1].user_id == 102);

    /* 3rd human joins -> seat 2 */
    Seat seat3 = 255;
    assert(rooms_join(rooms, 103, "Charlie", "cat", code, &seat3) == OK);
    assert(seat3 == 2);
    assert(room->seats[2].occupied && room->seats[2].user_id == 103);

    /* 4th human joins -> seat 3 */
    Seat seat4 = 255;
    assert(rooms_join(rooms, 104, "Dave", "dog", code, &seat4) == OK);
    assert(seat4 == 3);
    assert(room->seats[3].occupied && room->seats[3].user_id == 104);

    /* 5th human joining returns ROOM_FULL */
    Seat seat5 = 255;
    Status st5 = rooms_join(rooms, 105, "Eve", "panda", code, &seat5);
    assert(st5 == ROOM_FULL);

    rooms_destroy(rooms);
}

static void testHostPrivileges(void) {
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, startSucceeds) == OK);

    char code[7] = {0};
    assert(rooms_create(rooms, 101, "Host", "fox", CLASSIC, code) == OK);
    Seat s2 = 0;
    assert(rooms_join(rooms, 102, "Guest", "bear", code, &s2) == OK);

    /* Non-host (102) attempts host-only commands */
    assert(rooms_set_mode(rooms, 102, TWO_V_TWO) == FORBIDDEN);
    assert(rooms_set_bot(rooms, 102, 2, EASY) == FORBIDDEN);
    assert(rooms_kick(rooms, 102, 0) == FORBIDDEN);

    /* Host attempts host commands -> allowed */
    assert(rooms_set_mode(rooms, 101, TWO_V_TWO) == OK);
    Room *room = rooms_find_by_code(rooms, code);
    assert(room->mode == TWO_V_TWO);

    assert(rooms_set_bot(rooms, 101, 2, EASY) == OK);
    assert(room->seats[2].occupied && room->seats[2].is_bot);

    /* Host kicks guest at seat 1 -> allowed */
    assert(rooms_kick(rooms, 101, 1) == OK);
    assert(!room->seats[1].occupied);

    /* Host cannot kick self */
    assert(rooms_kick(rooms, 101, 0) == INVALID_ACTION || rooms_kick(rooms, 101, 0) == FORBIDDEN);

    rooms_destroy(rooms);
}

static void testBotConfiguration(void) {
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(rooms_create(rooms, 101, "Host", "fox", CLASSIC, code) == OK);

    Room *room = rooms_find_by_code(rooms, code);

    /* Add EASY bot at seat 1 */
    assert(rooms_set_bot(rooms, 101, 1, EASY) == OK);
    assert(room->seats[1].occupied && room->seats[1].is_bot && room->seats[1].bot_level == EASY);

    /* Reconfigure bot at seat 1 to NORMAL */
    assert(rooms_set_bot(rooms, 101, 1, NORMAL) == OK);
    assert(room->seats[1].occupied && room->seats[1].is_bot && room->seats[1].bot_level == NORMAL);

    /* Add HARD bot at seat 2 */
    assert(rooms_set_bot(rooms, 101, 2, HARD) == OK);
    assert(room->seats[2].occupied && room->seats[2].is_bot && room->seats[2].bot_level == HARD);

    rooms_destroy(rooms);
}

static void testHostMigration(void) {
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(rooms_create(rooms, 201, "User1", "fox", CLASSIC, code) == OK);
    Seat s2, s3;
    assert(rooms_join(rooms, 202, "User2", "bear", code, &s2) == OK);
    assert(rooms_join(rooms, 203, "User3", "cat", code, &s3) == OK);
    assert(rooms_set_bot(rooms, 201, 3, EASY) == OK);

    Room *room = rooms_find_by_code(rooms, code);
    assert(room->host_seat == 0);

    /* When host (User1) leaves before start, host migrates to the remaining human
     * with the lowest seat number (User2, seat 1). */
    assert(rooms_leave(rooms, 201) == OK);
    assert(!room->seats[0].occupied);
    assert(room->host_seat == 1);
    assert(room->seats[1].occupied && room->seats[1].user_id == 202);

    /* User2 leaves -> host migrates to User3 (seat 2) */
    assert(rooms_leave(rooms, 202) == OK);
    assert(!room->seats[1].occupied);
    assert(room->host_seat == 2);
    assert(room->seats[2].occupied && room->seats[2].user_id == 203);

    /* User3 (last human) leaves -> room is cleaned up even if bot was in seat 3 */
    assert(rooms_leave(rooms, 203) == OK);
    assert(rooms_find_by_code(rooms, code) == NULL);

    rooms_destroy(rooms);
}

static void testRoomStart(void) {
    Rooms *rooms = NULL;
    App *dummyApp = calloc(1, sizeof(App));
    assert(dummyApp != NULL);
    assert(rooms_init(&rooms, dummyApp, startFails) == OK);
    dummyApp->rooms = rooms;

    char code[7] = {0};
    assert(rooms_create(rooms, 301, "Host", "fox", CLASSIC, code) == OK);
    assert(rooms_set_bot(rooms, 301, 1, EASY) == OK);
    assert(rooms_set_bot(rooms, 301, 2, NORMAL) == OK);

    /* Not all 4 seats are filled (seat 3 empty) -> rooms_start must fail */
    assert(rooms_start(dummyApp, code) != OK);

    /* Fill seat 3 */
    assert(rooms_set_bot(rooms, 301, 3, HARD) == OK);

    /* Non-host cannot start */
    /* rooms_start by roomId uses the room's host. But let's verify callback failure behavior: */
    /* startFails returns BAD_REQUEST -> rooms_start must fail, room remains intact */
    Room *room = rooms_find_by_code(rooms, code);
    assert(room != NULL);
    assert(room->status == ROOM_STATUS_WAITING);

    Status status = rooms_start(dummyApp, code);
    assert(status == BAD_REQUEST);
    /* Room remains intact, active, not mutated to in-match */
    assert(room->active == true);
    assert(room->status == ROOM_STATUS_WAITING);

    /* Now set callback to success */
    rooms->on_start = startSucceeds;
    status = rooms_start(dummyApp, code);
    assert(status == OK);

    rooms_destroy(rooms);
    free(dummyApp);
}

static void testUserInRoomCannotCreateOrJoinAnother(void) {
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    char codeA[7] = {0};
    char codeB[7] = {0};
    assert(rooms_create(rooms, 1, "User1", "fox", CLASSIC, codeA) == OK);
    assert(rooms_create(rooms, 2, "User2", "bear", CLASSIC, codeB) == OK);

    /* User 1 is in Room A; calling rooms_create must return BUSY */
    char codeC[7] = {0};
    assert(rooms_create(rooms, 1, "User1", "fox", CLASSIC, codeC) == BUSY);

    /* User 1 is in Room A; calling rooms_join into Room B must return BUSY */
    Seat seat = 0;
    assert(rooms_join(rooms, 1, "User1", "fox", codeB, &seat) == BUSY);

    /* Room A still retains User 1 at seat 0 */
    Room *roomA = rooms_find_by_code(rooms, codeA);
    assert(roomA != NULL);
    assert(roomA->seats[0].occupied);
    assert(roomA->seats[0].user_id == 1);

    /* Room B does not have User 1 */
    Room *roomB = rooms_find_by_code(rooms, codeB);
    assert(roomB != NULL);
    assert(roomB->seats[0].user_id == 2);
    for (int i = 1; i < ROOM_MAX_SEATS; i++) {
        assert(!roomB->seats[i].occupied || roomB->seats[i].user_id != 1);
    }

    rooms_destroy(rooms);
}

static void testRoomStartInvariants(void) {
    Rooms *rooms = NULL;
    App *dummyApp = calloc(1, sizeof(App));
    assert(dummyApp != NULL);
    assert(rooms_init(&rooms, dummyApp, NULL) == OK);
    dummyApp->rooms = rooms;

    char code[7] = {0};
    assert(rooms_create(rooms, 301, "Host", "fox", CLASSIC, code) == OK);
    assert(rooms_set_bot(rooms, 301, 1, EASY) == OK);
    assert(rooms_set_bot(rooms, 301, 2, NORMAL) == OK);

    /* 1. Less than 4 seats occupied -> INVALID_ACTION */
    assert(rooms_start(dummyApp, code) == INVALID_ACTION);

    /* Fill 4th seat */
    assert(rooms_set_bot(rooms, 301, 3, HARD) == OK);

    /* 2. 4 seats occupied but on_start == NULL -> BUSY */
    assert(rooms_start(dummyApp, code) == BUSY);

    /* 3. With on_start callback succeeding -> OK */
    rooms->on_start = startSucceeds;
    assert(rooms_start(dummyApp, code) == OK);

    Room *room = rooms_find_by_code(rooms, code);
    assert(room != NULL);
    assert(room->status == ROOM_STATUS_IN_MATCH);

    /* 4. Room already IN_MATCH -> INVALID_ACTION */
    assert(rooms_start(dummyApp, code) == INVALID_ACTION);

    rooms_destroy(rooms);
    free(dummyApp);
}

static void testInvite30sSlotReclamation(void) {
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    /* Fill all 64 invite slots with expired invites (31 seconds ago) */
    uint64_t now = accounts_get_now_ms();
    for (int i = 0; i < MAX_INVITES; i++) {
        rooms->invites[i].active = true;
        snprintf(rooms->invites[i].invite_id, sizeof(rooms->invites[i].invite_id), "inv-%03d", i);
        rooms->invites[i].sender_id = 100 + (UserId)i;
        rooms->invites[i].recipient_id = 200 + (UserId)i;
        strcpy(rooms->invites[i].kind, "party");
        rooms->invites[i].created_at_ms = now - 31000ULL;
    }

    /* Slot 0 is made active and fresh (< 30s) */
    rooms->invites[0].created_at_ms = now;

    /* Slot 1 is expired (created 31s ago) */
    rooms->invites[1].created_at_ms = now - 31000ULL;

    char outId[37] = {0};
    /* Creating an invite should reclaim an expired slot (e.g. slot 1),
     * NOT overwrite active slot 0. */
    assert(rooms_invite_create(rooms, 999, 888, "party", NULL, outId) == OK);

    /* Verify slot 0 is STILL active and untouched */
    assert(rooms->invites[0].active == true);
    assert(rooms->invites[0].sender_id == 100);
    assert(strcmp(rooms->invites[0].invite_id, "inv-000") == 0);

    /* Verify an expired slot was reclaimed */
    assert(rooms->invites[1].active == true);
    assert(rooms->invites[1].sender_id == 999);
    assert(strcmp(rooms->invites[1].invite_id, outId) == 0);

    rooms_destroy(rooms);
}

static void testSeatBoundsValidation(void) {
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(rooms_create(rooms, 101, "Host", "fox", CLASSIC, code) == OK);

    assert(rooms_set_bot(rooms, 101, -1, EASY) == BAD_REQUEST);
    assert(rooms_set_bot(rooms, 101, 4, EASY) == BAD_REQUEST);
    assert(rooms_set_bot(rooms, 101, 257, EASY) == BAD_REQUEST);

    assert(rooms_kick(rooms, 101, -1) == BAD_REQUEST);
    assert(rooms_kick(rooms, 101, 4) == BAD_REQUEST);
    assert(rooms_kick(rooms, 101, 257) == BAD_REQUEST);

    rooms_destroy(rooms);
}

static void testMatchEndedFreesMembers(void) {
    enum { HOST_ID = 401, FIRST_GUEST_ID = 402 };
    App *app = calloc(1, sizeof(App));
    assert(app != NULL);
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, app, NULL) == OK);
    app->rooms = rooms;

    char code[7] = {0};
    assert(rooms_create(rooms, HOST_ID, "Host", "fox", CLASSIC, code) == OK);
    for (int seat = 1; seat < ROOM_MAX_SEATS; seat++) {
        Seat joinedSeat = 0;
        UserId guestId = (UserId)(FIRST_GUEST_ID + seat - 1);
        assert(rooms_join(rooms, guestId, "Guest", "bear", code, &joinedSeat) == OK);
    }
    Room *room = rooms_find_by_code(rooms, code);
    assert(room != NULL);
    /* A waiting room that reuses a finished match's code must survive. */
    rooms_match_ended(app, code);
    assert(rooms_find_by_user(rooms, HOST_ID) == room);
    room->status = ROOM_STATUS_IN_MATCH;

    /* Empty or unknown ids are ignored and leave the live room alone. */
    rooms_match_ended(app, "");
    rooms_match_ended(app, "ZZZZZZ");
    assert(rooms_find_by_user(rooms, HOST_ID) == room);

    rooms_match_ended(app, code);
    assert(rooms_find_by_code(rooms, code) == NULL);
    assert(rooms_find_by_user(rooms, HOST_ID) == NULL);
    for (int seat = 1; seat < ROOM_MAX_SEATS; seat++) {
        UserId guestId = (UserId)(FIRST_GUEST_ID + seat - 1);
        assert(rooms_find_by_user(rooms, guestId) == NULL);
    }

    /* Released members may host again. */
    char nextCode[7] = {0};
    assert(rooms_create(rooms, HOST_ID, "Host", "fox", CLASSIC, nextCode) == OK);

    rooms_destroy(rooms);
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
