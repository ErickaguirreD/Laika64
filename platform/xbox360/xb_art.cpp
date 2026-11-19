// Harissa64 - box art and game information for the ROM browser (see xb_art.h).
#include <xtl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../core/cart/h64_rom.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_crc32.h"
#include "../../core/common/h64_png.h"
#include "xb_art.h"
#include "xb_lang.h"
#include "xb_menu.h"
#include "xb_net.h"
#include "xb_preview.h"

#define ART_MAX_PNG (4u << 20)          // bytes of a downloaded or cached picture
#define ART_MAX_PIXELS (2048u * 2048u)   // pixels of a picture we decode
#define ART_MAX_DB (4u << 20)
#define ART_MAX_WIKI (256u << 10)
#define ART_MAX_RESULTS 128              // results waiting for the browser (the covers grid wants 3 pages at once; xb_menu.cpp ART_VIEWS)
#define ART_MAX_WANTS 128
#define ART_DB_MAX_AGE_DAYS 30           // "download all" refreshes older databases

// What the worker knows of a ROM file.
struct RomId
{
    int ok;
    char key[16], code[8];
    int version;
    char stem[128];
    char hack[96];   // a hack (s_hacks, or by the ROM catalogue): its ScreenScraper title, else "" - its own name
    char header[32]; // a hack's name in its header without its version ("MK64 - AMPED UP"), searched too
};

static CRITICAL_SECTION s_lock;
static HANDLE s_event, s_thread;
static char s_dir[64];                       // "game:\art\games"
static volatile LONG s_started, s_netAllowed, s_paused;
static volatile LONG s_extrasAllowed = 1;   // ArtSetExtras
static volatile LONG s_kind, s_kindGen;      // ArtSetKind; s_kindGen counts its changes

// Each kind's folder in libretro-thumbnails, and its files in the cache (<key>.<ext>).
// (The 3D box and the cartridge are not in libretro-thumbnails: ScreenScraper's, download_missing.)
static const char *const s_kindFolder[ART_KIND_COUNT] = { "Named_Boxarts", "Named_Snaps", "Named_Titles", "Named_Logos", "Box3D", "Cartridge",
                                                           "Mix", "BoxBack", "Fanart" };
static const char *const s_kindPng[ART_KIND_COUNT] = { "png", "snap.png", "title.png", "logo.png", "box3d.png", "cart.png", "mix.png", "back.png", "fanart.png" };
static const char *const s_kindCov[ART_KIND_COUNT] = { "cov", "snap.cov", "title.cov", "logo.cov", "box3d.cov", "cart.cov", "mix.cov", "back.cov", "fanart.cov" };
static const char *const s_kindNone[ART_KIND_COUNT] = { "", "snap.none", "title.none", "logo.none", "box3d.none", "cart.none", "mix.none", "back.none",
                                                         "fanart.none" };   // "looked for, not there"

// Shared with the UI thread (under s_lock).
static std::vector<std::string> s_wants;
static u32 s_wantGen;
static std::set<std::string> s_delivered;    // ROMs whose result was sent since ArtForgetAll
static std::deque<ArtResult> s_results;
static std::vector<std::string> s_bulk;
static size_t s_bulkPos;
static int s_bulkRetry, s_bulkChanged;       // ArtBulkStart's `retry`; ROMs whose files changed in this pass
static ArtStatus s_status;
static std::string s_note;

// The worker's own.
static std::set<std::string> s_tried;        // selections whose downloads were tried this session (for the kind s_triedGen)
static LONG s_triedGen;
static std::map<std::string, int> s_kindFor;   // ArtShowKind (under s_lock)
static std::set<std::string> s_triedAll;       // selections whose other kinds (and description) were looked for too (the worker's)
static volatile LONG s_langGen;                // ArtSetLang's changes ...
static LONG s_langSeen;                        // ... the last the worker saw
static char s_lang[8] = "en";   // the menus' language (ArtSetLang; under s_lock)
static std::map<std::string, RomId> s_ids;
// ROMs with nothing more to fetch for a kind of picture ("<kind>|<path>" lines in
// <s_dir>\complete.txt): the automatic pass over every ROM (each time the list
// opened, also after a game) skips them - it read every ROM's header and files
// again, keeping the drive busy while the covers were wanted (hitches with many
// ROMs). "Download all" (retry) still goes through all of them.
static std::set<std::string> s_complete;
static char s_completePath[96];
static H64GameDb *s_db;

static void pak_drop(const char *key, int kind);   // covers.pak (below)

// ---- Files ----
// The art folder's file names and sizes, read once (names_scan) and kept up to
// date by what is written or deleted here: with hundreds of games the folder
// holds thousands of files, and every look-up on the drive (is there a
// snap.png? a .desc.es?) went through its whole directory - several per cover.
// Files elsewhere (db\) are looked up on the drive as before.
static CRITICAL_SECTION s_namesLock;
static std::map<std::string, u32> s_names;   // lower-case name -> size
static volatile LONG s_namesReady;

// The name of `path` in the art folder (lower case), or 0 when it is not directly in it.
static int names_key(const char *path, std::string *out)
{
    size_t n = strlen(s_dir);
    const char *rest;
    if (!s_namesReady || _strnicmp(path, s_dir, n) || path[n] != '\\') return 0;
    rest = path + n + 1;
    if (strchr(rest, '\\')) return 0;
    out->assign(rest);
    for (n = 0; n < out->size(); n++) (*out)[n] = (char)tolower((u8)(*out)[n]);
    return 1;
}

static void names_set(const char *path, u32 size, int present)
{
    std::string k;
    if (!names_key(path, &k)) return;
    EnterCriticalSection(&s_namesLock);
    if (present) s_names[k] = size;
    else s_names.erase(k);
    LeaveCriticalSection(&s_namesLock);
}

// -1: not known here (look on the drive), 0: not there, 1: there (*size set).
static int names_find(const char *path, u32 *size)
{
    std::string k;
    std::map<std::string, u32>::const_iterator it;
    int r;
    if (!names_key(path, &k)) return -1;
    EnterCriticalSection(&s_namesLock);
    it = s_names.find(k);
    r = it != s_names.end();
    if (r && size) *size = it->second;
    LeaveCriticalSection(&s_namesLock);
    return r;
}

static void names_scan(void)
{
    char pat[96];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    std::map<std::string, u32> names;
    _snprintf(pat, sizeof(pat) - 1, "%s\\*", s_dir);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::string k(fd.cFileName);
            size_t i;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            for (i = 0; i < k.size(); i++) k[i] = (char)tolower((u8)k[i]);
            names[k] = fd.nFileSizeLow;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    EnterCriticalSection(&s_namesLock);
    s_names.swap(names);
    LeaveCriticalSection(&s_namesLock);
    InterlockedExchange(&s_namesReady, 1);
    H64_INFO("[art] %u files in %s", (unsigned)s_names.size(), s_dir);
}

static int file_read(const char *path, u8 **data, u32 *len, u32 maxLen)
{
    HANDLE h;
    DWORD size, got = 0;
    u8 *buf;
    *data = NULL;
    *len = 0;
    if (!names_find(path, NULL)) return 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > maxLen) { CloseHandle(h); return 0; }
    buf = (u8 *)malloc(size + 1);
    if (!buf || !ReadFile(h, buf, size, &got, NULL) || got != size)
    {
        free(buf);
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    buf[size] = 0;
    *data = buf;
    *len = size;
    return 1;
}

static int file_write(const char *path, const void *data, u32 len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, data, len, &put, NULL) && put == len;
    CloseHandle(h);
    if (!ok) DeleteFileA(path);
    names_set(path, len, ok);
    return ok;
}

static int file_exists(const char *path)
{
    int r = names_find(path, NULL);
    return r >= 0 ? r : GetFileAttributesA(path) != (DWORD)0xFFFFFFFF;
}

static void file_delete(const char *path)
{
    DeleteFileA(path);
    names_set(path, 0, 0);
}

// A file written by someone else (xb_preview.cpp's ScreenScraper pictures) into the names known:
// file_exists believed what it knew before, "not there", and the picture never showed.
static void file_noted(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int ok = GetFileAttributesExA(path, GetFileExInfoStandard, &fa) != 0;
    names_set(path, ok ? fa.nFileSizeLow : 0, ok);
}

// "Not on ScreenScraper" markers hold SS_NONE: those of an older lookup ("-": by the file and title
// only) do not count for the pictures only ScreenScraper has (the 3D box, the cartridge, the box
// libretro lacks), which are asked for once more - the No-Intro name and the title's parts find many.
#define SS_NONE "-2"
static int ss_marker(const char *path)
{
    u32 size = 0;
    int r = names_find(path, &size);
    if (r < 0)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        r = GetFileAttributesExA(path, GetFileExInfoStandard, &fa) != 0;
        size = r ? fa.nFileSizeLow : 0;
    }
    return r > 0 && size >= sizeof(SS_NONE) - 1;
}

// Days since the file was written (a large number when it is missing).
static int file_age_days(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILETIME now;
    ULARGE_INTEGER a, b;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 1 << 20;
    GetSystemTimeAsFileTime(&now);
    a.LowPart = fa.ftLastWriteTime.dwLowDateTime;
    a.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (int)((b.QuadPart - a.QuadPart) / (10000000ULL * 86400ULL)) : 0;
}

static void art_path(const char *key, const char *ext, char *out, size_t size)
{
    _snprintf(out, size - 1, "%s\\%s.%s", s_dir, key, ext);
    out[size - 1] = 0;
}

// ---- ROM identity ----
// Hacks that keep their original's game code (Smash Remix: NALE, Super Smash Bros.'s), known by the
// name in their header: the databases would give them the original's box. They get a key of their
// own (by CRC, as a ROM without a game code) and their box from ScreenScraper, found by `title`.
static const struct { const char *header, *title; } s_hacks[] = {
    { "SMASH REMIX", "Smash Remix" },
    { "REMIX EXTRA", "Smash Remix" },   // not on ScreenScraper itself: Smash Remix's box
    { "ZELDA MASTER QUEST", "Ocarina of Time Master Quest" },   // Ocarina of Time's NZLE
};

static const char *hack_title(const H64Rom *rom)
{
    size_t i;
    for (i = 0; i < sizeof(s_hacks) / sizeof(s_hacks[0]); i++)
        if (!_stricmp(rom->name, s_hacks[i].header)) return s_hacks[i].title;
    return NULL;
}

// A hack: one of those above, or one the ROM catalogue (mupen64plus's) calls a hack or a modified ROM, or does not
// know at all while the ROM keeps an N64 game's code (a newer hack: Mario Kart 64 Amped Up, its original's code) -
// the databases, by that code, would give it its original's box and page.
static int rom_is_hack(const H64Rom *rom)
{
    int k, i, code = 1;
    if (hack_title(rom)) return 1;
    k = h64_rom_catalogue(rom->crc1, rom->crc2, NULL);
    if (k == H64_ROMKIND_HACK || k == H64_ROMKIND_OTHER) return 1;
    for (i = 0; i < 4; i++)
        code = code && ((rom->gameCode[i] >= 'A' && rom->gameCode[i] <= 'Z') || (rom->gameCode[i] >= '0' && rom->gameCode[i] <= '9'));
    return k == H64_ROMKIND_UNKNOWN && code;
}

// A version at a title's end ("1.4", "v2.0.1", "3.21") left out: the databases name a hack without it.
static void strip_version(char *t)
{
    size_t k = strlen(t), e, b;
    while (k && t[k - 1] == ' ') t[--k] = 0;
    e = k;
    while (e > 0 && ((t[e - 1] >= '0' && t[e - 1] <= '9') || t[e - 1] == '.')) e--;
    b = e;
    if (b > 0 && (t[b - 1] == 'v' || t[b - 1] == 'V')) b--;
    if (e < k && b > 0 && t[b - 1] == ' ')
    {
        t[b - 1] = 0;
        k = b - 1;
        while (k && t[k - 1] == ' ') t[--k] = 0;
    }
}

// A hack's name in its header, without its version ("MK64 - AMPED UP 3.21" -> "MK64 - AMPED UP").
static void hack_header(const H64Rom *rom, char *out, size_t size)
{
    _snprintf(out, size - 1, "%s", rom->name);
    out[size - 1] = 0;
    strip_version(out);
}

// The key of a ROM's files in the cache (h64_gameinfo_key, or the CRC for a hack: its own files, not its original's).
static void art_key(const H64Rom *rom, char *key)
{
    if (rom_is_hack(rom)) sprintf(key, "X%08X", (unsigned)rom->crc1);
    else h64_gameinfo_key(rom->gameCode, rom->crc1, key);
}

static const RomId *rom_id(const std::string &path)
{
    std::map<std::string, RomId>::iterator it = s_ids.find(path);
    RomId id;
    H64Rom rom;
    const char *base, *dot;
    if (it != s_ids.end()) return &it->second;
    memset(&id, 0, sizeof(id));
    if (RomReadHeader(path.c_str(), &rom))
    {
        id.ok = 1;
        art_key(&rom, id.key);
        if (rom_is_hack(&rom)) hack_header(&rom, id.header, sizeof(id.header));
        if (hack_title(&rom)) strncpy(id.hack, hack_title(&rom), sizeof(id.hack) - 1);
        else if (rom_is_hack(&rom)) id.hack[0] = 1;   // its own name: the file's (below, once the stem is known)
        strncpy(id.code, rom.gameCode, sizeof(id.code) - 1);
        id.version = rom.version;
        h64_rom_free(&rom);
    }
    base = strrchr(path.c_str(), '\\');
    base = base ? base + 1 : path.c_str();
    dot = strrchr(base, '.');
    _snprintf(id.stem, sizeof(id.stem) - 1, "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
    if (id.hack[0] == 1)
    {
        // a hack found by the catalogue: its name as its file has it, its tags ("(USA)", "[!]") left out
        size_t n = strcspn(id.stem, "([");
        _snprintf(id.hack, sizeof(id.hack) - 1, "%.*s", (int)n, id.stem);
        while (n && (id.hack[n - 1] == ' ' || id.hack[n - 1] == '_')) id.hack[--n] = 0;
        for (n = 0; id.hack[n]; n++)
            if (id.hack[n] == '_') id.hack[n] = ' ';
    }
    s_ids[path] = id;
    return &s_ids[path];
}

// A hack's whole file's CRC32 in hex (ScreenScraper knows a hack's own page by it), read once (the worker's thread
// only); "" when the file could not be read.
// (Read by the worker's thread and the preview's - its videos: under s_crcLock, the reading itself outside it.)
static std::map<std::string, std::string> s_fileCrc;
static CRITICAL_SECTION s_crcLock;
static volatile LONG s_crcLockMade;
static void crc_lock(void)
{
    if (InterlockedCompareExchange(&s_crcLockMade, 1, 0) == 0) { InitializeCriticalSection(&s_crcLock); InterlockedExchange(&s_crcLockMade, 2); }
    while (s_crcLockMade != 2) Sleep(0);
    EnterCriticalSection(&s_crcLock);
}

static std::string file_crc_text(const std::string &path)
{
    std::map<std::string, std::string>::iterator it;
    std::string r;
    FILE *f;
    crc_lock();
    it = s_fileCrc.find(path);
    if (it != s_fileCrc.end()) r = it->second;
    LeaveCriticalSection(&s_crcLock);
    if (it != s_fileCrc.end()) return r;
    f = fopen(path.c_str(), "rb");
    if (f)
    {
        u8 *chunk = (u8 *)malloc(256 * 1024);
        u32 crc = 0;
        size_t n;
        char hex[12];
        while (chunk && (n = fread(chunk, 1, 256 * 1024, f)) > 0) crc = h64_crc32(crc, chunk, n);
        fclose(f);
        free(chunk);
        sprintf(hex, "%08X", (unsigned)crc);
        r = hex;
    }
    crc_lock();
    s_fileCrc[path] = r;
    LeaveCriticalSection(&s_crcLock);
    return r;
}

static const char *file_crc(const std::string &path)
{
    static std::string last;   // (the worker's thread only)
    last = file_crc_text(path);
    return last.c_str();
}

static int info_read(const char *key, H64GameInfo *info);   // below

int ArtSsHint(const char *romPath, char *crc, size_t crcSize, char *header, size_t headerSize, char *base, size_t baseSize)
{
    H64Rom rom;
    H64GameInfo info;
    char key[16], stem[128];
    const char *b, *dot;
    int hack;
    crc[0] = header[0] = base[0] = 0;
    if (!RomReadHeader(romPath, &rom)) return 0;
    hack = rom_is_hack(&rom);
    if (hack)
    {
        art_key(&rom, key);
        hack_header(&rom, header, headerSize);
        // (its header's name only when it says more than its file's)
        b = strrchr(romPath, '\\');
        b = b ? b + 1 : romPath;
        dot = strrchr(b, '.');
        _snprintf(stem, sizeof(stem) - 1, "%.*s", dot ? (int)(dot - b) : (int)strlen(b), b);
        stem[sizeof(stem) - 1] = 0;
        if (!_strnicmp(stem, header, strlen(header))) header[0] = 0;
        if (info_read(key, &info) && info.base[0]) { strncpy(base, info.base, baseSize - 1); base[baseSize - 1] = 0; }
    }
    h64_rom_free(&rom);
    if (hack) { strncpy(crc, file_crc_text(romPath).c_str(), crcSize - 1); crc[crcSize - 1] = 0; }
    return hack;
}

// What is known of each game (<key>.txt), kept once read: the browser asked for
// it again at every page. Under s_namesLock (the UI thread reads it too).
struct InfoMemo { int ok; H64GameInfo info; };
static std::map<std::string, InfoMemo> s_infoMemo;
static volatile LONG s_scoresReady;          // the critics' scores file is read (score_for, below)
static int score_for(const char *key);

static int info_read(const char *key, H64GameInfo *info)
{
    char path[128];
    u8 *data;
    u32 len;
    InfoMemo m;
    std::map<std::string, InfoMemo>::const_iterator it;
    EnterCriticalSection(&s_namesLock);
    it = s_infoMemo.find(key);
    if (it != s_infoMemo.end()) *info = it->second.info;
    m.ok = it != s_infoMemo.end() ? it->second.ok : -1;
    LeaveCriticalSection(&s_namesLock);
    if (m.ok >= 0)
    {
        if (s_scoresReady) info->score = score_for(key);
        return m.ok;
    }
    h64_gameinfo_clear(&m.info);
    art_path(key, "txt", path, sizeof(path));
    m.ok = 0;
    if (file_read(path, &data, &len, 64 * 1024))
    {
        m.ok = h64_gameinfo_parse(&m.info, (const char *)data, len);
        free(data);
    }
    EnterCriticalSection(&s_namesLock);
    s_infoMemo[key] = m;
    LeaveCriticalSection(&s_namesLock);
    *info = m.info;
    if (s_scoresReady) info->score = score_for(key);
    return m.ok;
}

static int load_info(const RomId *id, H64GameInfo *info) { return info_read(id->key, info); }

static void save_info(const RomId *id, const H64GameInfo *info)
{
    char path[128], text[4096];
    int n = h64_gameinfo_format(info, text, sizeof(text));
    art_path(id->key, "txt", path, sizeof(path));
    EnterCriticalSection(&s_namesLock);
    s_infoMemo.erase(id->key);   // read again from what is written
    LeaveCriticalSection(&s_namesLock);
    if (!file_write(path, text, (u32)n)) H64_WARN("[art] cannot write %s", path);
}

// ---- Databases ----
// Loads the databases from the db folder, downloading those missing (or all
// of them when `refresh`). Returns 1 when the No-Intro list is there.
static int ensure_db(int net, int refresh)
{
    int i, games;
    if (s_db && !refresh) return 1;
    if (s_db) { h64_gamedb_free(s_db); s_db = NULL; }
    s_db = h64_gamedb_new();
    for (i = 0; i < H64_GAMEDB_FILE_COUNT; i++)
    {
        const H64GameDbFile *f = &h64_gamedb_files[i];
        char path[128];
        u8 *data = NULL;
        u32 len = 0;
        int have;
        _snprintf(path, sizeof(path) - 1, "%s\\db\\%s", s_dir, f->file);
        path[sizeof(path) - 1] = 0;
        have = file_exists(path) && !(refresh && file_age_days(path) >= ART_DB_MAX_AGE_DAYS);
        if (have) have = file_read(path, &data, &len, ART_MAX_DB);
        if (!have && net)
        {
            int st = NetGet(f->host, f->path, 1, &data, &len, ART_MAX_DB, NULL, 0);
            if (st == 200 && data)
            {
                file_write(path, data, len);
                have = 1;
                H64_INFO("[art] downloaded %s (%u bytes)", f->file, len);
            }
            else
            {
                free(data);
                data = NULL;
                H64_INFO("[art] %s: download failed (status %d)", f->file, st);
                if (!have) have = file_read(path, &data, &len, ART_MAX_DB);   // an old copy is better than none
            }
        }
        if (!have) continue;
        {
            int n = f->json ? h64_gamedb_add_gamedb_json(s_db, (const char *)data, len) : h64_gamedb_add_dat(s_db, (const char *)data, len);
            if (n < 0) H64_WARN("[art] %s: not readable", f->file);
        }
        free(data);
    }
    games = h64_gamedb_count(s_db);
    H64_INFO("[art] databases: %d games", games);
    if (!games)
    {
        h64_gamedb_free(s_db);
        s_db = NULL;
        return 0;
    }
    return 1;
}

// ---- Downloads ----
// 1: a picture was saved, 0: none of the names has one, -1: the network failed.
static int fetch_cover(const RomId *id, char (*names)[128], int count, int kind)
{
    char base[128];
    int i, netFail = 0;
    // H64_THUMB_PATH is the boxes' folder: the other kinds sit beside it.
    _snprintf(base, sizeof(base) - 1, "/libretro-thumbnails/Nintendo_-_Nintendo_64/master/%s/", s_kindFolder[kind]);
    base[sizeof(base) - 1] = 0;
    for (i = 0; i < count; i++)
    {
        char thumb[128], enc[400], path[600], file[128];
        u8 *data = NULL;
        u32 len = 0;
        int st, w, h;
        h64_gameinfo_thumb_name(names[i], thumb, sizeof(thumb));
        h64_url_encode(thumb, enc, sizeof(enc));
        _snprintf(path, sizeof(path) - 1, "%s%s.png", base, enc);
        path[sizeof(path) - 1] = 0;
        st = NetGet(H64_THUMB_HOST, path, 1, &data, &len, ART_MAX_PNG, NULL, 0);
        if (st == 200 && data && len < 256 && data[0] != 0x89 && len > 4 && !memcmp(data + len - 4, ".png", 4) && !strchr((char *)data, '/'))
        {
            // A git symbolic link (libretro-thumbnails names aliases this way): its text is the file it points to.
            h64_url_encode((const char *)data, enc, sizeof(enc));
            free(data);
            data = NULL;
            _snprintf(path, sizeof(path) - 1, "%s%s", base, enc);
            path[sizeof(path) - 1] = 0;
            st = NetGet(H64_THUMB_HOST, path, 1, &data, &len, ART_MAX_PNG, NULL, 0);
        }
        if (st == 200 && data)
        {
            u8 *px = h64_png_decode(data, len, &w, &h, ART_MAX_PIXELS);
            if (px)
            {
                free(px);
                art_path(id->key, s_kindPng[kind], file, sizeof(file));
                if (file_write(file, data, len))
                {
                    pak_drop(id->key, kind);
                    H64_INFO("[art] %s: %s \"%s\" (%dx%d, %u bytes)", id->key, s_kindFolder[kind], names[i], w, h, len);
                    free(data);
                    return 1;
                }
            }
            else
                H64_INFO("[art] %s: \"%s\" is not a usable PNG", id->key, names[i]);
        }
        else if (!st)
            netFail = 1;
        free(data);
    }
    return netFail ? -1 : 0;
}

static int fetch_wiki(H64GameInfo *info)
{
    char pages[3][256];
    int n = h64_gameinfo_wiki_pages(info, pages, 3), i, netFail = 0;
    for (i = 0; i < n; i++)
    {
        char path[400];
        u8 *data = NULL;
        u32 len = 0;
        int st;
        _snprintf(path, sizeof(path) - 1, "%s%s", H64_WIKI_PATH, pages[i]);
        path[sizeof(path) - 1] = 0;
        st = NetGet(H64_WIKI_HOST, path, 1, &data, &len, ART_MAX_WIKI, NULL, 0);
        if (st == 200 && data && h64_gameinfo_wiki_extract((const char *)data, len, info->description, sizeof(info->description)))
        {
            free(data);
            return 1;
        }
        if (!st) netFail = 1;
        free(data);
        if (netFail) break;
    }
    return netFail ? -1 : 0;
}

// ---- The critics' scores (the stars in the lists) ----
// One file for every game, data/ratings.txt of the repository (made on a PC by
// tools/gen_ratings.py from Wikipedia's review boxes): "<game code without its
// region letter> <score>" lines. Kept in db\ratings.txt, downloaded again after a
// week; every game's score comes from it at once - no request per game.
#define RATINGS_HOST "raw.githubusercontent.com"
#define RATINGS_PATH "/ErickaguirreD/Laika64/main/data/ratings.txt"
#define RATINGS_MAX (256u << 10)
static std::map<std::string, int> s_scores;   // under s_namesLock
static int s_scoresTried;                     // the worker's: downloaded (or tried) this session

static int scores_parse(const char *text, u32 len)
{
    std::map<std::string, int> m;
    const char *p = text, *end = text + len;
    while (p < end)
    {
        const char *eol = (const char *)memchr(p, '\n', end - p);
        char code[8];
        int v;
        if (!eol) eol = end;
        if (*p != '#' && eol - p < 32 && sscanf(p, "%7s %d", code, &v) == 2 && v >= 1 && v <= 100) m[code] = v;
        p = eol + 1;
    }
    if (m.empty()) return 0;
    EnterCriticalSection(&s_namesLock);
    s_scores.swap(m);
    LeaveCriticalSection(&s_namesLock);
    InterlockedExchange(&s_scoresReady, 1);
    return (int)s_scores.size();
}

// A game's score from the file (0: none), for its cache key: the game code when it has one.
static int score_for(const char *key)
{
    std::map<std::string, int>::const_iterator it;
    int v = 0;
    if (strlen(key) != 4 || key[0] == 'X') return 0;
    EnterCriticalSection(&s_namesLock);
    it = s_scores.find(std::string(key, 3));
    if (it != s_scores.end()) v = it->second;
    LeaveCriticalSection(&s_namesLock);
    return v;
}

static void set_busy(const std::string &path);

// The title a game is looked for by on ScreenScraper when its file name does not find it: a hack's
// (s_hacks), the databases' name, else the file name - without their tags ("Dinosaur Planet (USA)
// (Proto)" -> "Dinosaur Planet").
static void search_title(const RomId *id, const H64GameInfo *info, char *title, size_t size)
{
    const char *from = id->hack[0] ? id->hack : info->found && info->name[0] ? info->name : id->stem;
    size_t n = strcspn(from, "(["), k;
    _snprintf(title, size - 1, "%.*s", (int)n, from);
    title[size - 1] = 0;
    for (k = 0; title[k]; k++)
        if (title[k] == '_') title[k] = ' ';
    while (k > 0 && title[k - 1] == ' ') title[--k] = 0;
    if (id->hack[0]) strip_version(title);   // a hack's version at its end ("1.4", "v2.0.1"): the databases name it without
}

// The file name the databases give a game (No-Intro's, with .z64), which ScreenScraper knows a GoodN64
// name by when it does not know the file's own ("1080 Snowboarding (Japan, USA) (En,Ja).z64" for
// "N64 1080 Snowboarding (JU).z64"); "" for a hack (that name is its original's) or a game not in them.
static void alt_name(const RomId *id, const H64GameInfo *info, char *out, size_t size)
{
    out[0] = 0;
    if (id->hack[0] || !info->found || !info->name[0]) return;
    _snprintf(out, size - 1, "%s.z64", info->name);
    out[size - 1] = 0;
}

// Each kind of picture on ScreenScraper (PreviewFetchMedia): the box, the picture from the game, the
// title screen, the logo (the "wheel"; some games have only its HD one), the 3D box, the cartridge.
static const char *const s_kindSs[ART_KIND_COUNT] = { "box-2D", "ss", "sstitle", "wheel,wheel-hd", "box-3D", "support-2D", "mixrbv2,mixrbv1",
                                                       "box-2D-back", "fanart" };

// A picture other than the box from ScreenScraper (libretro's thumbnails did not have it, or never
// can: the 3D box, a game not in the databases, a hack): saved, or its "not there" marker written.
// 1 when it came.
static int ss_picture(const RomId *id, const H64GameInfo *info, const std::string &path, int kind, const char *png, const char *none)
{
    char title[128], alt[160];
    int r;
    search_title(id, info, title, sizeof(title));
    alt_name(id, info, alt, sizeof(alt));
    r = PreviewFetchMedia(path.c_str(), alt, title, s_kindSs[kind], png, id->hack[0] ? file_crc(path) : NULL, id->hack[0] ? id->header : NULL);
    H64_INFO("[art] %s (%s): ScreenScraper %s for \"%s\": %s", id->key, id->stem, s_kindSs[kind], title, r > 0 ? "saved" : r == 0 ? "none" : "no answer");
    if (r > 0)
    {
        file_noted(png);
        file_delete(none);
        return 1;
    }
    if (r == 0) file_write(none, SS_NONE, sizeof(SS_NONE) - 1);   // not there: asked again only by "download all"
    return 0;
}

// Downloads what a ROM is missing. `bulk`: also retries what was not found
// before. Returns 1 when its files changed.
static int s_force;   // download_missing: the settings' limits off - the detailed view asked for that picture (step_fetch)

static int download_missing(const std::string &path, int bulk, int kind)
{
    const RomId *id = rom_id(path);
    H64GameInfo info;
    char png[128], none[128], ssNone[128], covers[8][128];
    int have, hasPng, nc = 0, changed = 0, missing, ssTried = 0;
    if (!id->ok) return 0;
    if (!s_force && ART_KIND_PICKER_ONLY(kind)) return 0;   // only the detailed view's picker asks for those
    if (!s_force && kind != ART_KIND_BOX && !ART_KIND_VIEW_ONLY(kind) && !s_extrasAllowed) return 0;   // the box art only (Game images > downloads)
    if (!s_force && ART_KIND_VIEW_ONLY(kind) && s_kind != kind) return 0;   // only while their view shows them
    have = load_info(id, &info);
    art_path(id->key, s_kindPng[kind], png, sizeof(png));
    hasPng = file_exists(png);
    // Known not to be there: the box in the game's information, the others by a marker file.
    // A box libretro did not have was looked for on ScreenScraper once too (marker <key>.ssbox.none).
    if (kind == ART_KIND_BOX)
    {
        missing = info.cover == 0;
        art_path(id->key, "ssbox.none", ssNone, sizeof(ssNone));
        ssTried = ss_marker(ssNone);
    }
    else
    {
        art_path(id->key, s_kindNone[kind], none, sizeof(none));
        missing = ART_KIND_SS_ONLY(kind) ? ss_marker(none) : file_exists(none);
    }
    // What is known to be missing is only retried by "download all" (a game not in the databases:
    // once more for its box while that is unknown, and for each other picture, ScreenScraper's below).
    const int hackAsk = id->hack[0] && have && info.ss < 0;   // a hack whose own page was never asked for (below)
    if (have && !info.found && !bulk && !hackAsk && (hasPng || (kind == ART_KIND_BOX ? info.cover >= 0 : missing)))
        return 0;
    if (have && info.found && !hackAsk && (hasPng || (missing && !bulk && (kind != ART_KIND_BOX || ssTried))) && info.wiki >= 0) return 0;
    // (The first call waits for the network; "download all" retried it when it started.)
    if (!NetStart(0))
    {
        s_status.offline = 1;
        return 0;
    }
    s_status.offline = 0;
    // Why the network is asked (the carousel was seen downloading again what was there).
    H64_INFO("[art] %s: fetch %s (png %d, known missing %d, info %d/%d, cover %d, wiki %d, bulk %d)", id->key, s_kindFolder[kind],
             hasPng, missing, have, have ? info.found : -1, have ? info.cover : -9, have ? info.wiki : -9, bulk);
    set_busy(path);   // the browser shows "Downloading" from here
    if (ensure_db(1, 0))
    {
        H64GameInfo fresh;
        h64_gameinfo_clear(&fresh);
        h64_gamedb_lookup(s_db, id->code, id->version, id->stem, &fresh, covers, 8, &nc);
        if (!have || !info.found)
        {
            // First identification (or a retry): what the databases say.
            if (have && info.wiki > 0) { strcpy(fresh.description, info.description); fresh.wiki = 1; }
            {
                char box[128];
                art_path(id->key, "png", box, sizeof(box));
                fresh.cover = file_exists(box) ? 1 : -1;
            }
            info = fresh;
            have = 1;
            changed = 1;
            save_info(id, &info);
            H64_INFO("[art] %s (%s): %s", id->key, id->stem, info.found ? info.name : "not in the databases");
        }
    }
    if (!have) return changed;
    if (id->hack[0] && info.ss < 0)
    {
        // A hack: its own page on ScreenScraper (by its file's checksum: its name, description, makers, date);
        // else the databases' (its original's) under its own name, "a hack of" its original.
        H64GameInfo own;
        char lang[8], base[96];
        int r;
        EnterCriticalSection(&s_lock);
        strcpy(lang, s_lang);
        LeaveCriticalSection(&s_lock);
        _snprintf(base, sizeof(base) - 1, "%s", info.found ? info.title : "");
        base[sizeof(base) - 1] = 0;
        h64_gameinfo_clear(&own);
        own.cover = info.cover;
        r = PreviewFetchInfo(path.c_str(), file_crc(path), lang, &own);
        if (r > 0)
        {
            own.found = 1;
            own.ss = 1;
            own.wiki = 0;     // (its description is ScreenScraper's: no Wikipedia page to look for)
            own.score = 0;
            if (!own.title[0]) _snprintf(own.title, sizeof(own.title) - 1, "%s", id->hack);
            if (_stricmp(base, own.title)) _snprintf(own.base, sizeof(own.base) - 1, "%s", base);
            info = own;
        }
        else if (r == 0)
        {
            info.ss = 0;
            if (base[0] && _stricmp(base, id->hack)) _snprintf(info.base, sizeof(info.base) - 1, "%s", base);
            _snprintf(info.title, sizeof(info.title) - 1, "%s", id->hack);   // its own name, its original's the rest
        }
        if (r >= 0)
        {
            save_info(id, &info);
            changed = 1;
            H64_INFO("[art] %s (%s): hack, %s", id->key, id->stem, r > 0 ? "its own page on ScreenScraper" : info.base[0] ? "its original's page" : "no page");
        }
    }
    if (ART_KIND_SS_ONLY(kind))
    {
        // ScreenScraper's 3D box or cartridge (libretro has none), the game found as for the box below.
        if (hasPng || (missing && !bulk)) return changed;
        return changed | ss_picture(id, &info, path, kind, png, none);
    }
    if (kind == ART_KIND_BOX && !hasPng &&
        (id->hack[0] || !info.found ? !missing || bulk : missing && (!ssTried || bulk)))
    {
        // Not in the databases, or a hack the databases take for its original, or a game libretro has
        // no box of under the databases' name (Dinosaur Planet): ScreenScraper's box, the game found
        // by file name and size, else by title.
        char title[128], alt[160];
        int r;
        search_title(id, &info, title, sizeof(title));
        alt_name(id, &info, alt, sizeof(alt));
        r = PreviewFetchMedia(path.c_str(), alt, title, "box-2D", png, id->hack[0] ? file_crc(path) : NULL, id->hack[0] ? id->header : NULL);
        if (r == 0 && id->hack[0] && info.base[0])
        {
            // a hack ScreenScraper does not know: its original's box (as its page is)
            if (!nc && info.name[0]) { strcpy(covers[0], info.name); nc = 1; }
            if (nc) r = fetch_cover(id, covers, nc, kind);
            H64_INFO("[art] %s (%s): its original's box: %s", id->key, id->stem, r > 0 ? "saved" : r == 0 ? "none" : "no answer");
        }
        if (r > 0) { file_noted(png); pak_drop(id->key, kind); }
        if (r == 0 && info.found && !id->hack[0]) file_write(ssNone, SS_NONE, sizeof(SS_NONE) - 1);   // libretro's miss, ScreenScraper's too
        H64_INFO("[art] %s (%s): ScreenScraper box for \"%s\": %s", id->key, id->stem, title, r > 0 ? "saved" : r == 0 ? "none" : "no answer");
        if (r >= 0 || info.cover != r)
        {
            info.cover = r > 0 ? 1 : r == 0 ? 0 : -1;
            save_info(id, &info);
            changed = 1;
        }
    }
    else if (kind != ART_KIND_BOX && (id->hack[0] || !info.found) && !hasPng && (!missing || bulk))
    {
        const int r = ss_picture(id, &info, path, kind, png, none);   // libretro's would be the original's, or it has none
        changed |= r;
        if (!r && id->hack[0] && info.base[0])
        {
            // a hack ScreenScraper does not know: its original's picture
            if (!nc && info.name[0]) { strcpy(covers[0], info.name); nc = 1; }
            if (nc && fetch_cover(id, covers, nc, kind) > 0)
            {
                file_delete(none);
                changed = 1;
            }
        }
    }
    else if (info.found && !id->hack[0] && !hasPng && (!missing || bulk))
    {
        int r;
        if (!nc && info.name[0]) { strcpy(covers[0], info.name); nc = 1; }
        r = fetch_cover(id, covers, nc, kind);
        if (kind == ART_KIND_BOX)
        {
            if (r >= 0 || info.cover != r)
            {
                info.cover = r > 0 ? 1 : r == 0 ? 0 : -1;
                save_info(id, &info);
                changed = 1;
            }
        }
        else if (r > 0)
        {
            file_delete(none);
            changed = 1;
        }
        else if (r == 0)
            changed |= ss_picture(id, &info, path, kind, png, none);   // not in libretro's: ScreenScraper's, else marked missing
    }
    if (info.found && info.wiki < 0)
    {
        int r = fetch_wiki(&info);
        if (r >= 0)
        {
            info.wiki = r;
            save_info(id, &info);
            changed = 1;
        }
    }
    return changed;
}

// ---- Descriptions in the menus' language (ArtSetLang) ----
// The English page found for a game (fetch_wiki) names the same article in
// the other Wikipedias (its language links); that article's summary, cut to
// whole sentences like the English one, is kept in <key>.desc.<lang> (UTF-8:
// Latin letters with their accents, Cyrillic - what the menu font has) and
// shown in place of the English text; <key>.desc.<lang>.none when there is
// none. Arabic stays English: its letters would need joining at run time.

static int lang_has_desc(const char *lang) { return lang[0] && strcmp(lang, "en") && strcmp(lang, "ar"); }

// A JSON string's value after `key` ("extract", "*"), decoded to code points; 0 when not there.
static int json_string_after(const char *json, u32 len, const char *key, std::vector<u32> *out)
{
    std::string pat = std::string("\"") + key + "\":\"";
    const char *p = NULL, *end = json + len;
    size_t i;
    out->clear();
    for (i = 0; i + pat.size() <= len; i++)
        if (!memcmp(json + i, pat.c_str(), pat.size())) { p = json + i + pat.size(); break; }
    if (!p) return 0;
    while (p < end && *p != '"')
    {
        u32 c = (u8)*p++;
        if (c == '\\' && p < end)
        {
            char e = *p++;
            if (e == 'n' || e == 't' || e == 'r') c = ' ';
            else if (e == 'u' && end - p >= 4)
            {
                char hex[5];
                memcpy(hex, p, 4);
                hex[4] = 0;
                c = (u32)strtoul(hex, NULL, 16);
                p += 4;
                if (c >= 0xD800 && c < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u')
                {
                    // A surrogate pair (outside the font anyway): skipped whole.
                    p += 6;
                    c = 0;
                }
            }
            else c = (u8)e;
        }
        else if (c >= 0x80)
        {
            // UTF-8 straight in the JSON.
            int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0, k;
            c &= n == 1 ? 0x1F : n == 2 ? 0x0F : 0x07;
            for (k = 0; k < n && p < end; k++) c = (c << 6) | ((u8)*p++ & 0x3F);
        }
        if (c) out->push_back(c);
    }
    return 1;
}

// Code points to UTF-8 the menu font can draw: ASCII, Latin-1, OE/oe, Cyrillic; typographic
// quotes, dashes and spaces as plain ones; anything else left out.
static std::string font_utf8(const std::vector<u32> &cps)
{
    std::string s;
    size_t i;
    for (i = 0; i < cps.size(); i++)
    {
        u32 c = cps[i];
        if (c == 0x2018 || c == 0x2019 || c == 0x201A) c = '\'';
        else if (c == 0x201C || c == 0x201D || c == 0x201E) c = '"';
        else if (c >= 0x2010 && c <= 0x2015) c = '-';
        else if (c == 0x00A0 || c == 0x2009 || c == 0x200A || c == 0x202F) c = ' ';
        if (c < 0x20) continue;
        if (c < 0x80) s += (char)c;
        else if ((c >= 0xA0 && c <= 0xFF) || c == 0x152 || c == 0x153 || (c >= 0x400 && c <= 0x45F))
        {
            s += (char)(0xC0 | (c >> 6));
            s += (char)(0x80 | (c & 0x3F));
        }
        else if (c == 0x2026) s += "...";
    }
    return s;
}

// The first whole sentences (at most 4) that fit `size` bytes.
static int sentences_fit(const std::string &text, char *out, int size)
{
    size_t cut = 0, pos = 0;
    int n = 0;
    while (n < 4)
    {
        size_t dot = text.find(". ", pos), endPos = dot == std::string::npos ? (text.size() && text[text.size() - 1] == '.' ? text.size() : std::string::npos) : dot + 1;
        if (endPos == std::string::npos || endPos >= (size_t)size) break;
        cut = endPos;
        n++;
        if (dot == std::string::npos) break;
        pos = dot + 2;
    }
    if (!cut) return 0;
    memcpy(out, text.c_str(), cut);
    out[cut] = 0;
    return 1;
}

// 1: the description in `lang` was saved, 0: there is none, -1: the network failed.
static int fetch_desc_lang(const RomId *id, const H64GameInfo *info, const char *lang)
{
    char pages[3][256], file[128];
    int n = h64_gameinfo_wiki_pages(info, pages, 3), i, netFail = 0;
    for (i = 0; i < n; i++)
    {
        char path[600], host[48], title[400], enc[800];
        u8 *data = NULL;
        u32 len = 0;
        int st;
        std::vector<u32> cps;
        // The English article's link to the other language.
        _snprintf(path, sizeof(path) - 1, "/w/api.php?action=query&prop=langlinks&format=json&redirects=1&lllang=%s&titles=%s", lang, pages[i]);
        path[sizeof(path) - 1] = 0;
        st = NetGet(H64_WIKI_HOST, path, 1, &data, &len, ART_MAX_WIKI, NULL, 0);
        if (!st) { netFail = 1; free(data); break; }
        if (st != 200 || !data || !json_string_after((const char *)data, len, "*", &cps) || cps.empty()) { free(data); continue; }
        free(data);
        data = NULL;
        {
            // The title as UTF-8 (all of it, whatever the font), spaces as _, URL-encoded.
            std::string t;
            size_t k;
            for (k = 0; k < cps.size(); k++)
            {
                u32 c = cps[k] == ' ' ? '_' : cps[k];
                if (c < 0x80) t += (char)c;
                else if (c < 0x800) { t += (char)(0xC0 | (c >> 6)); t += (char)(0x80 | (c & 0x3F)); }
                else { t += (char)(0xE0 | (c >> 12)); t += (char)(0x80 | ((c >> 6) & 0x3F)); t += (char)(0x80 | (c & 0x3F)); }
            }
            strncpy(title, t.c_str(), sizeof(title) - 1);
            title[sizeof(title) - 1] = 0;
        }
        h64_url_encode(title, enc, sizeof(enc));
        _snprintf(host, sizeof(host) - 1, "%s.wikipedia.org", lang);
        host[sizeof(host) - 1] = 0;
        _snprintf(path, sizeof(path) - 1, "%s%s", H64_WIKI_PATH, enc);
        path[sizeof(path) - 1] = 0;
        st = NetGet(host, path, 1, &data, &len, ART_MAX_WIKI, NULL, 0);
        if (!st) { netFail = 1; free(data); break; }
        if (st == 200 && data && json_string_after((const char *)data, len, "extract", &cps))
        {
            char text[sizeof(info->description)];
            free(data);
            if (sentences_fit(font_utf8(cps), text, sizeof(text)))
            {
                _snprintf(file, sizeof(file) - 1, "%s\\%s.desc.%s", s_dir, id->key, lang);
                file[sizeof(file) - 1] = 0;
                if (file_write(file, text, (u32)strlen(text)))
                {
                    H64_INFO("[art] %s: description in %s (%s)", id->key, lang, title);
                    return 1;
                }
            }
            return 0;
        }
        free(data);
    }
    return netFail ? -1 : 0;
}

// The description in the menus' language for the selection, when it has none yet. 1 when it got one.
static int download_desc(const std::string &path)
{
    const RomId *id = rom_id(path);
    H64GameInfo info;
    char lang[8], file[128], none[128];
    int r;
    EnterCriticalSection(&s_lock);
    strcpy(lang, s_lang);
    LeaveCriticalSection(&s_lock);
    if (!id->ok || !lang_has_desc(lang) || !load_info(id, &info) || !info.found || info.wiki <= 0) return 0;
    _snprintf(file, sizeof(file) - 1, "%s\\%s.desc.%s", s_dir, id->key, lang);
    file[sizeof(file) - 1] = 0;
    _snprintf(none, sizeof(none) - 1, "%s.none", file);
    none[sizeof(none) - 1] = 0;
    if (file_exists(file) || file_exists(none) || !NetStart(0)) return 0;
    set_busy(path);
    r = fetch_desc_lang(id, &info, lang);
    if (r == 0) file_write(none, "-", 1);   // not in that Wikipedia: English it stays
    return r > 0;
}

// The description in the menus' language in place of the English one, when there is one.
static void desc_for_lang(const char *key, H64GameInfo *info)
{
    char lang[8], file[128];
    u8 *data;
    u32 len;
    EnterCriticalSection(&s_lock);
    strcpy(lang, s_lang);
    LeaveCriticalSection(&s_lock);
    if (!lang_has_desc(lang)) return;
    _snprintf(file, sizeof(file) - 1, "%s\\%s.desc.%s", s_dir, key, lang);
    file[sizeof(file) - 1] = 0;
    if (!file_read(file, &data, &len, sizeof(info->description) - 1)) return;
    memcpy(info->description, data, len);
    info->description[len] = 0;
    free(data);
}

// ---- Results ----
static void result_free(ArtResult *r)
{
    free(r->cover);
    free(r->glow);
    free(r->dxt);
    free(r->outline);
    r->cover = r->glow = r->dxt = r->outline = NULL;
}

// ---- The 3D boxes' and cartridges' cache: <key>.box3d.raw, <key>.cart.raw ----
// Their pictures keep their alpha (not DXT1, not in covers.pak), and ScreenScraper's are large: decoding
// each PNG every time it was shown made the cartridges view slow and the selection's late. Kept scaled
// as they are drawn: a header (the PNG's size, to know it is that one's), the picture, its glow.
#define RAW_MAGIC 0x4C523031u   // "LR01"
static const char *const s_kindRaw[ART_KIND_COUNT] = { "", "", "", "", "box3d.raw", "cart.raw" };

static int raw_load(const char *key, int kind, const char *png, ArtResult *r)
{
    char p[128];
    u8 *data;
    u32 len, size = 0, hdr[4];
    const u32 glowBytes = ART_GLOW_W * ART_GLOW_H * 4;
    if (!s_kindRaw[kind][0] || names_find(png, &size) <= 0) return 0;
    art_path(key, s_kindRaw[kind], p, sizeof(p));
    if (!file_read(p, &data, &len, 4u << 20)) return 0;
    if (len >= sizeof(hdr)) memcpy(hdr, data, sizeof(hdr));
    if (len < sizeof(hdr) || hdr[0] != RAW_MAGIC || hdr[1] != size || !hdr[2] || !hdr[3] || hdr[2] > ART_COVER_W || hdr[3] > ART_COVER_H ||
        len != sizeof(hdr) + hdr[2] * hdr[3] * 4 + glowBytes)
    {
        free(data);
        return 0;
    }
    r->coverW = (int)hdr[2];
    r->coverH = (int)hdr[3];
    r->cover = (u8 *)malloc(hdr[2] * hdr[3] * 4);
    r->glow = (u8 *)malloc(glowBytes);
    if (!r->cover || !r->glow)
    {
        free(r->cover); free(r->glow); free(data);
        r->cover = r->glow = NULL;
        return 0;
    }
    memcpy(r->cover, data + sizeof(hdr), hdr[2] * hdr[3] * 4);
    memcpy(r->glow, data + sizeof(hdr) + hdr[2] * hdr[3] * 4, glowBytes);
    free(data);
    return 1;
}

static void raw_save(const char *key, int kind, const char *png, const ArtResult *r)
{
    char p[128];
    u32 size = 0, hdr[4], n;
    const u32 glowBytes = ART_GLOW_W * ART_GLOW_H * 4;
    u8 *data;
    if (!s_kindRaw[kind][0] || !r->cover || !r->glow || names_find(png, &size) <= 0) return;
    n = (u32)r->coverW * r->coverH * 4;
    data = (u8 *)malloc(sizeof(hdr) + n + glowBytes);
    if (!data) return;
    hdr[0] = RAW_MAGIC; hdr[1] = size; hdr[2] = (u32)r->coverW; hdr[3] = (u32)r->coverH;
    memcpy(data, hdr, sizeof(hdr));
    memcpy(data + sizeof(hdr), r->cover, n);
    memcpy(data + sizeof(hdr) + n, r->glow, glowBytes);
    art_path(key, s_kindRaw[kind], p, sizeof(p));
    file_write(p, data, (u32)sizeof(hdr) + n + glowBytes);
    free(data);
}

// The outline's glow (ArtOutline): each pixel's distance to the picture's shape (alpha 128 and over in
// any of the 2 x 2 it stands for), in thirds of a pixel (3 straight, 4 diagonally: two passes), then lit by it.
u8 *ArtOutline(const u8 *rgba, int w, int h, int *ow, int *oh)
{
    const int P = ART_OUTLINE_PAD / 2, hw = (w + 1) / 2, hh = (h + 1) / 2, W = hw + 2 * P, H = hh + 2 * P, beyond = 3 * (P + 2);
    int x, y;
    unsigned short *d = (unsigned short *)malloc((size_t)W * H * sizeof(unsigned short));
    u8 *out = (u8 *)malloc((size_t)W * H * 4);
    if (!d || !out) { free(d); free(out); return NULL; }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
        {
            const int sx = (x - P) * 2, sy = (y - P) * 2;
            int in = 0, i, j;
            if (x >= P && y >= P && x < P + hw && y < P + hh)
                for (j = 0; j < 2 && !in; j++)
                    for (i = 0; i < 2 && !in; i++)
                        if (sx + i < w && sy + j < h && rgba[((size_t)(sy + j) * w + sx + i) * 4 + 3] >= 128) in = 1;
            d[y * W + x] = (unsigned short)(in ? 0 : beyond);
        }
#define DT(xx, yy, c) { int v_ = d[(yy) * W + (xx)] + (c); if (v_ < m) m = v_; }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
        {
            int m = d[y * W + x];
            if (!m) continue;
            if (x > 0) DT(x - 1, y, 3);
            if (y > 0)
            {
                DT(x, y - 1, 3);
                if (x > 0) DT(x - 1, y - 1, 4);
                if (x < W - 1) DT(x + 1, y - 1, 4);
            }
            d[y * W + x] = (unsigned short)m;
        }
    for (y = H - 1; y >= 0; y--)
        for (x = W - 1; x >= 0; x--)
        {
            int m = d[y * W + x];
            if (!m) continue;
            if (x < W - 1) DT(x + 1, y, 3);
            if (y < H - 1)
            {
                DT(x, y + 1, 3);
                if (x < W - 1) DT(x + 1, y + 1, 4);
                if (x > 0) DT(x - 1, y + 1, 4);
            }
            d[y * W + x] = (unsigned short)m;
        }
#undef DT
    for (y = 0; y < W * H; y++)
    {
        // Lit up to 1 pixel out (the ring: 2 of the picture's), then fading out to nothing at P.
        const float t = d[y] / 3.0f;
        float a;
        if (t <= 1.0f) a = 1.0f;
        else if (t >= P) a = 0.0f;
        else { a = 1.0f - (t - 1.0f) / (P - 1.0f); a = 0.55f * a * a; }
        out[y * 4] = out[y * 4 + 1] = out[y * 4 + 2] = 255;
        out[y * 4 + 3] = (u8)(255.0f * a);
    }
    free(d);
    *ow = W;
    *oh = H;
    return out;
}

// ---- Cover cache: covers.pak ----
// Every game's cover (each kind of picture), already scaled and compressed as
// DXT1 (what the GPU reads: 8 times smaller than RGBA, no conversion), with
// its glow, one after the other in one file. Its index is read once (pak_open)
// and kept: showing a cover is one seek and one read of ~46 KB, no file
// looked up - the per-game .cov files (370 KB of raw RGBA each, several
// look-ups per cover) made a page of 15 covers slow with 350 games. A record
// names the PNG's size: another PNG (a download, a copy from a PC) makes it
// stale; a new record is added and the last one counts. A record with no
// picture marks the cover dropped (a download replaced its PNG).
#define PAK_MAGIC 0x48363450u   // "H64P"
struct PakRec { u32 magic; char key[12]; u32 kind, pngSize, coverW, coverH, dxtW, dxtH, dxtBytes, glowBytes; };
struct PakEntry { u32 offset, pngSize, coverW, coverH, dxtW, dxtH, dxtBytes, glowBytes; };
static CRITICAL_SECTION s_pakLock;
static HANDLE s_pak = INVALID_HANDLE_VALUE;
static u32 s_pakEnd;                                // where the next record goes
static std::map<std::string, PakEntry> s_pakIndex;  // "<key>|<kind>"

static std::string pak_name(const char *key, int kind)
{
    char k[32];
    _snprintf(k, sizeof(k) - 1, "%s|%d", key, kind);
    k[sizeof(k) - 1] = 0;
    return k;
}

static void pak_open(void)
{
    char path[96];
    PakRec h;
    DWORD got;
    u32 pos = 0, n = 0;
    _snprintf(path, sizeof(path) - 1, "%s\\covers.pak", s_dir);
    path[sizeof(path) - 1] = 0;
    s_pak = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_pak == INVALID_HANDLE_VALUE) { H64_WARN("[art] cannot open %s", path); return; }
    for (;;)
    {
        PakEntry e;
        if (SetFilePointer(s_pak, pos, NULL, FILE_BEGIN) != pos || !ReadFile(s_pak, &h, sizeof(h), &got, NULL) || got != sizeof(h) ||
            h.magic != PAK_MAGIC || h.dxtBytes > (1u << 20) || h.glowBytes > 4096)
            break;
        h.key[sizeof(h.key) - 1] = 0;
        e.offset = pos + sizeof(h);
        e.pngSize = h.pngSize; e.coverW = h.coverW; e.coverH = h.coverH; e.dxtW = h.dxtW; e.dxtH = h.dxtH;
        e.dxtBytes = h.dxtBytes; e.glowBytes = h.glowBytes;
        if (e.offset + h.dxtBytes + h.glowBytes > GetFileSize(s_pak, NULL)) break;   // cut short: dropped from here
        if (h.dxtBytes) s_pakIndex[pak_name(h.key, (int)h.kind)] = e;
        else s_pakIndex.erase(pak_name(h.key, (int)h.kind));
        pos = e.offset + h.dxtBytes + h.glowBytes;
        n++;
    }
    s_pakEnd = pos;
    SetFilePointer(s_pak, pos, NULL, FILE_BEGIN);
    SetEndOfFile(s_pak);   // whatever came after the last whole record
    H64_INFO("[art] covers.pak: %u covers (%u records, %u KB)", (unsigned)s_pakIndex.size(), n, pos >> 10);
}

static void pak_append(const PakRec *h, const u8 *dxt, const u8 *glow)
{
    DWORD put;
    int ok;
    if (s_pak == INVALID_HANDLE_VALUE) return;
    SetFilePointer(s_pak, s_pakEnd, NULL, FILE_BEGIN);
    ok = WriteFile(s_pak, h, sizeof(*h), &put, NULL) && put == sizeof(*h) &&
         (!h->dxtBytes || (WriteFile(s_pak, dxt, h->dxtBytes, &put, NULL) && put == h->dxtBytes)) &&
         (!h->glowBytes || (WriteFile(s_pak, glow, h->glowBytes, &put, NULL) && put == h->glowBytes));
    if (!ok) { SetFilePointer(s_pak, s_pakEnd, NULL, FILE_BEGIN); SetEndOfFile(s_pak); return; }
    if (h->dxtBytes)
    {
        PakEntry e;
        e.offset = s_pakEnd + sizeof(*h);
        e.pngSize = h->pngSize; e.coverW = h->coverW; e.coverH = h->coverH; e.dxtW = h->dxtW; e.dxtH = h->dxtH;
        e.dxtBytes = h->dxtBytes; e.glowBytes = h->glowBytes;
        s_pakIndex[pak_name(h->key, (int)h->kind)] = e;
    }
    else
        s_pakIndex.erase(pak_name(h->key, (int)h->kind));
    s_pakEnd += sizeof(*h) + h->dxtBytes + h->glowBytes;
}

static void pak_drop(const char *key, int kind)
{
    PakRec h;
    memset(&h, 0, sizeof(h));
    h.magic = PAK_MAGIC;
    strncpy(h.key, key, sizeof(h.key) - 1);
    h.kind = (u32)kind;
    EnterCriticalSection(&s_pakLock);
    if (s_pakIndex.count(pak_name(key, kind))) pak_append(&h, NULL, NULL);
    LeaveCriticalSection(&s_pakLock);
}

static void pak_save(const char *key, int kind, const char *png, const ArtResult *r)
{
    PakRec h;
    u32 size = 0;
    if (!r->dxt || !r->glow || names_find(png, &size) <= 0) return;
    memset(&h, 0, sizeof(h));
    h.magic = PAK_MAGIC;
    strncpy(h.key, key, sizeof(h.key) - 1);
    h.kind = (u32)kind;
    h.pngSize = size;
    h.coverW = (u32)r->coverW; h.coverH = (u32)r->coverH;
    h.dxtW = (u32)r->dxtW; h.dxtH = (u32)r->dxtH;
    h.dxtBytes = (u32)(r->dxtW / 4) * (r->dxtH / 4) * 8;
    h.glowBytes = ART_GLOW_W * ART_GLOW_H * 4;
    EnterCriticalSection(&s_pakLock);
    pak_append(&h, r->dxt, r->glow);
    LeaveCriticalSection(&s_pakLock);
}

// The packed cover of a game's picture, when its PNG is still the one it was made from.
static int pak_load(const char *key, int kind, const char *png, ArtResult *r)
{
    std::map<std::string, PakEntry>::const_iterator it;
    PakEntry e;
    u32 size = 0;
    DWORD got;
    int ok = 0;
    if (names_find(png, &size) <= 0) return 0;
    EnterCriticalSection(&s_pakLock);
    it = s_pakIndex.find(pak_name(key, kind));
    if (it != s_pakIndex.end() && it->second.pngSize == size && it->second.glowBytes == ART_GLOW_W * ART_GLOW_H * 4)
    {
        e = it->second;
        r->dxt = (u8 *)malloc(e.dxtBytes);
        r->glow = (u8 *)malloc(e.glowBytes);
        if (r->dxt && r->glow && SetFilePointer(s_pak, e.offset, NULL, FILE_BEGIN) == e.offset &&
            ReadFile(s_pak, r->dxt, e.dxtBytes, &got, NULL) && got == e.dxtBytes &&
            ReadFile(s_pak, r->glow, e.glowBytes, &got, NULL) && got == e.glowBytes)
        {
            r->coverW = (int)e.coverW; r->coverH = (int)e.coverH;
            r->dxtW = (int)e.dxtW; r->dxtH = (int)e.dxtH;
            ok = 1;
        }
        else
        {
            free(r->dxt); free(r->glow);
            r->dxt = r->glow = NULL;
        }
    }
    LeaveCriticalSection(&s_pakLock);
    return ok;
}

// ---- DXT1 ----
// 4x4 blocks: two RGB565 end colours and a 2-bit index per pixel into them and
// the two thirds between. Written as the Xbox 360's GPU reads it: the PC
// layout swapped by 16-bit words (GPUENDIAN_8IN16), so the colours are stored
// as native (big-endian) words and the index bytes swapped in pairs. The
// picture is padded to whole blocks (edge pixels repeated).
static u16 rgb565(int r, int g, int b) { return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
static void unpack565(u16 c, int *rgb)
{
    rgb[0] = ((c >> 11) & 31) * 255 / 31; rgb[1] = ((c >> 5) & 63) * 255 / 63; rgb[2] = (c & 31) * 255 / 31;
}

static u8 *dxt1_encode(const u8 *rgba, int w, int h, int *outW, int *outH)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4, bx, by;
    u8 *out = (u8 *)malloc((size_t)bw * bh * 8), *d = out;
    if (!out) return NULL;
    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++, d += 8)
        {
            int px[16][3], lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 }, i, k, pal[4][3];
            u16 c0, c1;
            u32 idx = 0;
            for (i = 0; i < 16; i++)
            {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                const u8 *s = rgba + ((size_t)(y < h ? y : h - 1) * w + (x < w ? x : w - 1)) * 4;
                for (k = 0; k < 3; k++)
                {
                    px[i][k] = s[k];
                    if (s[k] < lo[k]) lo[k] = s[k];
                    if (s[k] > hi[k]) hi[k] = s[k];
                }
            }
            // The box's ends pulled in by 1/16 of its size (a little less error than its corners).
            for (k = 0; k < 3; k++) { int in = (hi[k] - lo[k]) >> 4; lo[k] += in; hi[k] -= in; }
            c0 = rgb565(hi[0], hi[1], hi[2]);
            c1 = rgb565(lo[0], lo[1], lo[2]);
            if (c0 < c1) { u16 t = c0; c0 = c1; c1 = t; }
            if (c0 == c1)
                idx = 0;   // one colour: every index 0
            else
            {
                unpack565(c0, pal[0]);
                unpack565(c1, pal[1]);
                for (k = 0; k < 3; k++) { pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3; pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3; }
                for (i = 0; i < 16; i++)
                {
                    int best = 0, bestD = 1 << 30, j;
                    for (j = 0; j < 4; j++)
                    {
                        int dr = px[i][0] - pal[j][0], dg = px[i][1] - pal[j][1], db = px[i][2] - pal[j][2];
                        int dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                        if (dd < bestD) { bestD = dd; best = j; }
                    }
                    idx |= (u32)best << (2 * i);
                }
            }
            // PC bytes: c0 lo, c0 hi, c1 lo, c1 hi, then the four index rows; swapped in 16-bit pairs.
            d[0] = (u8)(c0 >> 8); d[1] = (u8)c0;
            d[2] = (u8)(c1 >> 8); d[3] = (u8)c1;
            d[4] = (u8)(idx >> 8); d[5] = (u8)idx;
            d[6] = (u8)(idx >> 24); d[7] = (u8)(idx >> 16);
        }
    *outW = bw * 4;
    *outH = bh * 4;
    return out;
}

// Back to RGBA (w x h of the padded picture's top-left), for ArtReadCover.
static u8 *dxt1_decode(const u8 *dxt, int dw, int dh, int w, int h)
{
    int bw = dw / 4, bx, by, x, y;
    u8 *out = (u8 *)malloc((size_t)w * h * 4);
    if (!out) return NULL;
    for (by = 0; by < dh / 4; by++)
        for (bx = 0; bx < bw; bx++)
        {
            const u8 *s = dxt + ((size_t)by * bw + bx) * 8;
            u16 c0 = (u16)(s[0] << 8 | s[1]), c1 = (u16)(s[2] << 8 | s[3]);
            u32 idx = (u32)s[5] | (u32)s[4] << 8 | (u32)s[7] << 16 | (u32)s[6] << 24;
            int pal[4][3], k;
            unpack565(c0, pal[0]);
            unpack565(c1, pal[1]);
            for (k = 0; k < 3; k++)
            {
                pal[2][k] = c0 > c1 ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
                pal[3][k] = c0 > c1 ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
            }
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                {
                    int px = bx * 4 + x, py = by * 4 + y, j = (idx >> (2 * (y * 4 + x))) & 3;
                    u8 *o;
                    if (px >= w || py >= h) continue;
                    o = out + ((size_t)py * w + px) * 4;
                    o[0] = (u8)pal[j][0]; o[1] = (u8)pal[j][1]; o[2] = (u8)pal[j][2]; o[3] = 255;
                }
        }
    return out;
}

// Reads a ROM's files and queues them for the UI.
static void deliver(const std::string &path)
{
    const DWORD startedAt = GetTickCount();
    const RomId *id = rom_id(path);
    ArtResult r;
    memset(&r, 0, sizeof(r));
    strncpy(r.path, path.c_str(), sizeof(r.path) - 1);
    if (id->ok)
    {
        char png[128];
        u8 *data;
        u32 len;
        int kind = (int)s_kind, k;
        std::map<std::string, int>::const_iterator it;
        EnterCriticalSection(&s_lock);
        it = s_kindFor.find(path);
        if (it != s_kindFor.end()) kind = it->second;
        LeaveCriticalSection(&s_lock);
        strcpy(r.key, id->key);
        r.kind = kind;
        for (k = 0; k < ART_KIND_BOX3D; k++)   // (the 3D box and the cartridge are not ones the right stick goes through)
        {
            char p[128];
            art_path(id->key, s_kindPng[k], p, sizeof(p));
            if (file_exists(p)) r.kinds |= 1 << k;
        }
        r.hasInfo = load_info(id, &r.info);
        if (r.hasInfo) desc_for_lang(id->key, &r.info);   // its description in the menus' language, when there is one
        art_path(id->key, s_kindPng[kind], png, sizeof(png));
        if (kind != ART_KIND_BOX && !file_exists(png))
        {
            // That picture is not there (yet): the box meanwhile.
            kind = ART_KIND_BOX;
            art_path(id->key, "png", png, sizeof(png));
        }
        r.shownKind = kind;
        // The packed copy when there is one (one read, ready for the GPU; the 3D box's and the
        // cartridge's: their own, raw_load), else the PNG: decoded, scaled, compressed and packed
        // for next time.
        if (ART_KIND_VIEW_ONLY(kind) && raw_load(id->key, kind, png, &r))
            ;
        else if (!pak_load(id->key, kind, png, &r) && file_read(png, &data, &len, ART_MAX_PNG))
        {
            int w, h;
            u8 *px = h64_png_decode(data, len, &w, &h, ART_MAX_PIXELS);
            free(data);
            if (px)
            {
                u8 *rgba;
                h64_image_fit(w, h, ART_COVER_W, ART_COVER_H, &r.coverW, &r.coverH);
                rgba = h64_image_scale(px, w, h, r.coverW, r.coverH);
                r.glow = h64_image_scale(px, w, h, ART_GLOW_W, ART_GLOW_H);
                free(px);
                if (rgba && ART_KIND_VIEW_ONLY(kind))
                {
                    r.cover = rgba;   // its alpha kept: not compressed, kept raw
                    raw_save(id->key, kind, png, &r);
                }
                else if (rgba)
                {
                    r.dxt = dxt1_encode(rgba, r.coverW, r.coverH, &r.dxtW, &r.dxtH);
                    if (r.dxt)
                    {
                        char old[128];
                        pak_save(id->key, kind, png, &r);
                        free(rgba);
                        art_path(id->key, s_kindCov[kind], old, sizeof(old));   // the per-game copy of before (370 KB): packed now
                        if (file_exists(old)) file_delete(old);
                    }
                    else r.cover = rgba;
                }
            }
            else
                H64_INFO("[art] %s: cannot decode %s", id->key, png);
        }
        // The cartridge's selection glow, made here (the menu's thread drew its shape 88 times a frame).
        if (r.cover && kind == ART_KIND_CART) r.outline = ArtOutline(r.cover, r.coverW, r.coverH, &r.outlineW, &r.outlineH);
        if (GetTickCount() - startedAt > 150)   // what makes a picture late (the cartridges view was)
            H64_INFO("[art] %s: %s read in %u ms", id->key, s_kindPng[r.shownKind], (unsigned)(GetTickCount() - startedAt));
    }
    else
        h64_gameinfo_clear(&r.info);
    EnterCriticalSection(&s_lock);
    while (s_results.size() >= ART_MAX_RESULTS)
    {
        s_delivered.erase(s_results.front().path);   // never reached the browser: sent again when wanted
        result_free(&s_results.front());
        s_results.pop_front();
    }
    s_results.push_back(r);
    s_delivered.insert(path);
    LeaveCriticalSection(&s_lock);
}

// Only a ROM's information to the browser (deliver without its pictures): the
// critics' score that came after its pictures were shown.
static void deliver_info(const std::string &path)
{
    const RomId *id = rom_id(path);
    ArtResult r;
    if (!id->ok) return;
    memset(&r, 0, sizeof(r));
    strncpy(r.path, path.c_str(), sizeof(r.path) - 1);
    strcpy(r.key, id->key);
    r.infoOnly = 1;
    r.hasInfo = load_info(id, &r.info);
    if (r.hasInfo) desc_for_lang(id->key, &r.info);
    EnterCriticalSection(&s_lock);
    if (s_results.size() < ART_MAX_RESULTS) s_results.push_back(r);
    LeaveCriticalSection(&s_lock);
}

// ---- Worker ----
static void set_busy(const std::string &path)
{
    EnterCriticalSection(&s_lock);
    strncpy(s_status.busyPath, path.c_str(), sizeof(s_status.busyPath) - 1);
    s_status.busyPath[sizeof(s_status.busyPath) - 1] = 0;
    LeaveCriticalSection(&s_lock);
}

static void note(const char *text)
{
    EnterCriticalSection(&s_lock);
    s_note = text;
    LeaveCriticalSection(&s_lock);
}

// One step of the wanted list; returns 1 when it did something.
static int step_wants(void)
{
    std::vector<std::string> wants;
    u32 gen;
    size_t i;
    if (s_langSeen != s_langGen)
    {
        // Another language: the selections' descriptions may be looked for again.
        s_triedAll.clear();
        s_langSeen = s_langGen;
    }
    if (s_triedGen != s_kindGen)
    {
        // Another kind of picture: every selection may be looked for again.
        s_tried.clear();
        s_triedGen = s_kindGen;
    }
    EnterCriticalSection(&s_lock);
    wants = s_wants;
    gen = s_wantGen;
    LeaveCriticalSection(&s_lock);
    for (i = 0; i < wants.size(); i++)
    {
        int sent, tried;
        EnterCriticalSection(&s_lock);
        sent = s_delivered.count(wants[i]) != 0;
        LeaveCriticalSection(&s_lock);
        tried = s_tried.count(wants[i]) != 0;
        if (!sent)
        {
            deliver(wants[i]);   // first what is there (fast), the downloads next
            return 1;
        }
        if (i == 0 && !tried && s_netAllowed)
        {
            int changed;
            s_tried.insert(wants[0]);
            changed = download_missing(wants[0], 0, ArtKindFor(wants[0].c_str()));
            set_busy("");
            if (changed == 2) deliver_info(wants[0]);
            else if (changed)
            {
                EnterCriticalSection(&s_lock);
                if (s_wantGen == gen || std::find(s_wants.begin(), s_wants.end(), wants[0]) != s_wants.end()) s_delivered.erase(wants[0]);
                LeaveCriticalSection(&s_lock);
            }
            return 1;
        }
        if (i == 0 && tried && s_netAllowed && !s_triedAll.count(wants[0]))
        {
            // The selection's other pictures too (a picture from the game, the title screen, the logo,
            // the box): the browser's right stick goes through what there is of it.
            int k, changed = 0;
            s_triedAll.insert(wants[0]);
            for (k = 0; k < ART_KIND_COUNT; k++) changed |= download_missing(wants[0], 0, k);
            changed |= download_desc(wants[0]);
            set_busy("");
            if (changed == 2) deliver_info(wants[0]);
            else if (changed)
            {
                EnterCriticalSection(&s_lock);
                s_delivered.erase(wants[0]);
                LeaveCriticalSection(&s_lock);
            }
            return 1;
        }
        if (i > 0 && !tried && s_netAllowed)
        {
            // The other games on screen: the picture they show (after the selection's,
            // which comes first) - they used to wait until each was selected.
            int changed;
            s_tried.insert(wants[i]);
            changed = download_missing(wants[i], 0, ArtKindFor(wants[i].c_str()));
            set_busy("");
            if (changed == 2) deliver_info(wants[i]);
            else if (changed)
            {
                EnterCriticalSection(&s_lock);
                if (std::find(s_wants.begin(), s_wants.end(), wants[i]) != s_wants.end()) s_delivered.erase(wants[i]);
                LeaveCriticalSection(&s_lock);
            }
            return 1;
        }
    }
    return 0;
}

// The detailed view's downloads (ArtFetchKind): one kind of one game's pictures each, before anything else.
static std::vector<std::pair<std::string, int> > s_fetch;   // under s_lock
static std::string s_fetchNow;                              // the ROM of the one going on (under s_lock)
static int s_fetchNowKind = -1;                             // ... and its kind
static volatile LONG s_fetchDone;                           // how many have finished (ArtFetchCount)

static int step_fetch(void)
{
    std::pair<std::string, int> f;
    EnterCriticalSection(&s_lock);
    if (s_fetch.empty())
    {
        LeaveCriticalSection(&s_lock);
        return 0;
    }
    f = s_fetch[0];
    s_fetch.erase(s_fetch.begin());
    s_fetchNow = f.first;
    s_fetchNowKind = f.second;
    LeaveCriticalSection(&s_lock);
    s_force = 1;
    download_missing(f.first, 1, f.second);
    s_force = 0;
    set_busy("");
    EnterCriticalSection(&s_lock);
    s_fetchNow.clear();
    s_fetchNowKind = -1;
    s_delivered.erase(f.first);   // the list's copy sent again
    LeaveCriticalSection(&s_lock);
    InterlockedIncrement(&s_fetchDone);
    return 1;
}

static int step_bulk(void)
{
    std::string path;
    size_t pos, total;
    int retry;
    EnterCriticalSection(&s_lock);
    if (!s_status.bulk || s_bulkPos >= s_bulk.size())
    {
        LeaveCriticalSection(&s_lock);
        return 0;
    }
    pos = s_bulkPos;
    total = s_bulk.size();
    path = s_bulk[pos];
    retry = s_bulkRetry;
    LeaveCriticalSection(&s_lock);

    if (pos == 0 && retry)
    {
        // Fresh databases first; no network: stop here.
        if (!NetStart(1))
        {
            char text[96];
            EnterCriticalSection(&s_lock);
            s_status.bulk = 0;
            s_status.offline = 1;
            s_bulk.clear();
            LeaveCriticalSection(&s_lock);
            strcpy(text, "No network connection: nothing was downloaded");
            note(text);
            return 1;
        }
        ensure_db(1, 1);
    }
    // (Without `retry`, a ROM whose files are complete or known missing needs no network at all.)
    // The description in the menus' language too (it was only fetched for a
    // game selected online, after its pictures: most had none).
    // Every kind of picture at once when the screenshots and logos are wanted too (they used to come
    // only for a game shown in the list); the box art (or the kind shown) only otherwise.
    int got = download_missing(path, retry, (int)s_kind);
    if (s_extrasAllowed)
    {
        int k;
        for (k = 0; k < ART_KIND_COUNT; k++)
            if (k != (int)s_kind) got |= download_missing(path, retry, k);
    }
    else if (ART_KIND_VIEW_ONLY(s_kind))
        got |= download_missing(path, retry, ART_KIND_BOX);   // their views draw the box (art) where there is none of theirs
    if (got | download_desc(path))
    {
        EnterCriticalSection(&s_lock);
        s_delivered.erase(path);   // shown again if the browser still wants it
        s_bulkChanged++;
        LeaveCriticalSection(&s_lock);
    }
    set_busy("");
    {
        const RomId *id = rom_id(path);
        H64GameInfo info;
        char png[128];
        int found = 0, cover = 0;
        if (id->ok && load_info(id, &info)) found = info.found;
        if (id->ok)
        {
            art_path(id->key, "png", png, sizeof(png));
            cover = file_exists(png);
        }
        {
            // Nothing left to fetch without "download all": the pictures are there, or
            // known not to be (not in the databases, or looked for and missing).
            // With the screenshots and logos: all four kinds ("a|"), else the one shown.
            int kind = (int)s_kind, all = s_extrasAllowed != 0, settled = !id->ok;
            if (id->ok && load_info(id, &info))
            {
                int k;
                settled = 1;
                for (k = 0; k < ART_KIND_COUNT; k++)
                {
                    char p[128];
                    if (ART_KIND_PICKER_ONLY(k)) continue;   // never fetched by "download all"
                    if (!all && k != kind && !(ART_KIND_VIEW_ONLY(kind) && k == ART_KIND_BOX)) continue;
                    if (ART_KIND_VIEW_ONLY(k) && k != kind) continue;   // only asked for by its view
                    art_path(id->key, s_kindPng[k], p, sizeof(p));
                    if (file_exists(p)) continue;
                    // Each picture settled once there, or once marked missing (libretro's and ScreenScraper's asked).
                    if (k == ART_KIND_BOX)
                    {
                        // (A known game without libretro's box: until ScreenScraper was asked too.)
                        art_path(id->key, "ssbox.none", p, sizeof(p));
                        if (info.cover != 0 || (info.found && !ss_marker(p))) settled = 0;
                        continue;
                    }
                    art_path(id->key, s_kindNone[k], p, sizeof(p));
                    if (!(ART_KIND_VIEW_ONLY(k) ? ss_marker(p) : file_exists(p))) settled = 0;
                }
            }
            if (settled)
            {
                char line[16];
                std::string k;
                if (all && ART_KIND_VIEW_ONLY(kind)) sprintf(line, "a%d|", kind);   // (a view of its own kind: those pictures too)
                else if (all) strcpy(line, "a|");
                else sprintf(line, "%d|", kind);
                k = line + path;
                int fresh;
                EnterCriticalSection(&s_lock);   // ArtBulkStart reads it on the UI thread
                fresh = s_complete.insert(k).second;
                LeaveCriticalSection(&s_lock);
                if (fresh)
                {
                    FILE *f = fopen(s_completePath, "a");
                    if (f) { fprintf(f, "%s\n", k.c_str()); fclose(f); }
                }
            }
        }
        EnterCriticalSection(&s_lock);
        s_status.done = (int)pos + 1;
        s_status.found += found;
        s_status.covers += cover;
        s_bulkPos = pos + 1;
        if (s_bulkPos >= total)
        {
            char text[256];
            s_status.bulk = 0;
            s_bulk.clear();
            _snprintf(text, sizeof(text) - 1, Tr(s_status.offline ? T_ART_DONE_LOST : T_ART_DONE), s_status.covers, (int)total);
            text[sizeof(text) - 1] = 0;
            if (retry || s_bulkChanged) s_note = text;
            H64_INFO("[art] download all: %d games, %d identified, %d with box art", (int)total, s_status.found, s_status.covers);
        }
        LeaveCriticalSection(&s_lock);
    }
    return 1;
}

// The critics' scores file (score_for): read from the drive first, then downloaded when it is
// missing or a week old (once a session, when downloads are allowed). Every game shown so far
// gets its score at once, its pictures untouched.
static void scores_redeliver(void)
{
    std::set<std::string> shown;
    std::set<std::string>::const_iterator it;
    EnterCriticalSection(&s_lock);
    shown = s_delivered;
    LeaveCriticalSection(&s_lock);
    for (it = shown.begin(); it != shown.end(); ++it) deliver_info(*it);
}

static void step_scores(void)
{
    static int readFile;
    char path[128];
    u8 *data = NULL;
    u32 len = 0;
    int n;
    if (s_scoresTried) return;
    _snprintf(path, sizeof(path) - 1, "%s\\db\\ratings.txt", s_dir);
    path[sizeof(path) - 1] = 0;
    if (!readFile)
    {
        readFile = 1;
        if (file_read(path, &data, &len, RATINGS_MAX) && (n = scores_parse((const char *)data, len)) > 0)
        {
            H64_INFO("[art] critics' scores: %d games", n);
            scores_redeliver();
        }
        free(data);
        data = NULL;
    }
    if (s_scoresReady && file_age_days(path) < 7) { s_scoresTried = 1; return; }
    if (!s_netAllowed) return;   // tried again once downloads are allowed
    s_scoresTried = 1;
    if (!NetStart(0)) return;
    if (NetGet(RATINGS_HOST, RATINGS_PATH, 1, &data, &len, RATINGS_MAX, NULL, 0) == 200 && data &&
        (n = scores_parse((const char *)data, len)) > 0)
    {
        if (!file_write(path, data, len)) H64_WARN("[art] cannot write %s", path);
        H64_INFO("[art] critics' scores downloaded: %d games", n);
        scores_redeliver();
    }
    free(data);
}

static DWORD WINAPI art_thread(LPVOID)
{
    // First the folder's names and the covers' index (until then: looked up on
    // the drive, covers decoded from their PNGs).
    names_scan();
    EnterCriticalSection(&s_pakLock);
    pak_open();
    LeaveCriticalSection(&s_pakLock);
    for (;;)
    {
        WaitForSingleObject(s_event, 100);
        step_scores();
        while (s_paused != 1 && (step_fetch() || (!s_paused && step_wants()) || step_bulk())) ;   // 2: the downloads only
    }
}

// ---- API ----
void ArtStart(const char *drive)
{
    char dir[96];
    if (InterlockedExchange(&s_started, 1)) return;
    InitializeCriticalSection(&s_lock);
    InitializeCriticalSection(&s_namesLock);
    InitializeCriticalSection(&s_pakLock);
    s_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    _snprintf(dir, sizeof(dir) - 1, "%s:\\art", drive);
    dir[sizeof(dir) - 1] = 0;
    CreateDirectoryA(dir, NULL);
    _snprintf(s_dir, sizeof(s_dir) - 1, "%s:\\art\\games", drive);
    CreateDirectoryA(s_dir, NULL);
    _snprintf(dir, sizeof(dir) - 1, "%s\\db", s_dir);
    CreateDirectoryA(dir, NULL);
    // A big stack for the TLS handshakes; hardware thread 3 (as the update
    // check), away from the CPU (0), graphics (2, 5) and audio (4) threads.
    s_thread = CreateThread(NULL, 256 * 1024, art_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!s_thread) return;
    XSetThreadProcessor(s_thread, 3);
    SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(s_thread);
    H64_INFO("[art] folder %s", s_dir);
    {
        // The ROMs settled before (see s_complete).
        char line[300];
        FILE *f;
        _snprintf(s_completePath, sizeof(s_completePath) - 1, "%s\\complete.txt", s_dir);
        s_completePath[sizeof(s_completePath) - 1] = 0;
        f = fopen(s_completePath, "r");
        if (f)
        {
            while (fgets(line, sizeof(line), f))
            {
                size_t n = strlen(line);
                while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
                if (n) s_complete.insert(line);
            }
            fclose(f);
        }
    }
}

void ArtSetNetwork(int allowed) { InterlockedExchange(&s_netAllowed, allowed ? 1 : 0); }
void ArtSetExtras(int allowed) { InterlockedExchange(&s_extrasAllowed, allowed ? 1 : 0); }

void ArtPause(int paused)
{
    InterlockedExchange(&s_paused, paused == 2 ? 2 : paused ? 1 : 0);
    if (paused != 1 && s_event) SetEvent(s_event);
}

void ArtWant(const char *const *paths, int count)
{
    std::vector<std::string> w;
    int i;
    if (!s_started) return;
    for (i = 0; i < count && i < ART_MAX_WANTS; i++) w.push_back(paths[i]);
    EnterCriticalSection(&s_lock);
    if (w != s_wants)
    {
        s_wants = w;
        s_wantGen++;
        SetEvent(s_event);
    }
    LeaveCriticalSection(&s_lock);
}

int ArtTake(ArtResult *r)
{
    int got = 0;
    if (!s_started) return 0;
    EnterCriticalSection(&s_lock);
    if (!s_results.empty())
    {
        *r = s_results.front();
        s_results.pop_front();
        got = 1;
    }
    LeaveCriticalSection(&s_lock);
    return got;
}

void ArtResultFree(ArtResult *r) { result_free(r); }

void ArtForget(const char *path)
{
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    s_delivered.erase(path);
    SetEvent(s_event);
    LeaveCriticalSection(&s_lock);
}

void ArtSetKind(int kind)
{
    if (kind < 0 || kind >= ART_KIND_COUNT) kind = ART_KIND_BOX;
    if (InterlockedExchange(&s_kind, kind) == kind) return;
    InterlockedIncrement(&s_kindGen);
    ArtForgetAll();   // what was sent is sent again in the new kind
    if (s_event) SetEvent(s_event);
}

int ArtGetKind(void) { return (int)s_kind; }

void ArtSetLang(const char *code)
{
    int changed;
    if (!s_started || !code) return;
    EnterCriticalSection(&s_lock);
    changed = strcmp(s_lang, code) != 0;
    if (changed)
    {
        strncpy(s_lang, code, sizeof(s_lang) - 1);
        s_lang[sizeof(s_lang) - 1] = 0;
    }
    LeaveCriticalSection(&s_lock);
    if (!changed) return;
    InterlockedIncrement(&s_langGen);
    ArtForgetAll();   // sent again with the descriptions in it
    if (s_event) SetEvent(s_event);
}

void ArtShowKind(const char *romPath, int kind)
{
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    if (kind < 0 || kind >= ART_KIND_COUNT) s_kindFor.erase(romPath);
    else s_kindFor[romPath] = kind;
    s_delivered.erase(romPath);   // sent again in it
    LeaveCriticalSection(&s_lock);
    if (s_event) SetEvent(s_event);
}

int ArtKindFor(const char *romPath)
{
    int kind = (int)s_kind;
    std::map<std::string, int>::const_iterator it;
    if (!s_started) return kind;
    EnterCriticalSection(&s_lock);
    it = s_kindFor.find(romPath);
    if (it != s_kindFor.end()) kind = it->second;
    LeaveCriticalSection(&s_lock);
    return kind;
}

int ArtReadCover(const char *romPath, int kind, u8 **rgba, int *w, int *h, int *gotKind)
{
    H64Rom rom;
    char key[16];
    *rgba = NULL;
    // Its own header read and file reads, like ArtReadInfo: nothing of the worker's is touched.
    if (!s_started || !RomReadHeader(romPath, &rom)) return 0;
    art_key(&rom, key);
    h64_rom_free(&rom);
    return ArtReadCoverKey(key, kind, rgba, w, h, gotKind);
}

void ArtFetchKind(const char *romPath, int kind)
{
    if (!s_started || kind < 0 || kind >= ART_KIND_COUNT) return;
    EnterCriticalSection(&s_lock);
    s_fetch.push_back(std::make_pair(std::string(romPath), kind));
    LeaveCriticalSection(&s_lock);
    if (s_event) SetEvent(s_event);
}

int ArtFetchBusy(const char *romPath)
{
    size_t i;
    int busy = 0;
    if (!s_started) return 0;
    EnterCriticalSection(&s_lock);
    busy = !_stricmp(s_fetchNow.c_str(), romPath);
    for (i = 0; i < s_fetch.size() && !busy; i++) busy = !_stricmp(s_fetch[i].first.c_str(), romPath);
    LeaveCriticalSection(&s_lock);
    return busy;
}

int ArtFetchBusyKind(const char *romPath, int kind)
{
    size_t i;
    int busy;
    if (!s_started) return 0;
    EnterCriticalSection(&s_lock);
    busy = s_fetchNowKind == kind && !_stricmp(s_fetchNow.c_str(), romPath);
    for (i = 0; i < s_fetch.size() && !busy; i++) busy = s_fetch[i].second == kind && !_stricmp(s_fetch[i].first.c_str(), romPath);
    LeaveCriticalSection(&s_lock);
    return busy;
}

int ArtFetchCount(void) { return (int)s_fetchDone; }

void ArtDropPicture(const char *romPath, int kind)
{
    H64Rom rom;
    char key[16], file[128];
    if (!s_started || !RomReadHeader(romPath, &rom)) return;
    art_key(&rom, key);
    h64_rom_free(&rom);
    if (kind < 0 || kind >= ART_KIND_COUNT) kind = ART_KIND_BOX;
    art_path(key, s_kindPng[kind], file, sizeof(file));
    file_delete(file);   // the names known too: the download looks for it again, covers.pak's record is stale
    art_path(key, s_kindCov[kind], file, sizeof(file));
    file_delete(file);
    H64_INFO("[art] %s: picture %d dropped", key, kind);
}

int ArtReadCoverKey(const char *key, int kind, u8 **rgba, int *w, int *h, int *gotKind)
{
    char png[128];
    ArtResult r;
    u8 *data;
    u32 len;
    *rgba = NULL;
    if (!s_started || !key[0]) return 0;
    if (kind < 0 || kind >= ART_KIND_COUNT) kind = ART_KIND_BOX;
    art_path(key, s_kindPng[kind], png, sizeof(png));
    if (kind != ART_KIND_BOX && !file_exists(png))
    {
        kind = ART_KIND_BOX;
        art_path(key, "png", png, sizeof(png));
    }
    if (gotKind) *gotKind = kind;
    memset(&r, 0, sizeof(r));
    if (pak_load(key, kind, png, &r))
    {
        *rgba = dxt1_decode(r.dxt, r.dxtW, r.dxtH, r.coverW, r.coverH);
        *w = r.coverW;
        *h = r.coverH;
        result_free(&r);
        if (*rgba) return 1;
    }
    if (!file_read(png, &data, &len, ART_MAX_PNG)) return 0;
    {
        int pw, ph;
        u8 *px = h64_png_decode(data, len, &pw, &ph, ART_MAX_PIXELS);
        free(data);
        if (!px) return 0;
        h64_image_fit(pw, ph, ART_COVER_W, ART_COVER_H, w, h);
        *rgba = h64_image_scale(px, pw, ph, *w, *h);
        free(px);
        return *rgba != NULL;
    }
}

int ArtKeyOf(const char *romPath, char *key)
{
    H64Rom rom;
    if (!s_started || !RomReadHeader(romPath, &rom)) return 0;
    art_key(&rom, key);
    h64_rom_free(&rom);
    return 1;
}

int ArtKeyFromCode(const char *name, const char *code, char *key)
{
    H64Rom rom;
    unsigned crc = 0;
    char gc[8] = "";
    if (sscanf(code, "%7s / %X", gc, &crc) != 2) return 0;
    memset(&rom, 0, sizeof(rom));
    strncpy(rom.name, name, sizeof(rom.name) - 1);
    strncpy(rom.gameCode, gc, sizeof(rom.gameCode) - 1);
    rom.crc1 = (u32)crc;
    art_key(&rom, key);
    return 1;
}

const char *ArtFolder(void) { return s_dir; }

int ArtReadPicture(const char *romPath, int kind, int maxW, int maxH, u8 **rgba, int *w, int *h)
{
    H64Rom rom;
    char key[16], png[128];
    u8 *data, *px;
    u32 len;
    int pw, ph;
    *rgba = NULL;
    // As ArtReadCover, but the PNG itself (its alpha kept: a logo) and up to maxW x maxH.
    if (!s_started || kind < 0 || kind >= ART_KIND_COUNT || !RomReadHeader(romPath, &rom)) return 0;
    art_key(&rom, key);
    h64_rom_free(&rom);
    art_path(key, s_kindPng[kind], png, sizeof(png));
    if (!file_read(png, &data, &len, ART_MAX_PNG)) return 0;
    px = h64_png_decode(data, len, &pw, &ph, ART_MAX_PIXELS);
    free(data);
    if (!px) return 0;
    if (pw <= maxW && ph <= maxH)
    {
        *rgba = px;
        *w = pw;
        *h = ph;
        return 1;
    }
    h64_image_fit(pw, ph, maxW, maxH, w, h);
    *rgba = h64_image_scale(px, pw, ph, *w, *h);
    free(px);
    return *rgba != NULL;
}

void ArtForgetAll(void)
{
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    s_delivered.clear();
    while (!s_results.empty())
    {
        result_free(&s_results.front());
        s_results.pop_front();
    }
    LeaveCriticalSection(&s_lock);
}

void ArtBulkStart(const char *const *paths, int count, int retry)
{
    int i;
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    if (!s_status.bulk && count > 0)
    {
        char pre[16];
        if (s_extrasAllowed && ART_KIND_VIEW_ONLY(s_kind)) sprintf(pre, "a%d|", (int)s_kind);   // every kind of picture settled (step_bulk)
        else if (s_extrasAllowed) strcpy(pre, "a|");
        else sprintf(pre, "%d|", (int)s_kind);
        s_bulk.clear();
        // The automatic pass leaves out what is settled (s_complete); "download all" takes everything.
        for (i = 0; i < count; i++)
            if (retry || !s_complete.count(std::string(pre) + paths[i])) s_bulk.push_back(paths[i]);
        count = (int)s_bulk.size();
        if (!count)
        {
            LeaveCriticalSection(&s_lock);
            return;
        }
        s_bulkPos = 0;
        s_bulkRetry = retry;
        s_bulkChanged = 0;
        s_status.bulk = 1;
        s_status.done = s_status.found = s_status.covers = 0;
        s_status.total = count;
        SetEvent(s_event);
    }
    LeaveCriticalSection(&s_lock);
}

int ArtReadInfo(const char *romPath, H64GameInfo *info)
{
    H64Rom rom;
    char key[16], path[128];
    u8 *data;
    u32 len;
    int ok;
    h64_gameinfo_clear(info);
    // Its own header read and file read: nothing of the worker's is touched.
    if (!s_started || !RomReadHeader(romPath, &rom)) return 0;
    art_key(&rom, key);
    h64_rom_free(&rom);
    ok = info_read(key, info) && info->found;
    if (ok) desc_for_lang(key, info);   // the description in the menus' language, when there is one
    return ok;
}

void ArtGetStatus(ArtStatus *s)
{
    if (!s_started) { memset(s, 0, sizeof(*s)); return; }
    EnterCriticalSection(&s_lock);
    *s = s_status;
    LeaveCriticalSection(&s_lock);
}

int ArtTakeNote(char *text, int size)
{
    int got = 0;
    if (!s_started || size <= 0) return 0;
    EnterCriticalSection(&s_lock);
    if (!s_note.empty())
    {
        strncpy(text, s_note.c_str(), size - 1);
        text[size - 1] = 0;
        s_note.clear();
        got = 1;
    }
    LeaveCriticalSection(&s_lock);
    return got;
}
