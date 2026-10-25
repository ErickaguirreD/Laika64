// Harissa64 V2 - XAudio2 output (see xb_audio.h).
//
// Continuity, as V1 does it (xenon/xbox360_audio.cpp on master):
//  - Rate control: when the emulation runs a little slow (or fast), the
//    playback rate is steered toward a TARGET_MS fill of the queue with
//    SetFrequencyRatio (RATE_MIN..RATE_MAX, smoothed per submit), so a few
//    percent of speed difference stretches the sound instead of cutting it.
//    The fill is measured just before each submit.
//  - Re-buffering: after an underrun the voice is paused until START_MS is
//    queued again, which turns many tiny gaps into one short pause.
#include "xb_audio.h"

#include <xtl.h>
#include <xaudio2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_log.h"

// minimp3 (public domain, see third_party/minimp3) - skin music in .mp3.
// MINIMP3_NO_STDIO: its own file-loading functions pull in <windows.h> and
// POSIX mmap headers this console's XDK doesn't have; this file never calls
// them anyway (mp3dec_load_buf decodes a buffer this file already read with
// fopen/fread, same as the WAV path below).
#define MINIMP3_NO_STDIO
#define MINIMP3_IMPLEMENTATION
#include "../../third_party/minimp3/minimp3_ex.h"

#define SLOTS 64   // one game buffer (~23 ms) per slot: 500 ms of margin needs ~22
#define SLOT_BYTES 16384
static int s_targetMs = XB_AUDIO_DEFAULT_MS;   // fill the rate control aims for (the pacing threshold)
#define TARGET_MS s_targetMs
#define START_MS (s_targetMs * 3 / 5)          // queued before (re)starting playback
#define RATE_GAIN 0.10f                    // rate change at an empty queue
#define RATE_MIN 0.90f
#define RATE_MAX 1.02f
#define RATE_SMOOTH 0.05f                  // per submit, to avoid audible wobble

static IXAudio2 *s_xa;
static IXAudio2MasteringVoice *s_master;
static IXAudio2SourceVoice *s_voice;
static u32 s_rate;
static u8 s_slots[SLOTS][SLOT_BYTES];
static int s_next;
static DWORD s_lastSubmit;
static XbAudioStats s_stats;
static u64 s_framesSubmitted;
static int s_playing;
static float s_ratio, s_ratioApplied;
static u64 s_periodFrames;   // frames submitted since the last stats reset
static float s_gameVolume = 1.0f;   // Settings > Volumen - reapplied to each new game voice (create_voice)
static float s_musicVolume = 1.0f;  // reapplied to each new music voice (xb_audio_music_play)

int xb_audio_init(void)
{
    if (s_xa) return 0;   // already running (the ROM browser starts it; a game's call then does nothing)
    if (FAILED(XAudio2Create(&s_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        H64_ERROR("[audio] XAudio2Create failed");
        s_xa = NULL;
        return -1;
    }
    if (FAILED(s_xa->CreateMasteringVoice(&s_master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, 0, NULL)))
    {
        H64_ERROR("[audio] CreateMasteringVoice failed");
        s_master = NULL;
        return -1;
    }
    return 0;
}

static void destroy_voice(void)
{
    if (s_voice)
    {
        s_voice->Stop(0);
        s_voice->FlushSourceBuffers();
        s_voice->DestroyVoice();
        s_voice = NULL;
    }
}

static void music_stop_internal(void);

void xb_audio_shutdown(void)
{
    destroy_voice();
    music_stop_internal();
    if (s_master) { s_master->DestroyVoice(); s_master = NULL; }
    if (s_xa) { s_xa->Release(); s_xa = NULL; }
}

void xb_audio_game_stop(void) { destroy_voice(); }

static int create_voice(u32 rate)
{
    WAVEFORMATEX wf;
    destroy_voice();
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = rate * 4;
    if (FAILED(s_xa->CreateSourceVoice(&s_voice, &wf, 0, XAUDIO2_MAX_FREQ_RATIO, NULL, NULL, NULL)))
    {
        H64_ERROR("[audio] CreateSourceVoice(%u Hz) failed", rate);
        s_voice = NULL;
        return -1;
    }
    s_voice->SetVolume(s_gameVolume);
    // Not started: playback begins once START_MS is queued.
    s_rate = rate;
    s_framesSubmitted = 0;
    s_playing = 0;
    s_ratio = s_ratioApplied = 1.0f;
    H64_INFO("[audio] voice at %u Hz", rate);
    return 0;
}

// Stereo frames queued and not played yet; *queued = buffers still held by the voice.
static u32 pending_frames(u32 *queued)
{
    XAUDIO2_VOICE_STATE vs;
    s_voice->GetState(&vs);
    *queued = vs.BuffersQueued;
    return vs.SamplesPlayed >= s_framesSubmitted ? 0 : (u32)(s_framesSubmitted - vs.SamplesPlayed);
}

void xb_audio_sink(void *user, const u8 *samples, u32 len, u32 rate)
{
    XAUDIO2_BUFFER b;
    u32 queued, pending, frames;
    (void)user;
    if (!s_xa || !s_master || len == 0 || rate < 4000 || rate > 96000) return;
    if (!s_voice || rate != s_rate)
        if (create_voice(rate)) return;
    pending = pending_frames(&queued);
    if (s_playing && queued == 0)
    {
        // Ran dry: pause and build a small reserve before playing again.
        s_voice->Stop(0);
        s_playing = 0;
        s_stats.underruns++;
    }
    if (queued >= SLOTS - 1) { s_stats.dropped++; return; }   // full: drop rather than overwrite a queued slot
    if (len > SLOT_BYTES) len = SLOT_BYTES;
    len &= ~3u;
    frames = len / 4;
    memcpy(s_slots[s_next], samples, len);
    memset(&b, 0, sizeof(b));
    b.AudioBytes = len;
    b.pAudioData = s_slots[s_next];
    s_voice->SubmitSourceBuffer(&b, NULL);
    s_next = (s_next + 1) % SLOTS;
    s_framesSubmitted += frames;
    s_periodFrames += frames;
    s_stats.buffers++;
    s_lastSubmit = GetTickCount();

    if (!s_playing)
    {
        if (pending + frames >= s_rate * START_MS / 1000 || queued + 1 >= SLOTS - 1)
        {
            s_voice->Start(0, XAUDIO2_COMMIT_NOW);
            s_playing = 1;
        }
        return;
    }
    {
        // Rate control on the fill before this submit.
        float fill = (float)pending / (float)(s_rate * TARGET_MS / 1000);
        float target = 1.0f + RATE_GAIN * (fill - 1.0f);
        if (target < RATE_MIN) target = RATE_MIN;
        if (target > RATE_MAX) target = RATE_MAX;
        s_ratio += (target - s_ratio) * RATE_SMOOTH;
        if (fabsf(s_ratio - s_ratioApplied) > 0.002f)
        {
            s_voice->SetFrequencyRatio(s_ratio, XAUDIO2_COMMIT_NOW);
            s_ratioApplied = s_ratio;
        }
        s_stats.ratioPermille = (u32)(s_ratio * 1000.0f + 0.5f);
        s_stats.fillMs = (u32)((u64)pending * 1000 / s_rate);
    }
}

int xb_audio_queued_ms(void)
{
    u32 queued, pending;
    if (!s_voice || GetTickCount() - s_lastSubmit > 250) return -1;
    pending = pending_frames(&queued);
    // A paused voice does not drain: never make the emulation wait on it.
    if (!s_playing) return 0;
    return (int)((u64)pending * 1000 / s_rate);
}

void xb_audio_stats(XbAudioStats *out, int reset)
{
    static u64 lastPlayed;
    static LARGE_INTEGER freq;
    static u64 lastTick;
    LARGE_INTEGER t;
    u64 now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    now = (u64)t.QuadPart * 1000 / (u64)freq.QuadPart;   // ms of real time (GetTickCount falls behind on the console)
    u64 played = 0;
    if (s_voice)
    {
        XAUDIO2_VOICE_STATE vs;
        s_voice->GetState(&vs);
        played = vs.SamplesPlayed;
    }
    s_stats.playedHz = (lastTick && now != lastTick && played >= lastPlayed) ? (u32)((played - lastPlayed) * 1000 / (now - lastTick)) : 0;
    s_stats.submittedHz = (lastTick && now != lastTick) ? (u32)(s_periodFrames * 1000 / (now - lastTick)) : 0;
    *out = s_stats;
    if (reset) { lastPlayed = played; lastTick = now; s_periodFrames = 0; }
    if (reset)
    {
        u32 ratio = s_stats.ratioPermille, fill = s_stats.fillMs;
        memset(&s_stats, 0, sizeof(s_stats));
        s_stats.ratioPermille = ratio;
        s_stats.fillMs = fill;
    }
}

void xb_audio_set_target_ms(int ms)
{
    s_targetMs = ms < 40 ? 40 : ms > 500 ? 500 : ms;
}

int xb_audio_target_ms(void) { return s_targetMs; }

// ---- Menu music (skins) ----
// A second, independent source voice on the same mastering voice as the
// game's (see xb_audio_init/game_stop above) - looping game:\skins\<name>\
// music.wav. WAV is always little-endian; the console's PowerPC core is
// big-endian, so every multi-byte field is read one byte at a time (never
// through a pointer cast).
static IXAudio2SourceVoice *s_musicVoice;
static u8 *s_musicData;         // the whole file, kept alive while the voice reads from it
static char s_musicSkin[64];    // which skin's music is loaded now ("": none)

static u32 rd_le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 rd_le16(const u8 *p) { return (u16)((u32)p[0] | (u32)p[1] << 8); }

struct WavInfo { u16 format, channels, bits; u32 rate; u32 dataOffset, dataSize; };

// Walks RIFF/WAVE chunks for "fmt " and "data". Returns 0 on success.
static int wav_parse(const u8 *buf, u32 size, WavInfo *out)
{
    u32 pos = 12;
    int haveFmt = 0, haveData = 0;
    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return -1;
    memset(out, 0, sizeof(*out));
    while (pos + 8 <= size)
    {
        const u8 *id = buf + pos;
        u32 chunkSize = rd_le32(buf + pos + 4), body = pos + 8;
        if (body + chunkSize > size) break;
        if (!memcmp(id, "fmt ", 4) && chunkSize >= 16)
        {
            out->format = rd_le16(buf + body);
            out->channels = rd_le16(buf + body + 2);
            out->rate = rd_le32(buf + body + 4);
            out->bits = rd_le16(buf + body + 14);
            haveFmt = 1;
        }
        else if (!memcmp(id, "data", 4))
        {
            out->dataOffset = body;
            out->dataSize = chunkSize;
            haveData = 1;
        }
        pos = body + chunkSize + (chunkSize & 1);   // chunks are word-aligned
    }
    return haveFmt && haveData ? 0 : -1;
}

static void music_stop_internal(void)
{
    if (s_musicVoice)
    {
        s_musicVoice->Stop(0);
        s_musicVoice->FlushSourceBuffers();
        s_musicVoice->DestroyVoice();
        s_musicVoice = NULL;
    }
    free(s_musicData);
    s_musicData = NULL;
    s_musicSkin[0] = 0;
}

void xb_audio_music_stop(void) { music_stop_internal(); }

// Tries music.wav first (the original, still the simplest choice for a
// skin), then music.mp3 (minimp3, see the include above) - whichever the
// skin actually has. Either way the result ends up in s_musicData as plain
// PCM16 ready for XAudio2, at (pcmData, pcmBytes), rate and channels.
int xb_audio_music_play(const char *skinName)
{
    char path[160];
    FILE *f;
    long size;
    u8 *fileData;
    int isMp3 = 0;
    WAVEFORMATEX wf;
    XAUDIO2_BUFFER b;
    const void *pcmData;
    u32 pcmBytes, rate;
    int channels;

    if (!skinName || !skinName[0]) { music_stop_internal(); return 0; }
    if (!strcmp(skinName, s_musicSkin) && s_musicVoice) return 0;   // already playing it
    if (!s_xa || !s_master) return -1;
    _snprintf(path, sizeof(path) - 1, "game:\\skins\\%s\\music.wav", skinName);
    path[sizeof(path) - 1] = 0;
    f = fopen(path, "rb");
    if (!f)
    {
        _snprintf(path, sizeof(path) - 1, "game:\\skins\\%s\\music.mp3", skinName);
        path[sizeof(path) - 1] = 0;
        f = fopen(path, "rb");
        isMp3 = 1;
    }
    if (!f) { music_stop_internal(); return -1; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 32 * 1024 * 1024) { fclose(f); music_stop_internal(); return -1; }
    fileData = (u8 *)malloc((size_t)size);
    if (!fileData || fread(fileData, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(fileData);
        return -1;
    }
    fclose(f);
    music_stop_internal();   // the new file read fine - now drop whatever was playing

    if (isMp3)
    {
        mp3dec_t dec;
        mp3dec_file_info_t info;
        mp3dec_init(&dec);
        memset(&info, 0, sizeof(info));
        // Unlike a .wav's data chunk, minimp3 writes samples through plain
        // C int16_t stores: on this big-endian host they come out already
        // in XAudio2's native byte order - no swap needed, unlike the WAV path.
        if (mp3dec_load_buf(&dec, fileData, (size_t)size, &info, NULL, NULL) || !info.buffer || !info.samples ||
            (info.channels != 1 && info.channels != 2))
        {
            H64_WARN("[audio] skin music %s: MP3 decode failed", path);
            free(fileData);
            free(info.buffer);
            return -1;
        }
        free(fileData);
        s_musicData = (u8 *)info.buffer;
        pcmData = s_musicData;
        pcmBytes = (u32)(info.samples * sizeof(mp3d_sample_t));
        rate = (u32)info.hz;
        channels = info.channels;
    }
    else
    {
        WavInfo wav;
        if (wav_parse(fileData, (u32)size, &wav) || wav.format != 1 /* PCM */ || wav.bits != 16 ||
            (wav.channels != 1 && wav.channels != 2))
        {
            H64_WARN("[audio] skin music %s: not a supported WAV (PCM 16-bit mono/stereo)", path);
            free(fileData);
            return -1;
        }
        // A .wav's data chunk is always little-endian (the RIFF format
        // itself, whatever machine wrote it) - but this console's XAudio2
        // wants PCM in its own native byte order, big-endian, exactly like
        // the N64's AI buffers that xb_audio_sink passes through unmodified
        // (see the top of xb_audio.h). Every 16-bit sample needs swapping
        // once, here, or the voice plays noise instead of the tone.
        {
            u8 *s = fileData + wav.dataOffset, *end = s + (wav.dataSize & ~1u);
            for (; s < end; s += 2)
            {
                u8 t = s[0];
                s[0] = s[1];
                s[1] = t;
            }
        }
        s_musicData = fileData;
        pcmData = s_musicData + wav.dataOffset;
        pcmBytes = wav.dataSize;
        rate = wav.rate;
        channels = wav.channels;
    }

    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
    if (FAILED(s_xa->CreateSourceVoice(&s_musicVoice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)))
    {
        H64_ERROR("[audio] skin music %s: CreateSourceVoice failed", path);
        free(s_musicData);
        s_musicData = NULL;
        s_musicVoice = NULL;
        return -1;
    }
    s_musicVoice->SetVolume(s_musicVolume);
    memset(&b, 0, sizeof(b));
    b.AudioBytes = pcmBytes;
    b.pAudioData = (const BYTE *)pcmData;
    b.LoopCount = XAUDIO2_LOOP_INFINITE;   // LoopBegin/LoopLength left 0: loops the whole buffer
    s_musicVoice->SubmitSourceBuffer(&b, NULL);
    s_musicVoice->Start(0, XAUDIO2_COMMIT_NOW);
    strncpy(s_musicSkin, skinName, sizeof(s_musicSkin) - 1);
    s_musicSkin[sizeof(s_musicSkin) - 1] = 0;
    H64_INFO("[audio] skin music: %s (%u Hz, %d ch)", path, rate, channels);
    return 0;
}

static float volume_from_percent(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent / 100.0f;
}

void xb_audio_set_game_volume(int percent)
{
    s_gameVolume = volume_from_percent(percent);
    if (s_voice) s_voice->SetVolume(s_gameVolume);
}

void xb_audio_set_music_volume(int percent)
{
    s_musicVolume = volume_from_percent(percent);
    if (s_musicVoice) s_musicVoice->SetVolume(s_musicVolume);
}
