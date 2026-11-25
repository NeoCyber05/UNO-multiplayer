#ifndef UNO_QUEUE_H
#define UNO_QUEUE_H

#include "shared/contracts.h"
#include "shared/protocol.h"

enum {
    QUEUE_MATCH_SEATS = 4,
    QUEUE_ENTRY_MAX_USERS = 2,
    /* One entry per connection is the most the server can ever hold per mode. */
    QUEUE_MAX_ENTRIES = 64,
    /* Back-off before re-trying a match start that failed (storage, capacity). */
    QUEUE_RETRY_MS = 500
};

typedef struct {
    UserId users[QUEUE_ENTRY_MAX_USERS];
    size_t count; /* 1 = solo, 2 = party */
} QueueEntry;

/**
 * @brief Allocate an empty matchmaking queue for every mode.
 * @param[out] out Receives the queue; destroy it with queue_destroy.
 * @return OK on success, BAD_REQUEST if out is NULL, STORAGE_ERROR if allocation fails.
 */
Status queue_init(Queue **out);

/**
 * @brief Free a queue created by queue_init.
 * @param[in] q Queue to free, or NULL.
 */
void queue_destroy(Queue *q);

/**
 * @brief Append a solo or party entry at the back of a mode's FIFO queue.
 * @param[in,out] q Queue owned by the poll loop.
 * @param[in] mode Mode the entry waits for.
 * @param[in] e Entry copied into the queue.
 * @return OK when queued; BAD_REQUEST for a bad mode, count, zero or repeated user;
 *         INVALID_ACTION for a party in CLASSIC or DRAFT (parties only play team modes);
 *         BUSY if any user already waits in any mode; SERVER_FULL if the mode is full.
 */
Status queue_add(Queue *q, Mode mode, const QueueEntry *e);

/**
 * @brief Check whether a user waits in any mode's queue.
 * @param[in] q Queue, or NULL.
 * @param[in] u Account to look for.
 * @return true if some entry contains u.
 */
bool queue_contains(const Queue *q, UserId u);

/**
 * @brief Remove the whole entry containing a user, keeping FIFO order of the rest.
 * @param[in,out] q Queue owned by the poll loop.
 * @param[in] u Account whose entry (solo or party) is dropped.
 * @param[out] mode_out Receives the entry's mode; may be NULL.
 * @param[out] removed Receives a copy of the removed entry; may be NULL.
 * @return true if an entry was removed, false if u was not queued.
 */
bool queue_remove(Queue *q, UserId u, Mode *mode_out, QueueEntry *removed);

/**
 * @brief Greedily choose the oldest entries that add up to exactly four players.
 *
 * Entries are scanned in FIFO order and taken while the total stays <= 4.
 * CLASSIC/DRAFT seat players in pick order; in 2v2 team A is seats {0,2} and
 * B is {1,3}, in side A is {0,1} and B is {2,3}. The first party takes team A,
 * the second team B, and solos fill the remaining seats in FIFO order.
 * @param[in] q Queue to inspect (not modified).
 * @param[in] mode Mode to match.
 * @param[out] seats_out Seat order for the match, valid only on true.
 * @param[out] idx_out Indexes of the picked entries in ascending order.
 * @param[out] n_entries Number of indexes written to idx_out.
 * @return true if four players were found, false otherwise.
 */
bool queue_pick(const Queue *q, Mode mode, UserId seats_out[QUEUE_MATCH_SEATS],
                size_t idx_out[QUEUE_MATCH_SEATS], size_t *n_entries);

/**
 * @brief Drop the entries queue_pick returned once their match has started.
 * @param[in,out] q Queue owned by the poll loop.
 * @param[in] mode Mode the indexes refer to.
 * @param[in] idx Entry indexes from queue_pick; out-of-range indexes are ignored.
 * @param[in] n Number of indexes.
 */
void queue_take(Queue *q, Mode mode, const size_t idx[], size_t n);

/**
 * @brief Handle QUEUE_JOIN {mode} and QUEUE_LEAVE {} for the session's account.
 *
 * QUEUE_JOIN queues the caller's party when they have one (the mode must equal
 * the party mode), replies OK {queue: QUEUE_STATE payload}, pushes QUEUE_STATE
 * to the mode's queue and tries to start a ranked match at once.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] conn Connection that sent the request.
 * @param[in] e Request envelope borrowed for the call.
 * @return OK once a reply was sent (errors travel as ERROR replies),
 *         BAD_REQUEST if the arguments are NULL.
 */
Status queue_handle(App *app, ConnId conn, const Envelope *e);

/**
 * @brief Drop a user's entry after disconnect or PARTY_LEAVE and notify everyone affected.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] u Account that left; a user who is not queued is ignored.
 */
void queue_user_gone(App *app, UserId u);

/**
 * @brief Prune entries whose users went offline and retry modes whose start failed.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] now_ms Monotonic time from app_now_ms.
 */
void queue_tick(App *app, uint64_t now_ms);

#endif
