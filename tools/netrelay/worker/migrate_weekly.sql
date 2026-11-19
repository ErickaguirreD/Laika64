-- 2026-11-18: the weekly ranking (hours played online), filled by the rooms (Room.week).
CREATE TABLE IF NOT EXISTS weekly (week INTEGER NOT NULL, user INTEGER NOT NULL, seconds INTEGER NOT NULL, PRIMARY KEY (week, user));
