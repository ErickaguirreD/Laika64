// LAIKA 64 - XAudio2 output (see xb_audio.h).
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

// minimp3 (public domain, see third_party/minimp3) - the menu music's .mp3 tracks (game:\music\).
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
// Queued before (re)starting playback: the whole target. Starting at 3/5 of it left
// the rate control to make up the rest by playing slower - 3.6 % at a game's start
// (OoT, Smash Remix: the narrator's voice dragged for seconds).
#define START_MS s_targetMs
// Rate change at an empty queue. The emulation's pacing (main.cpp) already refills
// the queue by running up to 1 % faster; at 0.10 a dip after a slow frame (a JIT
// flush) pulled the pitch down 1.3 % and the sound behind the picture.
#define RATE_GAIN 0.03f
#define RATE_MIN 0.97f
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
static float s_musicVolume = 1.0f;  // reapplied to each new music voice (music_open)
static float s_sfxVolume = 1.0f;    // the interface sounds (xb_audio_ui_sound)
static int s_mono;                  // Settings > Sound > Audio output: mono
static volatile LONG s_musicDuck;   // a preview video's sound plays: the music is silent meanwhile
static int s_musicPaused;           // xb_audio_music_pause (under the music's lock)

// The music and a preview's sound cross-fade (MUSIC_FADE_MS each way) instead of one cutting the other: how far the
// music has gone down (0 playing, 1 silent) and how far a preview's sound has come up (music_thread moves both).
#define MUSIC_FADE_MS 1200.0f
static float s_duckLevel, s_pvLevel;
static float music_gain(void) { return s_musicVolume * (1.0f - s_duckLevel); }

// A stereo voice's way to the speakers: its two channels apart (stereo) or
// mixed into both (mono). The mastering voice may have more channels than
// two (5.1): only its front left and right get anything.
static void apply_output_matrix(IXAudio2SourceVoice *v, int sourceChannels)
{
    XAUDIO2_VOICE_DETAILS md;
    float m[2 * 8];
    int d;
    if (!v || !s_master || sourceChannels != 2) return;
    s_master->GetVoiceDetails(&md);
    if (md.InputChannels < 2 || md.InputChannels > 8) return;
    memset(m, 0, sizeof(m));
    for (d = 0; d < 2; d++)
    {
        // pLevelMatrix[SourceChannels * d + s]: source channel s into destination channel d.
        m[2 * d + 0] = s_mono ? 0.5f : (d == 0 ? 1.0f : 0.0f);
        m[2 * d + 1] = s_mono ? 0.5f : (d == 1 ? 1.0f : 0.0f);
    }
    v->SetOutputMatrix(NULL, 2, md.InputChannels, m);
}

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
static void sfx_stop_internal(void);

void xb_audio_shutdown(void)
{
    destroy_voice();
    xb_audio_preview_close();
    music_stop_internal();
    sfx_stop_internal();
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
    apply_output_matrix(s_voice, 2);
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
// game's (see xb_audio_init/game_stop above) - the tracks of game:\music\
// (.wav or .mp3) one after the other, round and round (xb_audio_music_tick
// moves on when one ends; a single track just loops). WAV is always
// little-endian; the console's PowerPC core is big-endian, so every
// multi-byte field is read one byte at a time (never through a pointer cast).
#define MUSIC_MAX_TRACKS 64
static IXAudio2SourceVoice *s_musicVoice;
static char s_musicFolder[96];  // the folder playing now ("": none)
static char s_musicTracks[MUSIC_MAX_TRACKS][96];   // its tracks' file names, sorted
static int s_musicCount, s_musicIndex, s_musicMode;
static char s_musicPick[96];    // XB_MUSIC_TRACK: the track chosen

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

static int ends_with(const char *s, const char *ext);   // below

static int track_cmp(const void *a, const void *b) { return _stricmp((const char *)a, (const char *)b); }

int xb_audio_music_scan(const char *folder)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[128];
    s_musicCount = 0;
    if (!folder || !folder[0]) return 0;
    _snprintf(pattern, sizeof(pattern) - 1, "%s\\*", folder);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || s_musicCount >= MUSIC_MAX_TRACKS) continue;
            if (!ends_with(fd.cFileName, ".mp3") && !ends_with(fd.cFileName, ".wav")) continue;
            strncpy(s_musicTracks[s_musicCount], fd.cFileName, sizeof(s_musicTracks[0]) - 1);
            s_musicTracks[s_musicCount][sizeof(s_musicTracks[0]) - 1] = 0;
            s_musicCount++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    qsort(s_musicTracks, s_musicCount, sizeof(s_musicTracks[0]), track_cmp);
    return s_musicCount;
}

const char *xb_audio_music_track(int i) { return i >= 0 && i < s_musicCount ? s_musicTracks[i] : ""; }

// ---- Streaming ----
// A track is played as it is decoded, a few short buffers ahead, on a thread
// of its own: reading a file and decoding a whole MP3 at once (40 MB of PCM
// for four minutes) held the menus for seconds at each change of track. The
// file's bytes stay in memory (a few MB of MP3), the PCM goes through a ring
// of MUS_BUFS buffers; the next track follows with no gap when its format
// is the same (else its own voice). s_musLock guards the voice and the
// track list against the menus' thread (start, stop, volume, mono).
#define MUS_BUFS 6
#define MUS_FRAMES 8192   // per buffer (per channel): ~0.19 s at 44.1 kHz
static CRITICAL_SECTION s_musLock;
static int s_musLockReady;
static HANDLE s_musThread;
static volatile LONG s_musQuit;
static s16 *s_musBuf[MUS_BUFS];
static int s_musBufNext;
static u8 *s_musFile;           // the track playing: its file's bytes
static long s_musSize, s_musPos;  // and how far it is decoded (MP3: bytes; WAV: into its data)
static int s_musIsMp3, s_musLoop, s_musCh;
static u32 s_musRate, s_musDataStart, s_musDataEnd;
static mp3dec_t s_musDec;

static void music_lock(void)
{
    if (!s_musLockReady) { InitializeCriticalSection(&s_musLock); s_musLockReady = 1; }
    EnterCriticalSection(&s_musLock);
}

static void music_unlock(void) { LeaveCriticalSection(&s_musLock); }

static void music_voice_stop(void)
{
    if (s_musicVoice)
    {
        s_musicVoice->Stop(0);
        s_musicVoice->FlushSourceBuffers();
        s_musicVoice->DestroyVoice();
        s_musicVoice = NULL;
    }
    free(s_musFile);
    s_musFile = NULL;
}

static void music_thread_stop(void)
{
    if (s_musThread)
    {
        InterlockedExchange(&s_musQuit, 1);
        WaitForSingleObject(s_musThread, INFINITE);
        CloseHandle(s_musThread);
        s_musThread = NULL;
    }
}

static void music_stop_internal(void)
{
    music_thread_stop();
    music_lock();
    music_voice_stop();
    s_musicFolder[0] = 0;
    s_musicCount = 0;
    music_unlock();
}

void xb_audio_music_stop(void) { music_stop_internal(); }

static int ends_with(const char *s, const char *ext)
{
    size_t n = strlen(s), e = strlen(ext);
    return n > e && !_stricmp(s + n - e, ext);
}

// The track's file read in and its format found (decoding starts at its beginning). 0 when it plays.
static int music_open(const char *path, int loop)
{
    FILE *f;
    long size;
    u8 *data;
    int mp3 = ends_with(path, ".mp3"), ch;
    u32 rate, start = 0, end = 0;
    f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024 * 1024) { fclose(f); return -1; }
    data = (u8 *)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(data);
        return -1;
    }
    fclose(f);
    if (mp3)
    {
        // The first frame gives the format (the rest are decoded as they are wanted).
        mp3dec_frame_info_t info;
        mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        long pos = 0;
        mp3dec_init(&s_musDec);
        memset(&info, 0, sizeof(info));
        while (pos < size)
        {
            int n = mp3dec_decode_frame(&s_musDec, data + pos, (int)(size - pos), pcm, &info);
            if (!info.frame_bytes) break;
            if (n > 0) break;
            pos += info.frame_bytes;   // ID3 tags and the like
        }
        if (!info.frame_bytes || (info.channels != 1 && info.channels != 2) || !info.hz)
        {
            H64_WARN("[audio] music %s: MP3 decode failed", path);
            free(data);
            return -1;
        }
        mp3dec_init(&s_musDec);
        rate = (u32)info.hz;
        ch = info.channels;
        start = 0;
    }
    else
    {
        WavInfo wav;
        if (wav_parse(data, (u32)size, &wav) || wav.format != 1 /* PCM */ || wav.bits != 16 || (wav.channels != 1 && wav.channels != 2))
        {
            H64_WARN("[audio] music %s: not a supported WAV (PCM 16-bit mono/stereo)", path);
            free(data);
            return -1;
        }
        rate = wav.rate;
        ch = wav.channels;
        start = wav.dataOffset;
        end = wav.dataOffset + (wav.dataSize & ~1u);
    }
    if (s_musicVoice && (rate != s_musRate || ch != s_musCh))
    {
        // Another format: the next one on a voice of its own, once this one has played out.
        XAUDIO2_VOICE_STATE st;
        for (;;)
        {
            s_musicVoice->GetState(&st);
            if (!st.BuffersQueued || s_musQuit) break;
            music_unlock();
            Sleep(10);
            music_lock();
        }
        s_musicVoice->DestroyVoice();
        s_musicVoice = NULL;
    }
    free(s_musFile);
    s_musFile = data;
    s_musSize = size;
    s_musPos = start;
    s_musDataStart = start;
    s_musDataEnd = end;
    s_musIsMp3 = mp3;
    s_musLoop = loop;
    s_musRate = rate;
    s_musCh = ch;
    if (!s_musicVoice)
    {
        WAVEFORMATEX wf;
        memset(&wf, 0, sizeof(wf));
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = (WORD)ch;
        wf.nSamplesPerSec = rate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)(ch * 2);
        wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
        if (FAILED(s_xa->CreateSourceVoice(&s_musicVoice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)))
        {
            H64_ERROR("[audio] music %s: CreateSourceVoice failed", path);
            s_musicVoice = NULL;
            return -1;
        }
        s_musicVoice->SetVolume(music_gain());
        apply_output_matrix(s_musicVoice, ch);
        if (!s_musicPaused) s_musicVoice->Start(0, XAUDIO2_COMMIT_NOW);
    }
    H64_INFO("[audio] music: %s (%u Hz, %d ch)", path, rate, ch);
    return 0;
}

// The next track that plays (past any that cannot be played): the following
// one in name order, a different one at random, or the chosen one again.
static int music_next(void)
{
    int tries;
    for (tries = 0; tries < s_musicCount; tries++)
    {
        char path[200];
        if (s_musicMode == XB_MUSIC_RANDOM && s_musicCount > 1)
        {
            int k = rand() % (s_musicCount - 1);
            s_musicIndex = s_musicIndex < 0 || k < s_musicIndex ? k : k + 1;   // never the same one twice in a row
        }
        else if (s_musicMode != XB_MUSIC_TRACK || s_musicIndex < 0) s_musicIndex = (s_musicIndex + 1) % s_musicCount;
        _snprintf(path, sizeof(path) - 1, "%s\\%s", s_musicFolder, s_musicTracks[s_musicIndex]);
        path[sizeof(path) - 1] = 0;
        if (!music_open(path, s_musicCount == 1 || s_musicMode == XB_MUSIC_TRACK)) return 0;
        if (s_musicMode == XB_MUSIC_TRACK) s_musicMode = XB_MUSIC_ORDER;   // the chosen one does not play: the others instead
    }
    return -1;   // none of them plays
}

// One buffer of the track's PCM (frames per channel; 0 at its end).
static int music_decode(s16 *out)
{
    int frames = 0;
    if (s_musIsMp3)
    {
        while (frames + MINIMP3_MAX_SAMPLES_PER_FRAME / 2 <= MUS_FRAMES && s_musPos < s_musSize)
        {
            mp3dec_frame_info_t info;
            // minimp3 writes plain C int16_t: already in XAudio2's (big-endian) byte order here.
            int n = mp3dec_decode_frame(&s_musDec, s_musFile + s_musPos, (int)(s_musSize - s_musPos), out + frames * s_musCh, &info);
            if (!info.frame_bytes) { s_musPos = s_musSize; break; }
            s_musPos += info.frame_bytes;
            if (n > 0 && info.channels == s_musCh) frames += n;
        }
    }
    else
    {
        // A .wav's data is little-endian (RIFF): each sample swapped to the console's order.
        u32 bytes = (u32)MUS_FRAMES * s_musCh * 2, left = s_musDataEnd > (u32)s_musPos ? s_musDataEnd - (u32)s_musPos : 0, i;
        const u8 *s = s_musFile + s_musPos;
        u8 *d = (u8 *)out;
        if (bytes > left) bytes = left;
        for (i = 0; i + 1 < bytes; i += 2) { d[i] = s[i + 1]; d[i + 1] = s[i]; }
        s_musPos += bytes;
        frames = (int)(bytes / (s_musCh * 2));
    }
    return frames;
}

static void pv_lock(void);
static void pv_unlock(void);
static IXAudio2SourceVoice *s_pvVoice;
// One step of the fades: the music towards silent while a preview plays (and back after), the preview's sound up.
static void fade_step(float ms)
{
    const float step = ms / MUSIC_FADE_MS, duck = s_musicDuck ? 1.0f : 0.0f;
    if (s_duckLevel != duck)
    {
        s_duckLevel += s_duckLevel < duck ? step : -step;
        if (s_duckLevel < 0) s_duckLevel = 0;
        if (s_duckLevel > 1) s_duckLevel = 1;
        music_lock();
        if (s_musicVoice) s_musicVoice->SetVolume(music_gain());
        music_unlock();
    }
    if (!s_pvVoice || s_pvLevel >= 1.0f) return;   // (no voice: its lock may not be made yet)
    pv_lock();
    if (s_pvVoice && s_pvLevel < 1.0f)
    {
        s_pvLevel += step;
        if (s_pvLevel > 1) s_pvLevel = 1;
        s_pvVoice->SetVolume(s_musicVolume * s_pvLevel);
    }
    pv_unlock();
}

static DWORD WINAPI music_thread(LPVOID)
{
    while (!s_musQuit)
    {
        XAUDIO2_VOICE_STATE st;
        music_lock();
        if (s_musicVoice && s_musFile)
        {
            s_musicVoice->GetState(&st);
            while (st.BuffersQueued < MUS_BUFS - 1 && !s_musQuit)
            {
                s16 *buf = s_musBuf[s_musBufNext];
                int frames = music_decode(buf);
                if (!frames)
                {
                    // The end of the track: again (one alone, or the one chosen), else the next one.
                    if (s_musLoop)
                    {
                        s_musPos = (long)s_musDataStart;
                        if (s_musIsMp3) mp3dec_init(&s_musDec);
                    }
                    else if (music_next() || !s_musicVoice) { music_voice_stop(); break; }
                    if (!s_musicVoice) break;
                    s_musicVoice->GetState(&st);
                    continue;
                }
                {
                    XAUDIO2_BUFFER b;
                    memset(&b, 0, sizeof(b));
                    b.AudioBytes = (UINT32)(frames * s_musCh * 2);
                    b.pAudioData = (const BYTE *)buf;
                    if (FAILED(s_musicVoice->SubmitSourceBuffer(&b, NULL))) break;
                }
                s_musBufNext = (s_musBufNext + 1) % MUS_BUFS;
                s_musicVoice->GetState(&st);
            }
        }
        music_unlock();
        fade_step(15.0f);
        Sleep(15);
    }
    return 0;
}

int xb_audio_music_scan(const char *folder);   // below

int xb_audio_music_start(const char *folder, int mode, const char *track)
{
    int i, ok;
    if (!track) track = "";
    if (!folder || !folder[0] || mode == XB_MUSIC_OFF) { music_stop_internal(); return 0; }
    if (s_musicVoice && s_musThread && !strcmp(folder, s_musicFolder) && mode == s_musicMode && (mode != XB_MUSIC_TRACK || !_stricmp(track, s_musicPick)))
        return 0;   // already playing that
    if (!s_xa || !s_master) return -1;
    music_stop_internal();
    if (!xb_audio_music_scan(folder)) { H64_INFO("[audio] music: no .mp3/.wav in %s", folder); return -1; }
    music_lock();
    strncpy(s_musicFolder, folder, sizeof(s_musicFolder) - 1);
    s_musicFolder[sizeof(s_musicFolder) - 1] = 0;
    strncpy(s_musicPick, track, sizeof(s_musicPick) - 1);
    s_musicPick[sizeof(s_musicPick) - 1] = 0;
    s_musicMode = mode;
    s_musicIndex = -1;
    if (mode == XB_MUSIC_TRACK)
    {
        for (i = 0; i < s_musicCount; i++)
            if (!_stricmp(s_musicTracks[i], track)) s_musicIndex = i;
        if (s_musicIndex < 0) s_musicMode = XB_MUSIC_ORDER;   // gone from the folder: in order instead
    }
    else if (mode == XB_MUSIC_RANDOM) srand(GetTickCount());
    for (i = 0; i < MUS_BUFS; i++)
        if (!s_musBuf[i]) s_musBuf[i] = (s16 *)malloc(MUS_FRAMES * 2 * sizeof(s16));
    ok = !music_next() && s_musicVoice;
    music_unlock();
    if (!ok) { music_stop_internal(); return -1; }
    InterlockedExchange(&s_musQuit, 0);
    s_musThread = CreateThread(NULL, 0, music_thread, NULL, 0, NULL);   // it decodes and queues from here on
    if (!s_musThread) { music_stop_internal(); return -1; }
    return 0;
}

void xb_audio_music_tick(void) {}   // the music's own thread moves on from track to track now

int xb_audio_music_skip(int dir)
{
    int r;
    music_lock();
    if (!s_musicVoice || s_musicCount < 1) { music_unlock(); return -1; }
    // this one stopped at once (its voice gone: the next opens its own), then the next or the one before
    s_musicVoice->Stop(0);
    s_musicVoice->FlushSourceBuffers();
    s_musicVoice->DestroyVoice();
    s_musicVoice = NULL;
    if (s_musicMode == XB_MUSIC_TRACK) s_musicMode = XB_MUSIC_ORDER;
    if (dir < 0 && s_musicMode == XB_MUSIC_ORDER) s_musicIndex = (s_musicIndex - 2 + 2 * s_musicCount) % s_musicCount;
    r = music_next();
    music_unlock();
    return r;
}

const char *xb_audio_music_now(void)
{
    return s_musicVoice && s_musicIndex >= 0 && s_musicIndex < s_musicCount ? s_musicTracks[s_musicIndex] : "";
}

void xb_audio_music_pause(int on)
{
    // The voice stopped keeps its queue: its thread waits, and Start goes on from the same place.
    music_lock();
    s_musicPaused = on ? 1 : 0;
    if (s_musicVoice)
    {
        if (on) s_musicVoice->Stop(0);
        else s_musicVoice->Start(0, XAUDIO2_COMMIT_NOW);
    }
    music_unlock();
}

static float volume_from_percent(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent / 100.0f;
}

// ---- The preview videos' sound ----
// Its own voice, fed by the preview's thread from a ring of PV_SLOTS buffers (the voice reads a
// buffer until it has played it: a slot is free again once BuffersQueued says so).
#define PV_SLOTS 8
#define PV_FRAMES 4096   // per slot (per channel): ~85 ms at 48 kHz
static CRITICAL_SECTION s_pvLock;
static int s_pvLockReady;
static s16 *s_pvBuf[PV_SLOTS];
static int s_pvNext, s_pvCh;

static void pv_lock(void)
{
    if (!s_pvLockReady) { InitializeCriticalSection(&s_pvLock); s_pvLockReady = 1; }
    EnterCriticalSection(&s_pvLock);
}

static void pv_unlock(void) { LeaveCriticalSection(&s_pvLock); }

static void preview_volume(void)
{
    pv_lock();
    if (s_pvVoice) s_pvVoice->SetVolume(s_musicVolume * s_pvLevel);
    pv_unlock();
}

static void music_duck(int on)
{
    InterlockedExchange(&s_musicDuck, on);   // (music_thread fades it there)
}

int xb_audio_preview_open(u32 rate, int channels)
{
    WAVEFORMATEX wf;
    int i;
    if (!s_xa || !s_master || (channels != 1 && channels != 2) || rate < 8000 || rate > 96000) return -1;
    xb_audio_preview_close();
    for (i = 0; i < PV_SLOTS; i++)
        if (!s_pvBuf[i] && !(s_pvBuf[i] = (s16 *)malloc(PV_FRAMES * 2 * sizeof(s16)))) return -1;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
    pv_lock();
    if (FAILED(s_xa->CreateSourceVoice(&s_pvVoice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)))
    {
        s_pvVoice = NULL;
        pv_unlock();
        H64_ERROR("[audio] preview: CreateSourceVoice(%u Hz, %d ch) failed", rate, channels);
        return -1;
    }
    s_pvLevel = 0;   // rising as the music goes down (fade_step)
    s_pvVoice->SetVolume(0.0f);
    apply_output_matrix(s_pvVoice, channels);
    s_pvVoice->Start(0, XAUDIO2_COMMIT_NOW);
    s_pvCh = channels;
    s_pvNext = 0;
    pv_unlock();
    music_duck(1);
    return 0;
}

int xb_audio_preview_push(const s16 *pcm, u32 frames)
{
    XAUDIO2_VOICE_STATE st;
    XAUDIO2_BUFFER b;
    int ok = 0;
    pv_lock();
    if (s_pvVoice && frames && frames <= PV_FRAMES)
    {
        s_pvVoice->GetState(&st);
        if (st.BuffersQueued < PV_SLOTS - 1)
        {
            s16 *buf = s_pvBuf[s_pvNext];
            memcpy(buf, pcm, frames * s_pvCh * sizeof(s16));
            memset(&b, 0, sizeof(b));
            b.AudioBytes = frames * s_pvCh * sizeof(s16);
            b.pAudioData = (const BYTE *)buf;
            if (SUCCEEDED(s_pvVoice->SubmitSourceBuffer(&b, NULL)))
            {
                s_pvNext = (s_pvNext + 1) % PV_SLOTS;
                ok = 1;
            }
        }
    }
    pv_unlock();
    return ok;
}

u64 xb_audio_preview_played(void)
{
    XAUDIO2_VOICE_STATE st;
    u64 n = 0;
    pv_lock();
    if (s_pvVoice)
    {
        s_pvVoice->GetState(&st);
        n = st.SamplesPlayed;
    }
    pv_unlock();
    return n;
}

void xb_audio_preview_close(void)
{
    int had;
    pv_lock();
    had = s_pvVoice != NULL;
    if (s_pvVoice)
    {
        s_pvVoice->Stop(0);
        s_pvVoice->FlushSourceBuffers();
        s_pvVoice->DestroyVoice();   // waits for the engine: the buffers are free after it
        s_pvVoice = NULL;
    }
    pv_unlock();
    if (had) music_duck(0);
}

void xb_audio_set_game_volume(int percent)
{
    s_gameVolume = volume_from_percent(percent);
    if (s_voice) s_voice->SetVolume(s_gameVolume);
}

void xb_audio_set_music_volume(int percent)
{
    s_musicVolume = volume_from_percent(percent);
    music_lock();
    if (s_musicVoice) s_musicVoice->SetVolume(music_gain());
    music_unlock();
    preview_volume();
}

void xb_audio_set_sfx_volume(int percent) { s_sfxVolume = volume_from_percent(percent); }

void xb_audio_set_mono(int on)
{
    XAUDIO2_VOICE_DETAILS d;
    s_mono = on ? 1 : 0;
    apply_output_matrix(s_voice, 2);
    music_lock();
    if (s_musicVoice)
    {
        s_musicVoice->GetVoiceDetails(&d);
        apply_output_matrix(s_musicVoice, (int)d.InputChannels);
    }
    music_unlock();
    pv_lock();
    if (s_pvVoice) apply_output_matrix(s_pvVoice, s_pvCh);
    pv_unlock();
}

// ---- Interface sounds ----
// Each one is a few sine partials with a soft attack and an exponential
// decay (a short, clean "glass" tone), mixed into a mono 48 kHz buffer once,
// the first time a sound is wanted. Native 16-bit PCM (big-endian here, as
// the game's buffers). Four voices take turns, so quick presses overlap
// instead of cutting each other.
#define SFX_RATE 48000
#define SFX_VOICES 4
#define SFX_GAIN 0.55f   // under the interface music's volume

#include "xb_laika_bark.h"
static s16 *s_sfx[XB_SFX_COUNT];
static u32 s_sfxLen[XB_SFX_COUNT];
static IXAudio2SourceVoice *s_sfxVoice[SFX_VOICES];
static int s_sfxNext, s_sfxOn = 1, s_sfxStyle, s_sfxLastVoice = -1;
static char s_sfxFolder[128];   // xb_audio_ui_folder
static const char *const s_sfxFile[XB_SFX_COUNT] = { "move", "select", "back", "action", "page", "open", "tab", "value_up", "value_down",
                                                     "fav_on", "fav_off", "launch", "save", "notify", "error", "saver", "bark", "angry" };

static DWORD s_sfxLastAt;

// Each style's pitch, length and loudness against the glass sounds'.
static const float s_styleFreq[XB_SFX_STYLE_COUNT] = { 1.0f, 0.55f, 0.75f, 1.35f };
static const float s_styleTime[XB_SFX_STYLE_COUNT] = { 1.0f, 1.6f, 1.15f, 0.55f };
static const float s_styleAmp[XB_SFX_STYLE_COUNT] = { 1.0f, 0.95f, 0.42f, 0.8f };

// Adds one partial to `buf`: from `t0` ms, `f0` gliding to `f1` Hz over `ms`,
// `amp` at most, rising in `attack` ms then dying away with time constant `tau` ms.
// The style sets its wave (sine; a softer sine; square; triangle) and scales
// its pitch, times and loudness.
static void sfx_tone(float *buf, u32 len, float t0, float ms, float f0, float f1, float amp, float attack, float tau)
{
    const float kf = s_styleFreq[s_sfxStyle], kt = s_styleTime[s_sfxStyle];
    u32 start, n, i;
    double phase = 0;
    t0 *= kt; ms *= kt; tau *= kt; f0 *= kf; f1 *= kf;
    amp *= s_styleAmp[s_sfxStyle];
    if (s_sfxStyle == XB_SFX_STYLE_SOFT) attack *= 3.0f;
    start = (u32)(t0 * SFX_RATE / 1000.0f);
    n = (u32)(ms * SFX_RATE / 1000.0f);
    for (i = 0; i < n && start + i < len; i++)
    {
        float t = i * 1000.0f / SFX_RATE, f = f0 + (f1 - f0) * (i / (float)n), w;
        float env = (t < attack ? t / attack : 1.0f) * expf(-t / tau);
        phase += 2.0 * 3.14159265358979 * f / SFX_RATE;
        w = (float)sin(phase);
        if (s_sfxStyle == XB_SFX_STYLE_RETRO) w = w >= 0 ? 1.0f : -1.0f;
        else if (s_sfxStyle == XB_SFX_STYLE_DIGITAL) w = (float)(asin(w) * 0.63661977);   // a triangle
        buf[start + i] += amp * env * w;
    }
}

// The user's own sound for `kind` (s_sfxFolder\<name>.wav): mixed down to mono, resampled to
// SFX_RATE, at most 4 s, as the made-up ones are kept. 0 when there is none.
static int sfx_load_file(int kind)
{
    char path[200];
    FILE *f;
    long size;
    u8 *data;
    WavInfo wav;
    u32 frames, outLen, i;
    s16 *out;
    _snprintf(path, sizeof(path) - 1, "%s\\%s.wav", s_sfxFolder, s_sfxFile[kind]);
    path[sizeof(path) - 1] = 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 44 || size > 4 * 1024 * 1024) { fclose(f); return 0; }
    data = (u8 *)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(data); return 0; }
    fclose(f);
    if (wav_parse(data, (u32)size, &wav) || wav.format != 1 || (wav.bits != 16 && wav.bits != 8) || !wav.channels || !wav.rate)
    {
        H64_WARN("[audio] sound %s: not a PCM WAV", path);
        free(data);
        return 0;
    }
    frames = wav.dataSize / (wav.channels * (wav.bits / 8));
    outLen = (u32)((double)frames * SFX_RATE / wav.rate);
    if (outLen > SFX_RATE * 4) outLen = SFX_RATE * 4;
    out = outLen ? (s16 *)malloc(outLen * sizeof(s16)) : NULL;
    if (!out) { free(data); return 0; }
    for (i = 0; i < outLen; i++)
    {
        // The nearest source frame (sounds this short need no better), its channels averaged.
        u32 src = (u32)((double)i * wav.rate / SFX_RATE), c;
        int sum = 0;
        if (src >= frames) src = frames - 1;
        for (c = 0; c < wav.channels; c++)
        {
            const u8 *p = data + wav.dataOffset + (src * wav.channels + c) * (wav.bits / 8);
            sum += wav.bits == 16 ? (int)(s16)(p[0] | (p[1] << 8)) : ((int)p[0] - 128) << 8;   // little-endian (RIFF)
        }
        out[i] = (s16)(sum / (int)wav.channels);
    }
    free(data);
    free(s_sfx[kind]);
    s_sfx[kind] = out;
    s_sfxLen[kind] = outLen;
    H64_INFO("[audio] sound %s: %s", s_sfxFile[kind], path);
    return 1;
}


static void sfx_make(void)
{
    static const float lengthMs[XB_SFX_COUNT] = { 40, 190, 120, 90, 80, 300, 130, 50, 50, 260, 190, 600, 330, 420, 230, 650, 10, 10 };
    int k;
    for (k = 0; k < XB_SFX_COUNT; k++)
    {
        u32 len = (u32)(lengthMs[k] * s_styleTime[s_sfxStyle] * SFX_RATE / 1000.0f), i;
        float *mix = (float *)calloc(len, sizeof(float));
        if (!mix) return;
        switch (k)
        {
        case XB_SFX_MOVE:     // a soft, high tick
            sfx_tone(mix, len, 0, 40, 2300, 2050, 0.30f, 0.8f, 7);
            sfx_tone(mix, len, 0, 40, 4600, 4100, 0.06f, 0.8f, 5);
            break;
        case XB_SFX_SELECT:   // two rising notes (C6, G6), glassy
            sfx_tone(mix, len, 0, 90, 1046.5f, 1046.5f, 0.34f, 2, 35);
            sfx_tone(mix, len, 0, 90, 3139.5f, 3139.5f, 0.05f, 2, 20);
            sfx_tone(mix, len, 45, 145, 1568.0f, 1568.0f, 0.36f, 2, 45);
            sfx_tone(mix, len, 45, 145, 4704.0f, 4704.0f, 0.05f, 2, 25);
            break;
        case XB_SFX_BACK:     // falling, a little lower and softer
            sfx_tone(mix, len, 0, 120, 880, 587, 0.32f, 2, 45);
            sfx_tone(mix, len, 0, 120, 1760, 1174, 0.06f, 2, 30);
            break;
        case XB_SFX_ACTION:   // a round "pop" (X, Y)
            sfx_tone(mix, len, 0, 90, 1318.5f, 1240, 0.32f, 1.5f, 28);
            sfx_tone(mix, len, 0, 90, 1975.5f, 1975.5f, 0.10f, 1.5f, 18);
            break;
        case XB_SFX_PAGE:     // a quick upward sweep (LB, RB)
            sfx_tone(mix, len, 0, 80, 650, 1700, 0.24f, 4, 30);
            sfx_tone(mix, len, 0, 80, 1300, 3400, 0.05f, 4, 20);
            break;
        case XB_SFX_TAB:      // a swoosh and a light second note
            sfx_tone(mix, len, 0, 70, 900, 1400, 0.22f, 3, 30);
            sfx_tone(mix, len, 45, 85, 1864.7f, 1864.7f, 0.20f, 2, 30);
            break;
        case XB_SFX_VALUE_UP:   // a short tick going up ...
            sfx_tone(mix, len, 0, 50, 1500, 1900, 0.26f, 0.8f, 14);
            break;
        case XB_SFX_VALUE_DOWN: // ... and going down
            sfx_tone(mix, len, 0, 50, 1500, 1150, 0.26f, 0.8f, 14);
            break;
        case XB_SFX_FAV_ON:   // a sparkle: three quick high notes (E6, G6, C7)
            sfx_tone(mix, len, 0, 120, 1318.5f, 1318.5f, 0.22f, 1.5f, 40);
            sfx_tone(mix, len, 40, 130, 1568.0f, 1568.0f, 0.22f, 1.5f, 45);
            sfx_tone(mix, len, 80, 180, 2093.0f, 2093.0f, 0.26f, 1.5f, 60);
            sfx_tone(mix, len, 80, 180, 4186.0f, 4186.0f, 0.04f, 1.5f, 40);
            break;
        case XB_SFX_FAV_OFF:  // two notes falling (G6, C6)
            sfx_tone(mix, len, 0, 90, 1568.0f, 1568.0f, 0.24f, 2, 35);
            sfx_tone(mix, len, 60, 130, 1046.5f, 1046.5f, 0.26f, 2, 50);
            break;
        case XB_SFX_LAUNCH:   // a rising sweep into a bright chord (C6, E6, G6)
            sfx_tone(mix, len, 0, 260, 400, 1600, 0.18f, 10, 200);
            sfx_tone(mix, len, 200, 400, 1046.5f, 1046.5f, 0.18f, 3, 140);
            sfx_tone(mix, len, 200, 400, 1318.5f, 1318.5f, 0.16f, 3, 140);
            sfx_tone(mix, len, 200, 400, 1568.0f, 1568.0f, 0.16f, 3, 150);
            sfx_tone(mix, len, 200, 300, 3136.0f, 3136.0f, 0.04f, 3, 80);
            break;
        case XB_SFX_SAVE:     // a confirmation: G5 then a longer C6 with a shimmer
            sfx_tone(mix, len, 0, 110, 784.0f, 784.0f, 0.26f, 2, 45);
            sfx_tone(mix, len, 90, 240, 1046.5f, 1046.5f, 0.30f, 2, 90);
            sfx_tone(mix, len, 90, 240, 2093.0f, 2093.0f, 0.06f, 2, 70);
            break;
        case XB_SFX_NOTIFY:   // a bell (A6 and its overtones)
            sfx_tone(mix, len, 0, 420, 1760.0f, 1760.0f, 0.24f, 1, 130);
            sfx_tone(mix, len, 0, 300, 3520.0f, 3520.0f, 0.07f, 1, 80);
            sfx_tone(mix, len, 0, 200, 5280.0f, 5280.0f, 0.03f, 1, 50);
            break;
        case XB_SFX_ERROR:    // two low, slightly sour buzzes
            sfx_tone(mix, len, 0, 90, 220, 210, 0.30f, 2, 60);
            sfx_tone(mix, len, 0, 90, 233, 222, 0.22f, 2, 60);
            sfx_tone(mix, len, 120, 110, 220, 205, 0.30f, 2, 60);
            sfx_tone(mix, len, 120, 110, 233, 217, 0.22f, 2, 60);
            break;
        case XB_SFX_SAVER:    // a soft falling pad (G5 to C5, over E5)
            sfx_tone(mix, len, 0, 650, 784, 523, 0.16f, 60, 260);
            sfx_tone(mix, len, 0, 650, 659.3f, 659.3f, 0.10f, 80, 260);
            break;
        default:              // open (START): a bright arpeggio (G5, D6, G6)
            sfx_tone(mix, len, 0, 300, 784.0f, 784.0f, 0.22f, 3, 90);
            sfx_tone(mix, len, 35, 265, 1174.7f, 1174.7f, 0.22f, 3, 90);
            sfx_tone(mix, len, 70, 230, 1568.0f, 1568.0f, 0.24f, 3, 100);
            sfx_tone(mix, len, 70, 230, 3136.0f, 3136.0f, 0.04f, 3, 60);
            break;
        }
        if (k == XB_SFX_BARK || k == XB_SFX_ANGRY)
        {
            // Laika's bark, or her angry growl and barks: her recordings (in the executable: there after an update
            // from the console too)
            const short *pcm = k == XB_SFX_BARK ? s_laikaBark : s_laikaAngry;
            const u32 n = k == XB_SFX_BARK ? LAIKA_BARK_LEN : LAIKA_ANGRY_LEN;
            free(mix);
            s_sfx[k] = (s16 *)malloc(n * sizeof(s16));
            if (s_sfx[k]) memcpy(s_sfx[k], pcm, n * sizeof(s16));
            s_sfxLen[k] = s_sfx[k] ? n : 0;
            if (s_sfxFolder[0]) sfx_load_file(k);   // the user's own bark.wav, when there is one
            continue;
        }
        s_sfx[k] = (s16 *)malloc(len * sizeof(s16));
        if (s_sfx[k])
            for (i = 0; i < len; i++)
            {
                float v = mix[i];
                if (i + 48 > len) v *= (len - i) / 48.0f;   // no click at the very end
                v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
                s_sfx[k][i] = (s16)(v * 32000.0f);
            }
        s_sfxLen[k] = s_sfx[k] ? len : 0;
        free(mix);
        if (s_sfxFolder[0]) sfx_load_file(k);   // the user's own, when there is one
    }
}

static void sfx_stop_internal(void)
{
    int i;
    for (i = 0; i < SFX_VOICES; i++)
        if (s_sfxVoice[i])
        {
            s_sfxVoice[i]->Stop(0);
            s_sfxVoice[i]->FlushSourceBuffers();
            s_sfxVoice[i]->DestroyVoice();
            s_sfxVoice[i] = NULL;
        }
}

void xb_audio_ui_enable(int on) { s_sfxOn = on ? 1 : 0; }

void xb_audio_ui_folder(const char *folder)
{
    int k;
    if (!folder) folder = "";
    if (!strcmp(folder, s_sfxFolder)) return;
    strncpy(s_sfxFolder, folder, sizeof(s_sfxFolder) - 1);
    s_sfxFolder[sizeof(s_sfxFolder) - 1] = 0;
    sfx_stop_internal();   // made again at the next sound, with the folder's files
    for (k = 0; k < XB_SFX_COUNT; k++)
    {
        free(s_sfx[k]);
        s_sfx[k] = NULL;
        s_sfxLen[k] = 0;
    }
    s_sfxLastVoice = -1;
}

void xb_audio_ui_style(int style)
{
    int k;
    if (style < 0 || style >= XB_SFX_STYLE_COUNT) style = 0;
    if (style == s_sfxStyle && s_sfx[0]) return;
    sfx_stop_internal();   // nothing may still play the buffers about to go
    for (k = 0; k < XB_SFX_COUNT; k++)
    {
        free(s_sfx[k]);
        s_sfx[k] = NULL;
        s_sfxLen[k] = 0;
    }
    s_sfxStyle = style;
    s_sfxLastVoice = -1;
}

static void sfx_play(int kind, int instead)
{
    IXAudio2SourceVoice **v;
    XAUDIO2_BUFFER b;
    if (!s_sfxOn || !s_xa || !s_master || kind < 0 || kind >= XB_SFX_COUNT || s_sfxVolume <= 0.0f) return;
    if (!s_sfx[0]) sfx_make();
    if (!s_sfx[kind]) return;
    if (instead && s_sfxLastVoice >= 0 && GetTickCount() - s_sfxLastAt < 80) v = &s_sfxVoice[s_sfxLastVoice];   // over the press's own sound
    else
    {
        s_sfxLastVoice = s_sfxNext;
        v = &s_sfxVoice[s_sfxNext];
        s_sfxNext = (s_sfxNext + 1) % SFX_VOICES;
    }
    s_sfxLastAt = GetTickCount();
    if (!*v)
    {
        WAVEFORMATEX wf;
        memset(&wf, 0, sizeof(wf));
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = SFX_RATE;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2;
        wf.nAvgBytesPerSec = SFX_RATE * 2;
        if (FAILED(s_xa->CreateSourceVoice(v, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL))) { *v = NULL; return; }
    }
    (*v)->Stop(0);
    (*v)->FlushSourceBuffers();
    memset(&b, 0, sizeof(b));
    b.AudioBytes = s_sfxLen[kind] * 2;
    b.pAudioData = (const BYTE *)s_sfx[kind];
    b.Flags = XAUDIO2_END_OF_STREAM;
    (*v)->SetVolume(SFX_GAIN * s_sfxVolume);
    if (SUCCEEDED((*v)->SubmitSourceBuffer(&b, NULL))) (*v)->Start(0, XAUDIO2_COMMIT_NOW);
}

void xb_audio_ui_sound(int kind) { sfx_play(kind, 0); }

void xb_audio_ui_sound_instead(int kind) { sfx_play(kind, 1); }

// ---- The online intro's sound (xb_menu.cpp online_intro): data going down a line ----
// Made here as the interface sounds are (sfx_tone, in the glass style whatever the one chosen), 4.3 s timed to
// the tunnel: a rush of noise opening up and a carrier humming higher as it speeds up, short beeps of data
// quicker and quicker, then at 2.2 s (the logo in front) a low thump and a bright chord ringing out; from 3 s
// (the way out) the rush swelling again with beeps racing by, to 4 s (into the light: online play's screen) and
// a soft chord there.
#define INTRO_MS 4900
#define INTRO_LANDS 2.2f
#define INTRO_EXIT 3.0f
#define INTRO_LIGHT 4.0f
static IXAudio2SourceVoice *s_introVoice;
static s16 *s_introPcm;

static u32 intro_rand(u32 *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return *seed >> 8;
}

static u32 intro_make(void)
{
    static const float notes[8] = { 1046.5f, 1318.5f, 1568.0f, 1760.0f, 2093.0f, 2349.3f, 2637.0f, 3136.0f };
    const u32 len = INTRO_MS * SFX_RATE / 1000;
    const float pi2 = 6.2831853f;
    const int keepStyle = s_sfxStyle;
    float *mix = (float *)calloc(len, sizeof(float)), lp1 = 0, lp2 = 0, a1 = 0, a2 = 0, t;
    double hum = 0;
    u32 i, seed = 64;
    if (!mix) return 0;
    for (i = 0; i < len; i++)
    {
        const float ti = i / (float)SFX_RATE, open = ti < INTRO_LANDS ? ti / INTRO_LANDS : 1.0f;
        float n, env, henv;
        if (!(i & 63))
        {
            // The rush's band (between two one-pole low-passes), opening from 300 Hz to 3.8 kHz.
            const float fc = 300.0f + 3500.0f * open * open;
            a1 = 1.0f - expf(-pi2 * fc / SFX_RATE);
            a2 = 1.0f - expf(-pi2 * fc * 0.25f / SFX_RATE);
        }
        n = (intro_rand(&seed) & 0xFFFF) / 32768.0f - 1.0f;
        lp1 += a1 * (n - lp1);
        lp2 += a2 * (n - lp2);
        env = ti < INTRO_LANDS ? 0.05f + 0.25f * open * open : 0.30f * expf(-(ti - INTRO_LANDS) / 0.35f);
        if (ti > INTRO_EXIT)
        {
            // The way out: swelling into the light, then breathing out.
            const float e = (ti - INTRO_EXIT) / (INTRO_LIGHT - INTRO_EXIT);
            env += ti < INTRO_LIGHT ? 0.34f * e * e : 0.34f * expf(-(ti - INTRO_LIGHT) / 0.2f);
        }
        mix[i] += (lp1 - lp2) * env * 1.6f;
        // The carrier: 55 Hz rising to 110, pulsing eight times a second.
        hum += pi2 * (55.0f + 55.0f * open) / SFX_RATE;
        henv = (ti < 0.3f ? ti / 0.3f : 1.0f) * (ti < INTRO_LANDS + 0.1f ? 1.0f : expf(-(ti - INTRO_LANDS - 0.1f) / 0.5f));
        mix[i] += 0.10f * henv * (float)sin(hum) * (0.75f + 0.25f * sinf(pi2 * 8.0f * ti));
    }
    s_sfxStyle = XB_SFX_STYLE_GLASS;
    for (t = 0.12f; t < 3.6f;)
    {
        // The data: beeps of 12 to 32 ms, closer and closer until the logo lands, then thinning out.
        const float d = 0.012f + (intro_rand(&seed) % 20) / 1000.0f, f = notes[intro_rand(&seed) % 8];
        const float gap = t < INTRO_LANDS ? 0.075f - 0.05f * (t / INTRO_LANDS) : 0.03f + (t - INTRO_LANDS) * 0.12f;
        const float amp = t < INTRO_LANDS ? 0.05f + 0.06f * (t / INTRO_LANDS) : 0.11f * expf(-(t - INTRO_LANDS) / 0.5f);
        sfx_tone(mix, len, t * 1000.0f, d * 1000.0f, f, f * ((intro_rand(&seed) & 1) ? 1.06f : 0.97f), amp, 0.8f, d * 600.0f);
        t += d + gap * (0.5f + (intro_rand(&seed) % 100) / 100.0f);
    }
    // Locked on: the thump and the chord (C6 E6 G6, C7 and a shimmer over them).
    sfx_tone(mix, len, INTRO_LANDS * 1000.0f, 400, 140, 45, 0.45f, 2, 110);
    sfx_tone(mix, len, INTRO_LANDS * 1000.0f, 1900, 1046.5f, 1046.5f, 0.14f, 4, 700);
    sfx_tone(mix, len, INTRO_LANDS * 1000.0f + 30, 1870, 1318.5f, 1318.5f, 0.12f, 4, 700);
    sfx_tone(mix, len, INTRO_LANDS * 1000.0f + 60, 1840, 1568.0f, 1568.0f, 0.12f, 4, 750);
    sfx_tone(mix, len, INTRO_LANDS * 1000.0f + 90, 1500, 2093.0f, 2093.0f, 0.06f, 4, 600);
    sfx_tone(mix, len, INTRO_LANDS * 1000.0f + 90, 1200, 4186.0f, 4180.0f, 0.02f, 4, 400);
    for (t = INTRO_EXIT + 0.03f; t < INTRO_LIGHT;)
    {
        // The way out's beeps, racing by.
        const float d = 0.010f + (intro_rand(&seed) % 15) / 1000.0f, f = notes[intro_rand(&seed) % 8], e = (t - INTRO_EXIT) / (INTRO_LIGHT - INTRO_EXIT);
        sfx_tone(mix, len, t * 1000.0f, d * 1000.0f, f, f * 1.08f, 0.04f + 0.05f * e, 0.8f, d * 600.0f);
        t += d + (0.05f - 0.03f * e) * (0.5f + (intro_rand(&seed) % 100) / 100.0f);
    }
    // Into the light: a soft chord (G5 C6 E6).
    sfx_tone(mix, len, INTRO_LIGHT * 1000.0f, 700, 784.0f, 784.0f, 0.08f, 4, 500);
    sfx_tone(mix, len, INTRO_LIGHT * 1000.0f + 25, 680, 1046.5f, 1046.5f, 0.07f, 4, 500);
    sfx_tone(mix, len, INTRO_LIGHT * 1000.0f + 50, 660, 1318.5f, 1318.5f, 0.06f, 4, 500);
    s_sfxStyle = keepStyle;
    s_introPcm = (s16 *)malloc(len * sizeof(s16));
    if (s_introPcm)
        for (i = 0; i < len; i++)
        {
            float v = mix[i];
            if (i + 2400 > len) v *= (len - i) / 2400.0f;   // the last 50 ms fade out
            v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            s_introPcm[i] = (s16)(v * 32000.0f);
        }
    free(mix);
    return s_introPcm ? len : 0;
}

// Leaving online play (online_intro's way out, 1.6 s): the rush already fast and opening further, beeps of data
// racing by, swelling to 1.15 s (out into the light) and breathing out with a soft chord (G5 C6 E6).
#define OUTRO_MS 1800
#define OUTRO_OUT 1.15f
static u32 outro_make(void)
{
    static const float notes[8] = { 1046.5f, 1318.5f, 1568.0f, 1760.0f, 2093.0f, 2349.3f, 2637.0f, 3136.0f };
    const u32 len = OUTRO_MS * SFX_RATE / 1000;
    const float pi2 = 6.2831853f;
    const int keepStyle = s_sfxStyle;
    float *mix = (float *)calloc(len, sizeof(float)), lp1 = 0, lp2 = 0, a1 = 0, a2 = 0, t;
    double hum = 0;
    u32 i, seed = 46;
    if (!mix) return 0;
    for (i = 0; i < len; i++)
    {
        const float ti = i / (float)SFX_RATE, open = ti < OUTRO_OUT ? ti / OUTRO_OUT : 1.0f;
        float n, env, henv;
        if (!(i & 63))
        {
            const float fc = 1500.0f + 5000.0f * open * open;
            a1 = 1.0f - expf(-pi2 * fc / SFX_RATE);
            a2 = 1.0f - expf(-pi2 * fc * 0.25f / SFX_RATE);
        }
        n = (intro_rand(&seed) & 0xFFFF) / 32768.0f - 1.0f;
        lp1 += a1 * (n - lp1);
        lp2 += a2 * (n - lp2);
        env = ti < OUTRO_OUT ? 0.12f + 0.26f * open * open : 0.38f * expf(-(ti - OUTRO_OUT) / 0.22f);
        mix[i] += (lp1 - lp2) * env * 1.6f;
        hum += pi2 * (110.0f + 110.0f * open) / SFX_RATE;
        henv = (ti < 0.1f ? ti / 0.1f : 1.0f) * (ti < OUTRO_OUT ? 1.0f : expf(-(ti - OUTRO_OUT) / 0.15f));
        mix[i] += 0.08f * henv * (float)sin(hum) * (0.75f + 0.25f * sinf(pi2 * 12.0f * ti));
    }
    s_sfxStyle = XB_SFX_STYLE_GLASS;
    for (t = 0.03f; t < OUTRO_OUT;)
    {
        const float d = 0.010f + (intro_rand(&seed) % 15) / 1000.0f, f = notes[intro_rand(&seed) % 8];
        sfx_tone(mix, len, t * 1000.0f, d * 1000.0f, f, f * 1.08f, 0.05f + 0.05f * (t / OUTRO_OUT), 0.8f, d * 600.0f);
        t += d + 0.03f * (0.5f + (intro_rand(&seed) % 100) / 100.0f);
    }
    sfx_tone(mix, len, OUTRO_OUT * 1000.0f, 600, 784.0f, 784.0f, 0.08f, 4, 450);
    sfx_tone(mix, len, OUTRO_OUT * 1000.0f + 25, 580, 1046.5f, 1046.5f, 0.07f, 4, 450);
    sfx_tone(mix, len, OUTRO_OUT * 1000.0f + 50, 560, 1318.5f, 1318.5f, 0.06f, 4, 450);
    s_sfxStyle = keepStyle;
    s_introPcm = (s16 *)malloc(len * sizeof(s16));
    if (s_introPcm)
        for (i = 0; i < len; i++)
        {
            float v = mix[i];
            if (i + 2400 > len) v *= (len - i) / 2400.0f;
            v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            s_introPcm[i] = (s16)(v * 32000.0f);
        }
    free(mix);
    return s_introPcm ? len : 0;
}

void xb_audio_online_intro(int on)
{
    if (s_introVoice)
    {
        s_introVoice->Stop(0);
        s_introVoice->FlushSourceBuffers();
        s_introVoice->DestroyVoice();
        s_introVoice = NULL;
    }
    free(s_introPcm);
    s_introPcm = NULL;
    if (on && s_xa && s_master && s_sfxVolume > 0.0f)
    {
        WAVEFORMATEX wf;
        XAUDIO2_BUFFER b;
        const u32 len = on == 2 ? outro_make() : intro_make();
        if (!len) return;
        memset(&wf, 0, sizeof(wf));
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = SFX_RATE;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2;
        wf.nAvgBytesPerSec = SFX_RATE * 2;
        if (FAILED(s_xa->CreateSourceVoice(&s_introVoice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL))) { s_introVoice = NULL; return; }
        memset(&b, 0, sizeof(b));
        b.AudioBytes = len * 2;
        b.pAudioData = (const BYTE *)s_introPcm;
        b.Flags = XAUDIO2_END_OF_STREAM;
        s_introVoice->SetVolume(SFX_GAIN * 1.3f * s_sfxVolume);
        if (SUCCEEDED(s_introVoice->SubmitSourceBuffer(&b, NULL))) s_introVoice->Start(0, XAUDIO2_COMMIT_NOW);
    }
}
