// LAIKA 64 - HD texture packs downloaded from the internet: the community's
// packs for GLideN64 (GhostlyDark's "Reloaded" series and the packs published
// with it), each the latest release of its GitHub repository.
//
// A thread asks the GitHub API for the release, downloads its .hts pack (a .7z,
// up to a few hundred MB) into the game's hires_texture folder, then unpacks
// the .hts out of it a piece at a time (LZMA SDK, third_party/lzma) and drops
// the .7z. What is downloaded is used as data (texture pack files), never run.
#ifndef XB_HIRESPACK_H
#define XB_HIRESPACK_H

#include <stddef.h>

struct HiresPackEntry
{
    const char *romName;   // the ROM header's name, as HiresFolderName gives it
    const char *repo;      // github.com/<owner>/<repo>
    const char *owner;
    const char *title;     // the pack's name, as its authors call it
    const char *author;    // shown with it (credit)
    const char *asset;     // its release file's name ends so (a .zip of PNGs); NULL: the .hts pack
    unsigned size;         // a direct download's bytes (owner "": repo is then its https URL)
    const char *image;     // a picture of it (https, JPEG/PNG; NULL: none), on the Packs screen
};

// The ROM header's name as a folder name: characters a FAT32 name cannot hold
// (: * ? " < > | /) become '-', as GLideN64 names its packs ("Turok 3: Shadow
// of O" -> "Turok 3- Shadow of O").
void HiresFolderName(const char *romName, char *out, size_t size);

// The packs there are for a ROM (its header's name): how many, and each.
int HiresPackCountFor(const char *romName);
const HiresPackEntry *HiresPackGet(const char *romName, int i);

// Downloads and unpacks `e` into `destDir` (the game's folder, made if it is
// not there) in a thread; one at a time.
void HiresPackStart(const HiresPackEntry *e, const char *destDir);
// Stops it (it ends as HPACK_FAILED, HPERR_CANCELLED).
void HiresPackCancel(void);

// "Download all" (Settings > HD textures): the catalog packs `packs` (indices), one after
// another, each into <texturesDir>\<its game>. A pack got this way does not replace the
// game's pack in use. HiresQueueTick starts the next one when none is at work (1 when it
// did): called by its screen, the ROM list and the game loop, so it goes on everywhere.
// HiresQueueStatus: 1 while it runs; the packs started, in all, installed, failed.
void HiresQueueSet(const int *packs, int n, const char *texturesDir);
int HiresQueueTick(void);
int HiresQueueStatus(int *pos, int *total, int *ok, int *failed);
void HiresQueueCancel(void);

enum { HPACK_IDLE = 0, HPACK_LOOKUP, HPACK_DOWNLOAD, HPACK_UNPACK, HPACK_DONE, HPACK_FAILED, HPACK_CONVERT };
enum { HPERR_NONE = 0, HPERR_NETWORK, HPERR_NOPACK, HPERR_SPACE, HPERR_TOOBIG, HPERR_ARCHIVE, HPERR_FILE, HPERR_CANCELLED, HPERR_NOPNG };
// The state (HPACK_*); while it works the KB done and in all (`total` 0 while
// unknown); when it failed the reason (HPERR_*) in *error; when done the .hts
// file name in `file`.
int HiresPackStatus(unsigned *doneKb, unsigned *totalKb, int *error, char *file, size_t fileSize);
// The pack of the last start (NULL before any), and whether it is still at work. It goes
// on while a game runs (main.cpp shows a bar, and loads it when it is that game's).
const HiresPackEntry *HiresPackJob(void);
int HiresPackBusy(void);

// PNG packs (GLideN64's "<ROM>#<CRC>#<fmt>#<size>[#<palette CRC>]_all.png" files): whether
// `dir` (a game's hires_texture folder) holds any, and their conversion into one DXT
// .hts there, <ROM>_PNG.hts (in the background, as a download: HPACK_CONVERT).
int HiresPngPresent(const char *dir);

// The whole catalog (the Packs screen), and what GitHub says of a pack's latest release:
// HiresPackInfoGet copies what is known into *out (NULL: nothing); ask 1: asks for it when
// not known yet (in a thread; HPINFO_LOADING meanwhile), 2: a failed one again too.
enum { HPINFO_NONE = 0, HPINFO_LOADING, HPINFO_OK, HPINFO_FAILED };
struct HiresPackInfo
{
    int state;
    char version[32], date[16], file[96];
    unsigned size, downloads;   // bytes; times downloaded
    char notes[1024];           // the release's notes, plain text
};
int HiresPackTotal(void);
const char *HiresGameName(const char *romName);   // a catalog game's usual name ("Banjo-Kazooie", "Star Fox 64")
const HiresPackEntry *HiresPackAt(int i);
int HiresPackInfoGet(int i, HiresPackInfo *out, int ask);

// A pack's picture (catalog index i): HPINFO_OK with its RGBA pixels (at most 480 x 270),
// asked for (a thread) the first time; HiresPackImageDone once made a texture (the pixels
// go), HiresPackImageForget when the screen closes (its textures gone: loaded again).
int HiresPackImage(int i, const unsigned char **rgba, int *w, int *h);
void HiresPackImageDone(int i);
void HiresPackImageForget(void);

// Installed packs: the name a pack's .hts gets in its game's folder ("<title> - <author>.hts"),
// and that folder's active.txt, the pack in use (1: its file in *file; -1: none, HD off for
// this game; 0: not chosen, the first one). A pack just installed becomes the one in use.
void HiresPackFileName(const HiresPackEntry *e, char *out, size_t size);
int HiresActiveRead(const char *dir, char *file, size_t size);
void HiresActiveWrite(const char *dir, const char *file);   // NULL or "": none (HD off for it)
void HiresPngStart(const char *romName, const char *destDir);

// The user's own pack (Settings > HD textures > Optimize my pack): what a file or folder of
// the import folder is (HIMPORT_*; 0: not a pack), and its conversion into one DXT .hts in
// destDir, the game chosen (romName), named after it and made that game's pack in use
// (in the background, as a download: HPACK_CONVERT). The original is left as it is.
enum { HIMPORT_ZIP = 1, HIMPORT_7Z, HIMPORT_HTS, HIMPORT_PNG };
int HiresImportKind(const char *name, int isDir);
void HiresImportStart(const char *src, int isDir, const char *romName, const char *destDir);

#endif
