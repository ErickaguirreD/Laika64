// LAIKA 64 - preview videos in the ROM list (ScreenScraper's, CC BY-NC-SA 4.0).
//
// The game the list rests on plays its video in the card, round and round, with its sound
// (Settings > Sound > Preview sound) over the menus' music, which is silent meanwhile. Each video
// is downloaded once (Game images > Preview videos, with the downloads on) into
// <drive>:\art\games\video\<key>.mp4 and played from there afterwards.
//
// A thread of its own (hardware thread 4) looks the game up on ScreenScraper (by the ROM file's
// name and size, else by its title), downloads, and decodes the picture (xb_video.cpp) and the
// sound (FAAD2): the sound is the clock, the pictures follow it. The list asks for the picture
// every frame; when it stops asking (another screen, a game) the video stops by itself.
#ifndef XB_PREVIEW_H
#define XB_PREVIEW_H

#include <xtl.h>

// Starts the thread (first call). After ArtStart (its folder and the games' keys).
void PreviewStart(void);
// enabled: videos at all; sound: their sound; network: missing ones may be downloaded.
void PreviewSettings(int enabled, int sound, int network);
// The game whose video should play (NULL or "": none); `title` helps find it on ScreenScraper.
// Cheap to call every frame: only a change does anything.
void PreviewWant(const char *romPath, const char *title);
// romPath's video picture now, as a texture to draw over (w x h); NULL while there is none.
// *since: UiTime() when its first picture came (its fade-in). Every frame from the list's thread.
IDirect3DTexture9 *PreviewTexture(IDirect3DDevice9 *dev, const char *romPath, int *w, int *h, float *since);
// How many times the video playing now has reached its end (0 in its first round).
int PreviewRounds(void);
// The user's own ScreenScraper account (Settings > Game images; "" user: none, the anonymous access).
// Its check runs on the thread: the status is PREVIEW_ACCOUNT_CHECKING until it answers, then OK (with
// its level, download speed in KB/s and requests today / per day, as ScreenScraper says them), BAD (the
// name or password refused: the anonymous access is used meanwhile) or NONET.
enum { PREVIEW_ACCOUNT_NONE = 0, PREVIEW_ACCOUNT_UNKNOWN, PREVIEW_ACCOUNT_CHECKING, PREVIEW_ACCOUNT_OK, PREVIEW_ACCOUNT_BAD,
       PREVIEW_ACCOUNT_NONET };
void PreviewAccount(const char *user, const char *pass);
void PreviewAccountCheck(void);
int PreviewAccountStatus(char *level, char *speed, char *today, char *maxDay);   // each 16 bytes, or NULL
// Every game's video downloaded in the background, one at a time, while no video plays (with the
// downloads on); the list is copied. Status: how far it is; 1 while it goes on.
void PreviewBulkStart(const char *const *paths, int count);
int PreviewBulkStatus(int *done, int *total);
// A picture of the game from ScreenScraper (`media`: its kinds, the first there is taken: "box-2D",
// "box-3D", "ss", "sstitle", "wheel,wheel-hd") into `out` (a PNG; from xb_art.cpp's thread), the game
// found by its file, `altName` (the databases' file name for it, or NULL) or `title`.
// 1: saved, 0: none, -1: no answer.
// `crc`: the whole file's CRC32 in hex ("1A2B3C4D", a hack's), asked first: its own page when ScreenScraper has it.
// `title2`: another title tried when nothing is found (a hack's name in its header: "MK64 - AMPED UP").
int PreviewFetchMedia(const char *romPath, const char *altName, const char *title, const char *media, const char *out, const char *crc = NULL,
                      const char *title2 = NULL);
// A hack's own page on ScreenScraper, found by the whole file's CRC32 (hex) with its name and size only (never
// by title: that finds its original): its name, description (in `lang`, else English), developer, publisher,
// date, genre and players into the fields of *info that it has. 1 found, 0 not there, -1 no answer.
struct H64GameInfo;
int PreviewFetchInfo(const char *romPath, const char *crc, const char *lang, H64GameInfo *info);
// Stops the video and frees its memory (before a game), waiting for the thread; the textures too.
// The bulk pass waits until the list is back (PreviewWant of a game).
void PreviewStop(IDirect3DDevice9 *dev);

#endif
