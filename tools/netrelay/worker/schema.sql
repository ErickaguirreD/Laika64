-- LAIKA 64 online: the players' profiles (Cloudflare D1). Apply once:
--   npx wrangler d1 execute laika --remote --file schema.sql
CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, lower TEXT NOT NULL UNIQUE,
    token TEXT NOT NULL, country TEXT NOT NULL, avatar INTEGER NOT NULL, created INTEGER NOT NULL,
    seen INTEGER NOT NULL, status TEXT NOT NULL DEFAULT '', minutes INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS users_minutes ON users (minutes DESC);
CREATE TABLE IF NOT EXISTS friends (user INTEGER NOT NULL, friend INTEGER NOT NULL, PRIMARY KEY (user, friend));
CREATE TABLE IF NOT EXISTS played (user INTEGER NOT NULL, code TEXT NOT NULL, game TEXT NOT NULL,
    minutes INTEGER NOT NULL, PRIMARY KEY (user, code));
-- A registration's place (a salted hash of the address), kept an hour: how many one place may make.
CREATE TABLE IF NOT EXISTS registered (place TEXT NOT NULL, at INTEGER NOT NULL);
-- The news, the newest first; a language without items of its own gets the English ones.
CREATE TABLE IF NOT EXISTS news (id INTEGER PRIMARY KEY, date TEXT NOT NULL, lang TEXT NOT NULL, title TEXT NOT NULL, text TEXT NOT NULL);
-- Public rooms waiting for players (each room's Durable Object keeps its own row).
CREATE TABLE IF NOT EXISTS rooms (code TEXT PRIMARY KEY, host TEXT NOT NULL, avatar INTEGER NOT NULL, players INTEGER NOT NULL,
    game TEXT NOT NULL, name TEXT NOT NULL DEFAULT '', locked INTEGER NOT NULL DEFAULT 0, updated INTEGER NOT NULL);
-- The general chat (the last messages), and when each player last wrote in it.
CREATE TABLE IF NOT EXISTS chat (id INTEGER PRIMARY KEY, at INTEGER NOT NULL, tag TEXT NOT NULL, avatar INTEGER NOT NULL, text TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS said (user INTEGER PRIMARY KEY, at INTEGER NOT NULL);
-- Friendships asked and not answered yet (a friendship itself is a row of friends each way).
CREATE TABLE IF NOT EXISTS requests (from_id INTEGER NOT NULL, to_id INTEGER NOT NULL, at INTEGER NOT NULL, PRIMARY KEY (from_id, to_id));
-- Messages between friends.
CREATE TABLE IF NOT EXISTS dm (id INTEGER PRIMARY KEY, from_id INTEGER NOT NULL, to_id INTEGER NOT NULL, at INTEGER NOT NULL, text TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS dm_to ON dm (to_id, id);
CREATE INDEX IF NOT EXISTS dm_from ON dm (from_id, at);
