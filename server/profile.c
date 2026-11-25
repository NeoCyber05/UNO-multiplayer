#include "profile.h"
#include "store.h"

#include <string.h>

enum {
    FIRST_WIN_GOAL = 1,
    TOTAL_WINS_GOAL = 100,
    WIN_STREAK_GOAL = 10,
    PLUS_FOUR_GOAL = 50,
    UNO_CALLS_GOAL = 300,
    TEAM_WINS_GOAL = 50,
    MATCHES_GOAL = 500,
    DIAMOND_ELO = 1800
};

static uint32_t earnedBadges(const ProfileStats *stats) {
    uint32_t badges = 0;
    if (stats->wins >= FIRST_WIN_GOAL) badges |= BADGE_FIRST_WIN;
    if (stats->wins >= TOTAL_WINS_GOAL) badges |= BADGE_WIN_100;
    if (stats->bestStreak >= WIN_STREAK_GOAL) badges |= BADGE_STREAK_10;
    if (stats->plus4 >= PLUS_FOUR_GOAL) badges |= BADGE_PLUS4;
    if (stats->unoCalls >= UNO_CALLS_GOAL) badges |= BADGE_UNO_MASTER;
    if (stats->teamWins >= TEAM_WINS_GOAL) badges |= BADGE_TEAM;
    if (stats->matches >= MATCHES_GOAL) badges |= BADGE_MATCHES;
    if (stats->peakElo >= DIAMOND_ELO) badges |= BADGE_DIAMOND;
    return badges;
}

static uint32_t addCount(uint32_t base, uint32_t increment) {
    return increment > UINT32_MAX - base ? UINT32_MAX : base + increment;
}

Status profileCompute(Store *store, const MatchResult *result,
                      ProfileDelta out[PROFILE_SEAT_COUNT]) {
    if (!store || !result || !out || result->status == MATCH_ACTIVE) return BAD_REQUEST;
    ProfileDelta deltas[PROFILE_SEAT_COUNT] = {0};
    bool teamMode = result->mode == TWO_V_TWO || result->mode == SIDE;
    // Bot seats have no account; wins against bots must not farm streak or badges.
    bool vsBots = false;
    for (Seat seat = 0; seat < PROFILE_SEAT_COUNT; seat++) {
        if (!result->players[seat].userId) vsBots = true;
    }
    if (result->status == MATCH_FINALIZED) {
        for (Seat seat = 0; seat < PROFILE_SEAT_COUNT; seat++) {
            const PlayerResult *player = &result->players[seat];
            if (!player->userId) continue;
            ProfileStats before;
            Status status = store_profile_stats(store, player->userId, &before);
            if (status != OK) return status;

            ProfileStats after = before;
            after.matches = addCount(before.matches, 1);
            // streak/bestStreak/peakElo are written as absolute values, so a bot match keeps them as read.
            if (!vsBots) {
                bool won = !player->left && (teamMode ? player->team == result->winnerTeam :
                                                        seat == result->winnerSeat);
                after.wins = addCount(before.wins, won);
                after.teamWins = addCount(before.teamWins, won && teamMode);
                after.plus4 = addCount(before.plus4, player->stats.plus4);
                after.unoCalls = addCount(before.unoCalls, player->stats.unoCalls);
                if (won) {
                    after.streak = before.streak < 0 ? 1 :
                        before.streak == INT32_MAX ? INT32_MAX : before.streak + 1;
                    if ((uint32_t)after.streak > before.bestStreak) {
                        after.bestStreak = (uint32_t)after.streak;
                    }
                } else {
                    after.streak = before.streak > 0 ? -1 :
                        before.streak == INT32_MIN ? INT32_MIN : before.streak - 1;
                }
                if (player->eloAfter > before.peakElo) after.peakElo = player->eloAfter;
            }
            deltas[seat] = (ProfileDelta){
                .add = {
                    .matches = after.matches - before.matches,
                    .wins = after.wins - before.wins,
                    .teamWins = after.teamWins - before.teamWins,
                    .plus4 = after.plus4 - before.plus4,
                    .unoCalls = after.unoCalls - before.unoCalls
                },
                .streak = after.streak, .bestStreak = after.bestStreak, .peakElo = after.peakElo,
                .unlocked = earnedBadges(&after) & ~earnedBadges(&before)
            };
        }
    }
    memcpy(out, deltas, sizeof deltas);
    return OK;
}
