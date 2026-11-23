#ifndef UNO_MATCH_H
#define UNO_MATCH_H

#include "rooms.h"

typedef void (*MatchPush)(void *context, UserId userId, const char *type,
                          json_object *payload);
typedef void (*MatchEnded)(void *context, const char *roomId);

/**
 * @brief Create a coordinator owned by the server poll loop.
 * @param store Open store borrowed for the coordinator's lifetime.
 * @param push Callback that queues an envelope for an authenticated user.
 * @param ended Optional callback that releases room membership after finalization.
 * @param context Caller-owned context passed to callbacks.
 * @return A coordinator to destroy with matchesDestroy, or NULL on failure.
 *
 * Callbacks borrow their arguments only for the call and must not reenter.
 */
Matches *matchesCreate(Store *store, MatchPush push, MatchEnded ended, void *context);

/**
 * @brief Free the coordinator after the poll loop stops.
 * @param matches Coordinator owned by the caller, or NULL.
 */
void matchesDestroy(Matches *matches);

/**
 * @brief Persist an unranked match before sending its initial snapshots.
 * @param matches Coordinator owned by the poll loop.
 * @param room Occupied seats with authenticated humans and optional bots.
 * @param nowMs Current time from the poll loop's monotonic clock.
 * @param outMatchId Receives the new ID only on success.
 * @return OK on success, or a validation, capacity or storage error.
 */
Status matchesStart(Matches *matches, const RoomStart *room, uint64_t nowMs,
                    MatchId *outMatchId);

/**
 * @brief Check whether an account is reserved by a match awaiting completion.
 * @param matches Coordinator owned by the poll loop.
 * @param userId Account to check.
 * @return True if the account occupies a seat or left a match still in progress.
 */
bool matchesContains(const Matches *matches, UserId userId);

/**
 * @brief Update match presence following session disconnect or successful login/RESUME.
 * @param matches Coordinator owned by the poll loop.
 * @param userId Account from the authenticated session.
 * @param connected Whether the session now has a live connection.
 * @param nowMs Current time from the poll loop's monotonic clock.
 * @return OK if present, NOT_FOUND without a match, or FORBIDDEN after MATCH_LEAVE.
 */
Status matchesConnected(Matches *matches, UserId userId, bool connected, uint64_t nowMs);

/**
 * @brief Validate a match command against the authenticated account and game state.
 * @param matches Coordinator owned by the poll loop.
 * @param userId Account from the bound session.
 * @param request Envelope borrowed during dispatch.
 * @param nowMs Current time, used to resolve any deadline before the command.
 * @return OK on success, or the status to send in an ERROR response.
 */
Status matchesHandle(Matches *matches, UserId userId, const Envelope *request, uint64_t nowMs);

/**
 * @brief Advance due timers, bot actions and storage retries.
 * @param matches Coordinator owned by the poll loop.
 * @param nowMs Current time from the same monotonic clock as matchesStart.
 */
void matchesTick(Matches *matches, uint64_t nowMs);

#endif
