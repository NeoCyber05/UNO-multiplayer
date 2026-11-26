#include "server/rating.h"

#include <math.h>

enum {
    RATING_SEAT_COUNT = 4,
    RATING_ELO_FLOOR = 0
};

/* Standard ELO logistic: a 400-point gap means 10:1 expected odds. */
static const double RATING_SCALE = 400.0;
static const double RATING_BASE = 10.0;

int ratingDelta(double own, double opposition, bool won) {
    double expected = 1.0 / (1.0 + pow(RATING_BASE, (opposition - own) / RATING_SCALE));
    double score = won ? 1.0 : 0.0;
    /* lround rounds half away from zero, as the spec requires. */
    return (int)lround(RATING_K_FACTOR * (score - expected));
}

Rank ratingRank(int elo) {
    if (elo >= RATING_DIAMOND_MIN) {
        return RANK_DIAMOND;
    }
    if (elo >= RATING_GOLD_MIN) {
        return RANK_GOLD;
    }
    if (elo >= RATING_SILVER_MIN) {
        return RANK_SILVER;
    }
    return RANK_BRONZE;
}

const char *ratingRankKey(Rank rank) {
    switch (rank) {
    case RANK_SILVER:
        return "silver";
    case RANK_GOLD:
        return "gold";
    case RANK_DIAMOND:
        return "diamond";
    case RANK_BRONZE:
    default:
        return "bronze";
    }
}

static bool isTeamMode(Mode mode) {
    return mode == TWO_V_TWO || mode == SIDE;
}

/* Average eloBefore over seats that are (or are not) on `team`, skipping `exclude`. */
static bool averageRating(const MatchResult *result, int team, bool sameTeam,
                           int exclude, double *average) {
    double total = 0.0;
    int count = 0;
    for (int seat = 0; seat < RATING_SEAT_COUNT; seat++) {
        if (seat == exclude) {
            continue;
        }
        if (team >= 0 && (result->players[seat].team == team) != sameTeam) {
            continue;
        }
        total += result->players[seat].eloBefore;
        count++;
    }
    if (count == 0) {
        return false;
    }
    *average = total / count;
    return true;
}

static int seatDelta(const MatchResult *result, int seat) {
    double own = result->players[seat].eloBefore;
    double opposition = 0.0;
    bool won = false;
    if (isTeamMode(result->mode)) {
        int team = result->players[seat].team;
        /* Both teammates read the same two averages, so they get the same delta. */
        if (!averageRating(result, team, true, -1, &own) ||
            !averageRating(result, team, false, -1, &opposition)) {
            return 0;
        }
        won = team == result->winnerTeam;
    } else {
        /* team < 0 disables the team filter: every other seat is an opponent. */
        if (!averageRating(result, -1, false, seat, &opposition)) {
            return 0;
        }
        won = seat == result->winnerSeat;
    }
    return ratingDelta(own, opposition, won);
}

void ratingApply(MatchResult *result, const bool isBot[4]) {
    if (result == NULL) {
        return;
    }
    bool changes = result->ranked && result->status == MATCH_FINALIZED;
    /* Compute every delta from eloBefore only, so seat order cannot matter. */
    for (int seat = 0; seat < RATING_SEAT_COUNT; seat++) {
        PlayerResult *player = &result->players[seat];
        bool isBotSeat = isBot != NULL && isBot[seat];
        if (!changes || isBotSeat) {
            player->eloAfter = player->eloBefore;
            continue;
        }
        int eloAfter = player->eloBefore + seatDelta(result, seat);
        player->eloAfter = eloAfter < RATING_ELO_FLOOR ? RATING_ELO_FLOOR : eloAfter;
    }
}
