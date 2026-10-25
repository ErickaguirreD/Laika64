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

// ---- Menu music (skins - see xb_menu.h SkinScan) ----
// Loads and loops game:\skins\<skinName>\music.wav (PCM, 16-bit, mono or
// stereo) on its own XAudio2 voice, independent of the game's. Empty name,
// a missing file or an unsupported WAV just stop whatever was playing.
// Needs xb_audio_init() already called (the ROM browser calls it once,
// before any skin music; a game's own xb_audio_init call is then a no-op).
int xb_audio_music_play(const char *skinName);
void xb_audio_music_stop(void);

// ---- Volume (Settings > Volumen) ----
// 0..100, applied at once to whichever voice is live and remembered for the
// next one (a game's voice is recreated whenever the DAC rate changes - see
// create_voice - and the music voice every time xb_audio_music_play runs).
void xb_audio_set_game_volume(int percent);
void xb_audio_set_music_volume(int percent);

#endif
