// LAIKA 64 - anonymous play statistics (see xb_stats.h).

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "xb_net.h"
#include "xb_stats.h"

// The form "LAIKA 64 estadisticas" (laikaemu@gmail.com) and its questions' entry ids.
#define STATS_HOST "docs.google.com"
#define STATS_PATH "/forms/d/e/1FAIpQLSciDACApuYtMCF5FwiDXVgotbXJFmR8xroalb26JVh3bfNmnw/formResponse"
enum { F_GAME, F_CODE, F_VERSION, F_FPS_AVG, F_FPS_MIN, F_PCT_LOW, F_TIMES, F_ENDING, F_SETTINGS, F_MINUTES, F_VOTE, F_REASON, F_CONSOLE, F_COUNT };
static const char *const s_entry[F_COUNT] = { "659327786", "535791574", "173232975", "34059198", "2116461985", "1952752355",
                                              "1334835407", "2102775765", "788356227", "1997235836", "1181904485", "1132066091",
                                              "116688322" };

extern "C" VOID XeCryptRandom(PBYTE pb, DWORD cb);   // xboxkrnl export

static void config_file(const char *drive, const char *name, char *out, size_t size);

void StatsConsoleId(const char *drive, char *out, size_t size)
{
    char file[96], id[16] = "";
    FILE *f;
    config_file(drive, "stats_console.txt", file, sizeof(file));
    if ((f = fopen(file, "r")) != NULL)
    {
        if (fgets(id, sizeof(id), f)) id[strcspn(id, "\r\n")] = 0;
        fclose(f);
    }
    if (strlen(id) != 8)
    {
        // Made once: random, nothing of the console or of its owner in it.
        BYTE r[4];
        XeCryptRandom(r, sizeof(r));
        _snprintf(id, sizeof(id) - 1, "%02x%02x%02x%02x", r[0], r[1], r[2], r[3]);
        id[sizeof(id) - 1] = 0;
        if ((f = fopen(file, "w")) != NULL)
        {
            fputs(id, f);
            fclose(f);
        }
    }
    strncpy(out, id, size - 1);
    out[size - 1] = 0;
}

void StatsSessionStart(StatsSession *s)
{
    memset(s, 0, sizeof(*s));
    s->fpsMin = 1e9;
    s->startTick = GetTickCount();
}

int StatsSessionMinutes(const StatsSession *s)
{
    return (int)((GetTickCount() - s->startTick + 30000) / 60000);
}

int StatsSessionWorth(const StatsSession *s)
{
    return GetTickCount() - s->startTick >= 60000 && s->seconds > 0;
}

// Appends "&entry.ID=value" with the value percent-encoded.
static void add(char *out, size_t size, int field, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(out);
    const unsigned char *p = (const unsigned char *)value;
    n += _snprintf(out + n, size - n, "%centry.%s=", strchr(out, '?') ? '&' : '?', s_entry[field]);
    for (; *p && n + 4 < size; p++)
    {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')
            out[n++] = (char)*p;
        else
        {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = 0;
}

static void build_path(const StatsSession *s, const char *ending, const char *vote, const char *reason, char *out, size_t size)
{
    char v[96];
    double f = s->frames ? 1.0 / s->frames : 0.0;
    strncpy(out, STATS_PATH, size - 1);
    out[size - 1] = 0;
    add(out, size, F_GAME, s->game);
    add(out, size, F_CODE, s->code);
    add(out, size, F_VERSION, H64_VERSION_STRING);
    _snprintf(v, sizeof(v) - 1, "%.1f", s->seconds ? s->fpsSum / s->seconds : 0.0);
    v[sizeof(v) - 1] = 0;
    add(out, size, F_FPS_AVG, v);
    _snprintf(v, sizeof(v) - 1, "%.1f", s->seconds ? s->fpsMin : 0.0);
    add(out, size, F_FPS_MIN, v);
    _snprintf(v, sizeof(v) - 1, "%u", s->seconds ? s->below * 100 / s->seconds : 0);
    add(out, size, F_PCT_LOW, v);
    _snprintf(v, sizeof(v) - 1, "run %.1f gfx %.1f tex %.1f copias %.1f", s->runMs * f, s->gfxMs * f, s->texMs * f, s->copyMs * f);
    add(out, size, F_TIMES, v);
    add(out, size, F_ENDING, ending);
    add(out, size, F_SETTINGS, s->settings);
    _snprintf(v, sizeof(v) - 1, "%d", StatsSessionMinutes(s));
    add(out, size, F_MINUTES, v);
    if (vote[0]) add(out, size, F_VOTE, vote);
    if (reason[0]) add(out, size, F_REASON, reason);
    if (s->console[0]) add(out, size, F_CONSOLE, s->console);
    strncat(out, "&submit=Submit", size - strlen(out) - 1);
}

static volatile LONG s_sending;   // summaries on their way

static DWORD WINAPI send_thread(LPVOID arg)
{
    char *path = (char *)arg;
    int status = 0;
    if (NetStart(0)) status = NetGet(STATS_HOST, path, 0, NULL, NULL, 0, NULL, 0);
    H64_INFO("[stats] summary sent: HTTP %d", status);
    free(path);
    InterlockedDecrement(&s_sending);
    return 0;
}

// The request on its own thread: hardware thread 3, as the update check (a big stack for the TLS handshake).
static void send_path(const char *path)
{
    HANDLE t;
    char *copy = _strdup(path);
    if (!copy) return;
    InterlockedIncrement(&s_sending);
    t = CreateThread(NULL, 256 * 1024, send_thread, copy, CREATE_SUSPENDED, NULL);
    if (!t) { free(copy); InterlockedDecrement(&s_sending); return; }
    XSetThreadProcessor(t, 3);
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(t);
    CloseHandle(t);
}

void StatsSend(const StatsSession *s, const char *ending, const char *vote, const char *reason)
{
    static char path[2048];
    build_path(s, ending, vote, reason, path, sizeof(path));
    H64_INFO("[stats] %s: %.1f FPS on average, %d min, %s%s%s", s->game, s->seconds ? s->fpsSum / s->seconds : 0.0, StatsSessionMinutes(s), ending,
             vote[0] ? ", vote " : "", vote);
    send_path(path);
}

void StatsSendReport(const char *drive, const char *game, const char *code, const char *vote, const char *reason)
{
    static char path[1024];
    char console[12];
    StatsConsoleId(drive, console, sizeof(console));
    strncpy(path, STATS_PATH, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    add(path, sizeof(path), F_GAME, game);
    add(path, sizeof(path), F_CODE, code);
    add(path, sizeof(path), F_VERSION, H64_VERSION_STRING);
    add(path, sizeof(path), F_ENDING, "reporte");
    add(path, sizeof(path), F_VOTE, vote);
    if (reason[0]) add(path, sizeof(path), F_REASON, reason);
    add(path, sizeof(path), F_CONSOLE, console);
    strncat(path, "&submit=Submit", sizeof(path) - strlen(path) - 1);
    H64_INFO("[stats] report: %s (%s), %s%s%s", game, code, vote, reason[0] ? ", " : "", reason);
    send_path(path);
}

void StatsWaitSent(int ms)
{
    DWORD until = GetTickCount() + ms;
    while (s_sending > 0 && (int)(until - GetTickCount()) > 0) Sleep(20);
}

static void config_file(const char *drive, const char *name, char *out, size_t size)
{
    _snprintf(out, size - 1, "%s:\\config\\%s", drive, name);
    out[size - 1] = 0;
}

void StatsPendingWrite(const char *drive, const StatsSession *s)
{
    static char path[2048];
    char file[96];
    FILE *f;
    build_path(s, "sin cerrar", "", "", path, sizeof(path));   // a freeze, or the game left by the Guide button: one cannot tell
    config_file(drive, "stats_pending.txt", file, sizeof(file));
    if ((f = fopen(file, "w")) != NULL)
    {
        fputs(path, f);
        fclose(f);
    }
}

void StatsPendingClear(const char *drive)
{
    char file[96];
    config_file(drive, "stats_pending.txt", file, sizeof(file));
    DeleteFileA(file);
}

void StatsPendingFlush(const char *drive, int send)
{
    static char path[2048];
    char file[96];
    FILE *f;
    config_file(drive, "stats_pending.txt", file, sizeof(file));
    if ((f = fopen(file, "r")) == NULL) return;
    path[0] = 0;
    fgets(path, sizeof(path), f);
    fclose(f);
    DeleteFileA(file);
    if (send && !strncmp(path, STATS_PATH, strlen(STATS_PATH)))
    {
        H64_INFO("[stats] the last session never ended (a freeze, or left by the Guide button): sent");
        send_path(path);
    }
}

int StatsVoted(const char *drive, const char *code)
{
    char file[96], line[64];
    FILE *f;
    int found = 0;
    config_file(drive, "stats_voted.txt", file, sizeof(file));
    if ((f = fopen(file, "r")) == NULL) return 0;
    while (!found && fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = 0;
        found = !strcmp(line, code);
    }
    fclose(f);
    return found;
}

void StatsMarkVoted(const char *drive, const char *code)
{
    char file[96];
    FILE *f;
    config_file(drive, "stats_voted.txt", file, sizeof(file));
    if ((f = fopen(file, "a")) != NULL)
    {
        fprintf(f, "%s\n", code);
        fclose(f);
    }
}
