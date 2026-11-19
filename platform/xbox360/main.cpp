// Harissa64 V2 - Xbox 360 entry point.
//
// Start-up: unit tests on the console's own CPU, then, depending on
// game:\harissa64v2.ini (key=value lines, all optional):
//   mode=play      (default) run a ROM: recompiler or interpreter CPU, RSP
//                  task HLE (graphics and audio), Xenos renderer, XAudio2,
//                  XInput. The ROM is rom=<path>, else game:\test.z64, else
//                  the first .z64/.n64/.v64/.zip in game:\roms\.
//   mode=jittest   the M3 recompiler check (lockstep and timings, no video).
//   cpu=dynarec    (default) or cpu=interp (Xenia cannot run generated code).
//   hle=1          (default) or hle=0 for the LLE RSP (slow; no video yet:
//                  the Xenos renderer only draws HLE triangles).
// The log goes to game:\laika64.log, or cache:\ when game:\ is
// read-only (Xenia). Holding BACK for 3 s returns to the ROM list.
#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <share.h>

#include <string>
#include <ppcintrinsics.h>

#include "../../core/common/h64_types.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "../../core/common/h64_fenv.h"
#include "../../core/common/h64_png.h"
#include "../../core/system/h64_system.h"
#include "../../core/system/h64_profile.h"
#include "../../core/dynarec/h64_lockstep.h"
#include "../../core/hle/h64_hle.h"
#include "../../core/pif/h64_input_script.h"
#include "../../core/savestate/h64_state.h"
#include "../../render/xenos/h64_xenos.h"
#include "../../tests/unit/unit_tests.h"
#include "xb_config.h"
#include "xb_lang.h"
#include "xb_ui.h"
#include "xb_menu.h"
#include "xb_audio.h"
#include "xb_art.h"
#include "xb_hirespack.h"
#include "xb_stats.h"
#include "xb_net.h"
#include "xb_online.h"
#include "xb_emojis.h"
#include "xb_p2p.h"
#include "../../core/cheat/h64_cheat.h"

// ---- Recompiler code memory (M0.3 result: only an image section linked
// writable + executable runs generated code; /SECTION:.jitc,ERW in the project) ----
extern "C" VOID NTAPI KeSweepIcacheRange(PVOID Address, SIZE_T Size);

// 32 MB (the most a relative branch reaches: hot code in the first half,
// slow paths in the second); 16 MB filled up every ~40 s on DK64 and Conker,
// and each flush recompiled ~6000 blocks.
#define JIT_BYTES (32u * 1024u * 1024u)
#pragma section(".jitc", read, write, execute)
__declspec(allocate(".jitc")) static unsigned int s_jitMem[JIT_BYTES / 4] = { 1 };

static void FlushIcache(void *p, u32 size)
{
    u32 i;
    for (i = 0; i < size; i += 128)
        __dcbst((int)i, p);
    __sync();
    KeSweepIcacheRange(p, size);
    __emit(0x4C00012C);   // isync
}

// Speed of the code memory: a loop of 16 nops generated at the end of
// the code cache, against the same loop compiled into .text. A large ratio
// would mean the .jitc pages run uncached.
typedef u32 (*BenchFn)(u32 iterations);

__declspec(noinline) static u32 BenchText(u32 n)
{
    u32 v = 0;
    do
    {
        __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
        __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
        __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
        __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
        v++;
    } while (v < n);
    return v;
}

static void BenchCodeMemory(void)
{
    u32 *code = s_jitMem + (JIT_BYTES - 4096) / 4, i, k = 0;
    LARGE_INTEGER f, a, b, c;
    BenchFn fn = (BenchFn)(void *)code;
    code[k++] = 0x38800000;                       // li r4, 0
    code[k++] = 0x7C6903A6;                       // mtctr r3
    for (i = 0; i < 16; i++) code[k++] = 0x60000000;   // nop (both loops: instruction fetch speed)
    code[k++] = 0x4200FFC0;                       // bdnz -64
    code[k++] = 0x7C832378;                       // mr r3, r4
    code[k++] = 0x4E800020;                       // blr
    FlushIcache(code, k * 4);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    fn(100000);
    QueryPerformanceCounter(&b);
    BenchText(100000);
    QueryPerformanceCounter(&c);
    H64_INFO("[jit] code memory: 1.6M nops in %.2f ms (generated, .jitc) vs %.2f ms (.text)",
             (double)(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart, (double)(c.QuadPart - b.QuadPart) * 1000.0 / f.QuadPart);

    // Locality: a chain of 2048 one-instruction blocks ("b next") laid out at
    // different strides through the code cache. The time per hop shows what a
    // jump to code far away costs (instruction cache, L2, address translation).
    {
        static const u32 strides[4] = { 128, 1024, 4096, 8192 };
        char line[200];
        size_t len = 0;
        u32 s;
        line[0] = 0;
        for (s = 0; s < 4; s++)
        {
            u32 stride = strides[s], hops = 2048, words = stride / 4, h, rep;
            BenchFn chain = (BenchFn)(void *)s_jitMem;
            if ((u64)hops * stride + 4096 > JIT_BYTES) hops = (JIT_BYTES - 4096) / stride;
            for (h = 0; h < hops; h++)
            {
                u32 *w = s_jitMem + h * words;
                *w = h + 1 < hops ? 0x48000000u | (stride & 0x03FFFFFCu) : 0x4E800020u;   // b +stride / blr
                FlushIcache(w, 4);
            }
            chain(0);   // warm up
            QueryPerformanceCounter(&a);
            for (rep = 0; rep < 50; rep++) chain(0);
            QueryPerformanceCounter(&b);
            len += _snprintf(line + len, sizeof(line) - len, "%s%u B: %.1f ns", s ? ", " : "", stride,
                             (double)(b.QuadPart - a.QuadPart) * 1e9 / f.QuadPart / (50.0 * hops));
        }
        line[sizeof(line) - 1] = 0;
        H64_INFO("[jit] code locality, per jump between blocks %s", line);
    }
}

static FILE *s_log = NULL;
static const char *s_logDrive = "game";   // where the log (and screenshots) go

// The log goes to memory; a low-priority thread writes it every 500 ms.
// Writing it from the emulation thread stalled it for 100-140 ms every 2-s
// period of [perf] lines (FATX writes), which clock pacing turned into lost
// frames. Warnings and errors are written at once (a crash must not lose them).
static CRITICAL_SECTION s_logLock;
static std::string s_logBuf;
static HANDLE s_logThread;
static volatile LONG s_logQuit;

static CRITICAL_SECTION s_logFileLock;   // the file only: the buffer lock is never held while writing

static void LogFlush(void)
{
    std::string out;
    if (!s_log) return;
    EnterCriticalSection(&s_logFileLock);
    EnterCriticalSection(&s_logLock);
    out.swap(s_logBuf);
    LeaveCriticalSection(&s_logLock);
    if (!out.empty())
    {
        fwrite(out.data(), 1, out.size(), s_log);
        fflush(s_log);
    }
    LeaveCriticalSection(&s_logFileLock);
}

static DWORD WINAPI LogThread(LPVOID)
{
    while (!s_logQuit)
    {
        Sleep(500);
        LogFlush();
    }
    return 0;
}


// Any thread's unhandled exception, logged before the title ends as it did: where it happened (Iar, Lr) and the
// code addresses on its stack (named with the linker map). A crash outside the emulation's own handler (RunFrame)
// - the present, the per-second stats, the saves, a worker - closed LAIKA with nothing in the log (Rogue Squadron
// after its logos).
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *e)
{
    const CONTEXT *ctx = e->ContextRecord;
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    const u32 *sp = (const u32 *)(u32)ctx->Gpr1;
    char calls[200];
    int i, n = 0, len = 0;
    calls[0] = 0;
    __try
    {
        for (i = 0; i < 256 && n < 10 && len < (int)sizeof(calls) - 12; i++)
        {
            u32 w = sp[i];
            if (w >= 0x82000000u && w < 0x84000000u) { len += _snprintf(calls + len, sizeof(calls) - len - 1, " %08X", w); n++; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }   // (the stack ended)
    H64_ERROR("[crash] unhandled exception %08X at %08X (lr %08X), thread %08X, data address %08X | stack:%s", r->ExceptionCode,
              ctx->Iar, ctx->Lr, GetCurrentThreadId(), r->NumberParameters >= 2 ? (u32)r->ExceptionInformation[1] : 0, calls);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void file_sink(int level, const char *line)
{
    // No OutputDebugString: with XBDM loaded (DashLaunch plugin) each call goes
    // to the debug monitor over the network, ~9 ms per line: the [perf] block
    // stalled the emulation 90-120 ms every 2 s.
    if (!s_log) return;
    EnterCriticalSection(&s_logLock);
    s_logBuf += line;
    s_logBuf += '\n';
    LeaveCriticalSection(&s_logLock);
    if (level <= H64_LOG_WARN || !s_logThread) LogFlush();
}

// The console's drives under names the folder picker (Settings > Paths) can
// open, as dashboards name them: the hard disk's data partition, the USB
// drives, the disc. A drive that is not there is skipped (its link answers nothing).
extern "C" LONG __stdcall ObCreateSymbolicLink(void *link, void *device);
struct KString { USHORT Length, MaximumLength; char *Buffer; };

static void MountDrives(void)
{
    static const char *const map[][2] = {
        { "Hdd1:", "\\Device\\Harddisk0\\Partition1" }, { "Usb0:", "\\Device\\Mass0" }, { "Usb1:", "\\Device\\Mass1" },
        { "Usb2:", "\\Device\\Mass2" }, { "Dvd:", "\\Device\\Cdrom0" },
    };
    int i;
    for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
    {
        char link[32], dev[64];
        KString l, d;
        LONG st;
        _snprintf(link, sizeof(link) - 1, "\\??\\%s", map[i][0]);
        link[sizeof(link) - 1] = 0;
        strncpy(dev, map[i][1], sizeof(dev) - 1);
        dev[sizeof(dev) - 1] = 0;
        l.Buffer = link; l.Length = (USHORT)strlen(link); l.MaximumLength = l.Length + 1;
        d.Buffer = dev; d.Length = (USHORT)strlen(dev); d.MaximumLength = d.Length + 1;
        st = ObCreateSymbolicLink(&l, &d);
        H64_INFO("[main] %s -> %s: %s", map[i][0], map[i][1], st >= 0 ? "mounted" : "not there");
    }
}

// ---- Configuration: xb_config.cpp ----

static void LoadConfig(Config *c)
{
    ConfigDefaults(c);
    ConfigParseFile(c, "game:\\harissa64v2.ini");
}

// The menus' language: uilang= (checks) first, then lang= of the menu's
// settings file. Without one (the first start), the language list, starting
// on the console's language, unless a check runs (`canAsk` 0: English); the
// choice is written to the settings file. langpick=1 shows the list anyway.
static void ChooseLanguage(IDirect3DDevice9 *dev, const Config *cfg, const char *settingsPath, int canAsk)
{
    static Config menu;
    int lang;
    if (cfg->uiLang[0])
    {
        lang = LangFromCode(cfg->uiLang);
        LangSet(lang < 0 ? LANG_EN : lang);
        H64_INFO("[lang] menus in %s (uilang=)", LangCode(LangGet()));
        return;
    }
    menu = *cfg;
    if (settingsPath[0]) ConfigParseFile(&menu, settingsPath);
    lang = LangFromCode(menu.lang);
    if ((lang >= 0 && !cfg->langPick) || !dev || (!canAsk && !cfg->langPick))
    {
        LangSet(lang >= 0 ? lang : LANG_EN);
        H64_INFO("[lang] menus in %s%s", LangCode(LangGet()), lang >= 0 ? "" : " (none chosen yet)");
        return;
    }
    if (cfg->langPick >= 2) lang = cfg->langPick - 2;
    LanguageScreenAuto(cfg->langAuto);
    lang = LanguageScreen(dev, lang >= 0 ? lang : LangFromConsole(), 1);
    LangSet(lang);
    if (settingsPath[0] && !ConfigWriteLang(settingsPath, LangCode(lang))) H64_WARN("[lang] cannot write %s", settingsPath);
    H64_INFO("[lang] menus in %s (chosen at start)", LangCode(lang));
}

// Shows a message until BACK is pressed (and released).
static void MessageScreen(IDirect3DDevice9 *dev, const char *title, const char *text, D3DCOLOR color)
{
    for (;;)
    {
        XINPUT_STATE in;
        if (dev)
        {
            static char wrapped[2048];
            const float x = UI_SAFE_X, y = 140, w = UI_WIDTH - 2 * UI_SAFE_X, h = 470;
            dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
            UiBegin(dev);
            UiBackdrop();
            UiBrand(UI_SAFE_X, 72, 34);
            UiPanel(x, y, w, h, 22);
            UiRoundRect(x + 28, y + 34, 5, 40, 2.5f, color, color);
            UiText(UI_BOLD, 30, x + 52, y + 54, UI_COL_TEXT, title, UI_LEFT);
            UiTextWrap(UI_REGULAR, 21, w - 104, 12, text, wrapped, sizeof(wrapped));
            UiText(UI_REGULAR, 21, x + 52, y + 120, UI_COL_TEXT2, wrapped, UI_LEFT);
            UiScreenFooter(Tr(T_FOOT_CONTINUE));
            UiEnd();
            dev->Present(NULL, NULL, NULL, NULL);
        }
        if (XInputGetState(0, &in) == ERROR_SUCCESS && (in.Gamepad.wButtons & XINPUT_GAMEPAD_BACK))
            break;
        Sleep(16);
    }
    for (;;)
    {
        XINPUT_STATE in;
        if (XInputGetState(0, &in) != ERROR_SUCCESS || !(in.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)) break;
        Sleep(16);
    }
}

// ---- ROM files ----
static u8 *LoadFile(const char *path, u32 *size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0, len;
    u8 *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    len = GetFileSize(h, NULL);
    buf = (u8 *)malloc(len ? len : 1);
    if (buf && !ReadFile(h, buf, len, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!buf || got != len) { free(buf); return NULL; }
    *size = len;
    return buf;
}

static int FindRom(const Config *c, char *path, size_t len)
{
    static const char *patterns[] = { "game:\\roms\\*.z64", "game:\\roms\\*.n64", "game:\\roms\\*.v64", "game:\\roms\\*.zip" };
    WIN32_FIND_DATAA fd;
    int i;
    if (c->rom[0] && GetFileAttributesA(c->rom) != 0xFFFFFFFF) { strncpy(path, c->rom, len); path[len - 1] = 0; return 1; }
    if (GetFileAttributesA("game:\\test.z64") != 0xFFFFFFFF) { strncpy(path, "game:\\test.z64", len); return 1; }
    for (i = 0; i < 4; i++)
    {
        HANDLE f = FindFirstFileA(patterns[i], &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        _snprintf(path, len, "game:\\roms\\%s", fd.cFileName);
        path[len - 1] = 0;
        FindClose(f);
        return 1;
    }
    return 0;
}

// ---- Workers: jobs run in order on other hardware threads while the CPU
// thread emulates.
//  - Graphics (asyncgfx=1, hardware thread 2): graphics HLE tasks and frame
//    presents. The D3D device belongs to one thread at a time
//    (AcquireThreadOwnership): the worker takes it for each job; the CPU
//    thread takes it back only after waiting for every queued job, and gives
//    it away before queuing one.
//  - Audio (asyncaudio=1, hardware thread 4): audio HLE tasks.
//  - Display lists (parseworker=1, hardware thread 5): graphics tasks are
//    parsed there while the graphics worker renders the previous one.
#define WORK_SLOTS 8
struct WorkJob { void (*fn)(void *); void *arg; };
struct WorkQueue
{
    WorkJob jobs[WORK_SLOTS];
    volatile LONG queued, done;
    HANDLE sem, doneEvt, thread;
    HANDLE doneEvtW;   // the same for a waiter on another worker (auto-reset events wake one thread)
    volatile LONG quit;
    int usesDevice;   // the graphics queue: D3D ownership changes hands
};
static WorkQueue s_gfxQ, s_audioQ, s_parseQ;
static IDirect3DDevice9 *s_workDev;
static H64Renderer *s_workRenderer;
static int s_ownsDevice = 1;
static u32 s_presentRegs[WORK_SLOTS][32];
// The frame in reserve (Settings > Screen): each present's refresh to hold its picture for (0: none, as soon as it is ready).
static LONG s_presentSlot[WORK_SLOTS];
static LONG s_frameSlot;   // the frame being run's (RunGame's V-Sync pacing), 0: no reserve
static int s_reserveAuto;  // the game being played has a frame in reserve from its profile (h64_game_frame_reserve)
int FrameReserveAuto(void) { return s_reserveAuto; }
// The frame in reserve for this game: the player's choice, else its profile's.
static int FrameReserveOn(const Config *c) { return c->frameReserve < 0 ? s_reserveAuto : c->frameReserve; }
static void PresentHold(LONG slot);   // below, with the refreshes' counter

static DWORD WorkThreadBody(LPVOID param)
{
    WorkQueue *q = (WorkQueue *)param;
    for (;;)
    {
        WaitForSingleObject(q->sem, INFINITE);
        if (q->quit) break;
        {
            WorkJob *j = &q->jobs[q->done % WORK_SLOTS];
            if (q->usesDevice) s_workDev->AcquireThreadOwnership();
            j->fn(j->arg);
            if (q->usesDevice) s_workDev->ReleaseThreadOwnership();
        }
        __lwsync();   // the job's results before its count (Interlocked* has no barrier here)
        InterlockedIncrement(&q->done);
        SetEvent(q->doneEvt);
        if (q->doneEvtW) SetEvent(q->doneEvtW);
    }
    return 0;
}

// A worker's exception logged first (CrashFilter): DashLaunch takes the unhandled ones before the process-wide filter.
static DWORD WINAPI WorkThread(LPVOID param)
{
    __try { return WorkThreadBody(param); }
    __except (CrashFilter(GetExceptionInformation())) { return 0; }
}

static void QueueWait(WorkQueue *q)
{
    if (!q->thread) return;
    while (q->done != q->queued) WaitForSingleObject(q->doneEvt, INFINITE);
}

// Until job `ticket` (a QueuePush result) is done.
static void QueueWaitTicket(WorkQueue *q, u32 ticket)
{
    if (!q->thread) return;
    while ((s32)((u32)q->done - ticket) < 0) WaitForSingleObject(q->doneEvt, INFINITE);
    __lwsync();
}

// The same from another worker (its own event).
static void QueueWaitTicketW(WorkQueue *q, u32 ticket)
{
    if (!q->thread) return;
    while ((s32)((u32)q->done - ticket) < 0) WaitForSingleObject(q->doneEvtW, INFINITE);
    __lwsync();
}

static u32 QueuePush(WorkQueue *q, void (*fn)(void *), void *arg)
{
    if (q->queued - q->done >= WORK_SLOTS - 1) QueueWaitTicket(q, (u32)q->done + 1);
    q->jobs[q->queued % WORK_SLOTS].fn = fn;
    q->jobs[q->queued % WORK_SLOTS].arg = arg;
    InterlockedIncrement(&q->queued);
    ReleaseSemaphore(q->sem, 1, NULL);
    return (u32)q->queued;
}

static int QueueStart(WorkQueue *q, int usesDevice, DWORD hwThread)
{
    q->queued = q->done = 0;
    q->quit = 0;
    q->usesDevice = usesDevice;
    q->sem = CreateSemaphore(NULL, 0, 1000, NULL);
    q->doneEvt = CreateEvent(NULL, FALSE, FALSE, NULL);
    q->doneEvtW = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!q->sem || !q->doneEvt || !q->doneEvtW) return -1;
    q->thread = CreateThread(NULL, 256 * 1024, WorkThread, q, CREATE_SUSPENDED, NULL);
    if (!q->thread) return -1;
    XSetThreadProcessor(q->thread, hwThread);
    ResumeThread(q->thread);
    return 0;
}

static void QueueStop(WorkQueue *q)
{
    if (!q->thread) return;
    QueueWait(q);
    q->quit = 1;
    ReleaseSemaphore(q->sem, 1, NULL);
    WaitForSingleObject(q->thread, INFINITE);
    CloseHandle(q->thread);
    CloseHandle(q->sem);
    CloseHandle(q->doneEvt);
    CloseHandle(q->doneEvtW);
    q->doneEvtW = NULL;
    q->thread = NULL;
}

// Graphics queue: everything queued is done and the device is the CPU thread's again.
static void WorkWaitAll(void *)
{
    QueueWait(&s_parseQ);
    QueueWait(&s_gfxQ);
    if (!s_ownsDevice)
    {
        s_workDev->AcquireThreadOwnership();
        s_ownsDevice = 1;
        h64_xenos_set_worker(s_workRenderer, 0);   // RDRAM copy-backs reported to the recompiler now
    }
}

static u32 WorkStart(void *, void (*fn)(void *), void *arg)
{
    if (s_ownsDevice)
    {
        QueueWait(&s_gfxQ);
        h64_xenos_set_worker(s_workRenderer, 1);
        s_workDev->ReleaseThreadOwnership();
        s_ownsDevice = 0;
    }
    return QueuePush(&s_gfxQ, fn, arg);
}

static void WorkWaitTicket(void *, u32 ticket) { QueueWaitTicket(&s_gfxQ, ticket); }

static u32 ParseStart(void *, void (*fn)(void *), void *arg) { return QueuePush(&s_parseQ, fn, arg); }
static void ParseWaitTicket(void *, u32 ticket, int fromWorker)
{
    if (fromWorker) QueueWaitTicketW(&s_parseQ, ticket);
    else QueueWaitTicket(&s_parseQ, ticket);
}

static void AudioStart(void *, void (*fn)(void *), void *arg) { QueuePush(&s_audioQ, fn, arg); }
static void AudioWait(void *) { QueueWait(&s_audioQ); }

// Present pacing statistics: intervals between the moments frames reach the
// screen (QPC), for the [pace] line. Written by the thread that presents.
static LARGE_INTEGER s_presLast;
static double s_presSum, s_presSumSq, s_presMax, s_presMin = 1e9;
static u32 s_presCount, s_presLong;
// Longest time per loop section in the period (ms), to find stalls.
static double s_maxRun, s_maxPresent, s_maxSaves, s_maxWait, s_lastPerfBlock;

static void NotePresent(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (s_presLast.QuadPart)
    {
        double ms = (double)(now.QuadPart - s_presLast.QuadPart) * 1000.0 / (double)freq.QuadPart;
        s_presSum += ms;
        s_presSumSq += ms * ms;
        if (ms > s_presMax) s_presMax = ms;
        if (ms < s_presMin) s_presMin = ms;
        if (ms > 25.0) s_presLong++;
        s_presCount++;
    }
    s_presLast = now;
}

static void PresentJob(void *arg)
{
    const int k = (int)(((const u32 *)arg - s_presentRegs[0]) / 32);
    if (k >= 0 && k < WORK_SLOTS && s_presentSlot[k]) PresentHold(s_presentSlot[k]);
    h64_xenos_present_vi(s_workRenderer, (const u32 *)arg);
    NotePresent();
}

// The frame's present: on the graphics worker when it runs, with the VI registers of now.
static void PresentFrame(H64System *sys, H64Renderer *renderer)
{
    if (s_gfxQ.thread)
    {
        u32 *regs = s_presentRegs[s_gfxQ.queued % WORK_SLOTS];
        s_presentSlot[s_gfxQ.queued % WORK_SLOTS] = s_frameSlot;
        memcpy(regs, sys->vi.regs, sizeof(s_presentRegs[0]) < sizeof(sys->vi.regs) ? sizeof(s_presentRegs[0]) : sizeof(sys->vi.regs));
        WorkStart(NULL, PresentJob, regs);
    }
    else
    {
        h64_xenos_present(renderer);
        NotePresent();
    }
}

static void WorkersStart(H64System *sys, IDirect3DDevice9 *dev, H64Renderer *renderer, int gfx, u32 gfxCycles, int audio,
                         u32 audioCycles, int parse)
{
    s_workDev = dev;
    s_workRenderer = renderer;
    s_ownsDevice = 1;
    sys->asyncUser = NULL;
    if (gfx)
    {
        if (QueueStart(&s_gfxQ, 1, 2) == 0)
        {
            sys->asyncStart = WorkStart;
            sys->asyncWait = WorkWaitAll;
            sys->asyncWaitTicket = WorkWaitTicket;
            sys->asyncGfxCycles = gfxCycles;
            H64_INFO("[main] graphics worker on hardware thread 2 (graphics tasks keep the RSP busy %u cycles)", gfxCycles);
            if (parse && QueueStart(&s_parseQ, 0, 5) == 0)
            {
                sys->asyncParseStart = ParseStart;
                sys->asyncParseWaitTicket = ParseWaitTicket;
                H64_INFO("[main] display lists parsed on hardware thread 5");
            }
        }
        else
            H64_WARN("[main] graphics worker could not start: graphics on the CPU thread");
    }
    if (audio)
    {
        if (QueueStart(&s_audioQ, 0, 4) == 0)
        {
            sys->asyncAudioStart = AudioStart;
            sys->asyncAudioWait = AudioWait;
            sys->asyncAudioCycles = audioCycles;
            H64_INFO("[main] audio worker on hardware thread 4 (audio tasks keep the RSP busy %u cycles)", audioCycles);
        }
        else
            H64_WARN("[main] audio worker could not start: audio HLE on the CPU thread");
    }
}

static void WorkersStop(H64System *sys)
{
    QueueStop(&s_audioQ);
    if (s_gfxQ.thread)
    {
        WorkWaitAll(NULL);
        QueueStop(&s_parseQ);
        QueueStop(&s_gfxQ);
    }
    sys->asyncParseStart = NULL;
    sys->asyncParseWaitTicket = NULL;
    sys->asyncStart = NULL;
    sys->asyncWait = NULL;
    sys->asyncWaitTicket = NULL;
    sys->asyncAudioStart = NULL;
    sys->asyncAudioWait = NULL;
}

static int s_jitNoFpu;   // jitfpu=0
static int s_noRegCache; // regcache=0
static int s_noFpCache;  // fpcache=0
static int s_fastFpu;    // fastfpu=1
static int s_fullExits;  // fullexits=1
static int s_statusFast = 1;  // statusfast=0
static int s_noSuper;    // superblocks=0

static u32 s_cpi = 2;    // cpi=N

// ---- N64 PC sampler (diagnosis): a thread on another hardware thread reads
// the emulated PC every millisecond; [pc] lines give the hottest addresses
// (busy-wait loops the recompiler does not skip show up there).
#define PC_SLOTS 1024
static volatile const u64 *s_samplePc;
static volatile LONG s_samplerOn;
static u32 s_pcKey[PC_SLOTS], s_pcCount[PC_SLOTS], s_pcSamples;

static DWORD WINAPI PcSamplerThread(LPVOID)
{
    while (s_samplerOn)
    {
        u32 pc, h, n;
        Sleep(1);
        if (!s_samplePc) continue;
        pc = (u32)*s_samplePc;
        h = (pc >> 2) & (PC_SLOTS - 1);
        for (n = 0; n < 8; n++, h = (h + 1) & (PC_SLOTS - 1))
            if (s_pcCount[h] == 0 || s_pcKey[h] == pc) { s_pcKey[h] = pc; s_pcCount[h]++; break; }
        s_pcSamples++;
    }
    return 0;
}

static void PcSamplerStart(H64System *sys)
{
    HANDLE h;
    s_samplePc = &sys->cpu.pc;
    if (s_samplerOn) return;
    s_samplerOn = 1;
    h = CreateThread(NULL, 0, PcSamplerThread, NULL, CREATE_SUSPENDED, NULL);
    if (!h) { s_samplerOn = 0; return; }
    XSetThreadProcessor(h, 5);
    ResumeThread(h);
    CloseHandle(h);
}

static void PcSamplerReport(void)
{
    u32 best[4] = { 0, 0, 0, 0 }, bestPc[4] = { 0, 0, 0, 0 }, i, k, total = s_pcSamples;
    char line[160];
    if (!total) return;
    for (i = 0; i < PC_SLOTS; i++)
        for (k = 0; k < 4; k++)
            if (s_pcCount[i] > best[k])
            {
                u32 m;
                for (m = 3; m > k; m--) { best[m] = best[m - 1]; bestPc[m] = bestPc[m - 1]; }
                best[k] = s_pcCount[i];
                bestPc[k] = s_pcKey[i];
                break;
            }
    _snprintf(line, sizeof(line), "[pc] %u samples: %08X %u%%, %08X %u%%, %08X %u%%, %08X %u%%", total,
              bestPc[0], best[0] * 100 / total, bestPc[1], best[1] * 100 / total, bestPc[2], best[2] * 100 / total,
              bestPc[3], best[3] * 100 / total);
    line[sizeof(line) - 1] = 0;
    H64_INFO("%s", line);
    memset(s_pcCount, 0, sizeof(s_pcCount));
    s_pcSamples = 0;
}

static H64System *MakeSystem(const u8 *rom, u32 size, int jit)
{
    H64System *sys = (H64System *)malloc(sizeof(H64System));
    if (!sys || h64_system_init(sys, rom, size, 0)) { free(sys); return NULL; }
    if (jit && h64_jit_init(sys, s_jitMem, JIT_BYTES, FlushIcache))
    {
        H64_ERROR("[jit] h64_jit_init failed");
        h64_system_free(sys);
        free(sys);
        return NULL;
    }
    if (sys->jit) sys->jit->noFpu = s_jitNoFpu;
    if (sys->jit) sys->jit->noRegCache = s_noRegCache;
    if (sys->jit) sys->jit->noFpCache = s_noFpCache;
    if (sys->jit && s_fastFpu) { sys->jit->fastFpu = 1; sys->cpu.fpuNoTraps = 1; h64_jit_reset(sys); }
    if (sys->jit && s_fullExits) { sys->jit->fullExits = 1; h64_jit_reset(sys); }
    if (sys->jit && !s_statusFast) { sys->jit->noStatusFast = 1; h64_jit_reset(sys); }
    if (sys->jit && s_noSuper) { sys->jit->noSuper = 1; h64_jit_reset(sys); }
    sys->cpu.cpi = h64_game_cpi(sys->rom.gameCode, s_cpi);   // a game's own (profile) when cpi is the default
    s_reserveAuto = h64_game_frame_reserve(sys->rom.gameCode);
    if (s_reserveAuto) H64_INFO("[profile] %.4s: frame in reserve with V-Sync", sys->rom.gameCode);
    if (sys->cpu.cpi != s_cpi) H64_INFO("[profile] %.4s: cpi %u", sys->rom.gameCode, sys->cpu.cpi);
    return sys;
}

static void FreeSystem(H64System *sys)
{
    if (!sys) return;
    h64_system_free(sys);
    free(sys);
}

// ---- mode=jittest: the M3 recompiler check ----
// 10 emulated seconds with the recompiler in lockstep with the interpreter,
// then 10 seconds with each alone, without RDP drawing (CPU and RSP only).
static void RunDynarecTest(const Config *c, char *report, size_t len)
{
    char rom[256];
    u8 *data;
    u32 size = 0;
    const u64 cycles = 10ull * 93750000ull;
    H64System *ref, *jit;
    H64LockstepResult ls;
    DWORD t0, tLock, tInterp, tJit;

    if (!FindRom(c, rom, sizeof(rom)))
    {
        _snprintf(report, len, "Recompiler test: no ROM\n(put one at game:\\test.z64 or in game:\\roms\\)");
        report[len - 1] = 0;
        return;
    }
    data = RomFileLoad(rom, &size);
    if (!data) { _snprintf(report, len, "Recompiler test: cannot read %s", rom); report[len - 1] = 0; return; }
    H64_INFO("[jit] test ROM %s (%u bytes)", rom, size);

    ref = MakeSystem(data, size, 0);
    jit = MakeSystem(data, size, 1);
    if (!ref || !jit) { _snprintf(report, len, "Recompiler test: cannot create the systems"); report[len - 1] = 0; return; }
    ref->options.noRdpDraw = jit->options.noRdpDraw = 1;
    t0 = GetTickCount();
    h64_lockstep_run(ref, jit, cycles, &ls);
    tLock = GetTickCount() - t0;
    if (ls.diverged)
        H64_ERROR("[jit] LOCKSTEP DIVERGENCE after dispatch %llu at pc %08X (cycle %llu, frame %u): %s",
                  (unsigned long long)ls.dispatches, ls.pc, (unsigned long long)ls.cyclesBefore, ls.frame, ls.why);
    H64_INFO("[jit] lockstep %s: %llu dispatches, %llu blocks compiled, %llu idle instructions skipped, %u ms",
             ls.diverged ? "DIVERGED" : "OK", (unsigned long long)ls.dispatches,
             (unsigned long long)jit->jit->stats.blocksCompiled, (unsigned long long)jit->jit->stats.idleSkipped, tLock);
    FreeSystem(ref);
    FreeSystem(jit);

    ref = MakeSystem(data, size, 0);
    ref->options.noRdpDraw = 1;
    t0 = GetTickCount();
    h64_system_run_cycles(ref, cycles);
    tInterp = GetTickCount() - t0;
    FreeSystem(ref);
    jit = MakeSystem(data, size, 1);
    jit->options.noRdpDraw = 1;
    t0 = GetTickCount();
    h64_system_run_cycles(jit, cycles);
    tJit = GetTickCount() - t0;
    H64_INFO("[jit] 10 emulated seconds: interpreter %u ms, recompiler %u ms (%llu native instructions compiled, %llu through the interpreter)",
             tInterp, tJit, (unsigned long long)jit->jit->stats.nativeInsns, (unsigned long long)jit->jit->stats.helperInsns);
    FreeSystem(jit);
    free(data);

    _snprintf(report, len, "Recompiler, 10 emulated s of %s:\nlockstep %s (%u ms)%s%s\ninterpreter %u ms, recompiler %u ms",
              rom + 6, ls.diverged ? "DIVERGED" : "OK", tLock, ls.diverged ? "\n" : "", ls.diverged ? ls.why : "", tInterp, tJit);
    report[len - 1] = 0;
}

static int s_netMode;   // netplay: 0 off, 1 host, 2 guest, -1 ended (see NetConfig)
static int s_netPlayed; // the game being played went online (the detailed view's activity: PlayStatsAdd)

// The menu's Graphics settings on the renderer.
static void ApplyGraphics(H64System *sys, H64Renderer *renderer, const Config *c)
{
    H64XenosOptions o;
    u32 resW, resH;
    int wide;
    // The software stretch (its switch): the 4:3 picture pulled to 16:9, with
    // either list - for games the wider 3D does not suit. Without it a 16:9
    // resolution is widescreen (the 3D drawn wider), a 4:3 one 4:3 with borders.
    // Widescreen shows more than the game's viewport: keep the triangles the
    // microcode would drop outside it.
    ConfigResCurrent(c, &resW, &resH, &wide);
    int aspect = c->stretch ? 2 : wide ? 1 : 0;
    const int online = s_netMode != 0;
    if (online)
    {
        // Online every console draws the same picture: what a game copies from its frames into RDRAM (Mario Kart
        // 64's trackside screens, gfxFrameToRdram) differed between a console at 1280x720 and the others at
        // 1366x768, and their state hashes with it. One picture for all: 1280x720 widescreen (16:9, the setting most
        // players use), and nothing that costs time below.
        resW = 1280;
        resH = 720;
        aspect = 1;
    }
    sys->options.gfxKeepOffscreen = aspect == 1;
    if (!renderer) return;
    o.resW = resW;
    o.resH = resH;
    // Online: bilinear for every console (the 3-point filter is another shader, slower, and another picture).
    o.texFilter = online ? 1 : c->texFilter;
    o.hiTex = c->hiTex;
    o.smooth = c->smooth;
    // The bars: on when above 0 (one shader each way), their strength a constant. Online: off, as the scanlines
    // and the CRT screen (a pass over the whole picture each frame).
    o.sharpen = c->sharpLevel > 0 && !online ? 1 : 0;
    o.blur = c->blurLevel > 0 && !o.sharpen && !online ? 1 : 0;
    o.sharpAmount = c->sharpLevel / 100.0f;
    o.blurAmount = c->blurLevel / 100.0f;
    o.screen = online ? 0 : c->screen;
    o.brightness = c->brightness;
    {
        // The bar: 50 as the console; 100 lifts the dark tones (power 0.6), 0 deepens them (1.6).
        int lv = c->brightLevel < 0 || c->brightLevel > 100 ? 50 : c->brightLevel;
        o.brightPower = lv >= 50 ? 1.0f - 0.4f * (lv - 50) / 50.0f : 1.0f + 0.6f * (50 - lv) / 50.0f;
    }
    o.colorStyle = c->colorStyle;
    // Adaptive: a late picture shown at once (RunGame paces frames on the TV's refreshes); a frame in reserve: each
    // held for its refresh, never shown early (2); online: the clock, as the others.
    o.vsync = c->vsync && s_netMode <= 0 ? (FrameReserveOn(c) ? 2 : 1) : 0;
    o.deflicker = online ? 0 : c->deflicker;   // (a resolve and a pass more each frame)
    o.zoom = c->zoom;
    o.offsetX = c->offsetX;
    o.offsetY = c->offsetY;
    o.gfxLog = c->devMode && c->gfxLog && !online;
    o.aspect = aspect;
    h64_xenos_set_options(renderer, &o);
}

// ---- mode=play ----
static XINPUT_STATE s_pad;                  // controller 1 (also the front end's shortcuts)
static int s_padValid;
static XINPUT_STATE s_padsMore[3];          // controllers 2-4: N64 ports 2-4
static int s_padsMoreValid[3];
static const H64InputScript *s_script;   // scripted input (input=), NULL: the controller
static u32 s_scriptBase;                 // VI frame at the launch: input= counts from there, as shots=

static const H64PadMap *s_padMaps;      // the controls of ports 1-4 (Config.padMap; edited live by the menu)

// The pad mapping's button bits are XInput's.
static_assert(H64_XB_A == XINPUT_GAMEPAD_A && H64_XB_B == XINPUT_GAMEPAD_B && H64_XB_X == XINPUT_GAMEPAD_X &&
              H64_XB_Y == XINPUT_GAMEPAD_Y && H64_XB_START == XINPUT_GAMEPAD_START && H64_XB_BACK == XINPUT_GAMEPAD_BACK &&
              H64_XB_DPAD_UP == XINPUT_GAMEPAD_DPAD_UP && H64_XB_DPAD_DOWN == XINPUT_GAMEPAD_DPAD_DOWN &&
              H64_XB_DPAD_LEFT == XINPUT_GAMEPAD_DPAD_LEFT && H64_XB_DPAD_RIGHT == XINPUT_GAMEPAD_DPAD_RIGHT &&
              H64_XB_LEFT_THUMB == XINPUT_GAMEPAD_LEFT_THUMB && H64_XB_RIGHT_THUMB == XINPUT_GAMEPAD_RIGHT_THUMB &&
              H64_XB_LEFT_SHOULDER == XINPUT_GAMEPAD_LEFT_SHOULDER && H64_XB_RIGHT_SHOULDER == XINPUT_GAMEPAD_RIGHT_SHOULDER,
              "H64_XB_* must be XInput's button bits");

// An Xbox 360 controller as an N64 one, through port `port`'s mapping (the
// default one is V1's: core/pif/h64_padmap.cpp).
static void PadFromXInput(const XINPUT_GAMEPAD *g, int port, H64Pad *pad)
{
    H64PadInput in;
    H64PadMap def;
    const H64PadMap *m = s_padMaps ? &s_padMaps[port] : NULL;
    in.buttons = g->wButtons;
    in.lt = g->bLeftTrigger;
    in.rt = g->bRightTrigger;
    in.lx = g->sThumbLX;
    in.ly = g->sThumbLY;
    in.rx = g->sThumbRX;
    in.ry = g->sThumbRY;
    if (!m) { h64_padmap_default(&def); m = &def; }
    h64_padmap_apply(m, &in, pad);
}

// Rumble Pak motors (core: sys->rumbleMotor) on the controllers, once a frame.
// Games set the N64 motor's strength with short pulses (Ocarina of Time: on for
// 1-2 frames, or on and off within one frame); the 360 pad's heavier motors did
// not start on them, and the pulses inside a frame were never seen. So each
// port keeps whether its motor ran in each of the last RUMBLE_FRAMES frames
// (rumblePulse catches those inside one): the pad vibrates while any did,
// harder the more did. XInputSetState runs only on a change. `stop` (menu, end
// of the game) or the setting off: every motor stops.
#define RUMBLE_FRAMES 6
static u8 s_rumbleHist[4], s_rumbleOut[4];
static int s_rumbleOn = 100;   // Settings > Controls > Rumble Pak: its strength in % (0: off)
static int s_rumblePort = -1;  // online: the N64 port controller 1 plays (its motor is that port's, the others' stay still); -1: not online
static int s_rumbleLocal = 1;  // online: the controllers playing here - controller k has the motor of port s_rumblePort + k

// The menus set the motors themselves (a notice's pulse, Controls' test): what s_rumbleOut says was sent is not so
// any more, and the next RumbleUpdate sends its level again. Without it a pulse cut short by a game starting kept the
// pad vibrating until the game's own motor changed.
void RumbleForget(void)
{
    memset(s_rumbleOut, 0xFF, sizeof(s_rumbleOut));
}

static void RumbleUpdate(H64System *sys, int stop)
{
    int i, k;
    for (i = 0; i < 4; i++)
    {
        int connected = i == 0 ? s_padValid : s_padsMoreValid[i - 1];
        // The port whose motor this controller gets: its own, or online the port it plays (each local player its own;
        // only controller 1 had one, the others' players never vibrated).
        const int port = s_rumblePort < 0 ? i : i < s_rumbleLocal ? s_rumblePort + i : -1;
        u8 ran = 0, level = 0;
        if (sys && port >= 0) ran = (u8)(sys->rumbleMotor[port] || sys->rumblePulse[port]);
        s_rumbleHist[i] = stop ? 0 : (u8)(((s_rumbleHist[i] << 1) | ran) & ((1u << RUMBLE_FRAMES) - 1));
        for (k = 0; k < RUMBLE_FRAMES; k++) level = (u8)(level + ((s_rumbleHist[i] >> k) & 1));
        if (stop || !s_rumbleOn || !connected) level = 0;
        if (level != s_rumbleOut[i])
        {
            // From 4/9 of the strength (one frame of the last six) to all of it: weaker, the motors barely turn.
            int pct = level ? s_rumbleOn * (RUMBLE_FRAMES + 2 * level) / (3 * RUMBLE_FRAMES) : 0;
            XINPUT_VIBRATION v;
            v.wLeftMotorSpeed = (WORD)(0xC000 * pct / 100);   // the N64 pad's one motor: mostly the heavy one
            v.wRightMotorSpeed = (WORD)(0x6000 * pct / 100);
            XInputSetState((DWORD)i, &v);
            s_rumbleOut[i] = level;
        }
    }
    if (sys) memset(sys->rumblePulse, 0, sizeof(sys->rumblePulse));
}

// Called by the PIF just before the game reads the controllers. Controller n
// (XInput user n) is N64 port n + 1; port 1 is always plugged in, ports 2-4
// while their controller is connected.
static void NetPad(H64System *sys);
static void NetPadAlone(H64System *sys);

static void PadHook(H64System *sys)
{
    int i;
    if (s_script) { h64_input_script_apply_at(s_script, sys, sys->vi.frames - s_scriptBase); return; }
    if (s_netMode > 0) { NetPad(sys); return; }
    if (s_netMode < 0) { NetPadAlone(sys); return; }
    sys->padMask = 1;
    // BACK held: the buttons are front-end shortcuts, not the game's.
    if (!s_padValid || (s_pad.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)) { sys->pad[0].buttons = 0; sys->pad[0].x = sys->pad[0].y = 0; }
    else PadFromXInput(&s_pad.Gamepad, 0, &sys->pad[0]);
    for (i = 0; i < 3; i++)
    {
        if (s_padsMoreValid[i])
        {
            sys->padMask |= 2u << i;
            PadFromXInput(&s_padsMore[i].Gamepad, i + 1, &sys->pad[i + 1]);
        }
        else
        {
            sys->pad[i + 1].buttons = 0;
            sys->pad[i + 1].x = sys->pad[i + 1].y = 0;
        }
    }
}

// The profiling clock: the time base register (49.875 MHz; the [prof] lines
// divide by the performance counter's 50 MHz, 0.25 % off). Read thousands of
// times a frame (each vertex batch); QueryPerformanceCounter cost enough to
// show in what it measured.
static u64 ProfClock(void)
{
    return __mftb();
}

static int s_crashed;
static char s_crashText[256];

// Runs one VI's worth of cycles under the crash handler.
static int RunFrame(H64System *sys)
{
    __try
    {
        h64_system_run_cycles(sys, sys->vi.frameCycles);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        _snprintf(s_crashText, sizeof(s_crashText), "Exception %08X at N64 pc %08X", GetExceptionCode(),
                  (u32)sys->cpu.pc);
        s_crashText[sizeof(s_crashText) - 1] = 0;
        H64_ERROR("[crash] %s", s_crashText);
        s_crashed = 1;
        return -1;
    }
    return 0;
}

// ---- Hi-res texture packs ----
// game:\\hires_texture\\<ROM internal name>\\ can hold more than one GlideN64-style
// pack (*.hts): HiresScanPacks (xb_menu.cpp) lists them for the Graphics settings'
// page (pause menu > HD textures) and for us here. c->hiTexFile picks which one (empty: the first
// one found, same as before this game could have more than one).
static void HiresOpen(H64System *sys, H64Renderer *renderer, const Config *c)
{
    int n = 0, use = 0, i, act = 0;
    char active[128];
    HiresScanPacks(sys->rom.name);
    if (HiresPackCount() > 0)
    {
        // The game's folder's active.txt (the Packs screen, START in the ROM list): the pack in
        // use, or none ("-": HD off for this game); else Settings > Graphics' pick, else the first.
        char dir[192];
        const char *path = HiresPackPath(0), *slash = strrchr(path, '\\');
        size_t dn = slash ? (size_t)(slash - path) : 0;
        if (dn >= sizeof(dir)) dn = sizeof(dir) - 1;
        memcpy(dir, path, dn);
        dir[dn] = 0;
        act = HiresActiveRead(dir, active, sizeof(active));
        for (i = 0; i < HiresPackCount(); i++)
            if (!_stricmp(HiresPackName(i), act == 1 ? active : c->hiTexFile)) { use = i; break; }
    }
    if (c->hiTex && HiresPackCount() > 0 && act != -1)
        n = h64_xenos_load_hts(renderer, HiresPackPath(use));
    else
        h64_xenos_load_hts(renderer, NULL);
    H64_INFO("[hts] %s: %d pack(s) found, loaded %s (%d texture(s))", sys->rom.name, HiresPackCount(),
             HiresPackCount() && c->hiTex && act != -1 ? HiresPackName(use) : "none", n);
}

// ---- Game saves ----
// One folder per game: <drive>:\saves\<name> <game code>\ with eeprom.bin,
// sram.bin, flash.bin and pak1.bin (the drive of the log: game:, or cache:
// in Xenia). Written 2 s after the game first changes them, and on leaving.
static char s_saveDir[192];
static char s_savesRoot[128];   // Settings > Paths > Saves (RunGame: ConfigPath)
static char s_shotsRoot[128];   // Settings > Paths > Screenshots
static u32 s_saveDirtyAt;

static void SavesOpen(H64System *sys)
{
    char folder[64];
    h64_save_folder_name(&sys->rom, folder, sizeof(folder));
    if (!s_savesRoot[0]) _snprintf(s_savesRoot, sizeof(s_savesRoot) - 1, "%s:\\saves", s_logDrive);
    strcpy(s_saveDir, s_savesRoot);
    CreateDirectory(s_saveDir, NULL);
    _snprintf(s_saveDir, sizeof(s_saveDir), "%s\\%s", s_savesRoot, folder);
    s_saveDir[sizeof(s_saveDir) - 2] = 0;
    CreateDirectory(s_saveDir, NULL);
    strcat(s_saveDir, "\\");
    s_saveDirtyAt = 0;
    if (s_netMode > 0) { H64_INFO("[save] online: no saves loaded (both consoles start blank)"); return; }
    H64_INFO("[save] folder %s: %d file(s) loaded", s_saveDir, h64_save_load(sys->save, s_saveDir));
}

static void SavesTick(H64System *sys, u32 presented, int leaving)
{
    if (s_netMode) return;   // online: the saves stay as they were
    if (!sys->save->dirty) { s_saveDirtyAt = 0; return; }
    if (!s_saveDirtyAt) s_saveDirtyAt = presented ? presented : 1;
    if (leaving || presented - s_saveDirtyAt >= 120)
    {
        h64_save_store(sys->save, s_saveDir);
        s_saveDirtyAt = 0;
    }
}

// ---- Save states ----
// <save folder>\state<N>.h64s. While BACK is held: RB saves, LB loads,
// D-pad left/right change the slot. A message shows the result for 2 s.
static int s_stateSlot = 1;
static int s_statePending;          // a save waits for a quiet point (no HLE task in progress)
static char s_osd[160];
static DWORD s_osdUntil;
#define OSD_MS 2000                 // how long a toast stays
static DWORD s_osdLen = OSD_MS;     // ... this one (the online ones longer)

// A toast from a menu text (T_*), with a number for its %d if it has one;
// the log gets the English text.
static void OsdT(int id, int num)
{
    char en[160];
    _snprintf(s_osd, sizeof(s_osd) - 1, Tr(id), num);
    s_osd[sizeof(s_osd) - 1] = 0;
    _snprintf(en, sizeof(en) - 1, TrIn(LANG_EN, id), num);
    en[sizeof(en) - 1] = 0;
    s_osdUntil = GetTickCount() + OSD_MS;
    s_osdLen = OSD_MS;
    H64_INFO("[main] %s", en);
}

// A hash of one MB of RDRAM (four 64-bit FNV-style lanes, folded): the online play's state check.
static u32 NetHashBlock(const u8 *p)
{
    const u64 *w = (const u64 *)p;
    u64 a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
    const u64 k = 0x100000001B3ull;
    u32 i;
    for (i = 0; i < 0x100000 / 8; i += 4)
    {
        a = (a ^ w[i]) * k;
        b = (b ^ w[i + 1]) * k;
        c = (c ^ w[i + 2]) * k;
        d = (d ^ w[i + 3]) * k;
    }
    a = (((a ^ b) * k ^ c) * k ^ d) * k;
    return (u32)(a ^ (a >> 32));
}

// ---- Netplay: up to four consoles, one N64 port each ----
// Through a room of the server (Settings > Online play: up to 4 players, ports in the order they joined),
// or two consoles directly for tests (game:\netplay.txt: "host [delay]" or "join <host's IP> [delay]", TCP
// port NET_PORT; the host's delay is used). Each console's first controller plays its port. Only the
// controls travel: all run the same game from power-on and stay together because every controller read
// gets the same inputs on all of them. A console's input for read n is sent at its
// read n - delay, so a read waits only when another console's input has not arrived yet. Every
// NET_HASH_EVERY frames all compare a hash of the state (one MB of RDRAM and the CPU). Saves are neither
// loaded nor written (all start blank); states, reset and cheats are off. A console that leaves lets go of
// its controller: the others play on (alone when none is left).
static double QpcMs(void);
#define SO_MARKINSECURE 0x5801      // RGH homebrew: this socket outside Xbox Live (as xb_net.cpp)
#define NET_PORT 6464
#define NET_RING 256
#define NET_PROTO 2
#define NET_PLAYERS 4
#define NET_TIMEOUT_MS 10000        // nothing from the others this long (they ping every NET_PING_MS, their pause menu too): alone
#define NET_PING_MS 500
#define NET_P2P_NUDGE_MS 30         // waiting for the others on the direct way: the last inputs sent again this often
#define NET_DELAY_RELAY 6           // frames of input delay through the server's rooms (direct: netplay.txt's, 3; a room's direct way: 4)
#define NET_HASH_EVERY 60           // frames between two state hashes (each of a different MB of RDRAM, and the CPU)
#define NET_HASHES 64
// A message's type: its low bytes; the server puts the sender's port in the top byte (0: directly, the other console).
enum { NM_HELLO = 1, NM_INPUT, NM_HASH, NM_BYE, NM_PING, NM_PONG, NM_INPUT2 };   // a ping's a: its sender's tick; a pong's b: who it answers
// NM_INPUT2: the inputs of two reads in one message (the room's: half the messages the server counts), a and a + 1 in b and c.
// A type's third byte: which of the sender's ports an input is of (0: its first, 1 the next...).
#define NET_KIND(t) ((t) & 0xFFFF)
#define NET_SUB(t) (((t) >> 16) & 0xFF)
struct NetMsg { u32 type, a, b, c; };   // big-endian on every end
static SOCKET s_netSock = INVALID_SOCKET;   // the other console, directly (netplay.txt) ...
static NetWs *s_netWs;                      // ... or the server's room (Settings > Online play)
static int s_toRoom;                        // the pause menu's Back to the room: the game ends, the room's lobby again
static char s_netPeer[64];
static int s_netPort;               // this console's (first) port (1..s_netPlayers)
static int s_netPlayers;            // ports played (2..NET_PLAYERS)
// Two or three may play on one console (a room's, its controllers 1..s_netLocal on its ports s_netPort...): each
// port is owned by the console whose first port that is (s_netOwner), which sends its input; a console is known
// by its first port everywhere else (its hello, hashes, pings, the way to it directly, its leaving).
static int s_netLocal = 1;
static u8 s_netOwner[NET_PLAYERS + 1];
#define NET_MINE(p) (s_netOwner[p] == s_netPort)
#define NET_CONSOLE(p) (s_netOwner[p] == (p))
static int s_netHiTex = -1;         // Config::hiTex as it was before an online game turned it off (-1: not changed)
static char s_settingsPath[160];    // <drive>:\config\settings.ini ("" when the menu files are not used)
static char s_profilePath[160];     // <drive>:\config\<game>.ini

// The HD textures as just chosen in the game (R3 twice, Textures, Settings) kept for its next start: in the game's
// own settings when it has them (they would put back what they hold over the settings for all games), else in those.
// Not online: there the pack is off for that game only.
static void HiresRemember(const Config *c)
{
    const char *path = s_profilePath[0] && GetFileAttributesA(s_profilePath) != (DWORD)-1 ? s_profilePath : s_settingsPath;
    if (s_netHiTex >= 0 || !path[0]) return;
    if (!ConfigWriteKey(path, "hirestex", c->hiTex ? "1" : "0") || !ConfigWriteKey(path, "hirestexfile", c->hiTexFile))
        H64_WARN("[main] HD textures: cannot write %s", path);
    else H64_INFO("[main] HD textures %s kept in %s", c->hiTex ? "on" : "off", path);
}
static u32 s_netDelay = 3;
static u32 s_netReads;              // controller reads so far
static u32 s_netHave[NET_PLAYERS + 1];          // per port: its inputs received, reads [0, have)
static u8 s_netGone[NET_PLAYERS + 1];           // per port: it left the game
// Per console: the room said it left while it was still heard directly (the room had lost it, not the game): it
// leaves only once the direct way from it goes quiet too.
static u8 s_netByeHeld[NET_PLAYERS + 1];
static int s_netFromRoom;                       // the message NetMessage reads came through the room
static H64Pad s_netIn[NET_PLAYERS + 1][NET_RING];   // per port: its inputs (this console's own too)
static u8 s_netBuf[512];
static u32 s_netBufLen;
static u32 s_netMine[NET_HASHES][2], s_netTheirs[NET_PLAYERS + 1][NET_HASHES][2];   // per frame: frame + 1 (0: none), hash
static u32 s_netDesync, s_netDesyncFrame, s_netWaits;
static double s_netWaitMs, s_netWaitMax;
static DWORD s_netLastRecv, s_netLastSend;   // GetTickCount of the last message in and out
static int s_netRtt[NET_PLAYERS + 1];       // per port: the time there and back, ms (0: not known yet)
static char s_netTagOf[NET_PLAYERS + 1][ONLINE_TAG_MAX + 4];   // per port: its player's gamertag ("" directly: netplay.txt)
static int s_netAvatarOf[NET_PLAYERS + 1];  // ... and avatar
static DWORD s_netPingAt;                   // the next ping (one a second, as the game sends its inputs)
// Directly (xb_p2p.cpp): in a room's game the consoles try UDP to each other as it starts; when every one
// reaches every other, the game's messages go that way and the room only carries the chat (and a ping to
// keep it). A way that goes quiet sends everyone back to the room, the last inputs sent again there; the
// probes go on, and when every console is heard again a while the game's messages go directly again.
#define NET_P2P_PUNCH_MS 3000       // probing each other's addresses
#define NET_P2P_AGREE_MS 2500       // waiting for every console's verdict
#define NET_P2P_QUIET_MS 4000       // nothing directly from a console this long: back to the room (a Wi-Fi hiccup of 1.5 s was not the end of the way)
#define NET_P2P_BACK_MS 3000        // back on the room: every console heard directly this long, and directly again
#define NET_P2P_RETRY_MS 5000       // ... not before this long after leaving the direct way
#define NET_P2P_REPEAT 8            // the last input messages repeated in each packet (a lost one costs nothing; one per port played here)
static int s_netP2p;                        // the game's messages go directly
static int s_netP2pOpen;                    // the UDP socket is open (directly, or trying)
static int s_netP2pOk[NET_PLAYERS + 1];     // per port: its verdict, the ports it reaches (+1; 0: not told yet)
static NetMsg s_netP2pHist[NET_P2P_REPEAT]; // the last input messages, the newest first
static int s_netP2pHistN;
static u32 s_netSentUpTo;                   // reads whose own input is sent: [0, s_netSentUpTo)
// The input delay as a time, whatever the game: s_netDelay counts controller reads, and games read 60 times
// a second (Smash) or 30 and less (Mario Kart: the same 5 reads were twice the time and more). Every
// NET_RATE_VI frames each console counts the reads of those frames - the same on all, the emulation being the
// same - and the delay becomes the reads that make s_netBaseVi frames, changed at the same read on all: own
// inputs already sent are kept (a few presses dropped when it shrinks, the last one repeated when it grows).
#define NET_RATE_VI 180
static u32 s_netBaseVi;                     // the delay in frames (agreed: the hello's delay, 4 directly)
static u32 s_netNextSlot;                   // the next read this console's own input goes to
static u32 s_netRateReads;                  // s_netReads at the last count
static u32 s_netDelayNext;                  // a new delay, from the next read on (0: none)
static DWORD s_netRelayPingAt;              // directly: the next ping that keeps the room open
static DWORD s_netP2pOffAt;                 // the direct way was left (GetTickCount; 0: never, or the game never had one)
static DWORD s_netP2pGoodAt;                // back on the room: since when every console is heard directly (0: not now)
// This console standing still (a screenshot through xbdm froze the whole console, a slow disk...): coming back, the
// others' silence was its own; it gives them NET_P2P_QUIET_MS to be heard again before leaving the direct way or
// ending the game. It took the console that had stood still out of the game.
#define NET_STALL_MS 1500
static DWORD s_netAliveAt;                  // the last time NetPump ran (0: not in a game yet)
static DWORD s_netGraceAt;                  // when this console came back from standing still (0: never)
static void NetText(char *line);
static void NetP2pOff(int tell);
// The quick chat (BACK, online through a room): see NetQuickChatInput below.
#define NET_BACK_MENU_MS 1200       // BACK held this long: the menu (online), not the quick chat
#define NET_CHAT_SHOW_MS 14000      // a message stays this long beside the game
#define NET_CHAT_LINES 8
#define QC_TEXTS 32   // the quick chat's texts (T_QC_1...): sent as their number, "@q<n>" (the first 8 as before 2.2.2)
#define QC_CATS 4     // ... in tabs (LB/RB): the game, teasing, notices, greetings - 8 each
static const unsigned char s_qcTab[QC_CATS][8] = {
    { 0, 1, 3, 6, 8, 9, 10, 11 },          // the game: good game, well played, rematch?, let's go, so close, lucky, I won, one more
    { 12, 13, 14, 15, 16, 17, 18, 19 },    // teasing: the best player, sore loser, that was a cheat...
    { 2, 20, 21, 22, 23, 24, 25, 26 },     // notices: wait a moment, the bathroom, back soon, the internet went...
    { 4, 5, 7, 27, 28, 29, 30, 31 } };     // greetings: haha, oops, bye, hi, thanks, sorry, good night, see you tomorrow
struct NetChatLine { char tag[16], text[84]; DWORD at; };
static NetChatLine s_netChat[NET_CHAT_LINES];   // the last messages, the newest last
static int s_netChatCount;
static int s_qcCat;   // the quick chat's tab of texts
static BYTE s_qcLtWas = 255;   // the left trigger (the keyboard's letters or emojis)
static int s_qcOpen, s_qcKbd, s_qcRow, s_qcCol;   // the quick chat open, its keyboard, the selection (row 0 emojis, 1-2 texts, 3 keyboard)
static WORD s_qcPrev;
static char s_qcText[84];
static OnlineKbd s_qcKb;
static IDirect3DTexture9 *s_emojiTex[EMOJI_COUNT];   // made by the overlay (it has the device), kept

IDirect3DTexture9 *EmojiTex(int k)
{
    if (k < 0 || k >= EMOJI_COUNT) return NULL;
    if (!s_emojiTex[k] && UiDevice()) s_emojiTex[k] = UiImageCreate(UiDevice(), s_emojiRgba[k], EMOJI_SIZE, EMOJI_SIZE);
    return s_emojiTex[k];
}

int EmojiCount(void) { return EMOJI_COUNT; }

static u32 NetBuildId(void)
{
    const char *p = __DATE__ " " __TIME__;
    u32 h = 2166136261u;
    while (*p) { h ^= (u8)*p++; h *= 16777619u; }
    return h;
}

static void NetOsd(const char *text, DWORD ms)
{
    _snprintf(s_osd, sizeof(s_osd) - 1, "%s", text);
    s_osd[sizeof(s_osd) - 1] = 0;
    s_osdUntil = GetTickCount() + ms;
    s_osdLen = ms;
    H64_INFO("[net] %s", text);
}

static void NetEnd(const char *why)
{
    char t[160];
    if (s_netSock != INVALID_SOCKET) closesocket(s_netSock);
    s_netSock = INVALID_SOCKET;
    if (s_netWs) NetWsClose(s_netWs);
    s_netWs = NULL;
    if (s_netMode > 0)
    {
        _snprintf(t, sizeof(t) - 1, "EN LINEA: %s - sigues solo", why);
        t[sizeof(t) - 1] = 0;
        NetOsd(t, 12000);
        LogFlush();
    }
    s_netMode = -1;
}

static int NetSend(u32 type, u32 a, u32 b, u32 c)
{
    NetMsg m;
    int sent = 0;
    DWORD t0 = GetTickCount();
    m.type = type; m.a = a; m.b = b; m.c = c;
    if (s_netP2p)
    {
        // Directly: this message, and the last inputs again.
        NetMsg pkt[1 + NET_P2P_REPEAT];
        int k, n = 0;
        pkt[n++] = m;
        for (k = 0; k < s_netP2pHistN; k++) pkt[n++] = s_netP2pHist[k];
        if (NET_KIND(type) == NM_INPUT || NET_KIND(type) == NM_INPUT2)
        {
            memmove(s_netP2pHist + 1, s_netP2pHist, sizeof(s_netP2pHist[0]) * (NET_P2P_REPEAT - 1));
            s_netP2pHist[0] = m;
            if (s_netP2pHistN < NET_P2P_REPEAT) s_netP2pHistN++;
        }
        P2pSend(pkt, n);
        s_netLastSend = GetTickCount();
        return 1;
    }
    if (s_netWs)
    {
        if (NetWsSend(s_netWs, &m, sizeof(m))) { s_netLastSend = GetTickCount(); return 1; }
        NetEnd("conexion perdida (envio)");
        return 0;
    }
    while (s_netSock != INVALID_SOCKET && sent < (int)sizeof(m))
    {
        int n = send(s_netSock, (const char *)&m + sent, sizeof(m) - sent, 0);
        if (n > 0) { sent += n; s_netLastSend = GetTickCount(); continue; }
        if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK && GetTickCount() - t0 < 5000) { Sleep(1); continue; }
        NetEnd("conexion perdida (envio)");
        return 0;
    }
    return sent == (int)sizeof(m);
}

// The console a message is from (its first port): the server's stamp, or directly the one other console.
static int NetFrom(const NetMsg *m)
{
    const int p = (int)(m->type >> 24);
    if (!p) return s_netPort == 1 ? 2 : 1;
    return p >= 1 && p <= s_netPlayers && p != s_netPort && NET_CONSOLE(p) ? p : 0;
}

// The other consoles' ports still playing.
static int NetOthers(void)
{
    int p, n = 0;
    for (p = 1; p <= s_netPlayers; p++)
        if (!NET_MINE(p) && !s_netGone[p]) n++;
    return n;
}

static void NetHashCheck(int port, u32 slot)
{
    if (!s_netMine[slot][0] || s_netMine[slot][0] != s_netTheirs[port][slot][0] || s_netMine[slot][1] == s_netTheirs[port][slot][1]) return;
    if (!s_netDesync++)
    {
        char t[96];
        s_netDesyncFrame = s_netMine[slot][0] - 1;
        _snprintf(t, sizeof(t) - 1, "EN LINEA: DESINCRONIZADO en el frame %u", s_netDesyncFrame);
        t[sizeof(t) - 1] = 0;
        NetOsd(t, 15000);
    }
    if (s_netDesync <= 20)
        H64_INFO("[net] frame %u: state hash %08X here, %08X at port %d", s_netMine[slot][0] - 1, s_netMine[slot][1], s_netTheirs[port][slot][1], port);
}

// Console p (its first port) left the game: its controllers let go. 0 when the game is over for this one (nobody left).
static int NetConsoleLeft(int p)
{
    char t[80];
    int o;
    for (o = 1; o <= s_netPlayers; o++)
        if (s_netOwner[o] == p) s_netGone[o] = 1;   // every port of that console
    if (!NetOthers()) { NetEnd(s_netPlayers > 2 ? "los demas salieron del juego" : "la otra consola salio del juego"); return 0; }
    _snprintf(t, sizeof(t) - 1, "EN LINEA: el jugador %d salio del juego", p);
    t[sizeof(t) - 1] = 0;
    NetOsd(t, 9000);
    return 1;
}

// One message of another console's. 0 when the game is over for this one (nobody left).
static int NetMessage(const NetMsg *m)
{
    const int p = NetFrom(m), q = p + (int)NET_SUB(m->type);   // q: the port an input is of
    const u32 type = NET_KIND(m->type);
    if (!p) return 1;
    if ((type == NM_INPUT || type == NM_INPUT2) && q <= s_netPlayers && s_netOwner[q] == p && !s_netGone[q])
    {
        const u32 n = type == NM_INPUT2 ? 2 : 1;
        u32 k;
        for (k = 0; k < n; k++)
        {
            const u32 v = k ? m->c : m->b;
            H64Pad *pad = &s_netIn[q][(m->a + k) % NET_RING];
            pad->buttons = (u16)(v >> 16);
            pad->x = (s8)(v >> 8);
            pad->y = (s8)v;
        }
        if (m->a + n > s_netHave[q]) s_netHave[q] = m->a + n;
    }
    else if (type == NM_HASH)
    {
        const u32 slot = m->a % NET_HASHES;
        s_netTheirs[p][slot][0] = m->a + 1;
        s_netTheirs[p][slot][1] = m->b;
        NetHashCheck(p, slot);
    }
    else if (type == NM_PING) NetSend(NM_PONG, m->a, (u32)p, 0);
    else if (type == NM_PONG && (int)m->b == s_netPort)
    {
        const int ms = (int)(GetTickCount() - m->a);
        if (ms >= 0 && ms < 10000) s_netRtt[p] = s_netRtt[p] ? (s_netRtt[p] * 3 + ms) / 4 : ms;
    }
    else if (type == NM_BYE && !s_netGone[p])
    {
        // Its controller lets go after the last input it sent (the same read on every console: the server
        // passes a console's messages in order, its leaving last). The room's word for a console still heard
        // directly is its own lost connection to the room (silent there 60 s), not the console leaving: kept until
        // the direct way from it goes quiet as well (NetPump). It ended games that went on fine directly.
        if (s_netFromRoom && s_netP2p && GetTickCount() - P2pLastHeard(p) < NET_P2P_QUIET_MS)
        {
            if (!s_netByeHeld[p]) H64_INFO("[net] the room says console %d left, but it is heard directly: kept (read %u)", p, s_netReads);
            s_netByeHeld[p] = 1;
            return 1;
        }
        return NetConsoleLeft(p);
    }
    return 1;
}

// Reads what has arrived; `ms`: how long to wait for something when nothing has (0: not at all).
static void NetPump(int ms)
{
    {
        const DWORD now = GetTickCount();
        if (s_netAliveAt && now - s_netAliveAt > NET_STALL_MS && s_netMode > 0)
        {
            H64_INFO("[net] this console stood still %u ms: the others get time to be heard again (read %u)", (u32)(now - s_netAliveAt), s_netReads);
            s_netLastRecv = now;
            s_netGraceAt = now;
            NetOsd("EN LINEA: tu consola se detuvo un momento - recuperando la conexion", 5000);
        }
        s_netAliveAt = now;
    }
    if (s_netP2pOpen)
    {
        NetMsg m;
        int port, p;
        while (s_netMode > 0 && P2pRecv(&m, &port))
        {
            m.type = (m.type & 0xFFFFFFu) | ((u32)port << 24);
            s_netLastRecv = GetTickCount();
            if (!NetMessage(&m)) return;
        }
        if (s_netMode > 0 && s_netP2p)
        {
            // A console the room already said left (s_netByeHeld) and now quiet directly too: it left.
            for (p = 1; p <= s_netPlayers; p++)
                if (s_netByeHeld[p] && !s_netGone[p] && GetTickCount() - P2pLastHeard(p) > NET_P2P_QUIET_MS)
                {
                    s_netByeHeld[p] = 0;
                    if (!NetConsoleLeft(p)) return;
                }
            for (p = 1; p <= s_netPlayers && GetTickCount() - s_netGraceAt > NET_P2P_QUIET_MS; p++)
                if (NET_CONSOLE(p) && p != s_netPort && !s_netGone[p] && GetTickCount() - P2pLastHeard(p) > NET_P2P_QUIET_MS)
                {
                    // Without the room (lost during the game) there is no other way: the game ends.
                    if (!s_netWs) { NetEnd("conexion perdida (sin servidor ni conexion directa)"); return; }
                    NetP2pOff(1);
                    break;
                }
            if (s_netP2p && s_netWs && (int)(GetTickCount() - s_netRelayPingAt) >= 0)
            {
                NetWsSend(s_netWs, "ping\t0", 6, 1);   // the room closes when nothing comes for a while
                s_netRelayPingAt = GetTickCount() + 20000;
            }
        }
        else if (s_netMode > 0 && s_netP2pOffAt)
        {
            // On the room since the direct way went quiet: the probes go on (one each 100 ms, nothing waits); every
            // console reached both ways and heard lately, for NET_P2P_BACK_MS: directly again. Each console decides for
            // itself - what arrives either way is used, and the others' probes keep the quiet check fed meanwhile.
            const DWORD now = GetTickCount();
            int all = 1;
            P2pPunch(0);
            for (p = 1; p <= s_netPlayers; p++)
                if (NET_CONSOLE(p) && p != s_netPort && !s_netGone[p] && (!P2pReached(p) || now - P2pLastHeard(p) > 500)) all = 0;
            if (!all || now - s_netP2pOffAt < NET_P2P_RETRY_MS) s_netP2pGoodAt = 0;
            else if (!s_netP2pGoodAt) s_netP2pGoodAt = now;
            else if (now - s_netP2pGoodAt >= NET_P2P_BACK_MS)
            {
                s_netP2p = 1;
                s_netP2pOffAt = s_netP2pGoodAt = 0;
                s_netBaseVi = 4;
                s_netRelayPingAt = now + 20000;
                H64_INFO("[net] direct way back (read %u)", s_netReads);
                NetOsd("EN LINEA: conexion directa recuperada", 6000);
            }
        }
    }
    while (s_netWs)
    {
        NetMsg m;
        char buf[320];
        int text = 0, n = NetWsRecv(s_netWs, buf, sizeof(buf) - 1, ms, &text);
        ms = 0;
        if (n < 0)
        {
            H64_WARN("[net] the room's connection is over: %s (read %u)", NetWsWhy(s_netWs), s_netReads);
            if (s_netP2p)
            {
                // Playing directly with every console: the game goes on that way, without the room (its chat and its
                // way back if the direct one fails). Ending it here ended 20-minute games that went on fine directly.
                NetWsClose(s_netWs);
                s_netWs = NULL;
                NetOsd("EN LINEA: sin conexion al servidor - sigues por conexion directa", 8000);
                return;
            }
            NetEnd("conexion perdida");
            return;
        }
        if (n == 0) return;
        s_netLastRecv = GetTickCount();
        if (text) { buf[n] = 0; NetText(buf); continue; }   // the room's words (chat, menus)
        if (n != (int)sizeof(m)) continue;
        memcpy(&m, buf, sizeof(m));
        s_netFromRoom = 1;
        n = NetMessage(&m);
        s_netFromRoom = 0;
        if (!n) return;
    }
    if (ms > 0 && s_netSock != INVALID_SOCKET)
    {
        fd_set rd;
        timeval tv = { 0, ms * 1000 };
        FD_ZERO(&rd);
        FD_SET(s_netSock, &rd);
        select(0, &rd, NULL, NULL, &tv);
    }
    while (s_netSock != INVALID_SOCKET)
    {
        int n = recv(s_netSock, (char *)s_netBuf + s_netBufLen, sizeof(s_netBuf) - s_netBufLen, 0);
        if (n == 0) { NetEnd("la otra consola cerro la conexion"); return; }
        if (n < 0)
        {
            if (WSAGetLastError() != WSAEWOULDBLOCK) NetEnd("conexion perdida");
            return;
        }
        s_netBufLen += n;
        s_netLastRecv = GetTickCount();
        u32 at = 0;
        while (s_netBufLen - at >= sizeof(NetMsg))
        {
            NetMsg m;
            memcpy(&m, s_netBuf + at, sizeof(m));
            at += sizeof(m);
            if (!NetMessage(&m)) return;
        }
        memmove(s_netBuf, s_netBuf + at, s_netBufLen - at);
        s_netBufLen -= at;
    }
}

// Read n can go on: every other console's port still playing has its input for it.
static int NetReady(u32 n)
{
    int p;
    for (p = 1; p <= s_netPlayers; p++)
        if (!NET_MINE(p) && !s_netGone[p] && s_netHave[p] <= n) return 0;
    return 1;
}

static u32 NetPadWord(const H64Pad *p) { return ((u32)p->buttons << 16) | ((u32)(u8)p->x << 8) | (u8)p->y; }

static void NetPad(H64System *sys)
{
    H64Pad local[NET_PLAYERS];
    static const H64Pad zero = { 0, 0, 0 };
    u32 n = s_netReads++, made = 0, s;
    int p, k;
    double w0 = QpcMs();
    if (s_netDelayNext)
    {
        H64_INFO("[net] delay %u -> %u reads (read %u)", s_netDelay, s_netDelayNext, n);
        s_netDelay = s_netDelayNext;
        s_netDelayNext = 0;
    }
    // This console's controllers, one per port it plays (its first: the one whose BACK opens the quick chat).
    for (k = 0; k < s_netLocal; k++)
    {
        XINPUT_STATE xs;
        local[k] = zero;
        if (k == 0)
        {
            if (s_padValid && !(s_pad.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) && !s_qcOpen) PadFromXInput(&s_pad.Gamepad, 0, &local[0]);   // the quick chat: it plays nothing
        }
        else if (XInputGetState(k, &xs) == ERROR_SUCCESS) PadFromXInput(&xs.Gamepad, k, &local[k]);
    }
    // This read's input for read n + delay: none when that one is sent already (the delay shrank), the reads
    // up to it too when it grew (all ports go on without a gap).
    while (s_netNextSlot <= n + s_netDelay)
    {
        for (k = 0; k < s_netLocal; k++) s_netIn[s_netPort + k][s_netNextSlot % NET_RING] = local[k];
        s_netNextSlot++;
        made++;
    }
    if (s_netSock != INVALID_SOCKET)   // netplay.txt's direct TCP: one port each (not a room that lost its server)
    {
        for (; s_netSentUpTo < s_netNextSlot; s_netSentUpTo++)
            NetSend(NM_INPUT, s_netSentUpTo, NetPadWord(&s_netIn[s_netPort][s_netSentUpTo % NET_RING]), 0);
    }
    else
    {
        // Through a room: two reads' inputs in one message (each sent a read later at most: the delay still covers
        // it); one alone when no other follows this read. One message per port played here.
        while (s_netNextSlot - s_netSentUpTo >= 2)
        {
            for (k = 0, s = s_netSentUpTo; k < s_netLocal; k++)
                NetSend(NM_INPUT2 | ((u32)k << 16), s, NetPadWord(&s_netIn[s_netPort + k][s % NET_RING]), NetPadWord(&s_netIn[s_netPort + k][(s + 1) % NET_RING]));
            s_netSentUpTo += 2;
        }
        if (!made && s_netSentUpTo < s_netNextSlot)
        {
            for (k = 0; k < s_netLocal; k++)
                NetSend(NM_INPUT | ((u32)k << 16), s_netSentUpTo, NetPadWord(&s_netIn[s_netPort + k][s_netSentUpTo % NET_RING]), 0);
            s_netSentUpTo++;
        }
    }
    if ((int)(GetTickCount() - s_netPingAt) >= 0)
    {
        NetSend(NM_PING, GetTickCount(), 0, 0);   // the others answer: the time there and back (the ping shown)
        s_netPingAt = GetTickCount() + 1000;
    }
    NetPump(0);
    if (s_netMode > 0 && !NetReady(n))
    {
        const DWORD waitAt = GetTickCount();
        DWORD shownAt = waitAt;
        int told = 0;
        s_netWaits++;
        while (s_netMode > 0 && !NetReady(n))
        {
            NetPump(2);
            // Still here. Directly, every NET_P2P_NUDGE_MS: each message carries this console's last inputs again, and
            // a packet lost on the way left both consoles waiting for each other until the next ping (~0.5 s
            // freezes at the same frame on both, Mario Kart 2026-11-18).
            if (s_netMode > 0 && GetTickCount() - s_netLastSend >= (s_netP2p ? NET_P2P_NUDGE_MS : NET_PING_MS)) NetSend(NM_PING, GetTickCount(), 0, 0);
            if (s_netMode > 0 && !NetReady(n) && GetTickCount() - s_netLastRecv > NET_TIMEOUT_MS) NetEnd("las demas consolas no responden");
            if (s_netMode > 0 && !told && GetTickCount() - waitAt >= 1000)
            {
                // A wait the players notice: who the game waits for (the first console whose input has not come).
                char t[96];
                for (p = 1; p <= s_netPlayers; p++)
                    if (!NET_MINE(p) && !s_netGone[p] && s_netHave[p] <= n) break;
                if (p <= s_netPlayers && s_netTagOf[p][0]) _snprintf(t, sizeof(t) - 1, "EN LINEA: esperando a %s - conexion lenta", s_netTagOf[p]);
                else _snprintf(t, sizeof(t) - 1, "EN LINEA: esperando al jugador %d - conexion lenta", p <= s_netPlayers ? p : 0);
                t[sizeof(t) - 1] = 0;
                NetOsd(t, 4000);
                told = 1;
            }
            if (s_workRenderer && GetTickCount() - waitAt >= 250 && GetTickCount() - shownAt >= 100)
            {
                // A long wait (another player in the menu): the frame shown again, with what the others say over it.
                PresentFrame(sys, s_workRenderer);
                shownAt = GetTickCount();
            }
        }
        double w = QpcMs() - w0;
        s_netWaitMs += w;
        if (w > s_netWaitMax) s_netWaitMax = w;
    }
    // Each port: this console's own input, another's (neutral from the read after the last one it sent
    // when it left), or this console alone when the game is over for it.
    sys->padMask = (1u << s_netPlayers) - 1u;
    for (p = 1; p <= NET_PLAYERS; p++)
    {
        H64Pad *out = &sys->pad[p - 1];
        if (p > s_netPlayers) *out = zero;
        else if (NET_MINE(p)) *out = s_netIn[p][n % NET_RING];
        else if (s_netMode > 0 && n < s_netHave[p]) *out = s_netIn[p][n % NET_RING];
        else *out = zero;
    }
    if ((n % 1800) == 1799)
    {
        H64_INFO("[net] %u reads: waited %u times, %.0f ms in all (longest %.1f ms), %u frames out of step", n + 1, s_netWaits,
                 s_netWaitMs, s_netWaitMax, s_netDesync);
        s_netWaitMax = 0;
    }
}

// The online game over for this console in the middle of the game (the others left, or the connection was lost:
// "sigues solo"): each controller here keeps the port it played, the other ports stay plugged in, at rest. Back on
// the ports of a game played alone, player 2's controller took player 1's character and the others' players were
// unplugged from under the game.
static void NetPadAlone(H64System *sys)
{
    static const H64Pad zero = { 0, 0, 0 };
    int p, k;
    sys->padMask = (1u << s_netPlayers) - 1u;
    for (p = 0; p < NET_PLAYERS; p++) sys->pad[p] = zero;
    for (k = 0; k < s_netLocal && s_netPort + k <= s_netPlayers; k++)
    {
        H64Pad *out = &sys->pad[s_netPort - 1 + k];
        if (k == 0)
        {
            if (s_padValid && !(s_pad.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)) PadFromXInput(&s_pad.Gamepad, 0, out);
        }
        else if (s_padsMoreValid[k - 1]) PadFromXInput(&s_padsMore[k - 1].Gamepad, k, out);
    }
}

static void NetVi(H64System *sys)
{
    u32 f = sys->vi.frames, slot = f % NET_HASHES, cpu;
    const u32 turn = f / NET_HASH_EVERY, block = turn & 7;   // the MB of RDRAM this frame's hash is of
    int p;
    if (s_netMode > 0 && f && f % NET_RATE_VI == 0)
    {
        // The reads of the last NET_RATE_VI frames (a loading screen that hardly reads changes nothing).
        const u32 reads = s_netReads - s_netRateReads;
        s_netRateReads = s_netReads;
        if (reads >= NET_RATE_VI / 6)
        {
            // In quarters of a read: changed only when it is 3/4 of a read away from the delay now (a game reading
            // 1.1 times a frame made 4.4 and the delay went 4, 5, 4, 5... every few seconds).
            const u32 q = (reads * s_netBaseVi * 4 + NET_RATE_VI / 2) / NET_RATE_VI;
            u32 d = (q + 2) / 4;
            if (d < 1) d = 1;
            if (d > NET_RING / 4) d = NET_RING / 4;
            if (d != s_netDelay && (q >= s_netDelay * 4 + 3 || q + 3 <= s_netDelay * 4)) s_netDelayNext = d;
        }
    }
    if (s_netMode <= 0 || f % NET_HASH_EVERY) return;
    // What the graphics and audio workers write lands first: the network's waits move them against the CPU,
    // and a hash read in the middle of their writes differed between the consoles (RDRAM's first MB, in bursts).
    if (sys->asyncWait) sys->asyncWait(sys->asyncUser);
    if (sys->asyncAudioWait) sys->asyncAudioWait(sys->asyncUser);
    h64_system_state_hash(sys, &cpu, NULL);
    s_netMine[slot][0] = f + 1;
    s_netMine[slot][1] = NetHashBlock(sys->rdram + (block << 20)) ^ cpu;
    NetSend(NM_HASH, f, s_netMine[slot][1], 0);
    for (p = 1; p <= s_netPlayers; p++)
        if (NET_CONSOLE(p) && p != s_netPort) NetHashCheck(p, slot);
}

// Before the saves are opened: whether this game is played online (a room of Settings > Online play, or
// game:\netplay.txt).
static void NetConfig(H64System *sys)
{
    char line[96] = { 0 };
    FILE *f;
    int d = 0, port = 0, players = 0, local = 1, p;
    u8 owner[NET_PLAYERS + 1];
    NetWs *room = OnlineRoomTake(&port, &players, &local, owner);
    s_netMode = 0;
    s_netDelay = 3;
    s_netLocal = 1;
    for (p = 0; p <= NET_PLAYERS; p++) s_netOwner[p] = (u8)p;   // one port each (directly, netplay.txt)
    memset(s_netTagOf, 0, sizeof(s_netTagOf));
    memset(s_netAvatarOf, 0, sizeof(s_netAvatarOf));
    if (room)
    {
        if (players < 2 || players > NET_PLAYERS || port < 1 || port > players || local < 1 || port + local - 1 > players) { NetWsClose(room); return; }
        // Who owns each port (the server's start line): this console the ones from its first.
        for (p = 1; p <= players; p++) s_netOwner[p] = owner[p] >= 1 && owner[p] <= players ? owner[p] : (u8)p;
        for (p = port; p < port + local; p++) s_netOwner[p] = (u8)port;
        {
            // Each port's player, from the room as it started: its consoles in order, each its ports from the
            // next free one (the server's start); a console's second player "TAG 2".
            const OnlineLobby *lb = OnlineRoomLobby();
            int k, next = 1, j;
            for (k = 0; k < lb->count && k < ONLINE_ROOM_PLAYERS; k++)
                for (j = 0; j < (lb->local[k] >= 1 ? lb->local[k] : 1) && next <= players; j++, next++)
                {
                    if (j) _snprintf(s_netTagOf[next], sizeof(s_netTagOf[0]) - 1, "%.12s %d", lb->tag[k], j + 1);
                    else _snprintf(s_netTagOf[next], sizeof(s_netTagOf[0]) - 1, "%s", lb->tag[k]);
                    s_netAvatarOf[next] = lb->avatar[k];
                }
        }
        s_netLocal = local;
        s_netWs = room;
        s_netPort = port;
        s_netPlayers = players;
        s_netMode = port == 1 ? 1 : 2;
        s_netPlayed = 1;
        s_netDelay = NET_DELAY_RELAY;
        s_rumblePort = port - 1;
        s_rumbleLocal = local;
        strcpy(s_netPeer, "sala");
        sys->viHook = NetVi;
        H64_INFO("[net] online room: port %d (%d here) of %d, delay %u reads", port, local, players, s_netDelay);
        return;
    }
    f = fopen("game:\\netplay.txt", "rb");
    if (!f) return;
    fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    s_netPeer[0] = 0;
    if (!strncmp(line, "host", 4)) { s_netMode = 1; d = atoi(line + 4); }
    else if (!strncmp(line, "join", 4) && sscanf(line + 4, "%63s %d", s_netPeer, &d) >= 1) s_netMode = 2;
    if (s_netMode > 0) s_netPlayed = 1;
    if (d > 0 && d < 60) s_netDelay = (u32)d;
    s_netPort = s_netMode;
    s_netPlayers = 2;
    if (s_netMode) { s_rumblePort = s_netMode - 1; s_rumbleLocal = 1; }
    if (s_netMode) sys->viHook = NetVi;
    H64_INFO("[net] netplay.txt: %s %s, delay %u reads", s_netMode == 1 ? "host" : s_netMode == 2 ? "join" : "nothing usable in", s_netPeer, s_netDelay);
}

static int NetRecvWait(NetMsg *m, DWORD ms)
{
    int got = 0;
    DWORD t0 = GetTickCount();
    if (s_netWs)
    {
        for (;;)
        {
            int text = 0, left = (int)ms - (int)(GetTickCount() - t0), n;
            n = NetWsRecv(s_netWs, m, sizeof(*m), left > 0 ? left : 0, &text);
            if (n == (int)sizeof(*m) && !text) return 1;
            if (n <= 0) return 0;   // over, or nothing in time (a text line of the server's is passed over)
        }
    }
    while (got < (int)sizeof(*m) && GetTickCount() - t0 < ms)
    {
        fd_set rd;
        timeval tv = { 0, 100000 };
        FD_ZERO(&rd);
        FD_SET(s_netSock, &rd);
        if (select(0, &rd, NULL, NULL, &tv) <= 0) continue;
        int n = recv(s_netSock, (char *)m + got, sizeof(*m) - got, 0);
        if (n <= 0) return 0;
        got += n;
    }
    return got == (int)sizeof(*m);
}

// Back to the room's relay (a direct way went quiet, or another console said so): this console's last inputs
// sent again there (the others may have missed them on the way that failed; an input sent twice changes nothing).
static void NetP2pOff(int tell)
{
    u32 r;
    int k;
    s_netP2p = 0;
    s_netBaseVi = NET_DELAY_RELAY;   // the room's delay from the next count (the server's way is longer)
    s_netP2pOffAt = GetTickCount();  // the probes go on: NetPump takes the direct way again when it works
    s_netP2pGoodAt = 0;
    H64_INFO("[net] direct way quiet: through the room (read %u)", s_netReads);
    if (!s_netWs) return;
    if (tell) NetWsSend(s_netWs, "p2p\toff", 7, 1);
    for (r = s_netSentUpTo > 128 ? s_netSentUpTo - 128 : 0; r < s_netSentUpTo; r += 2)
        for (k = 0; k < s_netLocal; k++)
        {
            const int q = s_netPort + k;
            if (r + 1 < s_netSentUpTo) NetSend(NM_INPUT2 | ((u32)k << 16), r, NetPadWord(&s_netIn[q][r % NET_RING]), NetPadWord(&s_netIn[q][(r + 1) % NET_RING]));
            else NetSend(NM_INPUT | ((u32)k << 16), r, NetPadWord(&s_netIn[q][r % NET_RING]), 0);
        }
    NetOsd("EN LINEA: conexion directa perdida - por el servidor", 8000);
}

// A room's game as it starts: directly when every console reaches every other (all decide the same, from the
// verdicts all of them hear; any doubt keeps the room).
static void NetP2pSetup(u32 token)
{
    char cand[80], line[128];
    int p, mask = 0, all, full = 0;
    DWORD t0;
    memset(s_netP2pOk, 0, sizeof(s_netP2pOk));
    s_netP2p = 0;
    s_netP2pHistN = 0;
    s_netP2pOffAt = s_netP2pGoodAt = 0;
    s_netP2pOpen = P2pOpen(token, s_netPort, cand, sizeof(cand));
    if (s_netP2pOpen)
    {
        _snprintf(line, sizeof(line) - 1, "p2p\tcand %s", cand);
        line[sizeof(line) - 1] = 0;
        NetWsSend(s_netWs, line, (int)strlen(line), 1);
        for (t0 = GetTickCount(); s_netMode > 0 && GetTickCount() - t0 < NET_P2P_PUNCH_MS;)
        {
            NetPump(5);
            mask = P2pPunch(20);
        }
    }
    for (p = 1; p <= s_netPlayers; p++)
        if (NET_CONSOLE(p)) full |= 1 << p;   // the consoles (their first ports)
    _snprintf(line, sizeof(line) - 1, "p2p\tok %d", mask);
    line[sizeof(line) - 1] = 0;
    NetWsSend(s_netWs, line, (int)strlen(line), 1);
    s_netP2pOk[s_netPort] = mask + 1;
    for (t0 = GetTickCount(); s_netMode > 0 && GetTickCount() - t0 < NET_P2P_AGREE_MS;)
    {
        NetPump(5);
        if (s_netP2pOpen) P2pPunch(20);   // the others may still be probing
        for (all = 1, p = 1; p <= s_netPlayers; p++)
            if (NET_CONSOLE(p) && !s_netP2pOk[p]) all = 0;
        if (all) break;
    }
    for (all = 1, p = 1; p <= s_netPlayers; p++)
        if (NET_CONSOLE(p) && (!s_netP2pOk[p] || (((s_netP2pOk[p] - 1) | (1 << p)) & full) != full)) all = 0;
    H64_INFO("[net] direct: reaches %X here; verdicts %X %X %X %X -> %s", mask, s_netP2pOk[1] - 1, s_netP2pOk[2] - 1, s_netP2pOk[3] - 1,
             s_netP2pOk[4] - 1, all && s_netP2pOpen ? "directly" : "through the room");
    if (all && s_netP2pOpen)
    {
        s_netP2p = 1;
        s_netRelayPingAt = GetTickCount() + 20000;
        return;
    }
    // Not every way works: the room for everyone (a console that decided otherwise is told).
    NetWsSend(s_netWs, "p2p\toff", 7, 1);
    if (s_netP2pOpen) P2pClose();
    s_netP2pOpen = 0;
}

// After the HD pack is loaded, before the game runs: the connection and the check that all run the same.
static void NetConnect(H64System *sys, const Config *c)
{
    sockaddr_in sa;
    BOOL one = TRUE;
    u_long nb = 1;
    NetMsg hello, theirs;
    DWORD t0 = GetTickCount();
    const int relay = s_netWs != NULL;   // connected already, through the server's room (NetConfig)
    int heard = 0, p, consoles = 0;
    if (s_netMode <= 0) return;
    if (!relay && !NetStart(1)) { NetEnd("sin red"); return; }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(NET_PORT);
    if (relay) H64_INFO("[net] online room: %d players", s_netPlayers);
    else if (s_netMode == 1)
    {
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        setsockopt(ls, SOL_SOCKET, SO_MARKINSECURE, (const char *)&one, sizeof(one));
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
        if (ls == INVALID_SOCKET || bind(ls, (const sockaddr *)&sa, sizeof(sa)) != 0 || listen(ls, 1) != 0)
        {
            H64_INFO("[net] cannot listen on port %d (%d)", NET_PORT, WSAGetLastError());
            if (ls != INVALID_SOCKET) closesocket(ls);
            NetEnd("no se pudo abrir el puerto");
            return;
        }
        H64_INFO("[net] host: waiting for the other console on port %d (3 minutes)", NET_PORT);
        LogFlush();
        while (s_netSock == INVALID_SOCKET && GetTickCount() - t0 < 180000)
        {
            fd_set rd;
            timeval tv = { 0, 200000 };
            FD_ZERO(&rd);
            FD_SET(ls, &rd);
            if (select(0, &rd, NULL, NULL, &tv) > 0)
            {
                sockaddr_in from;
                int fl = sizeof(from);
                s_netSock = accept(ls, (sockaddr *)&from, &fl);
                const u8 *ip = (const u8 *)&from.sin_addr;
                if (s_netSock != INVALID_SOCKET) _snprintf(s_netPeer, sizeof(s_netPeer) - 1, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            }
        }
        closesocket(ls);
    }
    else
    {
        sa.sin_addr.s_addr = inet_addr(s_netPeer);
        H64_INFO("[net] guest: connecting to %s:%d (3 minutes)", s_netPeer, NET_PORT);
        LogFlush();
        while (s_netSock == INVALID_SOCKET && GetTickCount() - t0 < 180000)
        {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            setsockopt(s, SOL_SOCKET, SO_MARKINSECURE, (const char *)&one, sizeof(one));
            if (connect(s, (const sockaddr *)&sa, sizeof(sa)) == 0) s_netSock = s;
            else { closesocket(s); Sleep(1000); }
        }
    }
    if (!relay)
    {
        if (s_netSock == INVALID_SOCKET) { NetEnd("la otra consola no aparecio"); return; }
        setsockopt(s_netSock, SOL_SOCKET, SO_MARKINSECURE, (const char *)&one, sizeof(one));
        setsockopt(s_netSock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    }
    // All must run the same: build, ROM, CPU mode. Directly the host's delay is the one used.
    hello.type = NM_HELLO;
    hello.a = (NET_PROTO << 24) | (s_netDelay << 16) | (c->softRenderer ? 0x400u : 0u) | (c->hle ? 0x200u : 0u) | ((sys->jit ? 1u : 0u) << 8) | (u8)c->cpi;
    hello.b = sys->rom.crc1;
    hello.c = NetBuildId() ^ sys->rom.crc2;
    if (relay ? !NetWsSend(s_netWs, &hello, sizeof(hello)) : send(s_netSock, (const char *)&hello, sizeof(hello), 0) != sizeof(hello)) { NetEnd("no hubo saludo"); return; }
    // One hello from each of the other consoles (they may still be loading the game).
    for (p = 1; p <= s_netPlayers; p++)
        if (NET_CONSOLE(p) && p != s_netPort) consoles++;
    while (heard < consoles)
    {
        const int left = (relay ? 90000 : 10000) - (int)(GetTickCount() - t0);
        if (left <= 0 || !NetRecvWait(&theirs, left)) { NetEnd("no hubo saludo"); return; }
        if (NET_KIND(theirs.type) != NM_HELLO) continue;
        heard++;
        H64_INFO("[net] hello of port %d: here %08X %08X %08X, there %08X %08X %08X", NetFrom(&theirs), hello.a, hello.b, hello.c, theirs.a, theirs.b, theirs.c);
        if (theirs.a >> 24 != NET_PROTO || theirs.c != hello.c) { NetEnd("las consolas no tienen la misma build"); return; }
        if (theirs.b != hello.b) { NetEnd("no es la misma ROM"); return; }
        if ((theirs.a & 0xFFFF) != (hello.a & 0xFFFF)) { NetEnd("ajustes de CPU, RSP o video distintos"); return; }
        if (!relay && s_netPort == 2) s_netDelay = (theirs.a >> 16) & 0xFF;
    }
    if (!relay) ioctlsocket(s_netSock, FIONBIO, &nb);
    s_netReads = 0;
    memset(s_netIn, 0, sizeof(s_netIn));
    memset(s_netGone, 0, sizeof(s_netGone));
    memset(s_netByeHeld, 0, sizeof(s_netByeHeld));
    for (p = 0; p <= NET_PLAYERS; p++) s_netHave[p] = s_netDelay;   // the first reads have no input from anyone: neutral on all
    memset(s_netMine, 0, sizeof(s_netMine));
    memset(s_netTheirs, 0, sizeof(s_netTheirs));
    s_netBufLen = s_netDesync = s_netWaits = 0;
    s_netWaitMs = s_netWaitMax = 0;
    s_netLastRecv = s_netLastSend = GetTickCount();
    s_netAliveAt = s_netGraceAt = 0;
    memset(s_netRtt, 0, sizeof(s_netRtt));
    s_netChatCount = 0;
    s_qcOpen = s_qcKbd = 0;
    s_netSentUpTo = s_netNextSlot = s_netDelay;   // reads [0, delay): neutral, sent by nobody
    s_netRateReads = s_netDelayNext = 0;
    if (relay) NetP2pSetup(hello.b ^ hello.c ^ (u32)s_netPlayers);   // directly if every way works (the same verdict everywhere)
    // The delay in frames from here on (see NET_RATE_VI): the hello's, 4 when the consoles reach each other directly.
    s_netBaseVi = s_netP2p && s_netDelay > 4 ? 4 : s_netDelay;
    {
        char t[128];
        _snprintf(t, sizeof(t) - 1, "EN LINEA: jugador %d de %d, retraso %u, %s", s_netPort, s_netPlayers, s_netDelay,
                  s_netP2p ? "conexion directa" : relay ? "por el servidor" : "directo (prueba)");
        t[sizeof(t) - 1] = 0;
        NetOsd(t, 9000);
    }
}

// Each pass of the game loop and of the pause menu: what has arrived, a ping now and then, and the end of a
// connection that went quiet (a console that left without saying so).
static void NetKeepAlive(void)
{
    if (s_netMode <= 0) return;
    NetPump(0);
    if (s_netMode > 0 && GetTickCount() - s_netLastSend >= NET_PING_MS) NetSend(NM_PING, GetTickCount(), 0, 0);
    if (s_netMode > 0 && GetTickCount() - s_netLastRecv > NET_TIMEOUT_MS) NetEnd("las demas consolas no responden");
}

static void NetStop(void)
{
    if (s_netMode > 0)
    {
        NetSend(NM_BYE, 0, 0, 0);
        H64_INFO("[net] left after %u reads: waited %u times, %.0f ms in all, %u frames out of step (first %u)", s_netReads, s_netWaits,
                 s_netWaitMs, s_netDesync, s_netDesyncFrame);
        LogFlush();
        if (s_netSock != INVALID_SOCKET) closesocket(s_netSock);
        s_netSock = INVALID_SOCKET;
        // Back to the room (the pause menu's): its connection stays open, the room's lobby again.
        if (s_toRoom && s_netWs) OnlineRoomGiveBack(s_netWs);
        else if (s_netWs) NetWsClose(s_netWs);
        s_netWs = NULL;
    }
    s_netMode = 0;
    s_rumblePort = -1;
    s_rumbleLocal = 1;
    s_qcOpen = s_qcKbd = 0;
    if (s_netP2pOpen) P2pClose();
    s_netP2pOpen = s_netP2p = 0;
    s_netP2pOffAt = s_netP2pGoodAt = 0;
}

// What would take one console away from the others: refused online.
static int NetRefuse(void)
{
    if (s_netMode <= 0) return 0;
    NetOsd("No disponible jugando en linea", 5000);
    return 1;
}

// ---- Online play in the game: the quick chat (BACK), the others' messages, their menus, the ping ----
// Through a room of the server only (its text lines: "chat\t<tag>\t<text>", "status\t<port>\t<tag>\t<what>").
// A short BACK opens the quick chat over the running game (the controller plays nothing meanwhile): the
// emojis (a message "@e<n>", xb_emojis.h), quick texts and the keyboard; BACK held opens the usual menu,
// and the others are told this player is in it (the game waits for it there). The messages show beside
// the game for the others, with the name of who wrote them.

static void NetChatAdd(const char *tag, const char *text)
{
    NetChatLine *l;
    if (s_netChatCount == NET_CHAT_LINES)
    {
        memmove(s_netChat, s_netChat + 1, sizeof(s_netChat[0]) * (NET_CHAT_LINES - 1));
        s_netChatCount--;
    }
    l = &s_netChat[s_netChatCount++];
    // A quick chat text ("@q<n>"): in this console's language.
    if (text[0] == '@' && text[1] == 'q' && text[2] >= '0' && text[2] <= '9' && (!text[3] || (text[3] >= '0' && text[3] <= '9' && !text[4])))
    {
        const int q = atoi(text + 2);
        if (q < QC_TEXTS) text = Tr(T_QC_1 + q);
    }
    _snprintf(l->tag, sizeof(l->tag) - 1, "%s", tag);
    l->tag[sizeof(l->tag) - 1] = 0;
    _snprintf(l->text, sizeof(l->text) - 1, "%s", text);
    l->text[sizeof(l->text) - 1] = 0;
    l->at = GetTickCount();
}

// A text line of the server's in the game.
static void NetText(char *line)
{
    char *f[5];
    int n = 0;
    f[n++] = line;
    while (n < 5 && (f[n] = strchr(f[n - 1], '\t')) != NULL) *f[n]++ = 0, n++;
    if (!strcmp(f[0], "chat") && n >= 3) NetChatAdd(f[1], f[2]);
    else if (!strcmp(f[0], "p2p") && n >= 3)
    {
        // Another console's addresses ("cand ..."), its verdict ("ok <mask>"), or everyone back to the room ("off").
        const int port = atoi(f[1]);
        char *what = f[2];
        if (n >= 4) f[3][-1] = '\t';   // the rest of the line is one field
        if (port >= 1 && port <= NET_PLAYERS && port != s_netPort)
        {
            if (!strncmp(what, "cand ", 5)) P2pPeer(port, what + 5);
            else if (!strncmp(what, "ok ", 3)) s_netP2pOk[port] = atoi(what + 3) + 1;
            else if (!strcmp(what, "off") && s_netP2p) NetP2pOff(0);
        }
    }
    else if (!strcmp(f[0], "status") && n >= 4)
    {
        char t[96];
        const int menu = !strcmp(f[3], "menu");
        _snprintf(t, sizeof(t) - 1, Tr(menu ? T_NET_IN_MENU : T_NET_BACK_GAME), f[2], atoi(f[1]));
        t[sizeof(t) - 1] = 0;
        NetOsd(t, menu ? 120000 : 8000);   // in the menu: shown until it comes back
    }
}

static void NetChatSay(const char *text)
{
    char line[100];
    if (!s_netWs || !text[0]) return;
    _snprintf(line, sizeof(line) - 1, "chat\t%s", text);
    line[sizeof(line) - 1] = 0;
    NetWsSend(s_netWs, line, (int)strlen(line), 1);
}

static void NetStatus(const char *what)
{
    char line[24];
    if (s_netMode <= 0 || !s_netWs) return;
    _snprintf(line, sizeof(line), "status\t%s", what);
    NetWsSend(s_netWs, line, (int)strlen(line), 1);
}

// The keyboard's row of quick phrases above the keys: the game's tab of the quick texts, the first QC_PHRASES.
#define QC_PHRASES 6
static int s_qcPhrase, s_qcPhraseCol;   // the keyboard's selection is on that row, and which phrase
static BYTE s_qcRtWas = 255;            // the right trigger (sends the text)

// The left stick as the D-pad on the keyboard: one step when it is pushed, then quick steps while held.
static WORD QcStickDirs(void)
{
    static WORD held;
    static DWORD at;
    WORD dir = 0;
    const DWORD now = GetTickCount();
    if (s_padValid)
    {
        const SHORT lx = s_pad.Gamepad.sThumbLX, ly = s_pad.Gamepad.sThumbLY;
        if (lx < -16000) dir |= XINPUT_GAMEPAD_DPAD_LEFT;
        else if (lx > 16000) dir |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (ly > 16000) dir |= XINPUT_GAMEPAD_DPAD_UP;
        else if (ly < -16000) dir |= XINPUT_GAMEPAD_DPAD_DOWN;
    }
    if (dir != held) { held = dir; at = now + 260; return dir; }
    if (dir && (int)(now - at) >= 0) { at = now + 70; return dir; }
    return 0;
}

// The keyboard, empty, on the letters (from the quick chat's last row, or a double press of BACK in the game).
static void NetQuickChatKeyboard(void)
{
    s_qcText[0] = 0;
    kbd_init(&s_qcKb, s_qcText, 1, 80, KBD_CHAT);
    s_qcOpen = 1;
    s_qcKbd = 1;
    s_qcLtWas = s_qcRtWas = 255;   // triggers already held fire nothing
    s_qcPhrase = s_qcPhraseCol = 0;
    s_qcPrev = 0xFFFF;
}

// The quick chat's buttons, each frame while it is open.
static void NetQuickChatInput(void)
{
    const WORD b = s_padValid ? s_pad.Gamepad.wButtons : 0, down = b & ~s_qcPrev & ~(WORD)XINPUT_GAMEPAD_BACK;
    const int cols = s_qcRow == 0 ? EMOJI_COUNT : s_qcRow <= 2 ? 4 : 1;
    s_qcPrev = b;
    if (s_qcKbd)
    {
        // A writes, LB (or X) deletes, RB a space, Y capitals, LT the emojis, RT sends (START no more), B back;
        // the stick moves like the D-pad, quickly when held; above the first row, a row of quick phrases.
        const BYTE lt = s_padValid ? s_pad.Gamepad.bLeftTrigger : 0, rt = s_padValid ? s_pad.Gamepad.bRightTrigger : 0;
        WORD kd = (down & ~(WORD)XINPUT_GAMEPAD_START) | QcStickDirs();
        if (down & XINPUT_GAMEPAD_B) { s_qcKbd = 0; xb_audio_ui_sound(XB_SFX_BACK); return; }
        if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) kd |= XINPUT_GAMEPAD_X;
        if (s_qcPhrase)
        {
            if (kd & XINPUT_GAMEPAD_DPAD_LEFT) s_qcPhraseCol = (s_qcPhraseCol + QC_PHRASES - 1) % QC_PHRASES;
            if (kd & XINPUT_GAMEPAD_DPAD_RIGHT) s_qcPhraseCol = (s_qcPhraseCol + 1) % QC_PHRASES;
            if (kd & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) xb_audio_ui_sound(XB_SFX_MOVE);
            if (kd & (XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_UP)) { s_qcPhrase = 0; xb_audio_ui_sound(XB_SFX_MOVE); }
            else if (kd & XINPUT_GAMEPAD_A)
            {
                // Its number, not its text: each console shows it in its own language (NetChatAdd).
                char msg[16];
                _snprintf(msg, sizeof(msg), "@q%d", s_qcTab[0][s_qcPhraseCol]);
                NetChatSay(msg);
                xb_audio_ui_sound(XB_SFX_SELECT);   // the keyboard stays: the player closes it (B, BACK)
            }
            return;
        }
        if (!s_qcKb.emoji && s_qcKb.row == 0 && (kd & XINPUT_GAMEPAD_DPAD_UP))
        {
            s_qcPhrase = 1;   // up from the first row: the phrases
            xb_audio_ui_sound(XB_SFX_MOVE);
            return;
        }
        if (lt > 128 && s_qcLtWas <= 128) kbd_toggle_emoji(&s_qcKb);   // the letters or the emojis
        s_qcLtWas = lt;
        if (rt > 128 && s_qcRtWas <= 128)
        {
            s_qcRtWas = rt;
            if (s_qcText[0]) { xb_audio_ui_sound(XB_SFX_SELECT); NetChatSay(s_qcText); s_qcText[0] = 0; }   // empty, still open
            else xb_audio_ui_sound(XB_SFX_ERROR);
            return;
        }
        s_qcRtWas = rt;
        kbd_input(&s_qcKb, kd);
        return;
    }
    if (down & XINPUT_GAMEPAD_B) { s_qcOpen = 0; xb_audio_ui_sound(XB_SFX_BACK); return; }
    if (down & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))
    {
        s_qcCat = (s_qcCat + ((down & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : QC_CATS - 1)) % QC_CATS;
        if (s_qcRow == 0 || s_qcRow == 3) s_qcRow = 1;   // to the texts
        xb_audio_ui_sound(XB_SFX_TAB);
    }
    if (down & XINPUT_GAMEPAD_DPAD_UP) s_qcRow = (s_qcRow + 3) % 4;
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) s_qcRow = (s_qcRow + 1) % 4;
    if (down & XINPUT_GAMEPAD_DPAD_LEFT) s_qcCol = (s_qcCol + cols - 1) % cols;
    if (down & XINPUT_GAMEPAD_DPAD_RIGHT) s_qcCol = (s_qcCol + 1) % cols;
    {
        const int now = s_qcRow == 0 ? EMOJI_COUNT : s_qcRow <= 2 ? 4 : 1;
        if (s_qcCol >= now) s_qcCol = now - 1;
    }
    if (down & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) xb_audio_ui_sound(XB_SFX_MOVE);
    if (down & XINPUT_GAMEPAD_A)
    {
        char msg[16];
        if (s_qcRow == 0)
        {
            _snprintf(msg, sizeof(msg), "@e%d", s_qcCol);
            NetChatSay(msg);
            s_qcOpen = 0;
        }
        else if (s_qcRow <= 2)
        {
            // Its number, not its text: each console shows it in its own language (NetChatAdd).
            _snprintf(msg, sizeof(msg), "@q%d", s_qcTab[s_qcCat][(s_qcRow - 1) * 4 + s_qcCol]);
            NetChatSay(msg);
            s_qcOpen = 0;
        }
        else NetQuickChatKeyboard();
        xb_audio_ui_sound(XB_SFX_SELECT);
    }
}

// A message's text with its emojis (a message "@e<n>": that emoji, big), from (x, cy): its width.
static float NetChatBody(float x, float cy, float size, const char *text, D3DCOLOR c)
{
    float w = 0;
    if (text[0] == '@' && text[1] == 'e' && text[2] >= '0' && text[2] <= '9' && (!text[3] || (text[3] >= '0' && text[3] <= '9' && !text[4])))
    {
        const int k = atoi(text + 2);
        if (k >= 0 && k < EMOJI_COUNT && EmojiTex(k))
        {
            UiImage(EmojiTex(k), x, cy - size * 0.75f, size * 1.5f, size * 1.5f, 0, D3DCOLOR_ARGB(255, 255, 255, 255));
            return size * 1.5f;
        }
    }
    RichText(UI_REGULAR, size, x, cy, 900, 1, c, text, 1, &w);
    return w;
}

// What the online play draws over the game (OverlayHook, between UiBegin and UiEnd).
static void NetOverlayDraw(IDirect3DDevice9 *dev)
{
    int k, p;
    char t[48];
    if (s_netMode <= 0) return;
    for (k = 0; k < EMOJI_COUNT; k++)
        if (!s_emojiTex[k]) s_emojiTex[k] = UiImageCreate(dev, s_emojiRgba[k], EMOJI_SIZE, EMOJI_SIZE);
    {
        // The players down the right edge, no panel behind: in fixed columns, each port's colour and avatar on
        // one margin, the names in one column, each console's ping (there and back) right-aligned in its own;
        // this console's players in light blue, "-" for its ping; the ones who left dimmed.
        static const D3DCOLOR portCol[4] = { D3DCOLOR_ARGB(230, 230, 70, 70), D3DCOLOR_ARGB(230, 70, 120, 235),
                                             D3DCOLOR_ARGB(230, 240, 200, 50), D3DCOLOR_ARGB(230, 70, 190, 90) };
        const D3DCOLOR shade = D3DCOLOR_ARGB(170, 0, 0, 0);
        char names[NET_PLAYERS + 1][24];
        float namew = 0, pingw = UiTextWidth(UI_BOLD, 15, "000 ms"), right = UI_WIDTH - UI_SAFE_X, xname, y = UI_SAFE_Y;
        for (p = 1; p <= s_netPlayers; p++)
        {
            float nw;
            if (s_netTagOf[p][0]) UiTextFit(UI_BOLD, 15, 130, s_netTagOf[p], names[p], sizeof(names[p]));
            else _snprintf(names[p], sizeof(names[p]), "J%d", p);
            names[p][sizeof(names[p]) - 1] = 0;
            nw = UiTextWidth(UI_BOLD, 15, names[p]);
            if (nw > namew) namew = nw;
        }
        xname = right - pingw - 14 - namew;
        for (p = 1; p <= s_netPlayers; p++, y += 26)
        {
            const int mine = NET_MINE(p), gone = s_netGone[p] && !mine, ms = mine ? 0 : s_netRtt[s_netOwner[p]];
            const D3DCOLOR nc = gone ? UI_COL_TEXT3 : mine ? D3DCOLOR_ARGB(235, 170, 205, 255) : D3DCOLOR_ARGB(235, 245, 247, 252);
            const D3DCOLOR pc = gone || ms <= 0 ? UI_COL_TEXT3 : ms < 90 ? D3DCOLOR_ARGB(235, 90, 225, 130) : ms < 160 ? D3DCOLOR_ARGB(235, 250, 210, 80) : D3DCOLOR_ARGB(235, 250, 100, 90);
            UiRoundRect(xname - 31, y + 4, 3, 12, 1.5f, portCol[(p - 1) & 3], portCol[(p - 1) & 3]);
            if (s_netTagOf[p][0]) OnlineGameAvatar(xname - 14, y + 10, 9, s_netAvatarOf[p], s_netTagOf[p]);
            UiText(UI_BOLD, 15, xname + 1, y + 11, shade, names[p], UI_LEFT);
            UiText(UI_BOLD, 15, xname, y + 10, nc, names[p], UI_LEFT);
            if (mine || gone || ms <= 0) _snprintf(t, sizeof(t), "-");
            else _snprintf(t, sizeof(t), "%d ms", ms);
            {
                const float tw = UiTextWidth(UI_BOLD, 15, t);
                UiText(UI_BOLD, 15, right - tw + 1, y + 11, shade, t, UI_LEFT);
                UiText(UI_BOLD, 15, right - tw, y + 10, pc, t, UI_LEFT);
            }
        }
    }
    if (!s_qcOpen)
    {
        // The last messages beside the game, for a few seconds, with who wrote them.
        int y = 0;
        for (k = 0; k < s_netChatCount; k++)
        {
            const NetChatLine *l = &s_netChat[k];
            const int age = (int)(GetTickCount() - l->at);
            float a, tw;
            if (age > NET_CHAT_SHOW_MS) continue;
            a = age > NET_CHAT_SHOW_MS - 600 ? (NET_CHAT_SHOW_MS - age) / 600.0f : 1.0f;
            tw = UiTextWidth(UI_BOLD, 18, l->tag) + 30 + (l->text[0] == '@' ? 30 : UiTextWidth(UI_REGULAR, 18, l->text));
            UiRoundRect(UI_SAFE_X - 8, UI_SAFE_Y + 40 + y * 40, tw + 16, 34, 12, UiAlpha(UI_COL_PANEL, a), UiAlpha(UI_COL_PANEL, a));
            tw = UiText(UI_BOLD, 18, UI_SAFE_X, UI_SAFE_Y + 57 + y * 40, UiAlpha(UI_COL_ACCENT_HI, a), l->tag, UI_LEFT);
            NetChatBody(UI_SAFE_X + tw + 12, UI_SAFE_Y + 57 + y * 40, 18, l->text, UiAlpha(UI_COL_TEXT, a));
            y++;
        }
        return;
    }
    // The quick chat: the last messages, then the emojis, the quick texts and the keyboard (or the keyboard open).
    {
        const float w = 960, h = s_qcKbd ? 392 : 292, x = (UI_WIDTH - w) * 0.5f, y = UI_HEIGHT - h - 50, cx = x + w * 0.5f;
        const int first = s_netChatCount > 5 ? s_netChatCount - 5 : 0;
        UiPanel(x, y - 200, w, 190, 18);
        for (k = first; k < s_netChatCount; k++)
        {
            const float cy = y - 175 + (k - first) * 34;
            const float tw = UiText(UI_BOLD, 18, x + 22, cy, UI_COL_ACCENT_HI, s_netChat[k].tag, UI_LEFT);
            NetChatBody(x + 34 + tw, cy, 18, s_netChat[k].text, UI_COL_TEXT);
        }
        if (!s_netChatCount) UiText(UI_REGULAR, 18, cx, y - 105, UI_COL_TEXT3, Tr(T_ONLINE_CHAT_EMPTY), UI_CENTER);
        UiPanel(x, y, w, h, 18);
        if (s_qcKbd)
        {
            // The text, the quick phrases (up from the first row of keys; A sends one), the tabs, the keys.
            float px = x + 30;
            kbd_draw_text(&s_qcKb, cx, y + 16, w - 60, 42, 20);
            for (k = 0; k < QC_PHRASES; k++)
            {
                const int on = s_qcPhrase && s_qcPhraseCol == k;
                char fit[48];
                float pw;
                UiTextFit(UI_BOLD, 16, (w - 60 - (QC_PHRASES - 1) * 10) / QC_PHRASES - 28, Tr(T_QC_1 + s_qcTab[0][k]), fit, sizeof(fit));
                pw = UiTextWidth(UI_BOLD, 16, fit) + 28;
                UiRoundRect(px, y + 70, pw, 32, 16, on ? UI_COL_ACCENT : UI_COL_CARD, on ? UI_COL_ACCENT : UI_COL_CARD);
                UiText(UI_BOLD, 16, px + pw * 0.5f, y + 86, on ? D3DCOLOR_ARGB(255, 255, 255, 255) : UI_COL_TEXT2, fit, UI_CENTER);
                px += pw + 10;
            }
            kbd_draw_tabs(&s_qcKb, x + 30, y + 124);
            kbd_draw_keys(&s_qcKb, cx, y + 146, 42, 5);
            UiScreenFooter(Tr(T_FOOT_QC_KEYS));
            return;
        }
        for (k = 0; k < EMOJI_COUNT; k++)
        {
            const float ex = x + 24 + k * 65, ey = y + 18;
            const int on = s_qcRow == 0 && s_qcCol == k;
            if (on) UiRoundRect(ex - 4, ey - 4, 60, 60, 12, UI_COL_SELECT, UI_COL_SELECT);
            if (EmojiTex(k)) UiImage(EmojiTex(k), ex + 2, ey + 2, 48, 48, 0, D3DCOLOR_ARGB(255, 255, 255, 255));
        }
        {
            // The texts' tabs (LB/RB), the one shown lit.
            float tx = x + 24;
            int t2;
            UiRoundRect(tx, y + 86, 36, 24, 12, D3DCOLOR_ARGB(255, 60, 62, 76), D3DCOLOR_ARGB(255, 60, 62, 76));
            UiText(UI_BOLD, 12, tx + 18, y + 98, UI_COL_TEXT2, "LB", UI_CENTER);
            tx += 46;
            for (t2 = 0; t2 < QC_CATS; t2++)
            {
                const int on = t2 == s_qcCat;
                const float tw = UiTextWidth(on ? UI_BOLD : UI_REGULAR, 16, Tr(T_QC_CAT_1 + t2)) + 30;
                if (on) UiRoundRect(tx, y + 83, tw, 30, 15, UI_COL_ACCENT, UI_COL_ACCENT);
                else UiRoundRing(tx, y + 83, tw, 30, 15, 1.0f, UI_COL_LINE);
                UiText(on ? UI_BOLD : UI_REGULAR, 16, tx + tw * 0.5f, y + 98, on ? D3DCOLOR_ARGB(255, 255, 255, 255) : UI_COL_TEXT2, Tr(T_QC_CAT_1 + t2), UI_CENTER);
                tx += tw + 8;
            }
            UiRoundRect(tx + 2, y + 86, 36, 24, 12, D3DCOLOR_ARGB(255, 60, 62, 76), D3DCOLOR_ARGB(255, 60, 62, 76));
            UiText(UI_BOLD, 12, tx + 20, y + 98, UI_COL_TEXT2, "RB", UI_CENTER);
        }
        for (k = 0; k < 8; k++)
        {
            const int row = 1 + k / 4, col = k % 4, on = s_qcRow == row && s_qcCol == col;
            const float bw = (w - 48 - 3 * 12) / 4, bx = x + 24 + col * (bw + 12), by = y + 126 + (row - 1) * 52;
            char fit[64];
            UiRoundRect(bx, by, bw, 44, 12, on ? UI_COL_ACCENT : UI_COL_CARD, on ? UI_COL_ACCENT : UI_COL_CARD);
            UiTextFit(UI_BOLD, 17, bw - 16, Tr(T_QC_1 + s_qcTab[s_qcCat][k]), fit, sizeof(fit));
            UiText(UI_BOLD, 17, bx + bw * 0.5f, by + 22, on ? D3DCOLOR_ARGB(255, 255, 255, 255) : UI_COL_TEXT2, fit, UI_CENTER);
        }
        {
            const int on = s_qcRow == 3;
            UiRoundRect(cx - 130, y + 236, 260, 40, 12, on ? UI_COL_ACCENT : UI_COL_CARD, on ? UI_COL_ACCENT : UI_COL_CARD);
            UiText(UI_BOLD, 17, cx, y + 256, on ? D3DCOLOR_ARGB(255, 255, 255, 255) : UI_COL_TEXT2, Tr(T_QC_KEYBOARD), UI_CENTER);
        }
        UiScreenFooter(Tr(T_FOOT_QC));
    }
}

// The States page's picture of the slot shown (state<N>.thm, see SaveStateThumb):
// loaded by the overlay, on the menu's thread, when the slot changes.
static int s_statesOpen;            // the States page is up
static int s_pauseMain;             // the pause menu's own page is up (its "Now playing" under the rows)
static char s_romNow[256];          // the game running (its "Now playing")
static char s_npTitle[160];         // its name ...
static u8 *s_npRgba;                // ... its picture, loaded as the menu opens, made a texture by the overlay
static int s_npW, s_npH, s_npBox;
static IDirect3DTexture9 *s_npTex;
// The pause menu's own page (PauseDraw): the frame as it opened - large (its picture on the right) and tiny (stretched
// over the screen: blurred behind the menu) - made textures on the menu's thread; the game's kind (hack...); when
// the game started (the time played this session).
static u8 *s_pauseShotRgba, *s_pauseBlurRgba;
static int s_pauseShotW, s_pauseShotH, s_pauseBlurW, s_pauseBlurH, s_pauseKind = -1;
static IDirect3DTexture9 *s_pauseShotTex, *s_pauseBlurTex;
static DWORD s_playStart;
static void PauseDraw(IDirect3DDevice9 *dev, int states);   // below, with the pause menu's rows
static void PauseBackdrop(IDirect3DDevice9 *dev);
static int PauseHack(void);
static const char *PauseTitle(char *out, size_t size);
// The States page's slots (PauseDraw): each one's picture small and how long ago it was saved (-1: empty), made as
// the page opens (s_slotsMade), again after a save, freed as it closes.
#define SLOT_TILE_W 64
#define SLOT_TILE_H 48
static IDirect3DTexture9 *s_slotTex[CONFIG_STATE_SLOTS + 1];
static int s_slotMins[CONFIG_STATE_SLOTS + 1], s_slotsMade;
// BACK held in a game (not online): how long (ms; 0 up). From BACK_COUNT_FROM a countdown shows (BackHoldDraw) and
// at BACK_LEAVE_AT the game is left for the ROM list; let go before, nothing (a short press: the pause menu).
#define BACK_COUNT_FROM 600.0
#define BACK_LEAVE_AT 3600.0
static volatile float s_backHeldMs;

// The countdown: a panel in the middle, a ring filling round the seconds left (3, 2, 1), "Leaving the game" and how
// to stay (let BACK go).
static void BackHoldDraw(float held)
{
    const float t = (float)((held - BACK_COUNT_FROM) / (BACK_LEAVE_AT - BACK_COUNT_FROM)), a = held - BACK_COUNT_FROM < 200 ? (held - (float)BACK_COUNT_FROM) / 200 : 1.0f;
    const float w = 460, h = 230, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f, cx = x + w * 0.5f, cy = y + 82, r = 46;
    const D3DCOLOR ring = D3DCOLOR_ARGB((int)(255 * a), 240, 90, 80);
    char num[4];
    float pts[65 * 2];
    int k, n = 3 - (int)(t * 3);
    if (t < 0) return;
    if (n < 1) n = 1;
    UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB((int)(110 * a), 0, 0, 0));
    UiShadow(x, y + 8, w, h, 22, 24, D3DCOLOR_ARGB((int)(130 * a), 0, 0, 0));
    UiRoundRect(x, y, w, h, 22, D3DCOLOR_ARGB((int)(240 * a), 22, 20, 36), D3DCOLOR_ARGB((int)(240 * a), 18, 16, 30));
    UiRoundRing(x, y, w, h, 22, 2.0f, UiAlpha(ring, 0.6f));
    UiRoundRing(cx - r, cy - r, r * 2, r * 2, r, 7.0f, D3DCOLOR_ARGB((int)(50 * a), 255, 255, 255));
    for (k = 0; k <= 64; k++)
    {
        // the ring filling, clockwise from the top
        const float ang = -1.5708f + 6.2832f * (t > 1 ? 1 : t) * k / 64;
        pts[k * 2] = cx + cosf(ang) * (r - 3.5f);
        pts[k * 2 + 1] = cy + sinf(ang) * (r - 3.5f);
    }
    if (t > 0.01f) UiPolyline(pts, 65, 7.0f, 1.0f, ring);
    _snprintf(num, sizeof(num), "%d", n);
    UiText(UI_BOLD, 44, cx, cy, D3DCOLOR_ARGB((int)(255 * a), 255, 255, 255), num, UI_CENTER);
    UiText(UI_BOLD, 22, cx, y + 160, UiAlpha(UI_COL_TEXT, a), Tr(T_BACK_LEAVING), UI_CENTER);
    UiText(UI_REGULAR, 16, cx, y + 192, UiAlpha(UI_COL_TEXT3, a), Tr(T_BACK_CANCEL), UI_CENTER);
}
static IDirect3DTexture9 *s_stateThumb;
static int s_stateThumbSlot = -1;   // the slot s_stateThumb is of (-1: load again)

static int s_showFps;               // what the performance box shows (Config::perfItems, CONFIG_PERF_*; 0: no box)
static int s_perfF, s_perfTemps;    // temperatures in degrees F; they can be read (not Xenia)
static char s_fpsText[256];          // its lines: "60.0 FPS", "CPU 42%", ... as chosen
// The next text, made here while the box goes on showing the last one (s_fpsText): built
// in place, the box was drawn half made now and then - the temperatures, read last and
// slowest, came and went (it flickered).
static char s_fpsBuild[256];

// One more line of the performance box.
static void PerfAdd(const char *fmt, ...)
{
    size_t n = strlen(s_fpsBuild);
    va_list ap;
    if (n && n < sizeof(s_fpsBuild) - 2) { s_fpsBuild[n++] = '\n'; s_fpsBuild[n] = 0; }
    va_start(ap, fmt);
    _vsnprintf(s_fpsBuild + n, sizeof(s_fpsBuild) - 1 - n, fmt, ap);
    va_end(ap);
    s_fpsBuild[sizeof(s_fpsBuild) - 1] = 0;
}
static UiMenu s_menu;
static int s_menuOpen;              // the in-game menu is drawn over the frozen frame
static int s_aboutOpen;             // ... showing the About panel
static int s_hiresPreviewOpen;      // ... showing the hi-res pack preview (HiresPreviewOpen already called)
static int s_confirmOpen;           // ... asking first: CONFIRM_* (0: not asking)
enum { CONFIRM_LIST = 1, CONFIRM_DASHBOARD, CONFIRM_RESET, CONFIRM_SAVE_GAME, CONFIRM_SAVE_ALL };

// <save folder>\state<N>.thm: the picture of save state N - "H64T", width,
// height (u32 each), then RGBA rows. Written with the state, at most THUMB_W x THUMB_H.
#define THUMB_W 320
#define THUMB_H 240
static void ThumbPath(int slot, char *out, size_t size)
{
    _snprintf(out, size, "%sstate%d.thm", s_saveDir, slot);
    out[size - 1] = 0;
}

// The texture of the slot shown, (re)made when the slot changed. On the menu's
// thread only (s_statesOpen): the graphics worker is idle while the menu runs.
static void StateThumbSync(IDirect3DDevice9 *dev)
{
    char path[160];
    u32 size = 0;
    u8 *data;
    if (s_stateThumbSlot == s_stateSlot) return;
    if (s_stateThumb) { UiImageRelease(dev, s_stateThumb); s_stateThumb = NULL; }
    s_stateThumbSlot = s_stateSlot;
    ThumbPath(s_stateSlot, path, sizeof(path));
    data = LoadFile(path, &size);
    if (data && size >= 12 && !memcmp(data, "H64T", 4))
    {
        u32 w, h;
        memcpy(&w, data + 4, 4);
        memcpy(&h, data + 8, 4);
        if (w && h && w <= THUMB_W && h <= THUMB_H && size >= 12 + w * h * 4) s_stateThumb = UiImageCreate(dev, data + 12, (int)w, (int)h);
    }
    free(data);
}

// ---- Cheats (the pause menu's Cheats page; core/cheat/h64_cheat.cpp) ----
// A game's GameShark codes: the mupen64plus database's (<drive>:\cheats\mupencheat.txt,
// by the ROM's CRC) and the user's own (<drive>:\cheats\<game>.txt, the same format).
// The ones on are kept per game (<drive>:\config\cheats\<CRC1-CRC2>.txt) and written
// into RDRAM once a frame.
static H64Cheats s_cheats;
static char s_cheatState[192];
static char s_cheatCustom[192];
static int s_cheatsOpen, s_cheatSel, s_cheatTop;
#define CHEAT_ROWS 11

static void CheatsOpen(H64System *sys)
{
    char path[192], folder[64];
    s_cheats = H64Cheats();
    s_cheats.booted = 0;
    _snprintf(path, sizeof(path) - 1, "%s:\\cheats\\mupencheat.txt", s_logDrive);
    path[sizeof(path) - 1] = 0;
    h64_cheats_load_db(&s_cheats, path, sys->rom.crc1, sys->rom.crc2, (u8)sys->rom.gameCode[3]);
    HiresFolderName(sys->rom.name, folder, sizeof(folder));
    _snprintf(s_cheatCustom, sizeof(s_cheatCustom) - 1, "%s:\\cheats\\%s.txt", s_logDrive, folder);
    s_cheatCustom[sizeof(s_cheatCustom) - 1] = 0;
    h64_cheats_load_custom(&s_cheats, s_cheatCustom);
    _snprintf(path, sizeof(path) - 1, "%s:\\config\\cheats", s_logDrive);
    path[sizeof(path) - 1] = 0;
    CreateDirectory(path, NULL);
    _snprintf(s_cheatState, sizeof(s_cheatState) - 1, "%s\\%08X-%08X.txt", path, sys->rom.crc1, sys->rom.crc2);
    s_cheatState[sizeof(s_cheatState) - 1] = 0;
    h64_cheats_load_state(&s_cheats, s_cheatState);
    H64_INFO("[cheat] %s: %u cheats, %d on", sys->rom.name, (unsigned)s_cheats.list.size(), h64_cheats_enabled(&s_cheats));
}

// The Cheats page over the frozen frame: each cheat, a switch, its choice when it has some,
// the selected one's description under them.
static void CheatsDraw(void)
{
    const float w = 940, h = 600, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f, rowH = 38, lx = x + 30, lw = w - 60;
    const int n = (int)s_cheats.list.size();
    char line[300], fit[300];
    int k;
    UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB(150, 0, 0, 0));
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 28, lx, y + 36, UI_COL_TEXT, Tr(T_CHEATS), UI_LEFT);
    _snprintf(line, sizeof(line) - 1, Tr(T_CHEATS_COUNT), h64_cheats_enabled(&s_cheats), n);
    line[sizeof(line) - 1] = 0;
    UiText(UI_REGULAR, 17, x + w - 30, y + 36, UI_COL_ACCENT_HI, line, UI_RIGHT);
    if (!s_cheats.game.empty())
    {
        UiTextFit(UI_REGULAR, 15, lw, s_cheats.game.c_str(), fit, sizeof(fit));
        UiText(UI_REGULAR, 15, lx, y + 64, UI_COL_TEXT3, fit, UI_LEFT);
    }
    UiRect(lx, y + 84, lw, 1, UI_COL_LINE);
    if (!n)
    {
        UiText(UI_BOLD, 22, x + w * 0.5f, y + 230, UI_COL_TEXT, Tr(T_CHEATS_NONE), UI_CENTER);
        UiText(UI_REGULAR, 18, x + w * 0.5f, y + 280, UI_COL_TEXT2, Tr(T_CHEATS_HOWTO), UI_CENTER);
        UiTextFit(UI_BOLD, 18, lw, s_cheatCustom, fit, sizeof(fit));
        UiText(UI_BOLD, 18, x + w * 0.5f, y + 312, UI_COL_ACCENT_HI, fit, UI_CENTER);
    }
    else
    {
        for (k = 0; k < CHEAT_ROWS && s_cheatTop + k < n; k++)
        {
            const H64Cheat *ch = &s_cheats.list[s_cheatTop + k];
            const float ry = y + 96 + k * rowH, cy = ry + rowH * 0.5f;
            const int sel = s_cheatTop + k == s_cheatSel;
            float right = x + w - 34, textR;
            size_t j;
            std::string shown;
            if (sel) UiRoundRect(x + 14, ry + 2, w - 28, rowH - 4, 10, UiAlpha(UI_COL_ACCENT, 0.35f), UiAlpha(UI_COL_ACCENT, 0.22f));
            // The switch.
            UiRoundRect(right - 44, cy - 11, 44, 22, 11, ch->enabled ? UI_COL_ACCENT_HI : D3DCOLOR_ARGB(70, 255, 255, 255),
                        ch->enabled ? UI_COL_ACCENT : D3DCOLOR_ARGB(55, 255, 255, 255));
            UiCircle(ch->enabled ? right - 11 : right - 33, cy, 8, UI_COL_TEXT);
            textR = right - 58;
            if (!ch->options.empty())
            {
                // Its choice: "< 3 Laps >".
                _snprintf(line, sizeof(line) - 1, "< %s >", ch->options[ch->option].name.c_str());
                line[sizeof(line) - 1] = 0;
                UiTextFit(UI_REGULAR, 16, 260, line, fit, sizeof(fit));
                UiText(UI_REGULAR, 16, textR, cy, sel ? UI_COL_ACCENT_HI : UI_COL_TEXT2, fit, UI_RIGHT);
                textR -= UiTextWidth(UI_REGULAR, 16, fit) + 18;
            }
            // Its name, a group shown "Group · Name".
            for (j = 0; j < ch->name.size(); j++)
                if (ch->name[j] == '\\') shown += "  " UI_CH_MIDDOT "  ";
                else shown += ch->name[j];
            UiTextFit(sel ? UI_BOLD : UI_REGULAR, 18, textR - lx - 10, shown.c_str(), fit, sizeof(fit));
            UiText(sel ? UI_BOLD : UI_REGULAR, 18, lx + 6, cy, sel || ch->enabled ? UI_COL_TEXT : UI_COL_TEXT2, fit, UI_LEFT);
        }
        if (n > CHEAT_ROWS)
        {
            float tx = x + w - 14, ty = y + 98, th = CHEAT_ROWS * rowH - 4, hh = th * CHEAT_ROWS / n;
            float yy = ty + (th - hh) * s_cheatTop / (float)(n - CHEAT_ROWS);
            UiRoundRect(tx, yy, 4, hh, 2, D3DCOLOR_ARGB(150, 255, 255, 255), D3DCOLOR_ARGB(110, 255, 255, 255));
        }
        // The selected one's description.
        if (s_cheatSel < n && !s_cheats.list[s_cheatSel].desc.empty())
        {
            UiTextWrap(UI_REGULAR, 15, lw, 2, s_cheats.list[s_cheatSel].desc.c_str(), fit, sizeof(fit));
            UiText(UI_REGULAR, 15, lx, y + h - 94, UI_COL_TEXT3, fit, UI_LEFT);
        }
    }
    UiRect(lx, y + h - 56, lw, 1, UI_COL_LINE);
    UiFooter(lx, y + h - 28, Tr(n ? T_FOOT_CHEATS : T_FOOT_BACK_ONLY));
}

// ---- The pause menu's Players online page (with an online profile and a network) ----
// Who is online and what each one does (the game played, in a room, in the lobby), each one's time to the
// server and its connection; xb_online.cpp's thread reads the list every few seconds meanwhile, so the
// menu never waits for the server (an online game's consoles keep hearing this one).
#define PLAYERS_ROWS 12
static int s_playersOpen, s_playersTop, s_playersPics;   // s_playersPics: their avatars and flags loaded (UiOnlinePicsLoad)

static int PlayersAvailable(void) { return OnlineRegistered() && OnlineHasServer() && OnlineMyConn() != ONLINE_CONN_NONE; }

static void PlayersDraw(void)
{
    const float w = 940, h = 600, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f, rowH = 36, lx = x + 30, lw = w - 60;
    std::vector<OnlineUser> users;
    const int st = OnlineWatchedUsers(&users), n = (int)users.size();
    char line[160], fit[160];
    int k;
    UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB(150, 0, 0, 0));
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 28, lx, y + 36, UI_COL_TEXT, Tr(T_ONLINE_NOW), UI_LEFT);
    if (st > 0)
    {
        _snprintf(line, sizeof(line) - 1, "%d", n);
        line[sizeof(line) - 1] = 0;
        k = (int)UiText(UI_BOLD, 18, x + w - 30, y + 36, UI_COL_TEXT2, line, UI_RIGHT);
        UiCircle(x + w - 30 - k - 14, y + 36, 6, D3DCOLOR_ARGB(255, 80, 210, 120));
    }
    UiRect(lx, y + 66, lw, 1, UI_COL_LINE);
    if (st <= 0 || !n)
        UiText(UI_REGULAR, 20, x + w * 0.5f, y + h * 0.5f, UI_COL_TEXT2, Tr(st == 0 ? T_ONLINE_CONNECTING : st < 0 ? T_ONLINE_NO_SERVER : T_ONLINE_EMPTY), UI_CENTER);
    if (s_playersTop > n - PLAYERS_ROWS) s_playersTop = n > PLAYERS_ROWS ? n - PLAYERS_ROWS : 0;
    for (k = 0; st > 0 && k < PLAYERS_ROWS && s_playersTop + k < n; k++)
    {
        const OnlineUser *u = &users[s_playersTop + k];
        const int me = !_stricmp(u->name, OnlineMe()->tag), ms = me && OnlineMyMs() ? OnlineMyMs() : u->ms;
        const int conn = me ? OnlineMyConn() : u->conn, lit = ms <= 0 ? 0 : ms < 60 ? 4 : ms < 100 ? 3 : ms < 160 ? 2 : 1;
        const D3DCOLOR pc = ms <= 0 ? UI_COL_TEXT3 : ms < 90 ? D3DCOLOR_ARGB(255, 80, 210, 120) : ms < 160 ? D3DCOLOR_ARGB(255, 240, 200, 70) : D3DCOLOR_ARGB(255, 240, 90, 80);
        const float cy = y + 80 + k * rowH + rowH * 0.5f;
        float right = x + w - 30;
        int b;
        if (k & 1) UiRect(x + 14, cy - rowH * 0.5f, w - 28, rowH, D3DCOLOR_ARGB(18, 255, 255, 255));
        // The connection, the time (four bars and the ms), then the country, the name and what it does.
        if (conn != ONLINE_CONN_NONE)
        {
            const char *t = conn == ONLINE_CONN_CABLE ? "LAN" : conn == ONLINE_CONN_WIFI ? "Wi-Fi" : Tr(T_ONLINE_CONN_OLD);
            const float tw = UiTextWidth(UI_BOLD, 13, t) + 12;
            UiRoundRect(right - tw, cy - 10, tw, 20, 6, UiAlpha(UI_COL_CARD, 0.9f), UiAlpha(UI_COL_CARD, 0.9f));
            UiText(UI_BOLD, 13, right - tw * 0.5f, cy, UI_COL_TEXT2, t, UI_CENTER);
        }
        right -= 80;
        if (ms) _snprintf(line, sizeof(line), "%d ms", ms);
        else _snprintf(line, sizeof(line), "-- ms");
        UiText(UI_BOLD, 15, right, cy, pc, line, UI_RIGHT);
        right -= 70;
        for (b = 0; b < 4; b++)
        {
            const float bh = 16 * (0.35f + 0.65f * b / 3.0f);
            UiRect(right + b * 6, cy + 8 - bh, 4, bh, b < lit ? pc : UiAlpha(UI_COL_TEXT3, 0.45f));
        }
        // the avatar, the country's flag (its code when there is none), the name
        UiOnlineAvatar(lx + 14, cy, 14, u->avatar, u->name);
        if (!UiOnlineFlag(lx + 36, cy, 14, u->country)) UiText(UI_BOLD, 13, lx + 36, cy, UI_COL_TEXT3, u->country, UI_LEFT);
        UiTextFit(UI_BOLD, 18, 230, u->name, fit, sizeof(fit));
        UiText(UI_BOLD, 18, lx + 70, cy, me ? UI_COL_ACCENT_HI : UI_COL_TEXT, fit, UI_LEFT);
        if (u->status[0])
        {
            _snprintf(line, sizeof(line) - 1, Tr(T_ONLINE_PLAYING), u->status);
            line[sizeof(line) - 1] = 0;
        }
        else _snprintf(line, sizeof(line), "%s", Tr(u->inRoom ? T_ONLINE_ST_ROOM : T_ONLINE_ST_LOBBY));
        UiTextFit(UI_REGULAR, 16, right - 20 - (lx + 320), line, fit, sizeof(fit));
        UiText(UI_REGULAR, 16, lx + 320, cy, u->status[0] ? D3DCOLOR_ARGB(255, 80, 210, 120) : u->inRoom ? UI_COL_ACCENT_HI : UI_COL_TEXT2, fit, UI_LEFT);
    }
    if (st > 0 && n > PLAYERS_ROWS)
    {
        const float ty = y + 82, th = PLAYERS_ROWS * rowH - 4, hh = th * PLAYERS_ROWS / n;
        UiRoundRect(x + w - 14, ty + (th - hh) * s_playersTop / (float)(n - PLAYERS_ROWS), 4, hh, 2, D3DCOLOR_ARGB(150, 255, 255, 255), D3DCOLOR_ARGB(110, 255, 255, 255));
    }
    UiRect(lx, y + h - 56, lw, 1, UI_COL_LINE);
    UiFooter(lx, y + h - 28, Tr(T_FOOT_BACK_ONLY));
}

// Its input (the D-pad scrolls); 1 when it closes.
static int PlayersInput(WORD down)
{
    if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 1;
    if ((down & XINPUT_GAMEPAD_DPAD_UP) && s_playersTop > 0) { s_playersTop--; xb_audio_ui_sound(XB_SFX_MOVE); }
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) { s_playersTop++; xb_audio_ui_sound(XB_SFX_MOVE); }   // PlayersDraw keeps it within the list
    return 0;
}

// Its input; 1 when it closes (what is on is kept for the game).
static int CheatsInput(WORD down)
{
    const int n = (int)s_cheats.list.size();
    if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))
    {
        h64_cheats_save_state(&s_cheats, s_cheatState);
        return 1;
    }
    if (!n) return 0;
    if (down & XINPUT_GAMEPAD_DPAD_UP) { s_cheatSel = (s_cheatSel + n - 1) % n; xb_audio_ui_sound(XB_SFX_MOVE); }
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) { s_cheatSel = (s_cheatSel + 1) % n; xb_audio_ui_sound(XB_SFX_MOVE); }
    if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) { s_cheatSel = s_cheatSel > CHEAT_ROWS ? s_cheatSel - CHEAT_ROWS : 0; xb_audio_ui_sound(XB_SFX_PAGE); }
    if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) { s_cheatSel = s_cheatSel + CHEAT_ROWS < n ? s_cheatSel + CHEAT_ROWS : n - 1; xb_audio_ui_sound(XB_SFX_PAGE); }
    {
        H64Cheat *ch = &s_cheats.list[s_cheatSel];
        if (down & XINPUT_GAMEPAD_A)
        {
            ch->enabled = !ch->enabled;
            xb_audio_ui_sound(ch->enabled ? XB_SFX_VALUE_UP : XB_SFX_VALUE_DOWN);
        }
        if ((down & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) && !ch->options.empty())
        {
            int m = (int)ch->options.size();
            ch->option = (ch->option + ((down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : m - 1)) % m;
            ch->enabled = 1;   // a choice made: the cheat on
            xb_audio_ui_sound((down & XINPUT_GAMEPAD_DPAD_RIGHT) ? XB_SFX_VALUE_UP : XB_SFX_VALUE_DOWN);
        }
    }
    if (s_cheatSel < s_cheatTop) s_cheatTop = s_cheatSel;
    if (s_cheatSel >= s_cheatTop + CHEAT_ROWS) s_cheatTop = s_cheatSel - CHEAT_ROWS + 1;
    return 0;
}

// ---- HD textures (the pause menu's page, above Settings) ----
// "Off", then each pack installed for the game once (HiresScanPacks, at launch): A uses it, Y
// shows a texture of it. The one in use in a green box, the other packs (downloaded) in blue,
// their authors when the catalog knows them; a bar when they do not all fit.
static int s_texOpen, s_texSel, s_texTop, s_texActive = -1;
#define TEX_ROWS 10

static void TexturesOpen(const Config *c)
{
    s_texActive = HiresActiveIndex(c);
    s_texSel = s_texActive + 1;   // row 0 is "Off"
    s_texTop = s_texSel >= TEX_ROWS ? s_texSel - TEX_ROWS + 1 : 0;
    s_texOpen = 1;
}

static void TexturesDraw(void)
{
    const float w = 860, h = 560, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f, rowH = 40, lx = x + 30, lw = w - 60;
    const int packs = HiresPackCount(), n = packs + 1;
    char fit[200], name[128];
    int k;
    UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB(150, 0, 0, 0));
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 28, lx, y + 36, UI_COL_TEXT, Tr(T_HIRESTEX), UI_LEFT);
    if (s_npTitle[0])
    {
        UiTextFit(UI_REGULAR, 15, lw, s_npTitle, fit, sizeof(fit));
        UiText(UI_REGULAR, 15, lx, y + 64, UI_COL_TEXT3, fit, UI_LEFT);
    }
    UiRect(lx, y + 84, lw, 1, UI_COL_LINE);
    for (k = 0; k < TEX_ROWS && s_texTop + k < n; k++)
    {
        const int row = s_texTop + k, pack = row - 1, sel = row == s_texSel, active = pack == s_texActive;
        const float ry = y + 96 + k * rowH, cy = ry + rowH * 0.5f;
        const char *author = pack >= 0 ? HiresPackAuthor(pack) : NULL;
        float textR = x + w - 40;
        int ba = sel ? 235 : 120;
        if (active) UiRoundRect(x + 14, ry + 2, w - 40, rowH - 4, 10, D3DCOLOR_ARGB(ba, 70, 200, 100), D3DCOLOR_ARGB(ba, 40, 150, 70));
        else if (pack >= 0) UiRoundRect(x + 14, ry + 2, w - 40, rowH - 4, 10, D3DCOLOR_ARGB(ba, 60, 130, 230), D3DCOLOR_ARGB(ba, 35, 90, 180));
        else if (sel) UiRoundRect(x + 14, ry + 2, w - 40, rowH - 4, 10, UiAlpha(UI_COL_ACCENT, 0.35f), UiAlpha(UI_COL_ACCENT, 0.22f));
        if (sel) UiRoundRing(x + 14, ry + 2, w - 40, rowH - 4, 10, 2.0f, UiAlpha(UI_COL_TEXT, 0.85f));
        if (active)
        {
            // The one in use: a check at the right.
            UiLine(textR - 14, cy, textR - 8, cy + 6, 3.0f, UI_COL_TEXT);
            UiLine(textR - 8, cy + 6, textR + 4, cy - 7, 3.0f, UI_COL_TEXT);
            textR -= 30;
        }
        if (author)
        {
            UiTextFit(UI_REGULAR, 15, 220, author, fit, sizeof(fit));
            UiText(UI_REGULAR, 15, textR, cy, pack >= 0 ? UI_COL_TEXT2 : UI_COL_TEXT3, fit, UI_RIGHT);
            textR -= UiTextWidth(UI_REGULAR, 15, fit) + 18;
        }
        if (pack < 0) _snprintf(name, sizeof(name) - 1, "%s", Tr(T_OFF));
        else
        {
            // The pack's file name without ".hts".
            const char *p = HiresPackName(pack);
            size_t len = strlen(p);
            if (len > 4 && !_stricmp(p + len - 4, ".hts")) len -= 4;
            if (len >= sizeof(name)) len = sizeof(name) - 1;
            memcpy(name, p, len);
            name[len] = 0;
        }
        name[sizeof(name) - 1] = 0;
        UiTextFit(sel || active ? UI_BOLD : UI_REGULAR, 19, textR - lx - 10, name, fit, sizeof(fit));
        UiText(sel || active ? UI_BOLD : UI_REGULAR, 19, lx + 6, cy, sel || active || pack >= 0 ? UI_COL_TEXT : UI_COL_TEXT2, fit, UI_LEFT);
    }
    if (n > TEX_ROWS)
    {
        // The bar: where the rows shown are in the list.
        float tx = x + w - 18, ty = y + 98, th = TEX_ROWS * rowH - 4, hh = th * TEX_ROWS / n;
        float yy = ty + (th - hh) * s_texTop / (float)(n - TEX_ROWS);
        UiRoundRect(tx, ty, 4, th, 2, D3DCOLOR_ARGB(20, 255, 255, 255), D3DCOLOR_ARGB(20, 255, 255, 255));
        UiRoundRect(tx, yy, 4, hh, 2, D3DCOLOR_ARGB(150, 255, 255, 255), D3DCOLOR_ARGB(110, 255, 255, 255));
    }
    if (!packs) UiText(UI_REGULAR, 18, x + w * 0.5f, y + 96 + 2 * rowH, UI_COL_TEXT2, Tr(T_HIRES_PAGE_NONE), UI_CENTER);
    UiRect(lx, y + h - 56, lw, 1, UI_COL_LINE);
    UiFooter(lx, y + h - 28, Tr(packs ? T_FOOT_HIRES_PAGE : T_FOOT_BACK_ONLY));
}

// Its input: 1 closes it, 2 a pack was chosen (or turned off), 3 Y asks for a preview.
static int TexturesInput(WORD down, Config *c)
{
    const int n = HiresPackCount() + 1;
    if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 1;
    if (n < 2) return 0;
    if (down & XINPUT_GAMEPAD_DPAD_UP) { s_texSel = (s_texSel + n - 1) % n; xb_audio_ui_sound(XB_SFX_MOVE); }
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) { s_texSel = (s_texSel + 1) % n; xb_audio_ui_sound(XB_SFX_MOVE); }
    if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) { s_texSel = s_texSel > TEX_ROWS ? s_texSel - TEX_ROWS : 0; xb_audio_ui_sound(XB_SFX_PAGE); }
    if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) { s_texSel = s_texSel + TEX_ROWS < n ? s_texSel + TEX_ROWS : n - 1; xb_audio_ui_sound(XB_SFX_PAGE); }
    if (s_texSel < s_texTop) s_texTop = s_texSel;
    if (s_texSel >= s_texTop + TEX_ROWS) s_texTop = s_texSel - TEX_ROWS + 1;
    if ((down & XINPUT_GAMEPAD_Y) && s_texSel > 0) return 3;
    if ((down & XINPUT_GAMEPAD_A) && s_texSel - 1 != s_texActive)
    {
        HiresSelect(c, s_texSel - 1);
        s_texActive = s_texSel - 1;
        xb_audio_ui_sound(s_texActive >= 0 ? XB_SFX_VALUE_UP : XB_SFX_VALUE_DOWN);
        return 2;
    }
    return 0;
}

// The pause menu's HD textures row: the pack in use, or "Off".
static void TexturesRowValue(const Config *c, char *out, size_t size)
{
    int a = HiresActiveIndex(c);
    const char *p = a >= 0 ? HiresPackName(a) : Tr(T_OFF);
    size_t len = strlen(p);
    char name[128];
    if (a >= 0 && len > 4 && !_stricmp(p + len - 4, ".hts")) len -= 4;
    if (len >= sizeof(name)) len = sizeof(name) - 1;
    memcpy(name, p, len);
    name[len] = 0;
    UiTextFit(UI_REGULAR, 21, 300, name, out, (int)size);
}

// The pause menu's Cheats row: how many are on.
static void CheatsRowValue(char *out, size_t size)
{
    int on = h64_cheats_enabled(&s_cheats);
    if (on) _snprintf(out, size - 1, Tr(T_CHEATS_ON_VALUE), on);
    else out[0] = 0;
    out[size - 1] = 0;
}


// An HD pack downloading in the background (xb_hirespack.cpp, Y on its screen): a small
// panel in the top left corner - the pack, what it does, a bar - while it works.
static void HiresBgDraw(void)
{
    const HiresPackEntry *e = HiresPackJob();
    unsigned done = 0, total = 0;
    int st = HiresPackStatus(&done, &total, NULL, NULL, 0);
    const float w = 360, h = 64, x = UI_SAFE_X, y = UI_SAFE_Y + 8, bx = x + 18, bw = w - 36;
    char line[160], fit[160];
    float f = total ? (float)done / (float)total : 0.0f;
    if (!e) return;
    if (f > 1.0f) f = 1.0f;
    UiShadow(x, y + 5, w, h, 14, 18, D3DCOLOR_ARGB(130, 0, 0, 0));
    UiRoundRect(x, y, w, h, 14, D3DCOLOR_ARGB(225, 40, 42, 54), D3DCOLOR_ARGB(225, 30, 32, 42));
    UiRoundRing(x, y, w, h, 14, 1.0f, UI_COL_LINE);
    _snprintf(line, sizeof(line) - 1, Tr(T_HIRES_BG_TITLE), e->title);
    line[sizeof(line) - 1] = 0;
    UiTextFit(UI_BOLD, 16, bw - 60, line, fit, sizeof(fit));
    UiText(UI_BOLD, 16, bx, y + 18, UI_COL_TEXT, fit, UI_LEFT);
    if (total) _snprintf(line, sizeof(line) - 1, "%d%%", (int)(f * 100.0f));
    else line[0] = 0;
    line[sizeof(line) - 1] = 0;
    UiText(UI_BOLD, 16, bx + bw, y + 18, UI_COL_ACCENT_HI, line, UI_RIGHT);
    UiText(UI_REGULAR, 14, bx, y + 37, UI_COL_TEXT2,
           Tr(st == HPACK_CONVERT ? T_HIRES_DL_CONVERTING : st == HPACK_UNPACK ? T_HIRES_DL_UNPACKING
              : st == HPACK_DOWNLOAD ? T_HIRES_DL_DOWNLOADING : T_HIRES_DL_LOOKUP), UI_LEFT);
    UiRoundRect(bx, y + 50, bw, 5, 2.5f, D3DCOLOR_ARGB(50, 255, 255, 255), D3DCOLOR_ARGB(35, 255, 255, 255));
    if (f > 0.0f) UiRoundRect(bx, y + 50, bw * f < 5 ? 5 : bw * f, 5, 2.5f, UI_COL_ACCENT_HI, UI_COL_ACCENT);
}

// Drawn by the renderer just before each present (on the graphics worker when it runs).
static void OverlayHook(void *, IDirect3DDevice9 *dev)
{
    // Nothing to show (the usual case in play): no state change at all.
    s32 left = (s32)(s_osdUntil - GetTickCount());
    int fps = s_showFps && s_fpsText[0], osd = s_osd[0] && left >= 0, hpack = HiresPackBusy() && !s_menuOpen, net = s_netMode > 0;
    const float backHeld = s_backHeldMs;
    const int back = !s_menuOpen && backHeld >= BACK_COUNT_FROM;   // leaving the game: the countdown
    if (s_playersPics && !(s_menuOpen && s_playersOpen))
    {
        UiOnlinePicsFree(dev);   // the Players online page closed: its avatars and flags go
        s_playersPics = 0;
    }
    if (!(s_menuOpen && s_statesOpen) && s_slotsMade)
    {
        // the States page closed: its slots' pictures go
        int k;
        for (k = 1; k <= CONFIG_STATE_SLOTS; k++)
            if (s_slotTex[k]) { UiImageRelease(dev, s_slotTex[k]); s_slotTex[k] = NULL; }
        s_slotsMade = 0;
    }
    if (!s_menuOpen && (s_pauseShotTex || s_pauseBlurTex || s_pauseShotRgba || s_pauseBlurRgba))
    {
        // the pause menu closed: its pictures of the frame go
        if (s_pauseShotTex) { UiImageRelease(dev, s_pauseShotTex); s_pauseShotTex = NULL; }
        if (s_pauseBlurTex) { UiImageRelease(dev, s_pauseBlurTex); s_pauseBlurTex = NULL; }
        free(s_pauseShotRgba);
        free(s_pauseBlurRgba);
        s_pauseShotRgba = s_pauseBlurRgba = NULL;
    }
    if (!fps && !osd && !s_menuOpen && !hpack && !net && !back) return;
    UiBegin(dev);
    if (net && !s_menuOpen) NetOverlayDraw(dev);   // online: the ping, the others' messages, the quick chat
    if (hpack) HiresBgDraw();
    if (fps && !s_menuOpen) UiPerfPanel(s_fpsText);   // one line: a pill, more: a panel (not over the pause menu and its pages)
    if (s_menuOpen && s_hiresPreviewOpen) HiresPreviewDraw(dev);
    else if (s_menuOpen && s_aboutOpen) AboutDraw();
    else if (s_menuOpen && s_cheatsOpen) { PauseBackdrop(dev); CheatsDraw(); }   // (the pause menu's pages: over the blurred game)
    else if (s_menuOpen && s_playersOpen)
    {
        if (!s_playersPics) { UiOnlinePicsLoad(dev); s_playersPics = 1; }   // each one's avatar and country's flag
        PauseBackdrop(dev);
        PlayersDraw();
    }
    else if (s_menuOpen && s_texOpen) { PauseBackdrop(dev); TexturesDraw(); }
    else if (s_menuOpen)
    {
        if (s_npRgba && !s_npTex)
        {
            s_npTex = UiImageCreate(dev, s_npRgba, s_npW, s_npH);   // on the menu's thread (the device is its own then)
            free(s_npRgba);
            s_npRgba = NULL;
        }
        if (s_pauseMain) PauseDraw(dev, 0);   // its own page: the game, the options in groups, the frame
        else if (s_statesOpen) PauseDraw(dev, 1);   // the States page, in the same design: the slot's picture, all the slots
        else if (s_menu.page == UI_PAGE_LIST && !strcmp(s_menu.title, Tr(T_SETTINGS))) PauseDraw(dev, 2);   // Settings' own page too
        else
        {
            // Settings' pages (Graphics, Screen...): over the game itself, live (no blurred picture: the changes to
            // its picture, brightness and effects show on it), their header the game's
            char title[160];
            const int controls = !strcmp(s_menu.title, Tr(T_CONTROLS)) || s_menu.page == UI_PAGE_CONTROLS || s_menu.page == UI_PAGE_CALIB ||
                                 s_menu.page == UI_PAGE_HOTKEYS || s_menu.page == UI_PAGE_RUMBLE;
            if (controls) PauseBackdrop(dev);   // Controls and its pages as they were: the whole width, over the blurred game
            UiSetGameHeader(s_npTex, s_npW, s_npH, PauseTitle(title, sizeof(title)), PauseHack());
            UiMenuDraw(&s_menu);
        }
        UiQuitLaikaDraw(dev, s_confirmOpen == CONFIRM_DASHBOARD);   // leaving to the dashboard: Laika asks, angry
        if (s_confirmOpen == CONFIRM_LIST) UiConfirmDraw(-1, T_LIST_ASK, T_FOOT_QUIT);
        else if (s_confirmOpen == CONFIRM_RESET) UiConfirmDraw(-1, T_RESET_ASK, T_FOOT_RESET_ASK);
        else if (s_confirmOpen == CONFIRM_SAVE_GAME) UiConfirmDraw(-1, T_SAVE_GAME_ASK, T_FOOT_SAVE_ASK);
        else if (s_confirmOpen == CONFIRM_SAVE_ALL) UiConfirmDraw(-1, T_SAVE_ALL_ASK, T_FOOT_SAVE_ASK);
    }
    if (osd) UiToast(s_osd, (float)((s32)s_osdLen - left), (float)s_osdLen);
    if (back) BackHoldDraw(backHeld);
    UiEnd();
}

static void StatePath(int slot, char *out, size_t size)
{
    _snprintf(out, size, "%sstate%d.h64s", s_saveDir, slot);
    out[size - 1] = 0;
}

// The frame on screen, made small, as the picture of the slot being saved.
static void SaveStateThumb(H64System *sys, int slot)
{
    char path[160];
    int w = 0, h = 0, tw, th;
    u8 *px, *small;
    FILE *f;
    if (!sys->renderer) return;
    h64_hle_async_wait(sys);
    WorkWaitAll(NULL);   // the device is this thread's for the capture
    px = h64_xenos_grab_frame(sys->renderer, &w, &h);
    if (!px) return;
    h64_image_fit(w, h, THUMB_W, THUMB_H, &tw, &th);
    small = h64_image_scale(px, w, h, tw, th);
    free(px);
    if (!small) return;
    ThumbPath(slot, path, sizeof(path));
    f = fopen(path, "wb");
    if (f)
    {
        u32 dims[2] = { (u32)tw, (u32)th };
        fwrite("H64T", 1, 4, f);
        fwrite(dims, 4, 2, f);
        fwrite(small, 1, (size_t)tw * th * 4, f);
        fclose(f);
    }
    free(small);
    s_stateThumbSlot = -1;   // the States page shows the new picture
    s_slotsMade = 0;         // ... and its slot's small one
}

// The slot kept for the save made when a controller is unplugged (0: none):
// manual saves are refused there; loading from it works as from any slot.
static int s_reservedSlot;

// Saves a state in `slot` (the reserved slot only when `autosave`). Returns 1
// when done (saved, failed or refused), 0 when the machine is not quiet yet.
static int SaveStateNow(H64System *sys, int slot, int autosave)
{
    std::vector<u8> st;
    char path[160];
    FILE *f;
    int ok;
    if (!autosave && slot == s_reservedSlot) { OsdT(T_SLOT_RESERVED, slot); return 1; }
    if (!h64_state_quiet(sys)) return 0;
    h64_state_save(sys, &st);
    StatePath(slot, path, sizeof(path));
    f = fopen(path, "wb");
    ok = f && fwrite(&st[0], 1, st.size(), f) == st.size();
    if (f) fclose(f);
    if (ok) SaveStateThumb(sys, slot);
    OsdT(!ok ? T_STATE_WRITE_FAILED : autosave ? T_AUTOSAVED : T_STATE_SAVED, slot);
    return 1;
}

static void LoadStateNow(H64System *sys)
{
    char path[160];
    u32 size = 0;
    u8 *data;
    if (NetRefuse()) return;
    StatePath(s_stateSlot, path, sizeof(path));
    data = LoadFile(path, &size);
    if (!data) OsdT(T_NO_STATE, s_stateSlot);
    else if (h64_state_load(sys, data, size)) OsdT(T_STATE_LOAD_FAILED, s_stateSlot);
    else OsdT(T_STATE_LOADED, s_stateSlot);
    free(data);
}

// ---- In-game menu ----
// BACK pressed and released (without RB/LB/D-pad) pauses the game and shows
// it over the frozen frame. Settings changed there apply at once (audio
// margin, smoothing, FPS) or at the next start (CPU, RSP); they are saved
// for all games (config\settings.ini) or for this game (config\<game>.ini).
enum { RG_DASHBOARD = 0, RG_BROWSER, RG_CONTINUE };
enum { M_RESUME = 0, M_STATES, M_CHEATS, M_TEXTURES, M_SETTINGS, M_PLAYERS, M_RESET, M_ROOM, M_ROMS, M_DASHBOARD };   // (About: Settings > About)
enum { ST_SLOT = 0, ST_LOAD, ST_SAVE };   // the States page, opened from M_STATES; B goes back, no row for it

static const Config *s_menuCfg;   // the settings the open menu shows (its HD textures row)

// The pause menu's rows, as M_* in order: HD textures only when the game has packs, Back to the room only
// online through a room, Players online with an online profile and a network.
static int s_pauseRows[10], s_pauseCount;

static int PauseRow(int action)
{
    int i;
    for (i = 0; i < s_pauseCount; i++)
        if (s_pauseRows[i] == action) return i;
    return 0;
}

static void BuildGameMenu(void)
{
    static const int labels[10] = { T_RESUME, T_STATES, T_CHEATS, T_HIRESTEX, T_SETTINGS, T_ONLINE_NOW, T_RESET_GAME, T_BACK_TO_ROOM, T_BACK_TO_LIST, T_PAUSE_QUIT };
    static const unsigned char iconOf[10] = { UI_ICON_PLAY, UI_ICON_STATES, UI_ICON_SLIDERS, UI_ICON_HD, UI_ICON_GEAR, UI_ICON_GLOBE, UI_ICON_RESET, UI_ICON_FRIENDS,
                                              UI_ICON_LIST, UI_ICON_POWER };
    unsigned char icons[10];
    char value[160];
    int a;
    UiMenuClear(&s_menu, Tr(T_PAUSED), Tr(T_FOOT_PAUSED));
    s_menu.rowMax = 40;   // shorter rows: "Now playing" (the game's name and picture) fits under them
    s_pauseCount = 0;
    static const int order[10] = { M_RESUME, M_STATES, M_CHEATS, M_RESET, M_TEXTURES, M_SETTINGS, M_PLAYERS, M_ROOM, M_ROMS, M_DASHBOARD };   // by group (PauseDraw)
    int o;
    for (o = 0; o < 10; o++)
    {
        a = order[o];
        if (a == M_TEXTURES && HiresPackCount() == 0) continue;
        if (s_netMode > 0 && (a == M_STATES || a == M_CHEATS || a == M_TEXTURES || a == M_RESET)) continue;   // online: what would set this console apart
        if (a == M_ROOM && !(s_netMode > 0 && s_netWs) && s_netMode != -1) continue;   // the room's lobby; online again once cut off
        if (a == M_PLAYERS && !PlayersAvailable()) continue;
        value[0] = 0;
        if (a == M_CHEATS) CheatsRowValue(value, sizeof(value));
        if (a == M_TEXTURES && s_menuCfg) TexturesRowValue(s_menuCfg, value, sizeof(value));
        // (an online game cut off - put out, the connection lost: the room's row is Back to online)
        UiMenuAdd(&s_menu, Tr(a == M_ROOM && s_netMode == -1 ? T_BACK_TO_ONLINE : labels[a]), value);
        icons[s_pauseCount] = a == M_ROOM && s_netMode == -1 ? UI_ICON_GLOBE : iconOf[a];
        s_pauseRows[s_pauseCount++] = a;
    }
    UiMenuIcons(&s_menu, icons, s_pauseCount);
}

// The pause menu's own page: the game behind blurred and dimmed; on the left a solid panel - the game's box, name,
// kind (hack...), the time played this session, its HD pack - then the options in groups (the game, the picture,
// online, leaving: the way out in red); on the right the frame as it was when the menu opened ("Paused"), and two
// small cards (the quick save's slot and when it was saved; the controllers connected).
// How long ago, as one would say it: "hace 5 min", "hace 3 h", "hace 2 días" (short: "5 min", "3 h", "2 días").
static void AgoText(int mins, int shortForm, char *out, size_t size)
{
    if (mins < 60) _snprintf(out, size - 1, Tr(shortForm ? T_AGO_S_MIN : T_AGO_MIN), mins);
    else if (mins < 48 * 60) _snprintf(out, size - 1, Tr(shortForm ? T_AGO_S_H : T_AGO_H), mins / 60);
    else _snprintf(out, size - 1, Tr(shortForm ? T_AGO_S_D : T_AGO_D), mins / (24 * 60));
    out[size - 1] = 0;
}

// When slot `slot`'s state was saved: minutes ago (-1: none there).
static int SlotMinutes(int slot)
{
    char path[160];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILETIME now;
    ULARGE_INTEGER a, b;
    StatePath(slot, path, sizeof(path));
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return -1;
    GetSystemTimeAsFileTime(&now);
    a.LowPart = now.dwLowDateTime; a.HighPart = now.dwHighDateTime;
    b.LowPart = fa.ftLastWriteTime.dwLowDateTime; b.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    return a.QuadPart > b.QuadPart ? (int)((a.QuadPart - b.QuadPart) / 600000000ULL) : 0;
}

// The slots' small pictures and times (s_slotTex, s_slotMins), from their state's picture files.
static void SlotsMake(IDirect3DDevice9 *dev)
{
    int k;
    for (k = 1; k <= CONFIG_STATE_SLOTS; k++)
    {
        char path[160];
        u32 size = 0;
        u8 *data;
        if (s_slotTex[k]) { UiImageRelease(dev, s_slotTex[k]); s_slotTex[k] = NULL; }
        s_slotMins[k] = SlotMinutes(k);
        if (s_slotMins[k] < 0) continue;
        ThumbPath(k, path, sizeof(path));
        data = LoadFile(path, &size);
        if (data && size >= 12 && !memcmp(data, "H64T", 4))
        {
            u32 w, h;
            memcpy(&w, data + 4, 4);
            memcpy(&h, data + 8, 4);
            if (w && h && w <= THUMB_W && h <= THUMB_H && size >= 12 + w * h * 4)
            {
                u8 *small = h64_image_scale(data + 12, (int)w, (int)h, SLOT_TILE_W, SLOT_TILE_H);
                if (small) { s_slotTex[k] = UiImageCreate(dev, small, SLOT_TILE_W, SLOT_TILE_H); free(small); }
            }
        }
        free(data);
    }
    s_slotsMade = 1;
}

// The States page (PauseDraw, states): under the game's header its rows - the slot "< 3 >", load (when it was
// saved), save; the reserved slot told of; on the right the selected slot's picture, large, and all the slots in two
// rows, each its small picture, number and time, the selected one ringed, the reserved one "auto".
static void StatesBody(IDirect3DDevice9 *dev, float PX, float PY, float PW, float PH, float y)
{
    const float GX = 600, GY = 36, GW = 640, GH = 360;
    const D3DCOLOR selHi = UiSelColor(0, 255), selLo = UiSelColor(1, 150);
    static const D3DCOLOR iconCol[3] = { D3DCOLOR_ARGB(255, 240, 180, 80), D3DCOLOR_ARGB(255, 90, 170, 255), D3DCOLOR_ARGB(255, 120, 220, 140) };
    char line[200], ago[64];
    int i, k;
    if (!s_slotsMade) SlotsMake(dev);
    StateThumbSync(dev);
    UiText(UI_BOLD, 12, PX + 30, y, UI_COL_TEXT3, Tr(T_STATES_HEAD), UI_LEFT);
    y += 20;
    for (i = 0; i < s_menu.count && i < 3; i++)
    {
        const float cy = y + 18;
        const int sel = i == s_menu.sel;
        if (sel)
        {
            UiRoundRect(PX + 16, cy - 20, PW - 32, 40, 12, selLo, selLo);
            UiRoundRing(PX + 16, cy - 20, PW - 32, 40, 12, 2.0f, selHi);
        }
        UiIcon(s_menu.icon[i], PX + 46, cy, iconCol[i]);
        UiText(sel ? UI_BOLD : UI_REGULAR, 20, PX + 70, cy, UI_COL_TEXT, s_menu.label[i], UI_LEFT);
        if (i == 0) UiText(sel ? UI_BOLD : UI_REGULAR, 17, PX + PW - 40, cy, sel ? UI_COL_TEXT : UI_COL_TEXT3, s_menu.value[0], UI_RIGHT);
        else if (i == 1)
        {
            // load: when the slot's state was saved (or there is none)
            const int mins = s_slotMins[s_stateSlot];
            if (mins >= 0) AgoText(mins, 0, ago, sizeof(ago));
            else _snprintf(ago, sizeof(ago) - 1, "%s", Tr(T_STATE_EMPTY_LOW));
            ago[sizeof(ago) - 1] = 0;
            UiText(UI_REGULAR, 17, PX + PW - 40, cy, UI_COL_TEXT3, ago, UI_RIGHT);
        }
        y += 44;
    }
    if (s_reservedSlot > 0)
    {
        // the slot kept for the save made when a controller is unplugged
        char wrapped[300];
        int lines;
        _snprintf(line, sizeof(line) - 1, Tr(T_STATES_RESERVED), s_reservedSlot);
        line[sizeof(line) - 1] = 0;
        lines = UiTextWrap(UI_REGULAR, 14, PW - 60, 3, line, wrapped, sizeof(wrapped));
        UiText(UI_REGULAR, 14, PX + 30, y + 20 + (lines - 1) * 9.0f, UI_COL_TEXT3, wrapped, UI_LEFT);
    }
    UiRect(PX + 24, PY + PH - 52, PW - 48, 1, UI_COL_LINE);
    UiFooter(PX + 30, PY + PH - 26, Tr(T_FOOT_STATES_PANEL));
    // the selected slot's picture, large, and when it was saved
    UiEdgeGlow(GX, GY, GW, GH, 18);
    UiPanel(GX, GY, GW, GH, 18);
    if (s_stateThumb)
        UiImage(s_stateThumb, GX + 4, GY + 4, GW - 8, GH - 8, 16, D3DCOLOR_ARGB(255, 255, 255, 255));
    else
    {
        UiIcon(UI_ICON_SAVE, GX + GW * 0.5f, GY + GH * 0.5f - 18, UI_COL_TEXT3);
        UiText(UI_REGULAR, 18, GX + GW * 0.5f, GY + GH * 0.5f + 16, UI_COL_TEXT3, Tr(T_STATE_EMPTY), UI_CENTER);
    }
    if (s_slotMins[s_stateSlot] >= 0)
    {
        AgoText(s_slotMins[s_stateSlot], 0, ago, sizeof(ago));
        _snprintf(line, sizeof(line) - 1, "%s %d  " UI_CH_MIDDOT "  %s", Tr(T_STATE_SLOT_WORD), s_stateSlot, ago);
    }
    else _snprintf(line, sizeof(line) - 1, "%s %d", Tr(T_STATE_SLOT_WORD), s_stateSlot);
    line[sizeof(line) - 1] = 0;
    {
        const float pw = UiTextWidth(UI_BOLD, 15, line) + 28;
        UiRoundRect(GX + 16, GY + 16, pw, 30, 15, D3DCOLOR_ARGB(170, 0, 0, 0), D3DCOLOR_ARGB(170, 0, 0, 0));
        UiText(UI_BOLD, 15, GX + 30, GY + 31, UI_COL_TEXT, line, UI_LEFT);
    }
    // all the slots: two rows
    UiText(UI_BOLD, 12, GX, GY + GH + 24, UI_COL_TEXT3, Tr(T_STATES_SLOTS), UI_LEFT);
    for (k = 1; k <= CONFIG_STATE_SLOTS; k++)
    {
        const int col = (k - 1) % 9, row = (k - 1) / 9;
        const float x = GX + col * (SLOT_TILE_W + 8), ty = GY + GH + 40 + row * 92;
        UiPanel(x, ty, SLOT_TILE_W, SLOT_TILE_H + 32, 10);
        if (s_slotTex[k]) UiImage(s_slotTex[k], x + 3, ty + 3, SLOT_TILE_W - 6, SLOT_TILE_H - 6, 6, D3DCOLOR_ARGB(255, 255, 255, 255));
        else
        {
            UiRoundRect(x + 3, ty + 3, SLOT_TILE_W - 6, SLOT_TILE_H - 6, 6, D3DCOLOR_ARGB(14, 255, 255, 255), D3DCOLOR_ARGB(14, 255, 255, 255));
            UiText(UI_REGULAR, 11, x + SLOT_TILE_W * 0.5f, ty + SLOT_TILE_H * 0.5f, UI_COL_TEXT3, Tr(T_STATE_EMPTY_LOW), UI_CENTER);
        }
        _snprintf(line, sizeof(line) - 1, "%d", k);
        line[sizeof(line) - 1] = 0;
        UiText(UI_BOLD, 14, x + SLOT_TILE_W * 0.5f, ty + SLOT_TILE_H + 9, k == s_stateSlot ? UI_COL_TEXT : UI_COL_TEXT2, line, UI_CENTER);
        if (k == s_reservedSlot) UiText(UI_BOLD, 10, x + SLOT_TILE_W * 0.5f, ty + SLOT_TILE_H + 23, D3DCOLOR_ARGB(255, 240, 180, 80), Tr(T_AUTO_TAG), UI_CENTER);
        else if (s_slotMins[k] >= 0)
        {
            AgoText(s_slotMins[k], 1, ago, sizeof(ago));
            UiText(UI_REGULAR, 10, x + SLOT_TILE_W * 0.5f, ty + SLOT_TILE_H + 23, UI_COL_TEXT3, ago, UI_CENTER);
        }
        if (k == s_stateSlot) UiRoundRing(x - 3, ty - 3, SLOT_TILE_W + 6, SLOT_TILE_H + 38, 12, 3.0f, selHi);
    }
}

// Settings' own page in a game (PauseDraw, 2): under the game's header its sections as rows - an icon of its colour,
// its name, what it holds now - and the two ways to keep them; on the right the frame ("Paused") and a card with the
// selected row: its name, what it is for, what it holds.
static void SettingsBody(float PX, float PY, float PW, float PH, float y)
{
    const float GX = 600, GY = 36, GW = 640, GH = 360;
    const D3DCOLOR selHi = UiSelColor(0, 255), selLo = UiSelColor(1, 150);
    int i, saving = -1;
    D3DCOLOR selCol = D3DCOLOR_ARGB(255, 120, 220, 170);
    UiText(UI_BOLD, 12, PX + 30, y, UI_COL_TEXT3, Tr(T_SETTINGS_HEAD), UI_LEFT);
    y += 20;
    for (i = 0; i < s_menu.count; i++)
    {
        const int sel = i == s_menu.sel, save = s_menu.icon[i] == UI_ICON_SAVE;
        const int ic = s_menu.icon[i];
        const D3DCOLOR col = ic == UI_ICON_GRAPHICS ? D3DCOLOR_ARGB(255, 236, 110, 170) : ic == UI_ICON_SCREEN ? D3DCOLOR_ARGB(255, 90, 190, 255) :
                             ic == UI_ICON_CONTROLS ? D3DCOLOR_ARGB(255, 110, 220, 130) : ic == UI_ICON_SOUND ? D3DCOLOR_ARGB(255, 250, 170, 60) :
                             ic == UI_ICON_SYSTEM ? D3DCOLOR_ARGB(255, 110, 150, 255) : D3DCOLOR_ARGB(255, 120, 220, 170);
        const float rh = save ? 38.0f : 48.0f;
        float cy;
        if (save && saving < 0)
        {
            // the ways to keep them, apart
            saving = i;
            y += 14;
            UiText(UI_BOLD, 12, PX + 30, y, UI_COL_TEXT3, Tr(T_SETTINGS_KEEP_HEAD), UI_LEFT);
            y += 20;
        }
        cy = y + rh * 0.5f;
        if (sel)
        {
            selCol = col;
            UiRoundRect(PX + 16, cy - rh * 0.5f + 1, PW - 32, rh - 2, 12, selLo, selLo);
            UiRoundRing(PX + 16, cy - rh * 0.5f + 1, PW - 32, rh - 2, 12, 2.0f, selHi);
        }
        if (!save) UiCircle(PX + 46, cy, 16, UiAlpha(col, 0.18f));
        UiIcon(ic, PX + 46, cy, col);
        UiText(sel ? UI_BOLD : UI_REGULAR, save ? 18 : 19, PX + 76, cy, UI_COL_TEXT, s_menu.label[i], UI_LEFT);
        if (!save && UiSettingsSub(i)[0])
        {
            char fit[120];
            UiTextFit(UI_REGULAR, 14, PW - 260, UiSettingsSub(i), fit, sizeof(fit));
            UiText(UI_REGULAR, 14, PX + PW - 40, cy, sel ? UI_COL_TEXT2 : UI_COL_TEXT3, fit, UI_RIGHT);
        }
        y += rh;
    }
    UiRect(PX + 24, PY + PH - 52, PW - 48, 1, UI_COL_LINE);
    UiFooter(PX + 30, PY + PH - 26, Tr(T_FOOT_SETTINGS_PANEL));
    // the frame as it was, "Paused"
    if (s_pauseShotTex)
    {
        UiEdgeGlow(GX, GY, GW, GH, 18);
        UiPanel(GX, GY, GW, GH, 18);
        UiImage(s_pauseShotTex, GX + 4, GY + 4, GW - 8, GH - 8, 16, D3DCOLOR_ARGB(255, 255, 255, 255));
        {
            const char *t = Tr(T_PAUSE_PAUSED);
            const float pw = UiTextWidth(UI_BOLD, 15, t) + 46;
            UiRoundRect(GX + 16, GY + 16, pw, 30, 15, D3DCOLOR_ARGB(170, 0, 0, 0), D3DCOLOR_ARGB(170, 0, 0, 0));
            UiRect(GX + 30, GY + 24, 4, 14, D3DCOLOR_ARGB(255, 255, 255, 255));
            UiRect(GX + 38, GY + 24, 4, 14, D3DCOLOR_ARGB(255, 255, 255, 255));
            UiText(UI_BOLD, 15, GX + 50, GY + 31, UI_COL_TEXT, t, UI_LEFT);
        }
    }
    // the selected row's card: its name, what it is for, what it holds
    if (s_menu.sel >= 0 && s_menu.sel < s_menu.count)
    {
        const float cy0 = GY + GH + 24, ch = 720 - 36 - cy0;
        const char *desc = UiRowDesc(s_menu.label[s_menu.sel]), *sub = UiSettingsSub(s_menu.sel);
        char wrapped[400];
        int lines = 0;
        UiEdgeGlow(GX, cy0, GW, ch, 18);
        UiPanel(GX, cy0, GW, ch, 18);
        UiRoundRect(GX + 20, cy0 + 18, 54, 3, 1.5f, selCol, selCol);
        UiText(UI_BOLD, 20, GX + 20, cy0 + 42, UI_COL_TEXT, s_menu.label[s_menu.sel], UI_LEFT);
        if (desc && desc[0])
        {
            lines = UiTextWrap(UI_REGULAR, 15, GW - 40, 3, desc, wrapped, sizeof(wrapped));
            UiText(UI_REGULAR, 15, GX + 20, cy0 + 70 + (lines - 1) * 10.0f, UI_COL_TEXT3, wrapped, UI_LEFT);
        }
        if (s_menu.icon[s_menu.sel] != UI_ICON_SAVE && sub && sub[0])
        {
            char fit[160];
            UiTextFit(UI_BOLD, 17, GW - 40, sub, fit, sizeof(fit));
            UiText(UI_BOLD, 17, GX + 20, cy0 + 96 + (lines > 1 ? (lines - 1) * 20.0f : 0), UI_COL_TEXT, fit, UI_LEFT);
        }
    }
}

// Behind the pause menu's pages (its own, States, Settings and their pages): the frame as the menu opened, tiny and
// stretched (blurred), dimmed - the game never showing through the menu.
// The game's name as the pause menu shows it: a hack's (or a ROM the catalogue does not know: a newer hack) as its
// file is named, its version in it - the databases' title is its game's ("Super Smash Bros." for Smash Remix).
static int PauseHack(void)
{
    return s_pauseKind == H64_ROMKIND_HACK || s_pauseKind == H64_ROMKIND_OTHER || s_pauseKind == H64_ROMKIND_UNKNOWN;
}

static const char *PauseTitle(char *out, size_t size)
{
    const char *base = strrchr(s_romNow, '\\'), *dot;
    base = base ? base + 1 : s_romNow;
    dot = strrchr(base, '.');
    if (PauseHack() && base[0]) _snprintf(out, size - 1, "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
    else _snprintf(out, size - 1, "%s", s_npTitle[0] ? s_npTitle : "LAIKA 64");
    out[size - 1] = 0;
    return out;
}

static void PauseBackdrop(IDirect3DDevice9 *dev)
{
    if (s_pauseShotRgba && !s_pauseShotTex) { s_pauseShotTex = UiImageCreate(dev, s_pauseShotRgba, s_pauseShotW, s_pauseShotH); free(s_pauseShotRgba); s_pauseShotRgba = NULL; }
    if (s_pauseBlurRgba && !s_pauseBlurTex) { s_pauseBlurTex = UiImageCreate(dev, s_pauseBlurRgba, s_pauseBlurW, s_pauseBlurH); free(s_pauseBlurRgba); s_pauseBlurRgba = NULL; }
    if (s_pauseBlurTex) UiImage(s_pauseBlurTex, 0, 0, UI_WIDTH, UI_HEIGHT, 0, D3DCOLOR_ARGB(255, 255, 255, 255));
    UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB(s_pauseBlurTex ? 165 : 200, 6, 7, 14));
}

static void PauseDraw(IDirect3DDevice9 *dev, int states)
{
    const float PX = 40, PY = 36, PW = 520, PH = 648, GX = 600, GY = 96, GW = 640, GH = 360;
    const D3DCOLOR selHi = UiSelColor(0, 255), selLo = UiSelColor(1, 150);
    const Config *c = s_menuCfg;
    char line[200], fit[200];
    float y, bx, by, cx;
    int i, group = -1;
    PauseBackdrop(dev);
    // the panel, as the player's panels are (Interface: colour, opacity, border and its glow)
    UiEdgeGlow(PX, PY, PW, PH, 24);
    UiPanel(PX, PY, PW, PH, 24);
    // the game: its box (or picture), name, kind, time, HD pack
    bx = PX + 24;
    by = PY + 24;
    if (s_npTex && s_npW > 0 && s_npH > 0)
    {
        const float k = 78.0f / s_npW < 104.0f / s_npH ? 78.0f / s_npW : 104.0f / s_npH, w = s_npW * k, h = s_npH * k;
        UiImage(s_npTex, bx + (78 - w) * 0.5f, by + (104 - h) * 0.5f, w, h, 8, D3DCOLOR_ARGB(255, 255, 255, 255));
    }
    else
    {
        UiRoundRect(bx, by, 78, 104, 8, UI_COL_CARD, UI_COL_CARD);
        UiIcon(UI_ICON_PLAY, bx + 39, by + 52, UI_COL_TEXT3);
    }
    {
        char name[160];
        UiTextFit(UI_BOLD, 26, PW - 140, PauseTitle(name, sizeof(name)), fit, sizeof(fit));   // (a hack: its file's name)
    }
    UiText(UI_BOLD, 26, bx + 96, by + 18, UI_COL_TEXT, fit, UI_LEFT);
    cx = bx + 96;
    if (s_pauseKind == H64_ROMKIND_HACK || s_pauseKind == H64_ROMKIND_OTHER || s_pauseKind == H64_ROMKIND_UNKNOWN || s_pauseKind == H64_ROMKIND_TRANSLATION)
    {
        const char *t = Tr(s_pauseKind == H64_ROMKIND_TRANSLATION ? T_ROMKIND_TRANSLATION : T_ROMKIND_HACK);
        const D3DCOLOR col = s_pauseKind == H64_ROMKIND_TRANSLATION ? D3DCOLOR_ARGB(255, 90, 170, 255) : D3DCOLOR_ARGB(255, 255, 150, 70);
        const float w = UiTextWidth(UI_BOLD, 12, t) + 16;
        UiRoundRect(cx, by + 40, w, 18, 9, UiAlpha(col, 0.18f), UiAlpha(col, 0.18f));
        UiRoundRing(cx, by + 40, w, 18, 9, 1.0f, UiAlpha(col, 0.6f));
        UiText(UI_BOLD, 12, cx + 8, by + 49, col, t, UI_LEFT);
    }
    {
        const int mins = (int)((GetTickCount() - s_playStart) / 60000);
        _snprintf(line, sizeof(line) - 1, Tr(T_PAUSE_PLAYING), mins < 1 ? 1 : mins);
        line[sizeof(line) - 1] = 0;
        UiText(UI_REGULAR, 15, bx + 96, by + 76 + (c && c->hiTex && HiresActiveIndex(c) >= 0 ? 0 : 10), UI_COL_TEXT3, line, UI_LEFT);
    }
    if (c && c->hiTex && HiresActiveIndex(c) >= 0)
    {
        char name[128];
        TexturesRowValue(c, name, sizeof(name));
        _snprintf(line, sizeof(line) - 1, Tr(T_PAUSE_PACK), name);
        line[sizeof(line) - 1] = 0;
        UiTextFit(UI_REGULAR, 13, PW - 140, line, fit, sizeof(fit));
        UiText(UI_REGULAR, 13, bx + 96, by + 96, UI_COL_TEXT3, fit, UI_LEFT);
    }
    UiRect(PX + 24, by + 124, PW - 48, 1, UI_COL_LINE);
    if (states == 1) { StatesBody(dev, PX, PY, PW, PH, by + 140); return; }   // the States page: its rows and slots
    if (states == 2) { SettingsBody(PX, PY, PW, PH, by + 140); return; }        // Settings: its sections, keeping them
    // the options, in their groups
    for (i = 0, y = by + 140; i < s_pauseCount && i < s_menu.count; i++)
    {
        static const int groupOf[10] = { 0, 0, 0, 1, 1, 2, 0, 2, 3, 3 };   // by M_*: the game, the picture, online, leaving
        static const int groupName[4] = { T_PAUSE_G_GAME, T_PAUSE_G_PICTURE, T_PAUSE_G_ONLINE, T_PAUSE_G_LEAVE };
        static const D3DCOLOR iconCol[10] = { D3DCOLOR_ARGB(255, 120, 220, 140), D3DCOLOR_ARGB(255, 240, 180, 80), D3DCOLOR_ARGB(255, 180, 150, 250),
                                              D3DCOLOR_ARGB(255, 240, 200, 70), D3DCOLOR_ARGB(255, 150, 170, 200), D3DCOLOR_ARGB(255, 80, 210, 120),
                                              D3DCOLOR_ARGB(255, 250, 140, 120), D3DCOLOR_ARGB(255, 120, 170, 255), D3DCOLOR_ARGB(255, 120, 170, 255),
                                              D3DCOLOR_ARGB(255, 240, 90, 80) };
        const int a = s_pauseRows[i], sel = i == s_menu.sel;
        float cy;
        if (groupOf[a] != group)
        {
            if (group >= 0) y += 16;
            group = groupOf[a];
            UiText(UI_BOLD, 12, PX + 30, y, UI_COL_TEXT3, Tr(groupName[group]), UI_LEFT);
            y += 20;
        }
        cy = y + 16;
        if (sel)
        {
            UiRoundRect(PX + 16, cy - 18, PW - 32, 36, 12, selLo, selLo);
            UiRoundRing(PX + 16, cy - 18, PW - 32, 36, 12, 2.0f, selHi);
        }
        UiIcon(s_menu.icon[i], PX + 46, cy, iconCol[a]);
        UiText(sel ? UI_BOLD : UI_REGULAR, 19, PX + 70, cy, a == M_DASHBOARD ? D3DCOLOR_ARGB(255, 240, 90, 80) : UI_COL_TEXT, s_menu.label[i], UI_LEFT);
        if (a == M_TEXTURES && c)
        {
            // a switch: on (green, the knob right) or off
            const int on = c->hiTex && HiresActiveIndex(c) >= 0;
            const float sx = PX + PW - 84;
            UiRoundRect(sx, cy - 12, 44, 24, 12, on ? D3DCOLOR_ARGB(255, 80, 210, 120) : D3DCOLOR_ARGB(255, 70, 74, 92), on ? D3DCOLOR_ARGB(255, 80, 210, 120) : D3DCOLOR_ARGB(255, 70, 74, 92));
            UiCircle(on ? sx + 31 : sx + 13, cy, 9, D3DCOLOR_ARGB(255, 255, 255, 255));
        }
        else if (a == M_STATES)
        {
            _snprintf(line, sizeof(line) - 1, Tr(T_PAUSE_SLOT), s_stateSlot);
            line[sizeof(line) - 1] = 0;
            UiText(UI_REGULAR, 15, PX + PW - 40, cy, UI_COL_TEXT3, line, UI_RIGHT);
        }
        else if (a == M_PLAYERS)
        {
            std::vector<OnlineUser> users;
            if (OnlineWatchedUsers(&users) > 0 && !users.empty())
            {
                _snprintf(line, sizeof(line) - 1, "%d", (int)users.size());
                line[sizeof(line) - 1] = 0;
                {
                    const float w = UiTextWidth(UI_BOLD, 13, line) + 18;
                    UiRoundRect(PX + PW - 40 - w, cy - 11, w, 22, 11, D3DCOLOR_ARGB(255, 80, 210, 120), D3DCOLOR_ARGB(255, 80, 210, 120));
                    UiText(UI_BOLD, 13, PX + PW - 40 - w * 0.5f, cy, D3DCOLOR_ARGB(255, 10, 30, 15), line, UI_CENTER);
                }
            }
        }
        else if (s_menu.value[i][0]) UiText(UI_REGULAR, 15, PX + PW - 40, cy, UI_COL_TEXT3, s_menu.value[i], UI_RIGHT);
        y += 32;
    }
    UiRect(PX + 24, PY + PH - 52, PW - 48, 1, UI_COL_LINE);
    UiFooter(PX + 30, PY + PH - 26, Tr(T_FOOT_PAUSE_PANEL));   // (shorter than the menu's: within the panel)
    // the frame as it was, "Paused"
    if (s_pauseShotTex)
    {
        const float k = (float)GW / s_pauseShotW < (float)GH / s_pauseShotH ? (float)GW / s_pauseShotW : (float)GH / s_pauseShotH;
        const float w = s_pauseShotW * k, h = s_pauseShotH * k, fx = GX + (GW - w) * 0.5f, fy = GY + (GH - h) * 0.5f;
        UiShadow(fx + 4, fy + 10, w, h, 18, 24, D3DCOLOR_ARGB(140, 0, 0, 0));
        UiEdgeGlow(fx, fy, w, h, 18);
        UiImage(s_pauseShotTex, fx, fy, w, h, 18, D3DCOLOR_ARGB(255, 255, 255, 255));
        if (UiPanelEdgeWidth() > 0) UiRoundRing(fx, fy, w, h, 18, UiPanelEdgeWidth(), UiPanelEdgeColor());   // the player's border
        {
            const char *t = Tr(T_PAUSE_PAUSED);
            const float pw = UiTextWidth(UI_BOLD, 15, t) + 46;
            UiRoundRect(fx + 16, fy + 16, pw, 30, 15, D3DCOLOR_ARGB(170, 0, 0, 0), D3DCOLOR_ARGB(170, 0, 0, 0));
            UiRect(fx + 30, fy + 24, 4, 14, D3DCOLOR_ARGB(255, 255, 255, 255));
            UiRect(fx + 38, fy + 24, 4, 14, D3DCOLOR_ARGB(255, 255, 255, 255));
            UiText(UI_BOLD, 15, fx + 50, fy + 31, UI_COL_TEXT, t, UI_LEFT);
        }
    }
    // the quick save and the controllers
    for (i = 0; i < 2; i++)
    {
        const float x0 = GX + i * (GW / 2 + 8), w = GW / 2 - 8, y0 = GY + GH + 26;
        const D3DCOLOR col = i ? D3DCOLOR_ARGB(255, 90, 170, 255) : D3DCOLOR_ARGB(255, 240, 180, 80);
        UiEdgeGlow(x0, y0, w, 84, 16);
        UiPanel(x0, y0, w, 84, 16);
        UiRoundRect(x0 + 16, y0 + 16, 54, 3, 1.5f, col, col);
        UiText(UI_BOLD, 17, x0 + 16, y0 + 38, UI_COL_TEXT, Tr(i ? T_PAUSE_PADS : T_PAUSE_QUICK), UI_LEFT);
        if (!i)
        {
            // the slot, and how long ago it was saved (or empty)
            char path[160];
            WIN32_FILE_ATTRIBUTE_DATA fa;
            StatePath(s_stateSlot, path, sizeof(path));
            if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
            {
                FILETIME now;
                ULARGE_INTEGER a, b;
                GetSystemTimeAsFileTime(&now);
                a.LowPart = now.dwLowDateTime; a.HighPart = now.dwHighDateTime;
                b.LowPart = fa.ftLastWriteTime.dwLowDateTime; b.HighPart = fa.ftLastWriteTime.dwHighDateTime;
                {
                    // the slot, how long ago (minutes, hours or days)
                    char ago[64];
                    AgoText(a.QuadPart > b.QuadPart ? (int)((a.QuadPart - b.QuadPart) / 600000000ULL) : 0, 0, ago, sizeof(ago));
                    _snprintf(line, sizeof(line) - 1, "%s %d  " UI_CH_MIDDOT "  %s", Tr(T_STATE_SLOT_WORD), s_stateSlot, ago);
                }
            }
            else _snprintf(line, sizeof(line) - 1, Tr(T_PAUSE_SLOT_EMPTY), s_stateSlot);
        }
        else
        {
            // the controllers connected
            XINPUT_STATE st;
            int k, n = 0;
            for (k = 0; k < 4; k++)
                if (XInputGetState(k, &st) == ERROR_SUCCESS) n++;
            if (n == 1) _snprintf(line, sizeof(line) - 1, "%s", Tr(T_PAUSE_PADS_1));
            else _snprintf(line, sizeof(line) - 1, Tr(T_PAUSE_PADS_N), n);
        }
        line[sizeof(line) - 1] = 0;
        UiText(UI_REGULAR, 14, x0 + 16, y0 + 62, UI_COL_TEXT3, line, UI_LEFT);
    }
}

// The States page's slot value: "< 3 >", the reserved slot tagged "< 18 · auto >".
static void SlotValue(char *out, size_t size)
{
    if (s_stateSlot == s_reservedSlot) _snprintf(out, size - 1, "< %d " UI_CH_MIDDOT " %s >", s_stateSlot, Tr(T_AUTO_TAG));
    else _snprintf(out, size - 1, "< %d >", s_stateSlot);
    out[size - 1] = 0;
}

static void BuildStatesMenu(void)
{
    char slot[64];
    UiMenuClear(&s_menu, Tr(T_STATES), Tr(T_FOOT_SELECT));
    SlotValue(slot, sizeof(slot));
    UiMenuAdd(&s_menu, Tr(T_STATE_SLOT), slot);
    UiMenuAdd(&s_menu, Tr(T_LOAD_STATE), "");
    UiMenuAdd(&s_menu, Tr(T_SAVE_STATE), "");
    {
        static const unsigned char icons[] = { UI_ICON_SLOT, UI_ICON_LOAD, UI_ICON_SAVE };
        UiMenuIcons(&s_menu, icons, sizeof(icons));
    }
}

static void ResetGame(H64System *sys)
{
    if (NetRefuse()) return;
    h64_hle_async_wait(sys);
    if (sys->asyncAudioWait) sys->asyncAudioWait(sys->asyncUser);
    h64_system_reset(sys);
    if (sys->renderer && sys->renderer->reset) sys->renderer->reset(sys->renderer->user);
    OsdT(T_GAME_RESET, 0);
}

// Runs the menu; returns RG_CONTINUE, RG_BROWSER or RG_DASHBOARD.
static int GameMenu(H64System *sys, H64Renderer *renderer, Config *c)
{
    UiInput in;
    int settings = 0, states = 0, sel, result = -1;
    // ApplyGraphics only flips the hiTex on/off switch (H64XenosOptions has no
    // file path) - picking a *different* pack while playing needs its own
    // reload, or the renderer keeps showing whatever HiresOpen loaded at
    // launch. Snapshot what's already loaded now, catch a change below.
    int hiTexApplied = c->hiTex;
    char hiTexFileApplied[sizeof(c->hiTexFile)];
    strncpy(hiTexFileApplied, c->hiTexFile, sizeof(hiTexFileApplied) - 1);
    hiTexFileApplied[sizeof(hiTexFileApplied) - 1] = 0;
    if (s_gfxQ.thread) WorkWaitAll(NULL);   // the device is this thread's while the menu shows
    s_menuCfg = c;
    if (renderer && !s_pauseShotTex && !s_pauseShotRgba)
    {
        // the frame as it is now: large for the pause menu's picture of it, tiny to blur it behind the menu
        int fw = 0, fh = 0;
        u8 *px = h64_xenos_grab_frame(renderer, &fw, &fh);
        if (px)
        {
            h64_image_fit(fw, fh, 640, 360, &s_pauseShotW, &s_pauseShotH);
            s_pauseShotRgba = h64_image_scale(px, fw, fh, s_pauseShotW, s_pauseShotH);
            s_pauseBlurW = 48;
            s_pauseBlurH = 27;
            s_pauseBlurRgba = h64_image_scale(px, fw, fh, s_pauseBlurW, s_pauseBlurH);
            free(px);
        }
    }
    s_pauseKind = h64_rom_catalogue(sys->rom.crc1, sys->rom.crc2, NULL);
    BuildGameMenu();
    if (!s_npTitle[0] && s_romNow[0])
    {
        // "Now playing": the game's name and picture, once per game (the texture stays until it ends).
        NowPlayingLoad(s_romNow, s_npTitle, sizeof(s_npTitle), &s_npRgba, &s_npW, &s_npH, &s_npBox);
    }
    UiInputInit(&in);
    s_menuOpen = 1;
    xb_audio_ui_sound(XB_SFX_OPEN);   // the pause menu opens (Settings > Sound > Interface sounds)
    while (result < 0)
    {
        WORD down = UiInputPoll(&in);
        NetKeepAlive();   // the other console waits: it hears this one is still here
        s_reservedSlot = c->padLoss == PADLOSS_SAVE ? c->autoSaveSlot : 0;   // Settings > System may change it from here
        if (!settings && !s_aboutOpen && !s_hiresPreviewOpen && !s_confirmOpen && !s_cheatsOpen && !s_texOpen && !s_playersOpen && (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)))
            UiMenuPress();   // the pause menu's and the States page's rows show the press (Settings does it itself)
        if (s_confirmOpen)
        {
            // "Back to the ROM list?" / "Quit to the dashboard?" / "Reset the game?": A does it, B or BACK
            // stays in the menu.
            if ((down & XINPUT_GAMEPAD_A) && (s_confirmOpen == CONFIRM_SAVE_GAME || s_confirmOpen == CONFIRM_SAVE_ALL))
            {
                // "Save for this game / for all games?": A saves and goes back to the menu, B stays in Settings.
                int all = s_confirmOpen == CONFIRM_SAVE_ALL, kept = c->hiTex, ok;
                const char *path = all ? s_settingsPath : s_profilePath;
                s_confirmOpen = 0;
                if (s_netHiTex >= 0) c->hiTex = s_netHiTex;   // online turned the HD pack off for this game only
                ok = path[0] && ConfigWriteMenuKeys(c, path, all);
                c->hiTex = kept;
                if (!path[0]) OsdT(T_NOT_SAVED_SCRIPT, 0);
                else if (ok)
                {
                    OsdT(all ? T_SAVED_ALL : T_SAVED_GAME, 0);
                    xb_audio_ui_sound_instead(XB_SFX_SAVE);
                }
                else OsdT(T_SETTINGS_WRITE_FAILED, 0);
                settings = 0;
                BuildGameMenu();
                s_menu.sel = PauseRow(M_SETTINGS);
            }
            else if ((down & XINPUT_GAMEPAD_A) && s_confirmOpen == CONFIRM_RESET) { s_confirmOpen = 0; ResetGame(sys); result = RG_CONTINUE; }
            else if (down & XINPUT_GAMEPAD_A) result = s_confirmOpen == CONFIRM_DASHBOARD ? RG_DASHBOARD : RG_BROWSER;
            else if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) s_confirmOpen = 0;
        }
        else if (s_hiresPreviewOpen)
        {
            if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X)) s_hiresPreviewOpen = 0;
        }
        else if (s_texOpen)
        {
            int r = TexturesInput(down, c);
            if (r == 1)
            {
                s_texOpen = 0;
                BuildGameMenu();   // its row: the pack in use now
                s_menu.sel = PauseRow(M_TEXTURES);
            }
            else if (r == 2)
            {
                // Another pack (or off): loaded now, and kept for the next time.
                ApplyGraphics(sys, renderer, c);
                HiresOpen(sys, renderer, c);
                HiresRemember(c);
                hiTexApplied = c->hiTex;
                strncpy(hiTexFileApplied, c->hiTexFile, sizeof(hiTexFileApplied) - 1);
                hiTexFileApplied[sizeof(hiTexFileApplied) - 1] = 0;
                s_texActive = HiresActiveIndex(c);
            }
            else if (r == 3)
            {
                // Y: a texture of the selected pack (closed by B, A or X; back on this page).
                Config look = *c;
                look.hiTex = 1;
                strncpy(look.hiTexFile, HiresPackName(s_texSel - 1), sizeof(look.hiTexFile) - 1);
                look.hiTexFile[sizeof(look.hiTexFile) - 1] = 0;
                HiresPreviewOpen(&look);
                s_hiresPreviewOpen = 1;
            }
        }
        else if (s_cheatsOpen)
        {
            if (CheatsInput(down))
            {
                s_cheatsOpen = 0;
                BuildGameMenu();   // its row: how many are on now
                s_menu.sel = PauseRow(M_CHEATS);
            }
        }
        else if (s_playersOpen)
        {
            if (PlayersInput(down))
            {
                s_playersOpen = 0;
                OnlineWatchUsers(0);
                s_menu.sel = PauseRow(M_PLAYERS);
            }
        }
        else if (s_aboutOpen)
        {
            if (AboutInput(down)) s_aboutOpen = 0;
        }
        else if (settings)
        {
            int r = SettingsInput(&s_menu, c, s_profilePath[0] != 0, down);
            if (r == SET_PREVIEW) { HiresPreviewOpen(c); s_hiresPreviewOpen = 1; }
            else if (r == SET_ABOUT) s_aboutOpen = 1;   // Settings > About, over the settings
            else if (r == SET_CHANGED)
            {
                xb_audio_set_target_ms(c->audioMs);
                ApplyGraphics(sys, renderer, c);
                if (c->hiTex != hiTexApplied || strcmp(c->hiTexFile, hiTexFileApplied) != 0)
                {
                    HiresOpen(sys, renderer, c);   // picked a different pack (or toggled hiTex): reload it now, not at the next launch
                    HiresRemember(c);              // ... and kept for the next one
                    hiTexApplied = c->hiTex;
                    strncpy(hiTexFileApplied, c->hiTexFile, sizeof(hiTexFileApplied) - 1);
                    hiTexFileApplied[sizeof(hiTexFileApplied) - 1] = 0;
                }
                s_showFps = c->perfItems;
                s_perfF = c->perfFahrenheit;
                PerfStyleApply(c);
                s_rumbleOn = c->rumble;
            }
            else if (r == SET_SAVE_ALL || r == SET_SAVE_GAME) s_confirmOpen = r == SET_SAVE_ALL ? CONFIRM_SAVE_ALL : CONFIRM_SAVE_GAME;   // asked first (above)
            if (r == SET_BACK)
            {
                settings = 0;
                BuildGameMenu();
                s_menu.sel = PauseRow(M_SETTINGS);
            }
        }
        else if (states)
        {
            UiMenuNavigate(&s_menu, down);
            sel = s_menu.sel;
            if (down & XINPUT_GAMEPAD_B)
            {
                states = 0;
                BuildGameMenu();
                s_menu.sel = PauseRow(M_STATES);
            }
            else if (sel == ST_SLOT && (down & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_A)))
            {
                s_stateSlot += (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 1;
                if (s_stateSlot < 1) s_stateSlot = CONFIG_STATE_SLOTS;
                if (s_stateSlot > CONFIG_STATE_SLOTS) s_stateSlot = 1;
                SlotValue(s_menu.value[ST_SLOT], sizeof(s_menu.value[ST_SLOT]));
            }
            else if (down & XINPUT_GAMEPAD_A)
            {
                switch (sel)
                {
                case ST_LOAD: LoadStateNow(sys); result = RG_CONTINUE; break;
                case ST_SAVE:
                    // The reserved slot: said here, the menu stays (it can still be loaded from).
                    if (s_stateSlot == s_reservedSlot) OsdT(T_SLOT_RESERVED, s_stateSlot);
                    else { s_statePending = 1; result = RG_CONTINUE; }   // at the next quiet point
                    break;
                }
            }
        }
        else
        {
            UiMenuNavigate(&s_menu, down);
            sel = s_menu.sel;
            if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) result = RG_CONTINUE;
            else if (down & XINPUT_GAMEPAD_A)
            {
                switch (sel >= 0 && sel < s_pauseCount ? s_pauseRows[sel] : -1)
                {
                case M_RESUME: result = RG_CONTINUE; break;
                case M_STATES: states = 1; BuildStatesMenu(); break;
                case M_CHEATS: s_cheatsOpen = 1; s_cheatSel = s_cheatTop = 0; break;
                case M_TEXTURES: TexturesOpen(c); break;
                case M_SETTINGS: settings = 1; SettingsBuild(&s_menu, c, s_profilePath[0] != 0); break;
                case M_PLAYERS: s_playersOpen = 1; s_playersTop = 0; OnlineWatchUsers(1); break;
                case M_RESET: s_confirmOpen = CONFIRM_RESET; break;      // asked first (above)
                case M_ROOM:
                    if (s_netMode == -1) UiRequestOnline();   // cut off: Online play again (the room was left)
                    else s_toRoom = 1;                        // the ROM list opens the room's lobby
                    result = RG_BROWSER;
                    break;
                case M_ROMS: s_confirmOpen = CONFIRM_LIST; break;        // asked first (above)
                case M_DASHBOARD: s_confirmOpen = CONFIRM_DASHBOARD; break;
                }
            }
        }
        s_statesOpen = states;   // the overlay shows the slot's picture under the rows
        s_pauseMain = !states && !settings && !s_aboutOpen && !s_hiresPreviewOpen && !s_cheatsOpen && !s_texOpen && !s_playersOpen;   // ... or the game's, on the main page
        h64_xenos_present(renderer);   // the frozen frame, the menu (overlay) on top
        Sleep(16);
    }
    if (s_cheatsOpen) h64_cheats_save_state(&s_cheats, s_cheatState);
    s_cheatsOpen = 0;
    s_texOpen = 0;
    if (s_playersOpen) OnlineWatchUsers(0);
    s_playersOpen = 0;
    s_menuCfg = NULL;
    s_statesOpen = 0;
    s_pauseMain = 0;
    s_menuOpen = 0;
    s_aboutOpen = 0;
    s_hiresPreviewOpen = 0;
    s_confirmOpen = 0;
    return result;
}


// Real time in ms from QueryPerformanceCounter. While the emulator runs, the
// console's system clock and GetTickCount fall behind real time (measured
// 2026-10-08 against the PC through XBDM: the system clock counted 96.2 s in
// 100 s, GetTickCount ~1.4 % less again); QPC stays within 0.3 %.
static double QpcMs(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// ---- V-Sync: the TV's refreshes (RunGame's adaptive V-Sync pacing) ----
static volatile LONG s_vblanks;         // counted by the GPU's vertical blank interrupt (VBlankCallback)
static volatile LONGLONG s_vblankAt;    // the QPC of the last one
static LONGLONG s_vblankTicks;          // a refresh's length in QPC ticks (measured once; 0: not yet)
static LONG s_paceVblank;               // the refresh the last frame started at (with a frame in reserve: the one it would have)

// A frame in reserve: its picture, ready early, held on the graphics worker until the refresh before its own
// (V-Sync then shows it at its own: a frame that took longer still makes it). 50 ms at most.
static void PresentHold(LONG slot)
{
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    while (s_vblanks - (slot - 1) < 0)
    {
        LONGLONG left;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - t0.QuadPart > freq.QuadPart / 20) break;
        left = s_vblankAt + s_vblankTicks - now.QuadPart;
        if (left > freq.QuadPart / 500) Sleep(1);
    }
}
static LONG s_rateVblank0 = -1;         // the refresh (and its QPC) the measure started from
static LONGLONG s_rateAt0;

static void WINAPI VBlankCallback(D3DVBLANKDATA *)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    s_vblankAt = t.QuadPart;
    InterlockedIncrement(&s_vblanks);
}

// Frames paced on the TV's refreshes: V-Sync on, the TV's rate measured (its first second of
// refreshes), and the game's rate within 1.5 % of it - a 50 Hz game on a 60 Hz TV stays on the clock.
static int VBlankPacing(const Config *c, const H64System *sys, const LARGE_INTEGER *freq)
{
    LONG n = s_vblanks;
    double tv, game;
    if (!c->vsync || c->xenia || !sys->vi.frameCycles || s_netMode > 0) return 0;   // online: every console on the clock
    if (!s_vblankTicks)
    {
        if (s_rateVblank0 < 0 || n - s_rateVblank0 < 0) { s_rateVblank0 = n; s_rateAt0 = s_vblankAt; return 0; }
        if (n - s_rateVblank0 < 60) return 0;   // no refreshes counted (no interrupt): the clock, always
        s_vblankTicks = (s_vblankAt - s_rateAt0) / (n - s_rateVblank0);
        H64_INFO("[main] TV refresh %.3f Hz", (double)freq->QuadPart / (double)s_vblankTicks);
        if (s_vblankTicks <= 0) return 0;
    }
    tv = (double)freq->QuadPart / (double)s_vblankTicks;
    game = 93750000.0 / (double)sys->vi.frameCycles;
    return fabs(game - tv) < tv * 0.015;
}

// The menu with a crash guard: an exception there is logged (and the log
// flushed) instead of DashLaunch's crash notice and a silent return to Aurora.
static int GameMenuSafe(H64System *sys, H64Renderer *renderer, Config *c)
{
    int r = RG_DASHBOARD;
    H64_INFO("[menu] opened at VI %u", sys->vi.frames);
    LogFlush();
    __try
    {
        RumbleUpdate(sys, 1);   // no vibration over the menu
        r = GameMenu(sys, renderer, c);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s_menuOpen = 0;
        s_confirmOpen = 0;
        H64_ERROR("[crash] exception %08X in the in-game menu", GetExceptionCode());
        r = RG_BROWSER;
    }
    H64_INFO("[menu] closed: %s", r == RG_CONTINUE ? "resume" : r == RG_BROWSER ? "ROM list" : "dashboard");
    LogFlush();
    return r;
}

// The user's screenshot combo (Config::shotBtn1/2): both held, at least one
// just pressed this frame. Saved next to the ROMs, in <root>:\screenshots\
// (made the first time, like xb_art.cpp's art\ folder).
static void TakeManualScreenshot(H64System *sys, H64Renderer *renderer)
{
    char dir[128], path[160];
    if (s_shotsRoot[0]) strcpy(dir, s_shotsRoot);   // Settings > Paths > Screenshots
    else _snprintf(dir, sizeof(dir) - 1, "%s:\\screenshots", s_logDrive);
    dir[sizeof(dir) - 1] = 0;
    CreateDirectoryA(dir, NULL);
    _snprintf(path, sizeof(path) - 1, "%s\\shot_%08u.bmp", dir, GetTickCount());
    path[sizeof(path) - 1] = 0;
    h64_hle_async_wait(sys);
    WorkWaitAll(NULL);
    {
        int failed = h64_xenos_save_frame(renderer, path);
        H64_INFO("[main] screenshot (pad combo) %s: %s", path, failed ? "failed" : "saved");
        OsdT(failed ? T_SCREENSHOT_FAILED : T_SCREENSHOT_SAVED, 0);
    }
}

static int RunGameBody(IDirect3DDevice9 *dev, Config *c, const char *romPath);

// The game with its exceptions logged first (CrashFilter), then left to end the title as before: DashLaunch takes
// the unhandled ones before the process-wide filter, and a crash outside RunFrame (the present, the per-second
// stats, the saves) went back to Aurora with nothing in the log.
static int RunGame(IDirect3DDevice9 *dev, Config *c, const char *romPath)
{
    __try { return RunGameBody(dev, c, romPath); }
    __except (CrashFilter(GetExceptionInformation())) { return RG_DASHBOARD; }
}

static int RunGameBody(IDirect3DDevice9 *dev, Config *c, const char *romPath)
{
    int result = RG_DASHBOARD, menuRequest = 0;
    u32 fpsFrames = 0;
    double fpsStart = QpcMs();
    LONGLONG busyTicks = 0;   // time running and presenting since fpsStart (the details' CPU load)
    u64 gfxProfAt = 0;   // snapshot at fpsStart, for the GPU load detail (Settings > Show FPS) - set once sys exists
    char rom[256];
    u8 *data;
    u32 size = 0;
    int jit = !strcmp(c->cpu, "dynarec");
    H64System *sys;
    H64Renderer *renderer = NULL;
    LARGE_INTEGER freq, now, frameStart;
    double perfStart = QpcMs();
    u32 framesSincePerf = 0, presented = 0;
    LARGE_INTEGER t0, t1, t2;
    LONGLONG profRun = 0, profPresent = 0, profWait = 0;
    u64 jitBlocks = 0, jitInval = 0, jitFlush = 0, jitHelper = 0, jitRun = 0, jitIdle = 0, jitSteps = 0, insnsPrev = 0;
    u64 instrAtPerf = 0;
    // Settings > System > Anonymous statistics: this session's numbers (xb_stats.cpp), sent on leaving.
    static StatsSession stats;
    int statsOn = s_settingsPath[0] && c->statsShare > 0 && !c->xenia, statsSkip = 1;
    DWORD statsSaveAt = GetTickCount() + 60000;

    s_crashed = 0;
    s_showFps = c->perfItems;
    s_perfF = c->perfFahrenheit;
    s_perfTemps = !c->xenia;
    strncpy(s_savesRoot, ConfigPath(c, CONFIG_PATH_SAVES, s_logDrive), sizeof(s_savesRoot) - 1);
    strncpy(s_shotsRoot, ConfigPath(c, CONFIG_PATH_SHOTS, s_logDrive), sizeof(s_shotsRoot) - 1);
    HiresSetFolder(ConfigPath(c, CONFIG_PATH_TEXTURES, s_logDrive));   // the texture packs (also for a game started without the ROM list)
    PerfStyleApply(c);
    s_fpsText[0] = 0;
    if (romPath)
    {
        strncpy(rom, romPath, sizeof(rom) - 1);
        rom[sizeof(rom) - 1] = 0;
    }
    else if (!FindRom(c, rom, sizeof(rom)))
    {
        MessageScreen(dev, Tr(T_NO_ROM), Tr(T_PUT_ROM), D3DCOLOR_XRGB(240, 200, 60));
        return RG_DASHBOARD;
    }
    data = RomFileLoad(rom, &size);
    if (!data) { MessageScreen(dev, Tr(T_CANNOT_READ), rom, D3DCOLOR_XRGB(240, 60, 60)); return RG_BROWSER; }
    strncpy(s_romNow, rom, sizeof(s_romNow) - 1);   // the pause menu's "Now playing" (read when it first opens)
    s_romNow[sizeof(s_romNow) - 1] = 0;
    s_npTitle[0] = 0;
    sys = MakeSystem(data, size, jit);
    free(data);
    if (!sys) { OnlineRoomClose(); MessageScreen(dev, Tr(T_NOT_N64), rom, D3DCOLOR_XRGB(240, 60, 60)); return RG_BROWSER; }
    gfxProfAt = sys->prof[H64_PROF_GFX_HLE];
    // Online play's profile: the game being played, and at the end the minutes it was.
    char onlineGame[24], onlineCode[24];
    const DWORD onlineStart = GetTickCount();
    s_playStart = onlineStart;   // (the pause menu: the time played this session)
    strncpy(onlineGame, sys->rom.name, sizeof(onlineGame) - 1);
    onlineGame[sizeof(onlineGame) - 1] = 0;
    _snprintf(onlineCode, sizeof(onlineCode) - 1, "%s / %08X", sys->rom.gameCode, sys->rom.crc1);
    onlineCode[sizeof(onlineCode) - 1] = 0;
    OnlineSetStatus(onlineGame);
    if (statsOn)
    {
        u32 rw, rh;
        int wide;
        StatsSessionStart(&stats);
        StatsConsoleId(s_logDrive, stats.console, sizeof(stats.console));
        strncpy(stats.game, sys->rom.name, sizeof(stats.game) - 1);
        _snprintf(stats.code, sizeof(stats.code) - 1, "%s / %08X", sys->rom.gameCode, sys->rom.crc1);
        ConfigResCurrent(c, &rw, &rh, &wide);
        _snprintf(stats.settings, sizeof(stats.settings) - 1, "%ux%u vsync%d hd%d%s%s", rw, rh, c->vsync ? 1 : 0, c->hiTex ? 1 : 0,
                  h64_rom_tv_type(&sys->rom) == 0 ? " 50hz" : "", strcmp(c->cpu, "interp") ? "" : " interp");
    }
    sys->options.hleGfx = c->hle;
    sys->options.gfxTiming = c->gfxTiming;
    sys->options.strictCpu = c->strict;
    // Per-game settings (core/system/h64_profile.cpp); the ini may still force late parse.
    h64_apply_game_profile(sys);
    if (c->gameFlags >= 0)
    {
        h64_apply_profile_flags(sys, (u32)c->gameFlags);
        H64_INFO("[profile] gameflags=%X from the ini", c->gameFlags);
    }
    if (c->lateParse >= 0) sys->options.gfxLateParse = c->lateParse;
    if (sys->options.gfxLateParse) H64_INFO("[main] late display list parse on");
    sys->options.hleAudio = c->hle;
    sys->options.hleAudioCheck = c->audioCheck;   // audiocheck=1: the LLE plays, the HLE is compared with it
    if (c->audioCheck) H64_INFO("[main] audio check: LLE RSP audio, HLE compared");
    sys->padHook = PadHook;
    NetConfig(sys);
    if (s_netMode > 0) { s_netHiTex = c->hiTex; c->hiTex = 0; }   // online: no HD pack (both consoles must draw the same)
    s_padMaps = c->padMap;
    // Rumble Paks in the controllers of the games that support one (on port 1
    // together with its Controller Pak when the game uses that, h64_pif.cpp;
    // the setting only mutes the motors, so it can change during the game).
    s_rumbleOn = c->rumble;
    sys->rumblePaks = h64_rom_rumble(&sys->rom) ? 0xFu : 0;
    H64_INFO("[main] rumble pak: %s%s", sys->rumblePaks ? (sys->save->pak ? "every port (with the Controller Pak on port 1)" : "every port") : "not supported by the game",
             sys->rumblePaks && !c->rumble ? ", motors off (setting)" : "");
    sys->profClock = ProfClock;
    s_script = c->input.count ? &c->input : NULL;
    xb_audio_set_target_ms(c->audioMs);
    xb_audio_ui_enable(c->uiSounds);   // the pause menu's button sounds (also for a game started without the ROM list)
    xb_audio_ui_style(c->uiSoundStyle);
    UiSetFont(c->uiFont);   // the menus' letters, also for a game started without the ROM list
    H64_INFO("[main] audio queue target %d ms", xb_audio_target_ms());
    if (xb_audio_init() == 0)
    {
        sys->aiSink = xb_audio_sink;
        sys->aiUser = NULL;
    }
    renderer = h64_xenos_create(sys, dev);
    if (!renderer) { MessageScreen(dev, Tr(T_RENDERER_FAILED), Tr(T_SEE_LOG), D3DCOLOR_XRGB(240, 60, 60)); FreeSystem(sys); return RG_DASHBOARD; }
    if (c->xenosDebug) h64_xenos_set_debug(renderer, c->xenosDebug);
    ApplyGraphics(sys, renderer, c);
    if (c->softRenderer) sys->options.rdpStateOnly = 0;
    else sys->renderer = renderer;
    H64_INFO("[main] running %s: cpu %s, RSP %s, %s renderer", rom, jit ? "recompiler" : "interpreter",
             c->hle ? "HLE" : "LLE", c->softRenderer ? "software" : "Xenos");
    if (c->hle && !c->trace)
        // Online: the graphics HLE on the CPU thread too. On the workers, Smash Remix 2.0.1 went out of step at frame
        // 420-780 on every game between two consoles; with both on the CPU thread, 8 minutes without one (59.5 VI/s).
        WorkersStart(sys, dev, renderer, c->asyncGfx && !c->softRenderer && s_netMode <= 0, c->gfxCycles ? c->gfxCycles : 400000,
                     // Online: the audio HLE on the CPU thread. On its worker it reads the game's RDRAM while
                     // the CPU goes on writing it, and the samples mixed came out a little different on the
                     // two consoles (state out of step, RDRAM 06E000-07C000 in Smash Remix).
                     c->asyncAudio && s_netMode <= 0, c->audioCycles ? c->audioCycles : 100000, c->parseWorker);

    if (c->trace)
    {
        u32 n;
        if (c->traceStep >= 1000000000u)
        {
            // Instruction trace (h64test --trace-step 1000000000+START --trace-frames N).
            h64_system_run_cycles(sys, c->traceStep - 1000000000u);
            for (n = 0; n < c->trace; n++)
            {
                u32 ch, pc = (u32)sys->cpu.pc;
                h64_system_step(sys);
                h64_system_state_hash(sys, &ch, NULL);
                H64_INFO("[itrace] %u pc=%08X cyc=%llu cpu=%08X", n, pc, (unsigned long long)sys->cpu.cycles, ch);
            }
        }
        else
        {
            u64 step = c->traceStep ? c->traceStep : sys->vi.frameCycles;
            for (n = 0; n < c->trace; n++)
            {
                u32 ch, rh;
                h64_system_run_cycles(sys, step);
                h64_system_state_hash(sys, &ch, &rh);
                H64_INFO("[trace] %u cycles=%llu instr=%llu pc=%08X cpu=%08X ram=%08X", n,
                         (unsigned long long)sys->cpu.cycles, (unsigned long long)sys->cpu.instructions, (u32)sys->cpu.pc,
                         ch, rh);
            }
        }
        H64_INFO("[main] exitafter reached (trace done)");
        sys->renderer = NULL;
        h64_xenos_free(renderer);
        xb_audio_game_stop();
        s_samplePc = NULL;
        Sleep(5);
        FreeSystem(sys);
        return RG_DASHBOARD;
    }
    SavesOpen(sys);
    HiresOpen(sys, renderer, c);
    NetConnect(sys, c);
    SettingsSetOnline(s_netMode > 0);   // the pause menu's settings without V-Sync
    CheatsOpen(sys);
    s_stateSlot = c->stateSlot >= 1 && c->stateSlot <= 9 ? c->stateSlot : 1;
    s_statePending = 0;
    s_reservedSlot = c->padLoss == PADLOSS_SAVE ? c->autoSaveSlot : 0;
    s_osd[0] = 0;
    h64_xenos_set_overlay(renderer, OverlayHook, NULL);
    if (c->loadState) LoadStateNow(sys);
    s_scriptBase = sys->vi.frames;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&frameStart);
    if (!c->xenia)
    {
        BenchCodeMemory();
        // The benchmark wrote over the code memory, where the recompiler keeps its
        // shared entry/exit code: rebuild it.
        if (sys->jit) h64_jit_reset(sys);
    }
    PcSamplerStart(sys);
    DWORD backSince = 0;
    int backUsed = 0;
    double backHeldMs = 0, backPolledAt = 0;   // time BACK was seen held, poll after poll
    WORD prevButtons = 0;
    u32 hkPrev = 0xFFFFFFFFu;   // the shortcuts' buttons a frame ago (all: nothing fires until released)
    // Settings > System > a controller unplugged: which ports were plugged at
    // the last poll, and 1 while the autosave waits for a quiet point.
    int padWas[4] = { 0, 0, 0, 0 }, padPolled = 0, padLossSaving = 0;
    // Game images > Download while playing: the downloads of every game go on in the
    // background; a note says so when the game starts, and another when they end.
    int artBulk = -1;
    DWORD artAt = GetTickCount() + 1500;   // after the game's first frames
    // An HD pack downloading in the background (its bar: HiresBgDraw): when it ends, a note,
    // and when it is this game's it is loaded there and then (as R3 does).
    int hpackWas = HiresPackBusy();
    DWORD hpackAt = GetTickCount() + 1000;
    for (;;)
    {
        if ((hpackWas || HiresQueueStatus(NULL, NULL, NULL, NULL)) && GetTickCount() >= hpackAt)
        {
            hpackAt = GetTickCount() + 1000;
            if (hpackWas && !HiresPackBusy())
            {
                unsigned kb[2];
                int err = HPERR_NONE, st = HiresPackStatus(&kb[0], &kb[1], &err, NULL, 0);
                const HiresPackEntry *e = HiresPackJob();
                char folder[64];
                hpackWas = 0;
                HiresFolderName(sys->rom.name, folder, sizeof(folder));
                if (st == HPACK_DONE && e && !strcmp(e->romName, folder) && c->hiTex)
                {
                    h64_hle_async_wait(sys);
                    WorkWaitAll(NULL);   // the renderer is this thread's for the change
                    HiresOpen(sys, renderer, c);
                    OsdT(T_HIRES_BG_APPLIED, 0);
                }
                else if (st == HPACK_DONE) OsdT(T_HIRES_BG_DONE, 0);
                else if (st == HPACK_FAILED && err != HPERR_CANCELLED) OsdT(T_HIRES_BG_FAILED, 0);
            }
            if (HiresQueueTick()) hpackWas = 1;   // "Download all": the next pack (slower while playing)
        }
        if (c->artBackground && c->artDownload && GetTickCount() >= artAt)
        {
            ArtStatus as;
            ArtGetStatus(&as);
            if (artBulk < 0 && as.bulk) OsdT(T_ART_BG_RUNNING, as.total - as.done);
            else if (artBulk > 0 && !as.bulk) OsdT(T_ART_BG_DONE, as.total);
            artBulk = as.bulk;
            artAt = GetTickCount() + 1000;
        }
        int queued;
        NetKeepAlive();
        {
            // Online play's news (a friend came online, a friendship asked, a message, an invitation to play - longer,
            // so it can be read without stopping): a toast over the game.
            char notice[200];
            int invite = 0;
            if (OnlineNoticeTake(notice, sizeof(notice), &invite)) NetOsd(notice, invite ? 12000 : 9000);
        }
        if (XInputGetState(0, &s_pad) == ERROR_SUCCESS) s_padValid = 1;
        else s_padValid = 0;
        {
            int k;
            for (k = 0; k < 3; k++) s_padsMoreValid[k] = XInputGetState(k + 1, &s_padsMore[k]) == ERROR_SUCCESS;
            // The plugged ports, for the games' controller probes (osContInit).
            sys->padMask = 1;
            if (!s_script)
                for (k = 0; k < 3; k++) if (s_padsMoreValid[k]) sys->padMask |= 2u << k;
            // Online: every port of the game, whatever is plugged here. It said ports 1 and 2 (the first online games
            // had two players): the controller probes saw ports 3 and 4 empty, and their Rumble Paks never started.
            if (s_netMode != 0) sys->padMask = (1u << s_netPlayers) - 1u;
        }
        s_reservedSlot = c->padLoss == PADLOSS_SAVE ? c->autoSaveSlot : 0;   // the menu may have changed it
        {
            // Settings > System > low battery: a toast, like the state messages.
            char low[160];
            if (BatteryCheck(c, low, sizeof(low)))
            {
                strcpy(s_osd, low);
                s_osdUntil = GetTickCount() + OSD_MS;
                s_osdLen = OSD_MS;
                H64_INFO("[main] %s", low);
            }
        }
        {
            // A controller that was plugged and is not any more: the game
            // pauses (the in-game menu, below), after a save in the reserved
            // slot (at the next quiet point) with PADLOSS_SAVE.
            int now[4], k;
            now[0] = s_padValid;
            for (k = 0; k < 3; k++) now[k + 1] = s_padsMoreValid[k];
            for (k = 0; k < 4; k++)
            {
                if (padPolled && padWas[k] && !now[k] && c->padLoss && !padLossSaving && !s_script && s_netMode <= 0)
                {
                    if (c->padLoss == PADLOSS_SAVE)
                    {
                        H64_INFO("[pad] controller %d unplugged: saving in slot %d, then pause", k + 1, c->autoSaveSlot);
                        padLossSaving = 1;
                    }
                    else
                    {
                        H64_INFO("[pad] controller %d unplugged: pause", k + 1);
                        OsdT(T_PAD_UNPLUGGED, k + 1);
                        menuRequest = 1;
                    }
                }
                padWas[k] = now[k];
            }
            padPolled = 1;
        }
        if (padLossSaving && SaveStateNow(sys, c->autoSaveSlot, 1))
        {
            padLossSaving = 0;
            LogFlush();
            menuRequest = 1;   // pause: the in-game menu, below
        }
        RumbleUpdate(sys, 0);
        // BACK held for 3 s goes back to the ROM list (the dashboard is left
        // only from the in-game menu or the ROM list); with BACK held, RB/LB
        // save/load a state and the D-pad changes the slot. The hold time adds
        // up the gaps between polls that saw BACK down, each counted 100 ms at
        // most: a slow first frame (a 6105 game's ~4 s boot) between two
        // presses no longer passes for a long hold (it sent the user back to
        // Aurora).
        if (s_padValid && (s_pad.Gamepad.wButtons & XINPUT_GAMEPAD_BACK))
        {
            double nowMs = QpcMs();
            if (!backSince)
            {
                backSince = GetTickCount() | 1;
                backUsed = 0;
                backHeldMs = 0;
                H64_INFO("[pad] BACK down at VI %u", presented);
                LogFlush();
            }
            else
                backHeldMs += nowMs - backPolledAt > 100.0 ? 100.0 : nowMs - backPolledAt;
            backPolledAt = nowMs;
            if (s_netMode > 0 && s_netWs)
            {
                // Online: BACK held a moment is the menu (a short press, the quick chat).
                if (!backUsed && backHeldMs >= NET_BACK_MENU_MS) { backUsed = 1; s_qcOpen = 0; menuRequest = 1; }
            }
            else s_backHeldMs = backUsed ? 0.0f : (float)backHeldMs;   // the countdown (BackHoldDraw) while it is held
            if (!backUsed && backHeldMs >= BACK_LEAVE_AT && !(s_netMode > 0 && s_netWs))
            {
                H64_INFO("[pad] BACK held to the end of the countdown: back to the ROM list");
                s_backHeldMs = 0;
                result = RG_BROWSER;
                break;
            }
        }
        else
        {
            if (backSince)
            {
                H64_INFO("[pad] BACK up after %u ms%s", GetTickCount() - backSince, backUsed ? " (used with RB/LB/D-pad)" : "");
                LogFlush();
            }
            s_backHeldMs = 0;   // let go: no countdown
            if (backSince && !backUsed && backHeldMs < 600.0)   // a short press
            {
                if (s_netMode > 0 && s_netWs)
                {
                    // One press: the quick chat, or closed; a second press within 400 ms: straight to the keyboard.
                    static DWORD tapAt;
                    const DWORD now = GetTickCount();
                    if (s_qcOpen && !s_qcKbd && tapAt && now - tapAt < 400) NetQuickChatKeyboard();
                    else { s_qcOpen = !s_qcOpen; s_qcKbd = 0; s_qcRow = s_qcCol = 0; s_qcPrev = 0xFFFF; }
                    tapAt = now;
                }
                else menuRequest = 1;
            }
            backSince = 0;
        }
        prevButtons = s_padValid ? s_pad.Gamepad.wButtons : 0;
        if (s_qcOpen) NetQuickChatInput();
        {
            // The shortcuts (Settings > Controls > Button shortcuts, Config::hotkey): one
            // fires when the last of its buttons goes down. With BACK in it, that BACK
            // press is not the pause menu's (backUsed).
            u32 ext = 0;
            int k;
            if (s_padValid)
            {
                ext = s_pad.Gamepad.wButtons;
                if (s_pad.Gamepad.bLeftTrigger > 128) ext |= HK_LT;
                if (s_pad.Gamepad.bRightTrigger > 128) ext |= HK_RT;
            }
            // Online: no shortcut at all, without a word (states and screenshots are off; a shortcut on a plain
            // button, RB for the screenshot, showed "not available online" at each press of it in the game).
            for (k = 0; k < HK_COUNT && s_netMode == 0; k++)
            {
                u32 m = c->hotkey[k];
                if (!m || (ext & m) != m || (hkPrev & m) == m) continue;
                if (m & XINPUT_GAMEPAD_BACK) backUsed = 1;
                switch (k)
                {
                case HK_SAVE: if (!NetRefuse()) s_statePending = 1; break;
                case HK_LOAD: LoadStateNow(sys); break;
                case HK_SLOT_PREV:
                case HK_SLOT_NEXT:
                    s_stateSlot += k == HK_SLOT_NEXT ? 1 : -1;
                    if (s_stateSlot < 1) s_stateSlot = CONFIG_STATE_SLOTS;
                    if (s_stateSlot > CONFIG_STATE_SLOTS) s_stateSlot = 1;
                    OsdT(T_SLOT, s_stateSlot);
                    break;
                case HK_SHOT: if (!NetRefuse()) TakeManualScreenshot(sys, renderer); break;
                }
            }
            hkPrev = ext;
        }
        if (c->texR3 && s_netMode <= 0)
        {
            // Button shortcuts > HD textures: two clicks of the right stick (R3, without L3: not the
            // screenshot combo) within 400 ms turn the HD textures on or off, to compare.
            static DWORD r3At;
            static int r3Was;
            WORD b = s_padValid ? s_pad.Gamepad.wButtons : 0;
            int r3 = (b & XINPUT_GAMEPAD_RIGHT_THUMB) && !(b & XINPUT_GAMEPAD_LEFT_THUMB);
            if (r3 && !r3Was)
            {
                DWORD now = GetTickCount();
                if (r3At && now - r3At <= 400)
                {
                    r3At = 0;
                    h64_hle_async_wait(sys);
                    WorkWaitAll(NULL);   // the renderer is this thread's for the change
                    c->hiTex = !c->hiTex;
                    ApplyGraphics(sys, renderer, c);
                    HiresOpen(sys, renderer, c);
                    HiresRemember(c);
                    OsdT(c->hiTex ? T_TEX_R3_ON : T_TEX_R3_OFF, 0);
                }
                else
                    r3At = now;
            }
            r3Was = r3;
        }
        if (c->saveStateAt && presented == c->saveStateAt) s_statePending = 1;
        if (c->menuAt && presented == c->menuAt) menuRequest = 1;
        if (c->aboutAt && presented == c->aboutAt) { menuRequest = 1; s_aboutOpen = 1; }   // remote check of the About panel
        if (c->browserAt && presented == c->browserAt)
        {
            H64_INFO("[main] browserat reached: back to the ROM list");
            result = RG_BROWSER;
            break;
        }
        if (s_statePending && SaveStateNow(sys, s_stateSlot, 0)) s_statePending = 0;
        if (menuRequest)
        {
            menuRequest = 0;
            NetStatus("menu");   // online: the others are told why the game waits
            result = GameMenuSafe(sys, renderer, c);
            if (result != RG_CONTINUE) break;   // (leaving: the others hear it left, not "back to the game")
            NetStatus("play");
            result = RG_DASHBOARD;
            statsSkip = 1;   // the second with the pause in it is not the game's speed
            QueryPerformanceCounter(&frameStart);   // no catching up for the paused time
            prevButtons = 0;
            hkPrev = 0xFFFFFFFFu;   // buttons still held from the menu fire nothing
            padPolled = 0;   // the controllers plugged now are the new reference (one still unplugged does not save again)
            continue;
        }
        u64 profBefore[H64_PROF_COUNT];
        u64 instBefore = sys->cpu.instructions;
        memcpy(profBefore, sys->prof, sizeof(profBefore));
        QueryPerformanceCounter(&t0);
        if (RunFrame(sys)) { result = RG_BROWSER; break; }
        QueryPerformanceCounter(&t1);
        {
            // A frame whose emulation took over 100 ms: where the time went
            // (sys->prof deltas, in ms; the N64 pc), to find periodic stalls.
            double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
            if (ms > 100.0)
            {
                double k = 1000.0 / (double)freq.QuadPart;   // profClock is the QPC
                H64_INFO("[stall] frame %u: run %.1f ms | cpu code %.1f helper %.1f events %.1f jit %.1f gfx %.1f audio %.1f rsp lle %.1f wait gfx %.1f | %llu instructions, pc %08X",
                         presented, ms, (sys->prof[H64_PROF_BLOCKS] - profBefore[H64_PROF_BLOCKS]) * k,
                         (sys->prof[H64_PROF_HELPER] - profBefore[H64_PROF_HELPER]) * k, (sys->prof[H64_PROF_EVENTS] - profBefore[H64_PROF_EVENTS]) * k,
                         (sys->prof[H64_PROF_JIT_COMPILE] - profBefore[H64_PROF_JIT_COMPILE]) * k, (sys->prof[H64_PROF_GFX_HLE] - profBefore[H64_PROF_GFX_HLE]) * k,
                         (sys->prof[H64_PROF_AUDIO_HLE] - profBefore[H64_PROF_AUDIO_HLE]) * k, (sys->prof[H64_PROF_RSP_LLE] - profBefore[H64_PROF_RSP_LLE]) * k,
                         (sys->prof[H64_PROF_ASYNC_WAIT] - profBefore[H64_PROF_ASYNC_WAIT]) * k,
                         (unsigned long long)(sys->cpu.instructions - instBefore), (u32)sys->cpu.pc);
            }
        }
        PresentFrame(sys, renderer);
        QueryPerformanceCounter(&t2);
        profRun += t1.QuadPart - t0.QuadPart;
        profPresent += t2.QuadPart - t1.QuadPart;
        busyTicks += t2.QuadPart - t0.QuadPart;
        {
            double r = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart, pr = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / freq.QuadPart;
            if (r > s_maxRun) s_maxRun = r;
            if (pr > s_maxPresent) s_maxPresent = pr;
        }
        framesSincePerf++;
        presented++;
        fpsFrames++;
        if (QpcMs() - fpsStart >= 1000.0)
        {
            double nowMs = QpcMs();
            if (s_showFps)
            {
                // The lines chosen (Settings > System > On-screen performance), in this order:
                // frames per second; the emulation thread's load (time spent running the N64
                // and presenting, not waiting for the next frame); memory in use; the internal
                // resolution and the N64's picture; the graphics worker's load; the console's
                // temperatures.
                MEMORYSTATUS ms;
                u32 rw = 0, rh = 0, vw = sys->vi.regs[2] & 0xFFF, vs = sys->vi.regs[10], ys = sys->vi.regs[13] & 0xFFF;   // width, V_START, Y_SCALE
                u32 vh = (((vs & 0x3FF) - ((vs >> 16) & 0x3FF)) / 2) * ys / 1024;
                double load = busyTicks * 100000.0 / (double)freq.QuadPart / (nowMs - fpsStart);
                s_fpsBuild[0] = 0;
                (void)vw; (void)vh;
                if (s_showFps & CONFIG_PERF_FPS) PerfAdd("\x01" "%.1f FPS", fpsFrames * 1000.0 / (nowMs - fpsStart));
                if (s_showFps & CONFIG_PERF_CPU) PerfAdd("\x02" "CPU %d%%", (int)(load > 100.0 ? 100.0 : load));
                if (s_showFps & CONFIG_PERF_RAM)
                {
                    GlobalMemoryStatus(&ms);
                    PerfAdd("\x03" "RAM %uMB", (u32)((ms.dwTotalPhys - ms.dwAvailPhys) >> 20));
                }
                if (s_showFps & CONFIG_PERF_RES)
                {
                    h64_xenos_internal_size(renderer, &rw, &rh);
                    PerfAdd("\x04" "%ux%u", rw, rh);
                }
                if (s_showFps & (CONFIG_PERF_TCPU | CONFIG_PERF_TGPU | CONFIG_PERF_TRAM))
                {
                    float t[3];
                    static const char *const names[3] = { "CPU", "GPU", "RAM" };
                    static const int bits[3] = { CONFIG_PERF_TCPU, CONFIG_PERF_TGPU, CONFIG_PERF_TRAM };
                    int k;
                    if (UiTemperatures(s_perfTemps, &t[0], &t[1], &t[2]))
                        for (k = 0; k < 3; k++)
                            if (s_showFps & bits[k])
                            {
                                // The thermometer by how warm it is: normal under 70 C, high to 75, hot above.
                                int icon = t[k] > 75.0f ? UI_PERF_ICON_TEMP_HOT : t[k] >= 70.0f ? UI_PERF_ICON_TEMP_WARN : UI_PERF_ICON_TEMP_OK;
                                PerfAdd("%c%s %d\xC2\xB0%c", icon, names[k], (int)(s_perfF ? t[k] * 9.0f / 5.0f + 32.0f + 0.5f : t[k] + 0.5f),
                                        s_perfF ? 'F' : 'C');
                            }
                }
                memcpy(s_fpsText, s_fpsBuild, sizeof(s_fpsText));   // the whole new text at once
            }
            if (statsOn)
            {
                // A sample a second, after the first 10 s (the boot); "low": under 92 % of the game's refresh rate.
                double fps = fpsFrames * 1000.0 / (nowMs - fpsStart), rate = 93750000.0 / sys->vi.frameCycles;
                if (statsSkip) statsSkip = 0;
                else if (GetTickCount() - stats.startTick >= 10000)
                {
                    stats.fpsSum += fps;
                    stats.seconds++;
                    if (fps < stats.fpsMin) stats.fpsMin = fps;
                    if (fps < rate * 0.92) stats.below++;
                }
                if (GetTickCount() >= statsSaveAt)
                {
                    StatsPendingWrite(s_logDrive, &stats);   // what is sent if this session never ends (a freeze)
                    statsSaveAt = GetTickCount() + 60000;
                }
            }
            busyTicks = 0;
            gfxProfAt = sys->prof[H64_PROF_GFX_HLE];
            fpsFrames = 0;
            fpsStart = nowMs;
        }
        {
            double s0 = QpcMs();
            SavesTick(sys, presented, 0);
            if (QpcMs() - s0 > s_maxSaves) s_maxSaves = QpcMs() - s0;
        }
        if (!s_cheats.list.empty() && s_netMode <= 0) h64_cheats_apply(&s_cheats, sys);   // the cheats on: written once a frame
        {
            int i;
            for (i = 0; i < c->shotCount; i++)
                if (c->shots[i] == presented)
                {
                    char path[64];
                    _snprintf(path, sizeof(path), "%s:\\shot_%05u.bmp", s_logDrive, presented);
                    path[sizeof(path) - 1] = 0;
                    h64_hle_async_wait(sys);
                    WorkWaitAll(NULL);
                    H64_INFO("[main] screenshot %s: %s", path, h64_xenos_save_frame(renderer, path) ? "failed" : "saved");
                }
        }
        if (c->pauseAt && presented == c->pauseAt)
        {
            int k;
            H64_INFO("[main] paused at VI %u", presented);
            if (s_gfxQ.thread) WorkWaitAll(NULL);
            for (k = 0; k < 400; k++) { Sleep(50); h64_xenos_present(renderer); }
            H64_INFO("[main] resumed");
        }
        if (c->exitAfter && presented >= c->exitAfter)
        {
            H64_INFO("[main] exitafter reached (%u VIs)", presented);
            break;
        }

        // Pacing on the clock, one VI period per frame, so frames reach the
        // screen evenly (pacing on the audio queue alone ran several frames
        // back to back, then waited: Paper Mario's name screen, intervals
        // from 2 to 130 ms at 60 FPS on average). XAudio2's rate control keeps
        // the queue near its target; the queue only nudges the period by up
        // to 1 %, and stops the clock if it ran far over the target.
        QueryPerformanceCounter(&t0);
        queued = xb_audio_queued_ms();
        if (VBlankPacing(c, sys, &freq))
        {
            // Adaptive V-Sync (Settings > Screen > V-Sync): each frame starts at a TV refresh,
            // so its picture is ready for the next one and the GPU's wait for it is short (on
            // the clock, out of step with the TV, each present waited up to a whole refresh
            // and held the graphics worker: Smash Remix 52-57 FPS, 60 without V-Sync). A frame
            // that missed its refresh starts at once (no catching up); the renderer shows a late
            // picture at once (a tear on that frame) instead of waiting for the next refresh.
            // The game runs at the TV's rate (59.94 Hz for 60.1): XAudio2's rate control keeps
            // the sound in step.
            // A frame in reserve (Settings > Screen): the game runs a refresh ahead, each picture held for
            // its own refresh (PresentHold), so one that took longer than a refresh is still shown on time.
            // Dinosaur Planet, 12.5 ms a frame with some of 18-20 ms: each of those was shown a refresh late
            // and the next 4 ms after it, a stutter at 60 FPS.
            const int reserve = FrameReserveOn(c) && s_gfxQ.thread;
            LONG want;
            int target = xb_audio_target_ms();
            if (reserve && (s_frameSlot == 0 || s_vblanks - s_paceVblank > 2)) s_paceVblank = s_vblanks - 1;   // (re)starting, or far behind: from now
            want = reserve ? s_paceVblank : s_paceVblank + 1;
            while (s_vblanks - want < 0)
            {
                LONGLONG left;
                QueryPerformanceCounter(&now);
                if (now.QuadPart - t0.QuadPart > freq.QuadPart / 20) break;   // no refresh in 50 ms: never wait for good
                left = s_vblankAt + s_vblankTicks - now.QuadPart;
                if (left > freq.QuadPart / 500) Sleep(1);   // more than 2 ms to the refresh: sleep, then spin
            }
            if (reserve)
            {
                s_paceVblank++;
                s_frameSlot = s_paceVblank + 1;
                if (!s_frameSlot) s_frameSlot = 1;
            }
            else
            {
                s_paceVblank = s_vblanks;
                s_frameSlot = 0;
            }
            QueryPerformanceCounter(&frameStart);   // the clock's pacing goes on from here if V-Sync is turned off
            if (queued > 2 * target && target > 0)
            {
                while (xb_audio_queued_ms() > target) Sleep(1);
                s_paceVblank = s_vblanks;
                if (reserve) s_frameSlot = s_paceVblank + 1;
                QueryPerformanceCounter(&frameStart);
            }
        }
        else
        {
            s_frameSlot = 0;
            LONGLONG frameTicks = freq.QuadPart * sys->vi.frameCycles / 93750000;
            int target = xb_audio_target_ms();
            if (queued >= 0 && target > 0)
            {
                double e = (double)(queued - target) / target;   // > 0: too much sound queued, slow down
                if (e > 1.0) e = 1.0;
                if (e < -1.0) e = -1.0;
                frameTicks += (LONGLONG)(frameTicks * 0.01 * e);
            }
            for (;;)
            {
                LONGLONG left;
                QueryPerformanceCounter(&now);
                left = frameTicks - (now.QuadPart - frameStart.QuadPart);
                if (left <= 0) break;
                if (left > freq.QuadPart / 500) Sleep(1);   // more than 2 ms left: sleep, then spin to the deadline
            }
            frameStart.QuadPart += frameTicks;
            if (now.QuadPart - frameStart.QuadPart > frameTicks * 4) frameStart = now;   // far behind: do not catch up
            if (queued > 2 * target && target > 0)
            {
                // Far too much sound queued (after a pause, a load): let it drain.
                while (xb_audio_queued_ms() > target) Sleep(1);
                QueryPerformanceCounter(&frameStart);
            }
        }

        QueryPerformanceCounter(&t1);
        profWait += t1.QuadPart - t0.QuadPart;
        {
            double wms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
            if (wms > s_maxWait) s_maxWait = wms;
        }
        if (QpcMs() - perfStart >= 2000.0)
        {
            double perfNow = QpcMs(), ms = perfNow - perfStart;
            H64XenosStats xs;
            XbAudioStats as;
            h64_xenos_stats(renderer, &xs, 1);
            xb_audio_stats(&as, 1);
            H64_INFO("[perf] vi/s=%.1f mips=%.1f tris=%u rects=%u fills=%u texup=%u (new %u) shaders=%u copyback=%u fbswitch=%u audio=%u buffers %u underruns fill %u ms rate %u.%03u played %u Hz submitted %u Hz dropped %u",
                     framesSincePerf * 1000.0 / ms, (double)(sys->cpu.instructions - instrAtPerf) / (ms * 1000.0),
                     xs.triangles, xs.rects, xs.fills, xs.textureUploads, xs.textureCreates, xs.shaderCompiles, xs.copyBacks, xs.fbSwitches, as.buffers, as.underruns, as.fillMs,
                     as.ratioPermille / 1000, as.ratioPermille % 1000, as.playedHz, as.submittedHz, as.dropped);
            if (s_presCount)
            {
                double mean = s_presSum / s_presCount, var = s_presSumSq / s_presCount - mean * mean;
                H64_INFO("[pace] %u presents: interval mean %.1f ms, sd %.1f, min %.1f, max %.1f, %u over 25 ms | longest: run %.1f, present %.1f, saves %.1f, wait %.1f, last stats block %.1f ms",
                         s_presCount, mean, var > 0 ? sqrt(var) : 0.0, s_presMin, s_presMax, s_presLong, s_maxRun, s_maxPresent, s_maxSaves,
                         s_maxWait, s_lastPerfBlock);
                s_maxRun = s_maxPresent = s_maxSaves = s_maxWait = 0;
                s_presSum = s_presSumSq = s_presMax = 0;
                s_presMin = 1e9;
                s_presCount = s_presLong = 0;
            }
            {
                // Audio the game queued in this period, and what it read back from AI_LEN.
                static u64 q0, r0, rs0;
                static u32 b0;
                u32 nb = sys->ai.statBuffers - b0;
                u64 nr = sys->ai.statReads - r0;
                H64_INFO("[ai] %u buffers, %.1f samples each; %llu AI_LEN reads, %.1f samples left on average", nb,
                         nb ? (sys->ai.statQueued - q0) / 4.0 / nb : 0.0, (unsigned long long)nr,
                         nr ? (sys->ai.statReadSum - rs0) / 4.0 / nr : 0.0);
                q0 = sys->ai.statQueued; r0 = sys->ai.statReads; rs0 = sys->ai.statReadSum; b0 = sys->ai.statBuffers;
            }
            {
                MEMORYSTATUS ms;
                GlobalMemoryStatus(&ms);
                H64_INFO("[mem] free %u KB of %u KB", (u32)(ms.dwAvailPhys / 1024), (u32)(ms.dwTotalPhys / 1024));
            }
            H64_INFO("[state] pc=%08X frames=%u rsp tasks=%u (hle %u) irq SP %u SI %u AI %u VI %u PI %u DP %u mi intr=%02X mask=%02X origin=%06X",
                     (u32)sys->cpu.pc, sys->vi.frames, sys->rsp.tasks, sys->rsp.hleTasks, sys->miRaised[0],
                     sys->miRaised[1], sys->miRaised[2], sys->miRaised[3], sys->miRaised[4], sys->miRaised[5],
                     sys->mi.intr, sys->mi.mask, sys->vi.regs[1] & 0xFFFFFF);
            if (framesSincePerf)
            {
                // Milliseconds per frame. cpu = the frame run minus the subsystems below it
                // (render is part of gfx for HLE tasks, of rsp for LLE ones).
                double k = 1000.0 / (double)freq.QuadPart / framesSincePerf;
                u64 *pr = sys->prof;
                double run = profRun * k, gfx = pr[H64_PROF_GFX_HLE] * k, aud = pr[H64_PROF_AUDIO_HLE] * k;
                double rsp = pr[H64_PROF_RSP_LLE] * k, jitc = pr[H64_PROF_JIT_COMPILE] * k;
                const H64JitStats *js = sys->jit ? &sys->jit->stats : NULL;
                H64_INFO("[prof] ms/frame: run %.1f (cpu %.1f, gfx parse %.1f, gfx hle %.1f incl. render %.1f, audio hle %.1f, rsp lle %.1f, "
                         "jit compile %.1f) present %.1f wait %.1f | jit +%llu blocks, +%llu invalidations, +%llu flushes, %llu interpreted/frame",
                         run, run - (s_gfxQ.thread ? 0.0 : gfx) - (s_audioQ.thread ? 0.0 : aud) - ((s_gfxQ.thread || s_audioQ.thread) ? pr[H64_PROF_ASYNC_WAIT] * k : 0.0) - rsp - jitc,
                         pr[H64_PROF_GFX_PARSE] * k, gfx,
                         pr[H64_PROF_RENDER] * k, aud, rsp, jitc, profPresent * k,
                         profWait * k, js ? (unsigned long long)(js->blocksCompiled - jitBlocks) : 0ull,
                         js ? (unsigned long long)(js->invalidations - jitInval) : 0ull,
                         js ? (unsigned long long)(js->flushes - jitFlush) : 0ull,
                         js ? (unsigned long long)((js->helperCalls - jitHelper) / framesSincePerf) : 0ull);
                if (js)
                {
                    u64 ins = sys->cpu.instructions - insnsPrev, idle = js->idleSkipped - jitIdle;
                    static u64 linksPrev, unlinksPrev, staleLast;
                    H64_INFO("[jit] per frame: %llu instructions executed, %llu skipped idle, %llu blocks run, %llu interpreter steps, "
                             "%llu links made, %llu TLB checks failed, %llu remapped blocks unlinked",
                             (unsigned long long)((ins - idle) / framesSincePerf), (unsigned long long)(idle / framesSincePerf),
                             (unsigned long long)((js->blocksRun - jitRun) / framesSincePerf),
                             (unsigned long long)((js->interpSteps - jitSteps) / framesSincePerf),
                             (unsigned long long)((js->linksMade - linksPrev) / framesSincePerf),
                             (unsigned long long)((js->tlbStale - staleLast) / framesSincePerf),
                             (unsigned long long)((js->tlbUnlinks - unlinksPrev) / framesSincePerf));
                    staleLast = js->tlbStale;
                    linksPrev = js->linksMade;
                    unlinksPrev = js->tlbUnlinks;
                    jitBlocks = js->blocksCompiled; jitInval = js->invalidations; jitFlush = js->flushes; jitHelper = js->helperCalls;
                    jitRun = js->blocksRun; jitIdle = js->idleSkipped; jitSteps = js->interpSteps;
                }
                insnsPrev = sys->cpu.instructions;
                if (xs.bigW)
                    H64_INFO("[xtex] largest decode %ux%u (%u of 64K+ texels) fmt %u size %u stride %u mask %u/%u flags %X from a %s",
                             xs.bigW, xs.bigH, xs.bigCount, xs.bigFmt, xs.bigSize, xs.bigStride, xs.bigMaskS, xs.bigMaskT, xs.bigFlags,
                             xs.bigRect ? "rectangle" : "triangle");
                PcSamplerReport();
                H64_INFO("[gprof] ms/frame on the graphics workers: %u triangles a frame, snapshots %.1f (%u KB a frame), vertices %.1f (%u a frame; lighting %.1f, %u lights a frame), TMEM loads %u (%u KB, %.1f ms), palettes %u (%.1f ms)",
                         (u32)(sys->prof[H64_PROF_GFX_NTRI] / framesSincePerf), pr[H64_PROF_GFX_SNAPSHOT] * k, (u32)(sys->prof[H64_PROF_SNAP_BYTES] / framesSincePerf / 1024),
                         pr[H64_PROF_GFX_VTX] * k, (u32)(sys->prof[H64_PROF_GFX_NVTX] / framesSincePerf),
                         pr[H64_PROF_GFX_LIGHT] * k, (u32)(sys->prof[H64_PROF_GFX_NLIGHT] / framesSincePerf),
                         (u32)(sys->prof[H64_PROF_TMEM_LOADS] / framesSincePerf), (u32)(sys->prof[H64_PROF_TMEM_WORDS] * 8 / framesSincePerf / 1024),
                         pr[H64_PROF_TMEM_TIME] * k, (u32)(sys->prof[H64_PROF_TMEM_NTLUT] / framesSincePerf), pr[H64_PROF_TMEM_TLUT] * k);
                H64_INFO("[cprof] ms/frame inside cpu: generated code %.1f (helper %.1f), scheduler events %.1f, TLB changes %.1f; waiting for the graphics worker %.1f (task end %.1f, RDP via DPC %.1f, task start %.1f)%s",
                         pr[H64_PROF_BLOCKS] * k, pr[H64_PROF_HELPER] * k, pr[H64_PROF_EVENTS] * k, pr[H64_PROF_TLB] * k, pr[H64_PROF_ASYNC_WAIT] * k,
                         pr[H64_PROF_WAIT_TASK] * k, pr[H64_PROF_WAIT_DPC] * k, pr[H64_PROF_WAIT_START] * k,
                         s_gfxQ.thread ? " (graphics and audio HLE run on workers, beside cpu)" : "");
                H64_INFO("[xprof] ms/frame: rdp commands %.1f (state %.1f, textures %.1f: keys %.1f, decode %.1f [create %.1f lock %.1f decode %.1f], %u texels/frame, %u cache resets, arena %u textures %u chunk reuses, %u DXT from packs, %u rectangles from GPU frames) draw calls %.1f (%u a frame), copy-backs %.1f (GPU wait %.1f), TMEM loads %.1f, %u resolves (%u copy backs), %u loads over CPU writes, %u CPU sprite presents",
                         xs.tRdp * k, xs.tState * k, xs.tTexture * k, xs.tHash * k, xs.tDecode * k, xs.tCreate * k, xs.tLock * k,
                         xs.tFill * k, xs.texelsDecoded / framesSincePerf, xs.retires, xs.arenaTextures, xs.arenaEvictions, xs.htsDxt, xs.fbTexRects, xs.tDraw * k, xs.draws / (framesSincePerf ? framesSincePerf : 1), xs.tCopyBack * k, xs.tCopyBackWait * k, xs.tLoads * k, xs.loadResolves, xs.resolveCopyBacks, xs.cpuWrittenLoads, xs.cpuSprites);
            }
            if (statsOn)
            {
                double k = 1000.0 / (double)freq.QuadPart;
                stats.runMs += profRun * k;
                stats.gfxMs += sys->prof[H64_PROF_GFX_HLE] * k;
                stats.texMs += xs.tTexture * k;
                stats.copyMs += xs.tCopyBack * k;
                stats.frames += framesSincePerf;
            }
            memset(sys->prof, 0, sizeof(sys->prof));
            profRun = profPresent = profWait = 0;
            perfStart = perfNow;   // the next period starts now, the time spent logging included
            s_lastPerfBlock = QpcMs() - perfNow;
            framesSincePerf = 0;
            instrAtPerf = sys->cpu.instructions;
        }
    }
    RumbleUpdate(sys, 1);
    WorkersStop(sys);
    SavesTick(sys, presented, 1);
    if (s_netHiTex >= 0) { c->hiTex = s_netHiTex; s_netHiTex = -1; }
    SettingsSetOnline(0);
    NetStop();
    LogFlush();
    if (s_stateThumb) { UiImageRelease(dev, s_stateThumb); s_stateThumb = NULL; }
    s_stateThumbSlot = -1;   // another game, other pictures
    if (s_npTex) { UiImageRelease(dev, s_npTex); s_npTex = NULL; }
    free(s_npRgba);
    s_npRgba = NULL;
    s_npTitle[0] = 0;
    sys->renderer = NULL;
    h64_xenos_free(renderer);
    xb_audio_game_stop();
    s_samplePc = NULL;
    Sleep(5);
    FreeSystem(sys);
    H64_INFO("[main] game ended: %s", result == RG_BROWSER ? "ROM list" : "dashboard");
    LogFlush();
    OnlineSetStatus("");
    s_toRoom = 0;
    if (!OnlineRoomBack()) OnlineRoomClose();   // a room this game did not take (it takes it in NetConfig) - not one given back to
    OnlinePlayed(onlineCode, onlineGame, (int)((GetTickCount() - onlineStart) / 60000));
    PlaytimeAdd(romPath, (int)((GetTickCount() - onlineStart) / 1000));   // the ROM list's "Most played"
    if (statsOn)
    {
        // The summary; back in the ROM list after 3 minutes or more, the game's one-time vote
        // goes with it (B skips it - asked once all the same).
        StatsPendingClear(s_logDrive);
        if (StatsSessionWorth(&stats))
        {
            const char *vote = "", *reason = "";
            if (!s_crashed && result == RG_BROWSER && !OnlineRoomBack() && StatsSessionMinutes(&stats) >= 3 && !StatsVoted(s_logDrive, stats.code))
            {
                StatsMarkVoted(s_logDrive, stats.code);
                VoteScreen(dev, s_npTitle[0] ? s_npTitle : stats.game, &vote, &reason);
            }
            StatsSend(&stats, s_crashed ? "error" : "normal", vote, reason);
        }
    }
    if (s_crashed) MessageScreen(dev, Tr(T_CRASHED), s_crashText, D3DCOLOR_XRGB(240, 60, 60));
    return result;
}

int __cdecl main()
{
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    int tests = 0, checks = 0, failures;
    char status[256];
    static char jitReport[512];
    Config cfg;

    InitializeCriticalSection(&s_logLock);
    InitializeCriticalSection(&s_logFileLock);
    // Shared for reading: xbdm's getfile (and FTP) can fetch it while a game runs
    // (with fopen's default sharing it answered "access denied").
    s_log = _fsopen("game:\\laika64.log", "w", _SH_DENYNO);
    if (!s_log)
    {
        s_log = _fsopen("cache:\\laika64.log", "w", _SH_DENYNO);
        s_logDrive = "cache";
    }
    h64_log_set_sink(file_sink);
    SetUnhandledExceptionFilter(CrashFilter);
    s_logThread = CreateThread(NULL, 64 * 1024, LogThread, NULL, CREATE_SUSPENDED, NULL);
    if (s_logThread)
    {
        XSetThreadProcessor(s_logThread, 5);
        SetThreadPriority(s_logThread, THREAD_PRIORITY_BELOW_NORMAL);
        ResumeThread(s_logThread);
    }
    H64_INFO("[main] LAIKA 64 V2 %s (Xbox 360), %s-endian, %d-bit pointers", H64_VERSION_STRING,
             H64_HOST_BIG_ENDIAN ? "big" : "little", (int)(sizeof(void *) * 8));
    LoadConfig(&cfg);
    if (!cfg.xenia) MountDrives();   // Hdd1:, Usb0: ... for Settings > Paths (and paths chosen on them)
    UiSetSkinWant(cfg.skinName);   // set early: UiBrand/UiBackdrop lazily load it on first use (even the language screen)
    xb_audio_set_game_volume(cfg.volumeGame);     // remembered even before the engine exists - applied to each voice as it's created
    xb_audio_set_music_volume(cfg.volumeMusic);
    xb_audio_set_sfx_volume(cfg.volumeSfx);
    xb_audio_set_mono(cfg.audioMono);
    // logcpu=N: the hardware thread that writes the log (5 by default, shared
    // with the display list parser; checks of the 2 s stalls).
    if (s_logThread && cfg.logCpu >= 0 && cfg.logCpu <= 5 && cfg.logCpu != 5) XSetThreadProcessor(s_logThread, (DWORD)cfg.logCpu);
    if (cfg.xenia || !cfg.fpuFlags) h64_fenv_disable_host_flags();
    s_jitNoFpu = !cfg.jitFpu;
    s_noRegCache = !cfg.regCache;
    s_noFpCache = !cfg.fpCache;
    s_fastFpu = cfg.fastFpu;
    s_fullExits = cfg.fullExits;
    s_statusFast = cfg.statusFast;
    s_noSuper = !cfg.superblocks;
    s_cpi = cfg.cpi >= 1 && cfg.cpi <= 8 ? (u32)cfg.cpi : 2;
    H64_INFO("[main] settings: mode=%s cpu=%s hle=%d rom=%s jitfpu=%d cpi=%d regcache=%d", cfg.mode, cfg.cpu, cfg.hle, cfg.rom[0] ? cfg.rom : "(auto)",
             cfg.jitFpu, cfg.cpi, cfg.regCache);

    failures = h64_run_all_unit_tests(&tests, &checks);
    _snprintf(status, sizeof(status), "Unit tests: %s\n%d tests, %d checks, %d failures",
              failures ? "FAILED" : "PASSED", tests, checks, failures);
    status[sizeof(status) - 1] = 0;
    H64_INFO("[main] UNIT %s: %d tests, %d checks, %d failures", failures ? "FAILED" : "PASSED", tests, checks,
             failures);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.EnableAutoDepthStencil = FALSE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    {
        H64_ERROR("[main] CreateDevice failed");
        dev = NULL;
    }
    if (dev) dev->SetVerticalBlankCallback(VBlankCallback);   // the TV's refreshes, for the adaptive V-Sync

    if (failures)
        MessageScreen(dev, "Unit tests failed", status, D3DCOLOR_XRGB(240, 60, 60));
    else if (!strcmp(cfg.mode, "jittest"))
    {
        if (dev)
        {
            dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
            UiBegin(dev);
            UiBackdrop();
            UiBrand(UI_SAFE_X, 72, 34);
            UiText(UI_REGULAR, 26, UI_WIDTH * 0.5f, UI_HEIGHT * 0.5f, UI_COL_TEXT, "Recompiler test running" UI_CH_ELLIPSIS, UI_CENTER);
            UiEnd();
            dev->Present(NULL, NULL, NULL, NULL);
        }
        RunDynarecTest(&cfg, jitReport, sizeof(jitReport));
        MessageScreen(dev, status, jitReport,
                      strstr(jitReport, "DIVERGED") ? D3DCOLOR_XRGB(240, 60, 60) : D3DCOLOR_XRGB(200, 200, 120));
    }
    else if (dev)
    {
        // Scripted runs and rom= (or game:\test.z64) start one game directly
        // with harissa64v2.ini alone; otherwise the ROM browser, with the
        // menu's settings and the game's profile over harissa64v2.ini.
        int scripted = cfg.input.count || cfg.exitAfter || cfg.trace || cfg.shotCount || cfg.loadState || cfg.saveStateAt;
        // Checks driven by harissa64v2.ini: no language list.
        int checks = cfg.autoStart || cfg.controlsTest >= 0 || cfg.browserDemo || cfg.artAll || cfg.browserSel[0] || cfg.browserAt ||
                     cfg.menuAt || cfg.aboutAt;
        char dir[64], menuFile[160];
        _snprintf(dir, sizeof(dir), "%s:\\config", s_logDrive);
        dir[sizeof(dir) - 1] = 0;
        _snprintf(menuFile, sizeof(menuFile), "%s\\settings.ini", dir);
        menuFile[sizeof(menuFile) - 1] = 0;
        if (scripted || cfg.rom[0] || GetFileAttributesA("game:\\test.z64") != 0xFFFFFFFF)
        {
            s_settingsPath[0] = s_profilePath[0] = 0;
            ChooseLanguage(dev, &cfg, menuFile, 0);
            SettingsSetLangFile("");
            HiresSetPlaying(1);
            RunGame(dev, &cfg, NULL);
            HiresSetPlaying(0);
        }
        else
        {
            static Config base, game;
            char path[256], folder[64];
            CreateDirectoryA(dir, NULL);
            strcpy(s_settingsPath, menuFile);
            {
                // The first start (no language chosen yet): the welcome and its set-up, written to the
                // menu's settings (System > Setup assistant opens it again); else the language as chosen.
                static Config menu;
                int first;
                menu = cfg;
                ConfigParseFile(&menu, s_settingsPath);
                first = LangFromCode(menu.lang) < 0;
                if (!checks && !cfg.uiLang[0] && first)
                {
                    xb_audio_init();   // its button sounds
                    SettingsSetLangFile(s_settingsPath);
                    WelcomeScreen(dev, &menu, 1);
                    // A new install: no "what is new" after the welcome (the ROM list shows it after updates).
                    strncpy(menu.lastVersion, H64_VERSION_STRING, sizeof(menu.lastVersion) - 1);
                    menu.lastVersion[sizeof(menu.lastVersion) - 1] = 0;
                    if (!ConfigWriteMenuKeys(&menu, s_settingsPath, 1)) H64_WARN("[main] cannot write %s", s_settingsPath);
                }
                else ChooseLanguage(dev, &cfg, s_settingsPath, !checks);
            }
            SettingsSetLangFile(s_settingsPath);
            {
                // Anonymous statistics: asked once (after the welcome on a new install; at the next start
                // after an update), then Settings > System. A summary a frozen session left is sent now.
                static Config menu;
                menu = cfg;
                ConfigParseFile(&menu, s_settingsPath);
                if (menu.statsShare < 0 && !checks && !cfg.xenia)
                {
                    const char *labels[2];
                    labels[0] = Tr(T_STATS_YES);
                    labels[1] = Tr(T_STATS_NO);
                    menu.statsShare = ChoiceScreen(dev, Tr(T_STATS_ASK_TITLE), Tr(T_STATS_ASK_TEXT), labels, 2, 0, Tr(T_FOOT_ASK), 0) == 0;
                    H64_INFO("[stats] anonymous statistics %s", menu.statsShare ? "accepted" : "declined");
                    if (!ConfigWriteMenuKeys(&menu, s_settingsPath, 1)) H64_WARN("[main] cannot write %s", s_settingsPath);
                }
                StatsPendingFlush(s_logDrive, menu.statsShare > 0 && !cfg.xenia);
                if (!cfg.xenia) OnlineInit(s_logDrive);   // Settings > Online play: the profile, "still here" to the server
            }
            xb_audio_init();   // the engine must be up before the browser's music (game:\music\); a game's own call then does nothing
            for (;;)
            {
                base = cfg;
                ConfigParseFile(&base, s_settingsPath);
                {
                    int rb = RomBrowser(dev, &base, s_settingsPath, path, sizeof(path));
                    if (rb == 2) continue;   // Settings > Paths: another ROMs folder, the list made again
                    if (!rb) break;
                }
                if (cfg.autoStart > 0) { cfg.autoStart--; cfg.autoIndex++; }
                game = base;
                s_profilePath[0] = 0;
                if (RomFolderName(path, folder, sizeof(folder)))
                {
                    _snprintf(s_profilePath, sizeof(s_profilePath), "%s\\%s.ini", dir, folder);
                    s_profilePath[sizeof(s_profilePath) - 1] = 0;
                    if (ConfigParseFile(&game, s_profilePath)) H64_INFO("[main] game profile %s", s_profilePath);
                }
                int rg;
                DWORD playAt = GetTickCount();
                HiresSetPlaying(1);   // a pack converting meanwhile: one thread only
                s_netPlayed = 0;
                rg = RunGame(dev, &game, path);
                PlayStatsAdd(s_settingsPath, path, (int)((GetTickCount() - playAt) / 1000), s_netPlayed);
                if (!OnlineRoomBack()) OnlineRoomClose();   // an online room the game never got to take (the ROM could not be read) - not one it gave back
                HiresSetPlaying(0);
                if (rg == RG_DASHBOARD) break;
            }
        }
    }

    // Leaving (from the ROM list or the pause menu): the goodbye animation first.
    if (dev && !cfg.xenia) OutroScreen(dev);
    StatsWaitSent(5000);   // the last game's summary, still on its way
    if (s_logThread)
    {
        s_logQuit = 1;
        WaitForSingleObject(s_logThread, INFINITE);
        CloseHandle(s_logThread);
        s_logThread = NULL;
    }
    LogFlush();
    if (s_log)
        fclose(s_log);
    // Back to the dashboard (Aurora on the console). Xenia has none: it looks
    // for game:\default.xex and shows "Title Launch Failed", so with xenia=1
    // the title just ends.
    if (!cfg.xenia)
        XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
    return 0;
}
