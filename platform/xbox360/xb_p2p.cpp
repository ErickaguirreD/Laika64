// LAIKA 64 - online play: consoles talking directly (see xb_p2p.h).

#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_log.h"
#include "xb_net.h"
#include "xb_p2p.h"

extern "C" VOID XeCryptRandom(PBYTE pb, DWORD cb);   // xboxkrnl export

#define SO_MARKINSECURE 0x5801      // RGH homebrew: this socket outside Xbox Live (as xb_net.cpp)
#define P2P_MAGIC 0x4C363450u       // "L64P"
#define P2P_STUN_HOST "stun.cloudflare.com"
#define P2P_STUN_PORT 3478
#define P2P_MSGS 12                 // messages in one packet at most
#define P2P_QUEUE 512               // messages received and not read yet

// A packet: the header, then `count` messages of 16 bytes.
struct P2pHead { u32 magic, token; u8 port, kind, heard, count; };   // kind 0: probe, 1: messages
enum { P2P_PROBE = 0, P2P_DATA };

struct P2pPeerAddr
{
    sockaddr_in cand[2];   // public, local (sin_port 0: none)
    sockaddr_in addr;      // where its packets come from (the way that works), once heard
    int heard;             // its packets arrive
    int hearsMe;           // its packets say mine arrive
    DWORD lastHeard;
};

static SOCKET s_sock = INVALID_SOCKET;
static u32 s_token;
static int s_myPort;
static P2pPeerAddr s_peer[P2P_PLAYERS + 1];
static u8 s_queue[P2P_QUEUE][17];   // port, then the message
static int s_qHead, s_qTail;
static DWORD s_probeAt;

static int addr_parse(const char *text, sockaddr_in *out)
{
    char ip[24];
    const char *colon = strchr(text, ':');
    memset(out, 0, sizeof(*out));
    if (!colon || colon - text >= (int)sizeof(ip)) return 0;
    memcpy(ip, text, colon - text);
    ip[colon - text] = 0;
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = inet_addr(ip);
    out->sin_port = htons((u_short)atoi(colon + 1));
    return out->sin_addr.s_addr != INADDR_NONE && out->sin_port != 0;
}

static void addr_text(const sockaddr_in *a, char *out, size_t size)
{
    const u8 *ip = (const u8 *)&a->sin_addr;
    _snprintf(out, size - 1, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], (unsigned)ntohs(a->sin_port));
    out[size - 1] = 0;
}

// The address the internet sees this socket at (STUN Binding, RFC 5389's XOR-MAPPED-ADDRESS): 1 when answered.
static int stun_ask(sockaddr_in *mapped)
{
    u8 req[20], buf[512];
    sockaddr_in sa;
    unsigned long ip = 0;
    DWORD t0, sentAt = 0;
    memset(req, 0, sizeof(req));
    req[1] = 0x01;                                       // Binding Request, no attributes
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;   // the magic cookie
    XeCryptRandom(req + 8, 12);                          // the transaction id
    if (!NetResolve(P2P_STUN_HOST, &ip)) return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(P2P_STUN_PORT);
    sa.sin_addr.s_addr = ip;
    for (t0 = GetTickCount(); GetTickCount() - t0 < 1500;)
    {
        sockaddr_in from;
        int fl = sizeof(from), n;
        if (GetTickCount() - sentAt >= 400) { sendto(s_sock, (const char *)req, sizeof(req), 0, (const sockaddr *)&sa, sizeof(sa)); sentAt = GetTickCount(); }
        n = recvfrom(s_sock, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fl);
        if (n < 20) { Sleep(10); continue; }
        if (buf[0] != 0x01 || buf[1] != 0x01 || memcmp(buf + 4, req + 4, 16)) continue;   // a Binding Success for this request
        {
            int at = 20, len = (buf[2] << 8) | buf[3];
            if (len > n - 20) len = n - 20;
            while (at + 4 <= 20 + len)
            {
                const int type = (buf[at] << 8) | buf[at + 1], alen = (buf[at + 2] << 8) | buf[at + 3];
                const u8 *v = buf + at + 4;
                if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01)   // XOR-MAPPED-ADDRESS (or MAPPED-ADDRESS), IPv4
                {
                    const int x = type == 0x0020;
                    u8 *ipb = (u8 *)&mapped->sin_addr;
                    memset(mapped, 0, sizeof(*mapped));
                    mapped->sin_family = AF_INET;
                    mapped->sin_port = htons((u_short)((((v[2] << 8) | v[3]) ^ (x ? 0x2112 : 0)) & 0xFFFF));
                    ipb[0] = (u8)(v[4] ^ (x ? 0x21 : 0)); ipb[1] = (u8)(v[5] ^ (x ? 0x12 : 0));
                    ipb[2] = (u8)(v[6] ^ (x ? 0xA4 : 0)); ipb[3] = (u8)(v[7] ^ (x ? 0x42 : 0));
                    return 1;
                }
                at += 4 + ((alen + 3) & ~3);
            }
        }
    }
    return 0;
}

int P2pOpen(u32 token, int myPort, char *cand, size_t size)
{
    sockaddr_in sa, mapped, local;
    BOOL one = TRUE;
    u_long nb = 1;
    XNADDR xna;
    char pub[32] = "-", lan[32];
    int fl = sizeof(local);
    P2pClose();
    if (myPort < 1 || myPort > P2P_PLAYERS || !NetStart(0)) return 0;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) { H64_INFO("[p2p] no UDP socket (%d)", WSAGetLastError()); return 0; }
    setsockopt(s_sock, SOL_SOCKET, SO_MARKINSECURE, (const char *)&one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    if (bind(s_sock, (const sockaddr *)&sa, sizeof(sa)) != 0) { H64_INFO("[p2p] bind failed (%d)", WSAGetLastError()); P2pClose(); return 0; }
    ioctlsocket(s_sock, FIONBIO, &nb);
    s_token = token;
    s_myPort = myPort;
    memset(s_peer, 0, sizeof(s_peer));
    s_qHead = s_qTail = 0;
    s_probeAt = 0;
    // The local address: the console's own on the network, the socket's port.
    memset(&local, 0, sizeof(local));
    getsockname(s_sock, (sockaddr *)&local, &fl);
    while (XNetGetTitleXnAddr(&xna) == XNET_GET_XNADDR_PENDING) Sleep(10);
    local.sin_addr = xna.ina;
    addr_text(&local, lan, sizeof(lan));
    if (stun_ask(&mapped)) addr_text(&mapped, pub, sizeof(pub));
    _snprintf(cand, size - 1, "%s %s", pub, lan);
    cand[size - 1] = 0;
    H64_INFO("[p2p] port %d: public %s, local %s", myPort, pub, lan);
    return 1;
}

void P2pPeer(int port, const char *cand)
{
    char pub[32] = "", lan[32] = "";
    if (port < 1 || port > P2P_PLAYERS || port == s_myPort) return;
    sscanf(cand, "%31s %31s", pub, lan);
    addr_parse(pub, &s_peer[port].cand[0]);
    addr_parse(lan, &s_peer[port].cand[1]);
    H64_INFO("[p2p] port %d is at %s (local %s)", port, pub, lan);
}

static u8 heard_mask(void)
{
    int p;
    u8 m = 0;
    for (p = 1; p <= P2P_PLAYERS; p++)
        if (s_peer[p].heard) m |= (u8)(1u << p);
    return m;
}

static void send_to(int port, const void *pkt, int len)
{
    P2pPeerAddr *pe = &s_peer[port];
    int k;
    if (pe->heard) { sendto(s_sock, (const char *)pkt, len, 0, (const sockaddr *)&pe->addr, sizeof(pe->addr)); return; }
    for (k = 0; k < 2; k++)   // not heard yet: every address it gave
        if (pe->cand[k].sin_port) sendto(s_sock, (const char *)pkt, len, 0, (const sockaddr *)&pe->cand[k], sizeof(pe->cand[k]));
}

// The packets waiting on the socket: their messages queued, the senders' addresses learned.
static void read_packets(void)
{
    u8 buf[sizeof(P2pHead) + P2P_MSGS * 16 + 16];
    for (;;)
    {
        sockaddr_in from;
        int fl = sizeof(from), n = recvfrom(s_sock, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fl), k;
        P2pHead h;
        P2pPeerAddr *pe;
        if (n < (int)sizeof(P2pHead)) { if (n < 0) return; continue; }
        memcpy(&h, buf, sizeof(h));
        if (h.magic != P2P_MAGIC || h.token != s_token || h.port < 1 || h.port > P2P_PLAYERS || h.port == s_myPort) continue;
        pe = &s_peer[h.port];
        pe->addr = from;   // the way its packets come is the way back
        pe->heard = 1;
        pe->hearsMe = (h.heard >> s_myPort) & 1;
        pe->lastHeard = GetTickCount();
        if (h.kind != P2P_DATA) continue;
        for (k = 0; k < h.count && (int)sizeof(P2pHead) + (k + 1) * 16 <= n; k++)
        {
            const int next = (s_qTail + 1) % P2P_QUEUE;
            if (next == s_qHead) break;   // full: the rest is lost (the next packets repeat the inputs)
            s_queue[s_qTail][0] = h.port;
            memcpy(s_queue[s_qTail] + 1, buf + sizeof(P2pHead) + k * 16, 16);
            s_qTail = next;
        }
    }
}

int P2pPunch(int ms)
{
    const DWORD t0 = GetTickCount();
    int p, mask = 0;
    if (s_sock == INVALID_SOCKET) return 0;
    do
    {
        if (GetTickCount() - s_probeAt >= 100)
        {
            P2pHead h;
            h.magic = P2P_MAGIC;
            h.token = s_token;
            h.port = (u8)s_myPort;
            h.kind = P2P_PROBE;
            h.heard = heard_mask();
            h.count = 0;
            for (p = 1; p <= P2P_PLAYERS; p++)
                if (p != s_myPort && (s_peer[p].cand[0].sin_port || s_peer[p].cand[1].sin_port || s_peer[p].heard)) send_to(p, &h, sizeof(h));
            s_probeAt = GetTickCount();
        }
        read_packets();
        if (GetTickCount() - t0 < (DWORD)ms) Sleep(5);
    } while (GetTickCount() - t0 < (DWORD)ms);
    for (p = 1; p <= P2P_PLAYERS; p++)
        if (P2pReached(p)) mask |= 1 << p;
    return mask;
}

int P2pReached(int port) { return port >= 1 && port <= P2P_PLAYERS && s_peer[port].heard && s_peer[port].hearsMe; }

void P2pSend(const void *msgs, int count)
{
    u8 pkt[sizeof(P2pHead) + P2P_MSGS * 16];
    P2pHead h;
    int p;
    if (s_sock == INVALID_SOCKET) return;
    if (count > P2P_MSGS) count = P2P_MSGS;
    h.magic = P2P_MAGIC;
    h.token = s_token;
    h.port = (u8)s_myPort;
    h.kind = P2P_DATA;
    h.heard = heard_mask();
    h.count = (u8)count;
    memcpy(pkt, &h, sizeof(h));
    memcpy(pkt + sizeof(h), msgs, count * 16);
    for (p = 1; p <= P2P_PLAYERS; p++)
        if (p != s_myPort && s_peer[p].heard) send_to(p, pkt, sizeof(h) + count * 16);
}

int P2pRecv(void *msg, int *port)
{
    if (s_sock == INVALID_SOCKET) return 0;
    if (s_qHead == s_qTail) read_packets();
    if (s_qHead == s_qTail) return 0;
    *port = s_queue[s_qHead][0];
    memcpy(msg, s_queue[s_qHead] + 1, 16);
    s_qHead = (s_qHead + 1) % P2P_QUEUE;
    return 1;
}

u32 P2pLastHeard(int port) { return port >= 1 && port <= P2P_PLAYERS ? s_peer[port].lastHeard : 0; }

void P2pClose(void)
{
    if (s_sock != INVALID_SOCKET) closesocket(s_sock);
    s_sock = INVALID_SOCKET;
    memset(s_peer, 0, sizeof(s_peer));
    s_qHead = s_qTail = 0;
}
