#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <json-c/json.h>
#include <sqlite3.h>
#include <stddef.h>
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

/* Giá trị cột friends.status; lời mời bị từ chối thì xóa hàng nên không có giá trị riêng. */
enum { FRIEND_STATUS_PENDING = 0, FRIEND_STATUS_ACCEPTED = 1 };

enum {
    MS_PER_SECOND = 1000,
    /* Chờ khóa ghi của SQLite tối đa 100 ms rồi mới báo SQLITE_BUSY. */
    BUSY_TIMEOUT_MS = 100,
    /* Số bit của mặt nạ BadgeBit (uint32_t). */
    BADGE_BIT_COUNT = 32,
    SQL_TEXT_BUFFER_SIZE = 160,
    /* Phiên bản schema, lưu trong PRAGMA user_version. Tăng khi EMBEDDED_SCHEMA_SQL
     * đổi theo cách CREATE TABLE IF NOT EXISTS không áp được lên file DB cũ.
     *   1: schema gốc (achievements dạng hàng key/progress)
     *   2: thêm bảng profile_stats, achievements(user_id, badge, unlocked_at, match_id)
     *   3: thêm matches.ended_at (thời điểm chốt trận, giây Unix) cho lịch sử hồ sơ */
    STORE_SCHEMA_PROFILE_VERSION = 2,
    STORE_SCHEMA_END_TIME_VERSION = 3,
    STORE_SCHEMA_VERSION = 3
};

/* Thứ tự cột của matchSelectStmt. */
enum {
    MATCH_COLUMN_MODE, MATCH_COLUMN_RANKED, MATCH_COLUMN_STATUS, MATCH_COLUMN_WINNER_SEAT,
    MATCH_COLUMN_WINNER_TEAM, MATCH_COLUMN_DURATION, MATCH_COLUMN_CREATED_AT
};

/* Thứ tự cột của playerSelectBySeatStmt và playerSelectAllStmt. */
enum {
    PLAYER_COLUMN_SEAT, PLAYER_COLUMN_USER_ID, PLAYER_COLUMN_NAME, PLAYER_COLUMN_AVATAR,
    PLAYER_COLUMN_BOT_LEVEL, PLAYER_COLUMN_IS_BOT, PLAYER_COLUMN_TEAM, PLAYER_COLUMN_POINTS,
    PLAYER_COLUMN_ELO_BEFORE, PLAYER_COLUMN_ELO_AFTER, PLAYER_COLUMN_LEFT, PLAYER_COLUMN_PLAYED,
    PLAYER_COLUMN_ATTACKS, PLAYER_COLUMN_UNO_CALLS, PLAYER_COLUMN_PENALTIES, PLAYER_COLUMN_PLUS4
};

/* Thứ tự cột của eventSelectStmt (một sự kiện theo seq). */
enum { EVENT_COLUMN_REVISION, EVENT_COLUMN_ACTOR, EVENT_COLUMN_TYPE, EVENT_COLUMN_PAYLOAD };

/* Thứ tự cột của eventSelectAllStmt. */
enum {
    EVENT_ROW_COLUMN_SEQ, EVENT_ROW_COLUMN_REVISION, EVENT_ROW_COLUMN_ACTOR,
    EVENT_ROW_COLUMN_TYPE, EVENT_ROW_COLUMN_PAYLOAD
};

/* Thứ tự cột của hai câu SELECT tài khoản. */
enum {
    ACCOUNT_COLUMN_ID, ACCOUNT_COLUMN_EMAIL, ACCOUNT_COLUMN_HASH, ACCOUNT_COLUMN_NAME,
    ACCOUNT_COLUMN_AVATAR, ACCOUNT_COLUMN_ELO
};

/* Thứ tự cột của profileStatsSelectStmt. */
enum {
    STATS_COLUMN_MATCHES, STATS_COLUMN_WINS, STATS_COLUMN_TEAM_WINS, STATS_COLUMN_PLUS4,
    STATS_COLUMN_UNO_CALLS, STATS_COLUMN_STREAK, STATS_COLUMN_BEST_STREAK, STATS_COLUMN_PEAK_ELO
};

/* Thứ tự cột của leaderboardStmt và friendListStmt (bốn cột đầu giống nhau). */
enum {
    PUBLIC_COLUMN_ID, PUBLIC_COLUMN_NAME, PUBLIC_COLUMN_AVATAR, PUBLIC_COLUMN_ELO,
    FRIEND_COLUMN_SENDER, FRIEND_COLUMN_STATUS
};

/* Cột của profileModesStmt, profileHistoryStmt và profileBadgesStmt. */
enum { MODES_COLUMN_MODE, MODES_COLUMN_MATCHES, MODES_COLUMN_WINS };
enum {
    HISTORY_COLUMN_MATCH_ID, HISTORY_COLUMN_MODE, HISTORY_COLUMN_WON, HISTORY_COLUMN_ELO_DELTA,
    HISTORY_COLUMN_ENDED_AT
};
enum { BADGE_COLUMN = 0, COUNT_COLUMN = 0 };

/* Ghế thắng theo đội ở chế độ đồng đội (?3, ?4 = TWO_V_TWO, SIDE) và theo ghế ở
 * các chế độ còn lại; -1 nghĩa là không có người thắng. Người đã rời trận không
 * bao giờ thắng, khớp với cách profileCompute đếm profile_stats.wins. */
#define PROFILE_WON_SQL \
    "(mp.player_left = 0 AND CASE WHEN m.mode IN (?3, ?4) " \
    "THEN m.winner_team >= 0 AND mp.team = m.winner_team " \
    "ELSE m.winner_seat >= 0 AND mp.seat = m.winner_seat END)"

/* ?1 = mã user, ?2 = MATCH_FINALIZED: chỉ trận đã chốt mới vào hồ sơ. */
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

/* Một câu lệnh được biên dịch sẵn: vị trí con trỏ trong Store và câu SQL của nó. */
typedef struct {
    size_t offset;
    const char *sql;
} StatementSpec;

#define STATEMENT(field, text) {offsetof(Store, field), text}

/* Mọi câu SQL được biên dịch một lần lúc mở DB: luồng poll không bao giờ tốn thời
 * gian parse SQL giữa trận, và nếu schema lệch với SQL thì storeOpen báo lỗi ngay. */
static const StatementSpec STATEMENTS[] = {
    STATEMENT(matchSelectStmt,
        "SELECT mode, ranked, status, winner_seat, winner_team, duration_ms, created_at FROM matches WHERE match_id = ?;"),
    STATEMENT(matchInsertStmt,
        "INSERT INTO matches (match_id, mode, ranked, status, winner_seat, winner_team, duration_ms, created_at) "
        "VALUES (?, ?, ?, ?, -1, -1, 0, ?);"),
    STATEMENT(matchUpdateStatusStmt,
        "UPDATE matches SET status = ?, winner_seat = ?, winner_team = ?, duration_ms = ?, "
        "ended_at = ? WHERE match_id = ?;"),
    STATEMENT(playerInsertStmt,
        "INSERT INTO match_players (match_id, seat, user_id, name, avatar, bot_level, is_bot, team, points, elo_before, elo_after) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, -1, 0, 1500, 1500);"),
    STATEMENT(playerSelectBySeatStmt,
        "SELECT seat, user_id, name, avatar, bot_level, is_bot, team, points, elo_before, elo_after, "
        "player_left, played, attacks, uno_calls, penalties, plus4 "
        "FROM match_players WHERE match_id = ? AND seat = ?;"),
    STATEMENT(playerSelectAllStmt,
        "SELECT seat, user_id, name, avatar, bot_level, is_bot, team, points, elo_before, elo_after, "
        "player_left, played, attacks, uno_calls, penalties, plus4 "
        "FROM match_players WHERE match_id = ? ORDER BY seat ASC;"),
    STATEMENT(playerUpdateStmt,
        "UPDATE match_players SET team = ?, points = ?, elo_before = ?, elo_after = ?, "
        "player_left = ?, played = ?, attacks = ?, uno_calls = ?, penalties = ?, plus4 = ? "
        "WHERE match_id = ? AND seat = ?;"),
    STATEMENT(eventSelectStmt,
        "SELECT revision, actor, type, payload FROM match_events WHERE match_id = ? AND seq = ?;"),
    STATEMENT(eventInsertStmt,
        "INSERT INTO match_events (match_id, seq, revision, actor, type, payload) VALUES (?, ?, ?, ?, ?, ?);"),
    STATEMENT(eventSelectAllStmt,
        "SELECT seq, revision, actor, type, payload FROM match_events WHERE match_id = ? ORDER BY seq ASC;"),
    STATEMENT(accountUpdateEloStmt, "UPDATE accounts SET elo = ? WHERE id = ?;"),
    STATEMENT(achievementInsertStmt,
        "INSERT OR IGNORE INTO achievements (user_id, badge, unlocked_at, match_id) VALUES (?, ?, ?, ?);"),
    STATEMENT(profileStatsInitStmt,
        "INSERT INTO profile_stats (user_id, peak_elo) VALUES (?, 1500);"),
    STATEMENT(profileStatsSelectStmt,
        "SELECT matches, wins, team_wins, plus4, uno_calls, streak, best_streak, peak_elo "
        "FROM profile_stats WHERE user_id = ?;"),
    STATEMENT(profileStatsUpdateStmt,
        "UPDATE profile_stats "
        "SET matches = matches + ?, "
        "    wins = wins + ?, "
        "    team_wins = team_wins + ?, "
        "    plus4 = plus4 + ?, "
        "    uno_calls = uno_calls + ?, "
        "    streak = ?, "
        "    best_streak = ?, "
        "    peak_elo = ? "
        "WHERE user_id = ?;"),
    STATEMENT(profileStatsInsertFallbackStmt,
        "INSERT INTO profile_stats (user_id, matches, wins, team_wins, plus4, uno_calls, streak, best_streak, peak_elo) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);"),
    STATEMENT(accountInsertStmt,
        "INSERT INTO accounts (email, password_hash, name, avatar, elo, created_at) "
        "VALUES (?, ?, ?, ?, 1500, strftime('%s', 'now'));"),
    STATEMENT(accountSelectByEmailStmt,
        "SELECT id, email, password_hash, name, avatar, elo FROM accounts WHERE email = ?;"),
    STATEMENT(accountSelectByIdStmt,
        "SELECT id, email, password_hash, name, avatar, elo FROM accounts WHERE id = ?;"),
    /* Điều kiện status bảo đảm trận đã FINALIZED không bao giờ bị ghi đè. */
    STATEMENT(matchAbortStmt,
        "UPDATE matches SET status = ?, duration_ms = ? "
        "WHERE match_id = ? AND status = ?;"),
    STATEMENT(leaderboardStmt,
        "SELECT id, name, avatar, elo FROM accounts "
        "ORDER BY elo DESC, id ASC LIMIT ?;"),
    STATEMENT(friendLinkSelectStmt,
        "SELECT status FROM friends "
        "WHERE (user_id = ?1 AND friend_id = ?2) OR (user_id = ?2 AND friend_id = ?1);"),
    STATEMENT(friendInsertStmt,
        "INSERT INTO friends (user_id, friend_id, status, created_at) "
        "VALUES (?, ?, ?, strftime('%s', 'now'));"),
    /* Cả hai câu trả lời lời mời chỉ đụng hàng PENDING theo chiều requester -> me,
     * nên người gửi không thể tự trả lời lời mời của chính mình. */
    STATEMENT(friendAcceptStmt,
        "UPDATE friends SET status = ?1 "
        "WHERE user_id = ?2 AND friend_id = ?3 AND status = ?4;"),
    STATEMENT(friendRejectStmt,
        "DELETE FROM friends WHERE user_id = ?1 AND friend_id = ?2 AND status = ?3;"),
    /* Bạn đã chấp nhận (status 1) đứng trước lời mời chờ (0): một loạt lời mời rác
     * không được đẩy bạn thật ra khỏi LIMIT. */
    STATEMENT(friendListStmt,
        "SELECT a.id, a.name, a.avatar, a.elo, f.user_id, f.status FROM friends f "
        "JOIN accounts a ON a.id = CASE WHEN f.user_id = ?1 THEN f.friend_id ELSE f.user_id END "
        "WHERE f.user_id = ?1 OR f.friend_id = ?1 "
        "ORDER BY f.status DESC, a.id ASC LIMIT ?2;"),
    STATEMENT(profileBadgesStmt, "SELECT badge FROM achievements WHERE user_id = ?;"),
    STATEMENT(profileModesStmt,
        "SELECT m.mode, count(*), sum(" PROFILE_WON_SQL ")"
        PROFILE_MATCHES_SQL " GROUP BY m.mode;"),
    /* rowid phân xử các trận kết thúc trong cùng một giây. */
    STATEMENT(profileHistoryStmt,
        "SELECT m.match_id, m.mode, " PROFILE_WON_SQL ", "
        "mp.elo_after - mp.elo_before, m.ended_at"
        PROFILE_MATCHES_SQL " ORDER BY m.ended_at DESC, m.rowid DESC LIMIT ?5;"),
    STATEMENT(profileUpdateStmt, "UPDATE accounts SET name = ?, avatar = ? WHERE id = ?;"),
};

#define STATEMENT_COUNT (sizeof(STATEMENTS) / sizeof(STATEMENTS[0]))

static sqlite3_stmt **statementSlot(Store *store, const StatementSpec *spec) {
    return (sqlite3_stmt **)((char *)store + spec->offset);
}

/* ------------------------- Tiện ích câu lệnh SQLite ------------------------- */

/* Gắn tham số tuần tự ?1, ?2, ... và nhớ lần bind nào thất bại. */
typedef struct {
    sqlite3_stmt *stmt;
    int nextIndex;
    bool ok;
} Binder;

/**
 * @brief Chuẩn bị dùng lại một câu lệnh đã biên dịch.
 *
 * sqlite3_reset trả lại mã lỗi của lần step trước, lỗi đó đã được xử lý lúc step;
 * sqlite3_clear_bindings luôn trả SQLITE_OK. Vì vậy hai kết quả này không cần xét.
 */
static Binder startBinding(sqlite3_stmt *stmt) {
    (void)sqlite3_reset(stmt);
    (void)sqlite3_clear_bindings(stmt);
    Binder binder = {stmt, 1, true};
    return binder;
}

static void bindInt(Binder *binder, int value) {
    if (binder->ok) {
        binder->ok = sqlite3_bind_int(binder->stmt, binder->nextIndex, value) == SQLITE_OK;
    }
    binder->nextIndex++;
}

static void bindInt64(Binder *binder, int64_t value) {
    if (binder->ok) {
        binder->ok = sqlite3_bind_int64(binder->stmt, binder->nextIndex, (sqlite3_int64)value) ==
                     SQLITE_OK;
    }
    binder->nextIndex++;
}

/* SQLITE_STATIC: chuỗi của caller còn sống tới khi câu lệnh được reset. */
static void bindText(Binder *binder, const char *text) {
    if (binder->ok) {
        binder->ok = sqlite3_bind_text(binder->stmt, binder->nextIndex, text, -1, SQLITE_STATIC) ==
                     SQLITE_OK;
    }
    binder->nextIndex++;
}

static void bindBool(Binder *binder, bool value) {
    bindInt(binder, value ? 1 : 0);
}

/* Chạy một bước nếu mọi tham số đã bind được; bind lỗi được coi như SQLITE_ERROR. */
static int stepBound(const Binder *binder) {
    return binder->ok ? sqlite3_step(binder->stmt) : SQLITE_ERROR;
}

/* Kết thúc lượt dùng câu lệnh; mã trả về của reset đã được xét ở bước step. */
static void endStatement(sqlite3_stmt *stmt) {
    (void)sqlite3_reset(stmt);
}

/* Chạy câu lệnh ghi (INSERT/UPDATE/DELETE) đã bind xong. */
static Status runWrite(const Binder *binder) {
    int resultCode = stepBound(binder);
    endStatement(binder->stmt);
    return resultCode == SQLITE_DONE ? OK : STORAGE_ERROR;
}

/* Đổi kết quả step của câu SELECT một hàng thành Status. */
static Status rowStatus(int resultCode) {
    if (resultCode == SQLITE_ROW) return OK;
    return resultCode == SQLITE_DONE ? NOT_FOUND : STORAGE_ERROR;
}

/* Chép cột chữ vào buffer cố định; cột NULL thành chuỗi rỗng. */
static void copyColumnText(sqlite3_stmt *stmt, int column, char *out, size_t size) {
    const char *text = (const char *)sqlite3_column_text(stmt, column);
    snprintf(out, size, "%s", text ? text : "");
}

/* ------------------------------ Giao dịch ------------------------------ */

static Status beginTransaction(sqlite3 *db) {
    return sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) == SQLITE_OK ? OK : STORAGE_ERROR;
}

/* ROLLBACK chỉ lỗi khi không còn giao dịch nào mở, đúng trạng thái mong muốn;
 * nếu lỗi mà giao dịch vẫn mở thì đó là sự cố thật nên ghi lại. */
static Status rollbackWith(sqlite3 *db, Status status) {
    bool failed = sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL) != SQLITE_OK;
    if (failed && !sqlite3_get_autocommit(db)) {
        fprintf(stderr, "store: ROLLBACK failed: %s\n", sqlite3_errmsg(db));
    }
    return status;
}

static Status commitTransaction(sqlite3 *db) {
    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK) return OK;
    return rollbackWith(db, STORAGE_ERROR);
}

/* Chốt giao dịch: COMMIT khi mọi bước OK, ngược lại ROLLBACK và trả lỗi của bước đó. */
static Status finishTransaction(sqlite3 *db, Status status) {
    return status == OK ? commitTransaction(db) : rollbackWith(db, status);
}

/* ----------------------- Mở, migrate và đóng DB ----------------------- */

static Status prepareStatements(Store *store) {
    for (size_t i = 0; i < STATEMENT_COUNT; i++) {
        sqlite3_stmt **slot = statementSlot(store, &STATEMENTS[i]);
        if (sqlite3_prepare_v2(store->db, STATEMENTS[i].sql, -1, slot, NULL) != SQLITE_OK) {
            fprintf(stderr, "store: prepare failed: %s\n", sqlite3_errmsg(store->db));
            return STORAGE_ERROR;
        }
    }
    return OK;
}

static void finalizeStatements(Store *store) {
    for (size_t i = 0; i < STATEMENT_COUNT; i++) {
        sqlite3_stmt **slot = statementSlot(store, &STATEMENTS[i]);
        /* sqlite3_finalize(NULL) không làm gì, nên Store mới prepare một phần vẫn an toàn;
         * mã trả về chỉ lặp lại lỗi của lần step trước. */
        (void)sqlite3_finalize(*slot);
        *slot = NULL;
    }
}

/* Chạy câu SELECT trả một số nguyên (cột đầu tiên của hàng đầu tiên). */
static Status queryInt(sqlite3 *db, const char *sql, int *out) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return STORAGE_ERROR;
    int resultCode = sqlite3_step(stmt);
    *out = (resultCode == SQLITE_ROW) ? sqlite3_column_int(stmt, COUNT_COLUMN) : 0;
    (void)sqlite3_finalize(stmt);
    return (resultCode == SQLITE_ROW || resultCode == SQLITE_DONE) ? OK : STORAGE_ERROR;
}

static Status execSql(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? OK : STORAGE_ERROR;
}

/* Chỉ thêm, không xóa: file v2 giữ nguyên mọi hàng. CREATE TABLE IF NOT EXISTS đã
 * cho file mới có sẵn cột này, nên chỉ thêm cột khi còn thiếu. */
static Status addMatchEndTime(sqlite3 *db) {
    int hasColumn = 0;
    if (queryInt(db, "SELECT count(*) FROM pragma_table_info('matches') WHERE name = 'ended_at';",
                 &hasColumn) != OK) return STORAGE_ERROR;
    if (!hasColumn &&
        execSql(db, "ALTER TABLE matches ADD COLUMN ended_at INTEGER NOT NULL DEFAULT 0;") != OK) {
        return STORAGE_ERROR;
    }
    /* Trận chốt trước v3 không ghi thời điểm kết thúc; giờ bắt đầu cộng thời lượng
     * là giá trị gần nhất mà hàng cũ cho được. */
    char sql[SQL_TEXT_BUFFER_SIZE];
    snprintf(sql, sizeof(sql),
             "UPDATE matches SET ended_at = created_at + duration_ms / %d "
             "WHERE status = %d AND ended_at = 0;",
             MS_PER_SECOND, (int)MATCH_FINALIZED);
    return execSql(db, sql);
}

/* v1 lưu tiến độ huy hiệu dạng hàng key/value mà mô hình v2 tính lại từ
 * profile_stats, nên các hàng cũ không có gì cần giữ. */
static Status dropLegacyAchievements(sqlite3 *db) {
    int legacy = 0;
    if (queryInt(db, "SELECT count(*) FROM pragma_table_info('achievements') WHERE name = 'key';",
                 &legacy) != OK) return STORAGE_ERROR;
    return legacy > 0 ? execSql(db, "DROP TABLE achievements;") : OK;
}

/* Các bước migrate chạy trong một giao dịch; bước nào lỗi thì cả lượt bị hủy. */
static Status migrateSteps(sqlite3 *db, int version) {
    if (version < STORE_SCHEMA_PROFILE_VERSION && dropLegacyAchievements(db) != OK) {
        return STORAGE_ERROR;
    }
    if (execSql(db, EMBEDDED_SCHEMA_SQL) != OK) return STORAGE_ERROR;
    /* Tài khoản tạo trước v2 chưa có hàng profile_stats. */
    if (version < STORE_SCHEMA_PROFILE_VERSION &&
        execSql(db, "INSERT OR IGNORE INTO profile_stats (user_id, peak_elo) "
                    "SELECT id, MAX(elo, 1500) FROM accounts;") != OK) {
        return STORAGE_ERROR;
    }
    if (version < STORE_SCHEMA_END_TIME_VERSION && addMatchEndTime(db) != OK) return STORAGE_ERROR;
    if (version < STORE_SCHEMA_VERSION) {
        char sql[SQL_TEXT_BUFFER_SIZE];
        snprintf(sql, sizeof(sql), "PRAGMA user_version = %d;", STORE_SCHEMA_VERSION);
        return execSql(db, sql);
    }
    return OK;
}

static Status migrateSchema(Store *store) {
    if (beginTransaction(store->db) != OK) return STORAGE_ERROR;
    int version = 0;
    Status status = queryInt(store->db, "PRAGMA user_version;", &version);
    if (status == OK) status = migrateSteps(store->db, version);
    return finishTransaction(store->db, status);
}

/* Bật khóa ngoại, chế độ WAL và thời gian chờ khóa cho kết nối vừa mở. */
static Status configureConnection(sqlite3 *db) {
    if (sqlite3_busy_timeout(db, BUSY_TIMEOUT_MS) != SQLITE_OK) return STORAGE_ERROR;
    if (execSql(db, "PRAGMA foreign_keys = ON;") != OK) return STORAGE_ERROR;
    /* WAL chỉ áp được cho file; DB ":memory:" báo "memory" nên lỗi ở đây được bỏ qua. */
    if (execSql(db, "PRAGMA journal_mode = WAL;") != OK) {
        fprintf(stderr, "store: WAL unavailable, using default journal\n");
    }
    return execSql(db, "PRAGMA busy_timeout = 100;");
}

/* Khôi phục sau crash: trận còn ACTIVE trong DB không thể chơi tiếp, nên đánh
 * dấu ABORTED. Lỗi ở đây không chặn việc mở DB nhưng được ghi lại. */
static void recoverActiveMatches(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE matches SET status = ? WHERE status = ?;", -1, &stmt,
                           NULL) != SQLITE_OK) {
        fprintf(stderr, "store: crash recovery skipped: %s\n", sqlite3_errmsg(db));
        return;
    }
    Binder binder = startBinding(stmt);
    bindInt(&binder, MATCH_ABORTED);
    bindInt(&binder, MATCH_ACTIVE);
    if (stepBound(&binder) != SQLITE_DONE) {
        fprintf(stderr, "store: crash recovery failed: %s\n", sqlite3_errmsg(db));
    }
    (void)sqlite3_finalize(stmt);
}

Status storeOpen(Store **out, const char *path) {
    if (!out || !path) return BAD_REQUEST;
    *out = NULL;

    Store *store = calloc(1, sizeof(Store));
    if (!store) return STORAGE_ERROR;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path, &store->db, flags, NULL) != SQLITE_OK) {
        /* sqlite3_open_v2 vẫn có thể cấp handle khi lỗi; storeClose đóng nó. */
        storeClose(store);
        return STORAGE_ERROR;
    }
    if (configureConnection(store->db) != OK || migrateSchema(store) != OK) {
        storeClose(store);
        return STORAGE_ERROR;
    }
    recoverActiveMatches(store->db);
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
    if (store->db && sqlite3_close(store->db) != SQLITE_OK) {
        fprintf(stderr, "store: close failed: %s\n", sqlite3_errmsg(store->db));
    }
    store->db = NULL;
    free(store);
}

sqlite3 *storeGetDb(Store *store) {
    return store ? store->db : NULL;
}

/* ----------------------------- Đọc trận đấu ----------------------------- */

/* Một hàng của bảng matches. */
typedef struct {
    Mode mode;
    bool ranked;
    MatchStatus status;
    int winnerSeat;
    int winnerTeam;
    uint64_t durationMs;
    uint64_t createdAt;
} MatchRow;

/* Đọc trận theo mã; trả OK, NOT_FOUND hoặc STORAGE_ERROR. */
static Status readMatchRow(Store *store, const char *matchId, MatchRow *out) {
    Binder binder = startBinding(store->matchSelectStmt);
    bindText(&binder, matchId);
    int resultCode = stepBound(&binder);
    if (resultCode == SQLITE_ROW) {
        sqlite3_stmt *stmt = store->matchSelectStmt;
        out->mode = (Mode)sqlite3_column_int(stmt, MATCH_COLUMN_MODE);
        out->ranked = sqlite3_column_int(stmt, MATCH_COLUMN_RANKED) != 0;
        out->status = (MatchStatus)sqlite3_column_int(stmt, MATCH_COLUMN_STATUS);
        out->winnerSeat = sqlite3_column_int(stmt, MATCH_COLUMN_WINNER_SEAT);
        out->winnerTeam = sqlite3_column_int(stmt, MATCH_COLUMN_WINNER_TEAM);
        out->durationMs = (uint64_t)sqlite3_column_int64(stmt, MATCH_COLUMN_DURATION);
        out->createdAt = (uint64_t)sqlite3_column_int64(stmt, MATCH_COLUMN_CREATED_AT);
    }
    endStatement(store->matchSelectStmt);
    return rowStatus(resultCode);
}

/* Các cột của một ghế dùng để kiểm tra lời gọi lặp lại (idempotency). */
typedef struct {
    UserId userId;
    char name[PLAYER_NAME_SIZE];
    int botLevel;
    bool isBot;
    int points;
    int eloBefore;
    int eloAfter;
} PlayerRow;

static Status readPlayerRow(Store *store, const char *matchId, int seat, PlayerRow *out) {
    sqlite3_stmt *stmt = store->playerSelectBySeatStmt;
    Binder binder = startBinding(stmt);
    bindText(&binder, matchId);
    bindInt(&binder, seat);
    int resultCode = stepBound(&binder);
    if (resultCode == SQLITE_ROW) {
        out->userId = (UserId)sqlite3_column_int64(stmt, PLAYER_COLUMN_USER_ID);
        copyColumnText(stmt, PLAYER_COLUMN_NAME, out->name, sizeof(out->name));
        out->botLevel = sqlite3_column_int(stmt, PLAYER_COLUMN_BOT_LEVEL);
        out->isBot = sqlite3_column_int(stmt, PLAYER_COLUMN_IS_BOT) != 0;
        out->points = sqlite3_column_int(stmt, PLAYER_COLUMN_POINTS);
        out->eloBefore = sqlite3_column_int(stmt, PLAYER_COLUMN_ELO_BEFORE);
        out->eloAfter = sqlite3_column_int(stmt, PLAYER_COLUMN_ELO_AFTER);
    }
    endStatement(stmt);
    return rowStatus(resultCode);
}

/* -------------------------- Bắt đầu một trận -------------------------- */

/* Trận đã có trong DB: chỉ chấp nhận khi chế độ, ranked và cả bốn ghế trùng khớp. */
static Status verifyStartedMatch(Store *store, const char *matchId, const MatchRow *row,
                                 const PlayerSpec players[MATCH_SEAT_COUNT], Mode mode,
                                 bool ranked) {
    if (row->mode != mode || row->ranked != ranked || row->status != MATCH_ACTIVE) {
        return INVALID_ACTION;
    }
    for (int seat = 0; seat < MATCH_SEAT_COUNT; seat++) {
        PlayerRow stored;
        if (readPlayerRow(store, matchId, seat, &stored) != OK) return INVALID_ACTION;
        const PlayerSpec *player = &players[seat];
        if (stored.userId != player->userId || strcmp(stored.name, player->name) != 0 ||
            stored.isBot != player->isBot || stored.botLevel != (int)player->botLevel) {
            return INVALID_ACTION;
        }
    }
    return OK;
}

static Status insertMatchRow(Store *store, const char *matchId, Mode mode, bool ranked) {
    Binder binder = startBinding(store->matchInsertStmt);
    bindText(&binder, matchId);
    bindInt(&binder, (int)mode);
    bindBool(&binder, ranked);
    bindInt(&binder, (int)MATCH_ACTIVE);
    bindInt64(&binder, (int64_t)time(NULL));
    return runWrite(&binder);
}

static Status insertPlayerRows(Store *store, const char *matchId,
                               const PlayerSpec players[MATCH_SEAT_COUNT]) {
    for (int seat = 0; seat < MATCH_SEAT_COUNT; seat++) {
        const PlayerSpec *player = &players[seat];
        Binder binder = startBinding(store->playerInsertStmt);
        bindText(&binder, matchId);
        bindInt(&binder, seat);
        bindInt64(&binder, (int64_t)player->userId);
        bindText(&binder, player->name);
        bindText(&binder, player->avatar);
        bindInt(&binder, (int)player->botLevel);
        bindBool(&binder, player->isBot);
        if (runWrite(&binder) != OK) return STORAGE_ERROR;
    }
    return OK;
}

Status storeMatchStart(Store *store, const MatchId *matchId,
                       const PlayerSpec players[MATCH_SEAT_COUNT], Mode mode, bool ranked) {
    if (!store || !matchId || !players) return BAD_REQUEST;
    const char *matchIdText = (const char *)matchId;
    if (strlen(matchIdText) == 0) return BAD_REQUEST;

    MatchRow existing;
    Status status = readMatchRow(store, matchIdText, &existing);
    if (status == OK) {
        return verifyStartedMatch(store, matchIdText, &existing, players, mode, ranked);
    }
    if (status != NOT_FOUND) return status;

    if (beginTransaction(store->db) != OK) return STORAGE_ERROR;
    status = insertMatchRow(store, matchIdText, mode, ranked);
    if (status == OK) status = insertPlayerRows(store, matchIdText, players);
    return finishTransaction(store->db, status);
}

/* ------------------------------ Sự kiện trận ------------------------------ */

/**
 * @brief So một sự kiện với bản đã lưu cùng seq (nếu có).
 * @param[out] outExists true khi seq đã có trong DB.
 * @return OK khi chưa có hoặc trùng hoàn toàn; INVALID_ACTION khi cùng seq mà
 *         khác nội dung; STORAGE_ERROR khi đọc lỗi.
 */
static Status compareStoredEvent(Store *store, const char *matchId, const MatchEvent *event,
                                 bool *outExists) {
    sqlite3_stmt *stmt = store->eventSelectStmt;
    Binder binder = startBinding(stmt);
    bindText(&binder, matchId);
    bindInt64(&binder, (int64_t)event->seq);
    int resultCode = stepBound(&binder);
    Status status = rowStatus(resultCode);
    *outExists = status == OK;
    if (status == OK) {
        const char *type = (const char *)sqlite3_column_text(stmt, EVENT_COLUMN_TYPE);
        const char *payload = (const char *)sqlite3_column_text(stmt, EVENT_COLUMN_PAYLOAD);
        uint64_t revision = (uint64_t)sqlite3_column_int64(stmt, EVENT_COLUMN_REVISION);
        bool same = revision == event->revision &&
                    sqlite3_column_int(stmt, EVENT_COLUMN_ACTOR) == event->actor &&
                    strcmp(type ? type : "", event->type) == 0 &&
                    strcmp(payload ? payload : "", event->payload) == 0;
        status = same ? OK : INVALID_ACTION;
    } else if (status == NOT_FOUND) {
        status = OK;
    }
    endStatement(stmt);
    return status;
}

static Status insertEvent(Store *store, const char *matchId, const MatchEvent *event) {
    Binder binder = startBinding(store->eventInsertStmt);
    bindText(&binder, matchId);
    bindInt64(&binder, (int64_t)event->seq);
    bindInt64(&binder, (int64_t)event->revision);
    bindInt(&binder, event->actor);
    bindText(&binder, event->type);
    bindText(&binder, event->payload);
    return runWrite(&binder);
}

/* Ghi lần lượt các sự kiện; sự kiện đã có và trùng khớp được bỏ qua để gọi lại an toàn. */
static Status insertEventBatch(Store *store, const char *matchId, const EventBatch *batch) {
    for (size_t i = 0; i < batch->count; i++) {
        bool exists = false;
        Status status = compareStoredEvent(store, matchId, &batch->events[i], &exists);
        if (status != OK) return status;
        if (exists) continue;
        status = insertEvent(store, matchId, &batch->events[i]);
        if (status != OK) return status;
    }
    return OK;
}

Status storeEvents(Store *store, const MatchId *matchId, const EventBatch *batch) {
    if (!store || !matchId || !batch) return BAD_REQUEST;
    const char *matchIdText = (const char *)matchId;

    MatchRow match;
    Status status = readMatchRow(store, matchIdText, &match);
    if (status != OK) return status;
    if (batch->count == 0) return OK;

    if (beginTransaction(store->db) != OK) return STORAGE_ERROR;
    return finishTransaction(store->db, insertEventBatch(store, matchIdText, batch));
}

/* -------------------------- Hồ sơ người chơi -------------------------- */

/* Tám giá trị hồ sơ theo đúng thứ tự cột của cả câu UPDATE lẫn INSERT dự phòng. */
static void bindProfileDeltaValues(Binder *binder, const ProfileDelta *delta) {
    bindInt64(binder, (int64_t)delta->add.matches);
    bindInt64(binder, (int64_t)delta->add.wins);
    bindInt64(binder, (int64_t)delta->add.teamWins);
    bindInt64(binder, (int64_t)delta->add.plus4);
    bindInt64(binder, (int64_t)delta->add.unoCalls);
    bindInt(binder, delta->streak);
    bindInt64(binder, (int64_t)delta->bestStreak);
    bindInt(binder, delta->peakElo);
}

/* Cộng dồn số liệu vào profile_stats; nếu chưa có hàng thì tạo hàng mới. */
static Status writeProfileStats(Store *store, UserId userId, const ProfileDelta *delta) {
    Binder binder = startBinding(store->profileStatsUpdateStmt);
    bindProfileDeltaValues(&binder, delta);
    bindInt64(&binder, (int64_t)userId);
    if (runWrite(&binder) != OK) return STORAGE_ERROR;
    if (sqlite3_changes(store->db) != 0) return OK;

    binder = startBinding(store->profileStatsInsertFallbackStmt);
    bindInt64(&binder, (int64_t)userId);
    bindProfileDeltaValues(&binder, delta);
    return runWrite(&binder);
}

/* Ghi mỗi huy hiệu vừa mở thành một hàng achievements (bỏ qua nếu đã có). */
static Status insertUnlockedBadges(Store *store, UserId userId, const char *matchId,
                                   uint32_t unlocked) {
    for (int bitIndex = 0; bitIndex < BADGE_BIT_COUNT; bitIndex++) {
        uint32_t bit = 1u << bitIndex;
        if (!(unlocked & bit)) continue;
        Binder binder = startBinding(store->achievementInsertStmt);
        bindInt64(&binder, (int64_t)userId);
        bindInt(&binder, (int)bit);
        bindInt64(&binder, (int64_t)time(NULL));
        bindText(&binder, matchId);
        if (runWrite(&binder) != OK) return STORAGE_ERROR;
    }
    return OK;
}

static Status applyProfileDelta(Store *store, UserId userId, const char *matchId,
                                const ProfileDelta *delta) {
    if (!delta) return OK;
    if (writeProfileStats(store, userId, delta) != OK) return STORAGE_ERROR;
    return insertUnlockedBadges(store, userId, matchId, delta->unlocked);
}

/* ------------------------------ Chốt trận ------------------------------ */

/* Trận đã FINALIZED: lời gọi lặp chỉ hợp lệ khi kết quả trùng khớp hoàn toàn. */
static Status verifyFinalizedMatch(Store *store, const char *matchId, const MatchRow *row,
                                   const MatchResult *result) {
    if (row->winnerSeat != result->winnerSeat || row->winnerTeam != result->winnerTeam ||
        row->durationMs != result->durationMs) {
        return INVALID_ACTION;
    }
    for (int seat = 0; seat < MATCH_SEAT_COUNT; seat++) {
        PlayerRow stored;
        if (readPlayerRow(store, matchId, seat, &stored) != OK) return INVALID_ACTION;
        const PlayerResult *player = &result->players[seat];
        if (stored.points != player->points || stored.eloBefore != player->eloBefore ||
            stored.eloAfter != player->eloAfter) {
            return INVALID_ACTION;
        }
    }
    return OK;
}

static Status updateMatchFinal(Store *store, const char *matchId, const MatchResult *result) {
    Binder binder = startBinding(store->matchUpdateStatusStmt);
    bindInt(&binder, (int)MATCH_FINALIZED);
    bindInt(&binder, result->winnerSeat);
    bindInt(&binder, result->winnerTeam);
    bindInt64(&binder, (int64_t)result->durationMs);
    bindInt64(&binder, (int64_t)time(NULL));
    bindText(&binder, matchId);
    return runWrite(&binder);
}

static Status updatePlayerFinal(Store *store, const char *matchId, int seat,
                                const PlayerResult *player) {
    Binder binder = startBinding(store->playerUpdateStmt);
    bindInt(&binder, player->team);
    bindInt(&binder, player->points);
    bindInt(&binder, player->eloBefore);
    bindInt(&binder, player->eloAfter);
    bindBool(&binder, player->left);
    bindInt(&binder, (int)player->stats.played);
    bindInt(&binder, (int)player->stats.attacks);
    bindInt(&binder, (int)player->stats.unoCalls);
    bindInt(&binder, (int)player->stats.penalties);
    bindInt(&binder, (int)player->stats.plus4);
    bindText(&binder, matchId);
    bindInt(&binder, seat);
    return runWrite(&binder);
}

static Status updateAccountElo(Store *store, const PlayerResult *player) {
    Binder binder = startBinding(store->accountUpdateEloStmt);
    bindInt(&binder, player->eloAfter);
    bindInt64(&binder, (int64_t)player->userId);
    return runWrite(&binder);
}

/* Ghi kết quả từng ghế; ELO chỉ đổi ở trận ranked, hồ sơ chỉ ghi cho người thật. */
static Status finalizePlayers(Store *store, const char *matchId, const MatchResult *result,
                              const ProfileDelta deltas[MATCH_SEAT_COUNT]) {
    for (int seat = 0; seat < MATCH_SEAT_COUNT; seat++) {
        const PlayerResult *player = &result->players[seat];
        bool human = player->userId > 0;
        if (updatePlayerFinal(store, matchId, seat, player) != OK) return STORAGE_ERROR;
        if (result->ranked && human && updateAccountElo(store, player) != OK) return STORAGE_ERROR;
        if (deltas && human &&
            applyProfileDelta(store, player->userId, matchId, &deltas[seat]) != OK) {
            return STORAGE_ERROR;
        }
    }
    return OK;
}

/* Phần chạy bên trong giao dịch của storeFinalize. */
static Status finalizeInTransaction(Store *store, const char *matchId, const MatchResult *result,
                                    const ProfileDelta deltas[MATCH_SEAT_COUNT]) {
    MatchRow row;
    Status status = readMatchRow(store, matchId, &row);
    if (status != OK) return status == STORAGE_ERROR ? NOT_FOUND : status;
    /* Gọi lại sau khi đã chốt: không cộng ELO hay số liệu lần hai. */
    if (row.status == MATCH_FINALIZED) return verifyFinalizedMatch(store, matchId, &row, result);
    if (row.status != MATCH_ACTIVE) return INVALID_ACTION;
    status = updateMatchFinal(store, matchId, result);
    if (status == OK) status = finalizePlayers(store, matchId, result, deltas);
    return status;
}

Status storeFinalize(Store *store, const MatchResult *result,
                     const ProfileDelta deltas[MATCH_SEAT_COUNT]) {
    if (!store || !result) return BAD_REQUEST;
    if (beginTransaction(store->db) != OK) return STORAGE_ERROR;
    Status status = finalizeInTransaction(store, (const char *)result->matchId, result, deltas);
    return finishTransaction(store->db, status);
}

/* ------------------------------ Hủy trận ------------------------------ */

static Status markAborted(Store *store, const char *matchId, uint64_t durationMs) {
    Binder binder = startBinding(store->matchAbortStmt);
    bindInt(&binder, MATCH_ABORTED);
    bindInt64(&binder, (int64_t)durationMs);
    bindText(&binder, matchId);
    bindInt(&binder, MATCH_ACTIVE);
    if (runWrite(&binder) != OK) return STORAGE_ERROR;
    return sqlite3_changes(store->db) == 1 ? OK : STORAGE_ERROR;
}

Status storeAbort(Store *store, const MatchId *id, const MatchEvent *event, uint64_t durationMs) {
    if (!store || !id || !event) return BAD_REQUEST;
    const char *matchId = (const char *)id;

    /* Sự kiện và trạng thái phải commit cùng nhau: nếu không, lỗi giữa chừng để lại
     * trận ABORTED thiếu sự kiện hủy, hoặc sự kiện hủy trên trận vẫn ACTIVE. */
    if (beginTransaction(store->db) != OK) return STORAGE_ERROR;
    MatchRow row;
    Status status = readMatchRow(store, matchId, &row);
    if (status != OK) return rollbackWith(store->db, status);
    /* Coordinator gọi lại tới khi OK; lần gọi sau khi đã hủy (hoặc sau khi khôi
     * phục crash đã đánh dấu) không được thêm sự kiện thứ hai. */
    if (row.status == MATCH_ABORTED) return rollbackWith(store->db, OK);
    if (row.status != MATCH_ACTIVE) return rollbackWith(store->db, INVALID_ACTION);

    status = insertEvent(store, matchId, event);
    if (status == OK) status = markAborted(store, matchId, durationMs);
    return finishTransaction(store->db, status);
}

/* ----------------------------- Bảng xếp hạng ----------------------------- */

Status storeLeaderboard(Store *store, LeaderboardEntry out[STORE_LEADERBOARD_MAX], size_t *count) {
    if (!store || !out || !count) return BAD_REQUEST;

    sqlite3_stmt *stmt = store->leaderboardStmt;
    Binder binder = startBinding(stmt);
    bindInt(&binder, STORE_LEADERBOARD_MAX);
    if (!binder.ok) return STORAGE_ERROR;

    size_t rows = 0;
    int resultCode = SQLITE_DONE;
    while (rows < STORE_LEADERBOARD_MAX && (resultCode = sqlite3_step(stmt)) == SQLITE_ROW) {
        LeaderboardEntry *entry = &out[rows++];
        memset(entry, 0, sizeof *entry);
        entry->userId = (UserId)sqlite3_column_int64(stmt, PUBLIC_COLUMN_ID);
        copyColumnText(stmt, PUBLIC_COLUMN_NAME, entry->name, sizeof entry->name);
        copyColumnText(stmt, PUBLIC_COLUMN_AVATAR, entry->avatar, sizeof entry->avatar);
        entry->elo = sqlite3_column_int(stmt, PUBLIC_COLUMN_ELO);
    }
    endStatement(stmt);
    if (resultCode != SQLITE_DONE && resultCode != SQLITE_ROW) return STORAGE_ERROR;
    *count = rows;
    return OK;
}

/* ------------------------------- Bạn bè ------------------------------- */

static Status accountExists(Store *store, UserId id) {
    Account account;
    return storeAccountGetById(store, id, &account);
}

/* Hai người đã có quan hệ (theo bất kỳ chiều nào) hay chưa. */
static Status readFriendLink(Store *store, UserId first, UserId second, bool *linked) {
    Binder binder = startBinding(store->friendLinkSelectStmt);
    bindInt64(&binder, (int64_t)first);
    bindInt64(&binder, (int64_t)second);
    int resultCode = stepBound(&binder);
    endStatement(store->friendLinkSelectStmt);
    if (resultCode != SQLITE_ROW && resultCode != SQLITE_DONE) return STORAGE_ERROR;
    *linked = resultCode == SQLITE_ROW;
    return OK;
}

static Status insertFriendRequest(Store *store, UserId from, UserId to) {
    Binder binder = startBinding(store->friendInsertStmt);
    bindInt64(&binder, (int64_t)from);
    bindInt64(&binder, (int64_t)to);
    bindInt(&binder, FRIEND_STATUS_PENDING);
    return runWrite(&binder);
}

/* Phần chạy trong giao dịch của storeFriendRequest. */
static Status friendRequestInTransaction(Store *store, UserId from, UserId to) {
    Status status = accountExists(store, to);
    if (status != OK) return status;
    bool linked = false;
    status = readFriendLink(store, from, to, &linked);
    if (status != OK) return status;
    if (linked) return INVALID_ACTION;
    return insertFriendRequest(store, from, to);
}

Status storeFriendRequest(Store *store, UserId from, UserId to) {
    if (!store || from == to) return BAD_REQUEST;
    /* Kiểm tra chiều ngược lại và lệnh chèn phải thấy cùng một trạng thái: nếu không,
     * hai lời mời chéo nhau có thể cùng lọt qua và để lại cả A->B lẫn B->A. */
    if (beginTransaction(store->db) != OK) return STORAGE_ERROR;
    return finishTransaction(store->db, friendRequestInTransaction(store, from, to));
}

Status storeFriendReply(Store *store, UserId me, UserId requester, bool accept) {
    if (!store) return BAD_REQUEST;

    /* Mỗi câu trả lời là một câu lệnh có điều kiện: không cần giao dịch, và số hàng
     * đổi bằng 0 nghĩa là không có lời mời chờ nào theo chiều requester -> me. */
    Binder binder = startBinding(accept ? store->friendAcceptStmt : store->friendRejectStmt);
    if (accept) bindInt(&binder, FRIEND_STATUS_ACCEPTED);
    bindInt64(&binder, (int64_t)requester);
    bindInt64(&binder, (int64_t)me);
    bindInt(&binder, FRIEND_STATUS_PENDING);
    if (runWrite(&binder) != OK) return STORAGE_ERROR;
    return sqlite3_changes(store->db) == 1 ? OK : NOT_FOUND;
}

/* Đổi một hàng friendListStmt thành FriendRow theo góc nhìn của @p me. */
static void readFriendRow(sqlite3_stmt *stmt, UserId me, FriendRow *row) {
    memset(row, 0, sizeof *row);
    row->userId = (UserId)sqlite3_column_int64(stmt, PUBLIC_COLUMN_ID);
    copyColumnText(stmt, PUBLIC_COLUMN_NAME, row->name, sizeof row->name);
    copyColumnText(stmt, PUBLIC_COLUMN_AVATAR, row->avatar, sizeof row->avatar);
    row->elo = sqlite3_column_int(stmt, PUBLIC_COLUMN_ELO);
    if (sqlite3_column_int(stmt, FRIEND_COLUMN_STATUS) == FRIEND_STATUS_ACCEPTED) {
        row->link = FRIEND_ACCEPTED;
    } else {
        UserId sender = (UserId)sqlite3_column_int64(stmt, FRIEND_COLUMN_SENDER);
        row->link = sender == me ? FRIEND_PENDING_OUT : FRIEND_PENDING_IN;
    }
}

Status storeFriendList(Store *store, UserId me, FriendRow out[STORE_FRIEND_MAX], size_t *count) {
    if (!store || !out || !count) return BAD_REQUEST;

    sqlite3_stmt *stmt = store->friendListStmt;
    Binder binder = startBinding(stmt);
    bindInt64(&binder, (int64_t)me);
    bindInt(&binder, STORE_FRIEND_MAX);
    if (!binder.ok) return STORAGE_ERROR;

    size_t rows = 0;
    int resultCode = SQLITE_DONE;
    while (rows < STORE_FRIEND_MAX && (resultCode = sqlite3_step(stmt)) == SQLITE_ROW) {
        readFriendRow(stmt, me, &out[rows++]);
    }
    endStatement(stmt);
    if (resultCode != SQLITE_DONE && resultCode != SQLITE_ROW) return STORAGE_ERROR;
    *count = rows;
    return OK;
}

/* -------------------------------- Hồ sơ -------------------------------- */

/* Gắn ?1 user, ?2 MATCH_FINALIZED, ?3/?4 hai chế độ đồng đội cho các đoạn
 * PROFILE_WON_SQL / PROFILE_MATCHES_SQL dùng chung. */
static Binder bindProfileMatches(sqlite3_stmt *stmt, UserId id) {
    Binder binder = startBinding(stmt);
    bindInt64(&binder, (int64_t)id);
    bindInt(&binder, MATCH_FINALIZED);
    bindInt(&binder, TWO_V_TWO);
    bindInt(&binder, SIDE);
    return binder;
}

static Status readProfileBadges(Store *store, UserId id, uint32_t *badges) {
    sqlite3_stmt *stmt = store->profileBadgesStmt;
    Binder binder = startBinding(stmt);
    bindInt64(&binder, (int64_t)id);
    if (!binder.ok) return STORAGE_ERROR;
    int resultCode;
    while ((resultCode = sqlite3_step(stmt)) == SQLITE_ROW) {
        *badges |= (uint32_t)sqlite3_column_int64(stmt, BADGE_COLUMN);
    }
    endStatement(stmt);
    return resultCode == SQLITE_DONE ? OK : STORAGE_ERROR;
}

static Status readProfileModes(Store *store, UserId id, Profile *out) {
    sqlite3_stmt *stmt = store->profileModesStmt;
    Binder binder = bindProfileMatches(stmt, id);
    if (!binder.ok) return STORAGE_ERROR;
    int resultCode;
    while ((resultCode = sqlite3_step(stmt)) == SQLITE_ROW) {
        int mode = sqlite3_column_int(stmt, MODES_COLUMN_MODE);
        /* Hàng của chế độ mà bản build này không biết thì bỏ qua, không tin. */
        if (mode < 0 || mode >= STORE_MODE_COUNT) continue;
        out->modes[mode].matches = (uint32_t)sqlite3_column_int64(stmt, MODES_COLUMN_MATCHES);
        out->modes[mode].wins = (uint32_t)sqlite3_column_int64(stmt, MODES_COLUMN_WINS);
    }
    endStatement(stmt);
    return resultCode == SQLITE_DONE ? OK : STORAGE_ERROR;
}

static void readHistoryRow(sqlite3_stmt *stmt, HistoryRow *row) {
    copyColumnText(stmt, HISTORY_COLUMN_MATCH_ID, row->matchId, sizeof row->matchId);
    row->mode = (Mode)sqlite3_column_int(stmt, HISTORY_COLUMN_MODE);
    row->won = sqlite3_column_int(stmt, HISTORY_COLUMN_WON) != 0;
    row->eloDelta = sqlite3_column_int(stmt, HISTORY_COLUMN_ELO_DELTA);
    row->endedAt = (int64_t)sqlite3_column_int64(stmt, HISTORY_COLUMN_ENDED_AT);
}

static Status readProfileHistory(Store *store, UserId id, Profile *out) {
    sqlite3_stmt *stmt = store->profileHistoryStmt;
    Binder binder = bindProfileMatches(stmt, id);
    bindInt(&binder, STORE_HISTORY_MAX);
    if (!binder.ok) return STORAGE_ERROR;
    int resultCode = SQLITE_DONE;
    while (out->historyCount < STORE_HISTORY_MAX &&
           (resultCode = sqlite3_step(stmt)) == SQLITE_ROW) {
        readHistoryRow(stmt, &out->history[out->historyCount++]);
    }
    endStatement(stmt);
    return resultCode == SQLITE_DONE || resultCode == SQLITE_ROW ? OK : STORAGE_ERROR;
}

Status storeProfileGet(Store *store, UserId id, Profile *out) {
    if (!store || !out) return BAD_REQUEST;

    Account account;
    Status status = storeAccountGetById(store, id, &account);
    if (status != OK) return status;

    /* Chỉ chép các trường công khai: email và hash ở lại trong Account cục bộ. */
    memset(out, 0, sizeof *out);
    out->userId = account.id;
    snprintf(out->name, sizeof out->name, "%s", account.name);
    snprintf(out->avatar, sizeof out->avatar, "%s", account.avatar);
    out->elo = account.elo;

    status = storeProfileStats(store, id, &out->stats);
    /* Mọi tài khoản đều có hàng profile_stats từ lúc tạo hoặc khi migrate v2;
     * thiếu hàng chỉ có nghĩa là chưa ghi nhận gì. */
    if (status == NOT_FOUND) status = OK;
    if (status == OK) status = readProfileBadges(store, id, &out->badges);
    if (status == OK) status = readProfileModes(store, id, out);
    if (status == OK) status = readProfileHistory(store, id, out);
    return status;
}

Status storeProfileUpdate(Store *store, UserId id, const char *name, const char *avatar) {
    if (!store || !name || !avatar) return BAD_REQUEST;

    Binder binder = startBinding(store->profileUpdateStmt);
    bindText(&binder, name);
    bindText(&binder, avatar);
    bindInt64(&binder, (int64_t)id);
    if (runWrite(&binder) != OK) return STORAGE_ERROR;
    return sqlite3_changes(store->db) == 1 ? OK : NOT_FOUND;
}

/* ----------------------------- Xuất trận ra JSON ----------------------------- */

static json_object *matchRowJson(const char *matchId, const MatchRow *row) {
    json_object *root = json_object_new_object();
    json_object_object_add(root, "matchId", json_object_new_string(matchId));
    json_object_object_add(root, "mode", json_object_new_int((int)row->mode));
    json_object_object_add(root, "ranked", json_object_new_boolean(row->ranked));
    json_object_object_add(root, "status", json_object_new_int((int)row->status));
    json_object_object_add(root, "winnerSeat", json_object_new_int(row->winnerSeat));
    json_object_object_add(root, "winnerTeam", json_object_new_int(row->winnerTeam));
    json_object_object_add(root, "durationMs", json_object_new_int64((int64_t)row->durationMs));
    json_object_object_add(root, "createdAt", json_object_new_int64((int64_t)row->createdAt));
    return root;
}

static void addIntColumn(json_object *object, const char *key, sqlite3_stmt *stmt, int column) {
    json_object_object_add(object, key, json_object_new_int(sqlite3_column_int(stmt, column)));
}

static void addInt64Column(json_object *object, const char *key, sqlite3_stmt *stmt, int column) {
    json_object_object_add(object, key, json_object_new_int64(sqlite3_column_int64(stmt, column)));
}

static void addBoolColumn(json_object *object, const char *key, sqlite3_stmt *stmt, int column) {
    bool value = sqlite3_column_int(stmt, column) != 0;
    json_object_object_add(object, key, json_object_new_boolean(value));
}

static void addTextColumn(json_object *object, const char *key, sqlite3_stmt *stmt, int column) {
    const char *text = (const char *)sqlite3_column_text(stmt, column);
    json_object_object_add(object, key, json_object_new_string(text ? text : ""));
}

static json_object *playerStatsJson(sqlite3_stmt *stmt) {
    json_object *stats = json_object_new_object();
    addIntColumn(stats, "played", stmt, PLAYER_COLUMN_PLAYED);
    addIntColumn(stats, "attacks", stmt, PLAYER_COLUMN_ATTACKS);
    addIntColumn(stats, "unoCalls", stmt, PLAYER_COLUMN_UNO_CALLS);
    addIntColumn(stats, "penalties", stmt, PLAYER_COLUMN_PENALTIES);
    addIntColumn(stats, "plus4", stmt, PLAYER_COLUMN_PLUS4);
    return stats;
}

static json_object *playerRowJson(sqlite3_stmt *stmt) {
    json_object *player = json_object_new_object();
    addIntColumn(player, "seat", stmt, PLAYER_COLUMN_SEAT);
    addInt64Column(player, "userId", stmt, PLAYER_COLUMN_USER_ID);
    addTextColumn(player, "name", stmt, PLAYER_COLUMN_NAME);
    addTextColumn(player, "avatar", stmt, PLAYER_COLUMN_AVATAR);
    addIntColumn(player, "botLevel", stmt, PLAYER_COLUMN_BOT_LEVEL);
    addBoolColumn(player, "isBot", stmt, PLAYER_COLUMN_IS_BOT);
    addIntColumn(player, "team", stmt, PLAYER_COLUMN_TEAM);
    addIntColumn(player, "points", stmt, PLAYER_COLUMN_POINTS);
    addIntColumn(player, "eloBefore", stmt, PLAYER_COLUMN_ELO_BEFORE);
    addIntColumn(player, "eloAfter", stmt, PLAYER_COLUMN_ELO_AFTER);
    addBoolColumn(player, "left", stmt, PLAYER_COLUMN_LEFT);
    json_object_object_add(player, "stats", playerStatsJson(stmt));
    return player;
}

/* Payload lưu dạng chuỗi JSON; nếu không parse được thì xuất nguyên chuỗi. */
static json_object *eventPayloadJson(const char *payloadText) {
    if (!payloadText) return json_object_new_object();
    json_object *parsed = json_tokener_parse(payloadText);
    return parsed ? parsed : json_object_new_string(payloadText);
}

static json_object *eventRowJson(sqlite3_stmt *stmt) {
    json_object *event = json_object_new_object();
    addInt64Column(event, "seq", stmt, EVENT_ROW_COLUMN_SEQ);
    addInt64Column(event, "revision", stmt, EVENT_ROW_COLUMN_REVISION);
    addIntColumn(event, "actor", stmt, EVENT_ROW_COLUMN_ACTOR);
    addTextColumn(event, "type", stmt, EVENT_ROW_COLUMN_TYPE);
    const char *payloadText = (const char *)sqlite3_column_text(stmt, EVENT_ROW_COLUMN_PAYLOAD);
    json_object_object_add(event, "payload", eventPayloadJson(payloadText));
    return event;
}

/* Chạy một câu SELECT theo mã trận và gom mọi hàng thành mảng JSON. */
static json_object *collectMatchRows(sqlite3_stmt *stmt, const char *matchId,
                                     json_object *(*rowJson)(sqlite3_stmt *)) {
    json_object *rows = json_object_new_array();
    Binder binder = startBinding(stmt);
    bindText(&binder, matchId);
    while (stepBound(&binder) == SQLITE_ROW) {
        json_object_array_add(rows, rowJson(stmt));
    }
    endStatement(stmt);
    return rows;
}

Status storeExportMatch(Store *store, const MatchId *matchId, const char *path) {
    if (!store || !matchId || !path) return BAD_REQUEST;
    const char *matchIdText = (const char *)matchId;

    MatchRow row;
    if (readMatchRow(store, matchIdText, &row) != OK) return NOT_FOUND;

    json_object *root = matchRowJson(matchIdText, &row);
    json_object *players = collectMatchRows(store->playerSelectAllStmt, matchIdText, playerRowJson);
    json_object *events = collectMatchRows(store->eventSelectAllStmt, matchIdText, eventRowJson);
    json_object_object_add(root, "players", players);
    json_object_object_add(root, "events", events);
    int dumpResult = json_object_to_file_ext(path, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);
    return dumpResult == 0 ? OK : STORAGE_ERROR;
}

/* ------------------------------- Tài khoản ------------------------------- */

/* Chèn hàng accounts; trùng email (vi phạm UNIQUE) trả BAD_REQUEST. */
static Status insertAccountRow(Store *store, const AccountCreate *newAccount) {
    Binder binder = startBinding(store->accountInsertStmt);
    bindText(&binder, newAccount->email);
    bindText(&binder, newAccount->hash);
    bindText(&binder, newAccount->name);
    bindText(&binder, newAccount->avatar);
    int resultCode = stepBound(&binder);
    endStatement(store->accountInsertStmt);
    if (resultCode == SQLITE_CONSTRAINT) return BAD_REQUEST;
    return resultCode == SQLITE_DONE ? OK : STORAGE_ERROR;
}

static Status insertInitialProfileStats(Store *store, UserId userId) {
    Binder binder = startBinding(store->profileStatsInitStmt);
    bindInt64(&binder, (int64_t)userId);
    return runWrite(&binder);
}

Status storeAccountCreate(Store *store, const AccountCreate *newAccount, UserId *outId) {
    if (!store || !newAccount || !outId) return BAD_REQUEST;

    /* Hàng accounts và hàng profile_stats phải tạo cùng nhau: nếu không, lệnh chèn
     * thứ hai lỗi sẽ để lại tài khoản chiếm email trong khi client được báo đăng ký thất bại. */
    if (beginTransaction(store->db) != OK) return STORAGE_ERROR;
    Status status = insertAccountRow(store, newAccount);
    UserId newId = 0;
    if (status == OK) {
        newId = (UserId)sqlite3_last_insert_rowid(store->db);
        status = insertInitialProfileStats(store, newId);
    }
    status = finishTransaction(store->db, status);
    if (status == OK) *outId = newId;
    return status;
}

/* Chạy câu SELECT tài khoản đã bind và chép hàng kết quả vào @p out. */
static Status readAccount(const Binder *binder, Account *out) {
    sqlite3_stmt *stmt = binder->stmt;
    int resultCode = stepBound(binder);
    if (resultCode == SQLITE_ROW) {
        memset(out, 0, sizeof(*out));
        out->id = (UserId)sqlite3_column_int64(stmt, ACCOUNT_COLUMN_ID);
        copyColumnText(stmt, ACCOUNT_COLUMN_EMAIL, out->email, sizeof(out->email));
        copyColumnText(stmt, ACCOUNT_COLUMN_HASH, out->hash, sizeof(out->hash));
        copyColumnText(stmt, ACCOUNT_COLUMN_NAME, out->name, sizeof(out->name));
        copyColumnText(stmt, ACCOUNT_COLUMN_AVATAR, out->avatar, sizeof(out->avatar));
        out->elo = sqlite3_column_int(stmt, ACCOUNT_COLUMN_ELO);
    }
    endStatement(stmt);
    return rowStatus(resultCode);
}

Status storeAccountGet(Store *store, const char *email, Account *out) {
    if (!store || !email || !out) return BAD_REQUEST;
    Binder binder = startBinding(store->accountSelectByEmailStmt);
    bindText(&binder, email);
    return readAccount(&binder, out);
}

Status storeAccountGetById(Store *store, UserId id, Account *out) {
    if (!store || !out) return BAD_REQUEST;
    Binder binder = startBinding(store->accountSelectByIdStmt);
    bindInt64(&binder, (int64_t)id);
    return readAccount(&binder, out);
}

Status storeProfileStats(Store *store, UserId userId, ProfileStats *out) {
    if (!store || !out) return BAD_REQUEST;

    sqlite3_stmt *stmt = store->profileStatsSelectStmt;
    Binder binder = startBinding(stmt);
    bindInt64(&binder, (int64_t)userId);
    int resultCode = stepBound(&binder);
    if (resultCode == SQLITE_ROW) {
        memset(out, 0, sizeof(*out));
        out->matches = (uint32_t)sqlite3_column_int64(stmt, STATS_COLUMN_MATCHES);
        out->wins = (uint32_t)sqlite3_column_int64(stmt, STATS_COLUMN_WINS);
        out->teamWins = (uint32_t)sqlite3_column_int64(stmt, STATS_COLUMN_TEAM_WINS);
        out->plus4 = (uint32_t)sqlite3_column_int64(stmt, STATS_COLUMN_PLUS4);
        out->unoCalls = (uint32_t)sqlite3_column_int64(stmt, STATS_COLUMN_UNO_CALLS);
        out->streak = (int32_t)sqlite3_column_int(stmt, STATS_COLUMN_STREAK);
        out->bestStreak = (uint32_t)sqlite3_column_int64(stmt, STATS_COLUMN_BEST_STREAK);
        out->peakElo = (int32_t)sqlite3_column_int(stmt, STATS_COLUMN_PEAK_ELO);
    }
    endStatement(stmt);
    return rowStatus(resultCode);
}
