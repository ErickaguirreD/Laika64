-- Once (2026-11-14): each room's box art key (xb_art.cpp's: the game code, or X and the header CRC1), for the
-- cover the Play online screen shows beside it.
ALTER TABLE rooms ADD COLUMN art TEXT NOT NULL DEFAULT '';
