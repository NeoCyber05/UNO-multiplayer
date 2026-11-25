#ifndef UNO_ROOMS_H
#define UNO_ROOMS_H

#include "shared/contracts.h"
#include "shared/protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MAX_ROOMS = 16,
    ROOM_MAX_SEATS = 4,
    MAX_INVITES = 64,
    MAX_PARTIES = 32,
    PARTY_MAX_MEMBERS = 2,
    INVITE_TIMEOUT_MS = 30000,
    /* Queue matches use room ids "Q" + 5 digits, so the serial wraps here. */
    QUEUE_ROOM_SERIAL_LIMIT = 100000
};

typedef enum {
    ROOM_STATUS_WAITING,
    ROOM_STATUS_IN_MATCH
} RoomStatus;

typedef struct {
    char roomId[7];
    Mode mode;
    bool ranked;
    PlayerSpec players[4];
} RoomStart;

typedef Status (*OnMatchStart)(App *app, const RoomStart *rs, MatchId *out_match_id);

typedef struct {
    bool occupied;
    bool is_bot;
    UserId user_id;
    char name[65];
    char avatar[32];
    BotLevel bot_level;
} RoomSeat;

typedef struct {
    bool active;
    char code[7];
    Mode mode;
    bool ranked;
    Seat host_seat;
    RoomSeat seats[ROOM_MAX_SEATS];
    uint64_t revision;
    RoomStatus status;
} Room;

typedef struct {
    bool active;
    char invite_id[37];
    UserId sender_id;
    UserId recipient_id;
    char kind[16]; /* "room" or "party" */
    char room_code[7];
    Mode mode; /* party invites only: mode the new party queues for */
    uint64_t created_at_ms;
} Invite;

typedef struct {
    bool active;
    uint32_t party_id;
    UserId leader_id;
    UserId members[PARTY_MAX_MEMBERS];
    size_t member_count;
    Mode mode;
    uint64_t revision;
} Party;

struct Rooms {
    App *app;
    OnMatchStart on_start;
    Room rooms[MAX_ROOMS];
    Invite invites[MAX_INVITES];
    Party parties[MAX_PARTIES];
    uint32_t next_party_id;
    uint32_t next_invite_id;
    uint32_t next_queue_room; /* serial for the next "Qnnnnn" queue match id */
};

/* Lifecycle */
Status rooms_init(Rooms **out, App *app, OnMatchStart on_start);
void rooms_destroy(Rooms *r);

/* Code generation & helpers */
void rooms_generate_code(Rooms *r, char out_code[7]);
Room *rooms_find_by_code(Rooms *r, const char *code);
Room *rooms_find_by_user(Rooms *r, UserId user_id);

/* Direct Room operations */
Status rooms_create(Rooms *r, UserId host_id, const char *name, const char *avatar, Mode mode, char out_code[7]);
Status rooms_join(Rooms *r, UserId user_id, const char *name, const char *avatar, const char *code, Seat *out_seat);
Status rooms_leave(Rooms *r, UserId user_id);
Status rooms_set_bot(Rooms *r, UserId requester_id, int seat, BotLevel level);
Status rooms_kick(Rooms *r, UserId requester_id, int seat);
Status rooms_set_mode(Rooms *r, UserId requester_id, Mode mode);
Status rooms_start(App *app, const char *room_id);

/* Invite operations */
Status rooms_invite_create(Rooms *r, UserId sender_id, UserId recipient_id, const char *kind, const char *room_code, char out_invite_id[37]);

/* Disconnect handler */
void rooms_disconnect(App *app, UserId user_id, uint64_t now_ms);

/**
 * @brief Start a ranked matchmaking match for four humans through Rooms.on_start.
 *
 * Builds RoomStart{roomId = "Q" + 5 increasing digits, ranked = true} with each
 * seat's name and avatar from the store. No room is created: rooms_match_ended
 * ignores the "Q" id, and room codes never take that form.
 * @param[in,out] app Server state; app->rooms supplies on_start.
 * @param[in] mode Mode of the match.
 * @param[in] users Account per seat, already in seat order; 0 (bot) is rejected.
 * @param[out] out_match_id Receives the match id on success.
 * @return OK on success; BAD_REQUEST for NULL arguments or a zero user;
 *         BUSY without an on_start callback; the store lookup error; or the
 *         on_start (matchesStart) error such as BUSY, SERVER_FULL or STORAGE_ERROR.
 */
Status rooms_make_match(App *app, Mode mode, const UserId users[4], MatchId *out_match_id);

/* Envelope command dispatcher */
Status rooms_handle(App *app, ConnId conn_id, const Envelope *e);

/* Broadcast */
Status rooms_broadcast_state(App *app, const Room *room);

/**
 * @brief Release a room once its match is finalized so members may play again.
 * @param[in,out] app Server state; app->rooms owns the room table.
 * @param[in] room_id Room code carried by the finished match; an empty or unknown
 *            code (e.g. a queue match or a room every human already left) is ignored,
 *            as is a room not in ROOM_STATUS_IN_MATCH.
 */
void rooms_match_ended(App *app, const char *room_id);

/**
 * @brief Push the user's current ROOM_STATE and PARTY_STATE to their live connection.
 *
 * Used after LOGIN: a re-login moves the session to a new socket without leaving
 * the room or party, and that socket has not seen any snapshot yet.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] user_id Account that just bound a connection.
 */
void rooms_sync_user(App *app, UserId user_id);

/* Party operations */
Party *party_find_by_user(Rooms *r, UserId user_id);
Status party_leave(Rooms *r, UserId user_id);

#endif
