#include "server/rooms.h"
#include "server/app.h"
#include "server/accounts.h"
#include "shared/contracts.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static Status dummy_start_success(App *app, const RoomStart *rs, MatchId *out_match_id)
{
    (void)app;
    (void)rs;
    strcpy(*out_match_id, "123e4567-e89b-12d3-a456-426614174000");
    return OK;
}

static Status dummy_start_fail(App *app, const RoomStart *rs, MatchId *out_match_id)
{
    (void)app;
    (void)rs;
    (void)out_match_id;
    return BAD_REQUEST;
}

static void test_room_code_generation(void)
{
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    /* Generate multiple codes and check format */
    for (int i = 0; i < 50; i++) {
        char code[7] = {0};
        rooms_generate_code(rooms, code);
        assert(strlen(code) == 6);
        for (int j = 0; j < 6; j++) {
            char c = code[j];
            bool valid = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
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

static void test_room_limits(void)
{
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    /* Maximum 16 concurrent rooms */
    for (int i = 0; i < MAX_ROOMS; i++) {
        char code[7] = {0};
        UserId host_id = 1000 + i;
        Status st = rooms_create(rooms, host_id, "Host", "fox", CLASSIC, code);
        assert(st == OK);
        assert(strlen(code) == 6);
    }

    /* 17th room creation must return SERVER_FULL */
    char code17[7] = {0};
    Status st17 = rooms_create(rooms, 9999, "ExtraHost", "owl", CLASSIC, code17);
    assert(st17 == SERVER_FULL);

    rooms_destroy(rooms);
}

static void test_seat_assignment(void)
{
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(rooms_create(rooms, 101, "Alice", "fox", CLASSIC, code) == OK);

    Room *r = rooms_find_by_code(rooms, code);
    assert(r != NULL);
    assert(r->seats[0].occupied && r->seats[0].user_id == 101);
    assert(r->host_seat == 0);

    /* 2nd human joins -> seat 1 */
    Seat seat2 = 255;
    assert(rooms_join(rooms, 102, "Bob", "bear", code, &seat2) == OK);
    assert(seat2 == 1);
    assert(r->seats[1].occupied && r->seats[1].user_id == 102);

    /* 3rd human joins -> seat 2 */
    Seat seat3 = 255;
    assert(rooms_join(rooms, 103, "Charlie", "cat", code, &seat3) == OK);
    assert(seat3 == 2);
    assert(r->seats[2].occupied && r->seats[2].user_id == 103);

    /* 4th human joins -> seat 3 */
    Seat seat4 = 255;
    assert(rooms_join(rooms, 104, "Dave", "dog", code, &seat4) == OK);
    assert(seat4 == 3);
    assert(r->seats[3].occupied && r->seats[3].user_id == 104);

    /* 5th human joining returns ROOM_FULL */
    Seat seat5 = 255;
    Status st5 = rooms_join(rooms, 105, "Eve", "panda", code, &seat5);
    assert(st5 == ROOM_FULL);

    rooms_destroy(rooms);
}

static void test_host_privileges(void)
{
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, dummy_start_success) == OK);

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
    Room *r = rooms_find_by_code(rooms, code);
    assert(r->mode == TWO_V_TWO);

    assert(rooms_set_bot(rooms, 101, 2, EASY) == OK);
    assert(r->seats[2].occupied && r->seats[2].is_bot);

    /* Host kicks guest at seat 1 -> allowed */
    assert(rooms_kick(rooms, 101, 1) == OK);
    assert(!r->seats[1].occupied);

    /* Host cannot kick self */
    assert(rooms_kick(rooms, 101, 0) == INVALID_ACTION || rooms_kick(rooms, 101, 0) == FORBIDDEN);

    rooms_destroy(rooms);
}

static void test_bot_configuration(void)
{
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(rooms_create(rooms, 101, "Host", "fox", CLASSIC, code) == OK);

    Room *r = rooms_find_by_code(rooms, code);

    /* Add EASY bot at seat 1 */
    assert(rooms_set_bot(rooms, 101, 1, EASY) == OK);
    assert(r->seats[1].occupied && r->seats[1].is_bot && r->seats[1].bot_level == EASY);

    /* Reconfigure bot at seat 1 to NORMAL */
    assert(rooms_set_bot(rooms, 101, 1, NORMAL) == OK);
    assert(r->seats[1].occupied && r->seats[1].is_bot && r->seats[1].bot_level == NORMAL);

    /* Add HARD bot at seat 2 */
    assert(rooms_set_bot(rooms, 101, 2, HARD) == OK);
    assert(r->seats[2].occupied && r->seats[2].is_bot && r->seats[2].bot_level == HARD);

    rooms_destroy(rooms);
}

static void test_host_migration(void)
{
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    char code[7] = {0};
    assert(rooms_create(rooms, 201, "User1", "fox", CLASSIC, code) == OK);
    Seat s2, s3;
    assert(rooms_join(rooms, 202, "User2", "bear", code, &s2) == OK);
    assert(rooms_join(rooms, 203, "User3", "cat", code, &s3) == OK);
    assert(rooms_set_bot(rooms, 201, 3, EASY) == OK);

    Room *r = rooms_find_by_code(rooms, code);
    assert(r->host_seat == 0);

    /* When host (User1) leaves before start, host migrates to remaining human with lowest seat number (User2, seat 1) */
    assert(rooms_leave(rooms, 201) == OK);
    assert(!r->seats[0].occupied);
    assert(r->host_seat == 1);
    assert(r->seats[1].occupied && r->seats[1].user_id == 202);

    /* User2 leaves -> host migrates to User3 (seat 2) */
    assert(rooms_leave(rooms, 202) == OK);
    assert(!r->seats[1].occupied);
    assert(r->host_seat == 2);
    assert(r->seats[2].occupied && r->seats[2].user_id == 203);

    /* User3 (last human) leaves -> room is cleaned up even if bot was in seat 3 */
    assert(rooms_leave(rooms, 203) == OK);
    assert(rooms_find_by_code(rooms, code) == NULL);

    rooms_destroy(rooms);
}

static void test_room_start(void)
{
    Rooms *rooms = NULL;
    App *dummy_app = calloc(1, sizeof(App));
    assert(dummy_app != NULL);
    assert(rooms_init(&rooms, dummy_app, dummy_start_fail) == OK);
    dummy_app->rooms = rooms;

    char code[7] = {0};
    assert(rooms_create(rooms, 301, "Host", "fox", CLASSIC, code) == OK);
    assert(rooms_set_bot(rooms, 301, 1, EASY) == OK);
    assert(rooms_set_bot(rooms, 301, 2, NORMAL) == OK);

    /* Not all 4 seats are filled (seat 3 empty) -> rooms_start must fail */
    assert(rooms_start(dummy_app, code) != OK);

    /* Fill seat 3 */
    assert(rooms_set_bot(rooms, 301, 3, HARD) == OK);

    /* Non-host cannot start */
    /* rooms_start by room_id uses the room's host. But let's verify callback failure behavior: */
    /* dummy_start_fail returns BAD_REQUEST -> rooms_start must fail, room remains intact */
    Room *r = rooms_find_by_code(rooms, code);
    assert(r != NULL);
    assert(r->status == ROOM_STATUS_WAITING);

    Status st = rooms_start(dummy_app, code);
    assert(st == BAD_REQUEST);
    /* Room remains intact, active, not mutated to in-match */
    assert(r->active == true);
    assert(r->status == ROOM_STATUS_WAITING);

    /* Now set callback to success */
    rooms->on_start = dummy_start_success;
    st = rooms_start(dummy_app, code);
    assert(st == OK);

    rooms_destroy(rooms);
    free(dummy_app);
}

static void test_user_in_room_cannot_create_or_join_another(void)
{
    Rooms *rooms = NULL;
    assert(rooms_init(&rooms, NULL, NULL) == OK);

    char code_a[7] = {0};
    char code_b[7] = {0};
    assert(rooms_create(rooms, 1, "User1", "fox", CLASSIC, code_a) == OK);
    assert(rooms_create(rooms, 2, "User2", "bear", CLASSIC, code_b) == OK);

    /* User 1 is in Room A; calling rooms_create must return BUSY */
    char code_c[7] = {0};
    assert(rooms_create(rooms, 1, "User1", "fox", CLASSIC, code_c) == BUSY);

    /* User 1 is in Room A; calling rooms_join into Room B must return BUSY */
    Seat seat = 0;
    assert(rooms_join(rooms, 1, "User1", "fox", code_b, &seat) == BUSY);

    /* Room A still retains User 1 at seat 0 */
    Room *room_a = rooms_find_by_code(rooms, code_a);
    assert(room_a != NULL);
    assert(room_a->seats[0].occupied);
    assert(room_a->seats[0].user_id == 1);

    /* Room B does not have User 1 */
    Room *room_b = rooms_find_by_code(rooms, code_b);
    assert(room_b != NULL);
    assert(room_b->seats[0].user_id == 2);
    for (int i = 1; i < ROOM_MAX_SEATS; i++) {
        assert(!room_b->seats[i].occupied || room_b->seats[i].user_id != 1);
    }

    rooms_destroy(rooms);
}

static void test_room_start_invariants(void)
{
    Rooms *rooms = NULL;
    App *dummy_app = calloc(1, sizeof(App));
    assert(dummy_app != NULL);
    assert(rooms_init(&rooms, dummy_app, NULL) == OK);
    dummy_app->rooms = rooms;

    char code[7] = {0};
    assert(rooms_create(rooms, 301, "Host", "fox", CLASSIC, code) == OK);
    assert(rooms_set_bot(rooms, 301, 1, EASY) == OK);
    assert(rooms_set_bot(rooms, 301, 2, NORMAL) == OK);

    /* 1. Less than 4 seats occupied -> INVALID_ACTION */
    assert(rooms_start(dummy_app, code) == INVALID_ACTION);

    /* Fill 4th seat */
    assert(rooms_set_bot(rooms, 301, 3, HARD) == OK);

    /* 2. 4 seats occupied but on_start == NULL -> BUSY */
    assert(rooms_start(dummy_app, code) == BUSY);

    /* 3. With on_start callback succeeding -> OK */
    rooms->on_start = dummy_start_success;
    assert(rooms_start(dummy_app, code) == OK);

    Room *r = rooms_find_by_code(rooms, code);
    assert(r != NULL);
    assert(r->status == ROOM_STATUS_IN_MATCH);

    /* 4. Room already IN_MATCH -> INVALID_ACTION */
    assert(rooms_start(dummy_app, code) == INVALID_ACTION);

    rooms_destroy(rooms);
    free(dummy_app);
}

static void test_invite_30s_slot_reclamation(void)
{
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

    char out_id[37] = {0};
    /* Creating an invite should reclaim an expired slot (e.g. slot 1), NOT overwrite active slot 0 */
    assert(rooms_invite_create(rooms, 999, 888, "party", NULL, out_id) == OK);

    /* Verify slot 0 is STILL active and untouched */
    assert(rooms->invites[0].active == true);
    assert(rooms->invites[0].sender_id == 100);
    assert(strcmp(rooms->invites[0].invite_id, "inv-000") == 0);

    /* Verify an expired slot was reclaimed */
    assert(rooms->invites[1].active == true);
    assert(rooms->invites[1].sender_id == 999);
    assert(strcmp(rooms->invites[1].invite_id, out_id) == 0);

    rooms_destroy(rooms);
}

static void test_seat_bounds_validation(void)
{
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

static void test_match_ended_frees_members(void)
{
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

int main(void)
{
    test_room_code_generation();
    test_room_limits();
    test_seat_assignment();
    test_host_privileges();
    test_bot_configuration();
    test_host_migration();
    test_room_start();
    test_user_in_room_cannot_create_or_join_another();
    test_room_start_invariants();
    test_invite_30s_slot_reclamation();
    test_seat_bounds_validation();
    test_match_ended_frees_members();

    printf("rooms: OK\n");
    return 0;
}
