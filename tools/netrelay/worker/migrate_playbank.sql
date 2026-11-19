-- The time played as the server sees it (the pings with a game open), not yet claimed by /v1/played.
ALTER TABLE users ADD COLUMN playbank INTEGER NOT NULL DEFAULT 0;
