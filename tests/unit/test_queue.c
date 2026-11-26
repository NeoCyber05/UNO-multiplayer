#include "server/lobby/queue.h"

#include <assert.h>
#include <string.h>

enum { SEATS = 4 };

static QueueEntry solo(UserId user) {
    QueueEntry entry = {{user, 0}, 1};
    return entry;
}

static QueueEntry party(UserId first, UserId second) {
    QueueEntry entry = {{first, second}, 2};
    return entry;
}

static void addSolo(Queue *queue, Mode mode, UserId user) {
    QueueEntry entry = solo(user);
    assert(queueAdd(queue, mode, &entry) == OK);
}

static void addParty(Queue *queue, Mode mode, UserId first, UserId second) {
    QueueEntry entry = party(first, second);
    assert(queueAdd(queue, mode, &entry) == OK);
}

static void testClassicFifo(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);
    UserId seats[SEATS];
    size_t indexes[SEATS];
    size_t entryCount = 0;

    addSolo(queue, CLASSIC, 11);
    addSolo(queue, CLASSIC, 12);
    addSolo(queue, CLASSIC, 13);
    assert(!queuePick(queue, CLASSIC, seats, indexes, &entryCount));

    addSolo(queue, CLASSIC, 14);
    assert(queuePick(queue, CLASSIC, seats, indexes, &entryCount));
    assert(entryCount == 4);
    assert(seats[0] == 11 && seats[1] == 12 && seats[2] == 13 && seats[3] == 14);

    queueTake(queue, CLASSIC, indexes, entryCount);
    assert(!queueContains(queue, 11) && !queueContains(queue, 14));
    assert(!queuePick(queue, CLASSIC, seats, indexes, &entryCount));
    queueDestroy(queue);
}

static void testModesDoNotMix(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);
    UserId seats[SEATS];
    size_t indexes[SEATS];
    size_t entryCount = 0;

    addSolo(queue, CLASSIC, 1);
    addSolo(queue, CLASSIC, 2);
    addSolo(queue, DRAFT, 3);
    addSolo(queue, DRAFT, 4);
    assert(!queuePick(queue, CLASSIC, seats, indexes, &entryCount));
    assert(!queuePick(queue, DRAFT, seats, indexes, &entryCount));
    queueDestroy(queue);
}

static void testTwoVTwoPartyTeam(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);
    UserId seats[SEATS];
    size_t indexes[SEATS];
    size_t entryCount = 0;

    addParty(queue, TWO_V_TWO, 21, 22);
    addSolo(queue, TWO_V_TWO, 23);
    assert(!queuePick(queue, TWO_V_TWO, seats, indexes, &entryCount));
    addSolo(queue, TWO_V_TWO, 24);
    assert(queuePick(queue, TWO_V_TWO, seats, indexes, &entryCount));
    assert(entryCount == 3);
    /* 2v2 teams are seats {0,2} and {1,3}; the party must share a team. */
    assert(seats[0] == 21 && seats[2] == 22);
    assert(seats[1] == 23 && seats[3] == 24);
    queueDestroy(queue);
}

static void testSoloBeforePartyStillTeamsParty(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);
    UserId seats[SEATS];
    size_t indexes[SEATS];
    size_t entryCount = 0;

    addSolo(queue, TWO_V_TWO, 31);
    addParty(queue, TWO_V_TWO, 32, 33);
    addSolo(queue, TWO_V_TWO, 34);
    assert(queuePick(queue, TWO_V_TWO, seats, indexes, &entryCount));
    assert(seats[0] == 32 && seats[2] == 33);
    assert(seats[1] == 31 && seats[3] == 34);
    queueDestroy(queue);
}

static void testSideTwoParties(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);
    UserId seats[SEATS];
    size_t indexes[SEATS];
    size_t entryCount = 0;

    addParty(queue, SIDE, 41, 42);
    addParty(queue, SIDE, 43, 44);
    assert(queuePick(queue, SIDE, seats, indexes, &entryCount));
    assert(entryCount == 2);
    /* Side teams are seats {0,1} and {2,3}. */
    assert(seats[0] == 41 && seats[1] == 42);
    assert(seats[2] == 43 && seats[3] == 44);
    queueDestroy(queue);
}

static void testSkipsOversizedEntry(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);
    UserId seats[SEATS];
    size_t indexes[SEATS];
    size_t entryCount = 0;

    addSolo(queue, SIDE, 51);
    addParty(queue, SIDE, 52, 53);
    addParty(queue, SIDE, 54, 55);
    addSolo(queue, SIDE, 56);
    assert(queuePick(queue, SIDE, seats, indexes, &entryCount));
    /* The second party would make five players, so the later solo is taken. */
    assert(entryCount == 3);
    assert(indexes[0] == 0 && indexes[1] == 1 && indexes[2] == 3);
    assert(seats[0] == 52 && seats[1] == 53);
    assert(seats[2] == 51 && seats[3] == 56);

    queueTake(queue, SIDE, indexes, entryCount);
    assert(queueContains(queue, 54) && queueContains(queue, 55));
    assert(!queueContains(queue, 51) && !queueContains(queue, 56));
    queueDestroy(queue);
}

/* Expects the earliest valid set by FIFO index: picked indexes and seat order. */
static void assertPick(const Queue *queue, Mode mode, const size_t expectedIdx[],
                       size_t expectedCount, const UserId expectedSeats[SEATS]) {
    UserId seats[SEATS];
    size_t indexes[SEATS];
    size_t entryCount = 0;
    assert(queuePick(queue, mode, seats, indexes, &entryCount));
    assert(entryCount == expectedCount);
    for (size_t index = 0; index < expectedCount; index++) {
        assert(indexes[index] == expectedIdx[index]);
    }
    for (size_t seat = 0; seat < SEATS; seat++) {
        assert(seats[seat] == expectedSeats[seat]);
    }
}

static void testPartySoloPartyMatchesBothParties(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);
    addParty(queue, TWO_V_TWO, 81, 82);
    addSolo(queue, TWO_V_TWO, 83);
    addParty(queue, TWO_V_TWO, 84, 85);
    const size_t expectedIdx[] = {0, 2};
    const UserId expectedSeats[SEATS] = {81, 84, 82, 85};
    assertPick(queue, TWO_V_TWO, expectedIdx, 2, expectedSeats);
    queueDestroy(queue);
}

static void testLoneSoloDoesNotStallParties(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);
    addSolo(queue, SIDE, 91);
    addParty(queue, SIDE, 92, 93);
    addParty(queue, SIDE, 94, 95);
    addParty(queue, SIDE, 96, 97);
    const size_t expectedIdx[] = {1, 2};
    const UserId expectedSeats[SEATS] = {92, 93, 94, 95};
    assertPick(queue, SIDE, expectedIdx, 2, expectedSeats);
    queueDestroy(queue);
}

static void testOldestSoloKeptWhenPossible(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);
    addSolo(queue, TWO_V_TWO, 101);
    addParty(queue, TWO_V_TWO, 102, 103);
    addSolo(queue, TWO_V_TWO, 104);
    const size_t spsIdx[] = {0, 1, 2};
    const UserId spsSeats[SEATS] = {102, 101, 103, 104};
    assertPick(queue, TWO_V_TWO, spsIdx, 3, spsSeats);
    queueDestroy(queue);

    assert(queueInit(&queue) == OK);
    addSolo(queue, SIDE, 111);
    addSolo(queue, SIDE, 112);
    addParty(queue, SIDE, 113, 114);
    addSolo(queue, SIDE, 115);
    const size_t sspsIdx[] = {0, 1, 2};
    const UserId sspsSeats[SEATS] = {113, 114, 111, 112};
    assertPick(queue, SIDE, sspsIdx, 3, sspsSeats);
    queueDestroy(queue);
}

static void testValidation(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);

    QueueEntry classicParty = party(61, 62);
    assert(queueAdd(queue, CLASSIC, &classicParty) == INVALID_ACTION);
    assert(queueAdd(queue, DRAFT, &classicParty) == INVALID_ACTION);
    assert(!queueContains(queue, 61));

    addSolo(queue, CLASSIC, 63);
    QueueEntry duplicate = solo(63);
    assert(queueAdd(queue, CLASSIC, &duplicate) == BUSY);
    assert(queueAdd(queue, DRAFT, &duplicate) == BUSY);
    QueueEntry overlapping = party(64, 63);
    assert(queueAdd(queue, TWO_V_TWO, &overlapping) == BUSY);
    assert(!queueContains(queue, 64));

    QueueEntry bot = solo(0);
    assert(queueAdd(queue, CLASSIC, &bot) == BAD_REQUEST);
    QueueEntry self = party(65, 65);
    assert(queueAdd(queue, SIDE, &self) == BAD_REQUEST);
    queueDestroy(queue);
}

static void testRemoveDropsWholeParty(void) {
    Queue *queue = NULL;
    assert(queueInit(&queue) == OK);

    addSolo(queue, TWO_V_TWO, 70);
    addParty(queue, TWO_V_TWO, 71, 72);
    Mode mode = CLASSIC;
    QueueEntry removed;
    memset(&removed, 0, sizeof removed);
    assert(queueRemove(queue, 72, &mode, &removed));
    assert(mode == TWO_V_TWO);
    assert(removed.count == 2 && removed.users[0] == 71 && removed.users[1] == 72);
    assert(!queueContains(queue, 71) && !queueContains(queue, 72));
    assert(queueContains(queue, 70));
    assert(!queueRemove(queue, 72, &mode, &removed));
    queueDestroy(queue);
}

int main(void) {
    testClassicFifo();
    testModesDoNotMix();
    testTwoVTwoPartyTeam();
    testSoloBeforePartyStillTeamsParty();
    testSideTwoParties();
    testSkipsOversizedEntry();
    testPartySoloPartyMatchesBothParties();
    testLoneSoloDoesNotStallParties();
    testOldestSoloKeptWhenPossible();
    testValidation();
    testRemoveDropsWholeParty();
    return 0;
}
