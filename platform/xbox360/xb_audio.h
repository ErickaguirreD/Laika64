// Harissa64 V2 - XAudio2 output of the AI buffers (Xbox 360).
//
// Each AI buffer is copied into a ring of slots and queued unmodified: the
// N64 writes big-endian 16-bit stereo, which is the console's native PCM.
// The source voice runs at the game's DAC rate (XAudio2 resamples) and is
// recreated when the rate changes. The emulation is paced by the queue: the
// main loop waits while more than xb_audio_target_ms() are queued. Rate
// control and re-buffering keep the sound continuous (see xb_audio.cpp).
#ifndef XB_AUDIO_H
#define XB_AUDIO_H

#include "../../core/common/h64_types.h"

#define XB_AUDIO_DEFAULT_MS 500   // user choice (2026-10-09, sound cuts at 250 in the Rare games); V1 used 100: one underrun every 2 s on OoT

int xb_audio_init(void);   // idempotent: a second call while already running does nothing
// Queue target (ini audioms=, 40..500): the pacing threshold and the fill the
// rate control aims for; playback (re)starts at 60 % of it.
void xb_audio_set_target_ms(int ms);
int xb_audio_target_ms(void);
void xb_audio_shutdown(void);
// Destroys only the game's voice (end of a game, back to the ROM browser):
// the engine and mastering voice, and any menu music voice, are left alone,
// since menu music must keep playing. xb_audio_shutdown is the full teardown.
void xb_audio_game_stop(void);
// The AI sink (H64System::aiSink).
void xb_audio_sink(void *user, const u8 *samples, u32 len, u32 rate);
// Milliseconds of sound queued and not played yet (-1: no sound queued for
// a while, pace on the clock instead).
int xb_audio_queued_ms(void);

// playedHz: stereo frames the voice really played per second (QPC) since the last reset.
struct XbAudioStats { u32 buffers, underruns, fillMs, ratioPermille, playedHz, submittedHz, dropped; };
void xb_audio_stats(XbAudioStats *out, int reset);

// ---- Menu music (game:\music\) ----
// The .mp3 and .wav (PCM 16-bit, mono or stereo) files of `folder`, on their
// own XAudio2 voice, independent of the game's: in name order or at random,
// one after the other (xb_audio_music_tick, called by every menu loop through
// UiInputPoll, moves on when a track ends), or one chosen track over and
// over. No tracks, or an empty folder name, just stop whatever was playing.
// Needs xb_audio_init() already called (the ROM browser calls it once; a
// game's own xb_audio_init call is then a no-op). A call that changes nothing
// (same folder, mode and track, still playing) keeps the music going.
enum { XB_MUSIC_ORDER = 0, XB_MUSIC_RANDOM, XB_MUSIC_TRACK, XB_MUSIC_OFF };   // OFF: no music at all
int xb_audio_music_start(const char *folder, int mode, const char *track);
void xb_audio_music_tick(void);
void xb_audio_music_stop(void);
// The folder's tracks, for the menu: their number (sorted by name), each one's file name.
int xb_audio_music_scan(const char *folder);
const char *xb_audio_music_track(int i);

// ---- Interface sounds (Settings > Sound > Interface sounds) ----
// Short tones made here (no sound files), a different one per kind of press,
// in the spirit of the console's own menus. Played on their own voices, at
// their own volume (xb_audio_set_sfx_volume); nothing happens before
// xb_audio_init or when off.
enum { XB_SFX_MOVE = 0, XB_SFX_SELECT, XB_SFX_BACK, XB_SFX_ACTION, XB_SFX_PAGE, XB_SFX_OPEN,
       XB_SFX_TAB,        // the ROM list's tabs (LT/RT)
       XB_SFX_VALUE_UP,   // a setting moved on (right) ...
       XB_SFX_VALUE_DOWN, // ... or back (left)
       XB_SFX_FAV_ON,     // a game into the favourites ...
       XB_SFX_FAV_OFF,    // ... and out
       XB_SFX_LAUNCH,     // a game starts
       XB_SFX_SAVE,       // settings saved
       XB_SFX_NOTIFY,     // a message (an update, box art, the battery)
       XB_SFX_ERROR,      // something that cannot be done
       XB_SFX_SAVER,      // the screen saver comes on
       XB_SFX_COUNT };
// The sounds' style (Settings > Sound > Sound style): glass (the first ones),
// soft, retro (8-bit square waves), digital.
enum { XB_SFX_STYLE_GLASS = 0, XB_SFX_STYLE_SOFT, XB_SFX_STYLE_RETRO, XB_SFX_STYLE_DIGITAL, XB_SFX_STYLE_COUNT };
void xb_audio_ui_sound(int kind);
// The same in place of the sound a press started a moment ago (UiInputPoll
// plays one per button: B's "back" becomes "out of the favourites").
void xb_audio_ui_sound_instead(int kind);
void xb_audio_ui_enable(int on);
// Every sound made again in another style (the next sound plays in it).
void xb_audio_ui_style(int style);
// A folder of the user's own sounds (Settings > Paths > Interface sounds): a
// <name>.wav there (move, select, back, action, page, open, tab, value_up,
// value_down, fav_on, fav_off, launch, save, notify, error, saver; PCM 8 or
// 16-bit, mono or stereo, any rate) plays in place of the made-up one.
void xb_audio_ui_folder(const char *folder);

// ---- Volume (Settings > Volumen) ----
// 0..100, applied at once to whichever voice is live and remembered for the
// next one (a game's voice is recreated whenever the DAC rate changes - see
// create_voice - and the music voice every time xb_audio_music_play runs).
void xb_audio_set_game_volume(int percent);
void xb_audio_set_music_volume(int percent);
void xb_audio_set_sfx_volume(int percent);   // the interface sounds
// Settings > Sound > Audio output: 1 mixes the game's and the music's two
// channels into both speakers (mono), 0 keeps them apart (stereo).
void xb_audio_set_mono(int on);

#endif
