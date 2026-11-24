// LAIKA 64 - online play: the player's profile and the server's lists (see xb_online.h).

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_log.h"
#include "xb_net.h"
#include "xb_online.h"
#include "xb_lang.h"
#include "../../core/common/h64_version.h"
#include <map>
#include <set>

#define ONLINE_SERVER "laika64.laikaemu.workers.dev"   // LAIKA 64's own (online.txt's server= names another)
#define ONLINE_ANSWER_MS 8000
#define ONLINE_PING_MS 90000        // the server calls a player online for 5 minutes after one (the news come with it)
#define ONLINE_PROTO 6              // the rooms' (the server's PROTOCOLS): 3 has their size and "back" to the lobby, 4 each
                                    // player's country and connection, 5 two or three playing on one console, 6 "left"
                                    // lines (who went and why)

static OnlineProfile s_me;
static char s_file[96];             // <drive>:\config\online.txt
static int s_serverSet;             // online.txt names the server (kept when the file is written again)
static char s_error[24];
static CRITICAL_SECTION s_lock;     // s_status, s_played, the news below
struct Notice { std::string text; int invite, kind; };
static volatile int s_notify = 15 | 16;   // Config::notify (OnlineSetNotify)
static char s_newer[32];                  // the newer release the server told of (told once a session)

const char *OnlineNewerRelease(void) { return s_newer; }

// Whether version a ("2.3", "2.2.1") is after b.
static int version_after(const char *a, const char *b)
{
    int x[3] = { 0, 0, 0 }, y[3] = { 0, 0, 0 }, k;
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (k = 0; k < 3; k++)
        if (x[k] != y[k]) return x[k] > y[k];
    return 0;
}
static std::vector<Notice> s_notices;               // the news not shown yet
static std::map<std::string, int> s_unread;         // per friend: messages not read yet
static std::map<std::string, int> s_friendOnline;   // per friend: online at the last ping
static std::vector<std::string> s_askedSeen;        // friendships asked already told
static int s_dmSeen;                                // the last message told (kept in the profile: dmseen=, or each start told the last ones again)
static unsigned s_invitedTo;                        // the room of the last invitation received ...
static DWORD s_invitedAt;                           // ... and when
static char s_status[44];
static volatile int s_myMs;         // this console's time to the server and back (the thread measures it)
static volatile int s_inRoom;       // a room's lobby is open (told with the ping: "in a room" for the others)
static int s_roomRtt;               // ... and the time its pings measured, ms
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
    fprintf(f, "gamertag=%s\ntoken=%s\ncountry=%s\navatar=%d\ndmseen=%d\n", s_me.tag, s_me.token, s_me.country, s_me.avatar, s_dmSeen);
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
        else if (!strcmp(line, "dmseen")) s_dmSeen = atoi(eq);
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

// How many days before today (this console's) a moment `ago` seconds before now was: 0 today, 1 yesterday...
static int days_ago(int ago)
{
    SYSTEMTIME st;
    FILETIME ft;
    ULARGE_INTEGER now, then;
    GetLocalTime(&st);
    SystemTimeToFileTime(&st, &ft);
    now.LowPart = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;
    then.QuadPart = now.QuadPart - (ULONGLONG)(ago > 0 ? ago : 0) * 10000000ull;
    return (int)(now.QuadPart / (86400ull * 10000000ull) - then.QuadPart / (86400ull * 10000000ull));
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
    if (kind == ONLINE_LIST_WEEK) add(&q, "list", 1);
    if (!request_me(kind == ONLINE_LIST_FRIENDS ? "friends" : kind == ONLINE_LIST_RANK ? "rank" : kind == ONLINE_LIST_WEEK ? "week" : "users", q, &rows))
        return 0;
    for (k = 0; k < rows.size(); k++)
    {
        // users, friends: name country avatar online status friend. rank: position name country avatar minutes online;
        // week: the same with seconds (this week's, online).
        const int r = kind == ONLINE_LIST_RANK || kind == ONLINE_LIST_WEEK;
        OnlineUser u;
        memset(&u, 0, sizeof(u));
        copy(u.name, sizeof(u.name), field(rows[k], r).c_str());
        copy(u.country, sizeof(u.country), field(rows[k], r + 1).c_str());
        u.avatar = atoi(field(rows[k], r + 2).c_str()) % ONLINE_AVATARS;
        if (r)
        {
            u.pos = atoi(field(rows[k], 0).c_str());
            u.minutes = atoi(field(rows[k], 4).c_str());
            if (kind == ONLINE_LIST_WEEK) u.minutes /= 60;
            u.online = atoi(field(rows[k], 5).c_str());
        }
        else
        {
            u.online = atoi(field(rows[k], 3).c_str());
            copy(u.status, sizeof(u.status), field(rows[k], 4).c_str());
            u.isFriend = atoi(field(rows[k], 5).c_str());
            u.ms = atoi(field(rows[k], 6).c_str());
            u.conn = atoi(field(rows[k], 7).c_str());
        }
        if (u.name[0]) out->push_back(u);
    }
    return 1;
}

int OnlineWeekGet(OnlineWeek *out)
{
    std::vector<std::string> rows;
    std::string q, first;
    size_t k;
    out->daysLeft = out->myPos = out->mySeconds = 0;
    out->top.clear();
    if (!OnlineRegistered()) { set_error(s_error, "auth"); return 0; }
    add(&q, "g", s_me.tag);
    add(&q, "t", s_me.token);
    if (!request("week", q, &rows, &first, s_error)) return 0;
    // the first line: days left, this player's place (0: not in it), its seconds; then place name country avatar seconds
    out->daysLeft = atoi(field(first, 0).c_str());
    out->myPos = atoi(field(first, 1).c_str());
    out->mySeconds = atoi(field(first, 2).c_str());
    for (k = 0; k < rows.size(); k++)
    {
        OnlineWeekRow r;
        memset(&r, 0, sizeof(r));
        r.pos = atoi(field(rows[k], 0).c_str());
        copy(r.name, sizeof(r.name), field(rows[k], 1).c_str());
        copy(r.country, sizeof(r.country), field(rows[k], 2).c_str());
        r.avatar = atoi(field(rows[k], 3).c_str());
        r.seconds = atoi(field(rows[k], 4).c_str());
        if (r.name[0]) out->top.push_back(r);
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
        m.when[0] = 0;
        m.daysAgo = -1;
        if (!field(rows[k], 3).empty())
        {
            // Its time and day on this console's clock (a server before 2.2.2 tells neither).
            const int ago = atoi(field(rows[k], 3).c_str());
            chat_when(m.when, ago);
            m.daysAgo = days_ago(ago);
        }
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

void OnlineSetNotify(int bits) { s_notify = bits; }

int OnlineNoticeTake(char *out, size_t size, int *invite, int *kind)
{
    int got = 0;
    if (!s_thread) return 0;
    EnterCriticalSection(&s_lock);
    if (!s_notices.empty())
    {
        copy(out, size, s_notices[0].text.c_str());
        if (invite) *invite = s_notices[0].invite;
        if (kind) *kind = s_notices[0].kind;
        s_notices.erase(s_notices.begin());
        got = 1;
    }
    LeaveCriticalSection(&s_lock);
    return got;
}

// A notice for the screens (under s_lock): invitations before the other news, the last eight kept.
static void notice_add(const char *text, int invite, int kind)
{
    Notice n;
    size_t at = 0;
    if ((kind == ONLINE_NOTE_ROOM && !(s_notify & 1)) || (kind == ONLINE_NOTE_FRIEND && !(s_notify & 2)) || (kind == ONLINE_NOTE_DM && !(s_notify & 4)) ||
        (kind == ONLINE_NOTE_UPDATE && !(s_notify & 16)))
        return;   // turned off (Settings > Interface > Notifications)
    n.text = text;
    n.invite = invite;
    n.kind = kind;
    if (invite)
        while (at < s_notices.size() && s_notices[at].invite) at++;
    else at = s_notices.size();
    s_notices.insert(s_notices.begin() + at, n);
    if (s_notices.size() > 8) s_notices.pop_back();
}

int OnlineInvite(const char *name, unsigned code, int *live)
{
    std::vector<std::string> rows;
    std::string q, how;
    add(&q, "to", name);
    add(&q, "code", (int)code);
    *live = 0;
    if (!OnlineRegistered()) { set_error(s_error, "auth"); return 0; }
    add(&q, "g", s_me.tag);
    add(&q, "t", s_me.token);
    if (!request("invite", q, NULL, &how, s_error)) return 0;
    *live = how == "live";
    return 1;
}

unsigned OnlineInvitedTo(void)
{
    unsigned code;
    if (!s_thread) return 0;
    EnterCriticalSection(&s_lock);
    code = s_invitedTo && GetTickCount() - s_invitedAt < 600000 ? s_invitedTo : 0;
    LeaveCriticalSection(&s_lock);
    return code;
}

// The news a ping answered with (the thread's): friends coming online, friendships asked, messages.
static std::map<std::string, int> s_friendAvatar;   // under s_lock (the ping's friends' avatars)
static volatile int s_srvState, s_srvMs, s_srvBackAt = -1;
static volatile int s_inetState;   // ONLINE_NET_*

// Internet: Google's generate_204 (what Android and Chrome ask; an empty answer). 1 when it answered.
static int internet_ask(void)
{
    int st;
    if (!NetStart(0)) return 0;
    st = NetGet("www.google.com", "/generate_204", 0, NULL, NULL, 0, NULL, 0);
    return st == 204 || st == 200;
}

int OnlineInternetState(void) { return s_inetState; }
static volatile int s_inboxRtt;   // the inbox's "p" answered "q" (by the server at once, waking nothing): its time, on a
                                  // connection already open - the time to the server, for nothing (0: none yet)

// The server's state from a request's outcome: `ok` with its time, else why (err: "net" nothing reached, "quota" the
// daily reads used up, anything else an answer that was none).
static void server_seen(int ok, int ms, const char *err)
{
    if (ok)
    {
        s_inetState = ONLINE_NET_YES;
        // Slow by the inbox's time when there is one (a request's own counts the handshake of its connection).
        s_srvMs = s_inboxRtt ? s_inboxRtt : ms;
        s_srvBackAt = -1;
        s_srvState = (s_inboxRtt ? s_inboxRtt > ONLINE_SRV_SLOW_MS : ms > 4 * ONLINE_SRV_SLOW_MS) ? ONLINE_SRV_SLOW : ONLINE_SRV_OK;
        return;
    }
    if (!strcmp(err, "net"))
    {
        // Nothing from the server: no internet only when Google does not answer either (a server down said "check
        // your Wi-Fi" on a console well connected - and so did the first ping, as the network came up).
        if (internet_ask()) { s_inetState = ONLINE_NET_YES; s_srvState = ONLINE_SRV_DOWN; s_srvBackAt = -1; }
        else { s_inetState = ONLINE_NET_NO; s_srvState = ONLINE_SRV_NONET; }
        return;
    }
    s_inetState = ONLINE_NET_YES;   // (the server answered something: the way there works)
    s_srvBackAt = -1;
    if (!strcmp(err, "quota"))
    {
        // Back at midnight UTC: that moment on this console's clock.
        SYSTEMTIME utc, local;
        FILETIME fu, fl;
        GetSystemTime(&utc);
        utc.wHour = utc.wMinute = utc.wSecond = utc.wMilliseconds = 0;
        if (SystemTimeToFileTime(&utc, &fu))
        {
            ULARGE_INTEGER u;
            u.LowPart = fu.dwLowDateTime; u.HighPart = fu.dwHighDateTime;
            u.QuadPart += 864000000000ull;   // the next midnight
            fu.dwLowDateTime = u.LowPart; fu.dwHighDateTime = u.HighPart;
            if (FileTimeToLocalFileTime(&fu, &fl) && FileTimeToSystemTime(&fl, &local)) s_srvBackAt = local.wHour * 60 + local.wMinute;
        }
    }
    s_srvState = ONLINE_SRV_DOWN;
}

int OnlineServerState(int *ms, int *backAt)
{
    if (ms) *ms = s_srvMs;
    if (backAt) *backAt = s_srvBackAt;
    return s_srvState;
}

int OnlineFriendsOnline(OnlineFriendOn *out, int max)
{
    int n = 0;
    std::map<std::string, int>::const_iterator it;
    EnterCriticalSection(&s_lock);
    for (it = s_friendOnline.begin(); it != s_friendOnline.end() && n < max; ++it)
        if (it->second)
        {
            std::map<std::string, int>::const_iterator a = s_friendAvatar.find(it->first);
            copy(out[n].name, sizeof(out[n].name), it->first.c_str());
            out[n].avatar = a != s_friendAvatar.end() ? a->second : 0;
            n++;
        }
    LeaveCriticalSection(&s_lock);
    return n;
}

static void ping_news(const std::vector<std::string> &rows)
{
    static int first = 1;   // the first ping only learns who is online (no notice for everyone at once)
    size_t k;
    char t[200];
    const int dmWas = s_dmSeen;
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
                notice_add(t, 0, ONLINE_NOTE_FRIEND);
            }
            s_friendOnline[name] = on;
            s_friendAvatar[name] = atoi(field(rows[k], 3).c_str());
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
                notice_add(t, 0, ONLINE_NOTE_REQUEST);
            }
        }
        else if (kind == "u")
        {
            // The latest release: a newer one than this, told once.
            if (version_after(name.c_str(), H64_VERSION_STRING) && strcmp(name.c_str(), s_newer))
            {
                copy(s_newer, sizeof(s_newer), name.c_str());
                _snprintf(t, sizeof(t) - 1, Tr(T_ONLINE_N_UPDATE), name.c_str());
                t[sizeof(t) - 1] = 0;
                notice_add(t, 0, ONLINE_NOTE_UPDATE);
            }
        }
        else if (kind == "c")
        {
            // The country the server sees this console in (the profile's, found by itself).
            if (name.size() == 2 && _stricmp(name.c_str(), s_me.country))
            {
                copy(s_me.country, sizeof(s_me.country), name.c_str());
                profile_write();
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
                notice_add(t, 0, ONLINE_NOTE_DM);
            }
        }
    }
    first = 0;
    if (s_dmSeen != dmWas) profile_write();   // the messages told stay told after LAIKA 64 is closed
    LeaveCriticalSection(&s_lock);
}

int OnlineStatsOf(const char *name, OnlineStats *out)
{
    std::vector<std::string> rows;
    std::string q;
    size_t k;
    out->minutes = out->games = out->rank = out->isFriend = out->together = out->avatar = out->online = out->ms = out->conn = out->inRoom = 0;
    out->friendDays = -1;
    out->country[0] = out->status[0] = 0;
    out->list.clear();
    add(&q, "name", name);
    if (!request_me("stats", q, &rows) || rows.empty()) return 0;
    out->minutes = atoi(field(rows[0], 0).c_str());
    out->games = atoi(field(rows[0], 1).c_str());
    out->rank = atoi(field(rows[0], 2).c_str());
    // The profile's (a server before 2.2.2 sends none of these: an empty field reads 0).
    out->isFriend = atoi(field(rows[0], 3).c_str());
    if (!field(rows[0], 4).empty()) out->friendDays = atoi(field(rows[0], 4).c_str());
    out->together = atoi(field(rows[0], 5).c_str());
    out->avatar = atoi(field(rows[0], 6).c_str()) % ONLINE_AVATARS;
    _snprintf(out->country, sizeof(out->country) - 1, "%s", field(rows[0], 7).c_str());
    out->country[sizeof(out->country) - 1] = 0;
    out->online = atoi(field(rows[0], 8).c_str());
    _snprintf(out->status, sizeof(out->status) - 1, "%s", field(rows[0], 9).c_str());
    out->status[sizeof(out->status) - 1] = 0;
    out->ms = atoi(field(rows[0], 10).c_str());
    out->conn = atoi(field(rows[0], 11).c_str());
    out->inRoom = atoi(field(rows[0], 12).c_str());
    for (k = 1; k < rows.size(); k++)
    {
        OnlineGameTime g;
        g.game = field(rows[k], 0);
        g.minutes = atoi(field(rows[k], 1).c_str());
        g.code = field(rows[k], 2);
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

int OnlineTopGames(char period, std::vector<OnlineTopGame> *out)
{
    std::vector<std::string> rows;
    std::string q;
    char p[2] = { period, 0 };
    size_t k, j;
    out->clear();
    add(&q, "period", p);
    if (!request_me("top", q, &rows)) return 0;
    for (k = 0; k < rows.size(); k++)
        if (!rows[k].compare(0, 2, "T\t"))
        {
            OnlineTopGame g;
            g.place = atoi(field(rows[k], 1).c_str());
            g.code = field(rows[k], 2);
            g.game = field(rows[k], 3);
            g.minutes = atoi(field(rows[k], 4).c_str());
            g.players = atoi(field(rows[k], 5).c_str());
            g.last = atoi(field(rows[k], 6).c_str());
            g.mine = 0;
            out->push_back(g);
        }
    for (k = 0; k < rows.size(); k++)
        if (!rows[k].compare(0, 2, "M\t"))
            for (j = 0; j < out->size(); j++)
                if ((*out)[j].code == field(rows[k], 1)) (*out)[j].mine = atoi(field(rows[k], 2).c_str());
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

int OnlineMyMs(void) { return s_inRoom && s_roomRtt ? s_roomRtt : s_myMs; }

int OnlineMyConn(void)
{
    const DWORD link = XNetGetEthernetLinkStatus();
    if (!(link & XNET_ETHERNET_LINK_ACTIVE)) return ONLINE_CONN_NONE;
    return (link & XNET_ETHERNET_LINK_WIRELESS) ? ONLINE_CONN_WIFI : ONLINE_CONN_CABLE;
}

// The players online, read by the thread every few seconds while a screen watches them (the pause menu's).
#define ONLINE_WATCH_MS 8000
static volatile int s_watch;
static DWORD s_watchAt;
static std::vector<OnlineUser> s_watched;   // under s_lock
static int s_watchState;                    // 0 not read yet, 1 read, -1 could not be read
static int user_row(const std::string &r, OnlineUser *u);

void OnlineWatchUsers(int on)
{
    if (!s_thread) return;
    EnterCriticalSection(&s_lock);
    if (on && !s_watch) { s_watchAt = GetTickCount(); s_watchState = 0; s_watched.clear(); }
    s_watch = on;
    LeaveCriticalSection(&s_lock);
}

int OnlineWatchedUsers(std::vector<OnlineUser> *out)
{
    int st;
    if (!s_thread) { out->clear(); return -1; }
    EnterCriticalSection(&s_lock);
    *out = s_watched;
    st = s_watchState;
    LeaveCriticalSection(&s_lock);
    return st;
}

static void watch_read(void)
{
    std::vector<std::string> rows;
    std::vector<OnlineUser> users;
    std::string q;
    char err[sizeof(s_error)];
    size_t k;
    add(&q, "since", 0x7FFFFFFF);   // the general chat not wanted
    const int ok = request_me("lobby", q, &rows, err);
    for (k = 0; ok && k < rows.size(); k++)
    {
        OnlineUser u;
        if (!rows[k].compare(0, 2, "U\t") && user_row(rows[k], &u)) users.push_back(u);
    }
    EnterCriticalSection(&s_lock);
    if (ok) s_watched.swap(users);
    if (ok || s_watchState <= 0) s_watchState = ok ? 1 : -1;   // a list already read stays when one read fails
    LeaveCriticalSection(&s_lock);
}

static void lobby_parse(const std::vector<std::string> &rows, std::vector<OnlineRoomInfo> *rooms, std::vector<OnlineGameInfo> *games,
                        std::vector<OnlineUser> *users, std::vector<OnlineChatLine> *chat);   // below

// The lobby read by the thread for a screen (OnlineLobbyWatch); all under s_lock.
static volatile int s_lobbyWatch;
static DWORD s_lobbyAt;
static int s_lobbySince, s_lobbyReady;
static std::vector<OnlineRoomInfo> s_lobbyRooms;
static std::vector<OnlineGameInfo> s_lobbyGames;
static std::vector<OnlineUser> s_lobbyUsers;
static std::vector<OnlineChatLine> s_lobbyChat;
// The pushed lobby (/v1/lobbyws, lobby_ws_thread): the version of the lists held (-1: none yet; /v1/lobby?v= answers
// "S" while it is the server's) and when the socket last brought them (its lists are fresh: no polling meanwhile).
static int s_lobbyV = -1;
static volatile DWORD s_lobbyLiveAt;
#define ONLINE_LOBBY_LIVE_MS 75000   // the server sends the lobby at least every 30 s while it changes; quiet longer: polled

// Presses on the controllers, watched by the online thread: buttons, triggers and sticks coarsely (a stick's jitter is
// not a press).
static volatile DWORD s_activeAt, s_lobbyReadAt;
static DWORD s_padKey[4];

static void activity_poll(void)
{
    DWORD i;
    for (i = 0; i < 4; i++)
    {
        XINPUT_STATE st;
        DWORD key;
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        key = st.Gamepad.wButtons | (st.Gamepad.bLeftTrigger > 64 ? 0x10000u : 0) | (st.Gamepad.bRightTrigger > 64 ? 0x20000u : 0) |
              ((DWORD)((st.Gamepad.sThumbLX + 32768) >> 14) << 18) | ((DWORD)((st.Gamepad.sThumbLY + 32768) >> 14) << 20) |
              ((DWORD)((st.Gamepad.sThumbRX + 32768) >> 14) << 22) | ((DWORD)((st.Gamepad.sThumbRY + 32768) >> 14) << 24);
        if (key != s_padKey[i]) { s_padKey[i] = key; s_activeAt = GetTickCount(); }
    }
}

DWORD OnlineIdleMs(void) { return GetTickCount() - s_activeAt; }
DWORD OnlineLobbyAgeMs(void) { return s_lobbyReadAt ? GetTickCount() - s_lobbyReadAt : 0; }

// How long until the next read of a list watched on screen: as usual, slower, or none (0) while nobody is there.
static DWORD idle_every(DWORD usual)
{
    const DWORD idle = OnlineIdleMs();
    return idle < ONLINE_IDLE_SLOW_MS ? usual : idle < ONLINE_IDLE_STOP_MS ? ONLINE_LOBBY_SLOW_MS : 0;
}

void OnlineLobbyWatch(int on, int since)
{
    if (!s_thread) return;
    EnterCriticalSection(&s_lock);
    if (on)
    {
        s_lobbySince = since;
        s_lobbyAt = GetTickCount();   // the first read at once
        s_lobbyReady = 0;
        s_lobbyChat.clear();
        s_lobbyV = -1;                // (a whole answer, not "S")
    }
    s_lobbyWatch = on;
    LeaveCriticalSection(&s_lock);
}

void OnlineLobbyRefresh(void)
{
    EnterCriticalSection(&s_lock);
    s_lobbyAt = GetTickCount();
    LeaveCriticalSection(&s_lock);
}

int OnlineLobbyTake(std::vector<OnlineRoomInfo> *rooms, std::vector<OnlineGameInfo> *games, std::vector<OnlineUser> *users,
                    std::vector<OnlineChatLine> *chat)
{
    int r;
    if (!s_thread) return 0;
    EnterCriticalSection(&s_lock);
    r = s_lobbyReady;
    if (r)
    {
        *rooms = s_lobbyRooms;
        *games = s_lobbyGames;
        *users = s_lobbyUsers;
        chat->swap(s_lobbyChat);
        s_lobbyChat.clear();
        s_lobbyReady = 0;
    }
    LeaveCriticalSection(&s_lock);
    return r;
}

// A room another player opened since the rooms were last looked at: "<host> opened a room of <game>". The first
// list only learns them (no notice for every room there already is); the rooms gone are forgotten.
static void rooms_news(const std::vector<OnlineRoomInfo> &rooms)
{
    static std::set<unsigned> seen;
    static int learnt;
    std::set<unsigned> now;
    size_t k;
    char t[200];
    EnterCriticalSection(&s_lock);
    for (k = 0; k < rooms.size(); k++)
    {
        now.insert(rooms[k].code);
        if (learnt && !seen.count(rooms[k].code) && _stricmp(rooms[k].host, s_me.tag))
        {
            _snprintf(t, sizeof(t) - 1, Tr(T_ONLINE_N_ROOM), rooms[k].host, rooms[k].game);
            t[sizeof(t) - 1] = 0;
            notice_add(t, 0, ONLINE_NOTE_ROOM);
        }
    }
    seen.swap(now);
    learnt = 1;
    LeaveCriticalSection(&s_lock);
}

static int lobby_version(const std::vector<std::string> &rows, int *same);   // below

// The rooms alone (the general chat not wanted), for rooms_news while no screen reads the lobby.
static void rooms_look(void)
{
    std::vector<std::string> rows;
    std::vector<OnlineRoomInfo> rooms;
    std::vector<OnlineGameInfo> games;
    std::vector<OnlineUser> users;
    std::vector<OnlineChatLine> chat;
    std::string q;
    char err[sizeof(s_error)];
    static int seenV = -1;   // the version last looked at: "S" (nothing read on the server) while it is the same
    int same, v;
    add(&q, "since", 0x7FFFFFFF);
    add(&q, "v", seenV);
    if (!request_me("lobby", q, &rows, err)) return;
    v = lobby_version(rows, &same);
    if (same) return;
    seenV = v;
    lobby_parse(rows, &rooms, &games, &users, &chat);
    rooms_news(rooms);
}

// A version line of /v1/lobby ("V <version>", or "S <version>" alone: nothing changed): the version, -1 when none.
static int lobby_version(const std::vector<std::string> &rows, int *same)
{
    size_t k;
    *same = 0;
    for (k = 0; k < rows.size(); k++)
        if (!rows[k].compare(0, 2, "V\t") || !rows[k].compare(0, 2, "S\t"))
        {
            *same = rows[k][0] == 'S';
            return atoi(rows[k].c_str() + 2);
        }
    return -1;
}

// The lists of an answer (read, or pushed) for the screen: rooms, games and players as they are, the chat lines after
// the ones it has. `since`: the chat line the request was made after (-1: pushed, whatever it holds).
static void lobby_apply(const std::vector<std::string> &rows, int since, int version)
{
    std::vector<OnlineRoomInfo> rooms;
    std::vector<OnlineGameInfo> games;
    std::vector<OnlineUser> users;
    std::vector<OnlineChatLine> chat;
    size_t k;
    lobby_parse(rows, &rooms, &games, &users, &chat);
    rooms_news(rooms);
    EnterCriticalSection(&s_lock);
    if (s_lobbyWatch && (since < 0 || s_lobbySince == since))   // (not a read for a screen gone, or one that started again)
    {
        s_lobbyRooms.swap(rooms);
        s_lobbyGames.swap(games);
        s_lobbyUsers.swap(users);
        for (k = 0; k < chat.size(); k++)
            if (chat[k].id > s_lobbySince)
            {
                s_lobbyChat.push_back(chat[k]);
                s_lobbySince = chat[k].id;
            }
        if (version >= 0) s_lobbyV = version;
        s_lobbyReady = 1;
    }
    LeaveCriticalSection(&s_lock);
}

static void lobby_read(void)
{
    std::vector<std::string> rows;
    std::string q;
    char err[sizeof(s_error)];
    int since, v, same;
    EnterCriticalSection(&s_lock);
    since = s_lobbySince;
    v = s_lobbyV;
    LeaveCriticalSection(&s_lock);
    add(&q, "since", since);
    add(&q, "v", v);   // (-1 at first: never the server's, so the whole lobby)
    if (!request_me("lobby", q, &rows, err)) return;   // not reached: the screen keeps what it has
    v = lobby_version(rows, &same);
    if (same)
    {
        // Nothing changed since the lists held (read without the database): they stay.
        EnterCriticalSection(&s_lock);
        if (s_lobbyWatch) s_lobbyReady = 1;
        LeaveCriticalSection(&s_lock);
        return;
    }
    lobby_apply(rows, since, v);
}

// ---- The pushed lobby: a WebSocket kept open while a screen shows the lobby (OnlineLobbyWatch). The server sends
// the lobby as it changes, in parts ("L\t<version>\t<part>\t<parts>\n<lines>"): put together, then the screen's.
// The 8 s reads stop while it brings them (online_thread); they come back if it goes quiet or is lost.
#define ONLINE_LOBBY_WS_KEEP_MS 30000   // "p" this often (the server lets go of one silent for 90 s)
#define ONLINE_LOBBY_WS_LINGER_MS 15000 // the socket kept this long after the lobby's screen closed (back soon: no new one)

static DWORD WINAPI lobby_ws_thread(LPVOID)
{
    static char msg[4200];
    DWORD wait = 1000;
    for (;;)
    {
        std::string q, parts[16];
        char tag[ONLINE_TAG_MAX + 1];
        NetWs *w;
        DWORD openAt, keepAt, unwatchedAt = 0;
        int partsV = -1, partsN = 0, got = 0;
        Sleep(wait);
        if (!s_lobbyWatch || !OnlineRegistered() || !s_me.server[0] || !NetStart(0)) { wait = 1000; continue; }
        copy(tag, sizeof(tag), s_me.tag);
        add(&q, "g", s_me.tag);
        add(&q, "t", s_me.token);
        w = NetWsOpen(s_me.server, ("/v1/lobbyws?" + q).c_str());
        // Not reached (an older server, or none): the reads go on; tried again later and later (up to two minutes).
        if (!w) { wait = wait < 60000 ? wait * 2 : 120000; continue; }
        H64_INFO("[online] lobby pushed (socket open)");
        openAt = GetTickCount();
        keepAt = openAt + ONLINE_LOBBY_WS_KEEP_MS;
        for (;;)
        {
            int text = 0;
            const int n = NetWsRecv(w, msg, sizeof(msg) - 1, 1000, &text);
            if (n < 0) break;
            if (n > 0 && text)
            {
                msg[n] = 0;
                if (msg[0] == 'L' && msg[1] == '\t')
                {
                    // One part: its version, place and count; the lines after its first one.
                    const char *nl = strchr(msg, '\n');
                    const std::string head(msg, nl ? nl - msg : n);
                    const int v = atoi(field(head, 1).c_str()), i = atoi(field(head, 2).c_str()), count = atoi(field(head, 3).c_str());
                    if (count < 1 || count > 16 || i < 0 || i >= count) continue;
                    if (v != partsV || count != partsN)
                    {
                        int k;
                        for (k = 0; k < 16; k++) parts[k].clear();
                        partsV = v;
                        partsN = count;
                        got = 0;
                    }
                    if (parts[i].empty())
                    {
                        parts[i] = nl ? std::string(nl + 1) : std::string();
                        parts[i] += '\n';   // (an empty part still counts as there)
                        got++;
                    }
                    if (got == partsN)
                    {
                        // The whole lobby: its lines for the screen.
                        std::vector<std::string> rows;
                        int k;
                        for (k = 0; k < partsN; k++)
                        {
                            size_t at = 0, e;
                            while ((e = parts[k].find('\n', at)) != std::string::npos)
                            {
                                if (e > at) rows.push_back(parts[k].substr(at, e - at));
                                at = e + 1;
                            }
                        }
                        lobby_apply(rows, -1, partsV);
                        s_lobbyLiveAt = GetTickCount();
                        partsV = -1;
                        partsN = got = 0;
                    }
                }
            }
            if (strcmp(tag, s_me.tag)) break;   // another gamertag
            if (!s_lobbyWatch)
            {
                // The lobby's screen closed: kept a little (it may be back at once), then let go.
                if (!unwatchedAt) unwatchedAt = GetTickCount();
                else if (GetTickCount() - unwatchedAt > ONLINE_LOBBY_WS_LINGER_MS) break;
            }
            else unwatchedAt = 0;
            if ((int)(GetTickCount() - keepAt) >= 0)
            {
                if (!NetWsSend(w, "p", 1, 1)) break;
                keepAt = GetTickCount() + ONLINE_LOBBY_WS_KEEP_MS;
            }
        }
        NetWsClose(w);
        s_lobbyLiveAt = 0;   // (the reads at once, if a screen still shows it)
        EnterCriticalSection(&s_lock);
        s_lobbyAt = GetTickCount();
        LeaveCriticalSection(&s_lock);
        H64_INFO("[online] lobby socket closed after %u s", (unsigned)((GetTickCount() - openAt) / 1000));
        // Lost soon after it opened: later and later; otherwise again as soon as a screen wants it.
        wait = GetTickCount() - openAt < 20000 ? (wait < 60000 ? wait * 2 : 120000) : 1000;
    }
}

static DWORD WINAPI online_thread(LPVOID)
{
    DWORD pingAt = GetTickCount();   // the first one at once
    DWORD roomsAt = GetTickCount() + 5000;   // the rooms first looked at a little after the start
    int away = 0;                    // nobody at the controllers (the lists not read)
    char sent[44] = "\1";
    s_activeAt = GetTickCount();     // (LAIKA just started: someone is there)
    {
        // Internet first, as LAIKA loads (the intro shows it): Google, a few tries while the network comes up.
        int k;
        for (k = 0; k < 6; k++)
        {
            if (internet_ask()) { s_inetState = ONLINE_NET_YES; break; }
            Sleep(2500);
        }
        if (s_inetState != ONLINE_NET_YES) { s_inetState = ONLINE_NET_NO; s_srvState = ONLINE_SRV_NONET; }
    }            // the status last told (never this one)
    int sentRoom = -1;               // ... and whether in a room
    for (;;)
    {
        char status[44], err[sizeof(s_error)];
        std::string q;
        Played p;
        int have = 0;
        Sleep(s_watch || s_lobbyWatch ? 100 : 2000);
        activity_poll();
        if (away && OnlineIdleMs() < ONLINE_IDLE_SLOW_MS)
        {
            // Someone back: the lists read again at once.
            EnterCriticalSection(&s_lock);
            s_lobbyAt = GetTickCount();
            LeaveCriticalSection(&s_lock);
            s_watchAt = GetTickCount();
        }
        away = OnlineIdleMs() >= ONLINE_IDLE_STOP_MS;
        if (!OnlineRegistered() && s_me.server[0] && s_srvState == ONLINE_SRV_UNKNOWN && NetStart(0))
        {
            // No profile (no pings): one look, for the server's state on the screens.
            const DWORD t0 = GetTickCount();
            const int ok = request("whereami", std::string(), NULL, NULL, err);
            server_seen(ok, (int)(GetTickCount() - t0), err);
        }
        if (!OnlineRegistered() || !s_me.server[0]) continue;
        if ((s_notify & 1) && !s_lobbyWatch && !s_watch && !s_inRoom && !away && (int)(GetTickCount() - roomsAt) >= 0)
        {
            // New rooms (Settings > Interface > Notifications): a look every 20 s (the lobby's reads do it while open).
            roomsAt = GetTickCount() + ONLINE_ROOMS_LOOK_MS;
            rooms_look();
            continue;
        }
        if (s_lobbyWatch && !away && (int)(GetTickCount() - s_lobbyAt) >= 0)
        {
            const DWORD live = s_lobbyLiveAt;
            EnterCriticalSection(&s_lock);
            s_lobbyAt = GetTickCount() + idle_every(ONLINE_LOBBY_MS);
            LeaveCriticalSection(&s_lock);
            if (live && GetTickCount() - live < ONLINE_LOBBY_LIVE_MS) continue;   // pushed (lobby_ws_thread): nothing to read
            lobby_read();
            s_lobbyReadAt = GetTickCount();
            continue;
        }
        if (s_watch && !away && (int)(GetTickCount() - s_watchAt) >= 0)
        {
            watch_read();
            s_watchAt = GetTickCount() + idle_every(ONLINE_WATCH_MS);
            continue;
        }
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
        if ((int)(GetTickCount() - pingAt) < 0 && !strcmp(status, sent) && sentRoom == s_inRoom) continue;
        {
            // In a room its own pings measure the time; in a game none is measured (the game's time is not to be taken).
            // The time to the server: this ping's own (three "whereami" before each one measured it: four requests
            // where one does). In a room its pings' time; in a game the last one (the game's time is not to be taken).
            const int room = s_inRoom;
            std::vector<std::string> rows;
            DWORD t0;
            int ok;
            if (room && s_roomRtt) s_myMs = s_roomRtt;
            add(&q, "status", status);
            add(&q, "since", s_dmSeen);
            add(&q, "ms", s_myMs);
            add(&q, "conn", OnlineMyConn());
            add(&q, "room", room);
            t0 = GetTickCount();
            ok = request_me("ping", q, &rows, err);
            server_seen(ok, (int)(GetTickCount() - t0), err);
            if (ok && !room && !status[0] && s_inboxRtt) s_myMs = s_inboxRtt;   // (not the request's: its handshake counted)
            if (ok)
            {
                strcpy(sent, status);
                sentRoom = room;
                ping_news(rows);
            }
        }
        // The server down (or not reached): looked at again sooner, so its coming back shows soon.
        pingAt = GetTickCount() + (s_srvState == ONLINE_SRV_DOWN || s_srvState == ONLINE_SRV_NONET ? ONLINE_PING_MS / 3 : ONLINE_PING_MS);
    }
}

// ---- The inbox: a WebSocket kept open to the server, which sends each invitation through it at once ----
#define ONLINE_INBOX_KEEP_MS 30000   // "p" this often (the server lets go of one silent for 90 s)

// "invite\t<from>\t<game>\t<code>\t<room>": "<from> wants to play <game> with you" for the screens.
static void inbox_line(const char *line)
{
    const std::string l = line, from = field(l, 1), game = field(l, 2);
    char t[200];
    if (field(l, 0) != "invite" || from.empty()) return;
    _snprintf(t, sizeof(t) - 1, Tr(T_ONLINE_N_INVITE), from.c_str(), game.c_str());
    t[sizeof(t) - 1] = 0;
    H64_INFO("[online] %s", t);
    EnterCriticalSection(&s_lock);
    notice_add(t, 1, ONLINE_NOTE_INVITE);
    s_invitedTo = (unsigned)atoi(field(l, 3).c_str());
    s_invitedAt = GetTickCount();
    LeaveCriticalSection(&s_lock);
}

static DWORD WINAPI inbox_thread(LPVOID)
{
    DWORD wait = 3000;
    for (;;)
    {
        std::string q;
        char tag[ONLINE_TAG_MAX + 1], line[512];
        NetWs *w;
        DWORD openAt, keepAt, sentAt = 0;
        Sleep(wait);
        if (!OnlineRegistered() || !s_me.server[0] || !NetStart(0)) { wait = 10000; continue; }
        copy(tag, sizeof(tag), s_me.tag);
        add(&q, "g", s_me.tag);
        add(&q, "t", s_me.token);
        w = NetWsOpen(s_me.server, ("/v1/inbox?" + q).c_str());
        // Not reached, or refused at once: again later and later (up to two minutes).
        if (!w) { wait = wait < 60000 ? wait * 2 : 120000; continue; }
        openAt = GetTickCount();
        keepAt = openAt;   // a "p" at once: its "q" gives the time to the server from the start
        for (;;)
        {
            int text = 0;
            const int n = NetWsRecv(w, line, sizeof(line) - 1, 1000, &text);
            if (n < 0) break;
            if (n > 0 && text)
            {
                line[n] = 0;
                if (!strcmp(line, "q")) { if (sentAt) { s_inboxRtt = (int)(GetTickCount() - sentAt) > 0 ? (int)(GetTickCount() - sentAt) : 1; sentAt = 0; } }
                else inbox_line(line);
            }
            if (strcmp(tag, s_me.tag)) break;   // another gamertag: its own inbox
            if ((int)(GetTickCount() - keepAt) >= 0)
            {
                if (!NetWsSend(w, "p", 1, 1)) break;
                sentAt = GetTickCount();   // its "q" times the way to the server
                keepAt = GetTickCount() + ONLINE_INBOX_KEEP_MS;
            }
        }
        NetWsClose(w);
        wait = GetTickCount() - openAt < 20000 ? (wait < 60000 ? wait * 2 : 120000) : 3000;
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
    {
        HANDLE inbox = CreateThread(NULL, 256 * 1024, inbox_thread, NULL, CREATE_SUSPENDED, NULL);
        if (!inbox) return;
        XSetThreadProcessor(inbox, 3);
        SetThreadPriority(inbox, THREAD_PRIORITY_BELOW_NORMAL);
        ResumeThread(inbox);
        CloseHandle(inbox);
    }
    {
        HANDLE lobby = CreateThread(NULL, 256 * 1024, lobby_ws_thread, NULL, CREATE_SUSPENDED, NULL);
        if (!lobby) return;
        XSetThreadProcessor(lobby, 3);
        SetThreadPriority(lobby, THREAD_PRIORITY_BELOW_NORMAL);
        ResumeThread(lobby);
        CloseHandle(lobby);
    }
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

// A player online of /v1/lobby: "U name country avatar status room ms conn". 1 when it names one.
static int user_row(const std::string &r, OnlineUser *u)
{
    memset(u, 0, sizeof(*u));
    copy(u->name, sizeof(u->name), field(r, 1).c_str());
    copy(u->country, sizeof(u->country), field(r, 2).c_str());
    u->avatar = atoi(field(r, 3).c_str()) % ONLINE_AVATARS;
    copy(u->status, sizeof(u->status), field(r, 4).c_str());
    u->inRoom = atoi(field(r, 5).c_str());
    u->ms = atoi(field(r, 6).c_str());
    u->conn = atoi(field(r, 7).c_str());
    u->online = 1;
    return u->name[0] != 0;
}

static void lobby_parse(const std::vector<std::string> &rows, std::vector<OnlineRoomInfo> *rooms, std::vector<OnlineGameInfo> *games,
                        std::vector<OnlineUser> *users, std::vector<OnlineChatLine> *chat);

int OnlineLobbyGet(int since, std::vector<OnlineRoomInfo> *rooms, std::vector<OnlineGameInfo> *games, std::vector<OnlineUser> *users,
                   std::vector<OnlineChatLine> *chat)
{
    std::vector<std::string> rows;
    std::string q;
    rooms->clear();
    games->clear();
    users->clear();
    chat->clear();
    add(&q, "since", since);
    if (!request_me("lobby", q, &rows)) return 0;
    lobby_parse(rows, rooms, games, users, chat);
    return 1;
}

// The lobby's rows: rooms (R), games being played (G), players (U), the chat (C).
static void lobby_parse(const std::vector<std::string> &rows, std::vector<OnlineRoomInfo> *rooms, std::vector<OnlineGameInfo> *games,
                        std::vector<OnlineUser> *users, std::vector<OnlineChatLine> *chat)
{
    size_t k;
    for (k = 0; k < rows.size(); k++)
    {
        const std::string &r = rows[k];
        if (!r.compare(0, 2, "R\t"))
        {
            // R code host avatar players game name locked max hostms art crc
            OnlineRoomInfo m;
            memset(&m, 0, sizeof(m));
            m.code = (unsigned)atoi(field(r, 1).c_str());
            copy(m.host, sizeof(m.host), field(r, 2).c_str());
            m.avatar = atoi(field(r, 3).c_str());
            m.players = atoi(field(r, 4).c_str());
            copy(m.game, sizeof(m.game), field(r, 5).c_str());
            copy(m.name, sizeof(m.name), field(r, 6).c_str());
            m.locked = atoi(field(r, 7).c_str());
            m.max = atoi(field(r, 8).c_str());
            if (m.max < 2 || m.max > ONLINE_ROOM_PLAYERS) m.max = ONLINE_ROOM_PLAYERS;
            m.hostMs = atoi(field(r, 9).c_str());
            copy(m.art, sizeof(m.art), field(r, 10).c_str());
            m.hasCrc = sscanf(field(r, 11).c_str(), "%8x %8x", &m.crc1, &m.crc2) == 2;   // "CRC1 CRC2" (newer servers)
            if (!m.name[0]) copy(m.name, sizeof(m.name), m.host);
            if (m.code) rooms->push_back(m);
        }
        else if (!r.compare(0, 2, "G\t"))
        {
            // G code game seconds tag,tag...
            OnlineGameInfo g;
            const std::string tags = field(r, 4);
            size_t at = 0, comma;
            memset(&g, 0, sizeof(g));
            g.code = (unsigned)atoi(field(r, 1).c_str());
            copy(g.game, sizeof(g.game), field(r, 2).c_str());
            g.seconds = atoi(field(r, 3).c_str());
            copy(g.art, sizeof(g.art), field(r, 5).c_str());
            while (at < tags.size() && g.count < 4)
            {
                comma = tags.find(',', at);
                copy(g.tag[g.count++], sizeof(g.tag[0]), tags.substr(at, comma == std::string::npos ? comma : comma - at).c_str());
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
            if (g.code && g.count) games->push_back(g);
        }
        else if (!r.compare(0, 2, "U\t"))
        {
            OnlineUser u;
            if (user_row(r, &u)) users->push_back(u);
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
static int s_back;                  // the room was given back by the game (OnlineRoomBack)
static int s_pongs;                 // the server's answers to this console's pings since the room opened
static DWORD s_roomAt;              // when the room opened (its first pings every second)
static int s_newPlayer;             // a player came into the room (OnlineRoomNewPlayer) ...
static char s_newTag[ONLINE_TAG_MAX + 1];   // ... this one

int OnlineRoomNewPlayer(char *tag, size_t size)
{
    const int n = s_newPlayer;
    if (n && tag && size) copy(tag, size, s_newTag);
    s_newPlayer = 0;
    return n;
}

void OnlineRoomClose(void)
{
    if (s_room) NetWsClose(s_room);   // the server tells the others
    s_room = NULL;
    s_inRoom = s_back = s_newPlayer = 0;
    s_roomRtt = 0;
    s_lobby.max = 0;
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
        s_lobby.max = f.size() >= 6 ? atoi(f[5].c_str()) : ONLINE_ROOM_PLAYERS;
        if (s_lobby.max < 2 || s_lobby.max > ONLINE_ROOM_PLAYERS) s_lobby.max = ONLINE_ROOM_PLAYERS;
        s_lobby.haveGame = 1;
        return 1;
    }
    if (f[0] == "status" || f[0] == "p2p") return 0;   // the game's words, left from it after "back"
    if (f[0] == "players" && f.size() >= 2)
    {
        size_t k;
        const int was = s_lobby.count;
        s_lobby.mine = atoi(f[1].c_str());
        s_lobby.count = 0;
        // Each console: tag avatar ms country conn local.
        s_lobby.players = 0;
        for (k = 2; k + 5 < f.size() && s_lobby.count < ONLINE_ROOM_PLAYERS; k += 6)
        {
            s_lobby.local[s_lobby.count] = atoi(f[k + 5].c_str());
            if (s_lobby.local[s_lobby.count] < 1 || s_lobby.local[s_lobby.count] > 3) s_lobby.local[s_lobby.count] = 1;
            s_lobby.players += s_lobby.local[s_lobby.count];
            copy(s_lobby.tag[s_lobby.count], sizeof(s_lobby.tag[0]), f[k].c_str());
            s_lobby.avatar[s_lobby.count] = atoi(f[k + 1].c_str());
            s_lobby.rtt[s_lobby.count] = atoi(f[k + 2].c_str());
            copy(s_lobby.country[s_lobby.count], sizeof(s_lobby.country[0]), f[k + 3].c_str());
            s_lobby.conn[s_lobby.count] = atoi(f[k + 4].c_str());
            s_lobby.count++;
        }
        // One more than there were (not this console's own first list, not the others coming back from a game).
        if (was > 0 && s_lobby.count > was)
        {
            s_newPlayer = 1;
            copy(s_newTag, sizeof(s_newTag), s_lobby.tag[s_lobby.count - 1]);   // the server puts who came last
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
        // A quick chat text of the game's ("@q<n>", main.cpp's 8): in this console's language.
        if (m.text.size() == 3 && m.text[0] == '@' && m.text[1] == 'q' && m.text[2] >= '0' && m.text[2] < '8') m.text = Tr(T_QC_1 + (m.text[2] - '0'));
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
        // The first answer (the connection just made, the room just woken) is slow: not counted. Then a quicker
        // time is taken at once and a slower one slowly - a moment's delay does not show as the time.
        if (ms >= 0 && ms < 10000 && ++s_pongs > 1)
        {
            s_lobby.myRtt = !s_lobby.myRtt ? ms : ms < s_lobby.myRtt ? (s_lobby.myRtt + ms * 3) / 4 : (s_lobby.myRtt * 3 + ms) / 4;
            s_roomRtt = s_lobby.myRtt;
            _snprintf(line, sizeof(line), "rtt\t%d", s_lobby.myRtt);
            NetWsSend(s_room, line, (int)strlen(line), 1);
        }
        return 1;
    }
    if (f[0] == "left" && f.size() >= 3)
    {
        // A player went: said in the chat, with why (no such ROM, its connection lost, put out, or by itself).
        OnlineChatLine m;
        m.id = 0;
        copy(m.tag, sizeof(m.tag), f[1].c_str());
        m.avatar = 0;
        for (size_t k = 0; k < (size_t)s_lobby.count; k++)
            if (!strcmp(s_lobby.tag[k], m.tag)) m.avatar = s_lobby.avatar[k];
        m.text = Tr(f[2] == "nogame" ? T_ONLINE_LEFT_NOGAME : f[2] == "gone" ? T_ONLINE_LEFT_GONE : f[2] == "kicked" ? T_ONLINE_LEFT_KICKED : T_ONLINE_LEFT);
        chat_when(m.when, 0);
        s_lobby.chat.push_back(m);
        if (s_lobby.chat.size() > ONLINE_ROOM_CHAT) s_lobby.chat.erase(s_lobby.chat.begin());
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
        // start players port local owners (each port's console, "1,1,3")
        size_t at = 0;
        int p = 1;
        s_lobby.startPlayers = atoi(f[1].c_str());
        s_lobby.startPort = atoi(f[2].c_str());
        s_lobby.startLocal = f.size() >= 4 ? atoi(f[3].c_str()) : 1;
        memset(s_lobby.startOwner, 0, sizeof(s_lobby.startOwner));
        for (p = 1; p <= ONLINE_ROOM_PLAYERS; p++) s_lobby.startOwner[p] = (unsigned char)p;
        for (p = 1; f.size() >= 5 && at < f[4].size() && p <= ONLINE_ROOM_PLAYERS; p++)
        {
            const size_t comma = f[4].find(',', at);
            s_lobby.startOwner[p] = (unsigned char)atoi(f[4].substr(at, comma == std::string::npos ? comma : comma - at).c_str());
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
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
        if (!s_roomPingAt && !s_lobby.myRtt && OnlineMyMs())
        {
            // To start with, the time the thread measured: the others see one at once.
            _snprintf(line, sizeof(line), "rtt\t%d", OnlineMyMs());
            NetWsSend(s_room, line, (int)strlen(line), 1);
        }
        _snprintf(line, sizeof(line), "ping\t%u", (unsigned)GetTickCount());
        NetWsSend(s_room, line, (int)strlen(line), 1);
        s_roomPingAt = GetTickCount() + (GetTickCount() - s_roomAt < 10000 ? 1000 : 2000);
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
    add(&q, "conn", OnlineMyConn());   // cable or Wi-Fi, shown to the others in the room
    path += "&" + q;
    s_room = s_me.server[0] ? NetWsOpen(s_me.server, path.c_str()) : NULL;
    s_inRoom = s_room != NULL;
    s_roomAt = GetTickCount();
    s_pongs = 0;
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

int OnlineRoomCreate(const char *name, const char *password, const char *game, u32 crc1, u32 crc2, const char *art, int max, unsigned *code)
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
    add(&q, "max", max);
    add(&q, "art", art);
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

void OnlineRoomLeave(const char *why)
{
    char line[24];
    _snprintf(line, sizeof(line) - 1, "bye\t%s", why);
    line[sizeof(line) - 1] = 0;
    if (s_room) NetWsSend(s_room, line, (int)strlen(line), 1);
    OnlineRoomClose();
}

int OnlineRoomStart(void) { return s_room && NetWsSend(s_room, "start", 5, 1); }

int OnlineRoomKick(int place)
{
    char line[16];
    _snprintf(line, sizeof(line), "kick\t%d", place);
    return s_room && NetWsSend(s_room, line, (int)strlen(line), 1);
}

int OnlineRoomLocal(int count)
{
    char line[16];
    _snprintf(line, sizeof(line), "local\t%d", count);
    return s_room && NetWsSend(s_room, line, (int)strlen(line), 1);
}

NetWs *OnlineRoomTake(int *port, int *players, int *local, unsigned char *owner)
{
    NetWs *w = s_room;
    *port = s_lobby.startPort;
    *players = s_lobby.startPlayers;
    *local = s_lobby.startLocal > 0 ? s_lobby.startLocal : 1;
    memcpy(owner, s_lobby.startOwner, ONLINE_ROOM_PLAYERS + 1);
    if (!w || *port < 1 || *players < 2) { *port = *players = 0; return NULL; }
    // The game's now; the lobby's name, game and chat kept for OnlineRoomGiveBack.
    s_room = NULL;
    s_inRoom = s_back = 0;
    return w;
}

void OnlineRoomGiveBack(NetWs *w)
{
    if (!w) return;
    if (s_room) NetWsClose(s_room);
    s_room = w;
    NetWsSend(s_room, "back", 4, 1);   // out of the game: the others are told, the lobby's lines come again
    s_inRoom = s_back = 1;
    s_lobby.startPlayers = s_lobby.startPort = s_lobby.countdown = 0;
    s_lobby.countAt = 0;
    s_roomPingAt = 0;
    s_roomAt = GetTickCount();
}

int OnlineRoomBack(void) { return s_room && s_back; }
