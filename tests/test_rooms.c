#include "server/rooms.h"
#include "server/app.h"
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
    assert(rooms_init(&rooms, NULL, dummy_start_fail) == OK);

    char code[7] = {0};
    assert(rooms_create(rooms, 301, "Host", "fox", CLASSIC, code) == OK);
    assert(rooms_set_bot(rooms, 301, 1, EASY) == OK);
    assert(rooms_set_bot(rooms, 301, 2, NORMAL) == OK);

    /* Not all 4 seats are filled (seat 3 empty) -> rooms_start must fail */
    assert(rooms_start(NULL, code) != OK);

    /* Fill seat 3 */
    assert(rooms_set_bot(rooms, 301, 3, HARD) == OK);

    /* Non-host cannot start */
    /* rooms_start by room_id uses the room's host. But let's verify callback failure behavior: */
    /* dummy_start_fail returns BAD_REQUEST -> rooms_start must fail, room remains intact */
    Room *r = rooms_find_by_code(rooms, code);
    assert(r != NULL);
    assert(r->status == ROOM_STATUS_WAITING);

    Status st = rooms_start(NULL, code);
    assert(st == BAD_REQUEST);
    /* Room remains intact, active, not mutated to in-match */
    assert(r->active == true);
    assert(r->status == ROOM_STATUS_WAITING);

    /* Now set callback to success */
    rooms->on_start = dummy_start_success;
    st = rooms_start(NULL, code);
    assert(st == OK);

    rooms_destroy(rooms);
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

    printf("rooms: OK\n");
    return 0;
}
