#define _POSIX_C_SOURCE 200809L
#include "server/store.h"
#include "shared/contracts.h"
#include <assert.h>
#include <json-c/json.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void cleanupFile(const char *path) {
    unlink(path);
    char shm[512], wal[512];
    snprintf(shm, sizeof(shm), "%s-shm", path);
    snprintf(wal, sizeof(wal), "%s-wal", path);
    unlink(shm);
    unlink(wal);
}

static void testTableCreationAndMigration(void) {
    const char *dbPath = "/tmp/test_uno_store_migration.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    Status status = store_open(&store, dbPath);
    assert(status == OK);
    assert(store != NULL);

    sqlite3 *db = store_get_db(store);
    assert(db != NULL);

    const char *tables[] = {
        "matches", "match_players", "match_events",
        "accounts", "friends", "achievements", "profile_stats"
    };
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        char query[256];
        snprintf(query, sizeof(query),
                 "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%s';",
                 tables[i]);
        sqlite3_stmt *stmt = NULL;
        assert(sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        assert(count == 1);
    }

    /* Verify PRAGMA journal_mode is WAL */
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const unsigned char *jmode = sqlite3_column_text(stmt, 0);
    assert(jmode != NULL && strcasecmp((const char *)jmode, "wal") == 0);
    sqlite3_finalize(stmt);

    /* Verify PRAGMA foreign_keys is ON */
    assert(sqlite3_prepare_v2(db, "PRAGMA foreign_keys;", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int fk = sqlite3_column_int(stmt, 0);
    assert(fk == 1);
    sqlite3_finalize(stmt);

    store_close(store);
    cleanupFile(dbPath);
}

static void testStoreMatchStartPersisting(void) {
    const char *dbPath = "/tmp/test_uno_store_start.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);

    MatchId mid = "00000000-0000-0000-0000-000000000001";
    PlayerSpec players[4] = {
        {101, "Alice", "avatar1", EASY, false},
        {102, "Bob", "avatar2", EASY, false},
        {0, "BotCharlie", "avatar_bot", EASY, true},
        {0, "BotDavid", "avatar_bot", HARD, true}
    };

    Status status = store_match_start(store, (const MatchId *)&mid, players, CLASSIC, true);
    assert(status == OK);

    /* Duplicate call with identical parameters should be idempotent OK */
    status = store_match_start(store, (const MatchId *)&mid, players, CLASSIC, true);
    assert(status == OK);

    /* Duplicate call with conflicting mode should return INVALID_ACTION */
    status = store_match_start(store, (const MatchId *)&mid, players, TWO_V_TWO, true);
    assert(status == INVALID_ACTION);

    /* Verify persisted data via SQLite directly */
    sqlite3 *db = store_get_db(store);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT mode, ranked, status FROM matches WHERE match_id = ?;",
                              -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == CLASSIC);
    assert(sqlite3_column_int(stmt, 1) == 1);
    assert(sqlite3_column_int(stmt, 2) == MATCH_ACTIVE);
    sqlite3_finalize(stmt);

    const char *seatsSql =
        "SELECT seat, user_id, name, is_bot, bot_level FROM match_players WHERE match_id = ? ORDER BY seat ASC;";
    assert(sqlite3_prepare_v2(db, seatsSql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);

    for (int i = 0; i < 4; i++) {
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        assert(sqlite3_column_int(stmt, 0) == i);
        assert((UserId)sqlite3_column_int64(stmt, 1) == players[i].userId);
        assert(strcmp((const char *)sqlite3_column_text(stmt, 2), players[i].name) == 0);
        assert(sqlite3_column_int(stmt, 3) == (players[i].isBot ? 1 : 0));
        assert(sqlite3_column_int(stmt, 4) == (int)players[i].botLevel);
    }
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    store_close(store);
    cleanupFile(dbPath);
}

static void testStoreEventsUniquenessAndIdempotency(void) {
    const char *dbPath = "/tmp/test_uno_store_events.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);

    MatchId mid = "00000000-0000-0000-0000-000000000002";
    PlayerSpec players[4] = {
        {101, "Alice", "a1", EASY, false},
        {102, "Bob", "a2", EASY, false},
        {103, "Charlie", "a3", EASY, false},
        {104, "David", "a4", EASY, false}
    };
    assert(store_match_start(store, (const MatchId *)&mid, players, CLASSIC, false) == OK);

    EventBatch batch1;
    memset(&batch1, 0, sizeof(batch1));
    batch1.count = 2;
    batch1.events[0].seq = 1;
    batch1.events[0].revision = 1;
    batch1.events[0].actor = 0;
    strncpy(batch1.events[0].type, "CARD_PLAYED", sizeof(batch1.events[0].type) - 1);
    strncpy(batch1.events[0].payload, "{\"card\":\"R5\"}", sizeof(batch1.events[0].payload) - 1);

    batch1.events[1].seq = 2;
    batch1.events[1].revision = 2;
    batch1.events[1].actor = 1;
    strncpy(batch1.events[1].type, "DRAW", sizeof(batch1.events[1].type) - 1);
    strncpy(batch1.events[1].payload, "{\"count\":1}", sizeof(batch1.events[1].payload) - 1);

    /* Insert batch 1 */
    Status status = store_events(store, (const MatchId *)&mid, &batch1);
    assert(status == OK);

    /* Duplicate identical batch -> idempotent OK */
    status = store_events(store, (const MatchId *)&mid, &batch1);
    assert(status == OK);

    /* Duplicate identical single event -> idempotent OK */
    EventBatch batchDup;
    memset(&batchDup, 0, sizeof(batchDup));
    batchDup.count = 1;
    batchDup.events[0] = batch1.events[0];
    status = store_events(store, (const MatchId *)&mid, &batchDup);
    assert(status == OK);

    /* Conflicting sequence: same seq=2, but different actor/type/payload -> INVALID_ACTION */
    EventBatch batchConflict;
    memset(&batchConflict, 0, sizeof(batchConflict));
    batchConflict.count = 1;
    batchConflict.events[0].seq = 2;
    batchConflict.events[0].revision = 2;
    batchConflict.events[0].actor = 2; /* changed actor */
    strncpy(batchConflict.events[0].type, "PASS", sizeof(batchConflict.events[0].type) - 1);
    strncpy(batchConflict.events[0].payload, "{}", sizeof(batchConflict.events[0].payload) - 1);

    status = store_events(store, (const MatchId *)&mid, &batchConflict);
    assert(status == INVALID_ACTION);

    /* Conflicting payload on same seq=1 -> INVALID_ACTION */
    batchConflict.events[0].seq = 1;
    batchConflict.events[0].revision = 1;
    batchConflict.events[0].actor = 0;
    strncpy(batchConflict.events[0].type, "CARD_PLAYED", sizeof(batchConflict.events[0].type) - 1);
    strncpy(batchConflict.events[0].payload, "{\"card\":\"G5\"}",
            sizeof(batchConflict.events[0].payload) - 1);

    status = store_events(store, (const MatchId *)&mid, &batchConflict);
    assert(status == INVALID_ACTION);

    /* Append next event seq=3 -> OK */
    EventBatch batch2;
    memset(&batch2, 0, sizeof(batch2));
    batch2.count = 1;
    batch2.events[0].seq = 3;
    batch2.events[0].revision = 3;
    batch2.events[0].actor = 2;
    strncpy(batch2.events[0].type, "CARD_PLAYED", sizeof(batch2.events[0].type) - 1);
    strncpy(batch2.events[0].payload, "{\"card\":\"R7\"}", sizeof(batch2.events[0].payload) - 1);

    status = store_events(store, (const MatchId *)&mid, &batch2);
    assert(status == OK);

    /* Verify count in DB is exactly 3 */
    sqlite3 *db = store_get_db(store);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM match_events WHERE match_id = ?;", -1,
                              &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 3);
    sqlite3_finalize(stmt);

    /* Calling store_events with unknown match_id should return NOT_FOUND */
    MatchId fakeMatchId = "99999999-9999-9999-9999-999999999999";
    status = store_events(store, (const MatchId *)&fakeMatchId, &batch2);
    assert(status == NOT_FOUND);

    store_close(store);
    cleanupFile(dbPath);
}

static void testStoreFinalizeIdempotencyAndNoDoubleElo(void) {
    const char *dbPath = "/tmp/test_uno_store_finalize.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);

    /* Seed test accounts */
    sqlite3 *db = store_get_db(store);
    assert(sqlite3_exec(db,
        "INSERT INTO accounts (id, email, password_hash, name, avatar, elo, created_at) VALUES "
        "(101, 'alice@test.com', 'h1', 'Alice', 'a1', 1500, 1000),"
        "(102, 'bob@test.com', 'h2', 'Bob', 'a2', 1500, 1000);"
        "INSERT INTO profile_stats (user_id, peak_elo) VALUES (101, 1500), (102, 1500);",
        NULL, NULL, NULL) == SQLITE_OK);

    MatchId mid = "00000000-0000-0000-0000-000000000003";
    PlayerSpec players[4] = {
        {101, "Alice", "a1", EASY, false},
        {102, "Bob", "a2", EASY, false},
        {0, "Bot1", "bot", EASY, true},
        {0, "Bot2", "bot", EASY, true}
    };
    assert(store_match_start(store, (const MatchId *)&mid, players, CLASSIC, true) == OK);

    MatchResult res;
    memset(&res, 0, sizeof(res));
    strncpy(res.matchId, mid, sizeof(res.matchId) - 1);
    res.mode = CLASSIC;
    res.ranked = true;
    res.status = MATCH_FINALIZED;
    res.winnerSeat = 0;
    res.winnerTeam = -1;
    res.durationMs = 60000;

    res.players[0].userId = 101;
    res.players[0].team = -1;
    res.players[0].points = 100;
    res.players[0].eloBefore = 1500;
    res.players[0].eloAfter = 1516;
    res.players[0].left = false;
    res.players[0].stats.played = 10;
    res.players[0].stats.attacks = 2;
    res.players[0].stats.unoCalls = 1;
    res.players[0].stats.penalties = 0;
    res.players[0].stats.plus4 = 1;

    res.players[1].userId = 102;
    res.players[1].team = -1;
    res.players[1].points = 40;
    res.players[1].eloBefore = 1500;
    res.players[1].eloAfter = 1484;
    res.players[1].left = false;
    res.players[1].stats.played = 8;
    res.players[1].stats.attacks = 1;
    res.players[1].stats.unoCalls = 0;
    res.players[1].stats.penalties = 1;
    res.players[1].stats.plus4 = 0;

    res.players[2].userId = 0;
    res.players[2].team = -1;
    res.players[2].points = 10;
    res.players[2].eloBefore = 1500;
    res.players[2].eloAfter = 1500;

    res.players[3].userId = 0;
    res.players[3].team = -1;
    res.players[3].points = 0;
    res.players[3].eloBefore = 1500;
    res.players[3].eloAfter = 1500;

    ProfileDelta deltas[4];
    memset(deltas, 0, sizeof(deltas));
    deltas[0].add.matches = 1;
    deltas[0].add.wins = 1;
    deltas[0].add.plus4 = 1;
    deltas[0].add.unoCalls = 1;
    deltas[0].streak = 1;
    deltas[0].bestStreak = 1;
    deltas[0].peakElo = 1516;
    deltas[0].unlocked = BADGE_FIRST_WIN;

    deltas[1].add.matches = 1;
    deltas[1].add.wins = 0;
    deltas[1].peakElo = 1500;

    /* First finalize -> should succeed */
    Status status = store_finalize(store, &res, deltas);
    assert(status == OK);

    /* Check ELO and stats after first finalize */
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 101;", -1, &stmt,
                              NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int elo101 = sqlite3_column_int(stmt, 0);
    assert(elo101 == 1516);
    sqlite3_finalize(stmt);

    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 102;", -1, &stmt,
                              NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int elo102 = sqlite3_column_int(stmt, 0);
    assert(elo102 == 1484);
    sqlite3_finalize(stmt);

    /* Second finalize with identical result -> idempotent OK, no doubling! */
    status = store_finalize(store, &res, deltas);
    assert(status == OK);

    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 101;", -1, &stmt,
                              NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    elo101 = sqlite3_column_int(stmt, 0);
    assert(elo101 == 1516); /* NOT doubled to 1532! */
    sqlite3_finalize(stmt);

    /* Second finalize with conflicting result -> INVALID_ACTION */
    MatchResult conflictingResult = res;
    conflictingResult.winnerSeat = 1; /* different winner */
    status = store_finalize(store, &conflictingResult, deltas);
    assert(status == INVALID_ACTION);

    /* Check that ELO is still 1516 after conflicting finalize attempt */
    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 101;", -1, &stmt,
                              NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    elo101 = sqlite3_column_int(stmt, 0);
    assert(elo101 == 1516);
    sqlite3_finalize(stmt);

    store_close(store);
    cleanupFile(dbPath);
}

static void testCrashRecoveryAborted(void) {
    const char *dbPath = "/tmp/test_uno_store_crash.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);

    MatchId matchIdActive = "00000000-0000-0000-0000-000000000004";
    MatchId matchIdFinal = "00000000-0000-0000-0000-000000000005";

    PlayerSpec players[4] = {
        {101, "Alice", "a1", EASY, false},
        {102, "Bob", "a2", EASY, false},
        {0, "Bot1", "bot", EASY, true},
        {0, "Bot2", "bot", EASY, true}
    };

    assert(store_match_start(store, (const MatchId *)&matchIdActive, players, CLASSIC,
                             false) == OK);
    assert(store_match_start(store, (const MatchId *)&matchIdFinal, players, CLASSIC, false) == OK);

    MatchResult res;
    memset(&res, 0, sizeof(res));
    strncpy(res.matchId, matchIdFinal, sizeof(res.matchId) - 1);
    res.mode = CLASSIC;
    res.ranked = false;
    res.status = MATCH_FINALIZED;
    res.winnerSeat = 0;
    res.winnerTeam = -1;
    res.durationMs = 30000;
    for (int i = 0; i < 4; i++) {
        res.players[i].userId = players[i].userId;
        res.players[i].eloBefore = 1500;
        res.players[i].eloAfter = 1500;
    }
    assert(store_finalize(store, &res, NULL) == OK);

    /* Close the store abruptly */
    store_close(store);
    store = NULL;

    /* Reopen the store -> crash recovery should mark active matches as aborted */
    assert(store_open(&store, dbPath) == OK);

    sqlite3 *db = store_get_db(store);
    sqlite3_stmt *stmt = NULL;

    /* matchIdActive should now be MATCH_ABORTED */
    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches WHERE match_id = ?;", -1, &stmt,
                              NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, matchIdActive, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int statusActive = sqlite3_column_int(stmt, 0);
    assert(statusActive == MATCH_ABORTED);
    sqlite3_finalize(stmt);

    /* matchIdFinal should still be MATCH_FINALIZED */
    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches WHERE match_id = ?;", -1, &stmt,
                              NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, matchIdFinal, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int statusFinal = sqlite3_column_int(stmt, 0);
    assert(statusFinal == MATCH_FINALIZED);
    sqlite3_finalize(stmt);

    store_close(store);
    cleanupFile(dbPath);
}

static void testTransactionRollbackOnFailure(void) {
    const char *dbPath = "/tmp/test_uno_store_rollback.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);

    sqlite3 *db = store_get_db(store);
    assert(sqlite3_exec(db,
        "INSERT INTO accounts (id, email, password_hash, name, avatar, elo, created_at) VALUES "
        "(101, 'alice@test.com', 'h1', 'Alice', 'a1', 1500, 1000);",
        NULL, NULL, NULL) == SQLITE_OK);

    MatchId mid = "00000000-0000-0000-0000-000000000006";
    PlayerSpec players[4] = {
        {101, "Alice", "a1", EASY, false},
        {0, "Bot1", "bot", EASY, true},
        {0, "Bot2", "bot", EASY, true},
        {0, "Bot3", "bot", EASY, true}
    };
    assert(store_match_start(store, (const MatchId *)&mid, players, CLASSIC, true) == OK);

    /* Inject SQL abort trigger to fail finalize midway (e.g. on updating match_players) */
    assert(sqlite3_exec(db,
        "CREATE TRIGGER test_fail_trigger BEFORE UPDATE ON match_players "
        "BEGIN SELECT RAISE(ABORT, 'forced test failure'); END;",
        NULL, NULL, NULL) == SQLITE_OK);

    MatchResult res;
    memset(&res, 0, sizeof(res));
    strncpy(res.matchId, mid, sizeof(res.matchId) - 1);
    res.mode = CLASSIC;
    res.ranked = true;
    res.status = MATCH_FINALIZED;
    res.winnerSeat = 0;
    res.winnerTeam = -1;
    res.durationMs = 40000;
    res.players[0].userId = 101;
    res.players[0].eloBefore = 1500;
    res.players[0].eloAfter = 1530;

    ProfileDelta deltas[4];
    memset(deltas, 0, sizeof(deltas));
    deltas[0].add.matches = 1;
    deltas[0].add.wins = 1;

    /* store_finalize should encounter error and rollback entire transaction */
    Status status = store_finalize(store, &res, deltas);
    assert(status == STORAGE_ERROR);

    /* Verify match status is STILL MATCH_ACTIVE */
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches WHERE match_id = ?;", -1, &stmt,
                              NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == MATCH_ACTIVE);
    sqlite3_finalize(stmt);

    /* Verify accounts ELO was NOT updated */
    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 101;", -1, &stmt,
                              NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1500);
    sqlite3_finalize(stmt);

    /* Drop the abort trigger */
    assert(sqlite3_exec(db, "DROP TRIGGER test_fail_trigger;", NULL, NULL, NULL) == SQLITE_OK);

    /* Now finalize should succeed cleanly */
    status = store_finalize(store, &res, deltas);
    assert(status == OK);

    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches WHERE match_id = ?;", -1, &stmt,
                              NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == MATCH_FINALIZED);
    sqlite3_finalize(stmt);

    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 101;", -1, &stmt,
                              NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1530);
    sqlite3_finalize(stmt);

    store_close(store);
    cleanupFile(dbPath);
}

static void testStoreExportMatch(void) {
    const char *dbPath = "/tmp/test_uno_store_export.db";
    const char *exportPath = "/tmp/test_uno_match_export.json";
    cleanupFile(dbPath);
    unlink(exportPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);

    MatchId mid = "00000000-0000-0000-0000-000000000007";
    PlayerSpec players[4] = {
        {101, "Alice", "avatar1", EASY, false},
        {102, "Bob", "avatar2", EASY, false},
        {0, "Bot1", "avatar_bot", EASY, true},
        {0, "Bot2", "avatar_bot", HARD, true}
    };
    assert(store_match_start(store, (const MatchId *)&mid, players, CLASSIC, true) == OK);

    EventBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.count = 2;
    batch.events[0].seq = 1;
    batch.events[0].revision = 1;
    batch.events[0].actor = 0;
    strncpy(batch.events[0].type, "CARD_PLAYED", sizeof(batch.events[0].type) - 1);
    strncpy(batch.events[0].payload, "{\"card\":\"R5\"}", sizeof(batch.events[0].payload) - 1);

    batch.events[1].seq = 2;
    batch.events[1].revision = 2;
    batch.events[1].actor = 1;
    strncpy(batch.events[1].type, "DRAW", sizeof(batch.events[1].type) - 1);
    strncpy(batch.events[1].payload, "{\"count\":1}", sizeof(batch.events[1].payload) - 1);
    assert(store_events(store, (const MatchId *)&mid, &batch) == OK);

    MatchResult res;
    memset(&res, 0, sizeof(res));
    strncpy(res.matchId, mid, sizeof(res.matchId) - 1);
    res.mode = CLASSIC;
    res.ranked = true;
    res.status = MATCH_FINALIZED;
    res.winnerSeat = 0;
    res.winnerTeam = -1;
    res.durationMs = 120000;
    for (int i = 0; i < 4; i++) {
        res.players[i].userId = players[i].userId;
        res.players[i].points = (i == 0) ? 50 : 10;
        res.players[i].eloBefore = 1500;
        res.players[i].eloAfter = (i == 0) ? 1516 : 1484;
    }
    assert(store_finalize(store, &res, NULL) == OK);

    /* Export match */
    Status status = store_export_match(store, (const MatchId *)&mid, exportPath);
    assert(status == OK);

    /* Read exported JSON file */
    FILE *f = fopen(exportPath, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    assert(len > 0);
    fseek(f, 0, SEEK_SET);
    char *content = malloc(len + 1);
    assert(content != NULL);
    assert(fread(content, 1, len, f) == (size_t)len);
    content[len] = '\0';
    fclose(f);

    /* Ensure no password or token leaked */
    assert(strstr(content, "password") == NULL);
    assert(strstr(content, "token") == NULL);
    assert(strstr(content, "hash") == NULL);

    /* Parse JSON and verify fields */
    struct json_object *root = json_tokener_parse(content);
    assert(root != NULL);

    struct json_object *val = NULL;
    assert(json_object_object_get_ex(root, "matchId", &val));
    assert(strcmp(json_object_get_string(val), mid) == 0);

    assert(json_object_object_get_ex(root, "mode", &val));
    assert(json_object_get_int(val) == CLASSIC);

    assert(json_object_object_get_ex(root, "ranked", &val));
    assert(json_object_get_boolean(val) == true);

    assert(json_object_object_get_ex(root, "status", &val));
    assert(json_object_get_int(val) == MATCH_FINALIZED);

    assert(json_object_object_get_ex(root, "winnerSeat", &val));
    assert(json_object_get_int(val) == 0);

    struct json_object *playersJson = NULL;
    assert(json_object_object_get_ex(root, "players", &playersJson));
    assert(json_object_get_type(playersJson) == json_type_array);
    assert(json_object_array_length(playersJson) == 4);

    struct json_object *p0 = json_object_array_get_idx(playersJson, 0);
    assert(json_object_object_get_ex(p0, "userId", &val));
    assert((UserId)json_object_get_int64(val) == 101);
    assert(json_object_object_get_ex(p0, "name", &val));
    assert(strcmp(json_object_get_string(val), "Alice") == 0);
    assert(json_object_object_get_ex(p0, "points", &val));
    assert(json_object_get_int(val) == 50);

    struct json_object *eventsJson = NULL;
    assert(json_object_object_get_ex(root, "events", &eventsJson));
    assert(json_object_get_type(eventsJson) == json_type_array);
    assert(json_object_array_length(eventsJson) == 2);

    struct json_object *e0 = json_object_array_get_idx(eventsJson, 0);
    assert(json_object_object_get_ex(e0, "seq", &val));
    assert(json_object_get_int(val) == 1);
    assert(json_object_object_get_ex(e0, "type", &val));
    assert(strcmp(json_object_get_string(val), "CARD_PLAYED") == 0);

    struct json_object *e1 = json_object_array_get_idx(eventsJson, 1);
    assert(json_object_object_get_ex(e1, "seq", &val));
    assert(json_object_get_int(val) == 2);
    assert(json_object_object_get_ex(e1, "type", &val));
    assert(strcmp(json_object_get_string(val), "DRAW") == 0);

    json_object_put(root);
    free(content);
    unlink(exportPath);

    /* Exporting non-existent match should return NOT_FOUND */
    MatchId fakeMatchId = "99999999-9999-9999-9999-999999999999";
    status = store_export_match(store, (const MatchId *)&fakeMatchId, exportPath);
    assert(status == NOT_FOUND);

    store_close(store);
    cleanupFile(dbPath);
}

static void testProfileStatsInitialState(void) {
    const char *dbPath = "/tmp/test_uno_profile_stats_init.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);

    AccountCreate c;
    memset(&c, 0, sizeof(c));
    strncpy(c.email, "player@test.com", sizeof(c.email) - 1);
    strncpy(c.hash, "hash123", sizeof(c.hash) - 1);
    strncpy(c.name, "PlayerOne", sizeof(c.name) - 1);
    strncpy(c.avatar, "avatar1", sizeof(c.avatar) - 1);

    UserId uid = 0;
    assert(store_account_create(store, &c, &uid) == OK);
    assert(uid > 0);

    ProfileStats stats;
    memset(&stats, 0xFF, sizeof(stats));
    assert(store_profile_stats(store, uid, &stats) == OK);
    assert(stats.matches == 0);
    assert(stats.wins == 0);
    assert(stats.teamWins == 0);
    assert(stats.plus4 == 0);
    assert(stats.unoCalls == 0);
    assert(stats.streak == 0);
    assert(stats.bestStreak == 0);
    assert(stats.peakElo == 1500);

    /* Non-existent user returns NOT_FOUND */
    assert(store_profile_stats(store, 999999, &stats) == NOT_FOUND);

    store_close(store);
    cleanupFile(dbPath);
}

static void testProfileStatsFinalizeAccumulationAndOverwrite(void) {
    const char *dbPath = "/tmp/test_uno_profile_stats_accum.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);

    AccountCreate c;
    memset(&c, 0, sizeof(c));
    strncpy(c.email, "player@test.com", sizeof(c.email) - 1);
    strncpy(c.hash, "hash123", sizeof(c.hash) - 1);
    strncpy(c.name, "PlayerOne", sizeof(c.name) - 1);
    strncpy(c.avatar, "avatar1", sizeof(c.avatar) - 1);

    UserId uid = 0;
    assert(store_account_create(store, &c, &uid) == OK);
    assert(uid > 0);

    PlayerSpec players[4] = {
        {uid, "PlayerOne", "avatar1", EASY, false},
        {0, "Bot1", "avatar_bot", EASY, true},
        {0, "Bot2", "avatar_bot", EASY, true},
        {0, "Bot3", "avatar_bot", EASY, true}
    };

    /* Match 1: Win */
    MatchId mid1 = "00000000-0000-0000-0000-000000000011";
    assert(store_match_start(store, (const MatchId *)&mid1, players, CLASSIC, true) == OK);

    MatchResult res1;
    memset(&res1, 0, sizeof(res1));
    strncpy(res1.matchId, mid1, sizeof(res1.matchId) - 1);
    res1.mode = CLASSIC;
    res1.ranked = true;
    res1.status = MATCH_FINALIZED;
    res1.winnerSeat = 0;
    res1.winnerTeam = -1;
    res1.durationMs = 30000;
    res1.players[0].userId = uid;
    res1.players[0].eloBefore = 1500;
    res1.players[0].eloAfter = 1516;

    ProfileDelta deltas1[4];
    memset(deltas1, 0, sizeof(deltas1));
    deltas1[0].add.matches = 1;
    deltas1[0].add.wins = 1;
    deltas1[0].streak = 1;
    deltas1[0].bestStreak = 1;
    deltas1[0].peakElo = 1516;
    deltas1[0].unlocked = BADGE_FIRST_WIN;

    assert(store_finalize(store, &res1, deltas1) == OK);

    ProfileStats stats;
    assert(store_profile_stats(store, uid, &stats) == OK);
    assert(stats.matches == 1);
    assert(stats.wins == 1);
    assert(stats.streak == 1);
    assert(stats.bestStreak == 1);
    assert(stats.peakElo == 1516);

    /* Verify badge in achievements table */
    sqlite3 *db = store_get_db(store);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT badge, match_id FROM achievements WHERE user_id = ?;", -1,
                              &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)uid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == BADGE_FIRST_WIN);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 1), mid1) == 0);
    sqlite3_finalize(stmt);

    /* Match 2: Loss */
    MatchId mid2 = "00000000-0000-0000-0000-000000000012";
    assert(store_match_start(store, (const MatchId *)&mid2, players, CLASSIC, true) == OK);

    MatchResult res2;
    memset(&res2, 0, sizeof(res2));
    strncpy(res2.matchId, mid2, sizeof(res2.matchId) - 1);
    res2.mode = CLASSIC;
    res2.ranked = true;
    res2.status = MATCH_FINALIZED;
    res2.winnerSeat = 1;
    res2.winnerTeam = -1;
    res2.durationMs = 45000;
    res2.players[0].userId = uid;
    res2.players[0].eloBefore = 1516;
    res2.players[0].eloAfter = 1500;

    ProfileDelta deltas2[4];
    memset(deltas2, 0, sizeof(deltas2));
    deltas2[0].add.matches = 1;
    deltas2[0].add.wins = 0;
    deltas2[0].streak = -1;
    deltas2[0].bestStreak = 1;
    deltas2[0].peakElo = 1516;
    deltas2[0].unlocked = 0;

    assert(store_finalize(store, &res2, deltas2) == OK);

    assert(store_profile_stats(store, uid, &stats) == OK);
    assert(stats.matches == 2);
    assert(stats.wins == 1);
    assert(stats.streak == -1);     /* resets to negative! */
    assert(stats.bestStreak == 1); /* preserved! */
    assert(stats.peakElo == 1516);  /* NOT 1516+1516=3032! */

    /* Match 3: Win with peak ELO 1805 */
    MatchId mid3 = "00000000-0000-0000-0000-000000000013";
    assert(store_match_start(store, (const MatchId *)&mid3, players, CLASSIC, true) == OK);

    MatchResult res3;
    memset(&res3, 0, sizeof(res3));
    strncpy(res3.matchId, mid3, sizeof(res3.matchId) - 1);
    res3.mode = CLASSIC;
    res3.ranked = true;
    res3.status = MATCH_FINALIZED;
    res3.winnerSeat = 0;
    res3.winnerTeam = -1;
    res3.durationMs = 50000;
    res3.players[0].userId = uid;
    res3.players[0].eloBefore = 1790;
    res3.players[0].eloAfter = 1805;

    ProfileDelta deltas3[4];
    memset(deltas3, 0, sizeof(deltas3));
    deltas3[0].add.matches = 1;
    deltas3[0].add.wins = 1;
    deltas3[0].streak = 1;
    deltas3[0].bestStreak = 1;
    deltas3[0].peakElo = 1805;
    deltas3[0].unlocked = BADGE_DIAMOND;

    assert(store_finalize(store, &res3, deltas3) == OK);

    assert(store_profile_stats(store, uid, &stats) == OK);
    assert(stats.matches == 3);
    assert(stats.wins == 2);
    assert(stats.peakElo == 1805);

    /* Verify BADGE_DIAMOND is unlocked in achievements */
    assert(sqlite3_prepare_v2(db,
                              "SELECT count(*) FROM achievements WHERE user_id = ? AND badge = ?;",
                              -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)uid);
    sqlite3_bind_int(stmt, 2, BADGE_DIAMOND);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    store_close(store);
    cleanupFile(dbPath);
}

static int dbQueryInt(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

static void testUpgradeFromV1Database(void) {
    const char *dbPath = "/tmp/test_uno_store_upgrade_v1.db";
    cleanupFile(dbPath);

    /* Build a v1-shaped file: an existing account, the old key/progress
     * achievements table, no profile_stats rows and user_version 0. */
    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    AccountCreate c;
    memset(&c, 0, sizeof(c));
    strncpy(c.email, "legacy@test.com", sizeof(c.email) - 1);
    strncpy(c.hash, "hash", sizeof(c.hash) - 1);
    strncpy(c.name, "Legacy", sizeof(c.name) - 1);
    strncpy(c.avatar, "fox", sizeof(c.avatar) - 1);
    UserId uid = 0;
    assert(store_account_create(store, &c, &uid) == OK);
    sqlite3 *db = store_get_db(store);
    assert(sqlite3_exec(db,
                        "UPDATE accounts SET elo = 1620 WHERE id = 1;"
                        "DELETE FROM profile_stats;"
                        "DROP TABLE achievements;"
                        "CREATE TABLE achievements (user_id INTEGER NOT NULL, key TEXT NOT NULL,"
                        " progress INTEGER NOT NULL DEFAULT 0, unlocked_at INTEGER NOT NULL DEFAULT 0,"
                        " PRIMARY KEY (user_id, key));"
                        "INSERT INTO achievements (user_id, key, progress) VALUES (1, 'wins', 3);"
                        "PRAGMA user_version = 0;",
                        NULL, NULL, NULL) == SQLITE_OK);
    store_close(store);

    /* Reopening must upgrade instead of failing to prepare statements. */
    store = NULL;
    assert(store_open(&store, dbPath) == OK);
    db = store_get_db(store);
    assert(dbQueryInt(db, "PRAGMA user_version;") == 3);
    assert(dbQueryInt(db,
                      "SELECT count(*) FROM pragma_table_info('achievements') WHERE name = 'badge';") == 1);
    assert(dbQueryInt(db,
                      "SELECT count(*) FROM pragma_table_info('achievements') WHERE name = 'key';") == 0);

    ProfileStats stats;
    assert(store_profile_stats(store, uid, &stats) == OK);
    assert(stats.matches == 0);
    assert(stats.peakElo == 1620);
    store_close(store);

    /* A second open of a v2 file is a no-op and keeps data. */
    store = NULL;
    assert(store_open(&store, dbPath) == OK);
    assert(store_profile_stats(store, uid, &stats) == OK);
    assert(stats.peakElo == 1620);
    store_close(store);
    cleanupFile(dbPath);
}

static void testAccountCreateIsAtomic(void) {
    const char *dbPath = "/tmp/test_uno_account_atomic.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    sqlite3 *db = store_get_db(store);

    /* Make the profile_stats insert fail after the account insert succeeded. */
    assert(sqlite3_exec(db,
                        "CREATE TEMP TRIGGER fail_profile BEFORE INSERT ON profile_stats "
                        "BEGIN SELECT RAISE(ABORT, 'forced'); END;",
                        NULL, NULL, NULL) == SQLITE_OK);

    AccountCreate c;
    memset(&c, 0, sizeof(c));
    strncpy(c.email, "atomic@test.com", sizeof(c.email) - 1);
    strncpy(c.hash, "hash", sizeof(c.hash) - 1);
    strncpy(c.name, "Atomic", sizeof(c.name) - 1);
    strncpy(c.avatar, "fox", sizeof(c.avatar) - 1);
    UserId uid = 0;
    assert(store_account_create(store, &c, &uid) == STORAGE_ERROR);
    assert(dbQueryInt(db, "SELECT count(*) FROM accounts;") == 0);

    /* Once storage recovers, the same email can register. */
    assert(sqlite3_exec(db, "DROP TRIGGER fail_profile;", NULL, NULL, NULL) == SQLITE_OK);
    assert(store_account_create(store, &c, &uid) == OK);
    assert(dbQueryInt(db, "SELECT count(*) FROM accounts;") == 1);
    assert(dbQueryInt(db, "SELECT count(*) FROM profile_stats;") == 1);

    /* Duplicate email still maps to BAD_REQUEST and leaves no open transaction. */
    assert(store_account_create(store, &c, &uid) == BAD_REQUEST);
    assert(sqlite3_get_autocommit(db) != 0);

    store_close(store);
    cleanupFile(dbPath);
}

enum {
    ABORT_DURATION_MS = 45000,
    ABORT_EVENT_SEQ = 7,
    LEADERBOARD_ACCOUNTS = 22,
    LEADERBOARD_TIE_ELO = 2000,
    LEADERBOARD_BASE_ELO = 1000,
    LEADERBOARD_ELO_STEP = 10,
    RANKED_WINNER_ELO = 1516,
    RANKED_LOSER_ELO = 1484
};

static MatchEvent abortEvent(void) {
    MatchEvent event = {.seq = ABORT_EVENT_SEQ, .revision = ABORT_EVENT_SEQ, .actor = -1};
    strncpy(event.type, "MATCH_ABORTED", sizeof(event.type) - 1);
    strncpy(event.payload, "{}", sizeof(event.payload) - 1);
    return event;
}

static int matchStatusOf(sqlite3 *db, const char *matchId) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches WHERE match_id = ?;",
                              -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, matchId, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int status = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return status;
}

static void testAbortAtomicAndIdempotent(void) {
    const char *dbPath = "/tmp/test_uno_store_abort.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    sqlite3 *db = store_get_db(store);

    MatchId mid = "00000000-0000-0000-0000-000000000020";
    PlayerSpec players[4] = {
        {101, "Alice", "a1", EASY, false},
        {0, "Bot1", "bot", EASY, true},
        {0, "Bot2", "bot", EASY, true},
        {0, "Bot3", "bot", EASY, true}
    };
    assert(store_match_start(store, (const MatchId *)&mid, players, CLASSIC, false) == OK);
    MatchEvent event = abortEvent();

    /* A failed event insert must not leave the match marked ABORTED. */
    assert(sqlite3_exec(db,
                        "CREATE TEMP TRIGGER fail_abort_event BEFORE INSERT ON match_events "
                        "BEGIN SELECT RAISE(ABORT, 'forced'); END;",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(store_abort(store, (const MatchId *)&mid, &event, ABORT_DURATION_MS) == STORAGE_ERROR);
    assert(matchStatusOf(db, mid) == MATCH_ACTIVE);
    assert(dbQueryInt(db, "SELECT count(*) FROM match_events;") == 0);
    assert(sqlite3_get_autocommit(db) != 0);
    assert(sqlite3_exec(db, "DROP TRIGGER fail_abort_event;", NULL, NULL, NULL) == SQLITE_OK);

    /* A retried abort after success is OK and writes nothing more. */
    assert(store_abort(store, (const MatchId *)&mid, &event, ABORT_DURATION_MS) == OK);
    assert(store_abort(store, (const MatchId *)&mid, &event, ABORT_DURATION_MS) == OK);
    assert(matchStatusOf(db, mid) == MATCH_ABORTED);
    assert(dbQueryInt(db, "SELECT count(*) FROM match_events "
                            "WHERE type = 'MATCH_ABORTED';") == 1);
    assert(dbQueryInt(db, "SELECT duration_ms FROM matches;") == ABORT_DURATION_MS);

    MatchId unknown = "99999999-9999-9999-9999-999999999998";
    assert(store_abort(store, (const MatchId *)&unknown, &event, 0) == NOT_FOUND);

    /* A finalized match cannot be turned into an aborted one. */
    MatchId finished = "00000000-0000-0000-0000-000000000021";
    assert(store_match_start(store, (const MatchId *)&finished, players, CLASSIC, false) == OK);
    MatchResult res;
    memset(&res, 0, sizeof(res));
    strncpy(res.matchId, finished, sizeof(res.matchId) - 1);
    res.status = MATCH_FINALIZED;
    res.winnerSeat = 0;
    res.winnerTeam = -1;
    res.players[0].userId = 101;
    assert(store_finalize(store, &res, NULL) == OK);
    assert(store_abort(store, (const MatchId *)&finished, &event, 0) == INVALID_ACTION);
    assert(matchStatusOf(db, finished) == MATCH_FINALIZED);
    assert(sqlite3_get_autocommit(db) != 0);

    store_close(store);
    cleanupFile(dbPath);
}

static void testRankedFinalizeUpdatesEloOnce(void) {
    const char *dbPath = "/tmp/test_uno_store_ranked.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    sqlite3 *db = store_get_db(store);

    AccountCreate c;
    memset(&c, 0, sizeof(c));
    strncpy(c.hash, "hash", sizeof(c.hash) - 1);
    strncpy(c.name, "Ranked", sizeof(c.name) - 1);
    strncpy(c.avatar, "fox", sizeof(c.avatar) - 1);
    UserId winner = 0, loser = 0;
    strncpy(c.email, "winner@test.com", sizeof(c.email) - 1);
    assert(store_account_create(store, &c, &winner) == OK);
    strncpy(c.email, "loser@test.com", sizeof(c.email) - 1);
    assert(store_account_create(store, &c, &loser) == OK);

    MatchId mid = "00000000-0000-0000-0000-000000000022";
    PlayerSpec players[4] = {
        {winner, "Ranked", "fox", EASY, false},
        {loser, "Ranked", "fox", EASY, false},
        {0, "Bot1", "bot", EASY, true},
        {0, "Bot2", "bot", EASY, true}
    };
    assert(store_match_start(store, (const MatchId *)&mid, players, CLASSIC, true) == OK);
    assert(dbQueryInt(db, "SELECT ranked FROM matches;") == 1);

    MatchResult res;
    memset(&res, 0, sizeof(res));
    strncpy(res.matchId, mid, sizeof(res.matchId) - 1);
    res.mode = CLASSIC;
    res.ranked = true;
    res.status = MATCH_FINALIZED;
    res.winnerSeat = 0;
    res.winnerTeam = -1;
    res.durationMs = 1000;
    for (int i = 0; i < 4; i++) {
        res.players[i].userId = players[i].userId;
        res.players[i].team = -1;
        res.players[i].eloBefore = 1500;
        res.players[i].eloAfter = 1500;
    }
    res.players[0].eloAfter = RANKED_WINNER_ELO;
    res.players[1].eloAfter = RANKED_LOSER_ELO;

    Account account;
    assert(store_finalize(store, &res, NULL) == OK);
    assert(store_account_get_by_id(store, winner, &account) == OK);
    assert(account.elo == RANKED_WINNER_ELO);
    assert(store_account_get_by_id(store, loser, &account) == OK);
    assert(account.elo == RANKED_LOSER_ELO);

    /* Replaying the same finalize (storage retry) must not apply ELO twice. */
    assert(store_finalize(store, &res, NULL) == OK);
    assert(store_account_get_by_id(store, winner, &account) == OK);
    assert(account.elo == RANKED_WINNER_ELO);
    assert(store_account_get_by_id(store, loser, &account) == OK);
    assert(account.elo == RANKED_LOSER_ELO);

    store_close(store);
    cleanupFile(dbPath);
}

static void testLeaderboardOrder(void) {
    const char *dbPath = "/tmp/test_uno_store_leaderboard.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    sqlite3 *db = store_get_db(store);

    LeaderboardEntry entries[STORE_LEADERBOARD_MAX];
    size_t count = 99;
    assert(store_leaderboard(store, entries, &count) == OK);
    assert(count == 0);

    UserId ids[LEADERBOARD_ACCOUNTS];
    for (int i = 0; i < LEADERBOARD_ACCOUNTS; i++) {
        AccountCreate c;
        memset(&c, 0, sizeof(c));
        snprintf(c.email, sizeof(c.email), "rank%d@test.com", i);
        snprintf(c.name, sizeof(c.name), "Rank%d", i);
        snprintf(c.avatar, sizeof(c.avatar), "av%d", i);
        strncpy(c.hash, "hash", sizeof(c.hash) - 1);
        assert(store_account_create(store, &c, &ids[i]) == OK);

        char sql[128];
        snprintf(sql, sizeof(sql), "UPDATE accounts SET elo = %d WHERE id = %lld;",
                 LEADERBOARD_BASE_ELO + i * LEADERBOARD_ELO_STEP, (long long)ids[i]);
        assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    }
    /* Tie at the top: the later account is updated first, the lower id must still lead. */
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE accounts SET elo = %d WHERE id IN (%lld, %lld);",
             LEADERBOARD_TIE_ELO, (long long)ids[9], (long long)ids[4]);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    assert(store_leaderboard(store, entries, &count) == OK);
    assert(count == STORE_LEADERBOARD_MAX);
    assert(entries[0].userId == ids[4] && entries[0].elo == LEADERBOARD_TIE_ELO);
    assert(entries[1].userId == ids[9] && entries[1].elo == LEADERBOARD_TIE_ELO);
    assert(strcmp(entries[0].name, "Rank4") == 0);
    assert(strcmp(entries[0].avatar, "av4") == 0);
    assert(entries[2].userId == ids[LEADERBOARD_ACCOUNTS - 1]);
    for (size_t i = 1; i < count; i++) {
        assert(entries[i - 1].elo > entries[i].elo ||
               (entries[i - 1].elo == entries[i].elo &&
                entries[i - 1].userId < entries[i].userId));
    }
    /* The two lowest remaining ELOs (accounts 0 and 1) fall outside the top 20. */
    for (size_t i = 0; i < count; i++) {
        assert(entries[i].userId != ids[0] && entries[i].userId != ids[1]);
    }

    store_close(store);
    cleanupFile(dbPath);
}

enum {
    UNKNOWN_ACCOUNT_ID = 999,
    PROFILE_ELO_BEFORE = 1500,
    PROFILE_ELO_GAIN = 16,
    PROFILE_DURATION_MS = 61000,
    MS_PER_SECOND = 1000
};

static const char *INJECTION_NAME = "'; DROP TABLE accounts;--";

static UserId createAccount(Store *store, const char *email, const char *name) {
    AccountCreate c;
    memset(&c, 0, sizeof(c));
    strncpy(c.email, email, sizeof(c.email) - 1);
    strncpy(c.hash, "hash", sizeof(c.hash) - 1);
    strncpy(c.name, name, sizeof(c.name) - 1);
    strncpy(c.avatar, "fox", sizeof(c.avatar) - 1);
    UserId id = 0;
    assert(store_account_create(store, &c, &id) == OK);
    return id;
}

static const FriendRow *findFriend(const FriendRow *rows, size_t count, UserId id) {
    for (size_t i = 0; i < count; i++) {
        if (rows[i].userId == id) return &rows[i];
    }
    return NULL;
}

static FriendLink linkOf(Store *store, UserId me, UserId other) {
    FriendRow rows[STORE_FRIEND_MAX];
    size_t count = 0;
    assert(store_friend_list(store, me, rows, &count) == OK);
    const FriendRow *row = findFriend(rows, count, other);
    return row ? row->link : FRIEND_NONE;
}

static void testFriendRequestRules(void) {
    const char *dbPath = "/tmp/test_uno_store_friend_request.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    UserId alice = createAccount(store, "alice@test.com", "Alice");
    UserId bob = createAccount(store, "bob@test.com", "Bob");

    assert(store_friend_request(store, alice, alice) == BAD_REQUEST);
    assert(store_friend_request(store, alice, UNKNOWN_ACCOUNT_ID) == NOT_FOUND);

    assert(store_friend_request(store, alice, bob) == OK);
    assert(linkOf(store, alice, bob) == FRIEND_PENDING_OUT);
    assert(linkOf(store, bob, alice) == FRIEND_PENDING_IN);

    /* A second request in either direction is refused, not merged. */
    assert(store_friend_request(store, alice, bob) == INVALID_ACTION);
    assert(store_friend_request(store, bob, alice) == INVALID_ACTION);
    assert(dbQueryInt(store_get_db(store), "SELECT count(*) FROM friends;") == 1);

    /* The sender cannot answer its own request. */
    assert(store_friend_reply(store, alice, bob, true) == NOT_FOUND);
    assert(linkOf(store, alice, bob) == FRIEND_PENDING_OUT);

    FriendRow rows[STORE_FRIEND_MAX];
    size_t count = 0;
    assert(store_friend_list(store, bob, rows, &count) == OK);
    assert(count == 1);
    assert(rows[0].userId == alice);
    assert(strcmp(rows[0].name, "Alice") == 0);
    assert(strcmp(rows[0].avatar, "fox") == 0);
    assert(rows[0].elo == PROFILE_ELO_BEFORE);

    store_close(store);
    cleanupFile(dbPath);
}

static void testFriendReplyAcceptAndReject(void) {
    const char *dbPath = "/tmp/test_uno_store_friend_reply.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    UserId alice = createAccount(store, "alice@test.com", "Alice");
    UserId bob = createAccount(store, "bob@test.com", "Bob");
    UserId carol = createAccount(store, "carol@test.com", "Carol");

    /* No pending request at all. */
    assert(store_friend_reply(store, bob, alice, true) == NOT_FOUND);
    assert(store_friend_reply(store, bob, alice, false) == NOT_FOUND);

    assert(store_friend_request(store, alice, bob) == OK);
    assert(store_friend_reply(store, bob, alice, true) == OK);
    assert(linkOf(store, alice, bob) == FRIEND_ACCEPTED);
    assert(linkOf(store, bob, alice) == FRIEND_ACCEPTED);
    /* Already accepted: nothing pending to answer, and no new request. */
    assert(store_friend_reply(store, bob, alice, false) == NOT_FOUND);
    assert(linkOf(store, alice, bob) == FRIEND_ACCEPTED);
    assert(store_friend_request(store, bob, alice) == INVALID_ACTION);

    /* Reject removes the row, so the list is empty and a new request works. */
    assert(store_friend_request(store, carol, alice) == OK);
    assert(store_friend_reply(store, alice, carol, false) == OK);
    FriendRow rows[STORE_FRIEND_MAX];
    size_t count = 99;
    assert(store_friend_list(store, carol, rows, &count) == OK);
    assert(count == 0);
    assert(linkOf(store, alice, carol) == FRIEND_NONE);
    assert(store_friend_request(store, carol, alice) == OK);
    assert(linkOf(store, alice, carol) == FRIEND_PENDING_IN);

    /* Alice now sees one accepted friend and one incoming request. */
    assert(store_friend_list(store, alice, rows, &count) == OK);
    assert(count == 2);
    assert(rows[0].userId == bob && rows[0].link == FRIEND_ACCEPTED);
    assert(rows[1].userId == carol && rows[1].link == FRIEND_PENDING_IN);

    store_close(store);
    cleanupFile(dbPath);
}

enum { FRIEND_FLOOD_EXTRA = 6 };

static void testFriendListAcceptedBeforePending(void) {
    const char *dbPath = "/tmp/test_uno_store_friend_flood.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    UserId me = createAccount(store, "me@test.com", "Me");
    /* More pending requests than the list can hold, all from lower ids than
     * the real friend, so ordering by id alone would push the friend out. */
    size_t flood = STORE_FRIEND_MAX + FRIEND_FLOOD_EXTRA;
    for (size_t i = 0; i < flood; i++) {
        char email[64];
        snprintf(email, sizeof(email), "spam%zu@test.com", i);
        UserId spammer = createAccount(store, email, "Spam");
        assert(store_friend_request(store, spammer, me) == OK);
    }
    UserId oldFriend = createAccount(store, "friend1@test.com", "Friend1");
    UserId newFriend = createAccount(store, "friend2@test.com", "Friend2");
    assert(store_friend_request(store, newFriend, me) == OK);
    assert(store_friend_reply(store, me, newFriend, true) == OK);
    assert(store_friend_request(store, me, oldFriend) == OK);
    assert(store_friend_reply(store, oldFriend, me, true) == OK);

    FriendRow rows[STORE_FRIEND_MAX];
    size_t count = 0;
    assert(store_friend_list(store, me, rows, &count) == OK);
    assert(count == STORE_FRIEND_MAX);
    /* Accepted rows come first, each group ordered by account id. */
    assert(rows[0].userId == oldFriend && rows[0].link == FRIEND_ACCEPTED);
    assert(rows[1].userId == newFriend && rows[1].link == FRIEND_ACCEPTED);
    for (size_t i = 2; i < count; i++) {
        assert(rows[i].link == FRIEND_PENDING_IN);
        assert(i == 2 || rows[i].userId > rows[i - 1].userId);
    }

    store_close(store);
    cleanupFile(dbPath);
}

static void startHumanMatch(Store *store, const char *matchId, Mode mode,
                            const UserId humans[4])
{
    PlayerSpec players[4];
    memset(players, 0, sizeof(players));
    for (int i = 0; i < 4; i++) {
        players[i].userId = humans[i];
        players[i].isBot = humans[i] == 0;
        snprintf(players[i].name, sizeof(players[i].name), "P%d", i);
        strncpy(players[i].avatar, "fox", sizeof(players[i].avatar) - 1);
    }
    assert(store_match_start(store, (const MatchId *)matchId, players, mode,
                             mode == CLASSIC) == OK);
}

static MatchResult finishedResult(const char *matchId, Mode mode, const UserId humans[4]) {
    MatchResult res;
    memset(&res, 0, sizeof(res));
    strncpy(res.matchId, matchId, sizeof(res.matchId) - 1);
    res.mode = mode;
    res.ranked = mode == CLASSIC;
    res.status = MATCH_FINALIZED;
    res.winnerSeat = -1;
    res.winnerTeam = -1;
    res.durationMs = PROFILE_DURATION_MS;
    for (int i = 0; i < 4; i++) {
        res.players[i].userId = humans[i];
        res.players[i].team = -1;
        res.players[i].eloBefore = PROFILE_ELO_BEFORE;
        res.players[i].eloAfter = PROFILE_ELO_BEFORE;
    }
    return res;
}

static void testProfileGetAfterMatches(void) {
    const char *dbPath = "/tmp/test_uno_store_profile.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    UserId alice = createAccount(store, "alice@test.com", "Alice");
    UserId bob = createAccount(store, "bob@test.com", "Bob");

    Profile profile;
    assert(store_profile_get(store, UNKNOWN_ACCOUNT_ID, &profile) == NOT_FOUND);
    assert(store_profile_get(store, alice, &profile) == OK);
    assert(profile.userId == alice);
    assert(profile.historyCount == 0);
    assert(profile.badges == 0);

    /* Ranked CLASSIC win for Alice at seat 0. */
    const UserId classicSeats[4] = {alice, bob, 0, 0};
    const char *rankedId = "00000000-0000-0000-0000-000000000030";
    startHumanMatch(store, rankedId, CLASSIC, classicSeats);
    MatchResult ranked = finishedResult(rankedId, CLASSIC, classicSeats);
    ranked.winnerSeat = 0;
    ranked.players[0].eloAfter = PROFILE_ELO_BEFORE + PROFILE_ELO_GAIN;
    ranked.players[1].eloAfter = PROFILE_ELO_BEFORE - PROFILE_ELO_GAIN;
    ProfileDelta deltas[4];
    memset(deltas, 0, sizeof(deltas));
    deltas[0].add.matches = 1;
    deltas[0].add.wins = 1;
    deltas[0].streak = 1;
    deltas[0].bestStreak = 1;
    deltas[0].peakElo = PROFILE_ELO_BEFORE + PROFILE_ELO_GAIN;
    deltas[0].unlocked = BADGE_FIRST_WIN;
    deltas[1].add.matches = 1;
    deltas[1].streak = -1;
    deltas[1].peakElo = PROFILE_ELO_BEFORE;
    assert(store_finalize(store, &ranked, deltas) == OK);

    /* Unranked TWO_V_TWO where Alice (seat 1, team 1) loses to team 0. */
    const UserId teamSeats[4] = {bob, alice, 0, 0};
    const char *teamId = "00000000-0000-0000-0000-000000000031";
    startHumanMatch(store, teamId, TWO_V_TWO, teamSeats);
    MatchResult team = finishedResult(teamId, TWO_V_TWO, teamSeats);
    team.winnerSeat = 0;
    team.winnerTeam = 0;
    for (int i = 0; i < 4; i++) team.players[i].team = i % 2;
    assert(store_finalize(store, &team, NULL) == OK);

    /* An aborted and an active match never reach the profile. */
    const char *abortedId = "00000000-0000-0000-0000-000000000032";
    startHumanMatch(store, abortedId, DRAFT, classicSeats);
    MatchEvent event = abortEvent();
    assert(store_abort(store, (const MatchId *)abortedId, &event, 0) == OK);
    startHumanMatch(store, "00000000-0000-0000-0000-000000000033", SIDE, classicSeats);

    assert(store_profile_get(store, alice, &profile) == OK);
    assert(profile.userId == alice);
    assert(strcmp(profile.name, "Alice") == 0);
    assert(strcmp(profile.avatar, "fox") == 0);
    assert(profile.elo == PROFILE_ELO_BEFORE + PROFILE_ELO_GAIN);
    assert(profile.stats.matches == 1 && profile.stats.wins == 1);
    assert(profile.badges == BADGE_FIRST_WIN);
    assert(profile.modes[CLASSIC].matches == 1 && profile.modes[CLASSIC].wins == 1);
    assert(profile.modes[TWO_V_TWO].matches == 1 && profile.modes[TWO_V_TWO].wins == 0);
    assert(profile.modes[SIDE].matches == 0 && profile.modes[DRAFT].matches == 0);

    assert(profile.historyCount == 2);
    /* Newest first: the team match was finalized after the ranked one. */
    assert(strcmp(profile.history[0].matchId, teamId) == 0);
    assert(profile.history[0].mode == TWO_V_TWO);
    assert(!profile.history[0].won);
    assert(profile.history[0].eloDelta == 0);
    assert(strcmp(profile.history[1].matchId, rankedId) == 0);
    assert(profile.history[1].mode == CLASSIC);
    assert(profile.history[1].won);
    assert(profile.history[1].eloDelta == PROFILE_ELO_GAIN);
    assert(profile.history[1].endedAt > 0);

    /* Bob: lost the ranked match, won the team match as team 0. */
    assert(store_profile_get(store, bob, &profile) == OK);
    assert(profile.modes[CLASSIC].matches == 1 && profile.modes[CLASSIC].wins == 0);
    assert(profile.modes[TWO_V_TWO].wins == 1);
    assert(profile.history[0].won);
    assert(profile.history[1].eloDelta == -PROFILE_ELO_GAIN);

    store_close(store);
    cleanupFile(dbPath);
}

static void testProfileHistoryNewestTwenty(void) {
    const char *dbPath = "/tmp/test_uno_store_history.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    UserId alice = createAccount(store, "alice@test.com", "Alice");
    const UserId seats[4] = {alice, 0, 0, 0};

    const int total = STORE_HISTORY_MAX + 2;
    for (int i = 0; i < total; i++) {
        char matchId[37];
        snprintf(matchId, sizeof(matchId), "00000000-0000-0000-0000-0000000001%02d", i);
        startHumanMatch(store, matchId, DRAFT, seats);
        MatchResult res = finishedResult(matchId, DRAFT, seats);
        res.winnerSeat = 0;
        assert(store_finalize(store, &res, NULL) == OK);
    }

    Profile profile;
    assert(store_profile_get(store, alice, &profile) == OK);
    assert(profile.historyCount == STORE_HISTORY_MAX);
    assert(profile.modes[DRAFT].matches == (uint32_t)total);
    assert(profile.modes[DRAFT].wins == (uint32_t)total);
    char newest[37];
    snprintf(newest, sizeof(newest), "00000000-0000-0000-0000-0000000001%02d", total - 1);
    assert(strcmp(profile.history[0].matchId, newest) == 0);

    store_close(store);
    cleanupFile(dbPath);
}

static void testProfileUpdateStoresTextVerbatim(void) {
    const char *dbPath = "/tmp/test_uno_store_profile_update.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    UserId alice = createAccount(store, "alice@test.com", "Alice");

    assert(store_profile_update(store, UNKNOWN_ACCOUNT_ID, "Ghost", "owl") == NOT_FOUND);
    assert(store_profile_update(store, alice, NULL, "owl") == BAD_REQUEST);

    assert(store_profile_update(store, alice, "Alicia", "owl") == OK);
    Account account;
    assert(store_account_get_by_id(store, alice, &account) == OK);
    assert(strcmp(account.name, "Alicia") == 0);
    assert(strcmp(account.avatar, "owl") == 0);
    assert(strcmp(account.email, "alice@test.com") == 0);

    assert(store_profile_update(store, alice, INJECTION_NAME, "owl") == OK);
    assert(store_account_get_by_id(store, alice, &account) == OK);
    assert(strcmp(account.name, INJECTION_NAME) == 0);
    assert(dbQueryInt(store_get_db(store), "SELECT count(*) FROM accounts;") == 1);

    store_close(store);
    cleanupFile(dbPath);
}

static void testFriendAndProfileSurviveReopen(void) {
    const char *dbPath = "/tmp/test_uno_store_social_reopen.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    UserId alice = createAccount(store, "alice@test.com", "Alice");
    UserId bob = createAccount(store, "bob@test.com", "Bob");
    UserId carol = createAccount(store, "carol@test.com", "Carol");
    assert(store_friend_request(store, alice, bob) == OK);
    assert(store_friend_reply(store, bob, alice, true) == OK);
    assert(store_friend_request(store, carol, alice) == OK);
    assert(store_profile_update(store, alice, "Alicia", "owl") == OK);

    const UserId seats[4] = {alice, bob, 0, 0};
    const char *matchId = "00000000-0000-0000-0000-000000000040";
    startHumanMatch(store, matchId, CLASSIC, seats);
    MatchResult res = finishedResult(matchId, CLASSIC, seats);
    res.winnerSeat = 0;
    res.players[0].eloAfter = PROFILE_ELO_BEFORE + PROFILE_ELO_GAIN;
    assert(store_finalize(store, &res, NULL) == OK);
    store_close(store);

    store = NULL;
    assert(store_open(&store, dbPath) == OK);
    assert(linkOf(store, alice, bob) == FRIEND_ACCEPTED);
    assert(linkOf(store, alice, carol) == FRIEND_PENDING_IN);
    assert(linkOf(store, carol, alice) == FRIEND_PENDING_OUT);
    Profile profile;
    assert(store_profile_get(store, alice, &profile) == OK);
    assert(strcmp(profile.name, "Alicia") == 0);
    assert(strcmp(profile.avatar, "owl") == 0);
    assert(profile.historyCount == 1);
    assert(profile.history[0].won);
    assert(profile.history[0].eloDelta == PROFILE_ELO_GAIN);
    assert(profile.modes[CLASSIC].wins == 1);

    store_close(store);
    cleanupFile(dbPath);
}

static void testUpgradeAddsMatchEndTime(void) {
    const char *dbPath = "/tmp/test_uno_store_upgrade_v2.db";
    cleanupFile(dbPath);

    Store *store = NULL;
    assert(store_open(&store, dbPath) == OK);
    UserId alice = createAccount(store, "alice@test.com", "Alice");
    const UserId seats[4] = {alice, 0, 0, 0};
    const char *matchId = "00000000-0000-0000-0000-000000000050";
    startHumanMatch(store, matchId, CLASSIC, seats);
    MatchResult res = finishedResult(matchId, CLASSIC, seats);
    res.winnerSeat = 0;
    assert(store_finalize(store, &res, NULL) == OK);

    /* Rebuild the v2 shape: matches had no end-time column. */
    sqlite3 *db = store_get_db(store);
    assert(sqlite3_exec(db,
                        "ALTER TABLE matches DROP COLUMN ended_at;"
                        "PRAGMA user_version = 2;",
                        NULL, NULL, NULL) == SQLITE_OK);
    int createdAt = dbQueryInt(db, "SELECT created_at FROM matches;");
    store_close(store);

    store = NULL;
    assert(store_open(&store, dbPath) == OK);
    db = store_get_db(store);
    assert(dbQueryInt(db, "PRAGMA user_version;") == 3);
    assert(dbQueryInt(db, "SELECT count(*) FROM pragma_table_info('matches') "
                            "WHERE name = 'ended_at';") == 1);

    /* Old finalized rows get start + duration as their end time. */
    Profile profile;
    assert(store_profile_get(store, alice, &profile) == OK);
    assert(profile.historyCount == 1);
    assert(profile.history[0].endedAt ==
           (int64_t)createdAt + PROFILE_DURATION_MS / MS_PER_SECOND);
    store_close(store);
    cleanupFile(dbPath);
}

int main(void) {
    testFriendRequestRules();
    testFriendReplyAcceptAndReject();
    testFriendListAcceptedBeforePending();
    testProfileGetAfterMatches();
    testProfileHistoryNewestTwenty();
    testProfileUpdateStoresTextVerbatim();
    testFriendAndProfileSurviveReopen();
    testUpgradeAddsMatchEndTime();
    testAbortAtomicAndIdempotent();
    testRankedFinalizeUpdatesEloOnce();
    testLeaderboardOrder();
    testTableCreationAndMigration();
    testUpgradeFromV1Database();
    testAccountCreateIsAtomic();
    testStoreMatchStartPersisting();
    testStoreEventsUniquenessAndIdempotency();
    testStoreFinalizeIdempotencyAndNoDoubleElo();
    testCrashRecoveryAborted();
    testTransactionRollbackOnFailure();
    testStoreExportMatch();
    testProfileStatsInitialState();
    testProfileStatsFinalizeAccumulationAndOverwrite();

    puts("store: OK");
    return 0;
}
