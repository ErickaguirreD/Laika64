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
#define ONLINE_AVATAR_CUSTOM 1000   // custom avatars (drawn for one player each, xb_avatar_custom.h): from here
#define ONLINE_AVATARS 1024  // avatar numbers the server keeps (0-1023): xb_avatars.h's (63 the team's), xb_avatars2.h's from 64,
                             // the animated ones (xb_avatars3.h) from 256

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
    int inRoom;         // in a room's lobby (Play online's players)
    int ms, conn;       // its time to the server and back (0: not known); its connection (ONLINE_CONN_*)
    int playSecs;       // playing what `status` says for this long, seconds (0: not known - an older server)
};
enum { ONLINE_CONN_NONE = 0, ONLINE_CONN_CABLE, ONLINE_CONN_WIFI, ONLINE_CONN_OLD };   // OLD: a LAIKA before 2.1 (tells neither)
enum { ONLINE_LIST_USERS = 0, ONLINE_LIST_FRIENDS, ONLINE_LIST_RANK, ONLINE_LIST_WEEK };   // WEEK: this week's ranking online
int OnlineList(int kind, int offset, std::vector<OnlineUser> *out);   // 50 at a time from `offset` (friends: all)
// add: asks for a friendship (*how "sent", or "accepted" when the other had asked too), else ends one.
int OnlineFriend(const char *name, int add, char *how = NULL, size_t howSize = 0);
// This week's ranking by time played online (from Monday 00:00 UTC): the top 10, this player's place and time.
struct OnlineWeekRow { int pos; char name[ONLINE_TAG_MAX + 1]; char country[4]; int avatar, seconds; };
struct OnlineWeek { int daysLeft, myPos, mySeconds; std::vector<OnlineWeekRow> top; };
int OnlineWeekGet(OnlineWeek *out);
int OnlineRequests(std::vector<OnlineUser> *out);   // the friendships asked of this player
int OnlineAnswer(const char *name, int yes);
// An invitation to the room `code` (waiting) for that player: told at once (*live 1) or when its console next
// connects (*live 0). OnlineError "busy": invited a moment ago; "none": the room is not listed (yet).
int OnlineInvite(const char *name, unsigned code, int *live);
// The room of the last invitation received in the last ten minutes (Play online selects it), 0: none.
unsigned OnlineInvitedTo(void);

// ---- Messages between friends ----
struct OnlineDmLine { int id, mine; std::string text; char when[8]; int daysAgo; int state; };   // state (own ones): 0 sent, 1 delivered, 2 seen, -1 not known   // when "HH:MM" ("" not known), daysAgo -1 not known
int OnlineDm(const char *to, const char *text);   // OnlineError "busy": too soon after the last one
int OnlineConv(const char *with, int since, std::vector<OnlineDmLine> *out);   // the messages with a friend after `since`
int OnlineUnread(const char *name);   // messages of that friend not read yet ("" : all of them)
void OnlineUnreadClear(const char *name);

struct OnlineGameTime { std::string game, code; int minutes; };   // code "NALE / 1234ABCD" (ArtKeyFromCode: its box art), "" from an old server
// A player's profile: the time, games and place; whether friends (and for how many days, -1 not known), the games
// played together; avatar, country, and online: what it does, its time to the server and connection, in a room.
struct OnlineStats
{
    int minutes, games, rank, isFriend, friendDays, together, avatar, online, ms, conn, inRoom;
    char country[4], status[48];
    std::vector<OnlineGameTime> list;
};
int OnlineStatsOf(const char *name, OnlineStats *out);

struct OnlineNews { std::string date, title, text; };
int OnlineNewsGet(const char *lang, std::vector<OnlineNews> *out);
// The most played games online (/v1/top): period 'w' this week, 'm' the last four weeks, 'a' all time. Each: its place,
// its code ("NALE / 34EE4749": its box art), its name (a hack's with its version), minutes, players, its place a week
// before (0: none, this week's only) and this player's own minutes with it.
struct OnlineTopGame { int place, minutes, players, last, mine; std::string code, game; };
int OnlineTopGames(char period, std::vector<OnlineTopGame> *out);

// ---- The thread's ----
void OnlineSetStatus(const char *game);   // the game being played ("": in the menus), told at the next ping
int OnlineMyMs(void);                     // this console's time to the server and back, ms (0: not measured yet)
// The players online read by the thread every few seconds while watched (the pause menu's page, which
// must not wait for the server): OnlineWatchedUsers gives the last list read - 1, 0 none read yet, -1 none
// could be read.
void OnlineWatchUsers(int on);
int OnlineWatchedUsers(std::vector<OnlineUser> *out);
int OnlineMyConn(void);                   // ONLINE_CONN_*
void OnlinePlayed(const char *code, const char *game, int minutes);   // a game just left: sent soon
// The next notice of the player's news (a friend came online, a friendship asked, a message, an invitation
// to play - this one at once, through a connection kept open to the server): 1 with its text in `out`, 0 when
// there is none; *invite (may be NULL) 1 for an invitation (shown longer). The ROM list and the game show them.
// What a notice is about (OnlineNoticeTake's *kind): the screens draw each with its own symbol.
enum { ONLINE_NOTE_OTHER = 0, ONLINE_NOTE_ROOM, ONLINE_NOTE_FRIEND, ONLINE_NOTE_REQUEST, ONLINE_NOTE_DM, ONLINE_NOTE_INVITE, ONLINE_NOTE_UPDATE };
// The newer LAIKA 64 the server told of (ONLINE_NOTE_UPDATE's), "" none.
const char *OnlineNewerRelease(void);
int OnlineNoticeTake(char *out, size_t size, int *invite = NULL, int *kind = NULL);
// Which news become notices (Config::notify's bits: 1 a room opened by another player, 2 a friend online, 4 a message;
// 16 a newer LAIKA 64 - Settings' update check; invitations and friendship requests always). With 1 the online thread looks at the rooms every ONLINE_ROOMS_LOOK_MS.
#define ONLINE_ROOMS_LOOK_MS 20000
void OnlineSetNotify(int bits);

// ---- Public rooms and the general chat ----
#define ONLINE_ROOM_NAME 24
#define ONLINE_ROOM_PASS 12
// max: the players it is for (2-4); hostMs: the host's time to the server (0: not known); art: its game's box art key
// (xb_art.cpp ArtKeyOf: "" when not known).
struct OnlineRoomInfo { unsigned code; char host[ONLINE_TAG_MAX + 1]; int avatar, players, locked, max, hostMs; char game[44], name[ONLINE_ROOM_NAME + 4], art[16];
                        u32 crc1, crc2; int hasCrc; };   // crc: its ROM's header CRCs (hasCrc 0: a server without them)
int OnlineRooms(std::vector<OnlineRoomInfo> *out);   // the rooms waiting for players (locked: a password is asked)
// A game being played in a room: who plays it, for how long.
struct OnlineGameInfo { unsigned code; char game[44], art[16]; int seconds, count; char tag[4][ONLINE_TAG_MAX + 1]; };
// when: "HH:MM", this console's clock; daysAgo: its day (0 today, 1 yesterday...).
struct OnlineChatLine { int id; char tag[ONLINE_TAG_MAX + 1]; int avatar; std::string text; char when[8]; int daysAgo;
                        OnlineChatLine() : id(0), avatar(0), daysAgo(0) { tag[0] = 0; when[0] = 0; } };
int OnlineChatGet(int since, std::vector<OnlineChatLine> *out);   // the general chat after message `since` (0: the last ones)
// The general chat's messages before message `before`, the newest last (40 at most; none: no older ones kept).
int OnlineChatBefore(int before, std::vector<OnlineChatLine> *out);
int OnlineChatSay(const char *text);                              // OnlineError "busy": too soon after the last one
// The rooms waiting, the games being played, the players online and the general chat after `since` in one
// request (the Play online screen's).
int OnlineLobbyGet(int since, std::vector<OnlineRoomInfo> *rooms, std::vector<OnlineGameInfo> *games, std::vector<OnlineUser> *users,
                   std::vector<OnlineChatLine> *chat);
// The same read again and again by the online thread while a screen shows it (the online menu's, Play online's):
// every ONLINE_LOBBY_MS, at once after OnlineLobbyRefresh. Read on the screen's own thread it held the screen each
// time (a stutter every 8 s). Watch(1, since): from the chat's message `since`; Take: 1 with a new read (the chat's
// lines after the last ones taken), 0 none yet.
#define ONLINE_LOBBY_MS 8000
// Nobody at the controllers: the lobby and the players read every ONLINE_LOBBY_SLOW_MS after ONLINE_IDLE_SLOW_MS without
// a press, not at all after ONLINE_IDLE_STOP_MS (a console left on Play online read the server all day: the free plan's
// daily reads ran out, 2026-11-20); a press reads again at once.
#define ONLINE_IDLE_SLOW_MS 120000
#define ONLINE_IDLE_STOP_MS 600000
#define ONLINE_LOBBY_SLOW_MS 60000
// The server as the last "still here" found it (no request of its own: that one is timed, its answer read): reached
// and quick, slow, not answering (an answer that is no answer, or "ERR quota": the free plan's daily reads used up -
// back at midnight UTC, `backAt` that hour and minute here, -1 not known), or nothing reached at all (no internet).
enum { ONLINE_SRV_UNKNOWN = 0, ONLINE_SRV_OK, ONLINE_SRV_SLOW, ONLINE_SRV_DOWN, ONLINE_SRV_NONET };
#define ONLINE_SRV_SLOW_MS 600
int OnlineServerState(int *ms, int *backAt);
// Internet, as Google's generate_204 answered (LAIKA's start, and again when the server is not reached): 0 still
// checking, 1 there, 2 not (no answer from Google either).
enum { ONLINE_NET_CHECKING = 0, ONLINE_NET_YES, ONLINE_NET_NO };
int OnlineInternetState(void);
// The friends online as the last "still here" told (name and avatar): no request of its own.
struct OnlineFriendOn { char name[ONLINE_TAG_MAX + 1]; int avatar; };
int OnlineFriendsOnline(OnlineFriendOn *out, int max);
DWORD OnlineIdleMs(void);        // since the last press on any controller (the online thread watches them)
DWORD OnlineLobbyAgeMs(void);    // since the lobby was last read (0: not yet)
void OnlineLobbyWatch(int on, int since);
void OnlineLobbyRefresh(void);
int OnlineLobbyTake(std::vector<OnlineRoomInfo> *rooms, std::vector<OnlineGameInfo> *games, std::vector<OnlineUser> *users,
                    std::vector<OnlineChatLine> *chat);

// ---- A game in a room (the server's): up to four consoles ----
// Create: a room for a game, its name (and a password: "" for none) - listed in OnlineRooms, a password
// one as locked - and its code. Join: the room of `code` (with its password when locked). Then
// OnlineRoomPoll each frame (it also measures the time to the server): 0 nothing new, 1 the lobby changed
// (players, chat, their times, the countdown), 2 the game starts (the lobby's startPort and startPlayers),
// -1 the room is over (OnlineError: "none" no such room, "full", "started", "closed" the host left,
// "expired", "busy", "old" another LAIKA version, "auth", "password", "kicked" the host put it out, "net").
// main.cpp takes the connection for the game (OnlineRoomTake: NULL when there is none; it closes it with
// NetWsClose) - or gives it back (OnlineRoomGiveBack) when the player goes back to the room's lobby: the
// room stays, with the same players, and the ROM list opens its lobby again (OnlineRoomBack).
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
    char country[ONLINE_ROOM_PLAYERS][4];             // each one's country ("MX"; "--": not known)
    int conn[ONLINE_ROOM_PLAYERS];                    // ... and connection (ONLINE_CONN_*)
    int local[ONLINE_ROOM_PLAYERS];                   // ... and how many play on it (1-3)
    int players;                                      // all of them (the room's places they take)
    int myRtt;                                        // this console's (measured here)
    int countdown;                                    // the host started: the game in this many seconds from countAt (0: not)
    unsigned countAt;                                 // GetTickCount of the countdown's start
    char name[ONLINE_ROOM_NAME + 4];                  // the room's name
    char game[44];                                    // the room's game: its name and header CRCs
    u32 crc1, crc2;
    int haveGame;
    int max;                                          // the players it is for (2-4)
    int startPlayers, startPort, startLocal;
    unsigned char startOwner[ONLINE_ROOM_PLAYERS + 1];   // each port's console (its first port)
    std::vector<OnlineChatLine> chat;                 // the room's last messages, the newest last
    std::vector<std::string> ready;                   // the consoles ready (their tags: "@r1"/"@r0" in the chat, not shown)
};
int OnlineRoomCreate(const char *name, const char *password, const char *game, u32 crc1, u32 crc2, const char *art, int max, unsigned *code);
int OnlineRoomJoin(unsigned code, const char *password);
int OnlineRoomPoll(void);
const OnlineLobby *OnlineRoomLobby(void);
int OnlineRoomSay(const char *text);
int OnlineRoomStart(void);   // the host only, with two players at least (a countdown, then the game)
int OnlineRoomKick(int place);   // the host only, before the game: that player out of the room (it may come back)
int OnlineRoomNewPlayer(char *tag, size_t size);   // 1 once after a player came into the room, its gamertag in `tag` (the lobby's alert)
void OnlineRoomClose(void);
void OnlineRoomLeave(const char *why);   // the room closed saying why first ("nogame": no such ROM here), told to the others
int OnlineRoomLocal(int count);   // how many play on this console (1-3), before the game: refused when the room has no places
NetWs *OnlineRoomTake(int *port, int *players, int *local, unsigned char *owner);   // owner: ONLINE_ROOM_PLAYERS + 1 entries
void OnlineRoomGiveBack(NetWs *w);   // the game's connection back: "back" said, the room's lobby again
int OnlineRoomBack(void);            // a room given back and still open (its lobby to show)
// The game's room again, its connection lost while the consoles play on directly: tried on a thread of its own
// (OnlineRoomRejoin: 1 when a try started; who: the consoles in the game, "1:TAG,3:TAG2"). OnlineRoomRejoined
// then: the connection once back (*state 1), else NULL with *state 0 (still trying, or none), -1 (failed this
// time) or -2 (the server has no such game any more). OnlineRoomRejoinCancel when the game is over.
int OnlineRoomRejoin(int port, int local, int players, const char *who);
NetWs *OnlineRoomRejoined(int *state);
void OnlineRoomRejoinCancel(void);

#endif
