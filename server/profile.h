#ifndef UNO_PROFILE_H
#define UNO_PROFILE_H

#include "shared/contracts.h"

enum { PROFILE_SEAT_COUNT = 4 };

/**
 * @brief Compute profile deltas without writing player stats.
 * @param[in] store Repository used to read current profile stats.
 * @param[in] result Completed result, or an aborted result with no changes to award.
 *                   A seat without userId is a bot: the match then counts only `matches`.
 * @param[out] out Receives one delta per seat on success.
 * @return OK on success, BAD_REQUEST for invalid inputs, or a repository error.
 *
 * The finalization transaction applies the deltas once per match.
 */
Status profileCompute(Store *store, const MatchResult *result,
                      ProfileDelta out[PROFILE_SEAT_COUNT]);

#endif
