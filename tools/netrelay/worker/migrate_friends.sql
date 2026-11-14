-- Once, when friendships became two-way (2026-11-13): an old one-way friend made a friend both ways.
INSERT OR IGNORE INTO friends (user, friend) SELECT friend, user FROM friends;
