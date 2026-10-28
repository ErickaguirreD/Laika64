// Harissa64 V2 - hi-res texture packs in the GlideHQ/GlideN64 .hts format.
//
// A pack maps a 64-bit "Rice" checksum of an N64 texture (taken over its texels
// in RDRAM, and for colour-indexed textures over the palette entries they use)
// to a replacement image. The renderer asks h64_hts_find() on each texture
// cache miss; the answer is cached by the renderer like any decoded texture.
//
// The file layout is read by h64_hts_open() in one place so it can be adjusted
// against a real pack (tools/hts_dump.py prints what a file holds).
#ifndef H64_HTS_H
#define H64_HTS_H

#include "../../core/common/h64_types.h"

struct H64Hts;

// Opens a pack and reads its index. NULL when the file is missing or not a pack.
H64Hts *h64_hts_open(const char *path);
void h64_hts_close(H64Hts *h);
u32 h64_hts_count(const H64Hts *h);

// Rice checksum of a texture (width x height texels of N64 size `size`, 0 4-bit
// .. 3 32-bit) at `src` in RDRAM (N64 byte order), `rowBytes` between rows.
// For colour-indexed textures pass the palette entries (`pal`, 16 or 256
// logical values) and `ci` = 4 or 8: the palette checksum goes in the upper 32 bits.
// Largest colour index used by a CI4 (ci = 4) or CI8 (ci = 8) texture, for diagnostics.
u32 h64_hts_cimax(const u8 *src, u32 width, u32 height, u32 size, u32 rowBytes, int ci);

u64 h64_hts_checksum(const u8 *src, u32 width, u32 height, u32 size, u32 rowBytes, const u16 *pal, int ci);

// Finds and decodes the replacement for `checksum` into A8R8G8B8 words.
// Returns 0 when the pack has none. `*out` points at memory owned by the
// pack object, valid until the next call.
// True when the pack has a texture for `checksum` (index lookup only, no disk access).
int h64_hts_has(const H64Hts *h, u64 checksum);

// The key of the one colour-indexed texture whose texels have this checksum (the
// lower 32 bits of a key), whatever its palette; 0 when none or several palettes.
u64 h64_hts_by_texture(const H64Hts *h, u32 texelChecksum);

// Background loading. h64_hts_request queues `checksum` for the worker threads to
// read and unpack (nothing if it is queued, loaded or failed already).
// h64_hts_ready: 2 loaded and taken before, 1 loaded, -1 the record cannot be read,
// 0 not yet. h64_hts_take: the pixels of a loaded texture (`*out` valid until the
// next take; one thread only), returning 2 if it was taken before, 1 for its first
// time - only when firstOk, else 0 as if not loaded -, -1 failed, 0 not loaded.
void h64_hts_request(H64Hts *h, u64 checksum);
int h64_hts_ready(H64Hts *h, u64 checksum);
int h64_hts_take(H64Hts *h, u64 checksum, int firstOk, const u32 **out, u32 *w, u32 *hgt);

// The pixels last taken for the same texels under another palette (not `exclude`), if
// still loaded: a stand-in while that palette's own texture loads. 1 found, 0 none.
int h64_hts_take_alike(H64Hts *h, u32 texelChecksum, u64 exclude, const u32 **out, u32 *w, u32 *hgt);

int h64_hts_find(H64Hts *h, u64 checksum, const u32 **out, u32 *w, u32 *hgt);

// A checksum from the pack's index, for a one-off preview image (packs carry
// no preview of their own) - the middle entry by key order. 0 when empty.
u64 h64_hts_sample_checksum(const H64Hts *h);

#endif
