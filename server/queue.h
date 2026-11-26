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
 * @param[out] outQueue Receives the queue; destroy it with queue_destroy.
 * @return OK on success, BAD_REQUEST if out is NULL, STORAGE_ERROR if allocation fails.
 */
Status queue_init(Queue **outQueue);

/**
 * @brief Free a queue created by queue_init.
 * @param[in] queue Queue to free, or NULL.
 */
void queue_destroy(Queue *queue);

/**
 * @brief Append a solo or party entry at the back of a mode's FIFO queue.
 * @param[in,out] queue Queue owned by the poll loop.
 * @param[in] mode Mode the entry waits for.
 * @param[in] entry Entry copied into the queue.
 * @return OK when queued; BAD_REQUEST for a bad mode, count, zero or repeated user;
 *         INVALID_ACTION for a party in CLASSIC or DRAFT (parties only play team modes);
 *         BUSY if any user already waits in any mode; SERVER_FULL if the mode is full.
 */
Status queue_add(Queue *queue, Mode mode, const QueueEntry *entry);

/**
 * @brief Check whether a user waits in any mode's queue.
 * @param[in] queue Queue, or NULL.
 * @param[in] user Account to look for.
 * @return true if some entry contains u.
 */
bool queue_contains(const Queue *queue, UserId user);

/**
 * @brief Remove the whole entry containing a user, keeping FIFO order of the rest.
 * @param[in,out] queue Queue owned by the poll loop.
 * @param[in] user Account whose entry (solo or party) is dropped.
 * @param[out] outMode Receives the entry's mode; may be NULL.
 * @param[out] removed Receives a copy of the removed entry; may be NULL.
 * @return true if an entry was removed, false if u was not queued.
 */
bool queue_remove(Queue *queue, UserId user, Mode *outMode, QueueEntry *removed);

/**
 * @brief Choose the earliest entries by FIFO index that add up to exactly four players.
 *
 * Among all sets of entries totalling four, the one whose ascending index list
 * is lexicographically smallest wins: the oldest entry plays whenever any valid
 * set contains it, and a valid set is never missed (e.g. [party, solo, party]
 * matches the two parties). CLASSIC/DRAFT seat players in pick order; in 2v2 team A is seats {0,2}
 * and B is {1,3}, in side A is {0,1} and B is {2,3}. The first party takes team A,
 * the second team B, and solos fill the remaining seats in FIFO order.
 * @param[in] queue Queue to inspect (not modified).
 * @param[in] mode Mode to match.
 * @param[out] outSeats Seat order for the match, valid only on true.
 * @param[out] outIndexes Indexes of the picked entries in ascending order.
 * @param[out] outEntryCount Number of indexes written to outIndexes.
 * @return true if four players were found, false otherwise.
 */
bool queue_pick(const Queue *queue, Mode mode, UserId outSeats[QUEUE_MATCH_SEATS],
                size_t outIndexes[QUEUE_MATCH_SEATS], size_t *outEntryCount);

/**
 * @brief Drop the entries queue_pick returned once their match has started.
 * @param[in,out] queue Queue owned by the poll loop.
 * @param[in] mode Mode the indexes refer to.
 * @param[in] indexes Entry indexes from queue_pick; out-of-range indexes are ignored.
 * @param[in] count Number of indexes.
 */
void queue_take(Queue *queue, Mode mode, const size_t indexes[], size_t count);

/**
 * @brief Handle QUEUE_JOIN {mode} and QUEUE_LEAVE {} for the session's account.
 *
 * QUEUE_JOIN queues the caller's party when they have one (the mode must equal
 * the party mode), replies OK {queue: QUEUE_STATE payload}, pushes QUEUE_STATE
 * to the mode's queue and tries to start a ranked match at once.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] conn Connection that sent the request.
 * @param[in] envelope Request envelope borrowed for the call.
 * @return OK once a reply was sent (errors travel as ERROR replies),
 *         BAD_REQUEST if the arguments are NULL.
 */
Status queue_handle(App *app, ConnId conn, const Envelope *envelope);

/**
 * @brief Drop a user's entry after disconnect or PARTY_LEAVE and notify everyone affected.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] user Account that left; a user who is not queued is ignored.
 */
void queue_user_gone(App *app, UserId user);

/**
 * @brief Resend the user's QUEUE_STATE after LOGIN or RESUME bound a new socket.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] user Account that just bound a connection; a user who is not queued
 *              gets nothing.
 */
void queue_sync_user(App *app, UserId user);

/**
 * @brief Prune entries whose users went offline and retry modes whose start failed.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] nowMs Monotonic time from app_now_ms.
 */
void queue_tick(App *app, uint64_t nowMs);

#endif
