-- Once, for LAIKA 64 2.1's Play online screen (2026-11-13): each player's time to the server, its
-- connection (1 cable, 2 Wi-Fi) and whether it is in a room's lobby; each room's size, and its game while
-- it is played (since when, who plays it).
ALTER TABLE users ADD COLUMN ms INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN conn INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN room INTEGER NOT NULL DEFAULT 0;
ALTER TABLE rooms ADD COLUMN size INTEGER NOT NULL DEFAULT 4;
ALTER TABLE rooms ADD COLUMN started INTEGER NOT NULL DEFAULT 0;
ALTER TABLE rooms ADD COLUMN tags TEXT NOT NULL DEFAULT '';
