-- 2026-11-24: since when each player plays what its status says (Play online's players: "25 min"). Apply once:
--   npx wrangler d1 execute laika --remote --file migrate_playing.sql
ALTER TABLE users ADD COLUMN playing INTEGER NOT NULL DEFAULT 0;
