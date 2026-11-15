-- UNO multiplayer SQLite Schema Version 1

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS matches (
    match_id TEXT PRIMARY KEY,
    mode INTEGER NOT NULL,
    ranked INTEGER NOT NULL,
    status INTEGER NOT NULL,
    winner_seat INTEGER NOT NULL DEFAULT -1,
    winner_team INTEGER NOT NULL DEFAULT -1,
    duration_ms INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS match_players (
    match_id TEXT NOT NULL REFERENCES matches(match_id) ON DELETE CASCADE,
    seat INTEGER NOT NULL,
    user_id INTEGER NOT NULL DEFAULT 0,
    name TEXT NOT NULL DEFAULT '',
    avatar TEXT NOT NULL DEFAULT '',
    bot_level INTEGER NOT NULL DEFAULT 0,
    is_bot INTEGER NOT NULL DEFAULT 0,
    team INTEGER NOT NULL DEFAULT -1,
    points INTEGER NOT NULL DEFAULT 0,
    elo_before INTEGER NOT NULL DEFAULT 1500,
    elo_after INTEGER NOT NULL DEFAULT 1500,
    player_left INTEGER NOT NULL DEFAULT 0,
    played INTEGER NOT NULL DEFAULT 0,
    attacks INTEGER NOT NULL DEFAULT 0,
    uno_calls INTEGER NOT NULL DEFAULT 0,
    penalties INTEGER NOT NULL DEFAULT 0,
    plus4 INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (match_id, seat)
);

CREATE TABLE IF NOT EXISTS match_events (
    match_id TEXT NOT NULL REFERENCES matches(match_id) ON DELETE CASCADE,
    seq INTEGER NOT NULL,
    revision INTEGER NOT NULL,
    actor INTEGER NOT NULL,
    type TEXT NOT NULL,
    payload TEXT NOT NULL,
    PRIMARY KEY (match_id, seq)
);

CREATE TABLE IF NOT EXISTS accounts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    email TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    name TEXT NOT NULL,
    avatar TEXT NOT NULL,
    elo INTEGER NOT NULL DEFAULT 1500,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS friends (
    user_id INTEGER NOT NULL,
    friend_id INTEGER NOT NULL,
    status INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    PRIMARY KEY (user_id, friend_id)
);

CREATE TABLE IF NOT EXISTS profile_stats (
    user_id INTEGER PRIMARY KEY REFERENCES accounts(id),
    matches INTEGER NOT NULL DEFAULT 0,
    wins INTEGER NOT NULL DEFAULT 0,
    team_wins INTEGER NOT NULL DEFAULT 0,
    plus4 INTEGER NOT NULL DEFAULT 0,
    uno_calls INTEGER NOT NULL DEFAULT 0,
    streak INTEGER NOT NULL DEFAULT 0,
    best_streak INTEGER NOT NULL DEFAULT 0,
    peak_elo INTEGER NOT NULL DEFAULT 1500
);

CREATE TABLE IF NOT EXISTS achievements (
    user_id INTEGER NOT NULL,
    badge INTEGER NOT NULL,
    unlocked_at INTEGER NOT NULL,
    match_id TEXT NOT NULL,
    PRIMARY KEY (user_id, badge)
);
