#ifndef UNO_STORE_H
#define UNO_STORE_H

#include "shared/contracts.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>

typedef struct {
    char email[255];
    char hash[128];
    char name[65];
    char avatar[32];
} AccountCreate;

typedef struct {
    UserId id;
    char email[255];
    char hash[128];
    char name[65];
    char avatar[32];
    int elo;
} Account;

/**
 * @brief Open (or create) the database and get it ready for the poll loop.
 *
 * Enables foreign keys, WAL and a 100 ms busy timeout, applies the embedded
 * schema and its migrations (tracked in PRAGMA user_version), marks matches
 * left ACTIVE by a crash as ABORTED, and prepares every statement once so
 * later calls never re-parse SQL.
 *
 * @param[out] out Receives the store; close it with store_close.
 * @param[in] path File path or ":memory:".
 * @return OK, BAD_REQUEST for NULL arguments, or STORAGE_ERROR.
 */
Status store_open(Store **out, const char *path);

/** @brief Finalize all statements, close the database and free @p store; NULL is ignored. */
void store_close(Store *store);

/**
 * @brief Insert a new ACTIVE match and its four seats in one transaction.
 *
 * Idempotent: repeating the call with the same id, mode, ranked flag and
 * players returns OK without writing.
 *
 * @return OK; BAD_REQUEST for missing arguments or an empty id; INVALID_ACTION
 *         when the id already exists with different data or is no longer
 *         ACTIVE; STORAGE_ERROR after a rolled-back write.
 */
Status store_match_start(Store *store, const MatchId *matchId, const PlayerSpec players[4],
                         Mode mode, bool ranked);

/**
 * @brief Append a batch of match events in one transaction.
 *
 * An event whose seq already exists with the same content is skipped, so a
 * retried batch is harmless.
 *
 * @return OK (also for an empty batch); NOT_FOUND for an unknown match;
 *         INVALID_ACTION when a stored seq differs; STORAGE_ERROR after rollback.
 */
Status store_events(Store *store, const MatchId *matchId, const EventBatch *batch);

/**
 * @brief Close a match: status, seats, ELO, profile stats and badges in one
 *        transaction.
 *
 * Idempotent per match id: finalizing an already FINALIZED match with the
 * same result returns OK and changes nothing, so ELO and stats are applied at
 * most once even when the caller retries after a crash or a timeout.
 *
 * @param[in] result Final result; eloAfter must already be filled (rating_apply).
 * @param[in] deltas Per-seat profile changes from profileCompute, or NULL.
 * @return OK; BAD_REQUEST; NOT_FOUND for an unknown match; INVALID_ACTION when
 *         the match was aborted or finalized with a different result;
 *         STORAGE_ERROR after rollback.
 */
Status store_finalize(Store *store, const MatchResult *result, const ProfileDelta deltas[4]);

/**
 * @brief Write one match, its seats and events as a JSON file (debug/demo aid).
 * @param[in] path Destination file; overwritten.
 * @return OK, BAD_REQUEST for NULL arguments, NOT_FOUND for an unknown match,
 *         or STORAGE_ERROR.
 */
Status store_export_match(Store *store, const MatchId *matchId, const char *path);

/**
 * @brief Mark an active match ABORTED and record its abort event atomically.
 * @param[in] store Open store.
 * @param[in] id Match to abort.
 * @param[in] event Abort event appended to the match log (e.g. MATCH_ABORTED).
 * @param[in] durationMs Played duration saved with the aborted match.
 * @return OK when the match is now ABORTED, including a retry on a match that
 *         is already ABORTED (nothing more is written);
 *         BAD_REQUEST for NULL arguments; NOT_FOUND for an unknown match;
 *         INVALID_ACTION when the match is already FINALIZED;
 *         STORAGE_ERROR when SQLite fails (the match stays ACTIVE).
 */
Status store_abort(Store *store, const MatchId *id, const MatchEvent *event, uint64_t durationMs);

enum { STORE_LEADERBOARD_MAX = 20 };

typedef struct { UserId userId; char name[65], avatar[32]; int elo; } LeaderboardEntry;

/**
 * @brief Read the top accounts by ELO.
 * @param[in] store Open store.
 * @param[out] out Receives up to STORE_LEADERBOARD_MAX rows, highest ELO first;
 *                 equal ELO is ordered by the lower account id first.
 * @param[out] count Number of rows written to out.
 * @return OK on success (count may be 0); BAD_REQUEST for NULL arguments;
 *         STORAGE_ERROR when SQLite fails (count is left unchanged).
 */
Status store_leaderboard(Store *store, LeaderboardEntry out[STORE_LEADERBOARD_MAX], size_t *count);

enum {
    STORE_FRIEND_MAX = 64,
    STORE_HISTORY_MAX = 20,
    STORE_MODE_COUNT = 4 /* CLASSIC, TWO_V_TWO, SIDE, DRAFT */
};

/* Relation as seen by the account that asked for the list: friends rows store
 * (user_id = sender, friend_id = receiver), so the direction of a pending row
 * tells who still has to reply. */
typedef enum { FRIEND_NONE, FRIEND_PENDING_OUT, FRIEND_PENDING_IN, FRIEND_ACCEPTED } FriendLink;

typedef struct { UserId userId; char name[65], avatar[32]; int elo; FriendLink link; } FriendRow;

/**
 * @brief Send a friend request from one account to another.
 * @param[in] store Open store.
 * @param[in] from Account sending the request.
 * @param[in] to Account receiving the request.
 * @return OK when a pending request from -> to was stored;
 *         BAD_REQUEST for a NULL store or from == to;
 *         NOT_FOUND when the account to does not exist;
 *         INVALID_ACTION when the two accounts are already linked in either
 *         direction (pending or accepted);
 *         STORAGE_ERROR when SQLite fails (nothing is written).
 */
Status store_friend_request(Store *store, UserId from, UserId to);

/**
 * @brief Accept or reject a pending request sent by requester to me.
 * @param[in] store Open store.
 * @param[in] me Account that received the request.
 * @param[in] requester Account that sent the request.
 * @param[in] accept true marks the link accepted, false deletes the request so
 *                   it can be sent again later.
 * @return OK when the request was accepted or removed;
 *         BAD_REQUEST for a NULL store;
 *         NOT_FOUND when there is no pending request requester -> me;
 *         STORAGE_ERROR when SQLite fails (the request is unchanged).
 */
Status store_friend_reply(Store *store, UserId me, UserId requester, bool accept);

/**
 * @brief List every friend and pending request involving an account.
 * @param[in] store Open store.
 * @param[in] me Account whose links are listed.
 * @param[out] out Receives up to STORE_FRIEND_MAX rows: accepted friends first,
 *                 then pending requests, each group ordered by user id, so a
 *                 flood of requests never hides a real friend. Each row has the
 *                 other account's public identity and the link as seen from me.
 * @param[out] count Number of rows written to out.
 * @return OK on success (count may be 0); BAD_REQUEST for NULL arguments;
 *         STORAGE_ERROR when SQLite fails (count is left unchanged).
 */
Status store_friend_list(Store *store, UserId me, FriendRow out[STORE_FRIEND_MAX], size_t *count);

/* endedAt is Unix seconds, the same clock as matches.created_at. */
typedef struct { char matchId[37]; Mode mode; bool won; int eloDelta; int64_t endedAt; } HistoryRow;

/* Public profile only: no email, password hash or token, so callers can put
 * the whole struct on the wire for any viewer. */
typedef struct {
    UserId userId; char name[65], avatar[32]; int elo;
    ProfileStats stats; uint32_t badges;           /* bit OR from achievements */
    struct { uint32_t matches, wins; } modes[STORE_MODE_COUNT]; /* finalized only */
    HistoryRow history[STORE_HISTORY_MAX]; size_t historyCount; /* newest first */
} Profile;

/**
 * @brief Read an account's public profile, mode totals and recent history.
 * @param[in] store Open store.
 * @param[in] id Account to read.
 * @param[out] out Filled profile; modes and history count FINALIZED matches
 *                 only, and a player who left never counts as a winner.
 * @return OK on success; BAD_REQUEST for NULL arguments;
 *         NOT_FOUND when the account does not exist;
 *         STORAGE_ERROR when SQLite fails (out may be partly written).
 */
Status store_profile_get(Store *store, UserId id, Profile *out);

/**
 * @brief Change an account's display name and avatar.
 * @param[in] store Open store.
 * @param[in] id Account to update.
 * @param[in] name New name, already checked by accounts_validate_name.
 * @param[in] avatar New avatar key, already checked by accounts_validate_avatar.
 * @return OK when the account was updated; BAD_REQUEST for NULL arguments;
 *         NOT_FOUND when the account does not exist;
 *         STORAGE_ERROR when SQLite fails (the account is unchanged).
 */
Status store_profile_update(Store *store, UserId id, const char *name, const char *avatar);

Status store_account_create(Store *store, const AccountCreate *newAccount, UserId *outId);
Status store_account_get(Store *store, const char *email, Account *out);
Status store_account_get_by_id(Store *store, UserId id, Account *out);
Status store_profile_stats(Store *store, UserId userId, ProfileStats *out);

sqlite3 *store_get_db(Store *store);

#endif
