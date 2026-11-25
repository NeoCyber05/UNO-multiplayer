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

Status store_open(Store **out, const char *path);
void store_close(Store *s);
Status store_match_start(Store *s, const MatchId *match_id, const PlayerSpec players[4], Mode mode, bool ranked);
Status store_events(Store *s, const MatchId *match_id, const EventBatch *batch);
Status store_finalize(Store *s, const MatchResult *result, const ProfileDelta deltas[4]);
Status store_export_match(Store *s, const MatchId *match_id, const char *path);

/**
 * @brief Mark an active match ABORTED and record its abort event atomically.
 * @param[in] s Open store.
 * @param[in] id Match to abort.
 * @param[in] event Abort event appended to the match log (e.g. MATCH_ABORTED).
 * @param[in] duration_ms Played duration saved with the aborted match.
 * @return OK when the match is now ABORTED, including a retry on a match that
 *         is already ABORTED (nothing more is written);
 *         BAD_REQUEST for NULL arguments; NOT_FOUND for an unknown match;
 *         INVALID_ACTION when the match is already FINALIZED;
 *         STORAGE_ERROR when SQLite fails (the match stays ACTIVE).
 */
Status store_abort(Store *s, const MatchId *id, const MatchEvent *event, uint64_t duration_ms);

enum { STORE_LEADERBOARD_MAX = 20 };

typedef struct { UserId userId; char name[65], avatar[32]; int elo; } LeaderboardEntry;

/**
 * @brief Read the top accounts by ELO.
 * @param[in] s Open store.
 * @param[out] out Receives up to STORE_LEADERBOARD_MAX rows, highest ELO first;
 *                 equal ELO is ordered by the lower account id first.
 * @param[out] count Number of rows written to out.
 * @return OK on success (count may be 0); BAD_REQUEST for NULL arguments;
 *         STORAGE_ERROR when SQLite fails (count is left unchanged).
 */
Status store_leaderboard(Store *s, LeaderboardEntry out[STORE_LEADERBOARD_MAX], size_t *count);

Status store_account_create(Store *s, const AccountCreate *c, UserId *out_id);
Status store_account_get(Store *s, const char *email, Account *out);
Status store_account_get_by_id(Store *s, UserId id, Account *out);
Status store_profile_stats(Store *s, UserId user_id, ProfileStats *out);

sqlite3 *store_get_db(Store *s);

#endif
