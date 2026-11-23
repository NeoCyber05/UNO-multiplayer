#include "server/profile.h"
#include "server/store.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    Store *store = NULL;
    assert(store_open(&store, ":memory:") == OK);
    AccountCreate account = {
        .email = "uno@example.com", .hash = "test", .name = "Player", .avatar = "boy"
    };
    UserId userId;
    assert(store_account_create(store, &account, &userId) == OK);
    char sql[256];
    snprintf(sql, sizeof sql, "UPDATE profile_stats SET matches=499, wins=99, "
             "team_wins=49, plus4=49, uno_calls=299, streak=9, best_streak=9, "
             "peak_elo=1799 WHERE user_id=%llu", (unsigned long long)userId);
    assert(sqlite3_exec(store_get_db(store), sql, NULL, NULL, NULL) == SQLITE_OK);

    MatchResult result = {
        .status = MATCH_FINALIZED, .mode = TWO_V_TWO, .winnerSeat = 2, .winnerTeam = 0
    };
    result.players[0] = (PlayerResult){
        .userId = userId, .team = 0, .eloAfter = 1800, .stats = {.plus4 = 1, .unoCalls = 1}
    };
    ProfileDelta deltas[4];
    assert(profileCompute(store, &result, deltas) == OK);
    assert(deltas[0].streak == 10 && deltas[0].bestStreak == 10 && deltas[0].peakElo == 1800);
    assert(deltas[0].add.matches == 1 && deltas[0].add.wins == 1 && deltas[0].add.teamWins == 1);
    assert(deltas[0].unlocked == (BADGE_WIN_100 | BADGE_STREAK_10 | BADGE_PLUS4 |
           BADGE_UNO_MASTER | BADGE_TEAM | BADGE_MATCHES | BADGE_DIAMOND));
    assert(deltas[1].add.matches == 0);
    ProfileStats after;
    assert(store_profile_stats(store, userId, &after) == OK && after.wins == 99);

    result.players[0].left = true;
    assert(profileCompute(store, &result, deltas) == OK);
    assert(deltas[0].streak == -1 && deltas[0].add.wins == 0);
    result.status = MATCH_ABORTED;
    assert(profileCompute(store, &result, deltas) == OK && deltas[0].add.matches == 0);
    store_close(store);
}
