// LAIKA 64 - cartridge ROM image.
//
// Loads a ROM from memory in any of the three common dump orders and keeps it
// in N64 (big-endian, ".z64") order. Parses the header and identifies the CIC
// chip from the CRC-32 of the IPL3 boot code (ROM offsets 0x40-0xFFF).
#ifndef H64_ROM_H
#define H64_ROM_H

#include "../common/h64_types.h"

enum H64Cic
{
    H64_CIC_UNKNOWN = 0,
    H64_CIC_6101,
    H64_CIC_6102,   // also 7101 (PAL)
    H64_CIC_6103,   // also 7103
    H64_CIC_6105,   // also 7105
    H64_CIC_6106,   // also 7106
    H64_CIC_7102,
    H64_CIC_8303    // 64DD
};

enum H64RomOrder
{
    H64_ROM_ORDER_Z64 = 0,   // big-endian, as on the cartridge
    H64_ROM_ORDER_V64 = 1,   // 16-bit byte-swapped
    H64_ROM_ORDER_N64 = 2    // 32-bit little-endian
};

struct H64Rom
{
    u8 *data;          // big-endian, size rounded up to 4 bytes
    u32 size;
    int sourceOrder;   // H64RomOrder of the file
    u32 ipl3Crc;       // CRC-32 of ROM[0x40..0x1000)
    int cic;           // H64Cic
    u32 cicSeed;       // IPL2 seed for this CIC
    u32 entryPoint;    // header 0x08
    u32 crc1, crc2;    // header 0x10/0x14
    char name[21];     // header 0x20, trimmed
    char gameCode[5];  // header 0x3B..0x3E, e.g. "NSME"
    u8 region;         // header 0x3E
    u8 version;        // header 0x3F
};

// Copies `size` bytes into a new image. Returns 0 on success, -1 if the data
// is not an N64 ROM (unknown first word or smaller than 4 KB).
int h64_rom_load(H64Rom *rom, const u8 *file, u32 size);
void h64_rom_free(H64Rom *rom);

const char *h64_cic_name(int cic);

// TV type for the region byte: 0 = PAL, 1 = NTSC, 2 = MPAL.
int h64_rom_tv_type(const H64Rom *rom);

// What a ROM is by mupen64plus's catalogue (h64_romdb.inc, tests/scripts/gen_romdb.py): an original dump, a
// translation, a hack, a bad dump, another modified dump (overdump, fix, trainer, pirate), homebrew; UNKNOWN when
// the catalogue does not know its header CRCs (a newer hack keeps its game's code but not its CRCs). *name: its
// GoodN64 name ("Mario Kart 64 (U) [!]"), or NULL when unknown.
enum { H64_ROMKIND_UNKNOWN = -1, H64_ROMKIND_ORIGINAL = 0, H64_ROMKIND_TRANSLATION, H64_ROMKIND_HACK, H64_ROMKIND_BAD,
       H64_ROMKIND_OTHER, H64_ROMKIND_HOMEBREW };
int h64_rom_catalogue(u32 crc1, u32 crc2, const char **name);

#endif
