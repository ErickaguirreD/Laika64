-- 2026-11-24: the games played online (in rooms), by week, game code and player: "Most played" counts only these (before,
-- the time with any game open while online: it led with games played alone). Apply once:
--   npx wrangler d1 execute laika --remote --file migrate_online_week.sql
CREATE TABLE IF NOT EXISTS online_week (week INTEGER NOT NULL, code TEXT NOT NULL, user INTEGER NOT NULL, game TEXT NOT NULL,
    minutes INTEGER NOT NULL, PRIMARY KEY (week, code, user));
DELETE FROM top_cache;   -- the ranking read again from it at once
