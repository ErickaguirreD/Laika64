// Harissa64 - HTTPS downloads from the console (see xb_net.h).
#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "xb_net.h"

extern "C" VOID XeCryptRandom(PBYTE pb, DWORD cb);   // xboxkrnl export

#define SO_MARKINSECURE 0x5801   // RGH homebrew: allow this socket outside Xbox Live
#define NET_TIMEOUT_MS 10000
#define NET_HEAD_MAX 16384
#define NET_URL_MAX 4096        // signed storage URLs of GitHub release assets run near 1 KB

// ---- Network stack ----
// 0: not started, 1: starting (another thread waits), 2: online, 3: offline.
static volatile LONG s_netState;
static volatile LONG s_stackUp;

static int wait_network(void)
{
    XNADDR xna;
    DWORD r = XNET_GET_XNADDR_PENDING;
    int i;
    for (i = 0; i < 150; i++)   // DHCP may still be running: up to 15 s
    {
        if (!(XNetGetEthernetLinkStatus() & XNET_ETHERNET_LINK_ACTIVE)) { Sleep(100); continue; }
        r = XNetGetTitleXnAddr(&xna);
        if (r != XNET_GET_XNADDR_PENDING) break;
        Sleep(100);
    }
    return r != XNET_GET_XNADDR_PENDING && r != XNET_GET_XNADDR_NONE;
}

int NetStart(int retry)
{
    for (;;)
    {
        LONG st = InterlockedCompareExchange(&s_netState, 1, 0);
        if (st == 0) break;                              // this thread starts it
        if (st == 1) { Sleep(50); continue; }            // another thread is starting it
        if (st == 3 && retry && InterlockedCompareExchange(&s_netState, 1, 3) == 3) break;
        return st == 2;
    }
    if (!s_stackUp)
    {
        XNetStartupParams xnsp;
        WSADATA wsa;
        memset(&xnsp, 0, sizeof(xnsp));
        xnsp.cfgSizeOfStruct = sizeof(xnsp);
        xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
        if (XNetStartup(&xnsp) != 0 || WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            H64_INFO("[net] no network stack");
            InterlockedExchange(&s_netState, 3);
            return 0;
        }
        InterlockedExchange(&s_stackUp, 1);
    }
    if (!wait_network())
    {
        H64_INFO("[net] no network");
        InterlockedExchange(&s_netState, 3);
        return 0;
    }
    InterlockedExchange(&s_netState, 2);
    return 1;
}

// ---- A certificate check that accepts a chain with no trusted root
// (BearSSL's tools/certs.c x509_noanchor, MIT).
struct NoAnchor
{
    const br_x509_class *vtable;
    const br_x509_class **inner;
};

static void na_start_chain(const br_x509_class **ctx, const char *name)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    (*in)->start_chain(in, name);
}
static void na_start_cert(const br_x509_class **ctx, uint32_t length)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    (*in)->start_cert(in, length);
}
static void na_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    (*in)->append(in, buf, len);
}
static void na_end_cert(const br_x509_class **ctx)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    (*in)->end_cert(in);
}
static unsigned na_end_chain(const br_x509_class **ctx)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    unsigned r = (*in)->end_chain(in);
    return r == BR_ERR_X509_NOT_TRUSTED ? 0 : r;
}
static const br_x509_pkey *na_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    const br_x509_class **in = ((NoAnchor *)ctx)->inner;
    return (*in)->get_pkey(in, usages);
}
static const br_x509_class s_noAnchorVtable = {
    sizeof(NoAnchor), na_start_chain, na_start_cert, na_append, na_end_cert, na_end_chain, na_get_pkey
};

// ---- socket callbacks for br_sslio
static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int n = recv(*(SOCKET *)ctx, (char *)buf, (int)len, 0);
    return n > 0 ? n : -1;
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int n = send(*(SOCKET *)ctx, (const char *)buf, (int)len, 0);
    return n > 0 ? n : -1;
}

// The console's clock as BearSSL wants it (days since 1 January of year 0,
// seconds in the day): certificates are checked for their dates.
static void ssl_time(uint32_t *days, uint32_t *secs)
{
    FILETIME ft;
    ULARGE_INTEGER t;
    unsigned long long unixSecs;
    GetSystemTimeAsFileTime(&ft);
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    unixSecs = (t.QuadPart - 116444736000000000ULL) / 10000000ULL;
    *days = (uint32_t)(unixSecs / 86400) + 719528;
    *secs = (uint32_t)(unixSecs % 86400);
}

static int resolve(const char *host, IN_ADDR *addr)
{
    XNDNS *dns = NULL;
    WSAEVENT ev = WSACreateEvent();
    int ok = 0;
    if (ev == WSA_INVALID_EVENT) return 0;
    if (XNetDnsLookup(host, ev, &dns) == 0 && dns)
    {
        WaitForSingleObject(ev, NET_TIMEOUT_MS);
        if (dns->iStatus == 0 && dns->cina > 0) { *addr = dns->aina[0]; ok = 1; }
        XNetDnsRelease(dns);
    }
    WSACloseEvent(ev);
    return ok;
}

int NetResolve(const char *host, unsigned long *ipv4)
{
    IN_ADDR a;
    a.s_addr = inet_addr(host);
    if (a.s_addr == INADDR_NONE && !resolve(host, &a)) return 0;
    *ipv4 = a.s_addr;
    return 1;
}

// One connection's TLS state (~50 KB: allocated per request).
struct NetTls
{
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    NoAnchor na;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

// The value of header `name` ("content-length") in `head`, or NULL; its length in *n.
static const char *header(const char *head, const char *name, int *n)
{
    size_t k = strlen(name);
    const char *p = head;
    while ((p = strstr(p, "\r\n")) != NULL)
    {
        p += 2;
        if (!_strnicmp(p, name, k) && p[k] == ':')
        {
            const char *v = p + k + 1, *e;
            while (*v == ' ' || *v == '\t') v++;
            for (e = v; *e && *e != '\r' && *e != '\n'; e++) ;
            *n = (int)(e - v);
            return v;
        }
    }
    return NULL;
}

// Decodes a chunked body in place; returns its length, or -1 when malformed or cut.
static int dechunk(u8 *buf, u32 len)
{
    u32 in = 0, out = 0;
    for (;;)
    {
        u32 size = 0;
        int digits = 0;
        while (in < len)
        {
            u8 c = buf[in];
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) break;
            if (size > 0x0FFFFFFF) return -1;
            size = size * 16 + (u32)v;
            digits++;
            in++;
        }
        if (!digits) return -1;
        while (in < len && buf[in] != '\n') in++;   // chunk extensions, CR
        if (in >= len) return -1;
        in++;
        if (!size) return (int)out;
        if (size > len - in) return -1;
        memmove(buf + out, buf + in, size);
        out += size;
        in += size;
        if (in < len && buf[in] == '\r') in++;
        if (in < len && buf[in] == '\n') in++;
    }
}

// ---- Connections kept open between requests (HTTP keep-alive) ----
// Each request used to make its own connection and TLS handshake (key exchange and certificate
// checks on the Xbox's CPU), about eight for each game's pictures and information. A connection whose
// answer was read to its end goes here for a moment; the next request to the same host takes it.
#define NET_IDLE 6
#define NET_IDLE_MS 4000        // servers drop idle connections after some seconds: not kept longer
struct NetIdle { SOCKET s; NetTls *t; char host[128]; DWORD at; };
static NetIdle s_idle[NET_IDLE];
static CRITICAL_SECTION s_idleLock;
static volatile LONG s_idleInit;

static void idle_lock(void)
{
    if (InterlockedCompareExchange(&s_idleInit, 1, 0) == 0)
    {
        InitializeCriticalSection(&s_idleLock);
        InterlockedExchange(&s_idleInit, 2);
    }
    while (s_idleInit != 2) Sleep(1);
    EnterCriticalSection(&s_idleLock);
}

static void idle_close(NetIdle *c)
{
    closesocket(c->s);
    free(c->t);
    c->t = NULL;
}

// A kept connection to `host` (taken out of the list): 1 when there was one.
static int idle_take(const char *host, SOCKET *s, NetTls **t)
{
    int i, got = 0;
    DWORD now = GetTickCount();
    idle_lock();
    for (i = 0; i < NET_IDLE; i++)
    {
        NetIdle *c = &s_idle[i];
        if (!c->t) continue;
        if (now - c->at >= NET_IDLE_MS) { idle_close(c); continue; }
        if (!got && !_stricmp(c->host, host))
        {
            *s = c->s;
            *t = c->t;
            c->t = NULL;
            got = 1;
        }
    }
    LeaveCriticalSection(&s_idleLock);
    return got;
}

static void idle_put(const char *host, SOCKET s, NetTls *t)
{
    int i, slot = 0;
    idle_lock();
    for (i = 0; i < NET_IDLE; i++)
    {
        if (!s_idle[i].t) { slot = i; break; }
        if (s_idle[i].at - s_idle[slot].at > 0x80000000u) slot = i;   // else the one kept longest
    }
    if (s_idle[slot].t) idle_close(&s_idle[slot]);
    s_idle[slot].s = s;
    s_idle[slot].t = t;
    s_idle[slot].at = GetTickCount();
    strncpy(s_idle[slot].host, host, sizeof(s_idle[slot].host) - 1);
    s_idle[slot].host[sizeof(s_idle[slot].host) - 1] = 0;
    LeaveCriticalSection(&s_idleLock);
}

// A new connection to host:443 with its TLS set up (the handshake happens on the first write).
static int connect_tls(const char *host, SOCKET *ps, NetTls **pt)
{
    NetTls *t;
    IN_ADDR ip;
    SOCKADDR_IN sa;
    SOCKET s;
    BOOL one = TRUE;
    DWORD timeout = NET_TIMEOUT_MS;
    unsigned char seed[32];
    uint32_t days, secs;
    if (!resolve(host, &ip)) { H64_INFO("[net] cannot resolve %s", host); return 0; }
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    setsockopt(s, SOL_SOCKET, SO_MARKINSECURE, (const char *)&one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    {
        // A big receive window (set before connect: it is offered in the handshake). With
        // the default one a far server (GitHub's) sent ~0.7 MB/s, waiting for each window's
        // acknowledgement. The largest the system takes, from 1 MB down; told once.
        static int s_rcvLogged;
        int want, got = 0, gl = sizeof(got);
        for (want = 1 << 20; want >= 64 << 10; want >>= 1)
            if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&want, sizeof(want)) == 0) break;
        if (!s_rcvLogged)
        {
            s_rcvLogged = 1;
            getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&got, &gl);
            H64_INFO("[net] receive window %d KB", got >> 10);
        }
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    sa.sin_addr = ip;
    if (connect(s, (const sockaddr *)&sa, sizeof(sa)) != 0)
    {
        H64_INFO("[net] cannot connect to %s (%d)", host, WSAGetLastError());
        closesocket(s);
        return 0;
    }
    t = (NetTls *)malloc(sizeof(NetTls));
    if (!t) { closesocket(s); return 0; }
    br_ssl_client_init_full(&t->sc, &t->xc, NULL, 0);
    {
        // Speed: the downloads (HD packs of hundreds of MB, updates) are decrypted here, on
        // one hardware thread with no AES instructions. ChaCha20-Poly1305 first (fastest in
        // software, where the server honours the client's order), then AES-GCM; and the
        // table AES and 64-bit GHASH instead of the constant-time ones init_full picks on a
        // 32-bit build (BR_64 is not detected on the Xbox's compiler).
        static const uint16_t suites[] = {
            BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
            BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
            BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
            BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
            BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA, BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
            BR_TLS_RSA_WITH_AES_128_GCM_SHA256, BR_TLS_RSA_WITH_AES_256_GCM_SHA384,
            BR_TLS_RSA_WITH_AES_128_CBC_SHA256, BR_TLS_RSA_WITH_AES_128_CBC_SHA
        };
        br_ssl_engine_set_suites(&t->sc.eng, suites, sizeof(suites) / sizeof(suites[0]));
        br_ssl_engine_set_aes_ctr(&t->sc.eng, &br_aes_big_ctr_vtable);
        br_ssl_engine_set_aes_cbc(&t->sc.eng, &br_aes_big_cbcenc_vtable, &br_aes_big_cbcdec_vtable);
        br_ssl_engine_set_ghash(&t->sc.eng, br_ghash_ctmul64);
    }
    ssl_time(&days, &secs);
    br_x509_minimal_set_time(&t->xc, days, secs);
    t->na.vtable = &s_noAnchorVtable;
    t->na.inner = &t->xc.vtable;
    br_ssl_engine_set_x509(&t->sc.eng, &t->na.vtable);
    XeCryptRandom(seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof(seed));
    br_ssl_engine_set_buffer(&t->sc.eng, t->iobuf, sizeof(t->iobuf), 1);
    br_ssl_client_reset(&t->sc, host, 0);
    *ps = s;
    *pt = t;
    return 1;
}

// A request's path as the log shows it: without its query, which may carry account names and passwords
// (ScreenScraper's devid, devpassword, ssid, sspassword).
static int path_shown(const char *path) { return (int)strcspn(path, "?"); }

// One GET (no redirects). Returns the status or 0; see NetGet. With `sink`
// the body goes into that file as it arrives (NetDownload), `cancel` set
// stops it.
static int request(const char *host, const char *path, u8 **body, u32 *bodyLen, u32 maxLen, char *location, int locSize,
                   NetProgress *pr, FILE *sink, volatile long *cancel)
{
    NetTls *t = NULL;
    SOCKET s = INVALID_SOCKET;
    char *req = NULL, *head = NULL;
    int n = 0, r, status = 0, hlen = 0, ok = 0, keep = 0, attempt, reused = 0, closeAsked;
    u8 *buf = NULL;
    u32 cap = 0, got = 0;

    if (location && locSize) location[0] = 0;
    if (strlen(path) + strlen(host) > NET_URL_MAX) return 0;
    head = (char *)malloc(NET_HEAD_MAX + 1);
    req = (char *)malloc(NET_URL_MAX + 512);
    if (!head || !req) { free(head); free(req); return 0; }
    _snprintf(req, NET_URL_MAX + 511,
              "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Harissa64/" H64_VERSION_STRING
              " (Xbox 360; https://github.com/kernel64/Harissa64)\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n",
              path, host);
    req[NET_URL_MAX + 511] = 0;

    for (attempt = 0; attempt < 2; attempt++)
    {
        reused = attempt == 0 && idle_take(host, &s, &t);
        if (!reused && !connect_tls(host, &s, &t)) { s = INVALID_SOCKET; t = NULL; goto done; }
        br_sslio_init(&t->ioc, &t->sc.eng, sock_read, &s, sock_write, &s);
        n = 0;
        hlen = 0;
        if (br_sslio_write_all(&t->ioc, req, strlen(req)) == 0 && br_sslio_flush(&t->ioc) == 0)
        {
            // Headers (whatever body arrives with them is kept).
            while (n < NET_HEAD_MAX)
            {
                char *e;
                r = br_sslio_read(&t->ioc, head + n, NET_HEAD_MAX - n);
                if (r <= 0) break;
                n += r;
                head[n] = 0;
                if ((e = strstr(head, "\r\n\r\n")) != NULL) { hlen = (int)(e - head) + 4; break; }
            }
        }
        if (hlen || !reused || n) break;
        // A kept connection the server had closed meanwhile: again on a new one.
        closesocket(s);
        free(t);
        s = INVALID_SOCKET;
        t = NULL;
    }
    if (!hlen)
    {
        if (!n && t) H64_INFO("[net] %s: TLS error %d", host, br_ssl_engine_last_error(&t->sc.eng));
        goto done;
    }
    if (sink && !reused)
    {
        // The suite the server chose (a download's speed depends on it; see above).
        br_ssl_session_parameters sp;
        br_ssl_engine_get_session_parameters(&t->sc.eng, &sp);
        H64_INFO("[net] %s: TLS suite %04X", host, sp.cipher_suite);
    }
    head[hlen - 2] = 0;   // the headers alone, one CRLF kept for header()
    if (strncmp(head, "HTTP/1.", 7) || strlen(head) < 12) goto done;
    status = atoi(head + 9);
    {
        int vn;
        const char *v = header(head, "location", &vn);
        if (v && location && locSize > 0)
        {
            if (vn >= locSize) vn = locSize - 1;
            memcpy(location, v, vn);
            location[vn] = 0;
        }
        v = header(head, "connection", &vn);
        closeAsked = (v && vn >= 5 && !_strnicmp(v, "close", 5)) || !strncmp(head, "HTTP/1.0", 8);
    }
    if ((!body && !sink) || status < 200 || status >= 300)
    {
        // No body wanted (a redirect, an error): a short one read and dropped keeps the connection.
        int vn;
        const char *cl = header(head, "content-length", &vn), *te = header(head, "transfer-encoding", &vn);
        ok = 1;
        if (cl && !te && !closeAsked)
        {
            u32 want = (u32)strtoul(cl, NULL, 10), have = (u32)(n - hlen);
            if (want <= 65536)
            {
                while (have < want)
                {
                    r = br_sslio_read(&t->ioc, head, NET_HEAD_MAX < want - have ? NET_HEAD_MAX : want - have);
                    if (r <= 0) break;
                    have += (u32)r;
                }
                keep = have == want;
            }
        }
        goto done;
    }

    // The body: Content-Length bytes, chunked, or up to the end of the connection.
    {
        int vn, chunked = 0;
        const char *te = header(head, "transfer-encoding", &vn), *cl = header(head, "content-length", &vn);
        u32 want = 0;
        if (te && !_strnicmp(te, "chunked", 7)) chunked = 1;
        if (cl && !chunked) want = (u32)strtoul(cl, NULL, 10);
        if (sink)
        {
            // Into the file 64 KB at a time (a texture pack: hundreds of MB): Content-Length bytes,
            // a chunked answer (decoded as it comes) or, without either, up to the connection's end
            // (n64textures.com sends its files so).
            enum { CS_SIZE, CS_DATA, CS_END, CS_DONE };
            u8 *chunk = (u8 *)malloc(64 * 1024);
            const u8 *p = (const u8 *)head + hlen;
            u32 avail = (u32)(n - hlen), csize = 0, left = 0;
            int cs = CS_SIZE, ext = 0, bad = 0;
            if (!chunk) goto done;
            got = 0;
            if (pr)
            {
                if (want) InterlockedExchange(&pr->total, (LONG)want);   // else the caller's guess (the release's size) stays
                InterlockedExchange(&pr->got, 0);
            }
            if (!chunked && !want && cl) cs = CS_DONE;   // an empty file
            for (;;)
            {
                while (avail && !bad && cs != CS_DONE)
                {
                    if (!chunked)
                    {
                        u32 take = want && avail > want - got ? want - got : avail;
                        if (fwrite(p, 1, take, sink) != take) { bad = 1; break; }
                        got += take; p += take; avail -= take;
                        if (want && got >= want) cs = CS_DONE;
                    }
                    else if (cs == CS_SIZE)
                    {
                        u8 c = *p++;
                        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                        avail--;
                        if (c == '\n') { if (!csize) cs = CS_DONE; else { cs = CS_DATA; left = csize; } }
                        else if (c == ';') ext = 1;
                        else if (v >= 0 && !ext) { if (csize > 0x0FFFFFFF) bad = 1; csize = csize * 16 + (u32)v; }
                    }
                    else if (cs == CS_DATA)
                    {
                        u32 take = avail < left ? avail : left;
                        if (fwrite(p, 1, take, sink) != take) { bad = 1; break; }
                        got += take; p += take; avail -= take; left -= take;
                        if (!left) cs = CS_END;
                    }
                    else   // CS_END: the CRLF after a chunk's data
                    {
                        if (*p++ == '\n') { cs = CS_SIZE; csize = 0; ext = 0; }
                        avail--;
                    }
                }
                if (pr) InterlockedExchange(&pr->got, (LONG)got);
                if (bad || cs == CS_DONE || (cancel && *cancel)) break;
                r = br_sslio_read(&t->ioc, chunk, 64 * 1024);
                if (r <= 0) break;   // the end of the connection
                p = chunk;
                avail = (u32)r;
            }
            free(chunk);
            if (bad || (cancel && *cancel) || (want && got < want) || (chunked && cs != CS_DONE) || (!got && cs != CS_DONE))
            {
                H64_INFO("[net] %s%.*s: stopped at %u of %u bytes%s", host, path_shown(path), path, got, want, chunked ? " (chunked)" : "");
                goto done;
            }
            if (bodyLen) *bodyLen = got;
            ok = 1;
            // Kept when its length was known (a chunked answer's last CRLF may still be on its way).
            keep = !closeAsked && cs == CS_DONE && !chunked && cl;
            goto done;
        }
        if (want > maxLen) { H64_INFO("[net] %s%.*s: %u bytes, more than %u", host, path_shown(path), path, want, maxLen); goto done; }
        cap = want ? want : 64 * 1024;
        buf = (u8 *)malloc(cap + 1);
        if (!buf) goto done;
        got = (u32)(n - hlen);
        if (got > cap) got = cap;
        memcpy(buf, head + hlen, got);
        if (pr)
        {
            InterlockedExchange(&pr->total, (LONG)want);
            InterlockedExchange(&pr->got, (LONG)got);
        }
        for (;;)
        {
            if (want && got >= want) { keep = !closeAsked; break; }
            // A chunked answer ends with a chunk of size 0 (the connection stays open: not read to its end).
            if (chunked && got >= 5 && !memcmp(buf + got - 5, "0\r\n\r\n", 5)) { keep = !closeAsked; break; }
            if (!want && cl) { keep = !closeAsked; break; }   // Content-Length: 0
            if (got == cap)
            {
                u8 *nb;
                // A chunked answer carries its framing on top of the data.
                if (cap >= maxLen + (chunked ? maxLen / 16 + 4096 : 0)) { H64_INFO("[net] %s%.*s: more than %u bytes", host, path_shown(path), path, maxLen); goto done; }
                cap *= 2;
                nb = (u8 *)realloc(buf, cap + 1);
                if (!nb) goto done;
                buf = nb;
            }
            r = br_sslio_read(&t->ioc, buf + got, cap - got);
            if (r <= 0) break;
            got += (u32)r;
            if (pr) InterlockedExchange(&pr->got, (LONG)got);
        }
        if (want && got < want) { H64_INFO("[net] %s%.*s: cut at %u of %u bytes", host, path_shown(path), path, got, want); keep = 0; goto done; }
        if (chunked)
        {
            int d = dechunk(buf, got);
            if (d < 0) { H64_INFO("[net] %s%.*s: bad chunked answer", host, path_shown(path), path); keep = 0; goto done; }
            got = (u32)d;
        }
        if (got > maxLen) { keep = 0; goto done; }
        buf[got] = 0;
        *body = buf;
        *bodyLen = got;
        buf = NULL;
        ok = 1;
    }

done:
    if (t)
    {
        if (keep && ok) idle_put(host, s, t);
        else
        {
            closesocket(s);
            free(t);
        }
    }
    free(buf);
    free(head);
    free(req);
    return ok ? status : 0;
}

int NetGet(const char *host, const char *path, int follow, u8 **body, u32 *len, u32 maxLen, char *location, int locSize)
{
    return NetGetEx(host, path, follow, body, len, maxLen, location, locSize, NULL);
}

static int net_get(const char *host, const char *path, int follow, u8 **body, u32 *len, u32 maxLen, char *location, int locSize,
                   NetProgress *progress, FILE *sink, volatile long *cancel);

int NetGetEx(const char *host, const char *path, int follow, u8 **body, u32 *len, u32 maxLen, char *location, int locSize,
             NetProgress *progress)
{
    return net_get(host, path, follow, body, len, maxLen, location, locSize, progress, NULL, NULL);
}

int NetDownload(const char *host, const char *path, FILE *file, u32 *len, NetProgress *progress, volatile long *cancel)
{
    return net_get(host, path, 1, NULL, len, 0xFFFFFFFFu, NULL, 0, progress, file, cancel);
}

// The redirects followed (up to 4) round request().
static int net_get(const char *host, const char *path, int follow, u8 **body, u32 *len, u32 maxLen, char *location, int locSize,
                   NetProgress *progress, FILE *sink, volatile long *cancel)
{
    char curHost[128], *curPath = (char *)malloc(NET_URL_MAX + 1), *loc = (char *)malloc(NET_URL_MAX + 1);
    int hop, status = 0, done = 0;
    if (body) *body = NULL;
    if (len) *len = 0;
    if (!curPath || !loc) { free(curPath); free(loc); return 0; }
    strncpy(curHost, host, sizeof(curHost) - 1);
    curHost[sizeof(curHost) - 1] = 0;
    strncpy(curPath, path, NET_URL_MAX);
    curPath[NET_URL_MAX] = 0;
    for (hop = 0; hop < 5 && !done; hop++)
    {
        status = request(curHost, curPath, body, len, maxLen, loc, NET_URL_MAX + 1, progress, sink, cancel);
        if (location && locSize > 0)
        {
            strncpy(location, loc, locSize - 1);
            location[locSize - 1] = 0;
        }
        if (!follow || status < 300 || status >= 400 || !loc[0]) { done = 1; break; }
        // https://host/path, or /path on the same host.
        if (!strncmp(loc, "https://", 8))
        {
            const char *h = loc + 8, *slash = strchr(h, '/');
            size_t hl = slash ? (size_t)(slash - h) : strlen(h);
            if (hl >= sizeof(curHost)) { status = 0; break; }
            memcpy(curHost, h, hl);
            curHost[hl] = 0;
            strncpy(curPath, slash ? slash : "/", NET_URL_MAX);
        }
        else if (loc[0] == '/')
            strncpy(curPath, loc, NET_URL_MAX);
        else
        {
            status = 0;   // plain HTTP or a relative path: not followed
            break;
        }
        curPath[NET_URL_MAX] = 0;
    }
    if (!done) status = 0;   // too many redirects, or one not followed
    free(curPath);
    free(loc);
    return status;
}

// ---- WebSocket over TLS (wss://host/path): the online play's rooms and their games ----
// The TLS engine is run by hand over a socket that never blocks, so a call can ask "anything there?"
// without waiting (a game asks every frame). Messages are small: one that does not fit NET_WS_BUF,
// or one sent in pieces, ends the connection.
#define NET_WS_BUF 4096
struct NetWs
{
    SOCKET s;
    NetTls *t;
    u8 in[NET_WS_BUF];   // what arrived and is not a whole message yet
    int inLen;
    int dead;
};

static int ws_wait(SOCKET s, int write, int ms)
{
    fd_set fds;
    timeval tv;
    if (ms < 0) ms = 0;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    return select(0, write ? NULL : &fds, write ? &fds : NULL, NULL, &tv) > 0;
}

// Moves TLS records between the engine and the socket until the engine has data for the application
// (1), or nothing more arrives within `ms` (0); -1 when the connection is over.
static int ws_pump(NetWs *w, int ms)
{
    br_ssl_engine_context *eng = &w->t->sc.eng;
    const DWORD t0 = GetTickCount();
    for (;;)
    {
        unsigned st = br_ssl_engine_current_state(eng);
        size_t len;
        unsigned char *buf;
        int n;
        if (st == BR_SSL_CLOSED) return -1;
        if (st & BR_SSL_SENDREC)
        {
            buf = br_ssl_engine_sendrec_buf(eng, &len);
            n = send(w->s, (const char *)buf, (int)len, 0);
            if (n > 0) { br_ssl_engine_sendrec_ack(eng, n); continue; }
            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK && ws_wait(w->s, 1, NET_TIMEOUT_MS)) continue;
            return -1;
        }
        if (st & BR_SSL_RECVAPP) return 1;
        if (st & BR_SSL_RECVREC)
        {
            buf = br_ssl_engine_recvrec_buf(eng, &len);
            n = recv(w->s, (char *)buf, (int)len, 0);
            if (n > 0) { br_ssl_engine_recvrec_ack(eng, n); continue; }
            if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) return -1;
            n = ms - (int)(GetTickCount() - t0);
            if (n <= 0 || !ws_wait(w->s, 0, n)) return 0;
            continue;
        }
        return 0;
    }
}

// All of `data` to the other end (encrypted and on the socket before it returns).
static int ws_write(NetWs *w, const void *data, int len)
{
    br_ssl_engine_context *eng = &w->t->sc.eng;
    const u8 *p = (const u8 *)data;
    const DWORD t0 = GetTickCount();
    while (len > 0)
    {
        size_t room;
        unsigned char *buf;
        if (!(br_ssl_engine_current_state(eng) & BR_SSL_SENDAPP))
        {
            // The handshake is not over yet (or the engine is full): its records go on, a little at a time.
            if (GetTickCount() - t0 > NET_TIMEOUT_MS || ws_pump(w, 20) < 0) return 0;
            continue;
        }
        buf = br_ssl_engine_sendapp_buf(eng, &room);
        if (room > (size_t)len) room = (size_t)len;
        memcpy(buf, p, room);
        br_ssl_engine_sendapp_ack(eng, room);
        p += room;
        len -= (int)room;
    }
    br_ssl_engine_flush(eng, 0);
    return ws_pump(w, 0) >= 0;
}

// What the engine has decrypted, into w->in (as much as fits).
static void ws_take(NetWs *w)
{
    br_ssl_engine_context *eng = &w->t->sc.eng;
    while ((br_ssl_engine_current_state(eng) & BR_SSL_RECVAPP) && w->inLen < NET_WS_BUF)
    {
        size_t len;
        unsigned char *buf = br_ssl_engine_recvapp_buf(eng, &len);
        if (len > (size_t)(NET_WS_BUF - w->inLen)) len = (size_t)(NET_WS_BUF - w->inLen);
        memcpy(w->in + w->inLen, buf, len);
        w->inLen += (int)len;
        br_ssl_engine_recvapp_ack(eng, len);
    }
}

// One frame to the server: masked, as a client's must be.
static int ws_frame(NetWs *w, int opcode, const void *data, int len)
{
    u8 f[8 + 1024];
    u8 mask[4];
    int n = 0, k;
    if (len < 0 || len > 1024) return 0;
    XeCryptRandom(mask, sizeof(mask));
    f[n++] = (u8)(0x80 | opcode);
    if (len < 126) f[n++] = (u8)(0x80 | len);
    else { f[n++] = 0x80 | 126; f[n++] = (u8)(len >> 8); f[n++] = (u8)len; }
    memcpy(f + n, mask, 4);
    n += 4;
    for (k = 0; k < len; k++) f[n + k] = ((const u8 *)data)[k] ^ mask[k & 3];
    return ws_write(w, f, n + len);
}

void NetWsClose(NetWs *w)
{
    if (!w) return;
    if (!w->dead) ws_frame(w, 8, NULL, 0);   // a close frame: the server ends its side at once
    closesocket(w->s);
    free(w->t);
    free(w);
}

NetWs *NetWsOpen(const char *host, const char *path)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    NetWs *w;
    IN_ADDR ip;
    SOCKADDR_IN sa;
    BOOL one = TRUE;
    u_long nb = 1;
    unsigned char seed[32], raw[18];
    uint32_t days, secs;
    char key[28], req[900];
    DWORD t0;
    int k;
    if (strlen(path) + strlen(host) > 600 || !NetStart(0) || !resolve(host, &ip)) return NULL;
    w = (NetWs *)calloc(1, sizeof(NetWs));
    if (!w) return NULL;
    w->t = (NetTls *)malloc(sizeof(NetTls));
    w->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!w->t || w->s == INVALID_SOCKET) goto fail;
    setsockopt(w->s, SOL_SOCKET, SO_MARKINSECURE, (const char *)&one, sizeof(one));
    setsockopt(w->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    ioctlsocket(w->s, FIONBIO, &nb);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    sa.sin_addr = ip;
    if (connect(w->s, (const sockaddr *)&sa, sizeof(sa)) != 0 && (WSAGetLastError() != WSAEWOULDBLOCK || !ws_wait(w->s, 1, NET_TIMEOUT_MS)))
    {
        H64_INFO("[net] cannot connect to %s (%d)", host, WSAGetLastError());
        goto fail;
    }
    // The TLS client as request() sets it up (fast ciphers first, no trusted roots on the console).
    br_ssl_client_init_full(&w->t->sc, &w->t->xc, NULL, 0);
    {
        static const uint16_t suites[] = {
            BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
            BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
            BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
        };
        br_ssl_engine_set_suites(&w->t->sc.eng, suites, sizeof(suites) / sizeof(suites[0]));
        br_ssl_engine_set_aes_ctr(&w->t->sc.eng, &br_aes_big_ctr_vtable);
        br_ssl_engine_set_ghash(&w->t->sc.eng, br_ghash_ctmul64);
    }
    ssl_time(&days, &secs);
    br_x509_minimal_set_time(&w->t->xc, days, secs);
    w->t->na.vtable = &s_noAnchorVtable;
    w->t->na.inner = &w->t->xc.vtable;
    br_ssl_engine_set_x509(&w->t->sc.eng, &w->t->na.vtable);
    XeCryptRandom(seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&w->t->sc.eng, seed, sizeof(seed));
    br_ssl_engine_set_buffer(&w->t->sc.eng, w->t->iobuf, sizeof(w->t->iobuf), 1);
    br_ssl_client_reset(&w->t->sc, host, 0);

    // The upgrade request, with a random key (16 bytes in base64; the answer's hash of it is not checked).
    XeCryptRandom(raw, 16);
    raw[16] = raw[17] = 0;
    for (k = 0; k < 6; k++)
    {
        const u32 v = ((u32)raw[k * 3] << 16) | ((u32)raw[k * 3 + 1] << 8) | raw[k * 3 + 2];
        key[k * 4] = b64[v >> 18];
        key[k * 4 + 1] = b64[(v >> 12) & 63];
        key[k * 4 + 2] = b64[(v >> 6) & 63];
        key[k * 4 + 3] = b64[v & 63];
    }
    key[22] = key[23] = '=';
    key[24] = 0;
    _snprintf(req, sizeof(req) - 1,
              "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
              "Sec-WebSocket-Version: 13\r\nUser-Agent: LAIKA64/" H64_VERSION_STRING " (Xbox 360)\r\n\r\n",
              path, host, key);
    req[sizeof(req) - 1] = 0;
    if (!ws_write(w, req, (int)strlen(req))) { H64_INFO("[net] %s: no TLS connection (error %d)", host, br_ssl_engine_last_error(&w->t->sc.eng)); goto fail; }
    // The answer's headers: "HTTP/1.1 101"; what follows them is the first messages.
    for (t0 = GetTickCount();;)
    {
        int r, end = -1;
        for (k = 0; k + 3 < w->inLen; k++)
            if (!memcmp(w->in + k, "\r\n\r\n", 4)) { end = k + 4; break; }
        if (end >= 0)
        {
            if (w->inLen < 12 || memcmp(w->in, "HTTP/1.1 101", 12))
            {
                H64_INFO("[net] %s%.*s: not a WebSocket (%.12s)", host, path_shown(path), path, (const char *)w->in);
                goto fail;
            }
            memmove(w->in, w->in + end, w->inLen - end);
            w->inLen -= end;
            return w;
        }
        if (w->inLen >= NET_WS_BUF || GetTickCount() - t0 > NET_TIMEOUT_MS) goto fail;
        r = ws_pump(w, 500);
        if (r < 0) goto fail;
        if (r > 0) ws_take(w);
    }
fail:
    w->dead = 1;
    if (w->s != INVALID_SOCKET) closesocket(w->s);
    free(w->t);
    free(w);
    return NULL;
}

int NetWsSend(NetWs *w, const void *data, int len, int text)
{
    if (!w || w->dead) return 0;
    if (!ws_frame(w, text ? 1 : 2, data, len)) { w->dead = 1; return 0; }
    return 1;
}

int NetWsRecv(NetWs *w, void *buf, int max, int ms, int *text)
{
    const DWORD t0 = GetTickCount();
    if (!w || w->dead) return -1;
    for (;;)
    {
        int r, left;
        // A whole frame in w->in?
        if (w->inLen >= 2)
        {
            const int opcode = w->in[0] & 0x0F, fin = w->in[0] & 0x80, masked = w->in[1] & 0x80;
            int len = w->in[1] & 0x7F, head = 2;
            if (len == 126)
            {
                if (w->inLen < 4) len = -1;
                else { len = (w->in[2] << 8) | w->in[3]; head = 4; }
            }
            else if (len == 127) { w->dead = 1; return -1; }   // 64-bit lengths: never ours
            if (len >= 0 && (masked || !fin || head + len > NET_WS_BUF)) { w->dead = 1; return -1; }
            if (len >= 0 && w->inLen >= head + len)
            {
                const u8 *payload = w->in + head;
                int out = -2;
                if (opcode == 8) { w->dead = 1; return -1; }   // the server closed
                if (opcode == 9) ws_frame(w, 10, payload, len < 125 ? len : 125);   // ping: pong
                else if (opcode == 1 || opcode == 2)
                {
                    out = len < max ? len : max;
                    memcpy(buf, payload, out);
                    if (text) *text = opcode == 1;
                }
                memmove(w->in, w->in + head + len, w->inLen - head - len);
                w->inLen -= head + len;
                if (out >= 0) return out > 0 ? out : 0;
                continue;
            }
        }
        left = ms - (int)(GetTickCount() - t0);
        r = ws_pump(w, left > 0 ? left : 0);
        if (r < 0) { w->dead = 1; return -1; }
        if (r > 0) { ws_take(w); continue; }
        return 0;
    }
}
