-- Gamertags given up by a rename stay their old owner's for a while (index.js NAME_HOLD_S). Apply once:
--   npx wrangler d1 execute laika --remote --file migrate_held_names.sql
CREATE TABLE IF NOT EXISTS held_names (lower TEXT PRIMARY KEY, owner INTEGER NOT NULL, until INTEGER NOT NULL);
