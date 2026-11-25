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
    sqlite3_stmt *stmt_match_select;
    sqlite3_stmt *stmt_match_insert;
    sqlite3_stmt *stmt_match_update_status;
    sqlite3_stmt *stmt_player_insert;
    sqlite3_stmt *stmt_player_select_by_seat;
    sqlite3_stmt *stmt_player_select_all;
    sqlite3_stmt *stmt_player_update;
    sqlite3_stmt *stmt_event_select;
    sqlite3_stmt *stmt_event_insert;
    sqlite3_stmt *stmt_event_select_all;
    sqlite3_stmt *stmt_account_update_elo;
    sqlite3_stmt *stmt_achievement_insert;
    sqlite3_stmt *stmt_account_insert;
    sqlite3_stmt *stmt_account_select_by_email;
    sqlite3_stmt *stmt_account_select_by_id;
    sqlite3_stmt *stmt_profile_stats_init;
    sqlite3_stmt *stmt_profile_stats_select;
    sqlite3_stmt *stmt_profile_stats_update;
    sqlite3_stmt *stmt_profile_stats_insert_fallback;
    sqlite3_stmt *stmt_match_abort;
    sqlite3_stmt *stmt_leaderboard;
    sqlite3_stmt *stmt_friend_link_select;
    sqlite3_stmt *stmt_friend_insert;
    sqlite3_stmt *stmt_friend_accept;
    sqlite3_stmt *stmt_friend_reject;
    sqlite3_stmt *stmt_friend_list;
    sqlite3_stmt *stmt_profile_badges;
    sqlite3_stmt *stmt_profile_modes;
    sqlite3_stmt *stmt_profile_history;
    sqlite3_stmt *stmt_profile_update;
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

static Status prepare_statements(Store *s)
{
    const char *sql_m_sel = "SELECT mode, ranked, status, winner_seat, winner_team, duration_ms, created_at FROM matches WHERE match_id = ?;";
    if (sqlite3_prepare_v2(s->db, sql_m_sel, -1, &s->stmt_match_select, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_m_ins = "INSERT INTO matches (match_id, mode, ranked, status, winner_seat, winner_team, duration_ms, created_at) "
                            "VALUES (?, ?, ?, ?, -1, -1, 0, ?);";
    if (sqlite3_prepare_v2(s->db, sql_m_ins, -1, &s->stmt_match_insert, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_m_upd = "UPDATE matches SET status = ?, winner_seat = ?, winner_team = ?, duration_ms = ?, "
                            "ended_at = ? WHERE match_id = ?;";
    if (sqlite3_prepare_v2(s->db, sql_m_upd, -1, &s->stmt_match_update_status, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_p_ins = "INSERT INTO match_players (match_id, seat, user_id, name, avatar, bot_level, is_bot, team, points, elo_before, elo_after) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, -1, 0, 1500, 1500);";
    if (sqlite3_prepare_v2(s->db, sql_p_ins, -1, &s->stmt_player_insert, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_p_sel_seat = "SELECT seat, user_id, name, avatar, bot_level, is_bot, team, points, elo_before, elo_after, "
                                 "player_left, played, attacks, uno_calls, penalties, plus4 "
                                 "FROM match_players WHERE match_id = ? AND seat = ?;";
    if (sqlite3_prepare_v2(s->db, sql_p_sel_seat, -1, &s->stmt_player_select_by_seat, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_p_sel_all = "SELECT seat, user_id, name, avatar, bot_level, is_bot, team, points, elo_before, elo_after, "
                                "player_left, played, attacks, uno_calls, penalties, plus4 "
                                "FROM match_players WHERE match_id = ? ORDER BY seat ASC;";
    if (sqlite3_prepare_v2(s->db, sql_p_sel_all, -1, &s->stmt_player_select_all, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_p_upd = "UPDATE match_players SET team = ?, points = ?, elo_before = ?, elo_after = ?, "
                            "player_left = ?, played = ?, attacks = ?, uno_calls = ?, penalties = ?, plus4 = ? "
                            "WHERE match_id = ? AND seat = ?;";
    if (sqlite3_prepare_v2(s->db, sql_p_upd, -1, &s->stmt_player_update, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_e_sel = "SELECT revision, actor, type, payload FROM match_events WHERE match_id = ? AND seq = ?;";
    if (sqlite3_prepare_v2(s->db, sql_e_sel, -1, &s->stmt_event_select, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_e_ins = "INSERT INTO match_events (match_id, seq, revision, actor, type, payload) VALUES (?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(s->db, sql_e_ins, -1, &s->stmt_event_insert, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_e_sel_all = "SELECT seq, revision, actor, type, payload FROM match_events WHERE match_id = ? ORDER BY seq ASC;";
    if (sqlite3_prepare_v2(s->db, sql_e_sel_all, -1, &s->stmt_event_select_all, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_a_upd_elo = "UPDATE accounts SET elo = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(s->db, sql_a_upd_elo, -1, &s->stmt_account_update_elo, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_ach_ins = "INSERT OR IGNORE INTO achievements (user_id, badge, unlocked_at, match_id) VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(s->db, sql_ach_ins, -1, &s->stmt_achievement_insert, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_prof_init = "INSERT INTO profile_stats (user_id, peak_elo) VALUES (?, 1500);";
    if (sqlite3_prepare_v2(s->db, sql_prof_init, -1, &s->stmt_profile_stats_init, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_prof_sel = "SELECT matches, wins, team_wins, plus4, uno_calls, streak, best_streak, peak_elo "
                               "FROM profile_stats WHERE user_id = ?;";
    if (sqlite3_prepare_v2(s->db, sql_prof_sel, -1, &s->stmt_profile_stats_select, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_prof_upd = "UPDATE profile_stats "
                               "SET matches = matches + ?, "
                               "    wins = wins + ?, "
                               "    team_wins = team_wins + ?, "
                               "    plus4 = plus4 + ?, "
                               "    uno_calls = uno_calls + ?, "
                               "    streak = ?, "
                               "    best_streak = ?, "
                               "    peak_elo = ? "
                               "WHERE user_id = ?;";
    if (sqlite3_prepare_v2(s->db, sql_prof_upd, -1, &s->stmt_profile_stats_update, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_prof_fb = "INSERT INTO profile_stats (user_id, matches, wins, team_wins, plus4, uno_calls, streak, best_streak, peak_elo) "
                              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(s->db, sql_prof_fb, -1, &s->stmt_profile_stats_insert_fallback, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_acc_ins = "INSERT INTO accounts (email, password_hash, name, avatar, elo, created_at) "
                              "VALUES (?, ?, ?, ?, 1500, strftime('%s', 'now'));";
    if (sqlite3_prepare_v2(s->db, sql_acc_ins, -1, &s->stmt_account_insert, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_acc_sel_email = "SELECT id, email, password_hash, name, avatar, elo FROM accounts WHERE email = ?;";
    if (sqlite3_prepare_v2(s->db, sql_acc_sel_email, -1, &s->stmt_account_select_by_email, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_acc_sel_id = "SELECT id, email, password_hash, name, avatar, elo FROM accounts WHERE id = ?;";
    if (sqlite3_prepare_v2(s->db, sql_acc_sel_id, -1, &s->stmt_account_select_by_id, NULL) != SQLITE_OK) return STORAGE_ERROR;

    /* Guarded by status so a FINALIZED match can never be overwritten. */
    const char *sql_m_abort = "UPDATE matches SET status = ?, duration_ms = ? "
                              "WHERE match_id = ? AND status = ?;";
    if (sqlite3_prepare_v2(s->db, sql_m_abort, -1, &s->stmt_match_abort, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_board = "SELECT id, name, avatar, elo FROM accounts "
                            "ORDER BY elo DESC, id ASC LIMIT ?;";
    if (sqlite3_prepare_v2(s->db, sql_board, -1, &s->stmt_leaderboard, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_f_link = "SELECT status FROM friends "
                             "WHERE (user_id = ?1 AND friend_id = ?2) OR (user_id = ?2 AND friend_id = ?1);";
    if (sqlite3_prepare_v2(s->db, sql_f_link, -1, &s->stmt_friend_link_select, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_f_ins = "INSERT INTO friends (user_id, friend_id, status, created_at) "
                            "VALUES (?, ?, ?, strftime('%s', 'now'));";
    if (sqlite3_prepare_v2(s->db, sql_f_ins, -1, &s->stmt_friend_insert, NULL) != SQLITE_OK) return STORAGE_ERROR;

    /* Both reply statements only touch a PENDING row in the requester -> me
     * direction, so the sender cannot answer its own request. */
    const char *sql_f_acc = "UPDATE friends SET status = ?1 "
                            "WHERE user_id = ?2 AND friend_id = ?3 AND status = ?4;";
    if (sqlite3_prepare_v2(s->db, sql_f_acc, -1, &s->stmt_friend_accept, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_f_rej = "DELETE FROM friends WHERE user_id = ?1 AND friend_id = ?2 AND status = ?3;";
    if (sqlite3_prepare_v2(s->db, sql_f_rej, -1, &s->stmt_friend_reject, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_f_list = "SELECT a.id, a.name, a.avatar, a.elo, f.user_id, f.status FROM friends f "
                             "JOIN accounts a ON a.id = CASE WHEN f.user_id = ?1 THEN f.friend_id ELSE f.user_id END "
                             "WHERE f.user_id = ?1 OR f.friend_id = ?1 ORDER BY a.id ASC LIMIT ?2;";
    if (sqlite3_prepare_v2(s->db, sql_f_list, -1, &s->stmt_friend_list, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_badges = "SELECT badge FROM achievements WHERE user_id = ?;";
    if (sqlite3_prepare_v2(s->db, sql_badges, -1, &s->stmt_profile_badges, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_modes = "SELECT m.mode, count(*), sum(" PROFILE_WON_SQL ")"
                            PROFILE_MATCHES_SQL " GROUP BY m.mode;";
    if (sqlite3_prepare_v2(s->db, sql_modes, -1, &s->stmt_profile_modes, NULL) != SQLITE_OK) return STORAGE_ERROR;

    /* rowid breaks ties between matches that ended in the same second. */
    const char *sql_history = "SELECT m.match_id, m.mode, " PROFILE_WON_SQL ", "
                              "mp.elo_after - mp.elo_before, m.ended_at"
                              PROFILE_MATCHES_SQL " ORDER BY m.ended_at DESC, m.rowid DESC LIMIT ?5;";
    if (sqlite3_prepare_v2(s->db, sql_history, -1, &s->stmt_profile_history, NULL) != SQLITE_OK) return STORAGE_ERROR;

    const char *sql_identity = "UPDATE accounts SET name = ?, avatar = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(s->db, sql_identity, -1, &s->stmt_profile_update, NULL) != SQLITE_OK) return STORAGE_ERROR;

    return OK;
}

static void finalize_statements(Store *s)
{
    if (!s) return;
    if (s->stmt_match_select) { sqlite3_finalize(s->stmt_match_select); s->stmt_match_select = NULL; }
    if (s->stmt_match_insert) { sqlite3_finalize(s->stmt_match_insert); s->stmt_match_insert = NULL; }
    if (s->stmt_match_update_status) { sqlite3_finalize(s->stmt_match_update_status); s->stmt_match_update_status = NULL; }
    if (s->stmt_player_insert) { sqlite3_finalize(s->stmt_player_insert); s->stmt_player_insert = NULL; }
    if (s->stmt_player_select_by_seat) { sqlite3_finalize(s->stmt_player_select_by_seat); s->stmt_player_select_by_seat = NULL; }
    if (s->stmt_player_select_all) { sqlite3_finalize(s->stmt_player_select_all); s->stmt_player_select_all = NULL; }
    if (s->stmt_player_update) { sqlite3_finalize(s->stmt_player_update); s->stmt_player_update = NULL; }
    if (s->stmt_event_select) { sqlite3_finalize(s->stmt_event_select); s->stmt_event_select = NULL; }
    if (s->stmt_event_insert) { sqlite3_finalize(s->stmt_event_insert); s->stmt_event_insert = NULL; }
    if (s->stmt_event_select_all) { sqlite3_finalize(s->stmt_event_select_all); s->stmt_event_select_all = NULL; }
    if (s->stmt_account_update_elo) { sqlite3_finalize(s->stmt_account_update_elo); s->stmt_account_update_elo = NULL; }
    if (s->stmt_achievement_insert) { sqlite3_finalize(s->stmt_achievement_insert); s->stmt_achievement_insert = NULL; }
    if (s->stmt_account_insert) { sqlite3_finalize(s->stmt_account_insert); s->stmt_account_insert = NULL; }
    if (s->stmt_account_select_by_email) { sqlite3_finalize(s->stmt_account_select_by_email); s->stmt_account_select_by_email = NULL; }
    if (s->stmt_account_select_by_id) { sqlite3_finalize(s->stmt_account_select_by_id); s->stmt_account_select_by_id = NULL; }
    if (s->stmt_profile_stats_init) { sqlite3_finalize(s->stmt_profile_stats_init); s->stmt_profile_stats_init = NULL; }
    if (s->stmt_profile_stats_select) { sqlite3_finalize(s->stmt_profile_stats_select); s->stmt_profile_stats_select = NULL; }
    if (s->stmt_profile_stats_update) { sqlite3_finalize(s->stmt_profile_stats_update); s->stmt_profile_stats_update = NULL; }
    if (s->stmt_profile_stats_insert_fallback) { sqlite3_finalize(s->stmt_profile_stats_insert_fallback); s->stmt_profile_stats_insert_fallback = NULL; }
    if (s->stmt_match_abort) { sqlite3_finalize(s->stmt_match_abort); s->stmt_match_abort = NULL; }
    if (s->stmt_leaderboard) { sqlite3_finalize(s->stmt_leaderboard); s->stmt_leaderboard = NULL; }
    sqlite3_stmt **socialStatements[] = {
        &s->stmt_friend_link_select, &s->stmt_friend_insert, &s->stmt_friend_accept,
        &s->stmt_friend_reject, &s->stmt_friend_list, &s->stmt_profile_badges,
        &s->stmt_profile_modes, &s->stmt_profile_history, &s->stmt_profile_update
    };
    for (size_t i = 0; i < sizeof socialStatements / sizeof socialStatements[0]; i++) {
        /* sqlite3_finalize(NULL) is a no-op, so a partly prepared store is fine. */
        sqlite3_finalize(*socialStatements[i]);
        *socialStatements[i] = NULL;
    }
}

/* Bump whenever EMBEDDED_SCHEMA_SQL changes in a way CREATE TABLE IF NOT EXISTS
 * cannot apply to an existing database file. Tracked in PRAGMA user_version.
 *   1: original schema (achievements as key/progress rows)
 *   2: profile_stats table + achievements(user_id, badge, unlocked_at, match_id)
 *   3: matches.ended_at (finalize time, Unix seconds) for profile history */
#define STORE_SCHEMA_VERSION 3
#define STORE_SCHEMA_END_TIME_VERSION 3

static int query_int(sqlite3 *db, const char *sql, int *out)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    int rc = sqlite3_step(stmt);
    *out = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? 0 : -1;
}

/* Additive only: a v2 file keeps every row. CREATE TABLE IF NOT EXISTS already
 * gave new files the column, so only add it when it is missing. */
static Status add_match_end_time(sqlite3 *db) {
    int hasColumn = 0;
    if (query_int(db, "SELECT count(*) FROM pragma_table_info('matches') WHERE name = 'ended_at';",
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

static Status migrate_schema(Store *s)
{
    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }

    int version = 0;
    if (query_int(s->db, "PRAGMA user_version;", &version) != 0) goto fail;

    if (version < 2) {
        /* v1 stored badge progress as key/value rows that the v2 model derives
         * from profile_stats instead, so the old rows carry nothing to keep. */
        int legacy = 0;
        if (query_int(s->db,
                      "SELECT count(*) FROM pragma_table_info('achievements') WHERE name = 'key';",
                      &legacy) != 0) goto fail;
        if (legacy > 0 &&
            sqlite3_exec(s->db, "DROP TABLE achievements;", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    }

    if (sqlite3_exec(s->db, EMBEDDED_SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) goto fail;

    if (version < 2) {
        /* Accounts created before v2 have no profile_stats row yet. */
        if (sqlite3_exec(s->db,
                         "INSERT OR IGNORE INTO profile_stats (user_id, peak_elo) "
                         "SELECT id, MAX(elo, 1500) FROM accounts;",
                         NULL, NULL, NULL) != SQLITE_OK) goto fail;
    }

    if (version < STORE_SCHEMA_END_TIME_VERSION && add_match_end_time(s->db) != OK) goto fail;

    if (version < STORE_SCHEMA_VERSION) {
        char sql[64];
        snprintf(sql, sizeof(sql), "PRAGMA user_version = %d;", STORE_SCHEMA_VERSION);
        if (sqlite3_exec(s->db, sql, NULL, NULL, NULL) != SQLITE_OK) goto fail;
    }

    if (sqlite3_exec(s->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    return OK;

fail:
    sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
    return STORAGE_ERROR;
}

Status store_open(Store **out, const char *path)
{
    if (!out || !path) return BAD_REQUEST;
    *out = NULL;

    Store *s = calloc(1, sizeof(Store));
    if (!s) return STORAGE_ERROR;

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path, &s->db, flags, NULL) != SQLITE_OK) {
        free(s);
        return STORAGE_ERROR;
    }

    sqlite3_busy_timeout(s->db, 100);

    /* Configure SQLite PRAGMAs */
    char *errmsg = NULL;
    if (sqlite3_exec(s->db, "PRAGMA foreign_keys = ON;", NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
        store_close(s);
        return STORAGE_ERROR;
    }

    /* Set WAL journal mode (for file DBs; in-memory returns "memory") */
    if (sqlite3_exec(s->db, "PRAGMA journal_mode = WAL;", NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
    }

    if (sqlite3_exec(s->db, "PRAGMA busy_timeout = 100;", NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
    }

    /* Execute Schema Migration */
    if (migrate_schema(s) != OK) {
        store_close(s);
        return STORAGE_ERROR;
    }

    /* Crash recovery: convert all MATCH_ACTIVE matches to MATCH_ABORTED */
    const char *sql_recovery = "UPDATE matches SET status = ? WHERE status = ?;";
    sqlite3_stmt *stmt_rec = NULL;
    if (sqlite3_prepare_v2(s->db, sql_recovery, -1, &stmt_rec, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt_rec, 1, MATCH_ABORTED);
        sqlite3_bind_int(stmt_rec, 2, MATCH_ACTIVE);
        sqlite3_step(stmt_rec);
        sqlite3_finalize(stmt_rec);
    }

    if (prepare_statements(s) != OK) {
        store_close(s);
        return STORAGE_ERROR;
    }

    *out = s;
    return OK;
}

void store_close(Store *s)
{
    if (!s) return;
    finalize_statements(s);
    if (s->db) {
        sqlite3_close(s->db);
        s->db = NULL;
    }
    free(s);
}

sqlite3 *store_get_db(Store *s)
{
    return s ? s->db : NULL;
}

Status store_match_start(Store *s, const MatchId *match_id, const PlayerSpec players[4], Mode mode, bool ranked)
{
    if (!s || !match_id || !players) return BAD_REQUEST;
    const char *mid = (const char *)match_id;
    if (strlen(mid) == 0) return BAD_REQUEST;

    sqlite3_stmt *stmt = s->stmt_match_select;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);

    int step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ROW) {
        /* Match already exists: check idempotency */
        Mode existing_mode = (Mode)sqlite3_column_int(stmt, 0);
        bool existing_ranked = sqlite3_column_int(stmt, 1) != 0;
        MatchStatus existing_status = (MatchStatus)sqlite3_column_int(stmt, 2);
        sqlite3_reset(stmt);

        if (existing_mode != mode || existing_ranked != ranked || existing_status != MATCH_ACTIVE) {
            return INVALID_ACTION;
        }

        /* Check players */
        for (int i = 0; i < 4; i++) {
            sqlite3_stmt *pstmt = s->stmt_player_select_by_seat;
            sqlite3_reset(pstmt);
            sqlite3_clear_bindings(pstmt);
            sqlite3_bind_text(pstmt, 1, mid, -1, SQLITE_STATIC);
            sqlite3_bind_int(pstmt, 2, i);
            if (sqlite3_step(pstmt) != SQLITE_ROW) {
                sqlite3_reset(pstmt);
                return INVALID_ACTION;
            }
            UserId uid = (UserId)sqlite3_column_int64(pstmt, 1);
            const char *name = (const char *)sqlite3_column_text(pstmt, 2);
            int is_bot = sqlite3_column_int(pstmt, 5);
            int bot_level = sqlite3_column_int(pstmt, 4);

            if (uid != players[i].userId ||
                strcmp(name ? name : "", players[i].name) != 0 ||
                is_bot != (players[i].isBot ? 1 : 0) ||
                bot_level != (int)players[i].botLevel) {
                sqlite3_reset(pstmt);
                return INVALID_ACTION;
            }
            sqlite3_reset(pstmt);
        }
        return OK;
    }
    sqlite3_reset(stmt);

    /* Begin atomic transaction */
    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }

    uint64_t now_sec = (uint64_t)time(NULL);
    stmt = s->stmt_match_insert;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)mode);
    sqlite3_bind_int(stmt, 3, ranked ? 1 : 0);
    sqlite3_bind_int(stmt, 4, (int)MATCH_ACTIVE);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)now_sec);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_reset(stmt);
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }
    sqlite3_reset(stmt);

    for (int i = 0; i < 4; i++) {
        sqlite3_stmt *pstmt = s->stmt_player_insert;
        sqlite3_reset(pstmt);
        sqlite3_clear_bindings(pstmt);
        sqlite3_bind_text(pstmt, 1, mid, -1, SQLITE_STATIC);
        sqlite3_bind_int(pstmt, 2, i);
        sqlite3_bind_int64(pstmt, 3, (sqlite3_int64)players[i].userId);
        sqlite3_bind_text(pstmt, 4, players[i].name, -1, SQLITE_STATIC);
        sqlite3_bind_text(pstmt, 5, players[i].avatar, -1, SQLITE_STATIC);
        sqlite3_bind_int(pstmt, 6, (int)players[i].botLevel);
        sqlite3_bind_int(pstmt, 7, players[i].isBot ? 1 : 0);

        if (sqlite3_step(pstmt) != SQLITE_DONE) {
            sqlite3_reset(pstmt);
            sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
            return STORAGE_ERROR;
        }
        sqlite3_reset(pstmt);
    }

    if (sqlite3_exec(s->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    return OK;
}

Status store_events(Store *s, const MatchId *match_id, const EventBatch *batch)
{
    if (!s || !match_id || !batch) return BAD_REQUEST;
    const char *mid = (const char *)match_id;

    /* Verify match exists */
    sqlite3_stmt *stmt = s->stmt_match_select;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_reset(stmt);
        return NOT_FOUND;
    }
    sqlite3_reset(stmt);

    if (batch->count == 0) return OK;

    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }

    for (size_t i = 0; i < batch->count; i++) {
        const MatchEvent *ev = &batch->events[i];

        sqlite3_stmt *sel = s->stmt_event_select;
        sqlite3_reset(sel);
        sqlite3_clear_bindings(sel);
        sqlite3_bind_text(sel, 1, mid, -1, SQLITE_STATIC);
        sqlite3_bind_int64(sel, 2, (sqlite3_int64)ev->seq);

        int rc = sqlite3_step(sel);
        if (rc == SQLITE_ROW) {
            /* Already exists: check if identical */
            uint64_t rev = (uint64_t)sqlite3_column_int64(sel, 0);
            int actor = sqlite3_column_int(sel, 1);
            const char *type = (const char *)sqlite3_column_text(sel, 2);
            const char *payload = (const char *)sqlite3_column_text(sel, 3);

            if (rev != ev->revision || actor != ev->actor ||
                strcmp(type ? type : "", ev->type) != 0 ||
                strcmp(payload ? payload : "", ev->payload) != 0) {
                sqlite3_reset(sel);
                sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
                return INVALID_ACTION;
            }
            sqlite3_reset(sel);
            continue; /* Identical event, idempotent */
        }
        sqlite3_reset(sel);

        /* Insert new event */
        sqlite3_stmt *ins = s->stmt_event_insert;
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        sqlite3_bind_text(ins, 1, mid, -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 2, (sqlite3_int64)ev->seq);
        sqlite3_bind_int64(ins, 3, (sqlite3_int64)ev->revision);
        sqlite3_bind_int(ins, 4, ev->actor);
        sqlite3_bind_text(ins, 5, ev->type, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 6, ev->payload, -1, SQLITE_STATIC);

        if (sqlite3_step(ins) != SQLITE_DONE) {
            sqlite3_reset(ins);
            sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
            return STORAGE_ERROR;
        }
        sqlite3_reset(ins);
    }

    if (sqlite3_exec(s->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    return OK;
}

static Status apply_profile_delta(Store *s, UserId uid, const char *mid, const ProfileDelta *d)
{
    if (!d) return OK;

    /* Update profile_stats */
    sqlite3_stmt *stmt = s->stmt_profile_stats_update;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)d->add.matches);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)d->add.wins);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)d->add.teamWins);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)d->add.plus4);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)d->add.unoCalls);
    sqlite3_bind_int(stmt, 6, d->streak);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)d->bestStreak);
    sqlite3_bind_int(stmt, 8, d->peakElo);
    sqlite3_bind_int64(stmt, 9, (sqlite3_int64)uid);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_reset(stmt);
        return STORAGE_ERROR;
    }
    sqlite3_reset(stmt);

    /* Fallback insert if row didn't exist */
    if (sqlite3_changes(s->db) == 0) {
        sqlite3_stmt *fb = s->stmt_profile_stats_insert_fallback;
        sqlite3_reset(fb);
        sqlite3_clear_bindings(fb);
        sqlite3_bind_int64(fb, 1, (sqlite3_int64)uid);
        sqlite3_bind_int64(fb, 2, (sqlite3_int64)d->add.matches);
        sqlite3_bind_int64(fb, 3, (sqlite3_int64)d->add.wins);
        sqlite3_bind_int64(fb, 4, (sqlite3_int64)d->add.teamWins);
        sqlite3_bind_int64(fb, 5, (sqlite3_int64)d->add.plus4);
        sqlite3_bind_int64(fb, 6, (sqlite3_int64)d->add.unoCalls);
        sqlite3_bind_int(fb, 7, d->streak);
        sqlite3_bind_int64(fb, 8, (sqlite3_int64)d->bestStreak);
        sqlite3_bind_int(fb, 9, d->peakElo);

        if (sqlite3_step(fb) != SQLITE_DONE) {
            sqlite3_reset(fb);
            return STORAGE_ERROR;
        }
        sqlite3_reset(fb);
    }

    /* Insert unlocked badge bits into achievements */
    if (d->unlocked != 0) {
        for (int b = 0; b < 32; b++) {
            uint32_t bit = 1u << b;
            if (d->unlocked & bit) {
                sqlite3_stmt *astmt = s->stmt_achievement_insert;
                sqlite3_reset(astmt);
                sqlite3_clear_bindings(astmt);
                sqlite3_bind_int64(astmt, 1, (sqlite3_int64)uid);
                sqlite3_bind_int(astmt, 2, (int)bit);
                sqlite3_bind_int64(astmt, 3, (sqlite3_int64)time(NULL));
                sqlite3_bind_text(astmt, 4, mid, -1, SQLITE_STATIC);

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

Status store_finalize(Store *s, const MatchResult *result, const ProfileDelta deltas[4])
{
    if (!s || !result) return BAD_REQUEST;
    const char *mid = (const char *)result->matchId;

    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }

    sqlite3_stmt *stmt = s->stmt_match_select;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(stmt);
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return NOT_FOUND;
    }

    MatchStatus cur_status = (MatchStatus)sqlite3_column_int(stmt, 2);
    int cur_winner_seat = sqlite3_column_int(stmt, 3);
    int cur_winner_team = sqlite3_column_int(stmt, 4);
    uint64_t cur_duration = (uint64_t)sqlite3_column_int64(stmt, 5);
    sqlite3_reset(stmt);

    if (cur_status == MATCH_FINALIZED) {
        /* Check if identical */
        if (cur_winner_seat != result->winnerSeat ||
            cur_winner_team != result->winnerTeam ||
            cur_duration != result->durationMs) {
            sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
            return INVALID_ACTION;
        }

        /* Check each player's points and elo */
        for (int i = 0; i < 4; i++) {
            sqlite3_stmt *pstmt = s->stmt_player_select_by_seat;
            sqlite3_reset(pstmt);
            sqlite3_clear_bindings(pstmt);
            sqlite3_bind_text(pstmt, 1, mid, -1, SQLITE_STATIC);
            sqlite3_bind_int(pstmt, 2, i);

            if (sqlite3_step(pstmt) != SQLITE_ROW) {
                sqlite3_reset(pstmt);
                sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
                return INVALID_ACTION;
            }

            int points = sqlite3_column_int(pstmt, 7);
            int elo_before = sqlite3_column_int(pstmt, 8);
            int elo_after = sqlite3_column_int(pstmt, 9);
            sqlite3_reset(pstmt);

            if (points != result->players[i].points ||
                elo_before != result->players[i].eloBefore ||
                elo_after != result->players[i].eloAfter) {
                sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
                return INVALID_ACTION;
            }
        }

        /* Idempotent finalize: do not re-apply stats or ELO */
        sqlite3_exec(s->db, "COMMIT;", NULL, NULL, NULL);
        return OK;
    }

    if (cur_status != MATCH_ACTIVE) {
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return INVALID_ACTION;
    }

    /* Update matches status */
    stmt = s->stmt_match_update_status;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int(stmt, 1, (int)MATCH_FINALIZED);
    sqlite3_bind_int(stmt, 2, result->winnerSeat);
    sqlite3_bind_int(stmt, 3, result->winnerTeam);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)result->durationMs);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(stmt, 6, mid, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_reset(stmt);
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }
    sqlite3_reset(stmt);

    /* Update match_players stats & elo */
    for (int i = 0; i < 4; i++) {
        const PlayerResult *pr = &result->players[i];
        sqlite3_stmt *pstmt = s->stmt_player_update;
        sqlite3_reset(pstmt);
        sqlite3_clear_bindings(pstmt);

        sqlite3_bind_int(pstmt, 1, pr->team);
        sqlite3_bind_int(pstmt, 2, pr->points);
        sqlite3_bind_int(pstmt, 3, pr->eloBefore);
        sqlite3_bind_int(pstmt, 4, pr->eloAfter);
        sqlite3_bind_int(pstmt, 5, pr->left ? 1 : 0);
        sqlite3_bind_int(pstmt, 6, (int)pr->stats.played);
        sqlite3_bind_int(pstmt, 7, (int)pr->stats.attacks);
        sqlite3_bind_int(pstmt, 8, (int)pr->stats.unoCalls);
        sqlite3_bind_int(pstmt, 9, (int)pr->stats.penalties);
        sqlite3_bind_int(pstmt, 10, (int)pr->stats.plus4);
        sqlite3_bind_text(pstmt, 11, mid, -1, SQLITE_STATIC);
        sqlite3_bind_int(pstmt, 12, i);

        if (sqlite3_step(pstmt) != SQLITE_DONE) {
            sqlite3_reset(pstmt);
            sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
            return STORAGE_ERROR;
        }
        sqlite3_reset(pstmt);

        /* Update accounts ELO if ranked and userId > 0 */
        if (result->ranked && pr->userId > 0) {
            sqlite3_stmt *astmt = s->stmt_account_update_elo;
            sqlite3_reset(astmt);
            sqlite3_clear_bindings(astmt);
            sqlite3_bind_int(astmt, 1, pr->eloAfter);
            sqlite3_bind_int64(astmt, 2, (sqlite3_int64)pr->userId);

            if (sqlite3_step(astmt) != SQLITE_DONE) {
                sqlite3_reset(astmt);
                sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
                return STORAGE_ERROR;
            }
            sqlite3_reset(astmt);
        }

        /* Update profile deltas if provided and userId > 0 */
        if (deltas && pr->userId > 0) {
            if (apply_profile_delta(s, pr->userId, mid, &deltas[i]) != OK) {
                sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
                return STORAGE_ERROR;
            }
        }
    }

    if (sqlite3_exec(s->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    return OK;
}

/* ROLLBACK can only fail when no transaction is open, which is the state wanted. */
static Status rollback_with(sqlite3 *db, Status status) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return status;
}

static Status read_match_status(Store *s, const char *matchId, MatchStatus *out) {
    sqlite3_stmt *stmt = s->stmt_match_select;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (sqlite3_bind_text(stmt, 1, matchId, -1, SQLITE_STATIC) != SQLITE_OK) return STORAGE_ERROR;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) *out = (MatchStatus)sqlite3_column_int(stmt, 2);
    sqlite3_reset(stmt);
    if (rc == SQLITE_ROW) return OK;
    return rc == SQLITE_DONE ? NOT_FOUND : STORAGE_ERROR;
}

static Status insert_event(Store *s, const char *matchId, const MatchEvent *event) {
    sqlite3_stmt *stmt = s->stmt_event_insert;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_text(stmt, 1, matchId, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event->seq) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)event->revision) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 4, event->actor) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 5, event->type, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 6, event->payload, -1, SQLITE_STATIC) == SQLITE_OK;
    int rc = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    return rc == SQLITE_DONE ? OK : STORAGE_ERROR;
}

static Status mark_aborted(Store *s, const char *matchId, uint64_t durationMs) {
    sqlite3_stmt *stmt = s->stmt_match_abort;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_int(stmt, 1, MATCH_ABORTED) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)durationMs) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 3, matchId, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 4, MATCH_ACTIVE) == SQLITE_OK;
    int rc = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(s->db) == 1 ? OK : STORAGE_ERROR;
}

Status store_abort(Store *s, const MatchId *id, const MatchEvent *event, uint64_t duration_ms) {
    if (!s || !id || !event) return BAD_REQUEST;
    const char *matchId = (const char *)id;

    /* Event and status commit together: otherwise a failure between them leaves
     * an ABORTED match without its abort event, or an event on an ACTIVE match. */
    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }
    MatchStatus current = MATCH_ACTIVE;
    Status status = read_match_status(s, matchId, &current);
    if (status != OK) return rollback_with(s->db, status);
    /* The coordinator retries until OK; a retry after a committed abort (or
     * after crash recovery marked the match) must not append a second event. */
    if (current == MATCH_ABORTED) return rollback_with(s->db, OK);
    if (current != MATCH_ACTIVE) return rollback_with(s->db, INVALID_ACTION);

    status = insert_event(s, matchId, event);
    if (status == OK) status = mark_aborted(s, matchId, duration_ms);
    if (status != OK) return rollback_with(s->db, status);

    if (sqlite3_exec(s->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        return rollback_with(s->db, STORAGE_ERROR);
    }
    return OK;
}

Status store_leaderboard(Store *s, LeaderboardEntry out[STORE_LEADERBOARD_MAX], size_t *count) {
    if (!s || !out || !count) return BAD_REQUEST;

    sqlite3_stmt *stmt = s->stmt_leaderboard;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (sqlite3_bind_int(stmt, 1, STORE_LEADERBOARD_MAX) != SQLITE_OK) return STORAGE_ERROR;

    size_t rows = 0;
    int rc = SQLITE_DONE;
    while (rows < STORE_LEADERBOARD_MAX && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
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
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) return STORAGE_ERROR;

    *count = rows;
    return OK;
}

static Status account_exists(Store *s, UserId id) {
    Account account;
    return store_account_get_by_id(s, id, &account);
}

static Status read_friend_link(Store *s, UserId first, UserId second, bool *linked) {
    sqlite3_stmt *stmt = s->stmt_friend_link_select;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)first) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)second) == SQLITE_OK;
    int rc = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return STORAGE_ERROR;
    *linked = rc == SQLITE_ROW;
    return OK;
}

static Status insert_friend_request(Store *s, UserId from, UserId to) {
    sqlite3_stmt *stmt = s->stmt_friend_insert;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)from) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)to) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 3, FRIEND_STATUS_PENDING) == SQLITE_OK;
    int rc = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    return rc == SQLITE_DONE ? OK : STORAGE_ERROR;
}

Status store_friend_request(Store *s, UserId from, UserId to) {
    if (!s || from == to) return BAD_REQUEST;

    /* The reverse-direction check and the insert must see the same state:
     * otherwise two crossing requests could both pass and leave A->B and B->A. */
    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }
    Status status = account_exists(s, to);
    if (status != OK) return rollback_with(s->db, status);

    bool linked = false;
    status = read_friend_link(s, from, to, &linked);
    if (status != OK) return rollback_with(s->db, status);
    if (linked) return rollback_with(s->db, INVALID_ACTION);

    status = insert_friend_request(s, from, to);
    if (status != OK) return rollback_with(s->db, status);

    if (sqlite3_exec(s->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        return rollback_with(s->db, STORAGE_ERROR);
    }
    return OK;
}

Status store_friend_reply(Store *s, UserId me, UserId requester, bool accept) {
    if (!s) return BAD_REQUEST;

    /* One guarded statement per answer: no transaction is needed, and a row
     * count of 0 means there was no pending requester -> me request. */
    sqlite3_stmt *stmt = accept ? s->stmt_friend_accept : s->stmt_friend_reject;
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
    int rc = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    if (rc != SQLITE_DONE) return STORAGE_ERROR;
    return sqlite3_changes(s->db) == 1 ? OK : NOT_FOUND;
}

Status store_friend_list(Store *s, UserId me, FriendRow out[STORE_FRIEND_MAX], size_t *count) {
    if (!s || !out || !count) return BAD_REQUEST;

    sqlite3_stmt *stmt = s->stmt_friend_list;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)me) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 2, STORE_FRIEND_MAX) == SQLITE_OK;
    if (!bound) return STORAGE_ERROR;

    size_t rows = 0;
    int rc = SQLITE_DONE;
    while (rows < STORE_FRIEND_MAX && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
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
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) return STORAGE_ERROR;

    *count = rows;
    return OK;
}

/* Binds ?1 user, ?2 MATCH_FINALIZED, ?3/?4 the team modes shared by the
 * PROFILE_WON_SQL / PROFILE_MATCHES_SQL fragments. */
static bool bind_profile_matches(sqlite3_stmt *stmt, UserId id) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 2, MATCH_FINALIZED) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 3, TWO_V_TWO) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 4, SIDE) == SQLITE_OK;
}

static Status read_profile_badges(Store *s, UserId id, uint32_t *badges) {
    sqlite3_stmt *stmt = s->stmt_profile_badges;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id) != SQLITE_OK) return STORAGE_ERROR;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        *badges |= (uint32_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_reset(stmt);
    return rc == SQLITE_DONE ? OK : STORAGE_ERROR;
}

static Status read_profile_modes(Store *s, UserId id, Profile *out) {
    sqlite3_stmt *stmt = s->stmt_profile_modes;
    if (!bind_profile_matches(stmt, id)) return STORAGE_ERROR;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int mode = sqlite3_column_int(stmt, 0);
        /* A row from a mode this build does not know is skipped, not trusted. */
        if (mode < 0 || mode >= STORE_MODE_COUNT) continue;
        out->modes[mode].matches = (uint32_t)sqlite3_column_int64(stmt, 1);
        out->modes[mode].wins = (uint32_t)sqlite3_column_int64(stmt, 2);
    }
    sqlite3_reset(stmt);
    return rc == SQLITE_DONE ? OK : STORAGE_ERROR;
}

static Status read_profile_history(Store *s, UserId id, Profile *out) {
    sqlite3_stmt *stmt = s->stmt_profile_history;
    if (!bind_profile_matches(stmt, id) ||
        sqlite3_bind_int(stmt, 5, STORE_HISTORY_MAX) != SQLITE_OK) return STORAGE_ERROR;
    int rc = SQLITE_DONE;
    while (out->historyCount < STORE_HISTORY_MAX && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        HistoryRow *row = &out->history[out->historyCount++];
        const char *matchId = (const char *)sqlite3_column_text(stmt, 0);
        snprintf(row->matchId, sizeof row->matchId, "%s", matchId ? matchId : "");
        row->mode = (Mode)sqlite3_column_int(stmt, 1);
        row->won = sqlite3_column_int(stmt, 2) != 0;
        row->eloDelta = sqlite3_column_int(stmt, 3);
        row->endedAt = (int64_t)sqlite3_column_int64(stmt, 4);
    }
    sqlite3_reset(stmt);
    return rc == SQLITE_DONE || rc == SQLITE_ROW ? OK : STORAGE_ERROR;
}

Status store_profile_get(Store *s, UserId id, Profile *out) {
    if (!s || !out) return BAD_REQUEST;

    Account account;
    Status status = store_account_get_by_id(s, id, &account);
    if (status != OK) return status;

    /* Copy only the public fields: email and hash stay in the local Account. */
    memset(out, 0, sizeof *out);
    out->userId = account.id;
    snprintf(out->name, sizeof out->name, "%s", account.name);
    snprintf(out->avatar, sizeof out->avatar, "%s", account.avatar);
    out->elo = account.elo;

    status = store_profile_stats(s, id, &out->stats);
    /* Every account gets a profile_stats row at creation or v2 migration;
     * a missing one just means nothing has been recorded yet. */
    if (status == NOT_FOUND) status = OK;
    if (status == OK) status = read_profile_badges(s, id, &out->badges);
    if (status == OK) status = read_profile_modes(s, id, out);
    if (status == OK) status = read_profile_history(s, id, out);
    return status;
}

Status store_profile_update(Store *s, UserId id, const char *name, const char *avatar) {
    if (!s || !name || !avatar) return BAD_REQUEST;

    sqlite3_stmt *stmt = s->stmt_profile_update;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bool bound = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 2, avatar, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)id) == SQLITE_OK;
    int rc = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
    sqlite3_reset(stmt);
    if (rc != SQLITE_DONE) return STORAGE_ERROR;
    return sqlite3_changes(s->db) == 1 ? OK : NOT_FOUND;
}

Status store_export_match(Store *s, const MatchId *match_id, const char *path)
{
    if (!s || !match_id || !path) return BAD_REQUEST;
    const char *mid = (const char *)match_id;

    sqlite3_stmt *m_stmt = s->stmt_match_select;
    sqlite3_reset(m_stmt);
    sqlite3_clear_bindings(m_stmt);
    sqlite3_bind_text(m_stmt, 1, mid, -1, SQLITE_STATIC);

    if (sqlite3_step(m_stmt) != SQLITE_ROW) {
        sqlite3_reset(m_stmt);
        return NOT_FOUND;
    }

    int mode = sqlite3_column_int(m_stmt, 0);
    int ranked = sqlite3_column_int(m_stmt, 1);
    int status = sqlite3_column_int(m_stmt, 2);
    int winner_seat = sqlite3_column_int(m_stmt, 3);
    int winner_team = sqlite3_column_int(m_stmt, 4);
    uint64_t duration = (uint64_t)sqlite3_column_int64(m_stmt, 5);
    uint64_t created_at = (uint64_t)sqlite3_column_int64(m_stmt, 6);
    sqlite3_reset(m_stmt);

    struct json_object *root = json_object_new_object();
    json_object_object_add(root, "matchId", json_object_new_string(mid));
    json_object_object_add(root, "mode", json_object_new_int(mode));
    json_object_object_add(root, "ranked", json_object_new_boolean(ranked ? 1 : 0));
    json_object_object_add(root, "status", json_object_new_int(status));
    json_object_object_add(root, "winnerSeat", json_object_new_int(winner_seat));
    json_object_object_add(root, "winnerTeam", json_object_new_int(winner_team));
    json_object_object_add(root, "durationMs", json_object_new_int64((int64_t)duration));
    json_object_object_add(root, "createdAt", json_object_new_int64((int64_t)created_at));

    /* Query match players */
    struct json_object *players_arr = json_object_new_array();
    sqlite3_stmt *p_stmt = s->stmt_player_select_all;
    sqlite3_reset(p_stmt);
    sqlite3_clear_bindings(p_stmt);
    sqlite3_bind_text(p_stmt, 1, mid, -1, SQLITE_STATIC);

    while (sqlite3_step(p_stmt) == SQLITE_ROW) {
        struct json_object *p = json_object_new_object();
        json_object_object_add(p, "seat", json_object_new_int(sqlite3_column_int(p_stmt, 0)));
        json_object_object_add(p, "userId", json_object_new_int64(sqlite3_column_int64(p_stmt, 1)));

        const char *name = (const char *)sqlite3_column_text(p_stmt, 2);
        json_object_object_add(p, "name", json_object_new_string(name ? name : ""));

        const char *avatar = (const char *)sqlite3_column_text(p_stmt, 3);
        json_object_object_add(p, "avatar", json_object_new_string(avatar ? avatar : ""));

        json_object_object_add(p, "botLevel", json_object_new_int(sqlite3_column_int(p_stmt, 4)));
        json_object_object_add(p, "isBot", json_object_new_boolean(sqlite3_column_int(p_stmt, 5) ? 1 : 0));
        json_object_object_add(p, "team", json_object_new_int(sqlite3_column_int(p_stmt, 6)));
        json_object_object_add(p, "points", json_object_new_int(sqlite3_column_int(p_stmt, 7)));
        json_object_object_add(p, "eloBefore", json_object_new_int(sqlite3_column_int(p_stmt, 8)));
        json_object_object_add(p, "eloAfter", json_object_new_int(sqlite3_column_int(p_stmt, 9)));
        json_object_object_add(p, "left", json_object_new_boolean(sqlite3_column_int(p_stmt, 10) ? 1 : 0));

        struct json_object *stats = json_object_new_object();
        json_object_object_add(stats, "played", json_object_new_int(sqlite3_column_int(p_stmt, 11)));
        json_object_object_add(stats, "attacks", json_object_new_int(sqlite3_column_int(p_stmt, 12)));
        json_object_object_add(stats, "unoCalls", json_object_new_int(sqlite3_column_int(p_stmt, 13)));
        json_object_object_add(stats, "penalties", json_object_new_int(sqlite3_column_int(p_stmt, 14)));
        json_object_object_add(stats, "plus4", json_object_new_int(sqlite3_column_int(p_stmt, 15)));
        json_object_object_add(p, "stats", stats);

        json_object_array_add(players_arr, p);
    }
    sqlite3_reset(p_stmt);
    json_object_object_add(root, "players", players_arr);

    /* Query match events */
    struct json_object *events_arr = json_object_new_array();
    sqlite3_stmt *e_stmt = s->stmt_event_select_all;
    sqlite3_reset(e_stmt);
    sqlite3_clear_bindings(e_stmt);
    sqlite3_bind_text(e_stmt, 1, mid, -1, SQLITE_STATIC);

    while (sqlite3_step(e_stmt) == SQLITE_ROW) {
        struct json_object *ev = json_object_new_object();
        json_object_object_add(ev, "seq", json_object_new_int64(sqlite3_column_int64(e_stmt, 0)));
        json_object_object_add(ev, "revision", json_object_new_int64(sqlite3_column_int64(e_stmt, 1)));
        json_object_object_add(ev, "actor", json_object_new_int(sqlite3_column_int(e_stmt, 2)));

        const char *etype = (const char *)sqlite3_column_text(e_stmt, 3);
        json_object_object_add(ev, "type", json_object_new_string(etype ? etype : ""));

        const char *payload_str = (const char *)sqlite3_column_text(e_stmt, 4);
        if (payload_str) {
            struct json_object *parsed_payload = json_tokener_parse(payload_str);
            if (parsed_payload) {
                json_object_object_add(ev, "payload", parsed_payload);
            } else {
                json_object_object_add(ev, "payload", json_object_new_string(payload_str));
            }
        } else {
            json_object_object_add(ev, "payload", json_object_new_object());
        }

        json_object_array_add(events_arr, ev);
    }
    sqlite3_reset(e_stmt);
    json_object_object_add(root, "events", events_arr);

    int dump_res = json_object_to_file_ext(path, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);

    if (dump_res != 0) return STORAGE_ERROR;
    return OK;
}

Status store_account_create(Store *s, const AccountCreate *c, UserId *out_id)
{
    if (!s || !c || !out_id) return BAD_REQUEST;

    /* The account row and its profile_stats row must be created together:
     * otherwise a failed second insert leaves an account whose email is taken
     * while the client was told registration failed. */
    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        return STORAGE_ERROR;
    }

    sqlite3_reset(s->stmt_account_insert);
    sqlite3_clear_bindings(s->stmt_account_insert);

    sqlite3_bind_text(s->stmt_account_insert, 1, c->email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s->stmt_account_insert, 2, c->hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s->stmt_account_insert, 3, c->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s->stmt_account_insert, 4, c->avatar, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(s->stmt_account_insert);
    sqlite3_reset(s->stmt_account_insert);
    if (rc == SQLITE_CONSTRAINT) {
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return BAD_REQUEST; /* email already taken */
    }
    if (rc != SQLITE_DONE) {
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    UserId new_id = (UserId)sqlite3_last_insert_rowid(s->db);

    sqlite3_stmt *p_init = s->stmt_profile_stats_init;
    sqlite3_reset(p_init);
    sqlite3_clear_bindings(p_init);
    sqlite3_bind_int64(p_init, 1, (sqlite3_int64)new_id);
    rc = sqlite3_step(p_init);
    sqlite3_reset(p_init);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    if (sqlite3_exec(s->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, NULL);
        return STORAGE_ERROR;
    }

    *out_id = new_id;
    return OK;
}

Status store_account_get(Store *s, const char *email, Account *out)
{
    if (!s || !email || !out) return BAD_REQUEST;

    sqlite3_reset(s->stmt_account_select_by_email);
    sqlite3_clear_bindings(s->stmt_account_select_by_email);

    sqlite3_bind_text(s->stmt_account_select_by_email, 1, email, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(s->stmt_account_select_by_email);
    if (rc == SQLITE_ROW) {
        memset(out, 0, sizeof(*out));
        out->id = (UserId)sqlite3_column_int64(s->stmt_account_select_by_email, 0);

        const char *db_email = (const char *)sqlite3_column_text(s->stmt_account_select_by_email, 1);
        if (db_email) strncpy(out->email, db_email, sizeof(out->email) - 1);

        const char *db_hash = (const char *)sqlite3_column_text(s->stmt_account_select_by_email, 2);
        if (db_hash) strncpy(out->hash, db_hash, sizeof(out->hash) - 1);

        const char *db_name = (const char *)sqlite3_column_text(s->stmt_account_select_by_email, 3);
        if (db_name) strncpy(out->name, db_name, sizeof(out->name) - 1);

        const char *db_avatar = (const char *)sqlite3_column_text(s->stmt_account_select_by_email, 4);
        if (db_avatar) strncpy(out->avatar, db_avatar, sizeof(out->avatar) - 1);

        out->elo = sqlite3_column_int(s->stmt_account_select_by_email, 5);

        sqlite3_reset(s->stmt_account_select_by_email);
        return OK;
    }
    sqlite3_reset(s->stmt_account_select_by_email);
    if (rc == SQLITE_DONE) return NOT_FOUND;
    return STORAGE_ERROR;
}

Status store_account_get_by_id(Store *s, UserId id, Account *out)
{
    if (!s || !out) return BAD_REQUEST;

    sqlite3_reset(s->stmt_account_select_by_id);
    sqlite3_clear_bindings(s->stmt_account_select_by_id);

    sqlite3_bind_int64(s->stmt_account_select_by_id, 1, (sqlite3_int64)id);

    int rc = sqlite3_step(s->stmt_account_select_by_id);
    if (rc == SQLITE_ROW) {
        memset(out, 0, sizeof(*out));
        out->id = (UserId)sqlite3_column_int64(s->stmt_account_select_by_id, 0);

        const char *db_email = (const char *)sqlite3_column_text(s->stmt_account_select_by_id, 1);
        if (db_email) strncpy(out->email, db_email, sizeof(out->email) - 1);

        const char *db_hash = (const char *)sqlite3_column_text(s->stmt_account_select_by_id, 2);
        if (db_hash) strncpy(out->hash, db_hash, sizeof(out->hash) - 1);

        const char *db_name = (const char *)sqlite3_column_text(s->stmt_account_select_by_id, 3);
        if (db_name) strncpy(out->name, db_name, sizeof(out->name) - 1);

        const char *db_avatar = (const char *)sqlite3_column_text(s->stmt_account_select_by_id, 4);
        if (db_avatar) strncpy(out->avatar, db_avatar, sizeof(out->avatar) - 1);

        out->elo = sqlite3_column_int(s->stmt_account_select_by_id, 5);

        sqlite3_reset(s->stmt_account_select_by_id);
        return OK;
    }
    sqlite3_reset(s->stmt_account_select_by_id);
    if (rc == SQLITE_DONE) return NOT_FOUND;
    return STORAGE_ERROR;
}

Status store_profile_stats(Store *s, UserId user_id, ProfileStats *out)
{
    if (!s || !out) return BAD_REQUEST;

    sqlite3_reset(s->stmt_profile_stats_select);
    sqlite3_clear_bindings(s->stmt_profile_stats_select);

    sqlite3_bind_int64(s->stmt_profile_stats_select, 1, (sqlite3_int64)user_id);

    int rc = sqlite3_step(s->stmt_profile_stats_select);
    if (rc == SQLITE_ROW) {
        memset(out, 0, sizeof(*out));
        out->matches = (uint32_t)sqlite3_column_int64(s->stmt_profile_stats_select, 0);
        out->wins = (uint32_t)sqlite3_column_int64(s->stmt_profile_stats_select, 1);
        out->teamWins = (uint32_t)sqlite3_column_int64(s->stmt_profile_stats_select, 2);
        out->plus4 = (uint32_t)sqlite3_column_int64(s->stmt_profile_stats_select, 3);
        out->unoCalls = (uint32_t)sqlite3_column_int64(s->stmt_profile_stats_select, 4);
        out->streak = (int32_t)sqlite3_column_int(s->stmt_profile_stats_select, 5);
        out->bestStreak = (uint32_t)sqlite3_column_int64(s->stmt_profile_stats_select, 6);
        out->peakElo = (int32_t)sqlite3_column_int(s->stmt_profile_stats_select, 7);

        sqlite3_reset(s->stmt_profile_stats_select);
        return OK;
    }
    sqlite3_reset(s->stmt_profile_stats_select);
    if (rc == SQLITE_DONE) return NOT_FOUND;
    return STORAGE_ERROR;
}
