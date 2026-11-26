#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <json-c/json.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Store {
    sqlite3 *db;
    sqlite3_stmt *matchSelectStmt;
    sqlite3_stmt *matchInsertStmt;
    sqlite3_stmt *matchUpdateStatusStmt;
    sqlite3_stmt *playerInsertStmt;
    sqlite3_stmt *playerSelectBySeatStmt;
    sqlite3_stmt *playerSelectAllStmt;
    sqlite3_stmt *playerUpdateStmt;
    sqlite3_stmt *eventSelectStmt;
    sqlite3_stmt *eventInsertStmt;
    sqlite3_stmt *eventSelectAllStmt;
    sqlite3_stmt *accountUpdateEloStmt;
    sqlite3_stmt *achievementInsertStmt;
    sqlite3_stmt *accountInsertStmt;
    sqlite3_stmt *accountSelectByEmailStmt;
    sqlite3_stmt *accountSelectByIdStmt;
    sqlite3_stmt *profileStatsInitStmt;
    sqlite3_stmt *profileStatsSelectStmt;
    sqlite3_stmt *profileStatsUpdateStmt;
    sqlite3_stmt *profileStatsInsertFallbackStmt;
    sqlite3_stmt *matchAbortStmt;
    sqlite3_stmt *leaderboardStmt;
    sqlite3_stmt *friendLinkSelectStmt;
    sqlite3_stmt *friendInsertStmt;
    sqlite3_stmt *friendAcceptStmt;
    sqlite3_stmt *friendRejectStmt;
    sqlite3_stmt *friendListStmt;
    sqlite3_stmt *profileBadgesStmt;
    sqlite3_stmt *profileModesStmt;
    sqlite3_stmt *profileHistoryStmt;
    sqlite3_stmt *profileUpdateStmt;
};

/* friends.status values; a rejected request is deleted, so it has no value. */
enum { FRIEND_STATUS_PENDING = 0, FRIEND_STATUS_ACCEPTED = 1 };

enum { MS_PER_SECOND = 1000 };

/* A seat wins by team in team modes (?3, ?4 = TWO_V_TWO, SIDE) and by seat
 * otherwise; -1 means no winner. A player who left never wins, matching how
 * profileCompute counts profile_stats.wins. */
#define PROFILE_WON_SQL \
    "(mp.player_left = 0 AND CASE WHEN m.mode IN (?3, ?4) " \
    "THEN m.winner_team >= 0 AND mp.team = m.winner_team " \
    "ELSE m.winner_seat >= 0 AND mp.seat = m.winner_seat END)"

/* ?1 = user id, ?2 = MATCH_FINALIZED: only finished matches reach a profile. */
#define PROFILE_MATCHES_SQL \
    " FROM match_players mp JOIN matches m ON m.match_id = mp.match_id " \
    "WHERE mp.user_id = ?1 AND m.status = ?2"

static const char *EMBEDDED_SCHEMA_SQL =
    "PRAGMA foreign_keys = ON;\n"
    "CREATE TABLE IF NOT EXISTS matches (\n"
    "    match_id TEXT PRIMARY KEY,\n"
    "    mode INTEGER NOT NULL,\n"
    "    ranked INTEGER NOT NULL,\n"
    "    status INTEGER NOT NULL,\n"
    "    winner_seat INTEGER NOT NULL DEFAULT -1,\n"
    "    winner_team INTEGER NOT NULL DEFAULT -1,\n"
    "    duration_ms INTEGER NOT NULL DEFAULT 0,\n"
    "    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),\n"
    "    ended_at INTEGER NOT NULL DEFAULT 0\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS match_players (\n"
    "    match_id TEXT NOT NULL REFERENCES matches(match_id) ON DELETE CASCADE,\n"
    "    seat INTEGER NOT NULL,\n"
    "    user_id INTEGER NOT NULL DEFAULT 0,\n"
    "    name TEXT NOT NULL DEFAULT '',\n"
    "    avatar TEXT NOT NULL DEFAULT '',\n"
    "    bot_level INTEGER NOT NULL DEFAULT 0,\n"
    "    is_bot INTEGER NOT NULL DEFAULT 0,\n"
    "    team INTEGER NOT NULL DEFAULT -1,\n"
    "    points INTEGER NOT NULL DEFAULT 0,\n"
    "    elo_before INTEGER NOT NULL DEFAULT 1500,\n"
    "    elo_after INTEGER NOT NULL DEFAULT 1500,\n"
    "    player_left INTEGER NOT NULL DEFAULT 0,\n"
    "    played INTEGER NOT NULL DEFAULT 0,\n"
    "    attacks INTEGER NOT NULL DEFAULT 0,\n"
    "    uno_calls INTEGER NOT NULL DEFAULT 0,\n"
    "    penalties INTEGER NOT NULL DEFAULT 0,\n"
    "    plus4 INTEGER NOT NULL DEFAULT 0,\n"
    "    PRIMARY KEY (match_id, seat)\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS match_events (\n"
    "    match_id TEXT NOT NULL REFERENCES matches(match_id) ON DELETE CASCADE,\n"
    "    seq INTEGER NOT NULL,\n"
    "    revision INTEGER NOT NULL,\n"
    "    actor INTEGER NOT NULL,\n"
    "    type TEXT NOT NULL,\n"
    "    payload TEXT NOT NULL,\n"
    "    PRIMARY KEY (match_id, seq)\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS accounts (\n"
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "    email TEXT UNIQUE NOT NULL,\n"
    "    password_hash TEXT NOT NULL,\n"
    "    name TEXT NOT NULL,\n"
    "    avatar TEXT NOT NULL,\n"
    "    elo INTEGER NOT NULL DEFAULT 1500,\n"
    "    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS friends (\n"
    "    user_id INTEGER NOT NULL,\n"
    "    friend_id INTEGER NOT NULL,\n"
    "    status INTEGER NOT NULL DEFAULT 0,\n"
    "    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),\n"
    "    PRIMARY KEY (user_id, friend_id)\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS profile_stats (\n"
    "    user_id INTEGER PRIMARY KEY REFERENCES accounts(id),\n"
    "    matches INTEGER NOT NULL DEFAULT 0,\n"
    "    wins INTEGER NOT NULL DEFAULT 0,\n"
    "    team_wins INTEGER NOT NULL DEFAULT 0,\n"
    "    plus4 INTEGER NOT NULL DEFAULT 0,\n"
    "    uno_calls INTEGER NOT NULL DEFAULT 0,\n"
    "    streak INTEGER NOT NULL DEFAULT 0,\n"
    "    best_streak INTEGER NOT NULL DEFAULT 0,\n"
    "    peak_elo INTEGER NOT NULL DEFAULT 1500\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS achievements (\n"
    "    user_id INTEGER NOT NULL,\n"
    "    badge INTEGER NOT NULL,\n"
    "    unlocked_at INTEGER NOT NULL,\n"
    "    match_id TEXT NOT NULL,\n"
    "    PRIMARY KEY (user_id, badge)\n"
    ");\n";

/* Every query is compiled once at open time: the poll thread never pays for SQL
 * parsing during a match, and a schema/SQL mismatch fails storeOpen early. */
static Status prepareStatements(Store *store) {
    const char *matchSelectSql =
        "SELECT mode, ranked, status, winner_seat, winner_team, duration_ms, created_at FROM matches WHERE match_id = ?;";
    if (sqlite3_prepare_v2(store->db, matchSelectSql, -1, &store->matchSelectStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *matchInsertSql =
        "INSERT INTO matches (match_id, mode, ranked, status, winner_seat, winner_team, duration_ms, created_at) "
        "VALUES (?, ?, ?, ?, -1, -1, 0, ?);";
    if (sqlite3_prepare_v2(store->db, matchInsertSql, -1, &store->matchInsertStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *matchUpdateStatusSql =
        "UPDATE matches SET status = ?, winner_seat = ?, winner_team = ?, duration_ms = ?, "
        "ended_at = ? WHERE match_id = ?;";
    if (sqlite3_prepare_v2(store->db, matchUpdateStatusSql, -1, &store->matchUpdateStatusStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *playerInsertSql =
        "INSERT INTO match_players (match_id, seat, user_id, name, avatar, bot_level, is_bot, team, points, elo_before, elo_after) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, -1, 0, 1500, 1500);";
    if (sqlite3_prepare_v2(store->db, playerInsertSql, -1, &store->playerInsertStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *playerSelectBySeatSql =
        "SELECT seat, user_id, name, avatar, bot_level, is_bot, team, points, elo_before, elo_after, "
        "player_left, played, attacks, uno_calls, penalties, plus4 "
        "FROM match_players WHERE match_id = ? AND seat = ?;";
    if (sqlite3_prepare_v2(store->db, playerSelectBySeatSql, -1, &store->playerSelectBySeatStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *playerSelectAllSql =
        "SELECT seat, user_id, name, avatar, bot_level, is_bot, team, points, elo_before, elo_after, "
        "player_left, played, attacks, uno_calls, penalties, plus4 "
        "FROM match_players WHERE match_id = ? ORDER BY seat ASC;";
    if (sqlite3_prepare_v2(store->db, playerSelectAllSql, -1, &store->playerSelectAllStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *playerUpdateSql =
        "UPDATE match_players SET team = ?, points = ?, elo_before = ?, elo_after = ?, "
        "player_left = ?, played = ?, attacks = ?, uno_calls = ?, penalties = ?, plus4 = ? "
        "WHERE match_id = ? AND seat = ?;";
    if (sqlite3_prepare_v2(store->db, playerUpdateSql, -1, &store->playerUpdateStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *eventSelectSql =
        "SELECT revision, actor, type, payload FROM match_events WHERE match_id = ? AND seq = ?;";
    if (sqlite3_prepare_v2(store->db, eventSelectSql, -1, &store->eventSelectStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *eventInsertSql =
        "INSERT INTO match_events (match_id, seq, revision, actor, type, payload) VALUES (?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(store->db, eventInsertSql, -1, &store->eventInsertStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *eventSelectAllSql =
        "SELECT seq, revision, actor, type, payload FROM match_events WHERE match_id = ? ORDER BY seq ASC;";
    if (sqlite3_prepare_v2(store->db, eventSelectAllSql, -1, &store->eventSelectAllStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *accountUpdateEloSql = "UPDATE accounts SET elo = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(store->db, accountUpdateEloSql, -1, &store->accountUpdateEloStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *achievementInsertSql =
        "INSERT OR IGNORE INTO achievements (user_id, badge, unlocked_at, match_id) VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(store->db, achievementInsertSql, -1, &store->achievementInsertStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *profileStatsInitSql =
        "INSERT INTO profile_stats (user_id, peak_elo) VALUES (?, 1500);";
    if (sqlite3_prepare_v2(store->db, profileStatsInitSql, -1, &store->profileStatsInitStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *profileStatsSelectSql =
        "SELECT matches, wins, team_wins, plus4, uno_calls, streak, best_streak, peak_elo "
        "FROM profile_stats WHERE user_id = ?;";
    if (sqlite3_prepare_v2(store->db, profileStatsSelectSql, -1, &store->profileStatsSelectStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *profileStatsUpdateSql =
        "UPDATE profile_stats "
        "SET matches = matches + ?, "
        "    wins = wins + ?, "
        "    team_wins = team_wins + ?, "
        "    plus4 = plus4 + ?, "
        "    uno_calls = uno_calls + ?, "
        "    streak = ?, "
        "    best_streak = ?, "
        "    peak_elo = ? "
        "WHERE user_id = ?;";
    if (sqlite3_prepare_v2(store->db, profileStatsUpdateSql, -1, &store->profileStatsUpdateStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *profileStatsInsertFallbackSql =
        "INSERT INTO profile_stats (user_id, matches, wins, team_wins, plus4, uno_calls, streak, best_streak, peak_elo) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(store->db, profileStatsInsertFallbackSql, -1,
                           &store->profileStatsInsertFallbackStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *accountInsertSql =
        "INSERT INTO accounts (email, password_hash, name, avatar, elo, created_at) "
        "VALUES (?, ?, ?, ?, 1500, strftime('%s', 'now'));";
    if (sqlite3_prepare_v2(store->db, accountInsertSql, -1, &store->accountInsertStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *accountSelectByEmailSql =
        "SELECT id, email, password_hash, name, avatar, elo FROM accounts WHERE email = ?;";
    if (sqlite3_prepare_v2(store->db, accountSelectByEmailSql, -1, &store->accountSelectByEmailStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *accountSelectByIdSql =
        "SELECT id, email, password_hash, name, avatar, elo FROM accounts WHERE id = ?;";
    if (sqlite3_prepare_v2(store->db, accountSelectByIdSql, -1, &store->accountSelectByIdStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    /* Guarded by status so a FINALIZED match can never be overwritten. */
    const char *matchAbortSql =
        "UPDATE matches SET status = ?, duration_ms = ? "
        "WHERE match_id = ? AND status = ?;";
    if (sqlite3_prepare_v2(store->db, matchAbortSql, -1, &store->matchAbortStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *leaderboardSql =
        "SELECT id, name, avatar, elo FROM accounts "
        "ORDER BY elo DESC, id ASC LIMIT ?;";
    if (sqlite3_prepare_v2(store->db, leaderboardSql, -1, &store->leaderboardStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *friendLinkSelectSql =
        "SELECT status FROM friends "
        "WHERE (user_id = ?1 AND friend_id = ?2) OR (user_id = ?2 AND friend_id = ?1);";
    if (sqlite3_prepare_v2(store->db, friendLinkSelectSql, -1, &store->friendLinkSelectStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *friendInsertSql =
        "INSERT INTO friends (user_id, friend_id, status, created_at) "
        "VALUES (?, ?, ?, strftime('%s', 'now'));";
    if (sqlite3_prepare_v2(store->db, friendInsertSql, -1, &store->friendInsertStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    /* Both reply statements only touch a PENDING row in the requester -> me
     * direction, so the sender cannot answer its own request. */
    const char *friendAcceptSql =
        "UPDATE friends SET status = ?1 "
        "WHERE user_id = ?2 AND friend_id = ?3 AND status = ?4;";
    if (sqlite3_prepare_v2(store->db, friendAcceptSql, -1, &store->friendAcceptStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *friendRejectSql =
        "DELETE FROM friends WHERE user_id = ?1 AND friend_id = ?2 AND status = ?3;";
    if (sqlite3_prepare_v2(store->db, friendRejectSql, -1, &store->friendRejectStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    /* Accepted (status 1) before pending (0): a flood of requests must not
     * push real friends past the LIMIT. */
    const char *friendListSql =
        "SELECT a.id, a.name, a.avatar, a.elo, f.user_id, f.status FROM friends f "
        "JOIN accounts a ON a.id = CASE WHEN f.user_id = ?1 THEN f.friend_id ELSE f.user_id END "
        "WHERE f.user_id = ?1 OR f.friend_id = ?1 "
        "ORDER BY f.status DESC, a.id ASC LIMIT ?2;";
    if (sqlite3_prepare_v2(store->db, friendListSql, -1, &store->friendListStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *profileBadgesSql = "SELECT badge FROM achievements WHERE user_id = ?;";
    if (sqlite3_prepare_v2(store->db, profileBadgesSql, -1, &store->profileBadgesStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *profileModesSql =
        "SELECT m.mode, count(*), sum(" PROFILE_WON_SQL ")"
        PROFILE_MATCHES_SQL " GROUP BY m.mode;";
    if (sqlite3_prepare_v2(store->db, profileModesSql, -1, &store->profileModesStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    /* rowid breaks ties between matches that ended in the same second. */
    const char *profileHistorySql =
        "SELECT m.match_id, m.mode, " PROFILE_WON_SQL ", "
        "mp.elo_after - mp.elo_before, m.ended_at"
        PROFILE_MATCHES_SQL " ORDER BY m.ended_at DESC, m.rowid DESC LIMIT ?5;";
    if (sqlite3_prepare_v2(store->db, profileHistorySql, -1, &store->profileHistoryStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *profileUpdateSql = "UPDATE accounts SET name = ?, avatar = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(store->db, profileUpdateSql, -1, &store->profileUpdateStmt,
                           NULL) != SQLITE_OK) return STORAGE_ERROR;

    return OK;
}

static void finalizeStatements(Store *store) {
    if (!store) return;
    sqlite3_stmt **statements[] = {
        &store->matchSelectStmt, &store->matchInsertStmt, &store->matchUpdateStatusStmt,
        &store->playerInsertStmt, &store->playerSelectBySeatStmt, &store->playerSelectAllStmt,
        &store->playerUpdateStmt, &store->eventSelectStmt, &store->eventInsertStmt,
        &store->eventSelectAllStmt, &store->accountUpdateEloStmt, &store->achievementInsertStmt,
        &store->accountInsertStmt, &store->accountSelectByEmailStmt, &store->accountSelectByIdStmt,
        &store->profileStatsInitStmt, &store->profileStatsSelectStmt,
        &store->profileStatsUpdateStmt, &store->profileStatsInsertFallbackStmt,
        &store->matchAbortStmt, &store->leaderboardStmt, &store->friendLinkSelectStmt,
        &store->friendInsertStmt, &store->friendAcceptStmt, &store->friendRejectStmt,
        &store->friendListStmt, &store->profileBadgesStmt, &store->profileModesStmt,
        &store->profileHistoryStmt, &store->profileUpdateStmt
    };
    for (size_t i = 0; i < sizeof statements / sizeof statements[0]; i++) {
        /* sqlite3_finalize(NULL) is a no-op, so a partly prepared store is fine. */
        sqlite3_finalize(*statements[i]);
        *statements[i] = NULL;
    }
}

/* Bump whenever EMBEDDED_SCHEMA_SQL changes in a way CREATE TABLE IF NOT EXISTS
 * cannot apply to an existing database file. Tracked in PRAGMA user_version.
 *   1: original schema (achievements as key/progress rows)
 *   2: profile_stats table + achievements(user_id, badge, unlocked_at, match_id)
 *   3: matches.ended_at (finalize time, Unix seconds) for profile history */
#define STORE_SCHEMA_VERSION 3
#define STORE_SCHEMA_END_TIME_VERSION 3

static int queryInt(sqlite3 *db, const char *sql, int *out) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    int resultCode = sqlite3_step(stmt);
    *out = (resultCode == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return (resultCode == SQLITE_ROW || resultCode == SQLITE_DONE) ? 0 : -1;
}

/* Additive only: a v2 file keeps every row. CREATE TABLE IF NOT EXISTS already
 * gave new files the column, so only add it when it is missing. */
static Status addMatchEndTime(sqlite3 *db) {
    int hasColumn = 0;
    if (queryInt(db, "SELECT count(*) FROM pragma_table_info('matches') WHERE name = 'ended_at';",
                  &hasColumn) != 0) return STORAGE_ERROR;
    if (!hasColumn &&
        sqlite3_exec(db, "ALTER TABLE matches ADD COLUMN ended_at INTEGER NOT NULL DEFAULT 0;",
                     NULL, NULL, NULL) != SQLITE_OK) return STORAGE_ERROR;

    /* Matches finalized before v3 never recorded an end time; start plus
     * played duration is the closest value the old rows can give. */
    char sql[160];
    snprintf(sql, sizeof(sql),
             "UPDATE matches SET ended_at = created_at + duration_ms / %d "
             "WHERE status = %d AND ended_at = 0;",
             MS_PER_SECOND, (int)MATCH_FINALIZED);
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? OK : STORAGE_ERROR;
}

static Status migrateSchema(Store *store) {
    if (sqlite3_exec(store->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }

    int version = 0;
    if (queryInt(store->db, "PRAGMA user_version;", &version) != 0) goto fail;

    if (version < 2) {
        /* v1 stored badge progress as key/value rows that the v2 model derives
         * from profile_stats instead, so the old rows carry nothing to keep. */
        int legacy = 0;
        if (queryInt(store->db,
                      "SELECT count(*) FROM pragma_table_info('achievements') WHERE name = 'key';",
                      &legacy) != 0) goto fail;
        if (legacy > 0 &&
            sqlite3_exec(store->db, "DROP TABLE achievements;", NULL, NULL,
                         NULL) != SQLITE_OK) goto fail;
    }

    if (sqlite3_exec(store->db, EMBEDDED_SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) goto fail;

    if (version < 2) {
        /* Accounts created before v2 have no profile_stats row yet. */
        if (sqlite3_exec(store->db,
                         "INSERT OR IGNORE INTO profile_stats (user_id, peak_elo) "
                         "SELECT id, MAX(elo, 1500) FROM accounts;",
                         NULL, NULL, NULL) != SQLITE_OK) goto fail;
    }

    if (version < STORE_SCHEMA_END_TIME_VERSION && addMatchEndTime(store->db) != OK) goto fail;

    if (version < STORE_SCHEMA_VERSION) {
        char sql[64];
        snprintf(sql, sizeof(sql), "PRAGMA user_version = %d;", STORE_SCHEMA_VERSION);
        if (sqlite3_exec(store->db, sql, NULL, NULL, NULL) != SQLITE_OK) goto fail;
    }

    if (sqlite3_exec(store->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    return OK;

fail:
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    return STORAGE_ERROR;
}

Status storeOpen(Store **out, const char *path) {
    if (!out || !path) return BAD_REQUEST;
    *out = NULL;

    Store *store = calloc(1, sizeof(Store));
    if (!store) return STORAGE_ERROR;

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path, &store->db, flags, NULL) != SQLITE_OK) {
        free(store);
        return STORAGE_ERROR;
    }

    sqlite3_busy_timeout(store->db, 100);

    /* Configure SQLite PRAGMAs */
    char *errmsg = NULL;
    if (sqlite3_exec(store->db, "PRAGMA foreign_keys = ON;", NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
        storeClose(store);
        return STORAGE_ERROR;
    }

    /* Set WAL journal mode (for file DBs; in-memory returns "memory") */
    if (sqlite3_exec(store->db, "PRAGMA journal_mode = WAL;", NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
    }

    if (sqlite3_exec(store->db, "PRAGMA busy_timeout = 100;", NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
    }

    /* Execute Schema Migration */
    if (migrateSchema(store) != OK) {
        storeClose(store);
        return STORAGE_ERROR;
    }

    /* Crash recovery: convert all MATCH_ACTIVE matches to MATCH_ABORTED */
    const char *recoverySql = "UPDATE matches SET status = ? WHERE status = ?;";
    sqlite3_stmt *recoveryStmt = NULL;
    if (sqlite3_prepare_v2(store->db, recoverySql, -1, &recoveryStmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(recoveryStmt, 1, MATCH_ABORTED);
        sqlite3_bind_int(recoveryStmt, 2, MATCH_ACTIVE);
        sqlite3_step(recoveryStmt);
        sqlite3_finalize(recoveryStmt);
    }

    if (prepareStatements(store) != OK) {
        storeClose(store);
        return STORAGE_ERROR;
    }

    *out = store;
    return OK;
}

void storeClose(Store *store) {
    if (!store) return;
    finalizeStatements(store);
    if (store->db) {
        sqlite3_close(store->db);
        store->db = NULL;
    }
    free(store);
}

sqlite3 *storeGetDb(Store *store) {
    return store ? store->db : NULL;
}

Status storeMatchStart(Store *store, const MatchId *matchId, const PlayerSpec players[4],
                         Mode mode, bool ranked) {
    if (!store || !matchId || !players) return BAD_REQUEST;
    const char *matchIdText = (const char *)matchId;
    if (strlen(matchIdText) == 0) return BAD_REQUEST;

    sqlite3_stmt *stmt = store->matchSelectStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, matchIdText, -1, SQLITE_STATIC);

    int stepResult = sqlite3_step(stmt);
    if (stepResult == SQLITE_ROW) {
        /* Match already exists: check idempotency */
        Mode existingMode = (Mode)sqlite3_column_int(stmt, 0);
        bool existingRanked = sqlite3_column_int(stmt, 1) != 0;
        MatchStatus existingStatus = (MatchStatus)sqlite3_column_int(stmt, 2);
        sqlite3_reset(stmt);

        if (existingMode != mode || existingRanked != ranked || existingStatus != MATCH_ACTIVE) {
            return INVALID_ACTION;
        }

        /* Check players */
        for (int i = 0; i < 4; i++) {
            sqlite3_stmt *pstmt = store->playerSelectBySeatStmt;
            sqlite3_reset(pstmt);
            sqlite3_clear_bindings(pstmt);
            sqlite3_bind_text(pstmt, 1, matchIdText, -1, SQLITE_STATIC);
            sqlite3_bind_int(pstmt, 2, i);
            if (sqlite3_step(pstmt) != SQLITE_ROW) {
                sqlite3_reset(pstmt);
                return INVALID_ACTION;
            }
            UserId userId = (UserId)sqlite3_column_int64(pstmt, 1);
            const char *name = (const char *)sqlite3_column_text(pstmt, 2);
            int isBot = sqlite3_column_int(pstmt, 5);
            int botLevel = sqlite3_column_int(pstmt, 4);

            if (userId != players[i].userId ||
                strcmp(name ? name : "", players[i].name) != 0 ||
                isBot != (players[i].isBot ? 1 : 0) ||
                botLevel != (int)players[i].botLevel) {
                sqlite3_reset(pstmt);
                return INVALID_ACTION;
            }
            sqlite3_reset(pstmt);
        }
        return OK;
    }
    sqlite3_reset(stmt);

    /* Begin atomic transaction */
    if (sqlite3_exec(store->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }

    uint64_t nowSec = (uint64_t)time(NULL);
    stmt = store->matchInsertStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, matchIdText, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)mode);
    sqlite3_bind_int(stmt, 3, ranked ? 1 : 0);
    sqlite3_bind_int(stmt, 4, (int)MATCH_ACTIVE);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)nowSec);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_reset(stmt);
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }
    sqlite3_reset(stmt);

    for (int i = 0; i < 4; i++) {
        sqlite3_stmt *pstmt = store->playerInsertStmt;
        sqlite3_reset(pstmt);
        sqlite3_clear_bindings(pstmt);
        sqlite3_bind_text(pstmt, 1, matchIdText, -1, SQLITE_STATIC);
        sqlite3_bind_int(pstmt, 2, i);
        sqlite3_bind_int64(pstmt, 3, (sqlite3_int64)players[i].userId);
        sqlite3_bind_text(pstmt, 4, players[i].name, -1, SQLITE_STATIC);
        sqlite3_bind_text(pstmt, 5, players[i].avatar, -1, SQLITE_STATIC);
        sqlite3_bind_int(pstmt, 6, (int)players[i].botLevel);
        sqlite3_bind_int(pstmt, 7, players[i].isBot ? 1 : 0);

        if (sqlite3_step(pstmt) != SQLITE_DONE) {
            sqlite3_reset(pstmt);
            sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
            return STORAGE_ERROR;
        }
        sqlite3_reset(pstmt);
    }

    if (sqlite3_exec(store->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    return OK;
}

Status storeEvents(Store *store, const MatchId *matchId, const EventBatch *batch) {
    if (!store || !matchId || !batch) return BAD_REQUEST;
    const char *matchIdText = (const char *)matchId;

    /* Verify match exists */
    sqlite3_stmt *stmt = store->matchSelectStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, matchIdText, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_reset(stmt);
        return NOT_FOUND;
    }
    sqlite3_reset(stmt);

    if (batch->count == 0) return OK;

    if (sqlite3_exec(store->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }

    for (size_t i = 0; i < batch->count; i++) {
        const MatchEvent *event = &batch->events[i];

        sqlite3_stmt *selectStmt = store->eventSelectStmt;
        sqlite3_reset(selectStmt);
        sqlite3_clear_bindings(selectStmt);
        sqlite3_bind_text(selectStmt, 1, matchIdText, -1, SQLITE_STATIC);
        sqlite3_bind_int64(selectStmt, 2, (sqlite3_int64)event->seq);

        int resultCode = sqlite3_step(selectStmt);
        if (resultCode == SQLITE_ROW) {
            /* Already exists: check if identical */
            uint64_t revision = (uint64_t)sqlite3_column_int64(selectStmt, 0);
            int actor = sqlite3_column_int(selectStmt, 1);
            const char *type = (const char *)sqlite3_column_text(selectStmt, 2);
            const char *payload = (const char *)sqlite3_column_text(selectStmt, 3);

            if (revision != event->revision || actor != event->actor ||
                strcmp(type ? type : "", event->type) != 0 ||
                strcmp(payload ? payload : "", event->payload) != 0) {
                sqlite3_reset(selectStmt);
                sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
                return INVALID_ACTION;
            }
            sqlite3_reset(selectStmt);
            continue; /* Identical event, idempotent */
        }
        sqlite3_reset(selectStmt);

        /* Insert new event */
        sqlite3_stmt *insertStmt = store->eventInsertStmt;
        sqlite3_reset(insertStmt);
        sqlite3_clear_bindings(insertStmt);
        sqlite3_bind_text(insertStmt, 1, matchIdText, -1, SQLITE_STATIC);
        sqlite3_bind_int64(insertStmt, 2, (sqlite3_int64)event->seq);
        sqlite3_bind_int64(insertStmt, 3, (sqlite3_int64)event->revision);
        sqlite3_bind_int(insertStmt, 4, event->actor);
        sqlite3_bind_text(insertStmt, 5, event->type, -1, SQLITE_STATIC);
        sqlite3_bind_text(insertStmt, 6, event->payload, -1, SQLITE_STATIC);

        if (sqlite3_step(insertStmt) != SQLITE_DONE) {
            sqlite3_reset(insertStmt);
            sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
            return STORAGE_ERROR;
        }
        sqlite3_reset(insertStmt);
    }

    if (sqlite3_exec(store->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    return OK;
}

static Status applyProfileDelta(Store *store, UserId userId, const char *matchIdText,
                                const ProfileDelta *delta) {
    if (!delta) return OK;

    /* Update profile_stats */
    sqlite3_stmt *stmt = store->profileStatsUpdateStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)delta->add.matches);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)delta->add.wins);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)delta->add.teamWins);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)delta->add.plus4);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)delta->add.unoCalls);
    sqlite3_bind_int(stmt, 6, delta->streak);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)delta->bestStreak);
    sqlite3_bind_int(stmt, 8, delta->peakElo);
    sqlite3_bind_int64(stmt, 9, (sqlite3_int64)userId);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_reset(stmt);
        return STORAGE_ERROR;
    }
    sqlite3_reset(stmt);

    /* Fallback insert if row didn't exist */
    if (sqlite3_changes(store->db) == 0) {
        sqlite3_stmt *fallbackStmt = store->profileStatsInsertFallbackStmt;
        sqlite3_reset(fallbackStmt);
        sqlite3_clear_bindings(fallbackStmt);
        sqlite3_bind_int64(fallbackStmt, 1, (sqlite3_int64)userId);
        sqlite3_bind_int64(fallbackStmt, 2, (sqlite3_int64)delta->add.matches);
        sqlite3_bind_int64(fallbackStmt, 3, (sqlite3_int64)delta->add.wins);
        sqlite3_bind_int64(fallbackStmt, 4, (sqlite3_int64)delta->add.teamWins);
        sqlite3_bind_int64(fallbackStmt, 5, (sqlite3_int64)delta->add.plus4);
        sqlite3_bind_int64(fallbackStmt, 6, (sqlite3_int64)delta->add.unoCalls);
        sqlite3_bind_int(fallbackStmt, 7, delta->streak);
        sqlite3_bind_int64(fallbackStmt, 8, (sqlite3_int64)delta->bestStreak);
        sqlite3_bind_int(fallbackStmt, 9, delta->peakElo);

        if (sqlite3_step(fallbackStmt) != SQLITE_DONE) {
            sqlite3_reset(fallbackStmt);
            return STORAGE_ERROR;
        }
        sqlite3_reset(fallbackStmt);
    }

    /* Insert unlocked badge bits into achievements */
    if (delta->unlocked != 0) {
        for (int bitIndex = 0; bitIndex < 32; bitIndex++) {
            uint32_t bit = 1u << bitIndex;
            if (delta->unlocked & bit) {
                sqlite3_stmt *astmt = store->achievementInsertStmt;
                sqlite3_reset(astmt);
                sqlite3_clear_bindings(astmt);
                sqlite3_bind_int64(astmt, 1, (sqlite3_int64)userId);
                sqlite3_bind_int(astmt, 2, (int)bit);
                sqlite3_bind_int64(astmt, 3, (sqlite3_int64)time(NULL));
                sqlite3_bind_text(astmt, 4, matchIdText, -1, SQLITE_STATIC);

                if (sqlite3_step(astmt) != SQLITE_DONE) {
                    sqlite3_reset(astmt);
                    return STORAGE_ERROR;
                }
                sqlite3_reset(astmt);
            }
        }
    }

    return OK;
}

Status storeFinalize(Store *store, const MatchResult *result, const ProfileDelta deltas[4]) {
    if (!store || !result) return BAD_REQUEST;
    const char *matchIdText = (const char *)result->matchId;

    if (sqlite3_exec(store->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }

    sqlite3_stmt *stmt = store->matchSelectStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, matchIdText, -1, SQLITE_STATIC);

    int resultCode = sqlite3_step(stmt);
    if (resultCode != SQLITE_ROW) {
        sqlite3_reset(stmt);
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return NOT_FOUND;
    }

    MatchStatus currentStatus = (MatchStatus)sqlite3_column_int(stmt, 2);
    int currentWinnerSeat = sqlite3_column_int(stmt, 3);
    int currentWinnerTeam = sqlite3_column_int(stmt, 4);
    uint64_t currentDuration = (uint64_t)sqlite3_column_int64(stmt, 5);
    sqlite3_reset(stmt);

    if (currentStatus == MATCH_FINALIZED) {
        /* Check if identical */
        if (currentWinnerSeat != result->winnerSeat ||
            currentWinnerTeam != result->winnerTeam ||
            currentDuration != result->durationMs) {
            sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
            return INVALID_ACTION;
        }

        /* Check each player's points and elo */
        for (int i = 0; i < 4; i++) {
            sqlite3_stmt *pstmt = store->playerSelectBySeatStmt;
            sqlite3_reset(pstmt);
            sqlite3_clear_bindings(pstmt);
            sqlite3_bind_text(pstmt, 1, matchIdText, -1, SQLITE_STATIC);
            sqlite3_bind_int(pstmt, 2, i);

            if (sqlite3_step(pstmt) != SQLITE_ROW) {
                sqlite3_reset(pstmt);
                sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
                return INVALID_ACTION;
            }

            int points = sqlite3_column_int(pstmt, 7);
            int eloBefore = sqlite3_column_int(pstmt, 8);
            int eloAfter = sqlite3_column_int(pstmt, 9);
            sqlite3_reset(pstmt);

            if (points != result->players[i].points ||
                eloBefore != result->players[i].eloBefore ||
                eloAfter != result->players[i].eloAfter) {
                sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
                return INVALID_ACTION;
            }
        }

        /* Idempotent finalize: do not re-apply stats or ELO */
        sqlite3_exec(store->db, "COMMIT;", NULL, NULL, NULL);
        return OK;
    }

    if (currentStatus != MATCH_ACTIVE) {
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return INVALID_ACTION;
    }

    /* Update matches status */
    stmt = store->matchUpdateStatusStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int(stmt, 1, (int)MATCH_FINALIZED);
    sqlite3_bind_int(stmt, 2, result->winnerSeat);
    sqlite3_bind_int(stmt, 3, result->winnerTeam);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)result->durationMs);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(stmt, 6, matchIdText, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_reset(stmt);
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }
    sqlite3_reset(stmt);

    /* Update match_players stats & elo */
    for (int i = 0; i < 4; i++) {
        const PlayerResult *playerResult = &result->players[i];
        sqlite3_stmt *pstmt = store->playerUpdateStmt;
        sqlite3_reset(pstmt);
        sqlite3_clear_bindings(pstmt);

        sqlite3_bind_int(pstmt, 1, playerResult->team);
        sqlite3_bind_int(pstmt, 2, playerResult->points);
        sqlite3_bind_int(pstmt, 3, playerResult->eloBefore);
        sqlite3_bind_int(pstmt, 4, playerResult->eloAfter);
        sqlite3_bind_int(pstmt, 5, playerResult->left ? 1 : 0);
        sqlite3_bind_int(pstmt, 6, (int)playerResult->stats.played);
        sqlite3_bind_int(pstmt, 7, (int)playerResult->stats.attacks);
        sqlite3_bind_int(pstmt, 8, (int)playerResult->stats.unoCalls);
        sqlite3_bind_int(pstmt, 9, (int)playerResult->stats.penalties);
        sqlite3_bind_int(pstmt, 10, (int)playerResult->stats.plus4);
        sqlite3_bind_text(pstmt, 11, matchIdText, -1, SQLITE_STATIC);
        sqlite3_bind_int(pstmt, 12, i);

        if (sqlite3_step(pstmt) != SQLITE_DONE) {
            sqlite3_reset(pstmt);
            sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
            return STORAGE_ERROR;
        }
        sqlite3_reset(pstmt);

        /* Update accounts ELO if ranked and userId > 0 */
        if (result->ranked && playerResult->userId > 0) {
            sqlite3_stmt *astmt = store->accountUpdateEloStmt;
            sqlite3_reset(astmt);
            sqlite3_clear_bindings(astmt);
            sqlite3_bind_int(astmt, 1, playerResult->eloAfter);
            sqlite3_bind_int64(astmt, 2, (sqlite3_int64)playerResult->userId);

            if (sqlite3_step(astmt) != SQLITE_DONE) {
                sqlite3_reset(astmt);
                sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
                return STORAGE_ERROR;
            }
            sqlite3_reset(astmt);
        }

        /* Update profile deltas if provided and userId > 0 */
        if (deltas && playerResult->userId > 0) {
            if (applyProfileDelta(store, playerResult->userId, matchIdText, &deltas[i]) != OK) {
                sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
                return STORAGE_ERROR;
            }
        }
    }

    if (sqlite3_exec(store->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    return OK;
}

/* ROLLBACK can only fail when no transaction is open, which is the state wanted. */
static Status rollbackWith(sqlite3 *db, Status status) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return status;
}

static Status readMatchStatus(Store *store, const char *matchId, MatchStatus *out) {
    sqlite3_stmt *stmt = store->matchSelectStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (sqlite3_bind_text(stmt, 1, matchId, -1, SQLITE_STATIC) != SQLITE_OK) return STORAGE_ERROR;
    int resultCode = sqlite3_step(stmt);
    if (resultCode == SQLITE_ROW) *out = (MatchStatus)sqlite3_column_int(stmt, 2);
    sqlite3_reset(stmt);
    if (resultCode == SQLITE_ROW) return OK;
    return resultCode == SQLITE_DONE ? NOT_FOUND : STORAGE_ERROR;
}

static Status insertEvent(Store *store, const char *matchId, const MatchEvent *event) {
    sqlite3_stmt *stmt = store->eventInsertStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_text(stmt, 1, matchId, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event->seq) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)event->revision) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 4, event->actor) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 5, event->type, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 6, event->payload, -1, SQLITE_STATIC) == SQLITE_OK;
    int resultCode = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    return resultCode == SQLITE_DONE ? OK : STORAGE_ERROR;
}

static Status markAborted(Store *store, const char *matchId, uint64_t durationMs) {
    sqlite3_stmt *stmt = store->matchAbortStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_int(stmt, 1, MATCH_ABORTED) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)durationMs) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 3, matchId, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 4, MATCH_ACTIVE) == SQLITE_OK;
    int resultCode = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    return resultCode == SQLITE_DONE && sqlite3_changes(store->db) == 1 ? OK : STORAGE_ERROR;
}

Status storeAbort(Store *store, const MatchId *id, const MatchEvent *event, uint64_t durationMs) {
    if (!store || !id || !event) return BAD_REQUEST;
    const char *matchId = (const char *)id;

    /* Event and status commit together: otherwise a failure between them leaves
     * an ABORTED match without its abort event, or an event on an ACTIVE match. */
    if (sqlite3_exec(store->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }
    MatchStatus current = MATCH_ACTIVE;
    Status status = readMatchStatus(store, matchId, &current);
    if (status != OK) return rollbackWith(store->db, status);
    /* The coordinator retries until OK; a retry after a committed abort (or
     * after crash recovery marked the match) must not append a second event. */
    if (current == MATCH_ABORTED) return rollbackWith(store->db, OK);
    if (current != MATCH_ACTIVE) return rollbackWith(store->db, INVALID_ACTION);

    status = insertEvent(store, matchId, event);
    if (status == OK) status = markAborted(store, matchId, durationMs);
    if (status != OK) return rollbackWith(store->db, status);

    if (sqlite3_exec(store->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        return rollbackWith(store->db, STORAGE_ERROR);
    }
    return OK;
}

Status storeLeaderboard(Store *store, LeaderboardEntry out[STORE_LEADERBOARD_MAX], size_t *count) {
    if (!store || !out || !count) return BAD_REQUEST;

    sqlite3_stmt *stmt = store->leaderboardStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (sqlite3_bind_int(stmt, 1, STORE_LEADERBOARD_MAX) != SQLITE_OK) return STORAGE_ERROR;

    size_t rows = 0;
    int resultCode = SQLITE_DONE;
    while (rows < STORE_LEADERBOARD_MAX && (resultCode = sqlite3_step(stmt)) == SQLITE_ROW) {
        LeaderboardEntry *entry = &out[rows++];
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *avatar = (const char *)sqlite3_column_text(stmt, 2);
        memset(entry, 0, sizeof *entry);
        entry->userId = (UserId)sqlite3_column_int64(stmt, 0);
        snprintf(entry->name, sizeof entry->name, "%s", name ? name : "");
        snprintf(entry->avatar, sizeof entry->avatar, "%s", avatar ? avatar : "");
        entry->elo = sqlite3_column_int(stmt, 3);
    }
    sqlite3_reset(stmt);
    if (resultCode != SQLITE_DONE && resultCode != SQLITE_ROW) return STORAGE_ERROR;

    *count = rows;
    return OK;
}

static Status accountExists(Store *store, UserId id) {
    Account account;
    return storeAccountGetById(store, id, &account);
}

static Status readFriendLink(Store *store, UserId first, UserId second, bool *linked) {
    sqlite3_stmt *stmt = store->friendLinkSelectStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)first) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)second) == SQLITE_OK;
    int resultCode = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    if (resultCode != SQLITE_ROW && resultCode != SQLITE_DONE) return STORAGE_ERROR;
    *linked = resultCode == SQLITE_ROW;
    return OK;
}

static Status insertFriendRequest(Store *store, UserId from, UserId to) {
    sqlite3_stmt *stmt = store->friendInsertStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)from) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)to) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 3, FRIEND_STATUS_PENDING) == SQLITE_OK;
    int resultCode = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    return resultCode == SQLITE_DONE ? OK : STORAGE_ERROR;
}

Status storeFriendRequest(Store *store, UserId from, UserId to) {
    if (!store || from == to) return BAD_REQUEST;

    /* The reverse-direction check and the insert must see the same state:
     * otherwise two crossing requests could both pass and leave A->B and B->A. */
    if (sqlite3_exec(store->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }
    Status status = accountExists(store, to);
    if (status != OK) return rollbackWith(store->db, status);

    bool linked = false;
    status = readFriendLink(store, from, to, &linked);
    if (status != OK) return rollbackWith(store->db, status);
    if (linked) return rollbackWith(store->db, INVALID_ACTION);

    status = insertFriendRequest(store, from, to);
    if (status != OK) return rollbackWith(store->db, status);

    if (sqlite3_exec(store->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        return rollbackWith(store->db, STORAGE_ERROR);
    }
    return OK;
}

Status storeFriendReply(Store *store, UserId me, UserId requester, bool accept) {
    if (!store) return BAD_REQUEST;

    /* One guarded statement per answer: no transaction is needed, and a row
     * count of 0 means there was no pending requester -> me request. */
    sqlite3_stmt *stmt = accept ? store->friendAcceptStmt : store->friendRejectStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound;
    if (accept) {
        bound = sqlite3_bind_int(stmt, 1, FRIEND_STATUS_ACCEPTED) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)requester) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 3, (sqlite3_int64)me) == SQLITE_OK &&
            sqlite3_bind_int(stmt, 4, FRIEND_STATUS_PENDING) == SQLITE_OK;
    } else {
        bound = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)requester) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)me) == SQLITE_OK &&
            sqlite3_bind_int(stmt, 3, FRIEND_STATUS_PENDING) == SQLITE_OK;
    }
    int resultCode = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    if (resultCode != SQLITE_DONE) return STORAGE_ERROR;
    return sqlite3_changes(store->db) == 1 ? OK : NOT_FOUND;
}

Status storeFriendList(Store *store, UserId me, FriendRow out[STORE_FRIEND_MAX], size_t *count) {
    if (!store || !out || !count) return BAD_REQUEST;

    sqlite3_stmt *stmt = store->friendListStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)me) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 2, STORE_FRIEND_MAX) == SQLITE_OK;
    if (!bound) return STORAGE_ERROR;

    size_t rows = 0;
    int resultCode = SQLITE_DONE;
    while (rows < STORE_FRIEND_MAX && (resultCode = sqlite3_step(stmt)) == SQLITE_ROW) {
        FriendRow *row = &out[rows++];
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *avatar = (const char *)sqlite3_column_text(stmt, 2);
        UserId sender = (UserId)sqlite3_column_int64(stmt, 4);
        memset(row, 0, sizeof *row);
        row->userId = (UserId)sqlite3_column_int64(stmt, 0);
        snprintf(row->name, sizeof row->name, "%s", name ? name : "");
        snprintf(row->avatar, sizeof row->avatar, "%s", avatar ? avatar : "");
        row->elo = sqlite3_column_int(stmt, 3);
        if (sqlite3_column_int(stmt, 5) == FRIEND_STATUS_ACCEPTED) {
            row->link = FRIEND_ACCEPTED;
        } else {
            row->link = sender == me ? FRIEND_PENDING_OUT : FRIEND_PENDING_IN;
        }
    }
    sqlite3_reset(stmt);
    if (resultCode != SQLITE_DONE && resultCode != SQLITE_ROW) return STORAGE_ERROR;

    *count = rows;
    return OK;
}

/* Binds ?1 user, ?2 MATCH_FINALIZED, ?3/?4 the team modes shared by the
 * PROFILE_WON_SQL / PROFILE_MATCHES_SQL fragments. */
static bool bindProfileMatches(sqlite3_stmt *stmt, UserId id) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 2, MATCH_FINALIZED) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 3, TWO_V_TWO) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 4, SIDE) == SQLITE_OK;
}

static Status readProfileBadges(Store *store, UserId id, uint32_t *badges) {
    sqlite3_stmt *stmt = store->profileBadgesStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id) != SQLITE_OK) return STORAGE_ERROR;
    int resultCode;
    while ((resultCode = sqlite3_step(stmt)) == SQLITE_ROW) {
        *badges |= (uint32_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_reset(stmt);
    return resultCode == SQLITE_DONE ? OK : STORAGE_ERROR;
}

static Status readProfileModes(Store *store, UserId id, Profile *out) {
    sqlite3_stmt *stmt = store->profileModesStmt;
    if (!bindProfileMatches(stmt, id)) return STORAGE_ERROR;
    int resultCode;
    while ((resultCode = sqlite3_step(stmt)) == SQLITE_ROW) {
        int mode = sqlite3_column_int(stmt, 0);
        /* A row from a mode this build does not know is skipped, not trusted. */
        if (mode < 0 || mode >= STORE_MODE_COUNT) continue;
        out->modes[mode].matches = (uint32_t)sqlite3_column_int64(stmt, 1);
        out->modes[mode].wins = (uint32_t)sqlite3_column_int64(stmt, 2);
    }
    sqlite3_reset(stmt);
    return resultCode == SQLITE_DONE ? OK : STORAGE_ERROR;
}

static Status readProfileHistory(Store *store, UserId id, Profile *out) {
    sqlite3_stmt *stmt = store->profileHistoryStmt;
    if (!bindProfileMatches(stmt, id) ||
        sqlite3_bind_int(stmt, 5, STORE_HISTORY_MAX) != SQLITE_OK) return STORAGE_ERROR;
    int resultCode = SQLITE_DONE;
    while (out->historyCount < STORE_HISTORY_MAX &&
           (resultCode = sqlite3_step(stmt)) == SQLITE_ROW) {
        HistoryRow *row = &out->history[out->historyCount++];
        const char *matchId = (const char *)sqlite3_column_text(stmt, 0);
        snprintf(row->matchId, sizeof row->matchId, "%s", matchId ? matchId : "");
        row->mode = (Mode)sqlite3_column_int(stmt, 1);
        row->won = sqlite3_column_int(stmt, 2) != 0;
        row->eloDelta = sqlite3_column_int(stmt, 3);
        row->endedAt = (int64_t)sqlite3_column_int64(stmt, 4);
    }
    sqlite3_reset(stmt);
    return resultCode == SQLITE_DONE || resultCode == SQLITE_ROW ? OK : STORAGE_ERROR;
}

Status storeProfileGet(Store *store, UserId id, Profile *out) {
    if (!store || !out) return BAD_REQUEST;

    Account account;
    Status status = storeAccountGetById(store, id, &account);
    if (status != OK) return status;

    /* Copy only the public fields: email and hash stay in the local Account. */
    memset(out, 0, sizeof *out);
    out->userId = account.id;
    snprintf(out->name, sizeof out->name, "%s", account.name);
    snprintf(out->avatar, sizeof out->avatar, "%s", account.avatar);
    out->elo = account.elo;

    status = storeProfileStats(store, id, &out->stats);
    /* Every account gets a profile_stats row at creation or v2 migration;
     * a missing one just means nothing has been recorded yet. */
    if (status == NOT_FOUND) status = OK;
    if (status == OK) status = readProfileBadges(store, id, &out->badges);
    if (status == OK) status = readProfileModes(store, id, out);
    if (status == OK) status = readProfileHistory(store, id, out);
    return status;
}

Status storeProfileUpdate(Store *store, UserId id, const char *name, const char *avatar) {
    if (!store || !name || !avatar) return BAD_REQUEST;

    sqlite3_stmt *stmt = store->profileUpdateStmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 2, avatar, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)id) == SQLITE_OK;
    int resultCode = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    if (resultCode != SQLITE_DONE) return STORAGE_ERROR;
    return sqlite3_changes(store->db) == 1 ? OK : NOT_FOUND;
}

Status storeExportMatch(Store *store, const MatchId *matchId, const char *path) {
    if (!store || !matchId || !path) return BAD_REQUEST;
    const char *matchIdText = (const char *)matchId;

    sqlite3_stmt *matchStmt = store->matchSelectStmt;
    sqlite3_reset(matchStmt);
    sqlite3_clear_bindings(matchStmt);
    sqlite3_bind_text(matchStmt, 1, matchIdText, -1, SQLITE_STATIC);

    if (sqlite3_step(matchStmt) != SQLITE_ROW) {
        sqlite3_reset(matchStmt);
        return NOT_FOUND;
    }

    int mode = sqlite3_column_int(matchStmt, 0);
    int ranked = sqlite3_column_int(matchStmt, 1);
    int status = sqlite3_column_int(matchStmt, 2);
    int winnerSeat = sqlite3_column_int(matchStmt, 3);
    int winnerTeam = sqlite3_column_int(matchStmt, 4);
    uint64_t duration = (uint64_t)sqlite3_column_int64(matchStmt, 5);
    uint64_t createdAt = (uint64_t)sqlite3_column_int64(matchStmt, 6);
    sqlite3_reset(matchStmt);

    struct json_object *root = json_object_new_object();
    json_object_object_add(root, "matchId", json_object_new_string(matchIdText));
    json_object_object_add(root, "mode", json_object_new_int(mode));
    json_object_object_add(root, "ranked", json_object_new_boolean(ranked ? 1 : 0));
    json_object_object_add(root, "status", json_object_new_int(status));
    json_object_object_add(root, "winnerSeat", json_object_new_int(winnerSeat));
    json_object_object_add(root, "winnerTeam", json_object_new_int(winnerTeam));
    json_object_object_add(root, "durationMs", json_object_new_int64((int64_t)duration));
    json_object_object_add(root, "createdAt", json_object_new_int64((int64_t)createdAt));

    /* Query match players */
    struct json_object *playersJson = json_object_new_array();
    sqlite3_stmt *playerStmt = store->playerSelectAllStmt;
    sqlite3_reset(playerStmt);
    sqlite3_clear_bindings(playerStmt);
    sqlite3_bind_text(playerStmt, 1, matchIdText, -1, SQLITE_STATIC);

    while (sqlite3_step(playerStmt) == SQLITE_ROW) {
        struct json_object *playerJson = json_object_new_object();
        json_object_object_add(playerJson, "seat",
                               json_object_new_int(sqlite3_column_int(playerStmt, 0)));
        json_object_object_add(playerJson, "userId",
                               json_object_new_int64(sqlite3_column_int64(playerStmt, 1)));

        const char *name = (const char *)sqlite3_column_text(playerStmt, 2);
        json_object_object_add(playerJson, "name", json_object_new_string(name ? name : ""));

        const char *avatar = (const char *)sqlite3_column_text(playerStmt, 3);
        json_object_object_add(playerJson, "avatar", json_object_new_string(avatar ? avatar : ""));

        json_object_object_add(playerJson, "botLevel",
                               json_object_new_int(sqlite3_column_int(playerStmt, 4)));
        json_object_object_add(playerJson, "isBot",
                               json_object_new_boolean(sqlite3_column_int(playerStmt, 5) ? 1 : 0));
        json_object_object_add(playerJson, "team",
                               json_object_new_int(sqlite3_column_int(playerStmt, 6)));
        json_object_object_add(playerJson, "points",
                               json_object_new_int(sqlite3_column_int(playerStmt, 7)));
        json_object_object_add(playerJson, "eloBefore",
                               json_object_new_int(sqlite3_column_int(playerStmt, 8)));
        json_object_object_add(playerJson, "eloAfter",
                               json_object_new_int(sqlite3_column_int(playerStmt, 9)));
        json_object_object_add(playerJson, "left",
                               json_object_new_boolean(sqlite3_column_int(playerStmt, 10) ? 1 : 0));

        struct json_object *stats = json_object_new_object();
        json_object_object_add(stats, "played",
                               json_object_new_int(sqlite3_column_int(playerStmt, 11)));
        json_object_object_add(stats, "attacks",
                               json_object_new_int(sqlite3_column_int(playerStmt, 12)));
        json_object_object_add(stats, "unoCalls",
                               json_object_new_int(sqlite3_column_int(playerStmt, 13)));
        json_object_object_add(stats, "penalties",
                               json_object_new_int(sqlite3_column_int(playerStmt, 14)));
        json_object_object_add(stats, "plus4",
                               json_object_new_int(sqlite3_column_int(playerStmt, 15)));
        json_object_object_add(playerJson, "stats", stats);

        json_object_array_add(playersJson, playerJson);
    }
    sqlite3_reset(playerStmt);
    json_object_object_add(root, "players", playersJson);

    /* Query match events */
    struct json_object *eventsJson = json_object_new_array();
    sqlite3_stmt *eventStmt = store->eventSelectAllStmt;
    sqlite3_reset(eventStmt);
    sqlite3_clear_bindings(eventStmt);
    sqlite3_bind_text(eventStmt, 1, matchIdText, -1, SQLITE_STATIC);

    while (sqlite3_step(eventStmt) == SQLITE_ROW) {
        struct json_object *event = json_object_new_object();
        json_object_object_add(event, "seq",
                               json_object_new_int64(sqlite3_column_int64(eventStmt, 0)));
        json_object_object_add(event, "revision",
                               json_object_new_int64(sqlite3_column_int64(eventStmt, 1)));
        json_object_object_add(event, "actor",
                               json_object_new_int(sqlite3_column_int(eventStmt, 2)));

        const char *etype = (const char *)sqlite3_column_text(eventStmt, 3);
        json_object_object_add(event, "type", json_object_new_string(etype ? etype : ""));

        const char *payloadText = (const char *)sqlite3_column_text(eventStmt, 4);
        if (payloadText) {
            struct json_object *parsedPayload = json_tokener_parse(payloadText);
            if (parsedPayload) {
                json_object_object_add(event, "payload", parsedPayload);
            } else {
                json_object_object_add(event, "payload", json_object_new_string(payloadText));
            }
        } else {
            json_object_object_add(event, "payload", json_object_new_object());
        }

        json_object_array_add(eventsJson, event);
    }
    sqlite3_reset(eventStmt);
    json_object_object_add(root, "events", eventsJson);

    int dumpResult = json_object_to_file_ext(path, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);

    if (dumpResult != 0) return STORAGE_ERROR;
    return OK;
}

Status storeAccountCreate(Store *store, const AccountCreate *newAccount, UserId *outId) {
    if (!store || !newAccount || !outId) return BAD_REQUEST;

    /* The account row and its profile_stats row must be created together:
     * otherwise a failed second insert leaves an account whose email is taken
     * while the client was told registration failed. */
    if (sqlite3_exec(store->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }

    sqlite3_reset(store->accountInsertStmt);
    sqlite3_clear_bindings(store->accountInsertStmt);

    sqlite3_bind_text(store->accountInsertStmt, 1, newAccount->email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(store->accountInsertStmt, 2, newAccount->hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(store->accountInsertStmt, 3, newAccount->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(store->accountInsertStmt, 4, newAccount->avatar, -1, SQLITE_TRANSIENT);

    int resultCode = sqlite3_step(store->accountInsertStmt);
    sqlite3_reset(store->accountInsertStmt);
    if (resultCode == SQLITE_CONSTRAINT) {
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return BAD_REQUEST; /* email already taken */
    }
    if (resultCode != SQLITE_DONE) {
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    UserId newId = (UserId)sqlite3_last_insert_rowid(store->db);

    sqlite3_stmt *profileInitStmt = store->profileStatsInitStmt;
    sqlite3_reset(profileInitStmt);
    sqlite3_clear_bindings(profileInitStmt);
    sqlite3_bind_int64(profileInitStmt, 1, (sqlite3_int64)newId);
    resultCode = sqlite3_step(profileInitStmt);
    sqlite3_reset(profileInitStmt);
    if (resultCode != SQLITE_DONE) {
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    if (sqlite3_exec(store->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    *outId = newId;
    return OK;
}

Status storeAccountGet(Store *store, const char *email, Account *out) {
    if (!store || !email || !out) return BAD_REQUEST;

    sqlite3_reset(store->accountSelectByEmailStmt);
    sqlite3_clear_bindings(store->accountSelectByEmailStmt);

    sqlite3_bind_text(store->accountSelectByEmailStmt, 1, email, -1, SQLITE_TRANSIENT);

    int resultCode = sqlite3_step(store->accountSelectByEmailStmt);
    if (resultCode == SQLITE_ROW) {
        memset(out, 0, sizeof(*out));
        out->id = (UserId)sqlite3_column_int64(store->accountSelectByEmailStmt, 0);

        const char *dbEmail = (const char *)sqlite3_column_text(store->accountSelectByEmailStmt, 1);
        if (dbEmail) strncpy(out->email, dbEmail, sizeof(out->email) - 1);

        const char *dbHash = (const char *)sqlite3_column_text(store->accountSelectByEmailStmt, 2);
        if (dbHash) strncpy(out->hash, dbHash, sizeof(out->hash) - 1);

        const char *dbName = (const char *)sqlite3_column_text(store->accountSelectByEmailStmt, 3);
        if (dbName) strncpy(out->name, dbName, sizeof(out->name) - 1);

        const char *dbAvatar = (const char *)sqlite3_column_text(store->accountSelectByEmailStmt,
                                                                 4);
        if (dbAvatar) strncpy(out->avatar, dbAvatar, sizeof(out->avatar) - 1);

        out->elo = sqlite3_column_int(store->accountSelectByEmailStmt, 5);

        sqlite3_reset(store->accountSelectByEmailStmt);
        return OK;
    }
    sqlite3_reset(store->accountSelectByEmailStmt);
    if (resultCode == SQLITE_DONE) return NOT_FOUND;
    return STORAGE_ERROR;
}

Status storeAccountGetById(Store *store, UserId id, Account *out) {
    if (!store || !out) return BAD_REQUEST;

    sqlite3_reset(store->accountSelectByIdStmt);
    sqlite3_clear_bindings(store->accountSelectByIdStmt);

    sqlite3_bind_int64(store->accountSelectByIdStmt, 1, (sqlite3_int64)id);

    int resultCode = sqlite3_step(store->accountSelectByIdStmt);
    if (resultCode == SQLITE_ROW) {
        memset(out, 0, sizeof(*out));
        out->id = (UserId)sqlite3_column_int64(store->accountSelectByIdStmt, 0);

        const char *dbEmail = (const char *)sqlite3_column_text(store->accountSelectByIdStmt, 1);
        if (dbEmail) strncpy(out->email, dbEmail, sizeof(out->email) - 1);

        const char *dbHash = (const char *)sqlite3_column_text(store->accountSelectByIdStmt, 2);
        if (dbHash) strncpy(out->hash, dbHash, sizeof(out->hash) - 1);

        const char *dbName = (const char *)sqlite3_column_text(store->accountSelectByIdStmt, 3);
        if (dbName) strncpy(out->name, dbName, sizeof(out->name) - 1);

        const char *dbAvatar = (const char *)sqlite3_column_text(store->accountSelectByIdStmt, 4);
        if (dbAvatar) strncpy(out->avatar, dbAvatar, sizeof(out->avatar) - 1);

        out->elo = sqlite3_column_int(store->accountSelectByIdStmt, 5);

        sqlite3_reset(store->accountSelectByIdStmt);
        return OK;
    }
    sqlite3_reset(store->accountSelectByIdStmt);
    if (resultCode == SQLITE_DONE) return NOT_FOUND;
    return STORAGE_ERROR;
}

Status storeProfileStats(Store *store, UserId userId, ProfileStats *out) {
    if (!store || !out) return BAD_REQUEST;

    sqlite3_reset(store->profileStatsSelectStmt);
    sqlite3_clear_bindings(store->profileStatsSelectStmt);

    sqlite3_bind_int64(store->profileStatsSelectStmt, 1, (sqlite3_int64)userId);

    int resultCode = sqlite3_step(store->profileStatsSelectStmt);
    if (resultCode == SQLITE_ROW) {
        memset(out, 0, sizeof(*out));
        out->matches = (uint32_t)sqlite3_column_int64(store->profileStatsSelectStmt, 0);
        out->wins = (uint32_t)sqlite3_column_int64(store->profileStatsSelectStmt, 1);
        out->teamWins = (uint32_t)sqlite3_column_int64(store->profileStatsSelectStmt, 2);
        out->plus4 = (uint32_t)sqlite3_column_int64(store->profileStatsSelectStmt, 3);
        out->unoCalls = (uint32_t)sqlite3_column_int64(store->profileStatsSelectStmt, 4);
        out->streak = (int32_t)sqlite3_column_int(store->profileStatsSelectStmt, 5);
        out->bestStreak = (uint32_t)sqlite3_column_int64(store->profileStatsSelectStmt, 6);
        out->peakElo = (int32_t)sqlite3_column_int(store->profileStatsSelectStmt, 7);

        sqlite3_reset(store->profileStatsSelectStmt);
        return OK;
    }
    sqlite3_reset(store->profileStatsSelectStmt);
    if (resultCode == SQLITE_DONE) return NOT_FOUND;
    return STORAGE_ERROR;
}
