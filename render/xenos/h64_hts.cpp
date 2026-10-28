// Harissa64 V2 - hi-res texture packs (.hts). See h64_hts.h.
#include "h64_hts.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <iterator>
#include <list>
#include <map>
#include <vector>

#include "../../core/common/h64_endian.h"
#include "../../core/common/h64_log.h"
#include "../../third_party/zlib/zlib.h"

// GL enums stored in the pack (GlideHQ keeps OpenGL format codes).
enum
{
    GLF_GZ = 0x80000000u,      // data is zlib-compressed (flag in `format`)
    GLF_RGBA = 0x1908, GLF_BGRA = 0x80E1,
    GLF_RGBA8 = 0x8058, GLF_RGB5_A1 = 0x8057, GLF_RGBA4 = 0x8056,
    GLF_DXT1 = 0x83F1, GLF_DXT3 = 0x83F2, GLF_DXT5 = 0x83F3,
    GLT_UBYTE = 0x1401, GLT_4444 = 0x8033, GLT_5551 = 0x8034
};

struct HtsEntry { u64 offset; };   // file offset of the texture record

// Recently read records are kept in memory (compressed, as stored), so a texture the
// renderer asks for again after its own cache dropped it costs an inflate, not a disk read.
#ifndef HTS_CACHE_BYTES
#define HTS_CACHE_BYTES (32u * 1024 * 1024)
#endif
struct HtsCached { std::vector<u8> rec; std::list<u64>::iterator pos; };

// Loading in the background (h64_hts_request / h64_hts_take): two worker threads read
// and unpack what the renderer asks for (one reads the USB drive while the other
// unpacks), so a scene change no longer stops the game while hundreds of textures
// come off the drive (Smash Remix's stage select: frames of 250-500 ms). Unpacked
// results stay here, HTS_READY_BYTES of them, least recently taken dropped first:
// a texture asked for again (its palette animated, the renderer's cache dropped it)
// is there at once - taking each only once made those flicker. Only the renderer's
// thread drops them (h64_hts_take), so what it took stays valid until its next take.
#ifndef HTS_READY_BYTES
#define HTS_READY_BYTES (48u * 1024 * 1024)
#endif
#define HTS_WORKERS 2
struct HtsReady { std::vector<u32> px; u32 w, h; int used; std::list<u64>::iterator pos; };

struct H64Hts
{
    FILE *f;
    u64 fileSize;
    std::map<u64, HtsEntry> index;
    std::map<u32, u64> byTexture;  // a colour-indexed texture's texel checksum -> its key (0: several palettes)
    std::map<u64, HtsCached> cache;
    std::list<u64> lru;             // least recently used first
    u64 cacheBytes;
    std::vector<u32> pixels;       // result of the last find
    std::vector<u8> raw, tmp;
    CRITICAL_SECTION findLock;     // h64_hts_find (file, record cache, buffers): the worker and callers
    CRITICAL_SECTION qLock;        // queue, ready, failed
    HANDLE thread[HTS_WORKERS], wake;
    volatile LONG quit;
    std::deque<u64> queue;
    std::map<u64, int> queued;     // in the queue or being loaded
    std::map<u64, HtsReady *> ready;
    std::list<u64> readyOrder;     // least recently taken first
    u64 readyBytes;
    std::map<u64, int> failed;     // records that cannot be read: not asked for again
    std::map<u32, u64> lastByTexture;   // a colour-indexed texture's texel checksum -> the key last taken for it
};

static voidpf hts_zalloc(voidpf, uInt items, uInt size) { return malloc((size_t)items * size); }
static void hts_zfree(voidpf, voidpf p) { free(p); }

static u32 rd32le(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64le(const u8 *p) { return (u64)rd32le(p) | (u64)rd32le(p + 4) << 32; }

static int read_at(FILE *f, u64 off, void *dst, u32 n)
{
    if (fseek(f, (long)off, SEEK_SET) != 0) return 0;
    return fread(dst, 1, n, f) == n;
}

// ---- Opening: header, then the index ----------------------------------------
//
// Layout assumed (GlideN64 TxCache, "storage" files):
//   u32 config; u32 unknown; s64 indexPos;          (little-endian; verified on a real
//                                                    pack, SMASH_REMIX_HIRESTEXTURES.hts)
//   ... texture records ...
//   at indexPos: [u32 count]  then count x { u64 checksum; u64 offset }
//     offset: low 48 bits = file position of the record, high 16 = N64 format/size
//   record: u32 width, u32 height, u32 format, u16 textureFormat, u16 pixelType,
//           u8 isHires, u16 formatSize (N64 fmt << 8 | size), u32 dataSize, data[dataSize]
// The count field is detected from the index size, so both variants load.
H64Hts *h64_hts_open(const char *path)
{
    H64Hts *h = new H64Hts;
    h->cacheBytes = 0;
    InitializeCriticalSection(&h->findLock);
    InitializeCriticalSection(&h->qLock);
    h->thread[0] = h->thread[1] = h->wake = NULL;
    h->quit = 0;
    h->readyBytes = 0;
    u8 hdr[16], *buf;
    u64 indexPos, bytes, n, i;
    u32 count;
    int withCount;
    h->f = fopen(path, "rb");
    if (!h->f) { delete h; return NULL; }
    fseek(h->f, 0, SEEK_END);
    h->fileSize = (u64)ftell(h->f);
    if (h->fileSize < 32 || !read_at(h->f, 0, hdr, 16)) { fclose(h->f); delete h; return NULL; }
    indexPos = rd64le(hdr + 8);
    if (indexPos < 16 || indexPos >= h->fileSize)
    {
        H64_INFO("[hts] %s: index position %llu outside the file (%llu bytes): not the layout this reader knows", path,
                 (unsigned long long)indexPos, (unsigned long long)h->fileSize);
        fclose(h->f); delete h; return NULL;
    }
    bytes = h->fileSize - indexPos;
    buf = (u8 *)malloc((size_t)bytes);
    if (!buf || !read_at(h->f, indexPos, buf, (u32)bytes)) { free(buf); fclose(h->f); delete h; return NULL; }
    count = rd32le(buf);
    withCount = bytes >= 4 && (bytes - 4) % 16 == 0 && (u64)count * 16 == bytes - 4;
    if (!withCount && bytes % 16 != 0)
    {
        H64_INFO("[hts] %s: index of %llu bytes does not fit 16-byte entries", path, (unsigned long long)bytes);
        free(buf); fclose(h->f); delete h; return NULL;
    }
    n = withCount ? count : bytes / 16;
    for (i = 0; i < n; i++)
    {
        const u8 *e = buf + (withCount ? 4 : 0) + i * 16;
        HtsEntry en;
        en.offset = rd64le(e + 8) & 0x0000FFFFFFFFFFFFull;
        if (en.offset < 16 || en.offset >= indexPos) continue;
        h->index[rd64le(e)] = en;
    }
    free(buf);
    {
        std::map<u64, HtsEntry>::const_iterator it;
        for (it = h->index.begin(); it != h->index.end(); ++it)
        {
            if (!(it->first >> 32)) continue;   // no palette checksum
            std::map<u32, u64>::iterator b = h->byTexture.find((u32)it->first);
            if (b == h->byTexture.end()) h->byTexture[(u32)it->first] = it->first;
            else b->second = 0;
        }
    }
    H64_INFO("[hts] %s: %u textures (%s index)", path, (u32)h->index.size(), withCount ? "counted" : "uncounted");
    return h->index.empty() ? (h64_hts_close(h), (H64Hts *)NULL) : h;
}

void h64_hts_close(H64Hts *h)
{
    std::map<u64, HtsReady *>::iterator it;
    if (!h) return;
    if (h->wake)
    {
        int k;
        InterlockedExchange(&h->quit, 1);
        ReleaseSemaphore(h->wake, HTS_WORKERS, NULL);
        for (k = 0; k < HTS_WORKERS; k++)
            if (h->thread[k]) { WaitForSingleObject(h->thread[k], INFINITE); CloseHandle(h->thread[k]); }
    }
    if (h->wake) CloseHandle(h->wake);
    for (it = h->ready.begin(); it != h->ready.end(); ++it) delete it->second;
    DeleteCriticalSection(&h->findLock);
    DeleteCriticalSection(&h->qLock);
    if (h->f) fclose(h->f);
    delete h;
}

u32 h64_hts_count(const H64Hts *h) { return h ? (u32)h->index.size() : 0; }

u64 h64_hts_sample_checksum(const H64Hts *h)
{
    std::map<u64, HtsEntry>::const_iterator it;
    if (!h || h->index.empty()) return 0;
    it = h->index.begin();
    std::advance(it, h->index.size() / 2);
    return it->first;
}

// ---- Rice checksum ------------------------------------------------------------
// The PC reference reads RDRAM as native 32-bit words of its word-swapped
// image, i.e. the N64's big-endian words: h64_load_be32 on our RDRAM bytes.
// At an address that is not a multiple of 4 its read straddles two swapped
// words, so it takes other bytes than a big-endian read would: N64 byte a ^ 3
// for each of a .. a + 3. OoT's sky panels start at odd addresses (32x32 parts
// of a 128-wide image, 31 texels apart) and none of them matched before.
static u32 rd32_ref(const u8 *p)
{
    size_t a = (size_t)p;
    if (!(a & 3)) return h64_load_be32(p);
    return (u32)*(const u8 *)(a ^ 3) | (u32)*(const u8 *)((a + 1) ^ 3) << 8 |
           (u32)*(const u8 *)((a + 2) ^ 3) << 16 | (u32)*(const u8 *)((a + 3) ^ 3) << 24;
}

static u32 rice_crc32(const u8 *src, u32 width, u32 height, u32 size, u32 rowBytes, u32 *ciMax, int ci)
{
    u32 bytesPerLine = ((width << size) + 1) >> 1, crc = 0, mx = 0;
    s32 y = (s32)height - 1;
    while (y >= 0)
    {
        u32 esi = 0;
        s32 x = (s32)bytesPerLine - 4;
        while (x >= 0)
        {
            esi = rd32_ref(src + x);
            if (ci == 8) { u32 k; for (k = 0; k < 4; k++) { u32 v = (esi >> (k * 8)) & 0xFF; if (v > mx) mx = v; } }
            else if (ci == 4) { u32 k; for (k = 0; k < 8; k++) { u32 v = (esi >> (k * 4)) & 0xF; if (v > mx) mx = v; } }
            esi ^= (u32)x;
            crc = (crc << 4) + ((crc >> 28) & 15);
            crc += esi;
            x -= 4;
        }
        esi ^= (u32)y;
        crc += esi;
        src += rowBytes;
        y--;
    }
    if (ciMax) *ciMax = mx;
    return crc;
}

// Palette checksum over the first n entries. Worked out against a real pack
// (SMASH_REMIX_HIRESTEXTURES.hts): the reference keeps the palette as an array of
// 16-bit values with each pair of entries swapped (entry i at index i ^ 1), reads
// it as little-endian 32-bit words from the end, byte offsets 2n-4 down to 0
// (unaligned for an odd n: the word then holds entry n too), and as RiceCRC32 does
// adds the last word read once more at the end. `pal` holds `limit` entries.
static u32 palette_crc(const u16 *pal, u32 n, u32 limit)
{
    u16 h[258];
    u8 img[520];
    u32 i, count = n + 1, crc = 0, esi = 0;
    s32 x;
    if (count > limit) count = limit;
    for (i = 0; i < 258; i++) h[i] = 0;
    for (i = 0; i < count; i++) h[i ^ 1] = pal[i];
    for (i = 0; i < count + 1 && i < 258; i++) { img[i * 2] = (u8)h[i]; img[i * 2 + 1] = (u8)(h[i] >> 8); }
    for (x = (s32)(n * 2) - 4; x >= 0; x -= 4)
    {
        esi = rd32le(img + x) ^ (u32)x;
        crc = (crc << 4) + ((crc >> 28) & 15);
        crc += esi;
    }
    return crc + esi;
}

u32 h64_hts_cimax(const u8 *src, u32 width, u32 height, u32 size, u32 rowBytes, int ci)
{
    u32 mx = 0;
    if (src && width && height) rice_crc32(src, width, height, size, rowBytes, &mx, ci);
    return mx;
}

u64 h64_hts_checksum(const u8 *src, u32 width, u32 height, u32 size, u32 rowBytes, const u16 *pal, int ci)
{
    u32 mx = 0, crc;
    if (!src || !width || !height) return 0;
    crc = rice_crc32(src, width, height, size, rowBytes, &mx, pal ? ci : 0);
    if (pal && ci)
    {
        u32 n = mx + 1, limit = ci == 4 ? 16 : 256;
        if (n > limit) n = limit;
        return (u64)palette_crc(pal, n, limit) << 32 | crc;
    }
    return crc;
}

// ---- Decoding ---------------------------------------------------------------
static u32 argb(u32 r, u32 g, u32 b, u32 a) { return a << 24 | r << 16 | g << 8 | b; }
static u32 e5(u32 v) { return (v << 3) | (v >> 2); }
static u32 e4(u32 v) { return v * 17; }

static u32 rgb565(u32 c, u32 *r, u32 *g, u32 *b)
{
    *r = e5(c >> 11 & 31); *g = (c >> 5 & 63) << 2 | (c >> 9 & 3); *b = e5(c & 31);
    return 0;
}

static void dxt_color_block(const u8 *p, u32 *px, int transparent3)
{
    u32 c0 = p[0] | p[1] << 8, c1 = p[2] | p[3] << 8, r[4], g[4], b[4], a[4] = { 255, 255, 255, 255 }, i;
    u32 bits = rd32le(p + 4);
    rgb565(c0, &r[0], &g[0], &b[0]);
    rgb565(c1, &r[1], &g[1], &b[1]);
    if (c0 > c1 || !transparent3)
    {
        for (i = 0; i < 3; i++) { u32 *v[3] = { r, g, b }; v[i][2] = (2 * v[i][0] + v[i][1]) / 3; v[i][3] = (v[i][0] + 2 * v[i][1]) / 3; }
    }
    else
    {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
        r[3] = g[3] = b[3] = 0; a[3] = 0;
    }
    for (i = 0; i < 16; i++) { u32 k = bits >> (i * 2) & 3; px[i] = argb(r[k], g[k], b[k], a[k]); }
}

static void dxt_decode(const u8 *src, u32 w, u32 hgt, int kind, u32 *dst)
{
    u32 bx, by, blockBytes = kind == 1 ? 8 : 16;
    for (by = 0; by < (hgt + 3) / 4; by++)
        for (bx = 0; bx < (w + 3) / 4; bx++)
        {
            const u8 *p = src + (by * ((w + 3) / 4) + bx) * blockBytes;
            u32 px[16], i;
            dxt_color_block(kind == 1 ? p : p + 8, px, kind == 1);
            if (kind == 3)
                for (i = 0; i < 16; i++) px[i] = (px[i] & 0x00FFFFFFu) | (u32)e4(p[i / 2] >> ((i & 1) * 4) & 15) << 24;
            else if (kind == 5)
            {
                u32 a0 = p[0], a1 = p[1], al[8], i2;
                u64 bits = 0;
                for (i2 = 0; i2 < 6; i2++) bits |= (u64)p[2 + i2] << (8 * i2);
                al[0] = a0; al[1] = a1;
                if (a0 > a1) for (i2 = 1; i2 < 7; i2++) al[i2 + 1] = ((7 - i2) * a0 + i2 * a1) / 7;
                else { for (i2 = 1; i2 < 5; i2++) al[i2 + 1] = ((5 - i2) * a0 + i2 * a1) / 5; al[6] = 0; al[7] = 255; }
                for (i = 0; i < 16; i++) px[i] = (px[i] & 0x00FFFFFFu) | al[(bits >> (3 * i)) & 7] << 24;
            }
            for (i = 0; i < 16; i++)
            {
                u32 x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < hgt) dst[y * w + x] = px[i];
            }
        }
}

int h64_hts_has(const H64Hts *h, u64 checksum) { return h && h->index.find(checksum) != h->index.end(); }

u64 h64_hts_by_texture(const H64Hts *h, u32 texelChecksum)
{
    std::map<u32, u64>::const_iterator it;
    if (!h) return 0;
    it = h->byTexture.find(texelChecksum);
    return it == h->byTexture.end() ? 0 : it->second;
}


static u32 s_whyLogged;
#define HTS_FAIL(...) do { if (s_whyLogged < 20) { s_whyLogged++; H64_INFO(__VA_ARGS__); } return 0; } while (0)

// A record as stored (header and data) into `out`, from the record cache or the
// file. Under findLock.
static int hts_read_record(H64Hts *h, u64 checksum, std::vector<u8> &out)
{
    std::map<u64, HtsEntry>::iterator it;
    std::map<u64, HtsCached>::iterator ch;
    u8 hd[23];
    u32 dataSize;
    if (!h) return 0;
    it = h->index.find(checksum);
    if (it == h->index.end()) return 0;
    ch = h->cache.find(checksum);
    if (ch != h->cache.end())
    {
        h->lru.splice(h->lru.end(), h->lru, ch->second.pos);   // now the most recently used
        out = ch->second.rec;
    }
    else
    {
        if (!read_at(h->f, it->second.offset, hd, 23)) HTS_FAIL("[hts] %016llX: cannot read the record header at %llu", (unsigned long long)checksum, (unsigned long long)it->second.offset);
        dataSize = rd32le(hd + 19);
        if (!dataSize || dataSize > 64u * 1024 * 1024) HTS_FAIL("[hts] %016llX: bad record of %u bytes", (unsigned long long)checksum, dataSize);
        out.resize(23 + (size_t)dataSize);
        memcpy(&out[0], hd, 23);
        if (!read_at(h->f, it->second.offset + 23, &out[23], dataSize)) HTS_FAIL("[hts] %016llX: cannot read %u bytes of data", (unsigned long long)checksum, dataSize);
        if (out.size() <= HTS_CACHE_BYTES)
        {
            while (h->cacheBytes + out.size() > HTS_CACHE_BYTES && !h->lru.empty())
            {
                std::map<u64, HtsCached>::iterator old = h->cache.find(h->lru.front());
                if (old != h->cache.end()) { h->cacheBytes -= old->second.rec.size(); h->cache.erase(old); }
                h->lru.pop_front();
            }
            HtsCached &c = h->cache[checksum];
            c.rec = out;
            h->lru.push_back(checksum);
            c.pos = --h->lru.end();
            h->cacheBytes += out.size();
        }
    }
    return 1;
}

// A record unpacked into A8R8G8B8 words (px), tmp for the inflate. No lock: the
// buffers are the caller's.
static int hts_decode_record(u64 checksum, const std::vector<u8> &recv, std::vector<u8> &tmp, std::vector<u32> &px, u32 *w, u32 *hgt)
{
    u32 width, height, format, dataSize, i, count;
    u16 texFmt, pixType;
    const u8 *data, *rec = &recv[0];
    width = rd32le(rec); height = rd32le(rec + 4); format = rd32le(rec + 8);
    texFmt = (u16)(rec[12] | rec[13] << 8); pixType = (u16)(rec[14] | rec[15] << 8);
    dataSize = rd32le(rec + 19);
    if (!width || !height || width > 4096 || height > 4096 || !dataSize) HTS_FAIL("[hts] %016llX: bad record %ux%u, %u bytes", (unsigned long long)checksum, width, height, dataSize);
    count = width * height;
    data = rec + 23;
    if (format & GLF_GZ)
    {
        z_stream z;
        u32 expect = count * 4 + 64;   // upper bound for every format handled here
        tmp.resize(expect);
        memset(&z, 0, sizeof(z));
        z.zalloc = hts_zalloc;   // this zlib is built without its own allocator (Z_SOLO)
        z.zfree = hts_zfree;
        if (inflateInit(&z) != Z_OK) HTS_FAIL("[hts] %016llX: inflateInit failed", (unsigned long long)checksum);
        z.next_in = (Bytef *)data; z.avail_in = dataSize;
        z.next_out = &tmp[0]; z.avail_out = expect;
        i = (u32)inflate(&z, Z_FINISH);
        inflateEnd(&z);
        if (i != Z_STREAM_END) HTS_FAIL("[hts] %016llX: inflate returned %d (%u of %u bytes)", (unsigned long long)checksum, (int)i, (u32)z.total_out, expect);
        dataSize = (u32)z.total_out;
        data = &tmp[0];
        format &= ~GLF_GZ;
    }
    px.resize(count);
    if (format == GLF_DXT1) { if (dataSize < ((width + 3) / 4) * ((height + 3) / 4) * 8) return 0; dxt_decode(data, width, height, 1, &px[0]); }
    else if (format == GLF_DXT3) { if (dataSize < ((width + 3) / 4) * ((height + 3) / 4) * 16) return 0; dxt_decode(data, width, height, 3, &px[0]); }
    else if (format == GLF_DXT5) { if (dataSize < ((width + 3) / 4) * ((height + 3) / 4) * 16) return 0; dxt_decode(data, width, height, 5, &px[0]); }
    else if (pixType == GLT_5551 || format == GLF_RGB5_A1)
    {
        if (dataSize < count * 2) return 0;
        for (i = 0; i < count; i++) { u32 v = data[i * 2] | data[i * 2 + 1] << 8; px[i] = argb(e5(v >> 11 & 31), e5(v >> 6 & 31), e5(v >> 1 & 31), (v & 1) * 255); }
    }
    else if (pixType == GLT_4444 || format == GLF_RGBA4)
    {
        if (dataSize < count * 2) return 0;
        for (i = 0; i < count; i++) { u32 v = data[i * 2] | data[i * 2 + 1] << 8; px[i] = argb(e4(v >> 12 & 15), e4(v >> 8 & 15), e4(v >> 4 & 15), e4(v & 15)); }
    }
    else if ((format == GLF_RGBA8 || format == GLF_RGBA || format == GLF_BGRA || texFmt == GLF_RGBA || texFmt == GLF_BGRA) && dataSize >= count * 4)
    {
        int bgra = texFmt == GLF_BGRA || format == GLF_BGRA;
        for (i = 0; i < count; i++)
        {
            const u8 *p = data + i * 4;
            px[i] = bgra ? argb(p[2], p[1], p[0], p[3]) : argb(p[0], p[1], p[2], p[3]);
        }
    }
    else
    {
        H64_INFO("[hts] texture %016llx: format %08X/%04X/%04X not supported", (unsigned long long)checksum, format, texFmt, pixType);
        return 0;
    }
    *w = width; *hgt = height;
    return 1;
}

int h64_hts_find(H64Hts *h, u64 checksum, const u32 **out, u32 *w, u32 *hgt)
{
    int r;
    if (!h) return 0;
    EnterCriticalSection(&h->findLock);
    r = hts_read_record(h, checksum, h->raw) && hts_decode_record(checksum, h->raw, h->tmp, h->pixels, w, hgt);
    if (r) *out = &h->pixels[0];
    LeaveCriticalSection(&h->findLock);
    return r;
}

// ---- Background loading -------------------------------------------------------
static DWORD WINAPI hts_worker(LPVOID arg)
{
    H64Hts *h = (H64Hts *)arg;
    std::vector<u8> rec, tmp;
    for (;;)
    {
        u64 key;
        int ok;
        HtsReady *r;
        WaitForSingleObject(h->wake, INFINITE);
        if (h->quit) break;
        EnterCriticalSection(&h->qLock);
        if (h->queue.empty()) { LeaveCriticalSection(&h->qLock); continue; }
        key = h->queue.front();
        h->queue.pop_front();
        LeaveCriticalSection(&h->qLock);
        r = new HtsReady;
        r->w = r->h = 0;
        r->used = 0;
        EnterCriticalSection(&h->findLock);   // the file and the record cache only: unpacking runs beside the other worker
        ok = hts_read_record(h, key, rec);
        LeaveCriticalSection(&h->findLock);
        if (ok) ok = hts_decode_record(key, rec, tmp, r->px, &r->w, &r->h);
        EnterCriticalSection(&h->qLock);
        h->queued.erase(key);
        if (!ok) { h->failed[key] = 1; delete r; }
        else
        {
            h->ready[key] = r;
            h->readyOrder.push_back(key);
            r->pos = --h->readyOrder.end();
            h->readyBytes += r->px.size() * 4;
        }
        LeaveCriticalSection(&h->qLock);
    }
    return 0;
}

void h64_hts_request(H64Hts *h, u64 checksum)
{
    if (!h || !checksum) return;
    EnterCriticalSection(&h->qLock);
    if (!h->wake)
    {
        int k;
        h->wake = CreateSemaphore(NULL, 0, 0x7FFFFFFF, NULL);
        for (k = 0; h->wake && k < HTS_WORKERS; k++)
        {
            h->thread[k] = CreateThread(NULL, 64 * 1024, hts_worker, h, CREATE_SUSPENDED, NULL);
            if (h->thread[k])
            {
                XSetThreadProcessor(h->thread[k], 3);   // the cover-art thread's, idle while a game runs
                ResumeThread(h->thread[k]);
            }
        }
    }
    if (h->thread[0] && !h->queued.count(checksum) && !h->ready.count(checksum) && !h->failed.count(checksum))
    {
        h->queued[checksum] = 1;
        h->queue.push_back(checksum);
        ReleaseSemaphore(h->wake, 1, NULL);
    }
    LeaveCriticalSection(&h->qLock);
}

int h64_hts_ready(H64Hts *h, u64 checksum)
{
    std::map<u64, HtsReady *>::iterator it;
    int r;
    if (!h) return 0;
    EnterCriticalSection(&h->qLock);
    it = h->ready.find(checksum);
    r = it != h->ready.end() ? (it->second->used ? 2 : 1) : h->failed.count(checksum) ? -1 : 0;
    LeaveCriticalSection(&h->qLock);
    return r;
}

int h64_hts_take(H64Hts *h, u64 checksum, int firstOk, const u32 **out, u32 *w, u32 *hgt)
{
    std::map<u64, HtsReady *>::iterator it;
    int r = 0;
    if (!h) return -1;
    EnterCriticalSection(&h->qLock);
    it = h->ready.find(checksum);
    if (it != h->ready.end() && (it->second->used || firstOk))
    {
        HtsReady *e = it->second;
        size_t n;
        r = e->used ? 2 : 1;
        e->used = 1;
        h->readyOrder.splice(h->readyOrder.end(), h->readyOrder, e->pos);   // now the most recently taken
        // Over the limit: the least recently taken go (never this one, last in the order).
        for (n = h->readyOrder.size(); h->readyBytes > HTS_READY_BYTES && n > 1; n--)
        {
            std::map<u64, HtsReady *>::iterator o = h->ready.find(h->readyOrder.front());
            h->readyOrder.pop_front();
            if (o != h->ready.end()) { h->readyBytes -= o->second->px.size() * 4; delete o->second; h->ready.erase(o); }
        }
        *out = &e->px[0]; *w = e->w; *hgt = e->h;
        if (checksum >> 32) h->lastByTexture[(u32)checksum] = checksum;
    }
    else if (it == h->ready.end() && h->failed.count(checksum)) r = -1;
    LeaveCriticalSection(&h->qLock);
    return r;
}

int h64_hts_take_alike(H64Hts *h, u32 texelChecksum, u64 exclude, const u32 **out, u32 *w, u32 *hgt)
{
    std::map<u32, u64>::iterator k;
    std::map<u64, HtsReady *>::iterator it;
    int r = 0;
    if (!h) return 0;
    EnterCriticalSection(&h->qLock);
    k = h->lastByTexture.find(texelChecksum);
    if (k != h->lastByTexture.end() && k->second != exclude && (it = h->ready.find(k->second)) != h->ready.end() && it->second->used)
    {
        HtsReady *e = it->second;
        h->readyOrder.splice(h->readyOrder.end(), h->readyOrder, e->pos);
        *out = &e->px[0]; *w = e->w; *hgt = e->h;
        r = 1;
    }
    LeaveCriticalSection(&h->qLock);
    return r;
}
