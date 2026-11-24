-- 2026-11-23: the most played games online (/v1/top): the minutes of each game by week (week, game code, player),
-- filled by /v1/played, and each period's ranking kept for an hour (one read for everybody meanwhile).
CREATE TABLE IF NOT EXISTS game_week (week INTEGER NOT NULL, code TEXT NOT NULL, user INTEGER NOT NULL, game TEXT NOT NULL,
    minutes INTEGER NOT NULL, PRIMARY KEY (week, code, user));
CREATE TABLE IF NOT EXISTS top_cache (period TEXT PRIMARY KEY, at INTEGER NOT NULL, body TEXT NOT NULL);
