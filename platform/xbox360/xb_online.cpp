// LAIKA 64 - online play: the player's profile and the server's lists (see xb_online.h).

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_log.h"
#include "xb_net.h"
#include "xb_online.h"
#include "xb_lang.h"
#include <map>

#define ONLINE_SERVER "laika64.laikaemu.workers.dev"   // LAIKA 64's own (online.txt's server= names another)
#define ONLINE_ANSWER_MS 8000
#define ONLINE_PING_MS 90000        // the server calls a player online for 5 minutes after one (the news come with it)
#define ONLINE_PROTO 2              // main.cpp NET_PROTO

static OnlineProfile s_me;
static char s_file[96];             // <drive>:\config\online.txt
static int s_serverSet;             // online.txt names the server (kept when the file is written again)
static char s_error[24];
static CRITICAL_SECTION s_lock;     // s_status, s_played, the news below
static std::vector<std::string> s_notices;          // the news not shown yet
static std::map<std::string, int> s_unread;         // per friend: messages not read yet
static std::map<std::string, int> s_friendOnline;   // per friend: online at the last ping
static std::vector<std::string> s_askedSeen;        // friendships asked already told
static int s_dmSeen;                                // the last message told
static char s_status[44];
struct Played { char code[24], game[44]; int minutes; };
static std::vector<Played> s_played;
static HANDLE s_thread;

const OnlineProfile *OnlineMe(void) { return &s_me; }
int OnlineRegistered(void) { return s_me.tag[0] && s_me.token[0]; }
int OnlineHasServer(void) { return s_me.server[0] != 0; }
const char *OnlineError(void) { return s_error; }

// `err`: s_error for the screens' requests (OnlineError), the thread's own for its requests.
static void set_error(char *err, const char *e)
{
    strncpy(err, e, sizeof(s_error) - 1);
    err[sizeof(s_error) - 1] = 0;
}

static void copy(char *out, size_t size, const char *in)
{
    strncpy(out, in, size - 1);
    out[size - 1] = 0;
}

static void profile_write(void)
{
    FILE *f = fopen(s_file, "w");
    if (!f) { H64_INFO("[online] %s NOT written", s_file); return; }
    if (s_serverSet) fprintf(f, "server=%s\n", s_me.server);
    fprintf(f, "gamertag=%s\ntoken=%s\ncountry=%s\navatar=%d\n", s_me.tag, s_me.token, s_me.country, s_me.avatar);
    fclose(f);
}

static void profile_read(void)
{
    char line[160];
    FILE *f = fopen(s_file, "r");
    memset(&s_me, 0, sizeof(s_me));
    strcpy(s_me.server, ONLINE_SERVER);
    s_serverSet = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f))
    {
        char *eq = strchr(line, '=');
        line[strcspn(line, "\r\n")] = 0;
        if (!eq) continue;
        *eq++ = 0;
        // A host name: the address of the first tests (an IP, a port) is not one - LAIKA 64's own then.
        if (!strcmp(line, "server")) { if (strchr(eq, '.') && !strchr(eq, ':') && (eq[0] < '0' || eq[0] > '9')) { copy(s_me.server, sizeof(s_me.server), eq); s_serverSet = 1; } }
        else if (!strcmp(line, "gamertag")) copy(s_me.tag, sizeof(s_me.tag), eq);
        else if (!strcmp(line, "token")) copy(s_me.token, sizeof(s_me.token), eq);
        else if (!strcmp(line, "country")) copy(s_me.country, sizeof(s_me.country), eq);
        else if (!strcmp(line, "avatar")) s_me.avatar = atoi(eq) % ONLINE_AVATARS;
    }
    fclose(f);
}

// ---- Requests: GET https://<server>/v1/<cmd>?key=value&... ----
// "&key=value" added to a query, the value escaped.
static void add(std::string *q, const char *key, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    *q += q->empty() ? "" : "&";
    *q += key;
    *q += '=';
    for (; *value; value++)
    {
        const unsigned char c = (unsigned char)*value;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.') *q += (char)c;
        else { *q += '%'; *q += hex[c >> 4]; *q += hex[c & 15]; }
    }
}

static void add(std::string *q, const char *key, int value)
{
    char t[16];
    _snprintf(t, sizeof(t), "%d", value);
    add(q, key, t);
}

// One request: the answer's data lines in *rows (NULL: not wanted); `first` gets what follows "OK\t"
// on the first line. 1 when the server said OK (else why in `err`).
static int request(const char *cmd, const std::string &query, std::vector<std::string> *rows, std::string *first, char *err)
{
    std::string path = std::string("/v1/") + cmd + "?" + query, all;
    u8 *body = NULL;
    u32 len = 0;
    size_t end, at, nl;
    if (rows) rows->clear();
    set_error(err, "net");
    if (!s_me.server[0] || !NetStart(0)) return 0;
    if (!NetGet(s_me.server, path.c_str(), 0, &body, &len, 256 * 1024, NULL, 0) || !body) { free(body); return 0; }
    all.assign((const char *)body, len);
    free(body);
    // "OK" or "ERR why", the data lines, a line with only ".".
    if (all.size() < 3 || all.compare(all.size() - 3, 3, "\n.\n") != 0) return 0;
    end = all.size() - 2;
    nl = all.find('\n');
    if (all.compare(0, 2, "OK") != 0)
    {
        set_error(err, all.compare(0, 4, "ERR ") == 0 ? all.substr(4, nl - 4).c_str() : "answer");
        return 0;
    }
    if (first) *first = nl > 3 ? all.substr(3, nl - 3) : std::string();
    for (at = nl + 1; rows && at < end; at = nl + 1)
    {
        nl = all.find('\n', at);
        rows->push_back(all.substr(at, nl - at));
    }
    set_error(err, "");
    return 1;
}

// A request of the player's: the gamertag and its token first, then `query`'s own.
static int request_me(const char *cmd, const std::string &query, std::vector<std::string> *rows, char *err = s_error)
{
    std::string q;
    if (!OnlineRegistered()) { set_error(err, "auth"); return 0; }
    add(&q, "g", s_me.tag);
    add(&q, "t", s_me.token);
    if (!query.empty()) q += "&" + query;
    return request(cmd, q, rows, NULL, err);
}

// Field k (from 0) of a tab-separated row; "" past its end.
// A chat message's time on this console's clock ("HH:MM"): `ago` seconds before now (the server counts them,
// so a console whose clock is wrong by hours still shows the right gap).
static void chat_when(char *out, int ago)
{
    SYSTEMTIME st;
    FILETIME ft;
    ULARGE_INTEGER u;
    GetLocalTime(&st);
    SystemTimeToFileTime(&st, &ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)(ago > 0 ? ago : 0) * 10000000ull;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    _snprintf(out, 7, "%02d:%02d", st.wHour, st.wMinute);
    out[7] = 0;
}

static std::string field(const std::string &row, int k)
{
    size_t at = 0, tab;
    while (k-- > 0)
    {
        tab = row.find('\t', at);
        if (tab == std::string::npos) return std::string();
        at = tab + 1;
    }
    tab = row.find('\t', at);
    return row.substr(at, tab == std::string::npos ? tab : tab - at);
}

int OnlineRegister(const char *tag, const char *country, int avatar)
{
    std::string q, token;
    add(&q, "tag", tag);
    add(&q, "country", country);
    add(&q, "avatar", avatar);
    if (!request("register", q, NULL, &token, s_error) || token.empty()) return 0;
    copy(s_me.tag, sizeof(s_me.tag), tag);
    copy(s_me.token, sizeof(s_me.token), token.c_str());
    copy(s_me.country, sizeof(s_me.country), country);
    s_me.avatar = avatar;
    profile_write();
    H64_INFO("[online] gamertag %s registered", s_me.tag);
    return 1;
}

int OnlineRename(const char *tag)
{
    std::string q;
    add(&q, "tag", tag);
    if (!request_me("rename", q, NULL)) return 0;
    copy(s_me.tag, sizeof(s_me.tag), tag);
    profile_write();
    H64_INFO("[online] gamertag now %s", s_me.tag);
    return 1;
}

int OnlineWhereAmI(char *country, size_t size)
{
    std::string cc;
    if (!request("whereami", std::string(), NULL, &cc, s_error) || cc.size() != 2) return 0;
    copy(country, size, cc.c_str());
    return 1;
}

int OnlineSetProfile(const char *country, int avatar)
{
    std::string q;
    add(&q, "country", country);
    add(&q, "avatar", avatar);
    if (!request_me("profile", q, NULL)) return 0;
    copy(s_me.country, sizeof(s_me.country), country);
    s_me.avatar = avatar;
    profile_write();
    return 1;
}

int OnlineList(int kind, int offset, std::vector<OnlineUser> *out)
{
    std::vector<std::string> rows;
    std::string q;
    size_t k;
    out->clear();
    if (kind != ONLINE_LIST_FRIENDS) add(&q, "offset", offset);
    if (!request_me(kind == ONLINE_LIST_FRIENDS ? "friends" : kind == ONLINE_LIST_RANK ? "rank" : "users", q, &rows)) return 0;
    for (k = 0; k < rows.size(); k++)
    {
        // users, friends: name country avatar online status friend. rank: position name country avatar minutes online.
        const int r = kind == ONLINE_LIST_RANK;
        OnlineUser u;
        memset(&u, 0, sizeof(u));
        copy(u.name, sizeof(u.name), field(rows[k], r).c_str());
        copy(u.country, sizeof(u.country), field(rows[k], r + 1).c_str());
        u.avatar = atoi(field(rows[k], r + 2).c_str()) % ONLINE_AVATARS;
        if (r)
        {
            u.pos = atoi(field(rows[k], 0).c_str());
            u.minutes = atoi(field(rows[k], 4).c_str());
            u.online = atoi(field(rows[k], 5).c_str());
        }
        else
        {
            u.online = atoi(field(rows[k], 3).c_str());
            copy(u.status, sizeof(u.status), field(rows[k], 4).c_str());
            u.isFriend = atoi(field(rows[k], 5).c_str());
        }
        if (u.name[0]) out->push_back(u);
    }
    return 1;
}

int OnlineFriend(const char *name, int add_, char *how, size_t howSize)
{
    std::string q, first;
    std::vector<std::string> rows;
    char line[200];
    add(&q, "name", name);
    add(&q, "add", add_ ? 1 : 0);
    if (!OnlineRegistered()) { set_error(s_error, "auth"); return 0; }
    _snprintf(line, sizeof(line) - 1, "g=%s&t=%s&", s_me.tag, s_me.token);
    line[sizeof(line) - 1] = 0;
    if (!request("friend", std::string(line) + q, &rows, &first, s_error)) return 0;
    if (how && howSize) copy(how, howSize, first.c_str());
    return 1;
}

int OnlineRequests(std::vector<OnlineUser> *out)
{
    std::vector<std::string> rows;
    size_t k;
    out->clear();
    if (!request_me("requests", std::string(), &rows)) return 0;
    for (k = 0; k < rows.size(); k++)
    {
        OnlineUser u;
        memset(&u, 0, sizeof(u));
        copy(u.name, sizeof(u.name), field(rows[k], 0).c_str());
        copy(u.country, sizeof(u.country), field(rows[k], 1).c_str());
        u.avatar = atoi(field(rows[k], 2).c_str()) % ONLINE_AVATARS;
        if (u.name[0]) out->push_back(u);
    }
    return 1;
}

int OnlineAnswer(const char *name, int yes)
{
    std::string q;
    add(&q, "name", name);
    add(&q, "yes", yes ? 1 : 0);
    return request_me("answer", q, NULL);
}

int OnlineDm(const char *to, const char *text)
{
    std::string q;
    add(&q, "to", to);
    add(&q, "text", text);
    return request_me("dm", q, NULL);
}

int OnlineConv(const char *with, int since, std::vector<OnlineDmLine> *out)
{
    std::vector<std::string> rows;
    std::string q;
    size_t k;
    out->clear();
    add(&q, "with", with);
    add(&q, "since", since);
    if (!request_me("conv", q, &rows)) return 0;
    for (k = 0; k < rows.size(); k++)
    {
        OnlineDmLine m;
        m.id = atoi(field(rows[k], 0).c_str());
        m.mine = atoi(field(rows[k], 1).c_str());
        m.text = field(rows[k], 2);
        if (m.id) out->push_back(m);
    }
    return 1;
}

int OnlineUnread(const char *name)
{
    int n = 0;
    std::map<std::string, int>::const_iterator it;
    if (!s_thread) return 0;
    EnterCriticalSection(&s_lock);
    for (it = s_unread.begin(); it != s_unread.end(); ++it)
        if (!name[0] || !_stricmp(it->first.c_str(), name)) n += it->second;
    LeaveCriticalSection(&s_lock);
    return n;
}

void OnlineUnreadClear(const char *name)
{
    if (!s_thread) return;
    EnterCriticalSection(&s_lock);
    s_unread.erase(name);
    LeaveCriticalSection(&s_lock);
}

int OnlineNoticeTake(char *out, size_t size)
{
    int got = 0;
    if (!s_thread) return 0;
    EnterCriticalSection(&s_lock);
    if (!s_notices.empty())
    {
        copy(out, size, s_notices[0].c_str());
        s_notices.erase(s_notices.begin());
        got = 1;
    }
    LeaveCriticalSection(&s_lock);
    return got;
}

// The news a ping answered with (the thread's): friends coming online, friendships asked, messages.
static void ping_news(const std::vector<std::string> &rows)
{
    static int first = 1;   // the first ping only learns who is online (no notice for everyone at once)
    size_t k;
    char t[200];
    EnterCriticalSection(&s_lock);
    for (k = 0; k < rows.size(); k++)
    {
        const std::string kind = field(rows[k], 0), name = field(rows[k], 1);
        if (kind == "f")
        {
            const int on = atoi(field(rows[k], 2).c_str());
            std::map<std::string, int>::iterator it = s_friendOnline.find(name);
            if (!first && on && (it == s_friendOnline.end() || !it->second))
            {
                _snprintf(t, sizeof(t) - 1, Tr(T_ONLINE_N_FRIEND_ON), name.c_str());
                t[sizeof(t) - 1] = 0;
                s_notices.push_back(t);
            }
            s_friendOnline[name] = on;
        }
        else if (kind == "r")
        {
            size_t j;
            int known = 0;
            for (j = 0; j < s_askedSeen.size(); j++)
                if (s_askedSeen[j] == name) known = 1;
            if (!known)
            {
                s_askedSeen.push_back(name);
                _snprintf(t, sizeof(t) - 1, Tr(T_ONLINE_N_REQUEST), name.c_str());
                t[sizeof(t) - 1] = 0;
                s_notices.push_back(t);
            }
        }
        else if (kind == "m")
        {
            const int id = atoi(name.c_str());
            const std::string from = field(rows[k], 2), text = field(rows[k], 3);
            if (id > s_dmSeen)
            {
                s_dmSeen = id;
                s_unread[from]++;
                _snprintf(t, sizeof(t) - 1, Tr(T_ONLINE_N_DM), from.c_str(), text.c_str());
                t[sizeof(t) - 1] = 0;
                s_notices.push_back(t);
            }
        }
    }
    if (s_notices.size() > 8) s_notices.erase(s_notices.begin(), s_notices.end() - 8);
    first = 0;
    LeaveCriticalSection(&s_lock);
}

int OnlineStatsOf(const char *name, OnlineStats *out)
{
    std::vector<std::string> rows;
    std::string q;
    size_t k;
    out->minutes = out->games = out->rank = 0;
    out->list.clear();
    add(&q, "name", name);
    if (!request_me("stats", q, &rows) || rows.empty()) return 0;
    out->minutes = atoi(field(rows[0], 0).c_str());
    out->games = atoi(field(rows[0], 1).c_str());
    out->rank = atoi(field(rows[0], 2).c_str());
    for (k = 1; k < rows.size(); k++)
    {
        OnlineGameTime g;
        g.game = field(rows[k], 0);
        g.minutes = atoi(field(rows[k], 1).c_str());
        out->list.push_back(g);
    }
    return 1;
}

int OnlineNewsGet(const char *lang, std::vector<OnlineNews> *out)
{
    std::vector<std::string> rows;
    std::string q;
    size_t k;
    out->clear();
    add(&q, "lang", lang);
    if (!request("news", q, &rows, NULL, s_error)) return 0;
    for (k = 0; k < rows.size(); k++)
    {
        OnlineNews n;
        n.date = field(rows[k], 0);
        n.title = field(rows[k], 1);
        n.text = field(rows[k], 2);
        out->push_back(n);
    }
    return 1;
}

// ---- The thread: "still here" every two minutes, and the games played ----
void OnlineSetStatus(const char *game)
{
    if (!s_thread) return;
    EnterCriticalSection(&s_lock);
    copy(s_status, sizeof(s_status), game);
    LeaveCriticalSection(&s_lock);
}

void OnlinePlayed(const char *code, const char *game, int minutes)
{
    Played p;
    if (!s_thread || minutes <= 0) return;
    copy(p.code, sizeof(p.code), code);
    copy(p.game, sizeof(p.game), game);
    p.minutes = minutes;
    EnterCriticalSection(&s_lock);
    if (s_played.size() < 32) s_played.push_back(p);
    LeaveCriticalSection(&s_lock);
}

static DWORD WINAPI online_thread(LPVOID)
{
    DWORD pingAt = GetTickCount();   // the first one at once
    char sent[44] = "\1";            // the status last told (never this one)
    for (;;)
    {
        char status[44], err[sizeof(s_error)];
        std::string q;
        Played p;
        int have = 0;
        Sleep(2000);
        if (!OnlineRegistered() || !s_me.server[0]) continue;
        EnterCriticalSection(&s_lock);
        strcpy(status, s_status);
        if (!s_played.empty()) { p = s_played[0]; have = 1; }
        LeaveCriticalSection(&s_lock);
        if (have)
        {
            add(&q, "code", p.code);
            add(&q, "game", p.game);
            add(&q, "minutes", p.minutes);
            // Sent, or refused by the server: done with it. Not reached: again in a while.
            if (request_me("played", q, NULL, err) || strcmp(err, "net"))
            {
                EnterCriticalSection(&s_lock);
                s_played.erase(s_played.begin());
                LeaveCriticalSection(&s_lock);
            }
            else Sleep(ONLINE_PING_MS / 4);
            continue;
        }
        if ((int)(GetTickCount() - pingAt) < 0 && !strcmp(status, sent)) continue;
        add(&q, "status", status);
        add(&q, "since", s_dmSeen);
        {
            std::vector<std::string> rows;
            if (request_me("ping", q, &rows, err))
            {
                strcpy(sent, status);
                ping_news(rows);
            }
        }
        pingAt = GetTickCount() + ONLINE_PING_MS;
    }
}

void OnlineInit(const char *drive)
{
    if (s_thread) return;
    _snprintf(s_file, sizeof(s_file) - 1, "%s:\\config\\online.txt", drive);
    profile_read();
    H64_INFO("[online] server %s, gamertag %s", s_me.server, OnlineRegistered() ? s_me.tag : "(none)");
    InitializeCriticalSection(&s_lock);
    // A big stack for the TLS handshake; hardware thread 3, away from the CPU (0) and graphics (2, 5)
    // threads, as the update check.
    s_thread = CreateThread(NULL, 256 * 1024, online_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!s_thread) return;
    XSetThreadProcessor(s_thread, 3);
    SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(s_thread);
}

// ---- Public rooms and the general chat (plain requests) ----
int OnlineRooms(std::vector<OnlineRoomInfo> *out)
{
    std::vector<std::string> rows;
    size_t k;
    out->clear();
    if (!request_me("rooms", std::string(), &rows)) return 0;
    for (k = 0; k < rows.size(); k++)
    {
        // code host avatar players game name locked
        OnlineRoomInfo r;
        memset(&r, 0, sizeof(r));
        r.code = (unsigned)atoi(field(rows[k], 0).c_str());
        copy(r.host, sizeof(r.host), field(rows[k], 1).c_str());
        r.avatar = atoi(field(rows[k], 2).c_str());
        r.players = atoi(field(rows[k], 3).c_str());
        copy(r.game, sizeof(r.game), field(rows[k], 4).c_str());
        copy(r.name, sizeof(r.name), field(rows[k], 5).c_str());
        r.locked = atoi(field(rows[k], 6).c_str());
        if (!r.name[0]) copy(r.name, sizeof(r.name), r.host);
        if (r.code) out->push_back(r);
    }
    return 1;
}

int OnlineChatGet(int since, std::vector<OnlineChatLine> *out)
{
    std::vector<std::string> rows;
    std::string q;
    size_t k;
    out->clear();
    add(&q, "since", since);
    if (!request_me("chat", q, &rows)) return 0;
    for (k = 0; k < rows.size(); k++)
    {
        // id tag avatar text
        OnlineChatLine m;
        m.id = atoi(field(rows[k], 0).c_str());
        copy(m.tag, sizeof(m.tag), field(rows[k], 1).c_str());
        m.avatar = atoi(field(rows[k], 2).c_str());
        m.text = field(rows[k], 3);
        chat_when(m.when, atoi(field(rows[k], 4).c_str()));
        if (m.id) out->push_back(m);
    }
    return 1;
}

int OnlineLobbyGet(int since, std::vector<OnlineRoomInfo> *rooms, std::vector<OnlineChatLine> *chat)
{
    std::vector<std::string> rows;
    std::string q;
    size_t k;
    rooms->clear();
    chat->clear();
    add(&q, "since", since);
    if (!request_me("lobby", q, &rows)) return 0;
    for (k = 0; k < rows.size(); k++)
    {
        const std::string &r = rows[k];
        if (!r.compare(0, 2, "R\t"))
        {
            // R code host avatar players game name locked
            OnlineRoomInfo m;
            memset(&m, 0, sizeof(m));
            m.code = (unsigned)atoi(field(r, 1).c_str());
            copy(m.host, sizeof(m.host), field(r, 2).c_str());
            m.avatar = atoi(field(r, 3).c_str());
            m.players = atoi(field(r, 4).c_str());
            copy(m.game, sizeof(m.game), field(r, 5).c_str());
            copy(m.name, sizeof(m.name), field(r, 6).c_str());
            m.locked = atoi(field(r, 7).c_str());
            if (!m.name[0]) copy(m.name, sizeof(m.name), m.host);
            if (m.code) rooms->push_back(m);
        }
        else if (!r.compare(0, 2, "C\t"))
        {
            // C id tag avatar text
            OnlineChatLine m;
            m.id = atoi(field(r, 1).c_str());
            copy(m.tag, sizeof(m.tag), field(r, 2).c_str());
            m.avatar = atoi(field(r, 3).c_str());
            m.text = field(r, 4);
            chat_when(m.when, atoi(field(r, 5).c_str()));
            if (m.id) chat->push_back(m);
        }
    }
    return 1;
}

int OnlineChatSay(const char *text)
{
    std::string q;
    add(&q, "text", text);
    return request_me("say", q, NULL);
}

// ---- Rooms: a WebSocket to /v1/room ----
// The server's words come as text lines (laika_server's worker, Room): "created <code>", "game ...",
// "players ...", "chat ...", "start <players> <port>", or a word that ends the room. From the start
// on the game's messages are binary (main.cpp's netplay takes the connection).
static NetWs *s_room;
static OnlineLobby s_lobby;
static DWORD s_roomPingAt;          // the next ping to the server (its answer gives this console's time)

void OnlineRoomClose(void)
{
    if (s_room) NetWsClose(s_room);   // the server tells the others
    s_room = NULL;
    s_lobby.code = 0;
    s_lobby.count = s_lobby.mine = s_lobby.haveGame = s_lobby.startPlayers = s_lobby.startPort = 0;
    s_lobby.myRtt = s_lobby.countdown = 0;
    s_lobby.countAt = 0;
    s_roomPingAt = 0;
    s_lobby.game[0] = s_lobby.name[0] = 0;
    s_lobby.crc1 = s_lobby.crc2 = 0;
    s_lobby.chat.clear();
}

const OnlineLobby *OnlineRoomLobby(void) { return &s_lobby; }

// A word that ends the room as OnlineError's reason.
static void room_refused(const char *word)
{
    static const char *const known[] = { "none", "full", "started", "closed", "expired", "busy", "old", "auth", "password", "kicked" };
    size_t k;
    for (k = 0; k < sizeof(known) / sizeof(known[0]); k++)
        if (!strcmp(word, known[k])) { set_error(s_error, word); return; }
    set_error(s_error, "net");
}

// One text line of the server's into the lobby: 1 a change, 2 the start, -1 the end of the room.
static int room_line(char *line)
{
    std::vector<std::string> f;
    std::string all(line);
    size_t at = 0, tab;
    for (;;)
    {
        tab = all.find('\t', at);
        f.push_back(all.substr(at, tab == std::string::npos ? tab : tab - at));
        if (tab == std::string::npos) break;
        at = tab + 1;
    }
    if (f[0] == "game" && f.size() >= 4)
    {
        s_lobby.crc1 = (u32)strtoul(f[1].c_str(), NULL, 16);
        s_lobby.crc2 = (u32)strtoul(f[2].c_str(), NULL, 16);
        copy(s_lobby.game, sizeof(s_lobby.game), f[3].c_str());
        copy(s_lobby.name, sizeof(s_lobby.name), f.size() >= 5 ? f[4].c_str() : f[3].c_str());
        s_lobby.haveGame = 1;
        return 1;
    }
    if (f[0] == "players" && f.size() >= 2)
    {
        size_t k;
        s_lobby.mine = atoi(f[1].c_str());
        s_lobby.count = 0;
        for (k = 2; k + 2 < f.size() && s_lobby.count < ONLINE_ROOM_PLAYERS; k += 3)
        {
            copy(s_lobby.tag[s_lobby.count], sizeof(s_lobby.tag[0]), f[k].c_str());
            s_lobby.avatar[s_lobby.count] = atoi(f[k + 1].c_str());
            s_lobby.rtt[s_lobby.count] = atoi(f[k + 2].c_str());
            s_lobby.count++;
        }
        return 1;
    }
    if (f[0] == "chat" && f.size() >= 3)
    {
        OnlineChatLine m;
        m.id = 0;
        copy(m.tag, sizeof(m.tag), f[1].c_str());
        m.avatar = 0;
        for (size_t k = 0; k < (size_t)s_lobby.count; k++)
            if (!strcmp(s_lobby.tag[k], m.tag)) m.avatar = s_lobby.avatar[k];
        m.text = f[2];
        chat_when(m.when, 0);
        s_lobby.chat.push_back(m);
        if (s_lobby.chat.size() > ONLINE_ROOM_CHAT) s_lobby.chat.erase(s_lobby.chat.begin());
        return 1;
    }
    if (f[0] == "pong" && f.size() >= 2)
    {
        // This console's time to the server and back, told to it (the others see it beside its name).
        char line[24];
        const int ms = (int)(GetTickCount() - (DWORD)strtoul(f[1].c_str(), NULL, 10));
        if (ms >= 0 && ms < 10000)
        {
            s_lobby.myRtt = s_lobby.myRtt ? (s_lobby.myRtt * 3 + ms) / 4 : ms;
            _snprintf(line, sizeof(line), "rtt\t%d", s_lobby.myRtt);
            NetWsSend(s_room, line, (int)strlen(line), 1);
        }
        return 1;
    }
    if (f[0] == "count" && f.size() >= 2)
    {
        s_lobby.countdown = atoi(f[1].c_str());
        s_lobby.countAt = GetTickCount();
        return 1;
    }
    if (f[0] == "start" && f.size() >= 3)
    {
        s_lobby.startPlayers = atoi(f[1].c_str());
        s_lobby.startPort = atoi(f[2].c_str());
        return 2;
    }
    if (!strncmp(line, "created ", 8)) { s_lobby.code = (unsigned)atoi(line + 8); return 1; }
    room_refused(line);
    return -1;
}

int OnlineRoomPoll(void)
{
    int result = 0;
    if (s_room && (int)(GetTickCount() - s_roomPingAt) >= 0)
    {
        char line[24];
        _snprintf(line, sizeof(line), "ping\t%u", (unsigned)GetTickCount());
        NetWsSend(s_room, line, (int)strlen(line), 1);
        s_roomPingAt = GetTickCount() + 2000;
    }
    while (s_room)
    {
        char line[600];
        int text = 0, n = NetWsRecv(s_room, line, sizeof(line) - 1, 0, &text), r;
        if (n == 0) break;
        if (n < 0) { set_error(s_error, "net"); OnlineRoomClose(); return -1; }
        if (!text) continue;   // the game's messages come after "start" (main.cpp reads them)
        line[n] = 0;
        r = room_line(line);
        if (r < 0) { OnlineRoomClose(); return -1; }
        if (r == 2) return 2;   // the rest is the game's
        result = 1;
    }
    return result;
}

static int room_open(const std::string &query)
{
    std::string path = "/v1/room?" + query, q;
    OnlineRoomClose();
    set_error(s_error, "net");
    if (!OnlineRegistered()) { set_error(s_error, "auth"); return 0; }
    add(&q, "g", s_me.tag);
    add(&q, "t", s_me.token);
    add(&q, "proto", ONLINE_PROTO);
    path += "&" + q;
    s_room = s_me.server[0] ? NetWsOpen(s_me.server, path.c_str()) : NULL;
    return s_room != NULL;
}

// The room's first words, waiting up to `ms`: 1 when `want` came ("created" or "players"), else 0 (why in OnlineError).
static int room_wait(const char *want, int ms)
{
    const DWORD t0 = GetTickCount();
    while (s_room && GetTickCount() - t0 < (DWORD)ms)
    {
        char line[600];
        int text = 0, n = NetWsRecv(s_room, line, sizeof(line) - 1, 200, &text);
        if (n < 0) { set_error(s_error, "net"); break; }
        if (n == 0 || !text) continue;
        line[n] = 0;
        if (room_line(line) < 0) break;
        if (!strncmp(line, want, strlen(want))) { set_error(s_error, ""); return 1; }
    }
    OnlineRoomClose();
    return 0;
}

int OnlineRoomCreate(const char *name, const char *password, const char *game, u32 crc1, u32 crc2, unsigned *code)
{
    std::string q;
    char hex[12];
    add(&q, "op", "create");
    add(&q, "name", name);
    add(&q, "pass", password);
    add(&q, "game", game);
    _snprintf(hex, sizeof(hex), "%08X", crc1);
    add(&q, "crc1", hex);
    _snprintf(hex, sizeof(hex), "%08X", crc2);
    add(&q, "crc2", hex);
    if (!room_open(q) || !room_wait("created", ONLINE_ANSWER_MS)) return 0;
    *code = s_lobby.code;
    return 1;
}

int OnlineRoomJoin(unsigned code, const char *password)
{
    std::string q;
    char text[8];
    _snprintf(text, sizeof(text), "%06u", code);
    add(&q, "op", "join");
    add(&q, "code", text);
    add(&q, "pass", password);
    if (!room_open(q) || !room_wait("players", ONLINE_ANSWER_MS)) return 0;
    s_lobby.code = code;
    return 1;
}

int OnlineRoomSay(const char *text)
{
    char line[200];
    _snprintf(line, sizeof(line) - 1, "chat\t%s", text);
    line[sizeof(line) - 1] = 0;
    return s_room && NetWsSend(s_room, line, (int)strlen(line), 1);
}

int OnlineRoomStart(void) { return s_room && NetWsSend(s_room, "start", 5, 1); }

int OnlineRoomKick(int place)
{
    char line[16];
    _snprintf(line, sizeof(line), "kick\t%d", place);
    return s_room && NetWsSend(s_room, line, (int)strlen(line), 1);
}

NetWs *OnlineRoomTake(int *port, int *players)
{
    NetWs *w = s_room;
    *port = s_lobby.startPort;
    *players = s_lobby.startPlayers;
    if (!w || *port < 1 || *players < 2) { *port = *players = 0; return NULL; }
    s_room = NULL;
    OnlineRoomClose();
    return w;
}
