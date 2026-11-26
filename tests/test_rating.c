#include "server/rating.h"

#include <assert.h>
#include <string.h>

static MatchResult makeResult(Mode mode, const int elos[4], const int teams[4]) {
    MatchResult result = {
        .mode = mode, .ranked = true, .status = MATCH_FINALIZED,
        .winnerSeat = -1, .winnerTeam = -1
    };
    for (int seat = 0; seat < 4; seat++) {
        result.players[seat].userId = (UserId)(seat + 1);
        result.players[seat].team = teams[seat];
        result.players[seat].eloBefore = elos[seat];
    }
    return result;
}

static void testDeltaAndRanks(void) {
    assert(ratingDelta(1500, 1500, true) == 16);
    assert(ratingDelta(1500, 1500, false) == -16);
    /* Higher-rated winner gains less than an even match. */
    assert(ratingDelta(1700, 1500, true) == 8);
    assert(ratingDelta(1500, 1700, true) == 24);
    assert(ratingRank(1199) == RANK_BRONZE && ratingRank(1200) == RANK_SILVER);
    assert(ratingRank(1399) == RANK_SILVER && ratingRank(1400) == RANK_GOLD);
    assert(ratingRank(1799) == RANK_GOLD && ratingRank(1800) == RANK_DIAMOND);
    assert(ratingRank(0) == RANK_BRONZE);
    assert(strcmp(ratingRankKey(RANK_BRONZE), "bronze") == 0);
    assert(strcmp(ratingRankKey(RANK_SILVER), "silver") == 0);
    assert(strcmp(ratingRankKey(RANK_GOLD), "gold") == 0);
    assert(strcmp(ratingRankKey(RANK_DIAMOND), "diamond") == 0);
}

static void testSolo(void) {
    const int elos[4] = {1500, 1500, 1500, 1500};
    const int teams[4] = {0, 1, 2, 3};
    const bool noBots[4] = {false, false, false, false};
    MatchResult result = makeResult(CLASSIC, elos, teams);
    result.winnerSeat = 2;
    ratingApply(&result, noBots);
    assert(result.players[0].eloAfter == 1484);
    assert(result.players[1].eloAfter == 1484);
    assert(result.players[2].eloAfter == 1516);
    assert(result.players[3].eloAfter == 1484);

    MatchResult draft = makeResult(DRAFT, elos, teams);
    draft.winnerSeat = 0;
    ratingApply(&draft, NULL);
    assert(draft.players[0].eloAfter == 1516 && draft.players[3].eloAfter == 1484);
}

static void testTeam(void) {
    const int elos[4] = {1600, 1500, 1400, 1500};
    const int teams[4] = {0, 1, 0, 1};
    const bool noBots[4] = {false, false, false, false};
    MatchResult result = makeResult(TWO_V_TWO, elos, teams);
    result.winnerTeam = 0;
    ratingApply(&result, noBots);
    assert(result.players[0].eloAfter == 1616);
    assert(result.players[2].eloAfter == 1416);
    assert(result.players[1].eloAfter == 1484);
    assert(result.players[3].eloAfter == 1484);

    MatchResult side = makeResult(SIDE, elos, teams);
    side.winnerTeam = 1;
    ratingApply(&side, noBots);
    assert(side.players[0].eloAfter == 1584 && side.players[1].eloAfter == 1516);
}

static void testClampBotsAndUnchanged(void) {
    /* Equal opposition makes the loss exactly -16, which would go below zero. */
    const int lowElos[4] = {5, 5, 5, 5};
    const int teams[4] = {0, 1, 2, 3};
    const bool noBots[4] = {false, false, false, false};
    MatchResult result = makeResult(CLASSIC, lowElos, teams);
    result.winnerSeat = 1;
    ratingApply(&result, noBots);
    assert(result.players[0].eloAfter == 0);
    assert(result.players[1].eloAfter == 21);

    const int elos[4] = {1500, 1500, 1500, 1500};
    const bool botSeat3[4] = {false, false, false, true};
    MatchResult withBot = makeResult(CLASSIC, elos, teams);
    withBot.winnerSeat = 1;
    ratingApply(&withBot, botSeat3);
    assert(withBot.players[3].eloAfter == 1500);

    MatchResult unranked = makeResult(CLASSIC, elos, teams);
    unranked.ranked = false;
    unranked.winnerSeat = 1;
    ratingApply(&unranked, noBots);
    for (int seat = 0; seat < 4; seat++) {
        assert(unranked.players[seat].eloAfter == unranked.players[seat].eloBefore);
    }

    MatchResult aborted = makeResult(TWO_V_TWO, elos, teams);
    aborted.status = MATCH_ABORTED;
    ratingApply(&aborted, noBots);
    for (int seat = 0; seat < 4; seat++) {
        assert(aborted.players[seat].eloAfter == aborted.players[seat].eloBefore);
    }
}

int main(void) {
    testDeltaAndRanks();
    testSolo();
    testTeam();
    testClampBotsAndUnchanged();
    return 0;
}
