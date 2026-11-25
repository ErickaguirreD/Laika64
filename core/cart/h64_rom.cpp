#include "h64_rom.h"

#include <stdlib.h>
#include <string.h>

#include "../common/h64_crc32.h"
#include "../common/h64_endian.h"
#include "../common/h64_log.h"

// IPL3 CRC-32 -> CIC, and the seed IPL2 passes to IPL3 (n64brew "CIC-NUS").
struct CicInfo { u32 crc; int cic; u32 seed; };
static const CicInfo s_cics[] = {
    { 0x6170A4A1u, H64_CIC_6101, 0x3F },
    { 0x90BB6CB5u, H64_CIC_6102, 0x3F },
    { 0x0B050EE0u, H64_CIC_6103, 0x78 },
    // The 6103 IPL3 with its two checksum branches (0x63C, 0x648) replaced by
    // nops, so a modified ROM boots without a new header checksum (Paper
    // Mario: Master Quest and Master Quest Jr.). As unknown, the HLE boot
    // jumped to the header's entry point instead of entry - 0x100000.
    { 0x9CE7C6E5u, H64_CIC_6103, 0x78 },
    { 0x98BC2C86u, H64_CIC_6105, 0x91 },
    { 0xACC8580Au, H64_CIC_6106, 0x85 },
    { 0x009E9EA3u, H64_CIC_7102, 0x3F },
    { 0x0E018159u, H64_CIC_8303, 0xDD },
};

// ROM words a hack got wrong, put right when the words around them match. Ocarina of Time 4 Player
// Edition (on the debug ROM): an overlay's relocations at 0xC010B0 hold one empty entry (section 0,
// offset 0), and Overlay_Relocate reads address 0 for it - a TLB miss on the console too, black after
// one frame. 0x40000000 (section 1, type 0) is read and writes nothing.
struct RomFix { u32 crc1, crc2, offset, before, bad, after, good; };
static const RomFix s_fixes[] = {
    { 0x917D18F6u, 0x69BC5453u, 0xC24DDCu, 0x4400736Cu, 0x00000000u, 0x440074FCu, 0x40000000u },
};

static void rom_fix(H64Rom *rom)
{
    u32 i;
    for (i = 0; i < H64_ARRAY_COUNT(s_fixes); i++)
    {
        const RomFix *f = &s_fixes[i];
        u8 *p = rom->data + f->offset;
        if (f->crc1 != rom->crc1 || f->crc2 != rom->crc2 || f->offset + 8 > rom->size) continue;
        if (h64_load_be32(p - 4) != f->before || h64_load_be32(p) != f->bad || h64_load_be32(p + 4) != f->after) continue;
        h64_store_be32(p, f->good);
        H64_INFO("[rom] fixed word %08X: %08X -> %08X", f->offset, f->bad, f->good);
    }
}

const char *h64_cic_name(int cic)
{
    switch (cic)
    {
    case H64_CIC_6101: return "6101";
    case H64_CIC_6102: return "6102/7101";
    case H64_CIC_6103: return "6103/7103";
    case H64_CIC_6105: return "6105/7105";
    case H64_CIC_6106: return "6106/7106";
    case H64_CIC_7102: return "7102";
    case H64_CIC_8303: return "8303";
    }
    return "unknown";
}

int h64_rom_tv_type(const H64Rom *rom)
{
    switch (rom->region)
    {
    case 'D': case 'F': case 'I': case 'P': case 'S': case 'U': case 'X': case 'Y': case 'L':
        return 0;   // PAL
    case 'B':
        return 2;   // MPAL (Brazil)
    }
    return 1;       // NTSC
}

static void copy_trimmed(char *dst, const u8 *src, int len)
{
    int i;
    for (i = 0; i < len; i++)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
    dst[len] = 0;
    while (len > 0 && dst[len - 1] == ' ')
        dst[--len] = 0;
}

int h64_rom_load(H64Rom *rom, const u8 *file, u32 size)
{
    u32 first, i, padded;
    int order;

    memset(rom, 0, sizeof(*rom));
    if (size < 0x1000)
        return -1;
    first = h64_ref_load_be32(file);
    if (first == 0x80371240u) order = H64_ROM_ORDER_Z64;
    else if (first == 0x37804012u) order = H64_ROM_ORDER_V64;
    else if (first == 0x40123780u) order = H64_ROM_ORDER_N64;
    else
    {
        // Some homebrew uses another first word; trust the z64 order then.
        H64_WARN("[rom] unknown first word %08X, assuming big-endian", first);
        order = H64_ROM_ORDER_Z64;
    }

    padded = (size + 3) & ~3u;
    rom->data = (u8 *)malloc(padded);
    if (!rom->data)
        return -1;
    memset(rom->data + size, 0, padded - size);
    for (i = 0; i < size; i++)
    {
        u32 src = i;
        if (order == H64_ROM_ORDER_V64) src = i ^ 1;
        else if (order == H64_ROM_ORDER_N64) src = i ^ 3;
        rom->data[i] = src < size ? file[src] : 0;
    }
    rom->size = padded;
    rom->sourceOrder = order;

    rom->entryPoint = h64_load_be32(rom->data + 0x08);
    rom->crc1 = h64_load_be32(rom->data + 0x10);
    rom->crc2 = h64_load_be32(rom->data + 0x14);
    copy_trimmed(rom->name, rom->data + 0x20, 20);
    copy_trimmed(rom->gameCode, rom->data + 0x3B, 4);
    rom->region = rom->data[0x3E];
    rom->version = rom->data[0x3F];

    rom->ipl3Crc = h64_crc32(0, rom->data + 0x40, 0x1000 - 0x40);
    rom->cic = H64_CIC_UNKNOWN;
    rom->cicSeed = 0x3F;
    for (i = 0; i < H64_ARRAY_COUNT(s_cics); i++)
        if (s_cics[i].crc == rom->ipl3Crc)
        {
            rom->cic = s_cics[i].cic;
            rom->cicSeed = s_cics[i].seed;
        }
    H64_INFO("[rom] \"%s\" %s rev %u, %u bytes (%s), entry %08X, IPL3 CRC %08X -> CIC %s", rom->name,
             rom->gameCode, (unsigned)rom->version, (unsigned)size,
             order == H64_ROM_ORDER_Z64 ? "z64" : order == H64_ROM_ORDER_V64 ? "v64" : "n64", rom->entryPoint,
             rom->ipl3Crc, h64_cic_name(rom->cic));
    rom_fix(rom);
    return 0;
}

void h64_rom_free(H64Rom *rom)
{
    free(rom->data);
    rom->data = 0;
    rom->size = 0;
}

struct RomDbEntry { u32 crc1, crc2; int kind; const char *name; };
static const RomDbEntry s_romDb[] = {
#include "h64_romdb.inc"
};

int h64_rom_catalogue(u32 crc1, u32 crc2, const char **name)
{
    int lo = 0, hi = (int)(sizeof(s_romDb) / sizeof(s_romDb[0])) - 1;
    if (name) *name = NULL;
    while (lo <= hi)   // sorted by CRC1, then CRC2
    {
        int mid = (lo + hi) / 2;
        const RomDbEntry *e = &s_romDb[mid];
        if (e->crc1 == crc1 && e->crc2 == crc2)
        {
            if (name) *name = e->name;
            return e->kind;
        }
        if (e->crc1 < crc1 || (e->crc1 == crc1 && e->crc2 < crc2)) lo = mid + 1;
        else hi = mid - 1;
    }
    return H64_ROMKIND_UNKNOWN;
}

int h64_rom_catalogue_crc1(u32 crc1)
{
    int lo = 0, hi = (int)(sizeof(s_romDb) / sizeof(s_romDb[0])) - 1;
    while (lo <= hi)   // sorted by CRC1 first
    {
        int mid = (lo + hi) / 2;
        const RomDbEntry *e = &s_romDb[mid];
        if (e->crc1 == crc1) return e->kind;
        if (e->crc1 < crc1) lo = mid + 1;
        else hi = mid - 1;
    }
    return H64_ROMKIND_UNKNOWN;
}
