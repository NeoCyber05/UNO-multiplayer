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

typedef Status (*OnMatchStart)(App *app, const RoomStart *roomStart, MatchId *outMatchId);

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

/**
 * @brief Allocate the room, invite and party tables.
 * @param[out] outRooms Receives the tables; free with rooms_destroy.
 * @param[in] app Server state the tables push through; borrowed.
 * @param[in] onStart Called by rooms_start/rooms_make_match to open a match.
 * @return OK, BAD_REQUEST for NULL @p outRooms, or STORAGE_ERROR when out of memory.
 */
Status rooms_init(Rooms **outRooms, App *app, OnMatchStart onStart);

/** @brief Free the tables; NULL is ignored. */
void rooms_destroy(Rooms *rooms);

/**
 * @brief Pick a random 6-character room code from [0-9A-Z].
 *
 * Codes that look like queue match ids ("Q" + 5 digits) or belong to another
 * active room are drawn again.
 *
 * @param[in] rooms Tables to check for collisions, or NULL to skip that check.
 * @param[out] outCode Buffer of 7 bytes, NUL-terminated on return.
 */
void rooms_generate_code(Rooms *rooms, char outCode[7]);

/** @brief Active room with this code (case-insensitive), or NULL. */
Room *rooms_find_by_code(Rooms *rooms, const char *code);

/** @brief Active room where @p userId sits as a human, or NULL. */
Room *rooms_find_by_user(Rooms *rooms, UserId userId);

/**
 * @brief Open a waiting, unranked room with @p hostId in seat 0.
 * @param[in] name Display name, or NULL for "Host"; copied.
 * @param[in] avatar Avatar id, or NULL for "fox"; copied.
 * @param[out] outCode Receives the new code; may be NULL.
 * @return OK; BAD_REQUEST for invalid arguments; BUSY when the host already
 *         sits in a room; SERVER_FULL when all MAX_ROOMS are in use.
 */
Status rooms_create(Rooms *rooms, UserId hostId, const char *name, const char *avatar, Mode mode,
                    char outCode[7]);

/**
 * @brief Seat @p userId in the lowest free seat of the room named by @p code.
 * @param[out] outSeat Receives the seat; may be NULL.
 * @return OK; BAD_REQUEST for invalid arguments; BUSY when the user already sits
 *         in a room; NOT_FOUND for an unknown code; ROOM_FULL with no free seat.
 */
Status rooms_join(Rooms *rooms, UserId userId, const char *name, const char *avatar,
                  const char *code, Seat *outSeat);

/**
 * @brief Free the user's seat; the last human leaving closes the room and a
 *        leaving host hands the host seat to the lowest remaining human.
 * @return Always OK, also when the user sits in no room.
 */
Status rooms_leave(Rooms *rooms, UserId userId);

/**
 * @brief Put a bot of @p level in @p seat (host only).
 * @return OK; BAD_REQUEST for a bad seat; NOT_FOUND without a room; FORBIDDEN
 *         for a non-host; INVALID_ACTION for the host seat or a human's seat.
 */
Status rooms_set_bot(Rooms *rooms, UserId requesterId, int seat, BotLevel level);

/**
 * @brief Empty an occupied seat other than the host's (host only).
 * @return OK; BAD_REQUEST for a bad seat; NOT_FOUND without a room; FORBIDDEN
 *         for a non-host; INVALID_ACTION for the host seat or an empty seat.
 */
Status rooms_kick(Rooms *rooms, UserId requesterId, int seat);

/**
 * @brief Change the room's game mode (host only).
 * @return OK, BAD_REQUEST, NOT_FOUND without a room, or FORBIDDEN for a non-host.
 */
Status rooms_set_mode(Rooms *rooms, UserId requesterId, Mode mode);

/**
 * @brief Start the match of a full waiting room through Rooms.on_start.
 *
 * The room only switches to ROOM_STATUS_IN_MATCH when the callback succeeds;
 * on failure it stays waiting and unchanged.
 *
 * @return OK; BAD_REQUEST; NOT_FOUND for an unknown room; INVALID_ACTION when
 *         the room is not waiting or a seat is empty; BUSY without a callback;
 *         otherwise the callback's error.
 */
Status rooms_start(App *app, const char *roomId);

/**
 * @brief Store a room or party invite in a free or expired slot.
 *
 * A slot is reused once its invite is older than INVITE_TIMEOUT_MS.
 *
 * @param[in] kind "room" or "party"; copied.
 * @param[in] roomCode Room the invite points at (room invites); copied.
 * @param[out] outInviteId Receives a random UUID; buffer of 37 bytes.
 * @return OK, BAD_REQUEST for missing users, or SERVER_FULL with no free slot.
 */
Status rooms_invite_create(Rooms *rooms, UserId senderId, UserId recipientId, const char *kind,
                           const char *roomCode, char outInviteId[37]);

/**
 * @brief Clean up after a user dropped: leave their party, cancel invites they
 *        sent or received, and free a waiting-room seat (the room then gets a
 *        fresh state push). A seat in a running match is kept for RESUME/LOGIN.
 * @param[in] nowMs Currently unused.
 */
void rooms_disconnect(App *app, UserId userId, uint64_t nowMs);

/**
 * @brief Start a ranked matchmaking match for four humans through Rooms.on_start.
 *
 * Builds RoomStart{roomId = "Q" + 5 increasing digits, ranked = true} with each
 * seat's name and avatar from the store. No room is created: rooms_match_ended
 * ignores the "Q" id, and room codes never take that form.
 * @param[in,out] app Server state; app->rooms supplies on_start.
 * @param[in] mode Mode of the match.
 * @param[in] users Account per seat, already in seat order; 0 (bot) is rejected.
 * @param[out] outMatchId Receives the match id on success.
 * @return OK on success; BAD_REQUEST for NULL arguments or a zero user;
 *         BUSY without an on_start callback; the store lookup error; or the
 *         on_start (matchesStart) error such as BUSY, SERVER_FULL or STORAGE_ERROR.
 */
Status rooms_make_match(App *app, Mode mode, const UserId users[4], MatchId *outMatchId);

/* Envelope command dispatcher */
Status rooms_handle(App *app, ConnId connId, const Envelope *envelope);

/* Broadcast */
Status rooms_broadcast_state(App *app, const Room *room);

/**
 * @brief Release a room once its match is finalized so members may play again.
 * @param[in,out] app Server state; app->rooms owns the room table.
 * @param[in] roomId Room code carried by the finished match; an empty or unknown
 *            code (e.g. a queue match or a room every human already left) is ignored,
 *            as is a room not in ROOM_STATUS_IN_MATCH.
 */
void rooms_match_ended(App *app, const char *roomId);

/**
 * @brief Push the user's current ROOM_STATE and PARTY_STATE to their live connection.
 *
 * Used after LOGIN: a re-login moves the session to a new socket without leaving
 * the room or party, and that socket has not seen any snapshot yet.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] userId Account that just bound a connection.
 */
void rooms_sync_user(App *app, UserId userId);

/**
 * @brief Copy a renamed account's name and avatar into its waiting-room seat.
 *
 * A room still WAITING bumps its revision and broadcasts ROOM_STATE; a room
 * whose match already started keeps the identity the match was created with.
 * @param[in,out] app Server state; app->rooms owns the room table.
 * @param[in] userId Account that changed its profile; a user in no room is ignored.
 * @param[in] name New display name, already validated.
 * @param[in] avatar New avatar key, already validated.
 */
void rooms_update_identity(App *app, UserId userId, const char *name, const char *avatar);

/* Party operations */
Party *party_find_by_user(Rooms *rooms, UserId userId);
Status party_leave(Rooms *rooms, UserId userId);

#endif
