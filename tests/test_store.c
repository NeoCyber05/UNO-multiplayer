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
    deltas[0].add.matches = 1;
    deltas[0].add.wins = 1;

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

static void test_profile_stats_initial_state(void)
{
    const char *db_path = "/tmp/test_uno_profile_stats_init.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);

    AccountCreate c;
    memset(&c, 0, sizeof(c));
    strncpy(c.email, "player@test.com", sizeof(c.email) - 1);
    strncpy(c.hash, "hash123", sizeof(c.hash) - 1);
    strncpy(c.name, "PlayerOne", sizeof(c.name) - 1);
    strncpy(c.avatar, "avatar1", sizeof(c.avatar) - 1);

    UserId uid = 0;
    assert(store_account_create(s, &c, &uid) == OK);
    assert(uid > 0);

    ProfileStats stats;
    memset(&stats, 0xFF, sizeof(stats));
    assert(store_profile_stats(s, uid, &stats) == OK);
    assert(stats.matches == 0);
    assert(stats.wins == 0);
    assert(stats.teamWins == 0);
    assert(stats.plus4 == 0);
    assert(stats.unoCalls == 0);
    assert(stats.streak == 0);
    assert(stats.bestStreak == 0);
    assert(stats.peakElo == 1500);

    /* Non-existent user returns NOT_FOUND */
    assert(store_profile_stats(s, 999999, &stats) == NOT_FOUND);

    store_close(s);
    cleanup_file(db_path);
}

static void test_profile_stats_finalize_accumulation_and_overwrite(void)
{
    const char *db_path = "/tmp/test_uno_profile_stats_accum.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);

    AccountCreate c;
    memset(&c, 0, sizeof(c));
    strncpy(c.email, "player@test.com", sizeof(c.email) - 1);
    strncpy(c.hash, "hash123", sizeof(c.hash) - 1);
    strncpy(c.name, "PlayerOne", sizeof(c.name) - 1);
    strncpy(c.avatar, "avatar1", sizeof(c.avatar) - 1);

    UserId uid = 0;
    assert(store_account_create(s, &c, &uid) == OK);
    assert(uid > 0);

    PlayerSpec players[4] = {
        {uid, "PlayerOne", "avatar1", EASY, false},
        {0, "Bot1", "avatar_bot", EASY, true},
        {0, "Bot2", "avatar_bot", EASY, true},
        {0, "Bot3", "avatar_bot", EASY, true}
    };

    /* Match 1: Win */
    MatchId mid1 = "00000000-0000-0000-0000-000000000011";
    assert(store_match_start(s, (const MatchId *)&mid1, players, CLASSIC, true) == OK);

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

    assert(store_finalize(s, &res1, deltas1) == OK);

    ProfileStats stats;
    assert(store_profile_stats(s, uid, &stats) == OK);
    assert(stats.matches == 1);
    assert(stats.wins == 1);
    assert(stats.streak == 1);
    assert(stats.bestStreak == 1);
    assert(stats.peakElo == 1516);

    /* Verify badge in achievements table */
    sqlite3 *db = store_get_db(s);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT badge, match_id FROM achievements WHERE user_id = ?;", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)uid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == BADGE_FIRST_WIN);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 1), mid1) == 0);
    sqlite3_finalize(stmt);

    /* Match 2: Loss */
    MatchId mid2 = "00000000-0000-0000-0000-000000000012";
    assert(store_match_start(s, (const MatchId *)&mid2, players, CLASSIC, true) == OK);

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

    assert(store_finalize(s, &res2, deltas2) == OK);

    assert(store_profile_stats(s, uid, &stats) == OK);
    assert(stats.matches == 2);
    assert(stats.wins == 1);
    assert(stats.streak == -1);     /* resets to negative! */
    assert(stats.bestStreak == 1); /* preserved! */
    assert(stats.peakElo == 1516);  /* NOT 1516+1516=3032! */

    /* Match 3: Win with peak ELO 1805 */
    MatchId mid3 = "00000000-0000-0000-0000-000000000013";
    assert(store_match_start(s, (const MatchId *)&mid3, players, CLASSIC, true) == OK);

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

    assert(store_finalize(s, &res3, deltas3) == OK);

    assert(store_profile_stats(s, uid, &stats) == OK);
    assert(stats.matches == 3);
    assert(stats.wins == 2);
    assert(stats.peakElo == 1805);

    /* Verify BADGE_DIAMOND is unlocked in achievements */
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM achievements WHERE user_id = ? AND badge = ?;", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)uid);
    sqlite3_bind_int(stmt, 2, BADGE_DIAMOND);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    store_close(s);
    cleanup_file(db_path);
}

static int db_query_int(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

static void test_upgrade_from_v1_database(void)
{
    const char *db_path = "/tmp/test_uno_store_upgrade_v1.db";
    cleanup_file(db_path);

    /* Build a v1-shaped file: an existing account, the old key/progress
     * achievements table, no profile_stats rows and user_version 0. */
    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);
    AccountCreate c;
    memset(&c, 0, sizeof(c));
    strncpy(c.email, "legacy@test.com", sizeof(c.email) - 1);
    strncpy(c.hash, "hash", sizeof(c.hash) - 1);
    strncpy(c.name, "Legacy", sizeof(c.name) - 1);
    strncpy(c.avatar, "fox", sizeof(c.avatar) - 1);
    UserId uid = 0;
    assert(store_account_create(s, &c, &uid) == OK);
    sqlite3 *db = store_get_db(s);
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
    store_close(s);

    /* Reopening must upgrade instead of failing to prepare statements. */
    s = NULL;
    assert(store_open(&s, db_path) == OK);
    db = store_get_db(s);
    assert(db_query_int(db, "PRAGMA user_version;") == 2);
    assert(db_query_int(db, "SELECT count(*) FROM pragma_table_info('achievements') WHERE name = 'badge';") == 1);
    assert(db_query_int(db, "SELECT count(*) FROM pragma_table_info('achievements') WHERE name = 'key';") == 0);

    ProfileStats stats;
    assert(store_profile_stats(s, uid, &stats) == OK);
    assert(stats.matches == 0);
    assert(stats.peakElo == 1620);
    store_close(s);

    /* A second open of a v2 file is a no-op and keeps data. */
    s = NULL;
    assert(store_open(&s, db_path) == OK);
    assert(store_profile_stats(s, uid, &stats) == OK);
    assert(stats.peakElo == 1620);
    store_close(s);
    cleanup_file(db_path);
}

static void test_account_create_is_atomic(void)
{
    const char *db_path = "/tmp/test_uno_account_atomic.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);
    sqlite3 *db = store_get_db(s);

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
    assert(store_account_create(s, &c, &uid) == STORAGE_ERROR);
    assert(db_query_int(db, "SELECT count(*) FROM accounts;") == 0);

    /* Once storage recovers, the same email can register. */
    assert(sqlite3_exec(db, "DROP TRIGGER fail_profile;", NULL, NULL, NULL) == SQLITE_OK);
    assert(store_account_create(s, &c, &uid) == OK);
    assert(db_query_int(db, "SELECT count(*) FROM accounts;") == 1);
    assert(db_query_int(db, "SELECT count(*) FROM profile_stats;") == 1);

    /* Duplicate email still maps to BAD_REQUEST and leaves no open transaction. */
    assert(store_account_create(s, &c, &uid) == BAD_REQUEST);
    assert(sqlite3_get_autocommit(db) != 0);

    store_close(s);
    cleanup_file(db_path);
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

static MatchEvent abortEvent(void)
{
    MatchEvent event = {.seq = ABORT_EVENT_SEQ, .revision = ABORT_EVENT_SEQ, .actor = -1};
    strncpy(event.type, "MATCH_ABORTED", sizeof(event.type) - 1);
    strncpy(event.payload, "{}", sizeof(event.payload) - 1);
    return event;
}

static int matchStatusOf(sqlite3 *db, const char *matchId)
{
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT status FROM matches WHERE match_id = ?;",
                              -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, matchId, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int status = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return status;
}

static void test_abort_atomic_and_idempotent(void)
{
    const char *db_path = "/tmp/test_uno_store_abort.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);
    sqlite3 *db = store_get_db(s);

    MatchId mid = "00000000-0000-0000-0000-000000000020";
    PlayerSpec players[4] = {
        {101, "Alice", "a1", EASY, false},
        {0, "Bot1", "bot", EASY, true},
        {0, "Bot2", "bot", EASY, true},
        {0, "Bot3", "bot", EASY, true}
    };
    assert(store_match_start(s, (const MatchId *)&mid, players, CLASSIC, false) == OK);
    MatchEvent event = abortEvent();

    /* A failed event insert must not leave the match marked ABORTED. */
    assert(sqlite3_exec(db,
                        "CREATE TEMP TRIGGER fail_abort_event BEFORE INSERT ON match_events "
                        "BEGIN SELECT RAISE(ABORT, 'forced'); END;",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(store_abort(s, (const MatchId *)&mid, &event, ABORT_DURATION_MS) == STORAGE_ERROR);
    assert(matchStatusOf(db, mid) == MATCH_ACTIVE);
    assert(db_query_int(db, "SELECT count(*) FROM match_events;") == 0);
    assert(sqlite3_get_autocommit(db) != 0);
    assert(sqlite3_exec(db, "DROP TRIGGER fail_abort_event;", NULL, NULL, NULL) == SQLITE_OK);

    /* A retried abort after success is OK and writes nothing more. */
    assert(store_abort(s, (const MatchId *)&mid, &event, ABORT_DURATION_MS) == OK);
    assert(store_abort(s, (const MatchId *)&mid, &event, ABORT_DURATION_MS) == OK);
    assert(matchStatusOf(db, mid) == MATCH_ABORTED);
    assert(db_query_int(db, "SELECT count(*) FROM match_events "
                            "WHERE type = 'MATCH_ABORTED';") == 1);
    assert(db_query_int(db, "SELECT duration_ms FROM matches;") == ABORT_DURATION_MS);

    MatchId unknown = "99999999-9999-9999-9999-999999999998";
    assert(store_abort(s, (const MatchId *)&unknown, &event, 0) == NOT_FOUND);

    /* A finalized match cannot be turned into an aborted one. */
    MatchId finished = "00000000-0000-0000-0000-000000000021";
    assert(store_match_start(s, (const MatchId *)&finished, players, CLASSIC, false) == OK);
    MatchResult res;
    memset(&res, 0, sizeof(res));
    strncpy(res.matchId, finished, sizeof(res.matchId) - 1);
    res.status = MATCH_FINALIZED;
    res.winnerSeat = 0;
    res.winnerTeam = -1;
    res.players[0].userId = 101;
    assert(store_finalize(s, &res, NULL) == OK);
    assert(store_abort(s, (const MatchId *)&finished, &event, 0) == INVALID_ACTION);
    assert(matchStatusOf(db, finished) == MATCH_FINALIZED);
    assert(sqlite3_get_autocommit(db) != 0);

    store_close(s);
    cleanup_file(db_path);
}

static void test_ranked_finalize_updates_elo_once(void)
{
    const char *db_path = "/tmp/test_uno_store_ranked.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);
    sqlite3 *db = store_get_db(s);

    AccountCreate c;
    memset(&c, 0, sizeof(c));
    strncpy(c.hash, "hash", sizeof(c.hash) - 1);
    strncpy(c.name, "Ranked", sizeof(c.name) - 1);
    strncpy(c.avatar, "fox", sizeof(c.avatar) - 1);
    UserId winner = 0, loser = 0;
    strncpy(c.email, "winner@test.com", sizeof(c.email) - 1);
    assert(store_account_create(s, &c, &winner) == OK);
    strncpy(c.email, "loser@test.com", sizeof(c.email) - 1);
    assert(store_account_create(s, &c, &loser) == OK);

    MatchId mid = "00000000-0000-0000-0000-000000000022";
    PlayerSpec players[4] = {
        {winner, "Ranked", "fox", EASY, false},
        {loser, "Ranked", "fox", EASY, false},
        {0, "Bot1", "bot", EASY, true},
        {0, "Bot2", "bot", EASY, true}
    };
    assert(store_match_start(s, (const MatchId *)&mid, players, CLASSIC, true) == OK);
    assert(db_query_int(db, "SELECT ranked FROM matches;") == 1);

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
    assert(store_finalize(s, &res, NULL) == OK);
    assert(store_account_get_by_id(s, winner, &account) == OK);
    assert(account.elo == RANKED_WINNER_ELO);
    assert(store_account_get_by_id(s, loser, &account) == OK);
    assert(account.elo == RANKED_LOSER_ELO);

    /* Replaying the same finalize (storage retry) must not apply ELO twice. */
    assert(store_finalize(s, &res, NULL) == OK);
    assert(store_account_get_by_id(s, winner, &account) == OK);
    assert(account.elo == RANKED_WINNER_ELO);
    assert(store_account_get_by_id(s, loser, &account) == OK);
    assert(account.elo == RANKED_LOSER_ELO);

    store_close(s);
    cleanup_file(db_path);
}

static void test_leaderboard_order(void)
{
    const char *db_path = "/tmp/test_uno_store_leaderboard.db";
    cleanup_file(db_path);

    Store *s = NULL;
    assert(store_open(&s, db_path) == OK);
    sqlite3 *db = store_get_db(s);

    LeaderboardEntry entries[STORE_LEADERBOARD_MAX];
    size_t count = 99;
    assert(store_leaderboard(s, entries, &count) == OK);
    assert(count == 0);

    UserId ids[LEADERBOARD_ACCOUNTS];
    for (int i = 0; i < LEADERBOARD_ACCOUNTS; i++) {
        AccountCreate c;
        memset(&c, 0, sizeof(c));
        snprintf(c.email, sizeof(c.email), "rank%d@test.com", i);
        snprintf(c.name, sizeof(c.name), "Rank%d", i);
        snprintf(c.avatar, sizeof(c.avatar), "av%d", i);
        strncpy(c.hash, "hash", sizeof(c.hash) - 1);
        assert(store_account_create(s, &c, &ids[i]) == OK);

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

    assert(store_leaderboard(s, entries, &count) == OK);
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

    store_close(s);
    cleanup_file(db_path);
}

int main(void)
{
    test_abort_atomic_and_idempotent();
    test_ranked_finalize_updates_elo_once();
    test_leaderboard_order();
    test_table_creation_and_migration();
    test_upgrade_from_v1_database();
    test_account_create_is_atomic();
    test_store_match_start_persisting();
    test_store_events_uniqueness_and_idempotency();
    test_store_finalize_idempotency_and_no_double_elo();
    test_crash_recovery_aborted();
    test_transaction_rollback_on_failure();
    test_store_export_match();
    test_profile_stats_initial_state();
    test_profile_stats_finalize_accumulation_and_overwrite();

    puts("store: OK");
    return 0;
}
