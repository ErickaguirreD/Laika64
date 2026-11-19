-- The rooms' ROM (header CRC1 CRC2) in the rooms' list: who has it before joining, what it is (index.js lobby).
ALTER TABLE rooms ADD COLUMN crc TEXT NOT NULL DEFAULT '';
