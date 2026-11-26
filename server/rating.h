#ifndef UNO_RATING_H
#define UNO_RATING_H

#include "shared/contracts.h"

#include <stdbool.h>

enum {
    RATING_K_FACTOR = 32,
    RATING_SILVER_MIN = 1200,
    RATING_GOLD_MIN = 1400,
    RATING_DIAMOND_MIN = 1800
};

typedef enum { RANK_BRONZE, RANK_SILVER, RANK_GOLD, RANK_DIAMOND } Rank;

/**
 * @brief Compute the ELO change for one side of a finished match.
 * @param[in] own ELO of the player, or the average ELO of the player's team.
 * @param[in] opposition Average ELO of the opponents.
 * @param[in] won true when this side won (score 1), false when it lost (score 0).
 * @return lround(K * (score - expected)), rounded half away from zero.
 */
int rating_delta(double own, double opposition, bool won);

/**
 * @brief Map an ELO value to one of the four rank tiers.
 * @param[in] elo Current ELO (never negative after rating_apply).
 * @return RANK_BRONZE below 1200, RANK_SILVER 1200..1399,
 *         RANK_GOLD 1400..1799, RANK_DIAMOND from 1800.
 */
Rank rating_rank(int elo);

/**
 * @brief Return the wire key used for a rank in protocol messages.
 * @param[in] rank Rank tier.
 * @return Static string "bronze", "silver", "gold" or "diamond";
 *         "bronze" for an out-of-range value.
 */
const char *rating_rank_key(Rank rank);

/**
 * @brief Fill eloAfter for every seat of a match result.
 * @param[in,out] result Result whose eloBefore, mode, team and winner fields are read.
 * @param[in] isBot Per-seat bot flags, or NULL when every seat is human.
 *
 * Only a ranked, finalized match changes ELO; otherwise (and for bot seats)
 * eloAfter equals eloBefore. Solo modes compare each player with the average
 * of the three others; team modes compare team averages and give both
 * teammates the same delta. The result is clamped to be >= 0.
 */
void rating_apply(MatchResult *result, const bool isBot[4]);

#endif
