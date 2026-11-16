-- The rooms' comings and goings (Room.log): who joined, was refused, or went and why; kept a week (2026-11-15).
CREATE TABLE IF NOT EXISTS room_log (
  at INTEGER NOT NULL,
  code TEXT NOT NULL,
  tag TEXT NOT NULL,
  what TEXT NOT NULL,
  players INTEGER NOT NULL,
  started INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS room_log_at ON room_log (at);
