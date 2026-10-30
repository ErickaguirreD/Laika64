// Harissa64 - box art and game information for the ROM browser.
//
// Everything lives in <drive>:\art\games\ (game:\ on the console, cache:\ in
// Xenia), one pair of files per game, named by its cache key (the header's
// game code, "NSME"; h64_gameinfo_key):
//   NSME.png   the box art (libretro-thumbnails, as downloaded)
//   NSME.txt   what is known about the game (h64_gameinfo_format)
//   db\        the databases the matching uses (h64_gamedb_files)
// tools/fetch_game_art.py fills the same folder from a PC.
//
// A worker thread (hardware thread 3) reads, decodes and, when allowed,
// downloads; the UI thread never waits for it. The browser says which games
// it wants (the selection first, then its neighbours: those only from the
// files already there) and takes the results when they are ready.
#ifndef XB_ART_H
#define XB_ART_H

#include "../../core/cart/h64_gameinfo.h"
#include "../../core/common/h64_types.h"

// The size covers are decoded to (UI pixels: the browser draws them 1:1 or smaller).
#define ART_COVER_W 400
#define ART_COVER_H 232
// A tiny copy, drawn enlarged as a soft coloured glow behind the cover.
#define ART_GLOW_W 16
#define ART_GLOW_H 10

// Which picture of a game the browser shows (libretro-thumbnails' folders):
// the box, a picture from the game, its title screen, its logo.
enum { ART_KIND_BOX = 0, ART_KIND_SNAP, ART_KIND_TITLE, ART_KIND_LOGO, ART_KIND_COUNT };

struct ArtResult
{
    char path[256];         // the ROM file
    int kind;               // ART_KIND_* asked for when it was read (the box when the other is not there)
    int kinds;              // the ART_KIND_* pictures there are of it (bit 1 << kind)
    int shownKind;          // the one in `cover` (`kind`, or the box when that one is not there)
    char key[16];
    int hasInfo;            // `info` read from the cache (else only `key` is known)
    H64GameInfo info;
    u8 *cover;              // RGBA, coverW x coverH, or NULL (when `dxt` holds it)
    int coverW, coverH;
    u8 *dxt;                // or the same compressed: DXT1 as the Xbox 360's GPU reads it (dxtW x dxtH, whole 4x4 blocks), or NULL
    int dxtW, dxtH;
    u8 *glow;               // RGBA, ART_GLOW_W x ART_GLOW_H, or NULL
};

struct ArtStatus
{
    int bulk;               // "download all" running
    int done, total;        // its progress
    int found, covers;      // games identified, box art present so far
    int offline;            // the last attempt found no network
    char busyPath[256];     // the ROM whose files are being downloaded now ("" when none)
};

// Starts the worker (first call); `drive` is "game" or "cache".
void ArtStart(const char *drive);
// Whether the selection's missing files may be downloaded.
void ArtSetNetwork(int allowed);
// With downloads on: 0 fetches only the box art (not pictures from the game, title screens, logos).
void ArtSetExtras(int allowed);
// While a game runs: the worker finishes what it is doing and waits.
void ArtPause(int paused);
// The ROMs the browser shows, the selection first (`count` up to 64: the list
// view wants 5, a covers grid its visible page and then the pages on either
// side, read ahead). Replaces the previous list; results come once per ROM
// until ArtForgetAll. A cover decoded once is kept ready-made in <key>.cov.
void ArtWant(const char *const *paths, int count);
// The next result (1), to free with ArtResultFree.
int ArtTake(ArtResult *r);
void ArtResultFree(ArtResult *r);
// The browser dropped its copy of a ROM's result (or of all): sent again when wanted.
void ArtForget(const char *path);
void ArtForgetAll(void);
// Downloads what is missing for all these ROMs (copied), in the background.
// `retry`: also what was not found before, with fresh databases, and a
// summary at the end; otherwise only what was never looked for, quietly
// (a summary only when something new came).
void ArtBulkStart(const char *const *paths, int count, int retry);
void ArtGetStatus(ArtStatus *s);
// What is known about a ROM (its <key>.txt), read now on the calling thread
// (the ROM's header, then the file: nothing downloaded). 1 when the game was
// identified (the browser's categories), 0 otherwise. After ArtStart.
int ArtReadInfo(const char *romPath, H64GameInfo *info);
// The picture shown (ART_KIND_*): the ones already sent are sent again in it,
// downloads look for it from now on. Any time, from the browser's thread.
void ArtSetKind(int kind);
int ArtGetKind(void);
// The menus' language (a code: "es"): the games' descriptions in it (from that
// Wikipedia, kept beside the English one) where there is one; Arabic stays English.
void ArtSetLang(const char *code);
// One game's picture in another kind (the browser's right stick): it is sent
// again in it; -1 goes back to ArtSetKind's. And the kind asked for it now.
void ArtShowKind(const char *romPath, int kind);
int ArtKindFor(const char *romPath);
// A game's picture as the list shows it (kind, else the box), decoded and
// scaled (ART_COVER_W x ART_COVER_H at most, RGBA, free() it), read now on the
// calling thread - the pause menu's "Now playing". 0 when there is none. After ArtStart.
int ArtReadCover(const char *romPath, int kind, u8 **rgba, int *w, int *h, int *gotKind);   // gotKind: the one read (the box as a fallback)
// A message for the user once ("No network connection", the bulk summary).
int ArtTakeNote(char *text, int size);

#endif
