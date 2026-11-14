// LAIKA 64 - online play: the player's profile and the server's lists (Settings > Online play).
//
// The server (tools/netrelay/worker, on Cloudflare Workers) keeps each
// player's gamertag, country and avatar, who is online, friends, the minutes
// played per game and the ranking made of them, and pairs two consoles with
// a room code for a game (the netplay itself is main.cpp's). The console
// keeps its gamertag and the token the server gave it in
// <drive>:\config\online.txt; a line server=host there names another server
// than LAIKA 64's own (tests).
//
// The requests are HTTPS GETs (xb_net.cpp) and block for a moment
// (connection, TLS, answer): the screens show "Connecting..." first. A thread
// says "still here" every two minutes, with the game being played, and sends
// the minutes of the games played. A room is a WebSocket to the server, which
// passes each console's messages to the other.
#ifndef XB_ONLINE_H
#define XB_ONLINE_H

#include <string>
#include <vector>
#include "../../core/common/h64_types.h"

#define ONLINE_TAG_MIN 3
#define ONLINE_TAG_MAX 15
#define ONLINE_AVATARS 64   // avatar numbers the server keeps (0-63); xb_avatars.h has the pictures of the first ones

struct OnlineProfile
{
    char server[64];    // the server's host
    char tag[ONLINE_TAG_MAX + 1], token[40], country[4];   // "" until registered; country: "MX"
    int avatar;
};

// Reads <drive>:\config\online.txt and starts the thread (once, at start).
void OnlineInit(const char *drive);
const OnlineProfile *OnlineMe(void);
int OnlineRegistered(void);   // a gamertag and its token
int OnlineHasServer(void);

// Why the last request failed: the server's word ("taken", "name", "auth", "busy"...), "net" when it
// could not be reached, "" after one that worked.
const char *OnlineError(void);

// ---- Requests (1 when the server said OK) ----
int OnlineRegister(const char *tag, const char *country, int avatar);   // kept in online.txt
int OnlineSetProfile(const char *country, int avatar);
int OnlineRename(const char *tag);                // another gamertag (OnlineError "taken", "name")
int OnlineWhereAmI(char *country, size_t size);   // the country the server sees this console in ("MX")

struct OnlineUser
{
    char name[ONLINE_TAG_MAX + 1], country[4], status[44];
    int avatar, online, isFriend;
    int pos, minutes;   // the ranking's
};
enum { ONLINE_LIST_USERS = 0, ONLINE_LIST_FRIENDS, ONLINE_LIST_RANK };
int OnlineList(int kind, int offset, std::vector<OnlineUser> *out);   // 50 at a time from `offset` (friends: all)
// add: asks for a friendship (*how "sent", or "accepted" when the other had asked too), else ends one.
int OnlineFriend(const char *name, int add, char *how = NULL, size_t howSize = 0);
int OnlineRequests(std::vector<OnlineUser> *out);   // the friendships asked of this player
int OnlineAnswer(const char *name, int yes);

// ---- Messages between friends ----
struct OnlineDmLine { int id, mine; std::string text; };
int OnlineDm(const char *to, const char *text);   // OnlineError "busy": too soon after the last one
int OnlineConv(const char *with, int since, std::vector<OnlineDmLine> *out);   // the messages with a friend after `since`
int OnlineUnread(const char *name);   // messages of that friend not read yet ("" : all of them)
void OnlineUnreadClear(const char *name);

struct OnlineGameTime { std::string game; int minutes; };
struct OnlineStats { int minutes, games, rank; std::vector<OnlineGameTime> list; };
int OnlineStatsOf(const char *name, OnlineStats *out);

struct OnlineNews { std::string date, title, text; };
int OnlineNewsGet(const char *lang, std::vector<OnlineNews> *out);

// ---- The thread's ----
void OnlineSetStatus(const char *game);   // the game being played ("": in the menus), told at the next ping
void OnlinePlayed(const char *code, const char *game, int minutes);   // a game just left: sent soon
// The next notice of the player's news (a friend came online, a friendship asked, a message): 1 with its
// text in `out`, 0 when there is none. The ROM list and the game show them.
int OnlineNoticeTake(char *out, size_t size);

// ---- Public rooms and the general chat ----
#define ONLINE_ROOM_NAME 24
#define ONLINE_ROOM_PASS 12
struct OnlineRoomInfo { unsigned code; char host[ONLINE_TAG_MAX + 1]; int avatar, players, locked; char game[44], name[ONLINE_ROOM_NAME + 4]; };
int OnlineRooms(std::vector<OnlineRoomInfo> *out);   // the rooms waiting for players (locked: a password is asked)
struct OnlineChatLine { int id; char tag[ONLINE_TAG_MAX + 1]; int avatar; std::string text; char when[8]; };   // when: "HH:MM", this console's clock
int OnlineChatGet(int since, std::vector<OnlineChatLine> *out);   // the general chat after message `since` (0: the last ones)
int OnlineChatSay(const char *text);                              // OnlineError "busy": too soon after the last one
// The rooms and the general chat after `since` in one request (the Play online screen's).
int OnlineLobbyGet(int since, std::vector<OnlineRoomInfo> *rooms, std::vector<OnlineChatLine> *chat);

// ---- A game in a room (the server's): up to four consoles ----
// Create: a room for a game, its name (and a password: "" for none) - listed in OnlineRooms, a password
// one as locked - and its code. Join: the room of `code` (with its password when locked). Then
// OnlineRoomPoll each frame (it also measures the time to the server): 0 nothing new, 1 the lobby changed
// (players, chat, their times, the countdown), 2 the game starts (the lobby's startPort and startPlayers),
// -1 the room is over (OnlineError: "none" no such room, "full", "started", "closed" the host left,
// "expired", "busy", "old" another LAIKA version, "auth", "password", "kicked" the host put it out, "net").
// main.cpp takes the connection for the game (OnlineRoomTake: NULL when there is none; it closes it with
// NetWsClose).
#define ONLINE_ROOM_PLAYERS 4
#define ONLINE_ROOM_CHAT 8   // chat lines kept
struct NetWs;
struct OnlineLobby
{
    unsigned code;
    int count, mine;                                  // players, the host first; this console's place
    char tag[ONLINE_ROOM_PLAYERS][ONLINE_TAG_MAX + 1];
    int avatar[ONLINE_ROOM_PLAYERS];
    int rtt[ONLINE_ROOM_PLAYERS];                     // each one's time to the server and back, ms (0: not known)
    int myRtt;                                        // this console's (measured here)
    int countdown;                                    // the host started: the game in this many seconds from countAt (0: not)
    unsigned countAt;                                 // GetTickCount of the countdown's start
    char name[ONLINE_ROOM_NAME + 4];                  // the room's name
    char game[44];                                    // the room's game: its name and header CRCs
    u32 crc1, crc2;
    int haveGame;
    int startPlayers, startPort;
    std::vector<OnlineChatLine> chat;                 // the room's last messages, the newest last
};
int OnlineRoomCreate(const char *name, const char *password, const char *game, u32 crc1, u32 crc2, unsigned *code);
int OnlineRoomJoin(unsigned code, const char *password);
int OnlineRoomPoll(void);
const OnlineLobby *OnlineRoomLobby(void);
int OnlineRoomSay(const char *text);
int OnlineRoomStart(void);   // the host only, with two players at least (a countdown, then the game)
int OnlineRoomKick(int place);   // the host only, before the game: that player out of the room (it may come back)
void OnlineRoomClose(void);
NetWs *OnlineRoomTake(int *port, int *players);

#endif
