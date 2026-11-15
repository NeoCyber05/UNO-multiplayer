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

static void cleanup_file(const char *path)
{
    unlink(path);
    char shm[512], wal[512];
    snprintf(shm, sizeof(shm), "%s-shm", path);
    snprintf(wal, sizeof(wal), "%s-wal", path);
    unlink(shm);
    unlink(wal);
}

static void test_table_creation_and_migration(void)
{
    const char *db_path = "/tmp/test_uno_store_migration.db";
    cleanup_file(db_path);

    Store *s = NULL;
    Status st = store_open(&s, db_path);
    assert(st == OK);
    assert(s != NULL);

    sqlite3 *db = store_get_db(s);
    assert(db != NULL);

    const char *tables[] = {
        "matches", "match_players", "match_events",
        "accounts", "friends", "achievements"
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

    store_close(s);
    cleanup_file(db_path);
}

static void test_store_match_start_persisting(void)
{
    const char *db_path = "/tmp/test_uno_store_start.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);

    MatchId mid = "00000000-0000-0000-0000-000000000001";
    PlayerSpec players[4] = {
        {101, "Alice", "avatar1", EASY, false},
        {102, "Bob", "avatar2", EASY, false},
        {0, "BotCharlie", "avatar_bot", EASY, true},
        {0, "BotDavid", "avatar_bot", HARD, true}
    };

    Status st = store_match_start(s, (const MatchId *)&mid, players, CLASSIC, true);
    assert(st == OK);

    /* Duplicate call with identical parameters should be idempotent OK */
    st = store_match_start(s, (const MatchId *)&mid, players, CLASSIC, true);
    assert(st == OK);

    /* Duplicate call with conflicting mode should return INVALID_ACTION */
    st = store_match_start(s, (const MatchId *)&mid, players, TWO_V_TWO, true);
    assert(st == INVALID_ACTION);

    /* Verify persisted data via SQLite directly */
    sqlite3 *db = store_get_db(s);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT mode, ranked, status FROM matches WHERE match_id = ?;", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == CLASSIC);
    assert(sqlite3_column_int(stmt, 1) == 1);
    assert(sqlite3_column_int(stmt, 2) == MATCH_ACTIVE);
    sqlite3_finalize(stmt);

    assert(sqlite3_prepare_v2(db, "SELECT seat, user_id, name, is_bot, bot_level FROM match_players WHERE match_id = ? ORDER BY seat ASC;", -1, &stmt, NULL) == SQLITE_OK);
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

    store_close(s);
    cleanup_file(db_path);
}

static void test_store_events_uniqueness_and_idempotency(void)
{
    const char *db_path = "/tmp/test_uno_store_events.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);

    MatchId mid = "00000000-0000-0000-0000-000000000002";
    PlayerSpec players[4] = {
        {101, "Alice", "a1", EASY, false},
        {102, "Bob", "a2", EASY, false},
        {103, "Charlie", "a3", EASY, false},
        {104, "David", "a4", EASY, false}
    };
    assert(store_match_start(s, (const MatchId *)&mid, players, CLASSIC, false) == OK);

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
    Status st = store_events(s, (const MatchId *)&mid, &batch1);
    assert(st == OK);

    /* Duplicate identical batch -> idempotent OK */
    st = store_events(s, (const MatchId *)&mid, &batch1);
    assert(st == OK);

    /* Duplicate identical single event -> idempotent OK */
    EventBatch batch_dup;
    memset(&batch_dup, 0, sizeof(batch_dup));
    batch_dup.count = 1;
    batch_dup.events[0] = batch1.events[0];
    st = store_events(s, (const MatchId *)&mid, &batch_dup);
    assert(st == OK);

    /* Conflicting sequence: same seq=2, but different actor/type/payload -> INVALID_ACTION */
    EventBatch batch_conflict;
    memset(&batch_conflict, 0, sizeof(batch_conflict));
    batch_conflict.count = 1;
    batch_conflict.events[0].seq = 2;
    batch_conflict.events[0].revision = 2;
    batch_conflict.events[0].actor = 2; /* changed actor */
    strncpy(batch_conflict.events[0].type, "PASS", sizeof(batch_conflict.events[0].type) - 1);
    strncpy(batch_conflict.events[0].payload, "{}", sizeof(batch_conflict.events[0].payload) - 1);

    st = store_events(s, (const MatchId *)&mid, &batch_conflict);
    assert(st == INVALID_ACTION);

    /* Conflicting payload on same seq=1 -> INVALID_ACTION */
    batch_conflict.events[0].seq = 1;
    batch_conflict.events[0].revision = 1;
    batch_conflict.events[0].actor = 0;
    strncpy(batch_conflict.events[0].type, "CARD_PLAYED", sizeof(batch_conflict.events[0].type) - 1);
    strncpy(batch_conflict.events[0].payload, "{\"card\":\"G5\"}", sizeof(batch_conflict.events[0].payload) - 1);

    st = store_events(s, (const MatchId *)&mid, &batch_conflict);
    assert(st == INVALID_ACTION);

    /* Append next event seq=3 -> OK */
    EventBatch batch2;
    memset(&batch2, 0, sizeof(batch2));
    batch2.count = 1;
    batch2.events[0].seq = 3;
    batch2.events[0].revision = 3;
    batch2.events[0].actor = 2;
    strncpy(batch2.events[0].type, "CARD_PLAYED", sizeof(batch2.events[0].type) - 1);
    strncpy(batch2.events[0].payload, "{\"card\":\"R7\"}", sizeof(batch2.events[0].payload) - 1);

    st = store_events(s, (const MatchId *)&mid, &batch2);
    assert(st == OK);

    /* Verify count in DB is exactly 3 */
    sqlite3 *db = store_get_db(s);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM match_events WHERE match_id = ?;", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 3);
    sqlite3_finalize(stmt);

    /* Calling store_events with unknown match_id should return NOT_FOUND */
    MatchId fake_mid = "99999999-9999-9999-9999-999999999999";
    st = store_events(s, (const MatchId *)&fake_mid, &batch2);
    assert(st == NOT_FOUND);

    store_close(s);
    cleanup_file(db_path);
}

static void test_store_finalize_idempotency_and_no_double_elo(void)
{
    const char *db_path = "/tmp/test_uno_store_finalize.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);

    /* Seed test accounts */
    sqlite3 *db = store_get_db(s);
    assert(sqlite3_exec(db,
        "INSERT INTO accounts (id, email, password_hash, name, avatar, elo, created_at) VALUES "
        "(101, 'alice@test.com', 'h1', 'Alice', 'a1', 1500, 1000),"
        "(102, 'bob@test.com', 'h2', 'Bob', 'a2', 1500, 1000);",
        NULL, NULL, NULL) == SQLITE_OK);

    MatchId mid = "00000000-0000-0000-0000-000000000003";
    PlayerSpec players[4] = {
        {101, "Alice", "a1", EASY, false},
        {102, "Bob", "a2", EASY, false},
        {0, "Bot1", "bot", EASY, true},
        {0, "Bot2", "bot", EASY, true}
    };
    assert(store_match_start(s, (const MatchId *)&mid, players, CLASSIC, true) == OK);

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
    deltas[0].matches = 1;
    deltas[0].wins = 1;
    deltas[0].plus4 = 1;
    deltas[0].unoCalls = 1;
    deltas[0].streak = 1;
    deltas[0].bestStreak = 1;
    deltas[0].peakElo = 1516;
    deltas[0].badges.firstWin = 1;

    deltas[1].matches = 1;
    deltas[1].wins = 0;
    deltas[1].peakElo = 1500;

    /* First finalize -> should succeed */
    Status st = store_finalize(s, &res, deltas);
    assert(st == OK);

    /* Check ELO and stats after first finalize */
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 101;", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int elo101 = sqlite3_column_int(stmt, 0);
    assert(elo101 == 1516);
    sqlite3_finalize(stmt);

    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 102;", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int elo102 = sqlite3_column_int(stmt, 0);
    assert(elo102 == 1484);
    sqlite3_finalize(stmt);

    /* Second finalize with identical result -> idempotent OK, no doubling! */
    st = store_finalize(s, &res, deltas);
    assert(st == OK);

    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 101;", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    elo101 = sqlite3_column_int(stmt, 0);
    assert(elo101 == 1516); /* NOT doubled to 1532! */
    sqlite3_finalize(stmt);

    /* Second finalize with conflicting result -> INVALID_ACTION */
    MatchResult conflicting_res = res;
    conflicting_res.winnerSeat = 1; /* different winner */
    st = store_finalize(s, &conflicting_res, deltas);
    assert(st == INVALID_ACTION);

    /* Check that ELO is still 1516 after conflicting finalize attempt */
    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 101;", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    elo101 = sqlite3_column_int(stmt, 0);
    assert(elo101 == 1516);
    sqlite3_finalize(stmt);

    store_close(s);
    cleanup_file(db_path);
}

static void test_crash_recovery_aborted(void)
{
    const char *db_path = "/tmp/test_uno_store_crash.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);

    MatchId mid_active = "00000000-0000-0000-0000-000000000004";
    MatchId mid_final = "00000000-0000-0000-0000-000000000005";

    PlayerSpec players[4] = {
        {101, "Alice", "a1", EASY, false},
        {102, "Bob", "a2", EASY, false},
        {0, "Bot1", "bot", EASY, true},
        {0, "Bot2", "bot", EASY, true}
    };

    assert(store_match_start(s, (const MatchId *)&mid_active, players, CLASSIC, false) == OK);
    assert(store_match_start(s, (const MatchId *)&mid_final, players, CLASSIC, false) == OK);

    MatchResult res;
    memset(&res, 0, sizeof(res));
    strncpy(res.matchId, mid_final, sizeof(res.matchId) - 1);
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
    assert(store_finalize(s, &res, NULL) == OK);

    /* Close the store abruptly */
    store_close(s);
    s = NULL;

    /* Reopen the store -> crash recovery should mark active matches as aborted */
    assert(store_open(&s, db_path) == OK);

    sqlite3 *db = store_get_db(s);
    sqlite3_stmt *stmt = NULL;

    /* mid_active should now be MATCH_ABORTED */
    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches WHERE match_id = ?;", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid_active, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int status_active = sqlite3_column_int(stmt, 0);
    assert(status_active == MATCH_ABORTED);
    sqlite3_finalize(stmt);

    /* mid_final should still be MATCH_FINALIZED */
    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches WHERE match_id = ?;", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid_final, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int status_final = sqlite3_column_int(stmt, 0);
    assert(status_final == MATCH_FINALIZED);
    sqlite3_finalize(stmt);

    store_close(s);
    cleanup_file(db_path);
}

static void test_transaction_rollback_on_failure(void)
{
    const char *db_path = "/tmp/test_uno_store_rollback.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);

    sqlite3 *db = store_get_db(s);
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
    assert(store_match_start(s, (const MatchId *)&mid, players, CLASSIC, true) == OK);

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
    deltas[0].matches = 1;
    deltas[0].wins = 1;

    /* store_finalize should encounter error and rollback entire transaction */
    Status st = store_finalize(s, &res, deltas);
    assert(st == STORAGE_ERROR);

    /* Verify match status is STILL MATCH_ACTIVE */
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches WHERE match_id = ?;", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == MATCH_ACTIVE);
    sqlite3_finalize(stmt);

    /* Verify accounts ELO was NOT updated */
    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 101;", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1500);
    sqlite3_finalize(stmt);

    /* Drop the abort trigger */
    assert(sqlite3_exec(db, "DROP TRIGGER test_fail_trigger;", NULL, NULL, NULL) == SQLITE_OK);

    /* Now finalize should succeed cleanly */
    st = store_finalize(s, &res, deltas);
    assert(st == OK);

    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches WHERE match_id = ?;", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == MATCH_FINALIZED);
    sqlite3_finalize(stmt);

    assert(sqlite3_prepare_v2(db, "SELECT elo FROM accounts WHERE id = 101;", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1530);
    sqlite3_finalize(stmt);

    store_close(s);
    cleanup_file(db_path);
}

static void test_store_export_match(void)
{
    const char *db_path = "/tmp/test_uno_store_export.db";
    const char *export_path = "/tmp/test_uno_match_export.json";
    cleanup_file(db_path);
    unlink(export_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);

    MatchId mid = "00000000-0000-0000-0000-000000000007";
    PlayerSpec players[4] = {
        {101, "Alice", "avatar1", EASY, false},
        {102, "Bob", "avatar2", EASY, false},
        {0, "Bot1", "avatar_bot", EASY, true},
        {0, "Bot2", "avatar_bot", HARD, true}
    };
    assert(store_match_start(s, (const MatchId *)&mid, players, CLASSIC, true) == OK);

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
    assert(store_events(s, (const MatchId *)&mid, &batch) == OK);

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
    assert(store_finalize(s, &res, NULL) == OK);

    /* Export match */
    Status st = store_export_match(s, (const MatchId *)&mid, export_path);
    assert(st == OK);

    /* Read exported JSON file */
    FILE *f = fopen(export_path, "rb");
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

    struct json_object *players_arr = NULL;
    assert(json_object_object_get_ex(root, "players", &players_arr));
    assert(json_object_get_type(players_arr) == json_type_array);
    assert(json_object_array_length(players_arr) == 4);

    struct json_object *p0 = json_object_array_get_idx(players_arr, 0);
    assert(json_object_object_get_ex(p0, "userId", &val));
    assert((UserId)json_object_get_int64(val) == 101);
    assert(json_object_object_get_ex(p0, "name", &val));
    assert(strcmp(json_object_get_string(val), "Alice") == 0);
    assert(json_object_object_get_ex(p0, "points", &val));
    assert(json_object_get_int(val) == 50);

    struct json_object *events_arr = NULL;
    assert(json_object_object_get_ex(root, "events", &events_arr));
    assert(json_object_get_type(events_arr) == json_type_array);
    assert(json_object_array_length(events_arr) == 2);

    struct json_object *e0 = json_object_array_get_idx(events_arr, 0);
    assert(json_object_object_get_ex(e0, "seq", &val));
    assert(json_object_get_int(val) == 1);
    assert(json_object_object_get_ex(e0, "type", &val));
    assert(strcmp(json_object_get_string(val), "CARD_PLAYED") == 0);

    struct json_object *e1 = json_object_array_get_idx(events_arr, 1);
    assert(json_object_object_get_ex(e1, "seq", &val));
    assert(json_object_get_int(val) == 2);
    assert(json_object_object_get_ex(e1, "type", &val));
    assert(strcmp(json_object_get_string(val), "DRAW") == 0);

    json_object_put(root);
    free(content);
    unlink(export_path);

    /* Exporting non-existent match should return NOT_FOUND */
    MatchId fake_mid = "99999999-9999-9999-9999-999999999999";
    st = store_export_match(s, (const MatchId *)&fake_mid, export_path);
    assert(st == NOT_FOUND);

    store_close(s);
    cleanup_file(db_path);
}

int main(void)
{
    test_table_creation_and_migration();
    test_store_match_start_persisting();
    test_store_events_uniqueness_and_idempotency();
    test_store_finalize_idempotency_and_no_double_elo();
    test_crash_recovery_aborted();
    test_transaction_rollback_on_failure();
    test_store_export_match();

    puts("store: OK");
    return 0;
}
