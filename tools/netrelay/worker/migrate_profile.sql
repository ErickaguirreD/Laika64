-- LAIKA 64 2.2.2: the profile's "friends since" and "played together". Apply once, before deploying the worker:
--   npx wrangler d1 execute laika --remote --file migrate_profile.sql
ALTER TABLE friends ADD COLUMN at INTEGER NOT NULL DEFAULT 0;
CREATE TABLE IF NOT EXISTS together (a INTEGER NOT NULL, b INTEGER NOT NULL, games INTEGER NOT NULL, PRIMARY KEY (a, b));
