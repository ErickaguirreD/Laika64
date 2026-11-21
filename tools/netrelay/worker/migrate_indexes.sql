-- 2026-11-20: the free plan's 5 million rows read a day ran out (every request then failed: "no internet" in LAIKA).
-- The players online in Play online (/v1/lobby, every 8 s from each console there) read the whole users table:
-- 6.25 M of the day's reads. With these indexes each request reads only what it returns.
CREATE INDEX IF NOT EXISTS users_seen ON users (seen);
CREATE INDEX IF NOT EXISTS requests_to ON requests (to_id, at);
CREATE INDEX IF NOT EXISTS news_lang ON news (lang, date DESC, id DESC);
