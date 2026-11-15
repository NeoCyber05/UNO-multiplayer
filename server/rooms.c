#define _POSIX_C_SOURCE 200809L
#include "rooms.h"
#include "accounts.h"
#include "app.h"
#include "store.h"
#include <arpa/inet.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static uint64_t get_now_ms(void)
{
    return accounts_get_now_ms();
}

static const char *const MODE_NAMES[] = {"classic", "2v2", "side", "draft"};

static void send_error(App *app, ConnId conn_id, int32_t req_id, const char *code, const char *msg)
{
    if (conn_id.slot >= APP_MAX_CONNS) return;
    AppConnSlot *slot = &app->slots[conn_id.slot];
    if (!slot->active || slot->generation != conn_id.generation) return;

    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "code", json_object_new_string(code));
    json_object_object_add(payload, "message", json_object_new_string(msg));

    Envelope resp = {1, req_id, "ERROR", payload};
    conn_queue(&slot->conn, &resp);
    json_object_put(payload);
    conn_flush(&slot->conn);
}

static void send_ok(App *app, ConnId conn_id, int32_t req_id, struct json_object *payload)
{
    if (conn_id.slot >= APP_MAX_CONNS) return;
    AppConnSlot *slot = &app->slots[conn_id.slot];
    if (!slot->active || slot->generation != conn_id.generation) return;

    Envelope resp = {1, req_id, "OK", payload ? payload : json_object_new_object()};
    conn_queue(&slot->conn, &resp);
    if (!payload) json_object_put(resp.payload);
    conn_flush(&slot->conn);
}

static void generate_uuid(char out[37])
{
    uint8_t rnd[16];
    randombytes_buf(rnd, sizeof(rnd));
    rnd[6] = (rnd[6] & 0x0f) | 0x40; /* version 4 */
    rnd[8] = (rnd[8] & 0x3f) | 0x80; /* variant 1 */
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7],
        rnd[8], rnd[9], rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);
}

void rooms_generate_code(Rooms *r, char out_code[7])
{
    static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char candidate[7];
    while (1) {
        uint8_t rand_bytes[6];
        randombytes_buf(rand_bytes, sizeof(rand_bytes));
        for (int i = 0; i < 6; i++) {
            candidate[i] = charset[rand_bytes[i] % 36];
        }
        candidate[6] = '\0';

        bool collision = false;
        if (r) {
            for (int i = 0; i < MAX_ROOMS; i++) {
                if (r->rooms[i].active && r->rooms[i].code != out_code && strcmp(r->rooms[i].code, candidate) == 0) {
                    collision = true;
                    break;
                }
            }
        }
        if (!collision) {
            memcpy(out_code, candidate, 7);
            break;
        }
    }
}

static bool parse_mode(struct json_object *obj, Mode *out_mode)
{
    if (!obj) {
        *out_mode = CLASSIC;
        return true;
    }
    if (json_object_is_type(obj, json_type_int)) {
        int m = json_object_get_int(obj);
        if (m >= 0 && m <= 3) {
            *out_mode = (Mode)m;
            return true;
        }
        return false;
    }
    if (json_object_is_type(obj, json_type_string)) {
        const char *s = json_object_get_string(obj);
        if (strcasecmp(s, "classic") == 0) { *out_mode = CLASSIC; return true; }
        if (strcasecmp(s, "2v2") == 0 || strcasecmp(s, "two_v_two") == 0) { *out_mode = TWO_V_TWO; return true; }
        if (strcasecmp(s, "side") == 0) { *out_mode = SIDE; return true; }
        if (strcasecmp(s, "draft") == 0) { *out_mode = DRAFT; return true; }
        return false;
    }
    return false;
}

static bool parse_bot_level(struct json_object *obj, BotLevel *out_level)
{
    if (!obj) return false;
    if (json_object_is_type(obj, json_type_int)) {
        int l = json_object_get_int(obj);
        if (l >= 0 && l <= 2) {
            *out_level = (BotLevel)l;
            return true;
        }
        return false;
    }
    if (json_object_is_type(obj, json_type_string)) {
        const char *s = json_object_get_string(obj);
        if (strcasecmp(s, "easy") == 0) { *out_level = EASY; return true; }
        if (strcasecmp(s, "normal") == 0 || strcasecmp(s, "medium") == 0) { *out_level = NORMAL; return true; }
        if (strcasecmp(s, "hard") == 0) { *out_level = HARD; return true; }
        return false;
    }
    return false;
}

Status rooms_init(Rooms **out, App *app, OnMatchStart on_start)
{
    if (!out) return BAD_REQUEST;
    Rooms *r = calloc(1, sizeof(Rooms));
    if (!r) return STORAGE_ERROR;
    r->app = app;
    r->on_start = on_start;
    r->next_party_id = 1;
    r->next_invite_id = 1;
    *out = r;
    return OK;
}

void rooms_destroy(Rooms *r)
{
    if (!r) return;
    free(r);
}

Room *rooms_find_by_code(Rooms *r, const char *code)
{
    if (!r || !code) return NULL;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (r->rooms[i].active && strcasecmp(r->rooms[i].code, code) == 0) {
            return &r->rooms[i];
        }
    }
    return NULL;
}

Room *rooms_find_by_user(Rooms *r, UserId user_id)
{
    if (!r || user_id == 0) return NULL;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (r->rooms[i].active) {
            for (int s = 0; s < ROOM_MAX_SEATS; s++) {
                if (r->rooms[i].seats[s].occupied && !r->rooms[i].seats[s].is_bot &&
                    r->rooms[i].seats[s].user_id == user_id) {
                    return &r->rooms[i];
                }
            }
        }
    }
    return NULL;
}

Status rooms_create(Rooms *r, UserId host_id, const char *name, const char *avatar, Mode mode, char out_code[7])
{
    if (!r || host_id == 0) return BAD_REQUEST;

    if (rooms_find_by_user(r, host_id) != NULL) return BUSY;

    /* Find empty room slot */
    int slot = -1;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (!r->rooms[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return SERVER_FULL;

    Room *room = &r->rooms[slot];
    memset(room, 0, sizeof(*room));
    rooms_generate_code(r, room->code);
    room->active = true;
    room->mode = mode;
    room->ranked = false;
    room->host_seat = 0;
    room->revision = 1;
    room->status = ROOM_STATUS_WAITING;

    room->seats[0].occupied = true;
    room->seats[0].is_bot = false;
    room->seats[0].user_id = host_id;
    strncpy(room->seats[0].name, name ? name : "Host", sizeof(room->seats[0].name) - 1);
    strncpy(room->seats[0].avatar, avatar ? avatar : "fox", sizeof(room->seats[0].avatar) - 1);

    if (out_code) {
        strcpy(out_code, room->code);
    }
    return OK;
}

Status rooms_join(Rooms *r, UserId user_id, const char *name, const char *avatar, const char *code, Seat *out_seat)
{
    if (!r || user_id == 0 || !code) return BAD_REQUEST;

    if (rooms_find_by_user(r, user_id) != NULL) return BUSY;

    Room *room = rooms_find_by_code(r, code);
    if (!room) return NOT_FOUND;

    /* Find lowest-numbered empty seat */
    int empty_seat = -1;
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (!room->seats[i].occupied) {
            empty_seat = i;
            break;
        }
    }
    if (empty_seat < 0) return ROOM_FULL;

    room->seats[empty_seat].occupied = true;
    room->seats[empty_seat].is_bot = false;
    room->seats[empty_seat].user_id = user_id;
    strncpy(room->seats[empty_seat].name, name ? name : "Player", sizeof(room->seats[empty_seat].name) - 1);
    strncpy(room->seats[empty_seat].avatar, avatar ? avatar : "fox", sizeof(room->seats[empty_seat].avatar) - 1);
    room->revision++;

    if (out_seat) *out_seat = (Seat)empty_seat;
    return OK;
}

Status rooms_leave(Rooms *r, UserId user_id)
{
    if (!r || user_id == 0) return OK;

    Room *room = rooms_find_by_user(r, user_id);
    if (!room) return OK;

    int seat_idx = -1;
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (room->seats[i].occupied && !room->seats[i].is_bot && room->seats[i].user_id == user_id) {
            seat_idx = i;
            break;
        }
    }
    if (seat_idx < 0) return OK;

    memset(&room->seats[seat_idx], 0, sizeof(room->seats[seat_idx]));
    room->revision++;

    /* Check remaining humans */
    int human_count = 0;
    int lowest_human_seat = -1;
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (room->seats[i].occupied && !room->seats[i].is_bot) {
            human_count++;
            if (lowest_human_seat < 0) lowest_human_seat = i;
        }
    }

    if (human_count == 0) {
        /* Last human left: room is destroyed */
        memset(room, 0, sizeof(*room));
        return OK;
    }

    /* If the host left, migrate host to remaining human with lowest seat */
    if (seat_idx == room->host_seat) {
        room->host_seat = (Seat)lowest_human_seat;
    }

    return OK;
}

Status rooms_set_bot(Rooms *r, UserId requester_id, int seat, BotLevel level)
{
    if (!r || requester_id == 0) return BAD_REQUEST;
    if (seat < 0 || seat >= ROOM_MAX_SEATS) return BAD_REQUEST;

    Room *room = rooms_find_by_user(r, requester_id);
    if (!room) return NOT_FOUND;

    /* Must be host */
    if (room->seats[room->host_seat].user_id != requester_id) return FORBIDDEN;

    /* Host cannot turn themselves into a bot */
    if (seat == room->host_seat) return INVALID_ACTION;

    /* Cannot overwrite an existing human */
    if (room->seats[seat].occupied && !room->seats[seat].is_bot) return INVALID_ACTION;

    room->seats[seat].occupied = true;
    room->seats[seat].is_bot = true;
    room->seats[seat].user_id = 0;
    room->seats[seat].bot_level = level;
    snprintf(room->seats[seat].name, sizeof(room->seats[seat].name), "Bot %u", (unsigned)(seat + 1));
    strcpy(room->seats[seat].avatar, "robot");
    room->revision++;

    return OK;
}

Status rooms_kick(Rooms *r, UserId requester_id, int seat)
{
    if (!r || requester_id == 0) return BAD_REQUEST;
    if (seat < 0 || seat >= ROOM_MAX_SEATS) return BAD_REQUEST;

    Room *room = rooms_find_by_user(r, requester_id);
    if (!room) return NOT_FOUND;

    /* Must be host */
    if (room->seats[room->host_seat].user_id != requester_id) return FORBIDDEN;

    /* Cannot kick self */
    if (seat == room->host_seat) return INVALID_ACTION;

    /* Must be occupied */
    if (!room->seats[seat].occupied) return INVALID_ACTION;

    memset(&room->seats[seat], 0, sizeof(room->seats[seat]));
    room->revision++;

    return OK;
}

Status rooms_set_mode(Rooms *r, UserId requester_id, Mode mode)
{
    if (!r || requester_id == 0) return BAD_REQUEST;

    Room *room = rooms_find_by_user(r, requester_id);
    if (!room) return NOT_FOUND;

    /* Must be host */
    if (room->seats[room->host_seat].user_id != requester_id) return FORBIDDEN;

    room->mode = mode;
    room->revision++;
    return OK;
}

Status rooms_start(App *app, const char *room_id)
{
    if (!app || !app->rooms || !room_id) return BAD_REQUEST;
    Rooms *r = app->rooms;

    Room *room = rooms_find_by_code(r, room_id);
    if (!room) return NOT_FOUND;

    if (room->status != ROOM_STATUS_WAITING) return INVALID_ACTION;

    /* Check all 4 seats occupied */
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (!room->seats[i].occupied) return INVALID_ACTION;
    }

    if (!r->on_start) return BUSY;

    RoomStart rs;
    memset(&rs, 0, sizeof(rs));
    strncpy(rs.roomId, room->code, sizeof(rs.roomId) - 1);
    rs.mode = room->mode;
    rs.ranked = room->ranked;
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        rs.players[i].userId = room->seats[i].user_id;
        strncpy(rs.players[i].name, room->seats[i].name, sizeof(rs.players[i].name) - 1);
        strncpy(rs.players[i].avatar, room->seats[i].avatar, sizeof(rs.players[i].avatar) - 1);
        rs.players[i].isBot = room->seats[i].is_bot;
        rs.players[i].botLevel = room->seats[i].bot_level;
    }

    MatchId out_match_id = {0};
    Status st = r->on_start(app, &rs, &out_match_id);
    if (st != OK) {
        /* Callback failed: room remains intact without mutating to in-match */
        return st;
    }

    room->status = ROOM_STATUS_IN_MATCH;
    return OK;
}

void rooms_disconnect(App *app, UserId user_id, uint64_t now_ms_val)
{
    (void)now_ms_val;
    if (!app || !app->rooms || user_id == 0) return;
    Rooms *r = app->rooms;

    party_leave(r, user_id);

    /* Cancel any pending invites involving this user */
    for (int i = 0; i < MAX_INVITES; i++) {
        if (r->invites[i].active &&
            (r->invites[i].sender_id == user_id || r->invites[i].recipient_id == user_id)) {
            r->invites[i].active = false;
        }
    }

    Room *room = rooms_find_by_user(r, user_id);
    if (room && room->status == ROOM_STATUS_WAITING) {
        char code[7];
        strcpy(code, room->code);
        rooms_leave(r, user_id);
        Room *after = rooms_find_by_code(r, code);
        if (after) {
            rooms_broadcast_state(app, after);
        }
    }
}

Status rooms_make_match(App *app, Mode mode, const UserId users[4], MatchId *out_match_id)
{
    if (!out_match_id) return BAD_REQUEST;
    generate_uuid(*out_match_id);

    PlayerSpec players[4];
    memset(players, 0, sizeof(players));
    for (int i = 0; i < 4; i++) {
        players[i].userId = users[i];
        players[i].isBot = (users[i] == 0);
        if (users[i] != 0 && app && app->store) {
            Account acc;
            if (store_account_get_by_id(app->store, users[i], &acc) == OK) {
                strncpy(players[i].name, acc.name, sizeof(players[i].name) - 1);
                strncpy(players[i].avatar, acc.avatar, sizeof(players[i].avatar) - 1);
            }
        }
        if (players[i].name[0] == '\0') {
            snprintf(players[i].name, sizeof(players[i].name), "Player%llu", (unsigned long long)users[i]);
            strcpy(players[i].avatar, "fox");
        }
    }

    if (app && app->store) {
        return store_match_start(app->store, (const MatchId *)out_match_id, players, mode, false);
    }
    return OK;
}

Status rooms_broadcast_state(App *app, const Room *room)
{
    if (!app || !room) return BAD_REQUEST;

    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "revision", json_object_new_int64(room->revision));
    json_object_object_add(payload, "roomId", json_object_new_string(room->code));
    json_object_object_add(payload, "code", json_object_new_string(room->code));
    json_object_object_add(payload, "mode", json_object_new_string(MODE_NAMES[room->mode]));
    json_object_object_add(payload, "ranked", json_object_new_boolean(room->ranked));
    json_object_object_add(payload, "hostSeat", json_object_new_int(room->host_seat));

    struct json_object *seats_arr = json_object_new_array();
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (!room->seats[i].occupied) {
            json_object_array_add(seats_arr, NULL);
        } else {
            struct json_object *seat_obj = json_object_new_object();
            json_object_object_add(seat_obj, "seat", json_object_new_int(i));
            json_object_object_add(seat_obj, "isBot", json_object_new_boolean(room->seats[i].is_bot));
            if (room->seats[i].is_bot) {
                json_object_object_add(seat_obj, "name", json_object_new_string(room->seats[i].name));
                json_object_object_add(seat_obj, "avatar", json_object_new_string(room->seats[i].avatar));
                const char *level_strs[] = {"easy", "normal", "hard"};
                json_object_object_add(seat_obj, "botLevel", json_object_new_string(level_strs[room->seats[i].bot_level]));
            } else {
                char uid_str[32];
                snprintf(uid_str, sizeof(uid_str), "%llu", (unsigned long long)room->seats[i].user_id);
                json_object_object_add(seat_obj, "userId", json_object_new_string(uid_str));
                json_object_object_add(seat_obj, "name", json_object_new_string(room->seats[i].name));
                json_object_object_add(seat_obj, "avatar", json_object_new_string(room->seats[i].avatar));
            }
            json_object_array_add(seats_arr, seat_obj);
        }
    }
    json_object_object_add(payload, "seats", seats_arr);

    Envelope push_env = {1, 0, "ROOM_STATE", payload};
    for (int i = 0; i < ROOM_MAX_SEATS; i++) {
        if (room->seats[i].occupied && !room->seats[i].is_bot) {
            ConnId conn_id;
            if (sessions_find_conn(app, room->seats[i].user_id, &conn_id) == OK) {
                if (conn_id.slot < APP_MAX_CONNS) {
                    AppConnSlot *s = &app->slots[conn_id.slot];
                    if (s->active && s->generation == conn_id.generation) {
                        conn_queue(&s->conn, &push_env);
                        conn_flush(&s->conn);
                    }
                }
            }
        }
    }
    json_object_put(payload);
    return OK;
}

Party *party_find_by_user(Rooms *r, UserId user_id)
{
    if (!r || user_id == 0) return NULL;
    for (int i = 0; i < MAX_PARTIES; i++) {
        if (r->parties[i].active) {
            for (size_t m = 0; m < r->parties[i].member_count; m++) {
                if (r->parties[i].members[m] == user_id) {
                    return &r->parties[i];
                }
            }
        }
    }
    return NULL;
}

static void party_broadcast_state(App *app, const Party *party)
{
    if (!app || !party) return;

    struct json_object *payload = json_object_new_object();
    json_object_object_add(payload, "partyId", json_object_new_int((int)party->party_id));
    char leader_str[32];
    snprintf(leader_str, sizeof(leader_str), "%llu", (unsigned long long)party->leader_id);
    json_object_object_add(payload, "leaderId", json_object_new_string(leader_str));
    json_object_object_add(payload, "revision", json_object_new_int64(party->revision));

    struct json_object *members_arr = json_object_new_array();
    for (size_t i = 0; i < party->member_count; i++) {
        char m_str[32];
        snprintf(m_str, sizeof(m_str), "%llu", (unsigned long long)party->members[i]);
        json_object_array_add(members_arr, json_object_new_string(m_str));
    }
    json_object_object_add(payload, "members", members_arr);

    Envelope push_env = {1, 0, "PARTY_STATE", payload};
    for (size_t i = 0; i < party->member_count; i++) {
        ConnId conn_id;
        if (sessions_find_conn(app, party->members[i], &conn_id) == OK) {
            if (conn_id.slot < APP_MAX_CONNS) {
                AppConnSlot *s = &app->slots[conn_id.slot];
                if (s->active && s->generation == conn_id.generation) {
                    conn_queue(&s->conn, &push_env);
                    conn_flush(&s->conn);
                }
            }
        }
    }
    json_object_put(payload);
}

Status party_leave(Rooms *r, UserId user_id)
{
    if (!r || user_id == 0) return OK;
    Party *p = party_find_by_user(r, user_id);
    if (!p) return OK;

    if (r->app) {
        UserId old_members[PARTY_MAX_MEMBERS];
        size_t old_count = p->member_count;
        for (size_t i = 0; i < old_count; i++) {
            old_members[i] = p->members[i];
        }
        uint32_t party_id = p->party_id;
        uint64_t rev = p->revision + 1;

        struct json_object *payload = json_object_new_object();
        json_object_object_add(payload, "partyId", json_object_new_int((int)party_id));
        json_object_object_add(payload, "leaderId", json_object_new_string("0"));
        json_object_object_add(payload, "revision", json_object_new_int64(rev));
        json_object_object_add(payload, "members", json_object_new_array());

        Envelope push_env = {1, 0, "PARTY_STATE", payload};
        for (size_t i = 0; i < old_count; i++) {
            ConnId conn_id;
            if (sessions_find_conn(r->app, old_members[i], &conn_id) == OK) {
                if (conn_id.slot < APP_MAX_CONNS) {
                    AppConnSlot *s = &r->app->slots[conn_id.slot];
                    if (s->active && s->generation == conn_id.generation) {
                        conn_queue(&s->conn, &push_env);
                        conn_flush(&s->conn);
                    }
                }
            }
        }
        json_object_put(payload);
    }

    /* Party max 2 members. If someone leaves, dissolve party */
    memset(p, 0, sizeof(*p));
    return OK;
}

Status rooms_invite_create(Rooms *r, UserId sender_id, UserId recipient_id, const char *kind, const char *room_code, char out_invite_id[37])
{
    if (!r || sender_id == 0 || recipient_id == 0) return BAD_REQUEST;

    uint64_t now_ms = get_now_ms();
    int inv_slot = -1;
    for (int i = 0; i < MAX_INVITES; i++) {
        if (!r->invites[i].active || (now_ms > r->invites[i].created_at_ms && (now_ms - r->invites[i].created_at_ms > 30000ULL))) {
            inv_slot = i;
            break;
        }
    }
    if (inv_slot < 0) {
        return SERVER_FULL;
    }

    Invite *inv = &r->invites[inv_slot];
    memset(inv, 0, sizeof(*inv));
    inv->active = true;
    generate_uuid(inv->invite_id);
    inv->sender_id = sender_id;
    inv->recipient_id = recipient_id;
    if (kind) strncpy(inv->kind, kind, sizeof(inv->kind) - 1);
    if (room_code) strncpy(inv->room_code, room_code, sizeof(inv->room_code) - 1);
    inv->created_at_ms = now_ms;

    if (out_invite_id) {
        strcpy(out_invite_id, inv->invite_id);
    }
    return OK;
}

Status rooms_handle(App *app, ConnId conn_id, const Envelope *e)
{
    if (!app || !app->rooms || !e) return BAD_REQUEST;
    Rooms *r = app->rooms;

    UserId user_id = 0;
    if (sessions_user(app, conn_id, &user_id) != OK) {
        send_error(app, conn_id, e->id, "UNAUTHENTICATED", "Authentication required");
        return OK;
    }

    Account acc;
    memset(&acc, 0, sizeof(acc));
    if (app->store) {
        store_account_get_by_id(app->store, user_id, &acc);
    }
    if (acc.name[0] == '\0') {
        snprintf(acc.name, sizeof(acc.name), "Player%llu", (unsigned long long)user_id);
        strcpy(acc.avatar, "fox");
    }

    if (strcmp(e->type, "ROOM_CREATE") == 0) {
        Mode mode = CLASSIC;
        struct json_object *m_obj = NULL;
        if (e->payload && json_object_object_get_ex(e->payload, "mode", &m_obj)) {
            parse_mode(m_obj, &mode);
        }

        char code[7] = {0};
        Status st = rooms_create(r, user_id, acc.name, acc.avatar, mode, code);
        if (st == BUSY) {
            send_error(app, conn_id, e->id, "BUSY", "User already in a room");
            return OK;
        }
        if (st == SERVER_FULL) {
            send_error(app, conn_id, e->id, "SERVER_FULL", "Server room capacity reached");
            return OK;
        }
        if (st != OK) {
            send_error(app, conn_id, e->id, "INVALID_ACTION", "Could not create room");
            return OK;
        }

        struct json_object *resp_payload = json_object_new_object();
        json_object_object_add(resp_payload, "roomId", json_object_new_string(code));
        json_object_object_add(resp_payload, "code", json_object_new_string(code));
        json_object_object_add(resp_payload, "mode", json_object_new_string(MODE_NAMES[mode]));
        json_object_object_add(resp_payload, "seat", json_object_new_int(0));
        send_ok(app, conn_id, e->id, resp_payload);

        Room *room = rooms_find_by_code(r, code);
        if (room) rooms_broadcast_state(app, room);
        return OK;
    }

    if (strcmp(e->type, "ROOM_JOIN") == 0) {
        struct json_object *c_obj = NULL;
        if (!e->payload || !json_object_object_get_ex(e->payload, "code", &c_obj)) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "Room code is required");
            return OK;
        }
        const char *code = json_object_get_string(c_obj);
        Seat seat = 0;
        Status st = rooms_join(r, user_id, acc.name, acc.avatar, code, &seat);
        if (st == BUSY) {
            send_error(app, conn_id, e->id, "BUSY", "User already in a room");
            return OK;
        }
        if (st == NOT_FOUND) {
            send_error(app, conn_id, e->id, "NOT_FOUND", "Room not found");
            return OK;
        }
        if (st == ROOM_FULL) {
            send_error(app, conn_id, e->id, "ROOM_FULL", "Room is full");
            return OK;
        }
        if (st != OK) {
            send_error(app, conn_id, e->id, "INVALID_ACTION", "Could not join room");
            return OK;
        }

        struct json_object *resp_payload = json_object_new_object();
        json_object_object_add(resp_payload, "roomId", json_object_new_string(code));
        json_object_object_add(resp_payload, "code", json_object_new_string(code));
        json_object_object_add(resp_payload, "seat", json_object_new_int(seat));
        send_ok(app, conn_id, e->id, resp_payload);

        Room *room = rooms_find_by_code(r, code);
        if (room) rooms_broadcast_state(app, room);
        return OK;
    }

    if (strcmp(e->type, "ROOM_LEAVE") == 0) {
        Room *room = rooms_find_by_user(r, user_id);
        if (!room) {
            send_ok(app, conn_id, e->id, NULL);
            return OK;
        }
        char code[7];
        strcpy(code, room->code);
        rooms_leave(r, user_id);
        send_ok(app, conn_id, e->id, NULL);

        Room *after = rooms_find_by_code(r, code);
        if (after) {
            rooms_broadcast_state(app, after);
        }
        return OK;
    }

    if (strcmp(e->type, "ROOM_MODE") == 0) {
        struct json_object *m_obj = NULL;
        if (!e->payload || !json_object_object_get_ex(e->payload, "mode", &m_obj)) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "Mode is required");
            return OK;
        }
        Mode mode = CLASSIC;
        if (!parse_mode(m_obj, &mode)) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "Invalid mode");
            return OK;
        }
        Status st = rooms_set_mode(r, user_id, mode);
        if (st == FORBIDDEN) {
            send_error(app, conn_id, e->id, "FORBIDDEN", "Only host can change room mode");
            return OK;
        }
        if (st == NOT_FOUND) {
            send_error(app, conn_id, e->id, "NOT_FOUND", "Room not found");
            return OK;
        }
        if (st != OK) {
            send_error(app, conn_id, e->id, "INVALID_ACTION", "Could not set mode");
            return OK;
        }

        struct json_object *resp_payload = json_object_new_object();
        json_object_object_add(resp_payload, "mode", json_object_new_string(MODE_NAMES[mode]));
        send_ok(app, conn_id, e->id, resp_payload);

        Room *room = rooms_find_by_user(r, user_id);
        if (room) rooms_broadcast_state(app, room);
        return OK;
    }

    if (strcmp(e->type, "ROOM_BOT") == 0) {
        struct json_object *s_obj = NULL, *l_obj = NULL;
        if (!e->payload ||
            !json_object_object_get_ex(e->payload, "seat", &s_obj) ||
            !json_object_object_get_ex(e->payload, "level", &l_obj)) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "Seat and level are required");
            return OK;
        }
        int seat_val = json_object_get_int(s_obj);
        if (seat_val < 0 || seat_val >= ROOM_MAX_SEATS) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "Seat must be between 0 and 3");
            return OK;
        }
        BotLevel level = EASY;
        if (!parse_bot_level(l_obj, &level)) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "Invalid bot level");
            return OK;
        }

        Status st = rooms_set_bot(r, user_id, seat_val, level);
        if (st == FORBIDDEN) {
            send_error(app, conn_id, e->id, "FORBIDDEN", "Only host can configure bots");
            return OK;
        }
        if (st == NOT_FOUND) {
            send_error(app, conn_id, e->id, "NOT_FOUND", "Room not found");
            return OK;
        }
        if (st != OK) {
            send_error(app, conn_id, e->id, "INVALID_ACTION", "Could not configure bot");
            return OK;
        }

        struct json_object *resp_payload = json_object_new_object();
        json_object_object_add(resp_payload, "seat", json_object_new_int(seat_val));
        const char *l_names[] = {"easy", "normal", "hard"};
        json_object_object_add(resp_payload, "level", json_object_new_string(l_names[level]));
        send_ok(app, conn_id, e->id, resp_payload);

        Room *room = rooms_find_by_user(r, user_id);
        if (room) rooms_broadcast_state(app, room);
        return OK;
    }

    if (strcmp(e->type, "ROOM_KICK") == 0) {
        struct json_object *s_obj = NULL;
        if (!e->payload || !json_object_object_get_ex(e->payload, "seat", &s_obj)) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "Seat is required");
            return OK;
        }
        int seat_val = json_object_get_int(s_obj);
        if (seat_val < 0 || seat_val >= ROOM_MAX_SEATS) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "Seat must be between 0 and 3");
            return OK;
        }
        Status st = rooms_kick(r, user_id, seat_val);
        if (st == FORBIDDEN) {
            send_error(app, conn_id, e->id, "FORBIDDEN", "Only host can kick members");
            return OK;
        }
        if (st == NOT_FOUND) {
            send_error(app, conn_id, e->id, "NOT_FOUND", "Room not found");
            return OK;
        }
        if (st != OK) {
            send_error(app, conn_id, e->id, "INVALID_ACTION", "Could not kick member");
            return OK;
        }

        struct json_object *resp_payload = json_object_new_object();
        json_object_object_add(resp_payload, "seat", json_object_new_int(seat_val));
        send_ok(app, conn_id, e->id, resp_payload);

        Room *room = rooms_find_by_user(r, user_id);
        if (room) rooms_broadcast_state(app, room);
        return OK;
    }

    if (strcmp(e->type, "ROOM_START") == 0) {
        Room *room = rooms_find_by_user(r, user_id);
        if (!room) {
            send_error(app, conn_id, e->id, "NOT_FOUND", "Room not found");
            return OK;
        }
        if (room->seats[room->host_seat].user_id != user_id) {
            send_error(app, conn_id, e->id, "FORBIDDEN", "Only host can start room");
            return OK;
        }

        Status st = rooms_start(app, room->code);
        if (st == BUSY) {
            send_error(app, conn_id, e->id, "BUSY", "Match engine not available");
            return OK;
        }
        if (st != OK) {
            send_error(app, conn_id, e->id, "INVALID_ACTION", "Cannot start room: all 4 seats must be filled");
            return OK;
        }

        send_ok(app, conn_id, e->id, NULL);
        return OK;
    }

    if (strcmp(e->type, "INVITE_SEND") == 0) {
        struct json_object *u_obj = NULL, *k_obj = NULL;
        if (!e->payload ||
            !json_object_object_get_ex(e->payload, "userId", &u_obj) ||
            !json_object_object_get_ex(e->payload, "kind", &k_obj)) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "userId and kind required");
            return OK;
        }

        UserId target_uid = 0;
        if (json_object_is_type(u_obj, json_type_string)) {
            target_uid = (UserId)strtoull(json_object_get_string(u_obj), NULL, 10);
        } else if (json_object_is_type(u_obj, json_type_int)) {
            target_uid = (UserId)json_object_get_int64(u_obj);
        }

        if (target_uid == 0 || target_uid == user_id) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "Invalid target user");
            return OK;
        }

        const char *kind = json_object_get_string(k_obj);
        if (strcasecmp(kind, "room") != 0 && strcasecmp(kind, "party") != 0) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "Kind must be room or party");
            return OK;
        }

        /* Check recipient is online */
        ConnId rc_conn;
        if (sessions_find_conn(app, target_uid, &rc_conn) != OK) {
            send_error(app, conn_id, e->id, "NOT_FOUND", "User is offline");
            return OK;
        }

        char room_code[7] = {0};
        if (strcasecmp(kind, "room") == 0) {
            Room *room = rooms_find_by_user(r, user_id);
            if (!room) {
                send_error(app, conn_id, e->id, "INVALID_ACTION", "You are not in a room");
                return OK;
            }
            int occ = 0;
            for (int i = 0; i < ROOM_MAX_SEATS; i++) {
                if (room->seats[i].occupied) occ++;
            }
            if (occ >= ROOM_MAX_SEATS) {
                send_error(app, conn_id, e->id, "ROOM_FULL", "Room is full");
                return OK;
            }
            if (rooms_find_by_user(r, target_uid) != NULL) {
                send_error(app, conn_id, e->id, "BUSY", "Target user is already in a room");
                return OK;
            }
            strcpy(room_code, room->code);
        } else {
            /* Party */
            Party *p = party_find_by_user(r, user_id);
            if (p && p->member_count >= PARTY_MAX_MEMBERS) {
                send_error(app, conn_id, e->id, "BUSY", "Party is full");
                return OK;
            }
            if (party_find_by_user(r, target_uid) != NULL) {
                send_error(app, conn_id, e->id, "BUSY", "Target user is already in a party");
                return OK;
            }
        }

        /* Create invite */
        char invite_id_buf[37];
        Status inv_st = rooms_invite_create(r, user_id, target_uid, kind, room_code, invite_id_buf);
        if (inv_st != OK) {
            send_error(app, conn_id, e->id, "SERVER_FULL", "Too many active invites");
            return OK;
        }
        Invite *inv = NULL;
        for (int i = 0; i < MAX_INVITES; i++) {
            if (r->invites[i].active && strcmp(r->invites[i].invite_id, invite_id_buf) == 0) {
                inv = &r->invites[i];
                break;
            }
        }
        if (!inv) {
            send_error(app, conn_id, e->id, "STORAGE_ERROR", "Failed to allocate invite");
            return OK;
        }

        /* Push INVITE to recipient */
        struct json_object *push_payload = json_object_new_object();
        json_object_object_add(push_payload, "inviteId", json_object_new_string(inv->invite_id));
        char sender_str[32];
        snprintf(sender_str, sizeof(sender_str), "%llu", (unsigned long long)user_id);
        json_object_object_add(push_payload, "senderId", json_object_new_string(sender_str));
        json_object_object_add(push_payload, "senderName", json_object_new_string(acc.name));
        json_object_object_add(push_payload, "kind", json_object_new_string(inv->kind));
        if (room_code[0]) {
            json_object_object_add(push_payload, "roomId", json_object_new_string(room_code));
        }

        Envelope push_env = {1, 0, "INVITE", push_payload};
        if (rc_conn.slot < APP_MAX_CONNS) {
            AppConnSlot *s = &app->slots[rc_conn.slot];
            if (s->active && s->generation == rc_conn.generation) {
                conn_queue(&s->conn, &push_env);
                conn_flush(&s->conn);
            }
        }
        json_object_put(push_payload);

        /* Send OK to sender */
        struct json_object *resp_payload = json_object_new_object();
        json_object_object_add(resp_payload, "inviteId", json_object_new_string(inv->invite_id));
        send_ok(app, conn_id, e->id, resp_payload);
        return OK;
    }

    if (strcmp(e->type, "INVITE_REPLY") == 0) {
        struct json_object *id_obj = NULL, *ac_obj = NULL;
        if (!e->payload ||
            !json_object_object_get_ex(e->payload, "inviteId", &id_obj) ||
            !json_object_object_get_ex(e->payload, "accept", &ac_obj)) {
            send_error(app, conn_id, e->id, "BAD_REQUEST", "inviteId and accept required");
            return OK;
        }

        const char *invite_id = json_object_get_string(id_obj);
        bool accept = json_object_get_boolean(ac_obj);

        Invite *inv = NULL;
        for (int i = 0; i < MAX_INVITES; i++) {
            if (r->invites[i].active && strcmp(r->invites[i].invite_id, invite_id) == 0) {
                inv = &r->invites[i];
                break;
            }
        }

        if (!inv || inv->recipient_id != user_id) {
            send_error(app, conn_id, e->id, "NOT_FOUND", "Invite not found");
            return OK;
        }

        uint64_t now = get_now_ms();
        if (now - inv->created_at_ms > INVITE_TIMEOUT_MS) {
            inv->active = false;
            send_error(app, conn_id, e->id, "NOT_FOUND", "Invite has expired");
            return OK;
        }

        if (!accept) {
            inv->active = false;
            send_ok(app, conn_id, e->id, NULL);
            return OK;
        }

        /* Accept invite */
        if (strcasecmp(inv->kind, "room") == 0) {
            if (rooms_find_by_user(r, user_id) != NULL) {
                send_error(app, conn_id, e->id, "BUSY", "Already in a room");
                return OK;
            }
            Seat seat = 0;
            Status st = rooms_join(r, user_id, acc.name, acc.avatar, inv->room_code, &seat);
            if (st == BUSY) {
                send_error(app, conn_id, e->id, "BUSY", "Already in a room");
                return OK;
            }
            if (st == NOT_FOUND) {
                inv->active = false;
                send_error(app, conn_id, e->id, "NOT_FOUND", "Room not found");
                return OK;
            }
            if (st == ROOM_FULL) {
                inv->active = false;
                send_error(app, conn_id, e->id, "ROOM_FULL", "Room is full");
                return OK;
            }
            if (st != OK) {
                inv->active = false;
                send_error(app, conn_id, e->id, "INVALID_ACTION", "Could not join room");
                return OK;
            }

            char code[7];
            strcpy(code, inv->room_code);
            inv->active = false;

            struct json_object *resp_payload = json_object_new_object();
            json_object_object_add(resp_payload, "roomId", json_object_new_string(code));
            json_object_object_add(resp_payload, "code", json_object_new_string(code));
            json_object_object_add(resp_payload, "seat", json_object_new_int(seat));
            send_ok(app, conn_id, e->id, resp_payload);

            Room *room = rooms_find_by_code(r, code);
            if (room) rooms_broadcast_state(app, room);
            return OK;
        } else {
            /* Party invite accepted */
            if (party_find_by_user(r, user_id) != NULL) {
                send_error(app, conn_id, e->id, "BUSY", "Already in a party");
                return OK;
            }
            Party *p = party_find_by_user(r, inv->sender_id);
            if (!p) {
                /* Create new party */
                int p_slot = -1;
                for (int i = 0; i < MAX_PARTIES; i++) {
                    if (!r->parties[i].active) {
                        p_slot = i;
                        break;
                    }
                }
                if (p_slot >= 0) {
                    p = &r->parties[p_slot];
                    p->active = true;
                    p->party_id = r->next_party_id++;
                    p->leader_id = inv->sender_id;
                    p->members[0] = inv->sender_id;
                    p->members[1] = user_id;
                    p->member_count = 2;
                    p->mode = TWO_V_TWO;
                    p->revision = 1;
                }
            } else {
                if (p->member_count < PARTY_MAX_MEMBERS) {
                    p->members[p->member_count++] = user_id;
                    p->revision++;
                } else {
                    inv->active = false;
                    send_error(app, conn_id, e->id, "BUSY", "Party is full");
                    return OK;
                }
            }

            inv->active = false;
            send_ok(app, conn_id, e->id, NULL);

            if (p) party_broadcast_state(app, p);
            return OK;
        }
    }

    if (strcmp(e->type, "PARTY_LEAVE") == 0) {
        party_leave(r, user_id);
        send_ok(app, conn_id, e->id, NULL);
        return OK;
    }

    send_error(app, conn_id, e->id, "BAD_REQUEST", "Unknown command");
    return OK;
}
