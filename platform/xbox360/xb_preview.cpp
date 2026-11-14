// LAIKA 64 - preview videos in the ROM list (see xb_preview.h).

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../../core/common/h64_log.h"
#include "xb_preview.h"
#include "xb_video.h"
#include "xb_audio.h"
#include "xb_art.h"
#include "xb_net.h"
#include "xb_ui.h"
#ifdef HAVE_SS_SECRET
#include "xb_ss_secret.h"
#endif
#include "../../third_party/faad2/include/neaacdec.h"
#include "../../third_party/minimp3/minimp3.h"   // implemented in xb_audio.cpp (the menu music)
#include <string>
#include <vector>
#include <set>

#define SS_HOST "api.screenscraper.fr"
#define SS_SYSTEM_N64 14
#define SS_RETRY_DAYS 14          // a game ScreenScraper had no video of: asked again after this long
#define PV_MAX_W 320              // the pictures as the card shows them (it draws them 280 x 210)
#define PV_MAX_H 240
#define PV_BEAT_MS 500            // the list stopped asking for the picture this long ago: stop
#define PV_AUDIO_FRAMES 4096      // PCM frames handed to the voice at a time (xb_audio's slot)

// ---- Shared with the list's thread (under s_lock) ----
static CRITICAL_SECTION s_lock;
static HANDLE s_thread, s_wake;
static int s_started;
static char s_wantPath[256], s_wantTitle[128];
static volatile LONG s_wantSeq;          // changes with every new wish: what runs stops
static int s_enabled = 1, s_sound = 1, s_netOk;
static volatile DWORD s_beat;            // the list's last PreviewTexture call (GetTickCount)
static volatile LONG s_idle = 1;         // nothing open (no video, no sound, no download)
static volatile long s_cancel;           // stops a download for a wish
static volatile long s_bulkCancel;       // stops the bulk pass's download (a game starts)
static volatile LONG s_stopAll;          // PreviewStop: nothing goes on until the list is back
static std::vector<std::string> s_bulk;  // the bulk pass: every game's video (under s_lock)
static volatile LONG s_bulkNext, s_bulkTotal;
// The games settled (lower-case ROM paths): their video is there, or ScreenScraper had none lately.
// Kept in <art>\video\done.txt ("v|path", "n<day>|path"): the bulk pass leaves them out, so it (and
// its bar) only runs for games that still need it - not over every game each time the list opens.
static std::set<std::string> s_settled;  // under s_lock
static int s_settledLoaded;
static u8 *s_shown;                      // the newest picture (ARGB bytes, s_shownW x s_shownH)
static int s_shownW, s_shownH, s_shownSeq;
static char s_shownPath[256];            // the ROM it belongs to ("" when none)
static DWORD s_ssPauseUntil;             // ScreenScraper said "too many": no requests until then
// The user's own ScreenScraper account (Settings; optional): its quota and speed instead of the anonymous
// ones (128 KB/s). Refused once (403): the anonymous access from then on, until it is set again.
static char s_ssUser[64], s_ssPass[64];  // under s_lock
static volatile LONG s_ssUserBad;
static volatile LONG s_acctCheck;        // PreviewAccountCheck asked
static volatile LONG s_acctState;        // PREVIEW_ACCOUNT_*
static char s_acctLevel[16], s_acctSpeed[16], s_acctToday[16], s_acctMax[16];   // under s_lock

// ---- The list's thread only: the textures the pictures go into ----
struct PvTex { IDirect3DTexture9 *tex; DWORD fence; int w, h; };
static PvTex s_tex[2];
static int s_texLast = -1, s_texSeq = -1;
static char s_texPath[256];
static float s_texSince;

static int want_changed(LONG seq) { return s_wantSeq != seq; }
// Whether a download should stop: one for a wish (seq >= 0) when the wish changes, the bulk pass's
// (seq < 0) only when a game starts.
static int aborted(LONG seq) { return seq < 0 ? s_stopAll != 0 : want_changed(seq); }

// ---- ScreenScraper ----

static int ss_secret(char *id, char *pass)
{
#ifdef HAVE_SS_SECRET
    int i;
    for (i = 0; i < 63; i++)
        if (!(id[i] = (char)(s_ssDevId[i] ^ SS_SECRET_KEY ^ (i * 29 & 0xFF)))) break;
    id[63] = 0;
    for (i = 0; i < 63; i++)
        if (!(pass[i] = (char)(s_ssDevPass[i] ^ SS_SECRET_KEY ^ (i * 29 & 0xFF)))) break;
    pass[63] = 0;
    return id[0] && pass[0];
#else
    (void)id;
    (void)pass;
    return 0;   // a build without the developer credentials: no downloads
#endif
}

static void url_put(char *out, size_t size, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(out);
    for (; *text && n + 4 < size; text++)
    {
        unsigned char ch = (unsigned char)*text;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')
            out[n++] = (char)ch;
        else
        {
            out[n++] = '%';
            out[n++] = hex[ch >> 4];
            out[n++] = hex[ch & 15];
        }
    }
    out[n] = 0;
}

// "/api2/<page>?devid=...&devpassword=...&softname=laika64&output=json" and what follows.
static int ss_path(char *out, size_t size, const char *page)
{
    char id[64], pass[64];
    if (!ss_secret(id, pass)) return 0;
    _snprintf(out, size - 1, "/api2/%s?devid=", page);
    out[size - 1] = 0;
    url_put(out, size, id);
    strncat(out, "&devpassword=", size - strlen(out) - 1);
    url_put(out, size, pass);
    strncat(out, "&softname=laika64&output=json", size - strlen(out) - 1);
    memset(id, 0, sizeof(id));
    memset(pass, 0, sizeof(pass));
    EnterCriticalSection(&s_lock);
    if (s_ssUser[0] && !s_ssUserBad)
    {
        strncat(out, "&ssid=", size - strlen(out) - 1);
        url_put(out, size, s_ssUser);
        strncat(out, "&sspassword=", size - strlen(out) - 1);
        url_put(out, size, s_ssPass);
    }
    LeaveCriticalSection(&s_lock);
    return 1;
}

// An HTTP status that means "not now": too many requests, quota, the API closed. 1 when so.
static int ss_busy(int status)
{
    if (status == 403 && s_ssUser[0] && !s_ssUserBad)
    {
        // The user's account refused (a wrong name or password): anonymous from now on.
        InterlockedExchange(&s_ssUserBad, 1);
        InterlockedExchange(&s_acctState, PREVIEW_ACCOUNT_BAD);
        H64_WARN("[preview] ScreenScraper refused the account: anonymous access from now on");
        return 1;
    }
    if (status == 401 || status == 403 || status == 423 || status == 426 || status == 429 || status == 430 || status == 431)
    {
        s_ssPauseUntil = GetTickCount() + 10 * 60 * 1000;
        H64_WARN("[preview] ScreenScraper answered %d: no more requests for 10 minutes", status);
        return 1;
    }
    return 0;
}

// The game's ScreenScraper id in an answer: the first "id" after `after` ("\"jeu\"", "\"jeux\""). 0: none.
static u32 ss_game_id(const char *body, const char *after)
{
    const char *p = strstr(body, after), *id;
    // Its first field ("jeu": {"id": "5396", ...}): an "id" further on belongs to something else.
    if (!p || !(id = strstr(p, "\"id\"")) || id - p > 24 || !(p = strchr(id + 4, ':'))) return 0;
    p++;
    while (*p == ' ' || *p == '"') p++;
    return (u32)strtoul(p, NULL, 10);
}

// The ROM looked up on ScreenScraper: by its file name and size, else by its title. Its id (0: not found,
// -1: no answer - no network, or ScreenScraper busy).
static long ss_lookup(const char *romPath, const char *title, LONG seq)
{
    char path[1024], size[16];
    const char *name = strrchr(romPath, '\\');
    WIN32_FILE_ATTRIBUTE_DATA fa;
    u8 *body = NULL;
    u32 len = 0, id = 0;
    int status, pass;
    name = name ? name + 1 : romPath;
    size[0] = 0;
    if (GetFileAttributesExA(romPath, GetFileExInfoStandard, &fa)) sprintf(size, "%lu", fa.nFileSizeLow);
    for (pass = 0; pass < 2 && !id; pass++)
    {
        if (pass == 1 && (!title || !title[0])) break;
        if (aborted(seq) || (s_ssPauseUntil && (int)(s_ssPauseUntil - GetTickCount()) > 0)) return -1;
        if (!ss_path(path, sizeof(path), pass ? "jeuRecherche.php" : "jeuInfos.php")) return -1;
        if (pass == 0)
        {
            sprintf(path + strlen(path), "&systemeid=%d&romtype=rom&romnom=", SS_SYSTEM_N64);
            url_put(path, sizeof(path), name);
            if (size[0]) { strncat(path, "&romtaille=", sizeof(path) - strlen(path) - 1); strncat(path, size, sizeof(path) - strlen(path) - 1); }
        }
        else
        {
            sprintf(path + strlen(path), "&systemeid=%d&recherche=", SS_SYSTEM_N64);
            url_put(path, sizeof(path), title);
        }
        status = NetGet(SS_HOST, path, 1, &body, &len, 2 * 1024 * 1024, NULL, 0);
        memset(path, 0, sizeof(path));
        if (!status) { free(body); return -1; }
        if (ss_busy(status)) { free(body); return -1; }
        if (status == 200 && body) id = ss_game_id((const char *)body, pass ? "\"jeux\"" : "\"jeu\"");
        H64_INFO("[preview] ScreenScraper %s \"%s\": HTTP %d, game %u", pass ? "search" : "file", pass ? title : name, status, id);
        free(body);
        body = NULL;
    }
    return (long)id;
}

// ScreenScraper's video of game `id` into `out` (through a .part file). 1 when it is there.
static int ss_download(u32 id, const char *out, LONG seq)
{
    static const char *const kinds[2] = { "video-normalized", "video" };
    char path[512], part[300];
    int k;
    _snprintf(part, sizeof(part) - 1, "%s.part", out);
    part[sizeof(part) - 1] = 0;
    for (k = 0; k < 2; k++)
    {
        FILE *f;
        u32 len = 0;
        int status, ok = 0;
        u8 head[8];
        if (aborted(seq) || !ss_path(path, sizeof(path), "mediaVideoJeu.php")) return 0;
        sprintf(path + strlen(path), "&systemeid=%d&jeuid=%u&media=%s", SS_SYSTEM_N64, id, kinds[k]);
        if (!(f = fopen(part, "wb"))) return 0;
        if (seq >= 0) InterlockedExchange(&s_cancel, 0);
        status = NetDownload(SS_HOST, path, f, &len, NULL, seq >= 0 ? &s_cancel : &s_bulkCancel);
        memset(path, 0, sizeof(path));
        fclose(f);
        if (status == 200 && len > 1024 && (f = fopen(part, "rb")) != NULL)
        {
            ok = fread(head, 1, 8, f) == 8 && !memcmp(head + 4, "ftyp", 4);   // an MP4 (else a text: no such media)
            fclose(f);
        }
        H64_INFO("[preview] video %s of game %u: HTTP %d, %u bytes%s", kinds[k], id, status, len, ok ? "" : " (not a video)");
        if (ok)
        {
            DeleteFileA(out);
            if (MoveFileA(part, out)) return 1;
        }
        DeleteFileA(part);
        if (!status || ss_busy(status)) return 0;
    }
    return 0;
}

// What is known of a game on ScreenScraper (<key>.ss beside its video): "id=N", "none=<day>" when it
// had none (day: days since 1601, from the system time), nothing when never asked.
static void ss_note_read(const char *file, long *id, u32 *noneDay)
{
    FILE *f = fopen(file, "rb");
    char line[64];
    *id = -1;
    *noneDay = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f))
    {
        if (!strncmp(line, "id=", 3)) *id = atol(line + 3);
        if (!strncmp(line, "none=", 5)) *noneDay = (u32)strtoul(line + 5, NULL, 10);
    }
    fclose(f);
}

static u32 today(void)
{
    FILETIME ft;
    ULARGE_INTEGER t;
    GetSystemTimeAsFileTime(&ft);
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return (u32)(t.QuadPart / (10000000ull * 86400ull));
}

static void ss_note_write(const char *file, long id, int none)
{
    FILE *f = fopen(file, "wb");
    if (!f) return;
    if (id > 0) fprintf(f, "id=%ld\n", id);
    if (none) fprintf(f, "none=%u\n", today());
    fclose(f);
}

// The game's video file, downloaded first when it is not there yet (and may be). 1 when it is there,
// -1 when ScreenScraper has none (lately), 0 otherwise (no network, not identified yet, stopped).
static int video_get(const char *romPath, const char *title, char *file, size_t size, LONG seq)
{
    char key[16], dir[96], note[128];
    long id;
    u32 noneDay;
    if (!ArtKeyOf(romPath, key)) return 0;
    _snprintf(dir, sizeof(dir) - 1, "%s\\video", ArtFolder());
    dir[sizeof(dir) - 1] = 0;
    _snprintf(file, size - 1, "%s\\%s.mp4", dir, key);
    file[size - 1] = 0;
    if (GetFileAttributesA(file) != 0xFFFFFFFF) return 1;
    if (!s_netOk) return 0;
    _snprintf(note, sizeof(note) - 1, "%s\\%s.ss", dir, key);
    note[sizeof(note) - 1] = 0;
    ss_note_read(note, &id, &noneDay);
    if (noneDay && today() - noneDay < SS_RETRY_DAYS) return -1;   // asked not long ago: none
    if (!NetStart(0)) return 0;
    CreateDirectoryA(dir, NULL);
    if (id <= 0)
    {
        id = ss_lookup(romPath, title, seq);
        if (id < 0) return 0;   // no answer: asked again next time
        if (!id)
        {
            // Not found: noted (not asked again for a while) once its title was tried too; a game not
            // identified yet (no title) is asked again later.
            if (title && title[0]) { ss_note_write(note, 0, 1); return -1; }
            return 0;
        }
        ss_note_write(note, id, 0);
    }
    if (ss_download((u32)id, file, seq)) return 1;
    if (!aborted(seq) && !(seq >= 0 ? s_cancel : s_bulkCancel) && !(s_ssPauseUntil && (int)(s_ssPauseUntil - GetTickCount()) > 0))
    {
        ss_note_write(note, id, 1);   // the game is there, its video is not
        return -1;
    }
    return 0;
}

// ---- Playing ----

// A picture (I420) halved (or as it is when small) into ARGB bytes: BT.601, limited range.
static void yuv_to_argb(const VideoPicture *p, u8 *out, int ow, int oh)
{
    int step = p->width > PV_MAX_W ? 2 : 1, x, y;
    for (y = 0; y < oh; y++)
    {
        const u8 *ys = p->y + y * step * p->strideY, *us = p->u + (y * step / 2) * p->strideUV, *vs = p->v + (y * step / 2) * p->strideUV;
        u8 *d = out + (size_t)y * ow * 4;
        for (x = 0; x < ow; x++, d += 4)
        {
            int c = (step == 2 ? (ys[2 * x] + ys[2 * x + 1] + ys[2 * x + p->strideY] + ys[2 * x + 1 + p->strideY] + 2) >> 2 : ys[x]) - 16;
            int u = us[x * step / 2] - 128, v = vs[x * step / 2] - 128, r, g, b;
            c *= 298;
            r = (c + 409 * v + 128) >> 8;
            g = (c - 100 * u - 208 * v + 128) >> 8;
            b = (c + 516 * u + 128) >> 8;
            d[0] = 255;
            d[1] = (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
            d[2] = (u8)(g < 0 ? 0 : g > 255 ? 255 : g);
            d[3] = (u8)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
    }
}

// The newest picture handed to the list (swapped with the thread's own buffer).
static void picture_publish(u8 **buf, int w, int h, const char *romPath)
{
    u8 *keep;
    EnterCriticalSection(&s_lock);
    keep = s_shown;
    s_shown = *buf;
    *buf = keep;
    s_shownW = w;
    s_shownH = h;
    s_shownSeq++;
    strncpy(s_shownPath, romPath, sizeof(s_shownPath) - 1);
    LeaveCriticalSection(&s_lock);
}

static void picture_forget(void)
{
    EnterCriticalSection(&s_lock);
    s_shownPath[0] = 0;
    LeaveCriticalSection(&s_lock);
}

// Whether the list still wants this video: the same wish, enabled, and still asking for its picture.
static int keep_playing(LONG seq)
{
    return !want_changed(seq) && s_enabled && GetTickCount() - s_beat < PV_BEAT_MS;
}

struct PvSound
{
    int codec;            // VIDEO_AUDIO_* (NONE: no sound open)
    NeAACDecHandle aac;
    mp3dec_t mp3;
    u32 rate, next;       // sample rate; the next packet
    int ch;
    s16 *pcm;             // decoded and not handed to the voice yet (room for PV_AUDIO_FRAMES * 2)
    u32 have;             // frames in pcm
    int ended;            // the track's last packet decoded
    u64 pushed;           // frames handed to the voice since it opened
};

// Decoded PCM (`channels` interleaved) added to s->pcm in the voice's channel count.
static void sound_add(PvSound *s, const s16 *out, u32 frames, int channels)
{
    u32 i;
    if (!frames || !channels || s->have + frames > PV_AUDIO_FRAMES * 2) return;
    if (channels == s->ch) memcpy(s->pcm + s->have * s->ch, out, frames * s->ch * sizeof(s16));
    else
        for (i = 0; i < frames; i++)
        {
            // Mono into stereo, or the first two of more.
            s->pcm[(s->have + i) * s->ch] = out[i * channels];
            if (s->ch == 2) s->pcm[(s->have + i) * 2 + 1] = out[i * channels + (channels > 1 ? 1 : 0)];
        }
    s->have += frames;
}

// The next packet decoded into s->pcm; 0 past the track's end.
static int sound_packet(XbVideo *v, PvSound *s)
{
    const u8 *data;
    u32 size;
    if (!VideoAudioPacket(v, s->next, &data, &size)) return 0;
    s->next++;
    if (s->codec == VIDEO_AUDIO_AAC)
    {
        NeAACDecFrameInfo info;
        s16 *out;
        memset(&info, 0, sizeof(info));
        out = (s16 *)NeAACDecDecode(s->aac, &info, (unsigned char *)data, size);
        if (!info.error && out && info.channels) sound_add(s, out, info.samples / info.channels, info.channels);
    }
    else
    {
        // MP3: the packet may hold more than one frame. minimp3 writes native int16 (big-endian here).
        static mp3d_sample_t out[MINIMP3_MAX_SAMPLES_PER_FRAME];
        while (size)
        {
            mp3dec_frame_info_t info;
            int n = mp3dec_decode_frame(&s->mp3, data, (int)size, out, &info);
            if (!info.frame_bytes) break;
            data += info.frame_bytes;
            size -= info.frame_bytes > (int)size ? size : (u32)info.frame_bytes;
            if (n > 0)
            {
                if (!s->rate) { s->rate = (u32)info.hz; s->ch = info.channels > 1 ? 2 : 1; }
                sound_add(s, out, (u32)n, info.channels);
            }
        }
    }
    return 1;
}

static void sound_free(PvSound *s)
{
    if (s->aac) NeAACDecClose(s->aac);
    free(s->pcm);
    s->aac = NULL;
    s->pcm = NULL;
    s->codec = VIDEO_AUDIO_NONE;
}

static int sound_open(XbVideo *v, PvSound *s)
{
    memset(s, 0, sizeof(*s));
    s->codec = VideoAudioCodec(v);
    if (s->codec == VIDEO_AUDIO_NONE) return 0;
    if (!(s->pcm = (s16 *)malloc(PV_AUDIO_FRAMES * 2 * 2 * sizeof(s16)))) { s->codec = VIDEO_AUDIO_NONE; return 0; }
    if (s->codec == VIDEO_AUDIO_AAC)
    {
        const u8 *asc;
        u32 ascSize;
        unsigned long rate = 0;
        unsigned char ch = 0;
        NeAACDecConfigurationPtr cfg;
        if (!VideoAudioConfig(v, &asc, &ascSize) || !(s->aac = NeAACDecOpen())) { sound_free(s); return 0; }
        cfg = NeAACDecGetCurrentConfiguration(s->aac);
        cfg->outputFormat = FAAD_FMT_16BIT;
        cfg->downMatrix = 1;   // 5.1 down to two channels
        NeAACDecSetConfiguration(s->aac, cfg);
        if (NeAACDecInit2(s->aac, (unsigned char *)asc, ascSize, &rate, &ch) < 0 || !rate || !ch)
        {
            H64_WARN("[preview] the AAC sound cannot be decoded (config of %u bytes)", ascSize);
            sound_free(s);
            return 0;
        }
        s->rate = (u32)rate;
        s->ch = ch > 2 ? 2 : ch;
    }
    else
    {
        // MP3 says its rate and channels in its frames: the first ones decoded now (kept for the voice).
        int k;
        mp3dec_init(&s->mp3);
        for (k = 0; k < 8 && !s->rate && sound_packet(v, s); k++) {}
        if (!s->rate) { H64_WARN("[preview] the MP3 sound cannot be decoded"); sound_free(s); return 0; }
    }
    if (xb_audio_preview_open(s->rate, s->ch)) { sound_free(s); return 0; }
    return 1;
}

static void sound_close(PvSound *s)
{
    if (s->codec == VIDEO_AUDIO_NONE) return;
    xb_audio_preview_close();
    sound_free(s);
}

// Back to the track's start (the video goes round again).
static void sound_rewind(PvSound *s)
{
    s->next = 0;
    s->ended = 0;
    if (s->codec == VIDEO_AUDIO_AAC) NeAACDecPostSeekReset(s->aac, 0);
    else mp3dec_init(&s->mp3);
}

// The voice kept fed: packets decoded until it holds what it can. `pad`: past the sound's end,
// silence (the pictures still going on: the clock must not stop).
static void sound_feed(XbVideo *v, PvSound *s, int pad)
{
    for (;;)
    {
        if (s->have >= PV_AUDIO_FRAMES - 2048 || (s->ended && s->have))
        {
            u32 n = s->have > PV_AUDIO_FRAMES ? PV_AUDIO_FRAMES : s->have;
            if (!xb_audio_preview_push(s->pcm, n)) return;   // full: later
            memmove(s->pcm, s->pcm + n * s->ch, (s->have - n) * s->ch * sizeof(s16));
            s->have -= n;
            s->pushed += n;
            continue;
        }
        if (s->ended)
        {
            if (!pad) return;
            memset(s->pcm, 0, 1024 * s->ch * sizeof(s16));
            if (!xb_audio_preview_push(s->pcm, 1024)) return;
            s->pushed += 1024;
            continue;
        }
        if (!sound_packet(v, s)) s->ended = 1;
    }
}

// The sound decoded and handed to the voice on a thread of its own (hardware thread 1, the menus'
// core): AAC at 96 kHz took a fifth of the pictures' thread. `lock` guards the PvSound.
struct PvFeed
{
    XbVideo *v;
    PvSound *snd;
    CRITICAL_SECTION lock;
    volatile LONG quit, pad;
    HANDLE thread;
};

static DWORD WINAPI feed_thread(LPVOID arg)
{
    PvFeed *f = (PvFeed *)arg;
    while (!f->quit)
    {
        EnterCriticalSection(&f->lock);
        sound_feed(f->v, f->snd, f->pad);
        LeaveCriticalSection(&f->lock);
        Sleep(10);
    }
    return 0;
}

static int feed_start(PvFeed *f, XbVideo *v, PvSound *snd)
{
    f->v = v;
    f->snd = snd;
    f->quit = 0;
    f->pad = 1;
    InitializeCriticalSection(&f->lock);
    sound_feed(v, snd, 1);   // the first buffers before the clock starts
    f->thread = CreateThread(NULL, 64 * 1024, feed_thread, f, CREATE_SUSPENDED, NULL);
    if (!f->thread) { DeleteCriticalSection(&f->lock); return 0; }
    XSetThreadProcessor(f->thread, 1);
    ResumeThread(f->thread);
    return 1;
}

static void feed_stop(PvFeed *f)
{
    if (!f->thread) return;
    InterlockedExchange(&f->quit, 1);
    WaitForSingleObject(f->thread, INFINITE);
    CloseHandle(f->thread);
    f->thread = NULL;
    DeleteCriticalSection(&f->lock);
}

// Plays `file` round and round while the list wants it.
static void play(const char *file, const char *romPath, LONG seq)
{
    XbVideo *v = VideoOpen(file);
    PvSound snd;
    PvFeed feed;
    VideoPicture pic;
    LARGE_INTEGER freq, t0, now;
    u8 *work = NULL;
    u32 rate = 0, frame = 0, shown = 0, skipped = 0;
    int ow = 0, oh = 0, sound, videoEnded = 0;
    double frameMs, loopMs = 0;   // a picture's time; where the current round started on the clock
    double lastShown = -1000.0;
    if (!v)
    {
        DeleteFileA(file);   // a broken download: downloaded again next time
        return;
    }
    memset(&snd, 0, sizeof(snd));
    memset(&feed, 0, sizeof(feed));
    sound = s_sound && sound_open(v, &snd);
    if (sound && !feed_start(&feed, v, &snd)) { sound_close(&snd); sound = 0; }
    rate = VideoRate1000(v);
    frameMs = rate ? 1000000.0 / rate : 1000.0 / 30.0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    H64_INFO("[preview] playing %s (%s%s, %.2f pictures/s)", file, sound ? "sound " : "no sound",
             sound ? (snd.codec == VIDEO_AUDIO_AAC ? "AAC" : "MP3") : "", 1000.0 / frameMs);
    if (sound) H64_INFO("[preview] sound: %u Hz, %d channel(s)", snd.rate, snd.ch);
    while (keep_playing(seq))
    {
        double clock, due;
        int r;
        if (sound && !s_sound)
        {
            // Preview sound turned off while it plays: on without it, the time going on from here.
            double at = (double)xb_audio_preview_played() * 1000.0 / snd.rate;
            feed_stop(&feed);
            sound_close(&snd);
            sound = 0;
            QueryPerformanceCounter(&t0);
            t0.QuadPart -= (LONGLONG)(at * freq.QuadPart / 1000.0);
        }
        if (sound) InterlockedExchange(&feed.pad, !videoEnded);
        // The clock: the sound played so far (the pictures follow it), else the time since the start.
        QueryPerformanceCounter(&now);
        clock = sound ? (double)xb_audio_preview_played() * 1000.0 / snd.rate : (double)(now.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        due = loopMs + frame * frameMs;
        if (!videoEnded && due > clock + 2.0)
        {
            Sleep(due - clock > 12.0 ? 8 : 1);
            continue;
        }
        // Behind by more than a picture: one nothing refers to may be left out (its time passes).
        r = videoEnded ? 0 : VideoFrame(v, &pic, clock - due > frameMs);
        if (r < 0) break;
        if (r == 2)
        {
            frame++;
            skipped++;
            continue;
        }
        if (r == 0)
        {
            // The end: once the sound is all handed over too, both again from the start - the new
            // round's sound right after the last one's, its pictures timed from there.
            videoEnded = 1;
            if (sound)
            {
                int done;
                InterlockedExchange(&feed.pad, 0);
                EnterCriticalSection(&feed.lock);
                done = snd.ended && !snd.have;
                if (done)
                {
                    loopMs = (double)snd.pushed * 1000.0 / snd.rate;
                    sound_rewind(&snd);
                }
                LeaveCriticalSection(&feed.lock);
                if (!done) { Sleep(5); continue; }
            }
            else
            {
                if (clock < due) { Sleep(5); continue; }
                loopMs = due;
            }
            frame = 0;
            videoEnded = 0;
            VideoRewind(v);
            continue;
        }
        frame++;
        if (!sound && clock - due > 500.0)
        {
            // Silent and far behind: no sound to keep up with, the pictures' time moves on instead.
            loopMs += clock - due;
        }
        if (!work)
        {
            int step = pic.width > PV_MAX_W ? 2 : 1;
            ow = (pic.width / step) & ~1;
            oh = (pic.height / step) & ~1;
            if (ow > PV_MAX_W) ow = PV_MAX_W;
            if (oh > PV_MAX_H) oh = PV_MAX_H;
            if (!(work = (u8 *)malloc((size_t)ow * oh * 4))) break;
        }
        // Late: the conversion left out to catch up - but a picture at least every 100 ms all the same.
        if (clock - due > frameMs * 2.0 && clock - lastShown < 100.0)
        {
            skipped++;
            continue;
        }
        yuv_to_argb(&pic, work, ow, oh);
        picture_publish(&work, ow, oh, romPath);
        if (!work && !(work = (u8 *)malloc((size_t)ow * oh * 4))) break;
        lastShown = clock;
        shown++;
    }
    H64_INFO("[preview] stopped: %u pictures shown, %u skipped", shown, skipped);
    picture_forget();
    feed_stop(&feed);
    sound_close(&snd);
    VideoClose(v);
    free(work);
}

// A field's value in ScreenScraper's JSON ("niveau": "1"), copied into out ("" when not there).
static void json_field(const char *json, const char *key, char *out, int size)
{
    char pat[48];
    const char *p;
    int n = 0;
    out[0] = 0;
    _snprintf(pat, sizeof(pat) - 1, "\"%s\"", key);
    pat[sizeof(pat) - 1] = 0;
    if (!(p = strstr(json, pat)) || !(p = strchr(p + strlen(pat), ':'))) return;
    p++;
    while (*p == ' ' || *p == '"') p++;
    while (*p && *p != '"' && *p != ',' && *p != '}' && n < size - 1) out[n++] = *p++;
    out[n] = 0;
}

// Settings' check of the account: ssuserInfos.php answers with its limits (200) or refuses it (403).
static void account_check(void)
{
    char path[1024], level[16], speed[16], today[16], maxDay[16];
    u8 *body = NULL;
    u32 len = 0;
    int status = 0, state = PREVIEW_ACCOUNT_NONET;
    InterlockedExchange(&s_ssUserBad, 0);
    if (NetStart(0) && ss_path(path, sizeof(path), "ssuserInfos.php"))
    {
        status = NetGet(SS_HOST, path, 1, &body, &len, 256 * 1024, NULL, 0);
        memset(path, 0, sizeof(path));
    }
    level[0] = speed[0] = today[0] = maxDay[0] = 0;
    if (status == 200 && body && strstr((const char *)body, "\"ssuser\""))
    {
        json_field((const char *)body, "niveau", level, sizeof(level));
        json_field((const char *)body, "maxdownloadspeed", speed, sizeof(speed));
        json_field((const char *)body, "requeststoday", today, sizeof(today));
        json_field((const char *)body, "maxrequestsperday", maxDay, sizeof(maxDay));
        state = PREVIEW_ACCOUNT_OK;
    }
    else if (status == 401 || status == 403)
    {
        InterlockedExchange(&s_ssUserBad, 1);
        state = PREVIEW_ACCOUNT_BAD;
    }
    free(body);
    EnterCriticalSection(&s_lock);
    strcpy(s_acctLevel, level);
    strcpy(s_acctSpeed, speed);
    strcpy(s_acctToday, today);
    strcpy(s_acctMax, maxDay);
    LeaveCriticalSection(&s_lock);
    H64_INFO("[preview] ScreenScraper account check: HTTP %d, level %s, %s KB/s, %s of %s requests today", status, level, speed, today, maxDay);
    InterlockedExchange(&s_acctState, state);
}

static std::string settled_key(const char *path)
{
    std::string k(path);
    size_t i;
    for (i = 0; i < k.size(); i++) k[i] = (char)tolower((unsigned char)k[i]);
    return k;
}

static void settled_file(char *out, size_t size)
{
    _snprintf(out, size - 1, "%s\\video\\done.txt", ArtFolder());
    out[size - 1] = 0;
}

// done.txt read once: a "none" older than SS_RETRY_DAYS is not settled (asked again).
static void settled_load(void)
{
    char name[128], line[400];
    FILE *f;
    u32 now = today();
    if (s_settledLoaded) return;
    s_settledLoaded = 1;
    settled_file(name, sizeof(name));
    if (!(f = fopen(name, "rb"))) return;
    while (fgets(line, sizeof(line), f))
    {
        char *bar = strchr(line, '|'), *e;
        if (!bar) continue;
        for (e = bar + strlen(bar); e > bar + 1 && (e[-1] == '\n' || e[-1] == '\r'); e--) e[-1] = 0;
        if (line[0] == 'n' && now - (u32)strtoul(line + 1, NULL, 10) >= SS_RETRY_DAYS) continue;
        if (line[0] == 'v' || line[0] == 'n') s_settled.insert(settled_key(bar + 1));
    }
    fclose(f);
}

static void settled_add(const char *path, int none)
{
    char name[128];
    FILE *f;
    int fresh;
    EnterCriticalSection(&s_lock);
    fresh = s_settled.insert(settled_key(path)).second;
    LeaveCriticalSection(&s_lock);
    if (!fresh) return;
    settled_file(name, sizeof(name));
    if ((f = fopen(name, "ab")) != NULL)
    {
        if (none) fprintf(f, "n%u|%s\r\n", today(), path);
        else fprintf(f, "v|%s\r\n", path);
        fclose(f);
    }
}

// One game of the bulk pass: its video downloaded when it is missing. 0 when there was none left.
static int bulk_step(void)
{
    std::string path;
    H64GameInfo info;
    char file[160];
    LONG i;
    EnterCriticalSection(&s_lock);
    i = s_bulkNext;
    if (i < (LONG)s_bulk.size()) path = s_bulk[i];
    LeaveCriticalSection(&s_lock);
    if (path.empty()) return 0;
    memset(&info, 0, sizeof(info));
    ArtReadInfo(path.c_str(), &info);
    {
        int got = video_get(path.c_str(), info.found ? info.title : "", file, sizeof(file), -1);
        if (got) settled_add(path.c_str(), got < 0);
    }
    if (!s_stopAll) InterlockedIncrement(&s_bulkNext);   // stopped: this one again when the list is back
    return 1;
}

static DWORD WINAPI preview_thread(LPVOID)
{
    LONG played = -1;   // the wish played last (or found without a video): not again until another one
    for (;;)
    {
        char path[256], title[128], file[160];
        LONG seq;
        int on, bulk;
        bulk = s_enabled && s_netOk && !s_stopAll && s_bulkNext < s_bulkTotal;
        WaitForSingleObject(s_wake, bulk ? 0 : 100);
        if (s_acctCheck)
        {
            InterlockedExchange(&s_acctCheck, 0);
            account_check();
            continue;
        }
        EnterCriticalSection(&s_lock);
        seq = s_wantSeq;
        strcpy(path, s_wantPath);
        strcpy(title, s_wantTitle);
        on = s_enabled && path[0] && GetTickCount() - s_beat < PV_BEAT_MS && seq != played;
        InterlockedExchange(&s_idle, on || bulk ? 0 : 1);
        LeaveCriticalSection(&s_lock);
        if (on)
        {
            int got = video_get(path, title, file, sizeof(file), seq);
            if (got > 0 && keep_playing(seq)) play(file, path, seq);
            // Stopped because the list was left a moment (Settings, a message): the same video again when
            // it is back; else not until another game is wanted.
            played = got > 0 && !want_changed(seq) && GetTickCount() - s_beat >= PV_BEAT_MS ? -1 : seq;
            continue;
        }
        if (bulk) bulk_step();
    }
    return 0;
}

// ---- The list's side ----

void PreviewAccount(const char *user, const char *pass)
{
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    strncpy(s_ssUser, user ? user : "", sizeof(s_ssUser) - 1);
    strncpy(s_ssPass, pass ? pass : "", sizeof(s_ssPass) - 1);
    LeaveCriticalSection(&s_lock);
    InterlockedExchange(&s_ssUserBad, 0);
    InterlockedExchange(&s_acctState, s_ssUser[0] ? PREVIEW_ACCOUNT_UNKNOWN : PREVIEW_ACCOUNT_NONE);
}

void PreviewAccountCheck(void)
{
    if (!s_started) return;
    InterlockedExchange(&s_acctState, PREVIEW_ACCOUNT_CHECKING);
    InterlockedExchange(&s_acctCheck, 1);
    SetEvent(s_wake);
}

int PreviewAccountStatus(char *level, char *speed, char *today, char *maxDay)
{
    if (s_started)
    {
        EnterCriticalSection(&s_lock);
        if (level) strcpy(level, s_acctLevel);
        if (speed) strcpy(speed, s_acctSpeed);
        if (today) strcpy(today, s_acctToday);
        if (maxDay) strcpy(maxDay, s_acctMax);
        LeaveCriticalSection(&s_lock);
    }
    return s_acctState;
}

void PreviewBulkStart(const char *const *paths, int count)
{
    int i;
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    settled_load();
    s_bulk.clear();
    for (i = 0; i < count; i++)
        if (!s_settled.count(settled_key(paths[i]))) s_bulk.push_back(paths[i]);   // only what is still missing
    InterlockedExchange(&s_bulkNext, 0);
    InterlockedExchange(&s_bulkTotal, (LONG)s_bulk.size());
    InterlockedExchange(&s_bulkCancel, 0);
    InterlockedExchange(&s_stopAll, 0);
    LeaveCriticalSection(&s_lock);
    SetEvent(s_wake);
}

int PreviewBulkStatus(int *done, int *total)
{
    *done = s_bulkNext;
    *total = s_bulkTotal;
    return s_enabled && s_netOk && !s_stopAll && s_bulkTotal > 0 && s_bulkNext < s_bulkTotal;
}

void PreviewStart(void)
{
    if (s_started) return;
    InitializeCriticalSection(&s_lock);
    s_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    s_thread = CreateThread(NULL, 256 * 1024, preview_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!s_thread) return;
    XSetThreadProcessor(s_thread, 2);   // core 1: hardware thread 4 shares core 2 with XAudio2 (5)
    ResumeThread(s_thread);
    s_started = 1;
}

void PreviewSettings(int enabled, int sound, int network)
{
    s_enabled = enabled;
    s_sound = sound;
    s_netOk = network;
    if (s_started) SetEvent(s_wake);
}

void PreviewWant(const char *romPath, const char *title)
{
    if (!s_started) return;
    if (!romPath) romPath = "";
    if (!title) title = "";
    if (romPath[0] && s_stopAll)
    {
        // The list is back after PreviewStop: the bulk pass goes on too.
        InterlockedExchange(&s_bulkCancel, 0);
        InterlockedExchange(&s_stopAll, 0);
    }
    if (!strcmp(romPath, s_wantPath)) return;
    EnterCriticalSection(&s_lock);
    strncpy(s_wantPath, romPath, sizeof(s_wantPath) - 1);
    strncpy(s_wantTitle, title, sizeof(s_wantTitle) - 1);
    InterlockedIncrement(&s_wantSeq);
    InterlockedExchange(&s_cancel, 1);   // a download for the one before stops
    LeaveCriticalSection(&s_lock);
    SetEvent(s_wake);
}

IDirect3DTexture9 *PreviewTexture(IDirect3DDevice9 *dev, const char *romPath, int *w, int *h, float *since)
{
    int seq, sw, sh, j;
    if (!s_started || !dev) return NULL;
    s_beat = GetTickCount();
    // The picture drawn last frame: a fence after its draw, so it is not written while the GPU reads it.
    if (s_texLast >= 0) s_tex[s_texLast].fence = dev->InsertFence();
    EnterCriticalSection(&s_lock);
    if (!s_shown || !romPath[0] || _stricmp(s_shownPath, romPath))
    {
        LeaveCriticalSection(&s_lock);
        s_texPath[0] = 0;
        return NULL;
    }
    seq = s_shownSeq;
    sw = s_shownW;
    sh = s_shownH;
    if (seq != s_texSeq)
    {
        // A new picture: into the texture not drawn last frame.
        D3DLOCKED_RECT lr;
        PvTex *t;
        j = s_texLast < 0 ? 0 : 1 - s_texLast;
        t = &s_tex[j];
        if (t->tex && (t->w != sw || t->h != sh)) { UiImageRelease(dev, t->tex); t->tex = NULL; }
        if (!t->tex)
        {
            if (FAILED(dev->CreateTexture(sw, sh, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &t->tex, NULL))) t->tex = NULL;
            t->w = sw;
            t->h = sh;
            t->fence = 0;
        }
        if (t->tex)
        {
            int y;
            if (t->fence) dev->BlockOnFence(t->fence);
            if (SUCCEEDED(t->tex->LockRect(0, &lr, NULL, 0)))
            {
                for (y = 0; y < sh; y++) memcpy((u8 *)lr.pBits + (size_t)y * lr.Pitch, s_shown + (size_t)y * sw * 4, (size_t)sw * 4);
                t->tex->UnlockRect(0);
                s_texLast = j;
                s_texSeq = seq;
            }
        }
    }
    LeaveCriticalSection(&s_lock);
    if (s_texLast < 0 || !s_tex[s_texLast].tex) return NULL;
    if (_stricmp(s_texPath, romPath))
    {
        strncpy(s_texPath, romPath, sizeof(s_texPath) - 1);
        s_texSince = UiTime();
    }
    *w = s_tex[s_texLast].w;
    *h = s_tex[s_texLast].h;
    if (since) *since = s_texSince;
    return s_tex[s_texLast].tex;
}

void PreviewStop(IDirect3DDevice9 *dev)
{
    int i;
    DWORD t0 = GetTickCount();
    if (!s_started) return;
    PreviewWant(NULL, NULL);
    InterlockedExchange(&s_stopAll, 1);
    InterlockedExchange(&s_bulkCancel, 1);
    s_beat = 0;
    while (!s_idle && GetTickCount() - t0 < 5000) Sleep(5);
    if (!s_idle) H64_WARN("[preview] the video thread did not stop in 5 s");
    EnterCriticalSection(&s_lock);
    free(s_shown);
    s_shown = NULL;
    s_shownPath[0] = 0;
    LeaveCriticalSection(&s_lock);
    for (i = 0; i < 2; i++)
        if (s_tex[i].tex)
        {
            UiImageRelease(dev, s_tex[i].tex);
            s_tex[i].tex = NULL;
        }
    s_texLast = s_texSeq = -1;
    s_texPath[0] = 0;
}
