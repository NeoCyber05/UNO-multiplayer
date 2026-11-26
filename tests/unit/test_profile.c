#include "server/game/profile.h"
#include "server/store.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    Store *store = NULL;
    assert(storeOpen(&store, ":memory:") == OK);
    UserId userIds[PROFILE_SEAT_COUNT];
    for (Seat seat = 0; seat < PROFILE_SEAT_COUNT; seat++) {
        AccountCreate account = {.hash = "test", .name = "Player", .avatar = "boy"};
        snprintf(account.email, sizeof account.email, "uno%d@example.com", seat);
        assert(storeAccountCreate(store, &account, &userIds[seat]) == OK);
    }
    UserId userId = userIds[0];
    char sql[256];
    snprintf(sql, sizeof sql,
             "UPDATE profile_stats SET matches=499, wins=99, "
             "team_wins=49, plus4=49, uno_calls=299, streak=9, best_streak=9, "
             "peak_elo=1799 WHERE user_id=%llu",
             (unsigned long long)userId);
    assert(sqlite3_exec(storeGetDb(store), sql, NULL, NULL, NULL) == SQLITE_OK);

    MatchResult result = {
        .status = MATCH_FINALIZED, .mode = TWO_V_TWO, .winnerSeat = 2, .winnerTeam = 0};
    result.players[0] = (PlayerResult){
        .userId = userId, .team = 0, .eloAfter = 1800, .stats = {.plus4 = 1, .unoCalls = 1}};
    for (Seat seat = 1; seat < PROFILE_SEAT_COUNT; seat++) {
        result.players[seat] = (PlayerResult){.userId = userIds[seat], .team = seat % 2};
    }
    ProfileDelta deltas[PROFILE_SEAT_COUNT];
    assert(profileCompute(store, &result, deltas) == OK);
    assert(deltas[0].streak == 10 && deltas[0].bestStreak == 10 && deltas[0].peakElo == 1800);
    assert(deltas[0].add.matches == 1 && deltas[0].add.wins == 1 && deltas[0].add.teamWins == 1);
    assert(deltas[0].unlocked == (BADGE_WIN_100 | BADGE_STREAK_10 | BADGE_PLUS4 | BADGE_UNO_MASTER |
                                  BADGE_TEAM | BADGE_MATCHES | BADGE_DIAMOND));
    assert(deltas[1].add.matches == 1 && deltas[1].add.wins == 0 && deltas[1].streak == -1);
    ProfileStats after;
    assert(storeProfileStats(store, userId, &after) == OK && after.wins == 99);

    result.players[0].left = true;
    assert(profileCompute(store, &result, deltas) == OK);
    assert(deltas[0].streak == -1 && deltas[0].add.wins == 0);

    // The 500th match still earns the participation badge when a bot seat is present.
    result.players[0].left = false;
    result.players[3].userId = 0;
    assert(profileCompute(store, &result, deltas) == OK);
    assert(deltas[0].add.matches == 1 && deltas[0].add.wins == 0 && deltas[0].add.plus4 == 0);
    assert(deltas[0].streak == 9 && deltas[0].bestStreak == 9 && deltas[0].peakElo == 1799);
    assert(deltas[0].unlocked == BADGE_MATCHES);
    assert(deltas[3].add.matches == 0);

    result.status = MATCH_ABORTED;
    assert(profileCompute(store, &result, deltas) == OK && deltas[0].add.matches == 0);
    storeClose(store);
}
