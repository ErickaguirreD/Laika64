-- LAIKA 64 online: the players' profiles (Cloudflare D1). Apply once:
--   npx wrangler d1 execute laika --remote --file schema.sql
CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, lower TEXT NOT NULL UNIQUE,
    token TEXT NOT NULL, country TEXT NOT NULL, avatar INTEGER NOT NULL, created INTEGER NOT NULL,
    seen INTEGER NOT NULL, status TEXT NOT NULL DEFAULT '', minutes INTEGER NOT NULL DEFAULT 0,
    ms INTEGER NOT NULL DEFAULT 0, conn INTEGER NOT NULL DEFAULT 0, room INTEGER NOT NULL DEFAULT 0,
    playbank INTEGER NOT NULL DEFAULT 0);   -- the time played the server saw (pings with a game open), not yet claimed
CREATE INDEX IF NOT EXISTS users_minutes ON users (minutes DESC);
-- A friendship, each way, and since when (0: before 2.2.2, not known).
CREATE TABLE IF NOT EXISTS friends (user INTEGER NOT NULL, friend INTEGER NOT NULL, at INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (user, friend));
-- How many games two players started together (a < b, their ids).
CREATE TABLE IF NOT EXISTS together (a INTEGER NOT NULL, b INTEGER NOT NULL, games INTEGER NOT NULL, PRIMARY KEY (a, b));
CREATE TABLE IF NOT EXISTS played (user INTEGER NOT NULL, code TEXT NOT NULL, game TEXT NOT NULL,
    minutes INTEGER NOT NULL, PRIMARY KEY (user, code));
-- A registration's place (a salted hash of the address), kept an hour: how many one place may make.
CREATE TABLE IF NOT EXISTS registered (place TEXT NOT NULL, at INTEGER NOT NULL);
-- The news, the newest first; a language without items of its own gets the English ones.
CREATE TABLE IF NOT EXISTS news (id INTEGER PRIMARY KEY, date TEXT NOT NULL, lang TEXT NOT NULL, title TEXT NOT NULL, text TEXT NOT NULL);
-- The rooms, waiting for players or playing (started: since when; tags: who plays). Each room's Durable
-- Object keeps its own row.
CREATE TABLE IF NOT EXISTS rooms (code TEXT PRIMARY KEY, host TEXT NOT NULL, avatar INTEGER NOT NULL, players INTEGER NOT NULL,
    game TEXT NOT NULL, name TEXT NOT NULL DEFAULT '', locked INTEGER NOT NULL DEFAULT 0, updated INTEGER NOT NULL,
    size INTEGER NOT NULL DEFAULT 4, started INTEGER NOT NULL DEFAULT 0, tags TEXT NOT NULL DEFAULT '', art TEXT NOT NULL DEFAULT '',
    crc TEXT NOT NULL DEFAULT '');
-- The general chat (the last messages), and when each player last wrote in it.
CREATE TABLE IF NOT EXISTS chat (id INTEGER PRIMARY KEY, at INTEGER NOT NULL, tag TEXT NOT NULL, avatar INTEGER NOT NULL, text TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS said (user INTEGER PRIMARY KEY, at INTEGER NOT NULL);
-- Friendships asked and not answered yet (a friendship itself is a row of friends each way).
CREATE TABLE IF NOT EXISTS requests (from_id INTEGER NOT NULL, to_id INTEGER NOT NULL, at INTEGER NOT NULL, PRIMARY KEY (from_id, to_id));
-- Messages between friends.
CREATE TABLE IF NOT EXISTS dm (id INTEGER PRIMARY KEY, from_id INTEGER NOT NULL, to_id INTEGER NOT NULL, at INTEGER NOT NULL, text TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS dm_to ON dm (to_id, id);
CREATE INDEX IF NOT EXISTS dm_from ON dm (from_id, at);
-- Gamertags given up by a rename, kept for their old owner until `until` (index.js NAME_HOLD_S).
CREATE TABLE IF NOT EXISTS held_names (lower TEXT PRIMARY KEY, owner INTEGER NOT NULL, until INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS weekly (week INTEGER NOT NULL, user INTEGER NOT NULL, seconds INTEGER NOT NULL, PRIMARY KEY (week, user));
