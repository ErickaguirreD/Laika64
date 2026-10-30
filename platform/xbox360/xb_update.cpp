// Harissa64 - is a newer release on GitHub? (see xb_update.h)
//
// The request is GET /kernel64/Harissa64/releases/latest, whose answer is a
// redirect to .../releases/tag/v<version>: the version is read from the
// Location header, the page itself is never downloaded (xb_net.cpp does the
// HTTPS). The worst a fake server could do is show a wrong "new version"
// message.

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_log.h"
#include "xb_net.h"
#include "../../third_party/bearssl/inc/bearssl_hash.h"   // SHA-1 of the downloaded executable
#include "xb_update.h"

#define UPDATE_HOST "github.com"
#define UPDATE_PATH "/ErickaguirreD/Laika64/releases/latest"
#define UPDATE_ASSET "/ErickaguirreD/Laika64/releases/download/v%s/%s"   // the release's files
#define UPDATE_MAX (96u << 20)                                            // largest default.xex taken

static volatile LONG s_started, s_found;
static char s_current[32], s_latest[32];
static HANDLE s_thread;

// "v2.0.1", "2.0.2-dev" -> 2, 0, 1 (missing parts 0).
static void parse_version(const char *s, int v[3])
{
    int i;
    if (*s == 'v' || *s == 'V') s++;
    for (i = 0; i < 3; i++)
    {
        v[i] = atoi(s);
        while (*s >= '0' && *s <= '9') s++;
        if (*s == '.') s++;
    }
}

static int newer(const char *latest, const char *current)
{
    int a[3], b[3], i;
    parse_version(latest, a);
    parse_version(current, b);
    for (i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

// Fills `version` from the Location header of the redirect; 0 on failure.
static int fetch_latest(char *version, size_t size)
{
    char loc[512];
    const char *p, *e;
    int status = NetGet(UPDATE_HOST, UPDATE_PATH, 0, NULL, NULL, 0, loc, sizeof(loc));
    if (!status) return 0;
    // "Location: https://github.com/kernel64/Harissa64/releases/tag/v2.0.1"
    p = strstr(loc, "/releases/tag/");
    if (p)
    {
        p += 14;
        for (e = p; *e && *e != '"' && *e != '/'; e++) ;
        if (e > p && (size_t)(e - p) < size)
        {
            memcpy(version, p, e - p);
            version[e - p] = 0;
            if (*version == 'v' || *version == 'V') memmove(version, version + 1, strlen(version));
            if (version[0] >= '0' && version[0] <= '9') return 1;
        }
    }
    H64_INFO("[update] no release in the answer (status %d, %.60s)", status, loc);
    return 0;
}

static DWORD WINAPI update_thread(LPVOID)
{
    char latest[32];
    DWORD t0 = GetTickCount();
    if (!NetStart(0)) return 0;
    if (!fetch_latest(latest, sizeof(latest))) return 0;
    H64_INFO("[update] latest release %s, running %s (%u ms)", latest, s_current, (unsigned)(GetTickCount() - t0));
    if (newer(latest, s_current))
    {
        strncpy(s_latest, latest, sizeof(s_latest) - 1);
        InterlockedExchange(&s_found, 1);
    }
    return 0;
}

void UpdateCheckStart(const char *current)
{
    if (InterlockedExchange(&s_started, 1)) return;
    strncpy(s_current, current, sizeof(s_current) - 1);
    // A big stack for the TLS handshake (RSA/ECDHE); hardware thread 3, away
    // from the CPU (0) and graphics (2, 5) threads.
    s_thread = CreateThread(NULL, 256 * 1024, update_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!s_thread) return;
    XSetThreadProcessor(s_thread, 3);
    SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(s_thread);
}

// ---- The updater ----
static volatile LONG s_install;   // UpdateInstallState
static char s_installVersion[32], s_installWhy[96];

static void install_fail(const char *why)
{
    strncpy(s_installWhy, why, sizeof(s_installWhy) - 1);
    H64_INFO("[update] install of %s failed: %s", s_installVersion, why);
    InterlockedExchange(&s_install, -1);
}

// 40 hex digits (the first word of a .sha1 file) into 20 bytes; 0 if not that.
static int parse_sha1(const char *s, u8 out[20])
{
    int i;
    for (i = 0; i < 40; i++)
    {
        char ch = s[i];
        int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (v < 0) return 0;
        if (i & 1) out[i / 2] |= (u8)v; else out[i / 2] = (u8)(v << 4);
    }
    return 1;
}

static int write_file(const char *path, const u8 *data, u32 len)
{
    FILE *f = fopen(path, "wb");
    size_t put;
    if (!f) return 0;
    put = fwrite(data, 1, len, f);
    return fclose(f) == 0 && put == len;
}

static DWORD WINAPI install_thread(LPVOID)
{
    char path[256];
    u8 *xex = NULL, *sum = NULL, want[20], got[20];
    u32 xlen = 0, slen = 0;
    int st;
    if (!NetStart(0)) { install_fail("no network"); return 0; }
    // The checksum first (small): a release without one is not installed.
    _snprintf(path, sizeof(path) - 1, UPDATE_ASSET, s_installVersion, "default.xex.sha1");
    path[sizeof(path) - 1] = 0;
    st = NetGet(UPDATE_HOST, path, 1, &sum, &slen, 4096, NULL, 0);
    if (st != 200 || !sum || slen < 40 || !parse_sha1((const char *)sum, want))
    {
        free(sum);
        install_fail("no checksum in the release");
        return 0;
    }
    free(sum);
    _snprintf(path, sizeof(path) - 1, UPDATE_ASSET, s_installVersion, "default.xex");
    path[sizeof(path) - 1] = 0;
    H64_INFO("[update] downloading %s", path);
    st = NetGet(UPDATE_HOST, path, 1, &xex, &xlen, UPDATE_MAX, NULL, 0);
    if (st != 200 || !xex) { free(xex); install_fail("download failed"); return 0; }
    // An Xbox 360 executable of a sane size whose SHA-1 is the release's.
    if (xlen < (4u << 20) || memcmp(xex, "XEX2", 4)) { free(xex); install_fail("not an XEX file"); return 0; }
    {
        br_sha1_context sc;
        br_sha1_init(&sc);
        br_sha1_update(&sc, xex, xlen);
        br_sha1_out(&sc, got);
    }
    if (memcmp(got, want, 20)) { free(xex); install_fail("checksum mismatch"); return 0; }
    // Written beside it, then swapped in: the running one kept as default.xex.bak.
    if (!write_file("game:\\default.xex.new", xex, xlen)) { free(xex); install_fail("cannot write the file"); return 0; }
    free(xex);
    DeleteFileA("game:\\default.xex.bak");
    if (!MoveFileA("game:\\default.xex", "game:\\default.xex.bak")) { DeleteFileA("game:\\default.xex.new"); install_fail("cannot keep the current version"); return 0; }
    if (!MoveFileA("game:\\default.xex.new", "game:\\default.xex"))
    {
        MoveFileA("game:\\default.xex.bak", "game:\\default.xex");   // back as it was
        install_fail("cannot put the new version in place");
        return 0;
    }
    H64_INFO("[update] %s installed (%u bytes), the previous one is default.xex.bak", s_installVersion, xlen);
    InterlockedExchange(&s_install, 2);
    return 0;
}

void UpdateInstallStart(const char *version)
{
    HANDLE t;
    if (s_install == 1 || s_install == 2) return;
    strncpy(s_installVersion, version, sizeof(s_installVersion) - 1);
    s_installWhy[0] = 0;
    InterlockedExchange(&s_install, 1);
    t = CreateThread(NULL, 256 * 1024, install_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!t) { install_fail("no thread"); return; }
    XSetThreadProcessor(t, 3);
    ResumeThread(t);
    CloseHandle(t);
}

int UpdateInstallState(char *why, size_t size)
{
    if (why && size) { strncpy(why, s_installWhy, size - 1); why[size - 1] = 0; }
    return (int)s_install;
}

int UpdateCheckTake(char *version, size_t size)
{
    if (!InterlockedExchange(&s_found, 0)) return 0;
    strncpy(version, s_latest, size - 1);
    version[size - 1] = 0;
    return 1;
}
