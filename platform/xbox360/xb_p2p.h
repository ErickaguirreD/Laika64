// LAIKA 64 - online play: consoles talking directly (UDP), so a room's game does not need the server's relay.
//
// Each console opens a UDP socket, asks a STUN server (stun.cloudflare.com) the address the internet sees
// it at, and tells the others that one and its own on the local network through the room (main.cpp). Then
// all send small probes to each other's addresses at once, which opens the way through most home routers
// ("hole punching"); a way works when each side hears the other. Routers that change the port for every
// address (some CGNAT) do not let it: those games stay on the relay.
//
// The packets carry LAIKA's 16-byte netplay messages (main.cpp NetMsg); the receiver is told the sender's
// port. A packet may carry a few messages (main.cpp repeats the last inputs in each, so a lost packet costs
// nothing). Nothing here blocks except P2pOpen (the STUN question, 1.5 s at most).
#ifndef XB_P2P_H
#define XB_P2P_H

#include <stddef.h>
#include "../../core/common/h64_types.h"

#define P2P_PLAYERS 4

// Opens the socket for this console's port (1..4) in a game whose consoles share `token` (any number they
// all know: stray packets are dropped). `cand` gets this console's addresses to tell the others:
// "<public ip:port> <local ip:port>" ("-" for the public one when STUN did not answer). 1 when open.
int P2pOpen(u32 token, int myPort, char *cand, size_t size);
// Another console's addresses (its `cand` from P2pOpen, through the room).
void P2pPeer(int port, const char *cand);
// Probes sent (every 100 ms) and packets read for up to `ms`: the ports reached both ways (bit p).
int P2pPunch(int ms);
int P2pReached(int port);
// Messages (16 bytes each, `count` of them) to every console reached.
void P2pSend(const void *msgs, int count);
// One message received: 1 with it in `msg` (16 bytes) and its sender's port in *port, 0 when none waits.
int P2pRecv(void *msg, int *port);
// GetTickCount of the last packet from that port (0: never).
u32 P2pLastHeard(int port);
void P2pClose(void);

#endif
