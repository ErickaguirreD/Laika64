// LAIKA 64 - online server on Cloudflare Workers: the players' profiles (D1) and, per game, a room
// that relays one console's netplay messages to the other (a Durable Object, over WebSocket).
//
// The console asks with plain GETs (its HTTPS client does nothing else) and reads text:
// "OK" or "ERR <reason>" on the first line ("OK\t<value>" for REGISTER), data lines with their fields
// separated by tabs, then a line with only "." - the same answers as tools/netrelay/laika_server.py,
// the server this one replaces where a machine of one's own cannot be had.
//
//   /v1/register?tag=&country=&avatar=        -> OK <token>   (the console's secret from then on)
//   /v1/profile?g=&t=&country=&avatar=
//   /v1/whereami                              OK <country> (the country the request comes from)
//   /v1/ping?g=&t=&status=&since=&ms=&conn=&room=
//                                             "still here"; status: "" or the game played; ms: its time to the
//                                             server and back; conn: 1 cable, 2 Wi-Fi (0 not known; not sent: kept
//                                             as 3, a LAIKA before 2.1); room: 1 in a
//                                             room's lobby. Answered with the player's news: "f name online" per
//                                             friend, "r name avatar" per friendship asked, "m id name text" per
//                                             message after `since`
//   /v1/rename?g=&t=&tag=                     another gamertag (free, and valid)
//   /v1/users?g=&t=&offset=                   name country avatar online status friend (1 friends, 2 asked) ms conn
//   /v1/friends?g=&t=                         the same, friends only
//   /v1/friend?g=&t=&name=&add=1|0            asks for a friendship (OK sent; OK accepted when the other had
//                                             asked too), or ends one
//   /v1/requests?g=&t=                        friendships asked of this player: name country avatar
//   /v1/answer?g=&t=&name=&yes=1|0            yes or no to one
//   /v1/dm?g=&t=&to=&text=                    a message to a friend
//   /v1/conv?g=&t=&with=&since=               the messages with a friend after `since`: id mine text
//   /v1/lobby?g=&t=&since=                    in one answer: the rooms waiting ("R" code host avatar players game
//                                             name locked max hostms art crc; crc: "CRC1 CRC2" of its ROM), the games being played ("G" code game
//                                             seconds tag,tag... art; art: the box art's key), the players online ("U" name country avatar
//                                             status room ms conn playing; playing: seconds playing what status says)
//                                             and the general chat after `since`
//                                             ("C" id tag avatar text ago)
//                                             With v=<version> (the last "V" line read): "S <version>" alone when
//                                             nothing changed since (no database read); otherwise the lists and
//                                             "V <version>"
//   /v1/lobbyws?g=&t=                         WebSocket kept open while a console is in Play online: the server sends
//                                             it the lobby (as /v1/lobby, the chat its last messages) as it changes,
//                                             in parts "L\t<version>\t<part>\t<parts>\n<lines>"; "p" now and then
//                                             (answered "q")
//   /v1/invite?g=&t=&to=&code=                invites a player to the room `code` (waiting): told at once through
//                                             its inbox ("OK live"), or when it next opens one ("OK later")
//   /v1/inbox?g=&t=                           WebSocket kept open by each console: the server sends through it
//                                             "invite\t<from>\t<game>\t<code>\t<room>" as soon as one is made;
//                                             the console sends "p" every half minute (answered "q")
//   /v1/played?g=&t=&code=&game=&minutes=     after a game
//   /v1/stats?g=&t=&name=                     total minutes, games, rank, friend (1 friends), days friends (-1 not known),
//                                             games played together, avatar, country, online, status, ms, conn, in a room;
//                                             then "game minutes code" per game (the code "NALE / 1234ABCD": its box art)
//   /v1/rank?g=&t=&offset=                    position name country avatar minutes online
//   /v1/week?g=&t=&list=&offset=              this week's ranking by time online: days left, my place, my seconds; the
//                                             top 10 (list=1: a page from offset) - place name country avatar seconds online
//   /v1/top?g=&t=&period=w|m|a                the most played games online - in rooms (this week, four weeks, all time):
//                                             "T place code game minutes players lastweek", then "M code minutes" mine
//   /v1/news?lang=                            date title text
//   /v1/rooms?g=&t=                           rooms waiting: code host avatar players game name locked
//   /v1/chat?g=&t=&since=|before=             the general chat after message `since` (or the 40 before message
//                                             `before`: its older messages): id tag avatar text ago (seconds)
//   /v1/say?g=&t=&text=                       a message to it
//   /v1/room?op=create&g=&t=&name=&pass=&game=&crc1=&crc2=&max= | op=join&code=&pass=&g=&t=   WebSocket: see Room below
//
// What is kept: gamertag, country, avatar, the token's hash, last time seen, friends, minutes per
// game. No addresses: a registration only leaves a salted hash of its address for an hour, to
// limit how many one place can make.

const ONLINE_S = 300;            // seen this recently: online (the consoles ping every two minutes)
const ROOM_NAME_MAX = 16;        // a room's name at most (LAIKA's keyboard takes as many: its chips fit after it)
const PLAY_STEP_S = 200;         // the time played counted from one ping to the next at most (they come each 90 s)
const PLAY_BANK_S = 86400;       // the time played seen and not yet claimed (/v1/played) at most
const PAGE = 50;                 // rows of users and rank per request
const MAX_FRIENDS = 100;
const MAX_REGISTER_PER_HOUR = 5; // per address
const TAG_RE = /^[A-Za-z0-9_]{3,15}$/;
const NAME_HOLD_S = 30 * 86400;  // a gamertag given up by a rename stays its old owner's this long (held_names): nobody
                                 // else takes it at once and gets the invitations and messages sent to that name
const PROTOCOLS = [2, 3, 4, 5, 6];  // xb_online.cpp ONLINE_PROTO values the rooms serve (3: a room's size, back to its lobby;
                                 // 4: each player's country and connection in "players"; 5: two or three on one console;
                                 // 6: "left" lines saying why a player went)
const LEAVE_WHY = ["left", "nogame", "gone", "kicked"];
const ROOM_LOG_S = 7 * 86400;    // the rooms' comings and goings (room_log) kept this long
const ROOM_PLAYERS = 4;
const LOBBY_USERS = 60;          // players online listed in /v1/lobby
const ROOM_WAIT_MS = 900000;     // a room nobody starts or talks in is closed after this long
const ROOM_BEAT_MS = 20000;      // a room looks at its consoles (and freshens its row in the list) this often
const ROOM_QUIET_MS = 30000;     // a console in the lobby silent this long is gone (they ping every 2 s) ...
const ROOM_PLAY_QUIET_MS = 60000; // ... in the game this long (directly connected ones ping the room every 20 s)
const ROOM_COUNTDOWN_S = 3;      // from the host's START to the game
const ROOM_LIST_S = 60;          // rooms whose row is older than this (gone without a word) are not listed
const CHAT_KEEP = 500;           // general chat messages kept
const CHAT_GAP_S = 3;            // a player's messages at least this far apart
const CHAT_LEN = 80;
const DM_LEN = 120;
const MAX_REQUESTS = 50;         // friendships one player has asked and not been answered
const INVITE_GAP_MS = 10000;     // one player's invitations to another at least this far apart
const INVITE_KEEP_MS = 600000;   // an invitation to a player without its inbox open waits this long
const INBOX_QUIET_MS = 90000;    // an inbox connection silent this long (no "p") is gone
const INBOX_SOCKETS = 3;         // connections kept per player (its newest)
const TOP_GAMES = 10;            // games in the most played (/v1/top)
const TOP_CACHE_S = 3600;        // ... read once in this long
const LOBBY_GATHER_MS = 1000;    // the lobby's changes gathered this long, then sent as one
const LOBBY_FRESH_MS = 30000;    // while consoles listen, the lobby looked at again this often (sent when it changed)
const LOBBY_QUIET_MS = 90000;    // a lobby connection silent this long (no "p") is gone
const LOBBY_PART = 3500;         // bytes of one message of the lobby at most (the console's frames: 4 KB)

const now = () => Math.floor(Date.now() / 1000);
// The weekly ranking (hours played online): weeks from Monday 00:00 UTC (1970-01-05 was one); a game counts from
// its start to the moment each console leaves it, at most WEEK_GAME_S, nothing under WEEK_MIN_S.
const WEEK_S = 7 * 86400, MONDAY0 = 4 * 86400, WEEK_GAME_S = 6 * 3600, WEEK_MIN_S = 30, WEEK_TOP = 10;
const weekOf = (t) => Math.floor((t - MONDAY0) / WEEK_S);
const clean = (text, limit) => String(text ?? "").replace(/[\t\r\n\x00-\x1f]/g, " ").trim().slice(0, limit);
const country = (text) => (/^[A-Za-z]{2}$/.test(text ?? "") ? text.toUpperCase() : "--");
const avatar = (text) => Math.max(0, Math.min(parseInt(text ?? "0", 10) || 0, 1023));   // 63 the team's, the colour ones from 64, the animated from 256
const reply = (lines, status = 200) =>
  new Response(lines.join("\n") + "\n.\n", { status, headers: { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" } });

async function sha256(text) {
  const hash = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(hash)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// The player a request is from (its id), or null: the gamertag and its token.
async function user(db, q) {
  const tag = q.get("g") ?? "", token = q.get("t") ?? "";
  if (!tag || !token) return null;
  const row = await db.prepare("SELECT id, token FROM users WHERE lower = ?").bind(tag.toLowerCase()).first();
  return row && row.token === (await sha256(token)) ? row.id : null;
}

async function rows(db, me, where, args, order, offset) {
  const t = now();
  let n = 1;   // the query's parameters numbered after ?1 (this player, used twice)
  const sql =
    "SELECT u.name, u.country, u.avatar, u.seen, u.status, u.ms, u.conn, " +
    "(CASE WHEN EXISTS (SELECT 1 FROM friends f WHERE f.user = ?1 AND f.friend = u.id) THEN 1 " +
    "WHEN EXISTS (SELECT 1 FROM requests r WHERE r.from_id = ?1 AND r.to_id = u.id) THEN 2 ELSE 0 END) AS friend FROM users u " +
    where + " ORDER BY " + order + " LIMIT ? OFFSET ?";
  const { results } = await db.prepare(sql.replace(/\?(?!1)/g, () => "?" + (++n))).bind(me, ...args, PAGE, offset).all();
  return results.map((u) => {
    const online = t - u.seen < ONLINE_S;
    return [u.name, u.country, u.avatar, online ? 1 : 0, online ? u.status : "", u.friend, online ? u.ms : 0, online ? u.conn : 0].join("\t");
  });
}

// Whether a gamertag (lower case) is held for another player than `me` (0: a new player), given up
// by a rename less than NAME_HOLD_S ago; expired holds are dropped now and then.
async function nameHeld(db, lower, me) {
  if (Math.random() < 0.02) await db.prepare("DELETE FROM held_names WHERE until < ?").bind(now()).run();
  const row = await db.prepare("SELECT owner FROM held_names WHERE lower = ? AND until >= ?").bind(lower, now()).first();
  return !!row && row.owner !== me;
}

async function register(db, q, request, env) {
  const tag = q.get("tag") ?? "";
  if (!TAG_RE.test(tag)) return ["ERR name"];
  const t = now();
  const place = await sha256((env.SALT ?? "laika") + (request.headers.get("cf-connecting-ip") ?? ""));
  await db.prepare("DELETE FROM registered WHERE at < ?").bind(t - 3600).run();
  const made = await db.prepare("SELECT COUNT(*) AS n FROM registered WHERE place = ?").bind(place).first();
  if (made.n >= MAX_REGISTER_PER_HOUR) return ["ERR busy"];
  if (await db.prepare("SELECT 1 FROM users WHERE lower = ?").bind(tag.toLowerCase()).first()) return ["ERR taken"];
  if (await nameHeld(db, tag.toLowerCase(), 0)) return ["ERR taken"];
  const token = [...crypto.getRandomValues(new Uint8Array(12))].map((b) => b.toString(16).padStart(2, "0")).join("");
  try {
    await db.batch([
      db.prepare("INSERT INTO users (name, lower, token, country, avatar, created, seen) VALUES (?, ?, ?, ?, ?, ?, ?)")
        .bind(tag, tag.toLowerCase(), await sha256(token), country(request.cf?.country) !== "--" ? country(request.cf?.country) : country(q.get("country")),
          avatar(q.get("avatar")), t, t),
      db.prepare("INSERT INTO registered (place, at) VALUES (?, ?)").bind(place, t),
    ]);
  } catch (e) {
    return ["ERR taken"];   // two consoles asked for the same gamertag at once
  }
  return ["OK\t" + token];
}

async function profiles(cmd, q, request, env) {
  const db = env.DB;
  if (cmd === "news") {
    const lang = (q.get("lang") ?? "en").toLowerCase();
    let { results } = await db.prepare("SELECT date, title, text FROM news WHERE lang = ? ORDER BY date DESC, id DESC LIMIT 20").bind(lang).all();
    if (!results.length && lang !== "en")
      ({ results } = await db.prepare("SELECT date, title, text FROM news WHERE lang = 'en' ORDER BY date DESC, id DESC LIMIT 20").all());
    return ["OK", ...results.map((n) => [n.date, clean(n.title, 300), clean(n.text, 300)].join("\t"))];
  }
  if (cmd === "register") return register(db, q, request, env);
  if (cmd === "whereami") return ["OK\t" + country(request.cf?.country)];
  const me = await user(db, q);
  if (me === null) return ["ERR auth"];
  const t = now();
  switch (cmd) {
    case "ping": {
      // The latest LAIKA 64 release (GitHub's, kept 10 minutes): told back as "u" - each console says so as a notice.
      const latest = await latestRelease();
      const ms = Math.max(0, Math.min(parseInt(q.get("ms") ?? "0", 10) || 0, 9999));
      // conn 3: a LAIKA before 2.1, which tells neither its time nor its connection (shown as an old version).
      const conn = q.has("conn") ? Math.max(0, Math.min(parseInt(q.get("conn"), 10) || 0, 2)) : 3;
      // The country: where the connection comes from, kept up to date (told back as "c").
      const cc = country(request.cf?.country);
      // The time played as the server sees it (playbank): the time since the last ping, when a game was open then or
      // is now (at most PLAY_STEP_S a ping; the bank at most PLAY_BANK_S). /v1/played takes its minutes from it.
      const status = clean(q.get("status"), 40);
      // The lobby's players change when this one comes online, plays something else or goes in or out of a room.
      const was = await db.prepare("SELECT seen, status, room FROM users WHERE id = ?").bind(me).first();
      const moved = !was || t - was.seen >= ONLINE_S || was.status !== status || was.room !== (q.get("room") === "1" ? 1 : 0);
      // playing: since when it plays what its status says (kept while the status stays; 0 when it plays nothing).
      await db.prepare("UPDATE users SET playbank = MIN(?, playbank + CASE WHEN status <> '' OR ? <> '' THEN MAX(0, MIN(? - seen, ?)) ELSE 0 END), " +
        "playing = CASE WHEN ? = '' THEN 0 WHEN status = ? AND playing > 0 THEN playing ELSE ? END, " +
        "seen = ?, status = ?, ms = ?, conn = ?, room = ?, country = CASE WHEN ? <> '--' THEN ? ELSE country END WHERE id = ?")
        .bind(PLAY_BANK_S, status, t, PLAY_STEP_S, status, status, t, t, status, ms, conn, q.get("room") === "1" ? 1 : 0, cc, cc, me).run();
      if (moved) await lobbyBump(env);
      const since = Math.max(0, parseInt(q.get("since") ?? "0", 10) || 0);
      const [friends, asked, msgs] = await db.batch([
        db.prepare("SELECT u.name, u.seen, u.avatar FROM friends f JOIN users u ON u.id = f.friend WHERE f.user = ?").bind(me),
        db.prepare("SELECT u.name, u.avatar FROM requests r JOIN users u ON u.id = r.from_id WHERE r.to_id = ? ORDER BY r.at LIMIT 20").bind(me),
        db.prepare("SELECT d.id, u.name, d.text FROM dm d JOIN users u ON u.id = d.from_id WHERE d.to_id = ? AND d.id > ? ORDER BY d.id LIMIT 20").bind(me, since),
      ]);
      // The messages handed to this console: delivered (the sender's two grey ticks).
      if (msgs.results.length)
        await db.prepare("UPDATE dm SET got = ? WHERE to_id = ? AND got = 0 AND id <= ?").bind(t, me, msgs.results[msgs.results.length - 1].id).run();
      return ["OK", ...(cc !== "--" ? ["c\t" + cc] : []), ...(latest ? ["u\t" + latest] : []), ...friends.results.map((f) => ["f", f.name, t - f.seen < ONLINE_S ? 1 : 0, f.avatar].join("\t")),
        ...asked.results.map((r) => ["r", r.name, r.avatar].join("\t")), ...msgs.results.map((m) => ["m", m.id, m.name, m.text].join("\t"))];
    }
    case "rename": {
      const tag = q.get("tag") ?? "";
      if (!TAG_RE.test(tag)) return ["ERR name"];
      const lower = tag.toLowerCase();
      const taken = await db.prepare("SELECT id FROM users WHERE lower = ?").bind(lower).first();
      if (taken && taken.id !== me) return ["ERR taken"];
      if (await nameHeld(db, lower, me)) return ["ERR taken"];
      const old = await db.prepare("SELECT lower FROM users WHERE id = ?").bind(me).first();
      const steps = [db.prepare("UPDATE users SET name = ?, lower = ? WHERE id = ?").bind(tag, lower, me),
        db.prepare("DELETE FROM held_names WHERE lower = ?").bind(lower)];   // its own old name taken back: no longer held
      // The name given up stays this player's for a while (a new spelling of the same name holds nothing).
      if (old && old.lower !== lower)
        steps.push(db.prepare("INSERT OR REPLACE INTO held_names (lower, owner, until) VALUES (?, ?, ?)").bind(old.lower, me, now() + NAME_HOLD_S));
      try {
        await db.batch(steps);
      } catch (e) {
        return ["ERR taken"];
      }
      return ["OK"];
    }
    case "profile": {
      // The country is the connection's when it is known (consoles before 2.0.1 still choose one).
      const cc = country(request.cf?.country) !== "--" ? country(request.cf?.country) : country(q.get("country"));
      await db.prepare("UPDATE users SET country = ?, avatar = ? WHERE id = ?").bind(cc, avatar(q.get("avatar")), me).run();
      return ["OK"];
    }
    case "users":
      // Online first, then by name.
      return ["OK", ...(await rows(db, me, "", [], "(u.seen > " + (t - ONLINE_S) + ") DESC, u.lower", Math.max(0, parseInt(q.get("offset") ?? "0", 10) || 0)))];
    case "friends":
      return ["OK", ...(await rows(db, me, "WHERE u.id IN (SELECT friend FROM friends WHERE user = ?)", [me], "u.seen DESC", 0))];
    case "friend": {
      const other = await db.prepare("SELECT id FROM users WHERE lower = ?").bind((q.get("name") ?? "").toLowerCase()).first();
      if (!other || other.id === me) return ["ERR name"];
      if (q.get("add") !== "1") {
        await db.batch([
          db.prepare("DELETE FROM friends WHERE (user = ?1 AND friend = ?2) OR (user = ?2 AND friend = ?1)").bind(me, other.id),
          db.prepare("DELETE FROM requests WHERE (from_id = ?1 AND to_id = ?2) OR (from_id = ?2 AND to_id = ?1)").bind(me, other.id),
        ]);
        return ["OK"];
      }
      if (await db.prepare("SELECT 1 FROM friends WHERE user = ? AND friend = ?").bind(me, other.id).first()) return ["OK\tfriends"];
      const have = await db.prepare("SELECT COUNT(*) AS n FROM friends WHERE user = ?").bind(me).first();
      if (have.n >= MAX_FRIENDS) return ["ERR full"];
      if (await db.prepare("SELECT 1 FROM requests WHERE from_id = ? AND to_id = ?").bind(other.id, me).first()) {
        await befriend(db, me, other.id);   // the other had asked too: friends
        return ["OK\taccepted"];
      }
      const asked = await db.prepare("SELECT COUNT(*) AS n FROM requests WHERE from_id = ?").bind(me).first();
      if (asked.n >= MAX_REQUESTS) return ["ERR full"];
      await db.prepare("INSERT OR IGNORE INTO requests (from_id, to_id, at) VALUES (?, ?, ?)").bind(me, other.id, t).run();
      return ["OK\tsent"];
    }
    case "requests": {
      const { results } = await db.prepare("SELECT u.name, u.country, u.avatar FROM requests r JOIN users u ON u.id = r.from_id WHERE r.to_id = ? ORDER BY r.at")
        .bind(me).all();
      return ["OK", ...results.map((u) => [u.name, u.country, u.avatar].join("\t"))];
    }
    case "answer": {
      const other = await db.prepare("SELECT id FROM users WHERE lower = ?").bind((q.get("name") ?? "").toLowerCase()).first();
      if (!other || !(await db.prepare("SELECT 1 FROM requests WHERE from_id = ? AND to_id = ?").bind(other.id, me).first())) return ["ERR name"];
      if (q.get("yes") === "1") await befriend(db, me, other.id);
      else await db.prepare("DELETE FROM requests WHERE from_id = ? AND to_id = ?").bind(other.id, me).run();
      return ["OK"];
    }
    case "dm": {
      const other = await db.prepare("SELECT id FROM users WHERE lower = ?").bind((q.get("to") ?? "").toLowerCase()).first();
      const text = clean(q.get("text"), DM_LEN);
      if (!other || !(await db.prepare("SELECT 1 FROM friends WHERE user = ? AND friend = ?").bind(me, other.id).first())) return ["ERR name"];
      if (!text) return ["ERR text"];
      const last = await db.prepare("SELECT MAX(at) AS at FROM dm WHERE from_id = ?").bind(me).first();
      if (last && last.at && t - last.at < 1) return ["ERR busy"];
      await db.prepare("INSERT INTO dm (from_id, to_id, at, text) VALUES (?, ?, ?, ?)").bind(me, other.id, t, text).run();
      return ["OK"];
    }
    case "conv": {
      const other = await db.prepare("SELECT id FROM users WHERE lower = ?").bind((q.get("with") ?? "").toLowerCase()).first();
      if (!other) return ["ERR name"];
      const since = Math.max(0, parseInt(q.get("since") ?? "0", 10) || 0);
      // Opened: the friend's messages to this player seen (their sender's blue ticks).
      await db.prepare("UPDATE dm SET seen = ?1, got = CASE WHEN got = 0 THEN ?1 ELSE got END WHERE to_id = ?2 AND from_id = ?3 AND seen = 0").bind(t, me, other.id).run();
      const { results } = await db.prepare("SELECT id, from_id, text, at, got, seen FROM dm WHERE ((from_id = ?1 AND to_id = ?2) OR (from_id = ?2 AND to_id = ?1)) AND id > ?3 " +
        "ORDER BY id DESC LIMIT 40").bind(me, other.id, since).all();
      // How long ago each was sent (seconds; as the general chat's): the console shows its time and day. Then, for the
      // player's own, its state (0 sent, 1 delivered, 2 seen; "" for the friend's): a LAIKA before 3.6.1 reads 4 fields.
      return ["OK", ...results.reverse().map((m) => [m.id, m.from_id === me ? 1 : 0, m.text, Math.max(0, t - m.at),
        m.from_id === me ? (m.seen ? 2 : m.got ? 1 : 0) : ""].join("\t"))];
    }
    case "lobby": {
      const since = Math.max(0, parseInt(q.get("since") ?? "0", 10) || 0);
      if (!q.has("v")) return ["OK", ...(await lobbyRows(db, t, since))];   // (a LAIKA before the pushed lobby)
      // The version asked: nothing read when the lobby has not changed since (the pushed lobby's fallback).
      const v = await lobbyVersion(env);
      if (v >= 0 && String(v) === q.get("v")) return ["OK", "S\t" + v];
      return ["OK", ...(await lobbyRows(db, t, since)), "V\t" + Math.max(0, v)];
    }
    case "invite": {
      // The room must be waiting; the invitation names its game (as the room's list has it), not the sender's words.
      const other = await db.prepare("SELECT id FROM users WHERE lower = ?").bind((q.get("to") ?? "").toLowerCase()).first();
      if (!other || other.id === me) return ["ERR name"];
      const code = q.get("code") ?? "";
      const room = /^[0-9]{6}$/.test(code) &&
        (await db.prepare("SELECT game, name FROM rooms WHERE code = ? AND started = 0 AND updated > ?").bind(code, t - ROOM_LIST_S).first());
      if (!room) return ["ERR none"];
      const who = await db.prepare("SELECT name FROM users WHERE id = ?").bind(me).first();
      const line = ["invite", who.name, clean(room.game, 40), code, clean(room.name, ROOM_NAME_MAX)].join("\t");
      const res = await env.INBOX.get(env.INBOX.idFromName("u" + other.id))
        .fetch("https://inbox/push?" + new URLSearchParams({ from: String(me), line }));
      const how = await res.text();
      return how === "busy" ? ["ERR busy"] : ["OK\t" + how];
    }
    case "played": {
      const asked = Math.max(0, Math.min(parseInt(q.get("minutes") ?? "0", 10) || 0, 24 * 60));   // one sitting: a day at most
      const code = clean(q.get("code"), 24), game = clean(q.get("game"), 40);
      // Only the time the server saw a game open (playbank, from the pings; the last stretch since the last ping
      // counted now): minutes asked for by hand, without playing, are not given.
      const u = await db.prepare("SELECT playbank, seen, status FROM users WHERE id = ?").bind(me).first();
      const pending = u && u.status ? Math.max(0, Math.min(t - u.seen, PLAY_STEP_S)) : 0;
      const bank = Math.min(PLAY_BANK_S, (u ? u.playbank : 0) + pending);
      const minutes = Math.min(asked, Math.floor(bank / 60));
      if (asked > minutes + 2) console.log("played: " + me + " asked " + asked + " min, the server saw " + Math.floor(bank / 60));
      await db.prepare("UPDATE users SET playbank = ?, seen = CASE WHEN ? > 0 THEN ? ELSE seen END WHERE id = ?")
        .bind(bank - minutes * 60, pending, t, me).run();
      if (minutes && code)
        await db.batch([
          db.prepare("INSERT INTO played (user, code, game, minutes) VALUES (?, ?, ?, ?) ON CONFLICT (user, code) DO UPDATE SET minutes = minutes + excluded.minutes, " +
            "game = excluded.game").bind(me, code, game, minutes),
          db.prepare("UPDATE users SET minutes = minutes + ? WHERE id = ?").bind(minutes, me),
          // the week's (the most played games of the week and month, /v1/top)
          db.prepare("INSERT INTO game_week (week, code, user, game, minutes) VALUES (?, ?, ?, ?, ?) ON CONFLICT (week, code, user) DO UPDATE SET " +
            "minutes = minutes + excluded.minutes, game = excluded.game").bind(weekOf(t), code, me, game, minutes),
        ]);
      return ["OK"];
    }
    case "top": {
      // The most played games online: this week (w), the last four weeks (m) or all time (a); TOP_GAMES of them, each
      // "T place code game minutes players lastweek" (lastweek: its place a week before, 0 none; this week's only),
      // then "M code minutes" for this player's own time with them. The ranking is read once an hour (top_cache).
      // Only games played in rooms count (online_week, the rooms' own count since 2026-11-24): with the time of any game
      // open while online (played, game_week) the ranking led with games played alone (Ocarina of Time).
      const period = ["w", "m", "a"].includes(q.get("period")) ? q.get("period") : "w";
      let body = null;
      const cached = await db.prepare("SELECT at, body FROM top_cache WHERE period = ?").bind(period).first();
      if (cached && t - cached.at < TOP_CACHE_S) body = cached.body;
      else {
        // The name of each game: the newest kind of name wins - a LAIKA from 3.5 on sends a hack's file name with its
        // version ("Smash Remix 2.0.1"), older ones its header's ("SMASH REMIX"); MAX keeps the mixed case one.
        const wk = weekOf(t);
        const sql = (where) => "SELECT code, MAX(game) AS game, SUM(minutes) AS m, COUNT(DISTINCT user) AS n FROM " + where +
          " GROUP BY code ORDER BY m DESC LIMIT " + TOP_GAMES;
        const top = period === "a" ? await db.prepare(sql("online_week")).all()
          : await db.prepare(sql("online_week WHERE week >= ?")).bind(period === "w" ? wk : wk - 3).all();
        let last = new Map();
        if (period === "w") {
          const prev = await db.prepare(sql("online_week WHERE week = ?")).bind(wk - 1).all();
          prev.results.forEach((r, i) => last.set(r.code, i + 1));
        }
        body = top.results.map((r, i) => ["T", i + 1, r.code, r.game, r.m, r.n, last.get(r.code) ?? 0].join("\t")).join("\n");
        await db.prepare("INSERT INTO top_cache (period, at, body) VALUES (?, ?, ?) ON CONFLICT (period) DO UPDATE SET at = excluded.at, body = excluded.body")
          .bind(period, t, body).run();
      }
      const lines = body ? body.split("\n") : [];
      const codes = lines.map((l) => l.split("\t")[2]);
      let mine = [];
      if (codes.length) {
        const marks = codes.map(() => "?").join(",");
        mine = (await db.prepare("SELECT code, SUM(minutes) AS m FROM online_week WHERE user = ? AND week >= ? AND code IN (" + marks + ") GROUP BY code")
          .bind(me, period === "w" ? weekOf(t) : period === "m" ? weekOf(t) - 3 : 0, ...codes).all()).results;
      }
      return ["OK", ...lines, ...mine.map((r) => ["M", r.code, r.m].join("\t"))];
    }
    case "stats": {
      const who = await db.prepare("SELECT id, minutes, avatar, country, seen, status, ms, conn, room FROM users WHERE lower = ?")
        .bind((q.get("name") ?? "").toLowerCase()).first();
      if (!who) return ["ERR name"];
      const [games, count, above, friend, together] = await db.batch([
        db.prepare("SELECT game, minutes, code FROM played WHERE user = ? ORDER BY minutes DESC LIMIT 20").bind(who.id),
        db.prepare("SELECT COUNT(*) AS n FROM played WHERE user = ?").bind(who.id),
        db.prepare("SELECT COUNT(*) AS n FROM users WHERE minutes > ?").bind(who.minutes),
        db.prepare("SELECT at FROM friends WHERE user = ? AND friend = ?").bind(me, who.id),
        db.prepare("SELECT games FROM together WHERE a = ? AND b = ?").bind(Math.min(me, who.id), Math.max(me, who.id)),
      ]);
      const f = friend.results[0], online = t - who.seen < ONLINE_S;
      const head = [who.minutes, count.results[0].n, above.results[0].n + 1, f ? 1 : 0, f && f.at ? Math.floor((t - f.at) / 86400) : -1,
        together.results[0] ? together.results[0].games : 0, who.avatar, who.country, online ? 1 : 0, online ? who.status : "",
        online ? who.ms : 0, online ? who.conn : 0, online && who.room ? 1 : 0];
      return ["OK", head.join("\t"), ...games.results.map((g) => [g.game, g.minutes, g.code].join("\t"))];
    }
    case "rooms": {
      const { results } = await db.prepare("SELECT code, host, avatar, players, game, name, locked FROM rooms WHERE updated > ? AND started = 0 ORDER BY updated DESC LIMIT 40")
        .bind(t - ROOM_LIST_S).all();
      return ["OK", ...results.map((r) => [r.code, r.host, r.avatar, r.players, r.game, r.name, r.locked].join("\t"))];
    }
    case "chat": {
      // before: the 40 messages before that one (the lobby's older messages), else the last ones after `since`.
      const since = Math.max(0, parseInt(q.get("since") ?? "0", 10) || 0);
      const before = Math.max(0, parseInt(q.get("before") ?? "0", 10) || 0);
      const { results } = await (before
        ? db.prepare("SELECT id, tag, avatar, text, at FROM chat WHERE id < ? ORDER BY id DESC LIMIT 40").bind(before)
        : db.prepare("SELECT id, tag, avatar, text, at FROM chat WHERE id > ? ORDER BY id DESC LIMIT 40").bind(since)).all();
      return ["OK", ...results.reverse().map((m) => [m.id, m.tag, m.avatar, m.text, Math.max(0, t - m.at)].join("\t"))];
    }
    case "say": {
      const text = clean(q.get("text"), CHAT_LEN);
      if (!text) return ["ERR text"];
      const last = await db.prepare("SELECT at FROM said WHERE user = ?").bind(me).first();
      if (last && t - last.at < CHAT_GAP_S) return ["ERR busy"];
      const who = await db.prepare("SELECT name, avatar FROM users WHERE id = ?").bind(me).first();
      await db.batch([
        db.prepare("INSERT INTO chat (at, tag, avatar, text) VALUES (?, ?, ?, ?)").bind(t, who.name, who.avatar, text),
        db.prepare("INSERT INTO said (user, at) VALUES (?, ?) ON CONFLICT (user) DO UPDATE SET at = excluded.at").bind(me, t),
        db.prepare("DELETE FROM chat WHERE id <= (SELECT MAX(id) FROM chat) - ?").bind(CHAT_KEEP),
      ]);
      await lobbyBump(env);   // the general chat: the lobby's consoles see it at once
      return ["OK"];
    }
    case "week": {
      // This week's ranking by time played online: the days left, this player's place and seconds, then the first
      // WEEK_TOP (place name country avatar seconds online) - with &list=1 a page of PAGE from &offset= (Players' tab).
      const week = weekOf(t), list = q.get("list") === "1";
      const offset = list ? Math.max(0, parseInt(q.get("offset") ?? "0", 10) || 0) : 0;
      const [top, mine] = await db.batch([
        db.prepare("SELECT u.name, u.country, u.avatar, u.seen, w.seconds FROM weekly w JOIN users u ON u.id = w.user WHERE w.week = ? " +
          "ORDER BY w.seconds DESC, w.user LIMIT ? OFFSET ?").bind(week, list ? PAGE : WEEK_TOP, offset),
        db.prepare("SELECT seconds, (SELECT COUNT(*) FROM weekly o WHERE o.week = ?1 AND o.seconds > w.seconds) AS above " +
          "FROM weekly w WHERE w.week = ?1 AND w.user = ?2").bind(week, me),
      ]);
      const m = mine.results[0], left = Math.max(1, Math.ceil((MONDAY0 + (week + 1) * WEEK_S - t) / 86400));
      return ["OK\t" + [left, m ? m.above + 1 : 0, m ? m.seconds : 0].join("\t"),
        ...top.results.map((u, i) => [offset + i + 1, u.name, u.country, u.avatar, u.seconds, t - u.seen < ONLINE_S ? 1 : 0].join("\t"))];
    }
    case "rank": {
      const offset = Math.max(0, parseInt(q.get("offset") ?? "0", 10) || 0);
      const { results } = await db.prepare("SELECT name, country, avatar, minutes, seen FROM users WHERE minutes > 0 ORDER BY minutes DESC, id LIMIT ? OFFSET ?")
        .bind(PAGE, offset).all();
      return ["OK", ...results.map((u, i) => [offset + i + 1, u.name, u.country, u.avatar, u.minutes, t - u.seen < ONLINE_S ? 1 : 0].join("\t"))];
    }
  }
  return ["ERR command"];
}

// Two players friends both ways; what either had asked is answered.
async function befriend(db, a, b) {
  await db.batch([
    db.prepare("INSERT OR IGNORE INTO friends (user, friend, at) VALUES (?, ?, ?)").bind(a, b, now()),
    db.prepare("INSERT OR IGNORE INTO friends (user, friend, at) VALUES (?, ?, ?)").bind(b, a, now()),
    db.prepare("DELETE FROM requests WHERE (from_id = ?1 AND to_id = ?2) OR (from_id = ?2 AND to_id = ?1)").bind(a, b),
  ]);
}

// The latest release's version, from GitHub's redirect to its tag (as each console's own check reads it); asked again
// after 10 minutes, the last one known kept meanwhile and when GitHub does not answer.
const LATEST_URL = "https://github.com/ErickaguirreD/Laika64/releases/latest";
let latestCache = { v: "", at: 0 };
async function latestRelease() {
  const t = Date.now();
  if (t - latestCache.at < 600000) return latestCache.v;
  latestCache.at = t;
  try {
    const r = await fetch(LATEST_URL, { redirect: "manual", headers: { "User-Agent": "laika64-server" } });
    const m = (r.headers.get("Location") || "").match(/\/tag\/v?([0-9]+(?:\.[0-9]+){1,2})$/);
    if (m) latestCache.v = m[1];
  } catch (e) {
    // (the last one known stays)
  }
  return latestCache.v;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const m = /^\/v1\/([a-z]+)$/.exec(url.pathname);
    if (!m) return new Response("LAIKA 64 online\n", { headers: { "content-type": "text/plain" } });
    if (m[1] === "room") return room(request, url, env);
    if (m[1] === "inbox") return inbox(request, url, env);
    if (m[1] === "lobbyws") return lobbySocket(request, url, env);
    try {
      return reply(await profiles(m[1], url.searchParams, request, env));
    } catch (e) {
      console.log("request failed: " + e);
      // The free plan's daily reads used up (D1 code 7500): said so, the consoles show when it comes back.
      if (/7500|daily row read limit/i.test(String(e))) return reply(["ERR quota"], 503);
      return reply(["ERR server"], 500);
    }
  },
};

// ---- Rooms: two to four consoles meet with a six-digit code ----
// A console opens a WebSocket with its gamertag and token: /v1/room?op=create&name=<room>&pass=<password>
// &game=<name>&crc1=<hex>&crc2=<hex>&max=<2..4>&proto=3 makes a room for that many players (text "created
// <code>"; with a password it is listed locked), /v1/room?op=join&code=<code>&pass=<password>&proto=3 joins
// one. Every room is listed (/v1/lobby): waiting, or its game being played (who plays it, since when). While
// the room waits the server sends text lines:
//     game\t<crc1>\t<crc2>\t<game>\t<room>\t<max>   the room's game, name and size (to whoever joins)
//     players\t<mine>\t<tag>\t<avatar>\t<ms>...   who is in it, the host first; <mine>: this console's place
//                                           (0..); <ms>: each one's time to the server and back (0: not known);
//                                           from proto 4 on also \t<country>\t<conn> each (conn: 1 cable, 2 Wi-Fi),
//                                           from 5 on also \t<local> (how many play on that console)
//     chat\t<tag>\t<text>                   a message of the room's chat (in the game too)
//     left\t<tag>\t<why>                    from 6 on: a player went (before the new "players"); why: "left" (by
//                                           itself), "nogame" (no such ROM), "gone" (silent too long), "kicked"
//     count\t<seconds>                       the host started: the game begins in that many seconds
//     start\t<players>\t<port>\t<local>\t<owners>   the game starts: how many play, this console's first N64 port
//                                           (1..), how many ports it has, each port's console ("1,1,3")
//     status\t<port>\t<tag>\t<what>         in the game: a player opened ("menu") or left ("play") its menu
//     pong\t<n>                             the answer to this console's "ping\t<n>"
//     p2p\t<port>\t<what>                    in the game: a console's word about talking directly (its addresses,
//                                           the ports it reaches, or "off"): the consoles agree among themselves
//     again\t<port>\t<tag>                  in the game: a console that lost its connection is back (Room.rejoin)
//     rejoined\t<port>                      to that console: its place in the game again
// (/v1/room?op=rejoin&code=<code>&port=<n>&local=<n>&players=<n>&who=<port:tag,...>&game=...&crc1=&crc2=&name=
// &locked=<0|1>: a console of a game the room lost while it played on directly; the room made again if gone.)
// and takes "chat\t<text>", "ping\t<n>", "rtt\t<ms>" (its time to the server, measured with the pings),
// "bye\t<why>" (why it is about to close: "nogame") from anyone, "local\t<1-3>" (how many play on it) before the game, "status\t<what>", "p2p\t<what>" and "back" in
// the game, and "start" (two players at least) and "kick\t<place>" from
// the host before the game. A player put out ("kicked") may come back. From the start on every binary
// message of a console goes to all the others in the game with its first byte set to the sender's port:
// LAIKA's netplay messages (main.cpp NetMsg, whose type is small). A console that leaves the game is told
// to the others as a message of type BYE (4) from its port - whether it closed its connection or said
// "back": out of the game, back in the room's lobby (it gets "players" lines again). When nobody plays any
// more the room waits again with whoever is left, the first of them its host. A text word ("none", "full",
// "started", "closed", "old", "expired", "auth", "busy", "password", "kicked") comes before a close that
// says why. Each room is one Durable Object, named by its code, and listed in D1 while it lasts.
async function room(request, url, env) {
  if (request.headers.get("upgrade") !== "websocket") return new Response("WebSocket only\n", { status: 426 });
  const q = url.searchParams;
  if (!PROTOCOLS.includes(parseInt(q.get("proto") ?? "0", 10))) return refuse("old");
  const me = await user(env.DB, q);
  if (me === null) return refuse("auth");
  const who = await env.DB.prepare("SELECT name, avatar, country FROM users WHERE id = ?").bind(me).first();
  const args = new URLSearchParams({ tag: who.name, avatar: String(who.avatar), country: who.country, proto: q.get("proto") ?? "0",
    conn: String(Math.max(0, Math.min(parseInt(q.get("conn") ?? "0", 10) || 0, 2))) });
  args.set("pass", clean(q.get("pass"), 16));
  if (q.get("op") === "create") {
    args.set("name", clean(q.get("name"), ROOM_NAME_MAX) || who.name);
    args.set("game", clean(q.get("game"), 40));
    args.set("crc1", clean(q.get("crc1"), 8));
    args.set("crc2", clean(q.get("crc2"), 8));
    args.set("art", /^[A-Z0-9]{4}$|^X[0-9A-F]{8}$/.test(q.get("art") ?? "") ? q.get("art") : "");   // its box art's key (xb_art.cpp)
    args.set("size", String(Math.max(2, Math.min(parseInt(q.get("max") ?? "", 10) || ROOM_PLAYERS, ROOM_PLAYERS))));
    // A code no room is using: a few tries among 900000.
    for (let k = 0; k < 8; k++) {
      const code = String(100000 + Math.floor(Math.random() * 900000));
      args.set("code", code);
      const res = await env.ROOMS.get(env.ROOMS.idFromName(code)).fetch("https://room/create?" + args, request);
      if (res.status !== 409) return res;
    }
    return refuse("busy");
  }
  const code = q.get("code") ?? "";
  if (!/^[0-9]{6}$/.test(code)) return refuse("none");
  if (q.get("op") === "rejoin") {
    // A console of a game back after losing its connection (Room.rejoin); what it knows of the game, should the
    // room be gone from the server.
    args.set("code", code);
    for (const k of ["port", "local", "players", "locked"]) args.set(k, String(parseInt(q.get(k) ?? "", 10) || 0));
    args.set("who", clean(q.get("who"), 120));
    args.set("name", clean(q.get("name"), ROOM_NAME_MAX) || who.name);
    args.set("game", clean(q.get("game"), 40));
    args.set("crc1", clean(q.get("crc1"), 8));
    args.set("crc2", clean(q.get("crc2"), 8));
    return env.ROOMS.get(env.ROOMS.idFromName(code)).fetch("https://room/rejoin?" + args, request);
  }
  return env.ROOMS.get(env.ROOMS.idFromName(code)).fetch("https://room/join?" + args, request);
}

// ---- Inboxes: one Durable Object per player, holding its consoles' WebSockets (hibernated while quiet) ----
async function inbox(request, url, env) {
  if (request.headers.get("upgrade") !== "websocket") return new Response("WebSocket only\n", { status: 426 });
  const me = await user(env.DB, url.searchParams);
  if (me === null) return refuse("auth");
  return env.INBOX.get(env.INBOX.idFromName("u" + me)).fetch("https://inbox/open", request);
}

export class Inbox {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.sent = new Map();   // sender -> its last invitation (forgotten when the object sleeps: no harm)
    state.setWebSocketAutoResponse(new WebSocketRequestResponsePair("p", "q"));   // answered without waking it
  }

  // The connections still alive: those that said "p" (or opened) within INBOX_QUIET_MS; the others closed.
  alive() {
    const t = Date.now();
    return this.state.getWebSockets().filter((ws) => {
      const at = Math.max(this.state.getWebSocketAutoResponseTimestamp(ws)?.getTime() ?? 0, ws.deserializeAttachment()?.at ?? 0);
      if (t - at < INBOX_QUIET_MS) return true;
      try {
        ws.close(1000, "gone");
      } catch (e) {}
      return false;
    });
  }

  async fetch(request) {
    const url = new URL(request.url), q = url.searchParams;
    if (url.pathname === "/open") {
      const pair = new WebSocketPair();
      const open = this.alive().sort((a, b) => (a.deserializeAttachment()?.at ?? 0) - (b.deserializeAttachment()?.at ?? 0));
      for (const ws of open.slice(0, Math.max(0, open.length - INBOX_SOCKETS + 1))) {
        try {
          ws.close(1000, "again");
        } catch (e) {}
      }
      this.state.acceptWebSocket(pair[1]);
      pair[1].serializeAttachment({ at: Date.now() });
      // The invitations that came while no console listened, if still fresh.
      const waiting = (await this.state.storage.get("waiting")) ?? [];
      for (const w of waiting) if (Date.now() - w.at < INVITE_KEEP_MS) pair[1].send(w.line);
      if (waiting.length) await this.state.storage.delete("waiting");
      return new Response(null, { status: 101, webSocket: pair[0] });
    }
    if (url.pathname === "/push") {
      const from = q.get("from") ?? "", line = q.get("line") ?? "";
      if (Date.now() - (this.sent.get(from) ?? 0) < INVITE_GAP_MS) return new Response("busy");
      this.sent.set(from, Date.now());
      const open = this.alive();
      for (const ws of open) {
        try {
          ws.send(line);
        } catch (e) {}
      }
      if (open.length) return new Response("live");
      const waiting = ((await this.state.storage.get("waiting")) ?? []).filter((w) => Date.now() - w.at < INVITE_KEEP_MS).slice(-4);
      waiting.push({ at: Date.now(), line });
      await this.state.storage.put("waiting", waiting);
      return new Response("later");
    }
    return new Response("?", { status: 404 });
  }

  webSocketMessage() {}   // only "p", answered by itself

  webSocketClose(ws, code, reason) {
    try {
      ws.close(code, reason);
    } catch (e) {}
  }

  webSocketError() {}
}

// ---- The lobby pushed: one Durable Object holding the WebSockets of the consoles in Play online (hibernated while
// quiet). What changes the lobby (a room made, filled, started or closed; the general chat; a player coming online
// or changing what it plays) tells it (/bump); a moment later (LOBBY_GATHER_MS, so a burst is one) it reads the lobby
// from D1 once and sends it to every console, in parts (each under LOBBY_PART bytes: the console's frames are 4 KB):
//     L\t<version>\t<part>\t<parts>\n<lines of /v1/lobby, the chat its last messages>
// While consoles listen it looks again every LOBBY_FRESH_MS (times to the server, players gone without a word) and
// sends only what changed. Its version (/version) answers /v1/lobby?v= without D1: "S" when nothing changed.
async function lobbyRows(db, t, since) {
  // The host's time to the server with each room waiting (a console adds its own: its time to the host).
  const [rooms, chat, online] = await db.batch([
    db.prepare("SELECT r.code, r.host, r.avatar, r.players, r.game, r.name, r.locked, r.size, r.started, r.tags, r.art, r.crc, " +
      "(SELECT u.ms FROM users u WHERE u.lower = lower(r.host)) AS ms FROM rooms r WHERE r.updated > ? ORDER BY r.started, r.updated DESC LIMIT 60")
      .bind(t - ROOM_LIST_S),
    db.prepare("SELECT id, tag, avatar, text, at FROM chat WHERE id > ? ORDER BY id DESC LIMIT 30").bind(since),
    db.prepare("SELECT name, country, avatar, status, room, ms, conn, playing FROM users WHERE seen > ? ORDER BY (status <> '') DESC, room DESC, lower LIMIT ?")
      .bind(t - ONLINE_S, LOBBY_USERS),
  ]);
  return [
    ...rooms.results.filter((r) => !r.started).map((r) => ["R", r.code, r.host, r.avatar, r.players, r.game, r.name, r.locked, r.size, r.ms ?? 0, r.art, r.crc ?? ""].join("\t")),
    ...rooms.results.filter((r) => r.started).map((r) => ["G", r.code, r.game, Math.max(0, t - r.started), r.tags, r.art].join("\t")),
    ...online.results.map((u) => ["U", u.name, u.country, u.avatar, u.status, u.room, u.ms, u.conn, u.playing ? Math.max(0, t - u.playing) : 0].join("\t")),
    ...chat.results.reverse().map((m) => ["C", m.id, m.tag, m.avatar, m.text, Math.max(0, t - m.at)].join("\t"))];
}

// The lobby's object, told something changed (from a request, or a room's object). Never waited on for long: a
// failure only means the consoles see it at the next look.
function lobbyBump(env) {
  try {
    return env.LOBBY.get(env.LOBBY.idFromName("lobby")).fetch("https://lobby/bump").catch(() => {});
  } catch (e) {
    return Promise.resolve();
  }
}

async function lobbyVersion(env) {
  try {
    return parseInt(await (await env.LOBBY.get(env.LOBBY.idFromName("lobby")).fetch("https://lobby/version")).text(), 10) || 0;
  } catch (e) {
    return -1;
  }
}

async function lobbySocket(request, url, env) {
  if (request.headers.get("upgrade") !== "websocket") return new Response("WebSocket only\n", { status: 426 });
  const me = await user(env.DB, url.searchParams);
  if (me === null) return refuse("auth");
  return env.LOBBY.get(env.LOBBY.idFromName("lobby")).fetch("https://lobby/open", request);
}

export class Lobby {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.sent = "";   // the lines last sent (forgotten when it sleeps: sent again then, no harm)
    state.setWebSocketAutoResponse(new WebSocketRequestResponsePair("p", "q"));   // answered without waking it
  }

  async version() {
    if (this.v === undefined) this.v = (await this.state.storage.get("v")) ?? 0;
    return this.v;
  }

  // The connections still alive: those that said "p" (or opened) within LOBBY_QUIET_MS; the others closed.
  alive() {
    const t = Date.now();
    return this.state.getWebSockets().filter((ws) => {
      const at = Math.max(this.state.getWebSocketAutoResponseTimestamp(ws)?.getTime() ?? 0, ws.deserializeAttachment()?.at ?? 0);
      if (t - at < LOBBY_QUIET_MS) return true;
      try {
        ws.close(1000, "gone");
      } catch (e) {}
      return false;
    });
  }

  // The lobby in parts, each its own message.
  parts(v, lines) {
    const chunks = [], enc = new TextEncoder();   // (bytes, as the console counts them: a name's accents take two)
    let cur = [], size = 0;
    for (const l of lines) {
      const bytes = enc.encode(l).length + 1;
      if (cur.length && size + bytes > LOBBY_PART) {
        chunks.push(cur);
        cur = [];
        size = 0;
      }
      cur.push(l);
      size += bytes;
    }
    chunks.push(cur);
    return chunks.map((c, i) => ["L", v, i, chunks.length].join("\t") + "\n" + c.join("\n"));
  }

  async send(sockets, force) {
    const lines = await lobbyRows(this.env.DB, now(), 0);
    const text = lines.join("\n");
    if (!force && text === this.sent) return;   // (the periodic look: nothing new)
    if (text !== this.sent) {
      this.v = (await this.version()) + 1;
      await this.state.storage.put("v", this.v);
      this.sent = text;
    }
    const msgs = this.parts(await this.version(), lines);
    for (const ws of sockets)
      for (const m of msgs) {
        try {
          ws.send(m);
        } catch (e) {}
      }
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/open") {
      const pair = new WebSocketPair();
      this.state.acceptWebSocket(pair[1]);
      pair[1].serializeAttachment({ at: Date.now() });
      // The lobby as it is, to this one at once (from the last read when fresh, else read now).
      try {
        if (this.sent && Date.now() - (this.sentAt ?? 0) < LOBBY_FRESH_MS) {
          for (const m of this.parts(await this.version(), this.sent.split("\n"))) pair[1].send(m);
        } else {
          await this.send([pair[1]], true);
          this.sentAt = Date.now();
        }
      } catch (e) {
        console.log("lobby open: " + e);
      }
      if (!(await this.state.storage.getAlarm())) await this.state.storage.setAlarm(Date.now() + LOBBY_FRESH_MS);
      return new Response(null, { status: 101, webSocket: pair[0] });
    }
    if (url.pathname === "/bump") {
      // Something changed: the version moves now (the consoles that poll see it), the lists go out a moment later.
      this.v = (await this.version()) + 1;
      await this.state.storage.put("v", this.v);
      const at = await this.state.storage.getAlarm();
      if (!at || at > Date.now() + LOBBY_GATHER_MS) await this.state.storage.setAlarm(Date.now() + LOBBY_GATHER_MS);
      return new Response("ok");
    }
    if (url.pathname === "/version") return new Response(String(await this.version()));
    return new Response("?", { status: 404 });
  }

  async alarm() {
    const open = this.alive();
    if (!open.length) return;   // nobody listens: nothing read (the next console reads it as it opens)
    try {
      await this.send(open, false);
      this.sentAt = Date.now();
    } catch (e) {
      console.log("lobby send: " + e);
    }
    await this.state.storage.setAlarm(Date.now() + LOBBY_FRESH_MS);
  }

  webSocketMessage() {}   // only "p", answered by itself

  webSocketClose(ws, code, reason) {
    try {
      ws.close(code, reason);
    } catch (e) {}
  }

  webSocketError() {}
}

// A WebSocket that says why and closes.
function refuse(why) {
  const pair = new WebSocketPair();
  pair[1].accept();
  pair[1].send(why);
  pair[1].close(1000, why);
  return new Response(null, { status: 101, webSocket: pair[0] });
}

export class Room {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.players = [];        // { ws, tag, avatar, port, rtt, playing, seen }: the host first
    this.open = false;        // made, not closed
    this.started = false;     // a game is being played (by those with `playing`)
    this.last = 0;            // the last message of anyone
  }

  async fetch(request) {
    const url = new URL(request.url), q = url.searchParams;
    const create = url.pathname === "/create";
    if (url.pathname === "/rejoin") return this.rejoin(q);
    if (create && this.open) return new Response("taken", { status: 409 });
    if (!create) {
      if (!this.open) return refuse("none");
      if (this.started) return refuse("started");
      // The same player again (its last connection not closed yet): that one is let go, not counted twice.
      const old = this.players.findIndex((o) => o.tag === q.get("tag"));
      if (old > 0) {
        const o = this.players.splice(old, 1)[0];
        try {
          o.ws.close(1000, "again");
        } catch (e) {}
      }
      if (this.total() >= this.size) {
        await this.log(q.get("tag"), "refused full");
        return refuse("full");
      }
      if (this.pass && (await sha256(this.code + (q.get("pass") ?? ""))) !== this.pass) {
        await this.log(q.get("tag"), "refused password");
        return refuse("password");
      }
    }
    const pair = new WebSocketPair();
    const ws = pair[1];
    ws.accept();
    ws.binaryType = "arraybuffer";   // the consoles' messages as they are (not Blobs, which arrive later)
    const p = { ws, tag: q.get("tag"), avatar: parseInt(q.get("avatar"), 10) || 0, port: 0, rtt: 0, playing: false,
      country: q.get("country") || "--", conn: parseInt(q.get("conn"), 10) || 0, proto: parseInt(q.get("proto"), 10) || 0, seen: Date.now(), local: 1 };
    this.last = Date.now();
    if (create) {
      this.open = true;
      this.started = false;
      this.counting = false;
      this.code = q.get("code");
      this.name = q.get("name") ?? "";
      this.size = Math.max(2, Math.min(parseInt(q.get("size"), 10) || ROOM_PLAYERS, ROOM_PLAYERS));
      this.pass = q.get("pass") ? await sha256(this.code + q.get("pass")) : "";   // kept hashed
      this.game = { name: q.get("game") ?? "", crc1: q.get("crc1") ?? "", crc2: q.get("crc2") ?? "", art: q.get("art") ?? "" };
      this.players = [p];
      ws.send("created " + this.code);
    } else this.players.push(p);
    await this.log(p.tag, create ? "created" : "joined");
    ws.send(["game", this.game.crc1, this.game.crc2, this.game.name, this.name, this.size].join("\t"));
    this.roster();
    await this.list();
    await this.state.storage.setAlarm(Date.now() + ROOM_BEAT_MS);
    this.listen(p, ws);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  // A player's connection: its messages, and its end (not that of a connection it was given back in, Room.rejoin).
  listen(p, ws) {
    ws.addEventListener("message", (e) => this.message(p, e.data));
    const gone = () => {
      if (p.ws === ws) this.leave(p);
    };
    ws.addEventListener("close", gone);
    ws.addEventListener("error", gone);
  }

  // A console of the game back after losing its connection to the room (the consoles went on directly): its place
  // in the game again ("rejoined\t<port>"), the others told ("again\t<port>\t<tag>": it did not leave). Its old
  // connection, still here when the server had not seen it go, is let go. A room the server no longer has (it
  // restarted, or every console lost it) is made again from what the console knows: the game playing, `who` the
  // consoles in it ("1:TAG,3:TAG2", their first ports).
  async rejoin(q) {
    const tag = q.get("tag") ?? "", port = parseInt(q.get("port"), 10) || 0;
    const local = Math.max(1, Math.min(parseInt(q.get("local"), 10) || 1, 3));
    if (!this.open) {
      const who = new Map();
      for (const w of (q.get("who") ?? "").split(",")) {
        const at = w.indexOf(":");
        if (at > 0) who.set(parseInt(w.slice(0, at), 10) || 0, w.slice(at + 1));
      }
      if (port < 1 || (who.get(port) ?? "").toLowerCase() !== tag.toLowerCase()) return refuse("none");
      this.open = true;
      this.started = true;
      this.counting = false;
      this.code = q.get("code");
      this.name = q.get("name") ?? "";
      this.size = Math.max(2, Math.min(parseInt(q.get("players"), 10) || ROOM_PLAYERS, ROOM_PLAYERS));
      // Its password is not known here: a locked room stays locked (to anyone new).
      this.pass = q.get("locked") === "1" ? await sha256(this.code + crypto.randomUUID()) : "";
      this.game = { name: q.get("game") ?? "", crc1: q.get("crc1") ?? "", crc2: q.get("crc2") ?? "", art: "" };
      this.startedAt = now();
      this.players = [];
      this.inGame = who;
      await this.log(tag, "made again");
    }
    if (!this.started || (this.inGame?.get(port) ?? "").toLowerCase() !== tag.toLowerCase()) return refuse("none");
    const pair = new WebSocketPair();
    const ws = pair[1];
    ws.accept();
    ws.binaryType = "arraybuffer";
    let p = this.players.find((o) => o.playing && o.port === port);
    if (p) {
      const old = p.ws;
      p.ws = ws;
      try {
        old.close(1000, "again");
      } catch (e) {}
    } else {
      p = { ws, tag, avatar: parseInt(q.get("avatar"), 10) || 0, port, rtt: 0, playing: true, country: q.get("country") || "--",
        conn: parseInt(q.get("conn"), 10) || 0, proto: parseInt(q.get("proto"), 10) || 0, seen: Date.now(), local, since: now() };
      this.players.push(p);
      for (const o of this.playing()) if (o !== p) this.send(o, ["again", port, tag].join("\t"));
    }
    this.last = p.seen = Date.now();
    ws.send("rejoined\t" + port);
    await this.log(tag, "rejoined");
    await this.list();
    await this.state.storage.setAlarm(Date.now() + ROOM_BEAT_MS);
    this.listen(p, ws);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  send(p, data) {
    try {
      p.ws.send(data);
    } catch (e) {}
  }

  // A coming or going of this room in room_log (who, what, how many were in it then); older rows dropped now and then.
  async log(tag, what) {
    try {
      await this.env.DB.prepare("INSERT INTO room_log (at, code, tag, what, players, started) VALUES (?, ?, ?, ?, ?, ?)")
        .bind(now(), this.code ?? "", tag ?? "", what, this.players.length, this.started ? 1 : 0).run();
      if (Math.random() < 0.02) await this.env.DB.prepare("DELETE FROM room_log WHERE at < ?").bind(now() - ROOM_LOG_S).run();
    } catch (e) {
      console.log("room log: " + e);
    }
  }

  playing() {
    return this.players.filter((p) => p.playing);
  }

  // The players in the room (a console may have two or three): the places they take.
  total() {
    return this.players.reduce((n, p) => n + p.local, 0);
  }

  // Who is in the room, to those in its lobby (not to the consoles in the game): tag avatar ms, and from
  // protocol 4 on its country and connection too.
  roster() {
    const list = this.players.flatMap((p) => [p.tag, p.avatar, p.rtt]);
    const list4 = this.players.flatMap((p) => [p.tag, p.avatar, p.rtt, p.country, p.conn]);
    const list5 = this.players.flatMap((p) => [p.tag, p.avatar, p.rtt, p.country, p.conn, p.local]);
    this.players.forEach((p, i) => {
      if (!p.playing) this.send(p, ["players", i, ...(p.proto >= 5 ? list5 : p.proto >= 4 ? list4 : list)].join("\t"));
    });
  }

  // The rooms' list in D1: this room as it is now (waiting, or its game and who plays it), or not at all. `beat`:
  // only its row freshened (nothing changed: the lobby is not told).
  async list(beat) {
    const db = this.env.DB;
    try {
      if (this.open && this.players.length)
        await db.prepare("INSERT INTO rooms (code, host, avatar, players, game, name, locked, updated, size, started, tags, art, crc) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) " +
          "ON CONFLICT (code) DO UPDATE SET host = excluded.host, avatar = excluded.avatar, players = excluded.players, game = excluded.game, " +
          "name = excluded.name, locked = excluded.locked, updated = excluded.updated, size = excluded.size, started = excluded.started, tags = excluded.tags, art = excluded.art, " +
          "crc = excluded.crc")
          .bind(this.code, this.players[0].tag, this.players[0].avatar, this.total(), this.game.name, this.name, this.pass ? 1 : 0, now(),
            this.size, this.started ? this.startedAt : 0, this.started ? this.playing().map((p) => p.tag).join(",") : "", this.game.art,
            (this.game.crc1 + " " + this.game.crc2).trim()).run();
      else if (this.code) await db.prepare("DELETE FROM rooms WHERE code = ?").bind(this.code).run();
      if (!beat) await lobbyBump(this.env);
    } catch (e) {
      console.log("room list: " + e);
    }
  }

  message(p, data) {
    this.last = p.seen = Date.now();
    if (typeof data === "string") {
      const host = p === this.players[0];
      if (data.startsWith("chat\t")) {
        const text = clean(data.slice(5), CHAT_LEN);
        if (text) for (const o of this.players) this.send(o, ["chat", p.tag, text].join("\t"));
      } else if (data.startsWith("ping\t")) this.send(p, "pong\t" + clean(data.slice(5), 12));
      else if (data.startsWith("bye\t")) {
        const why = data.slice(4);
        if (LEAVE_WHY.includes(why)) p.why = why;
      }
      else if (data.startsWith("rtt\t")) {
        const ms = Math.max(0, Math.min(parseInt(data.slice(4), 10) || 0, 9999));
        if (Math.abs(ms - p.rtt) >= 5) {
          p.rtt = ms;
          this.roster();
        }
      } else if (p.playing && data.startsWith("p2p\t")) {
        const what = clean(data.slice(4), 120);
        for (const o of this.playing()) if (o !== p) this.send(o, ["p2p", p.port, what].join("\t"));
      } else if (p.playing && data.startsWith("status\t")) {
        const what = clean(data.slice(7), 8);
        for (const o of this.playing()) if (o !== p) this.send(o, ["status", p.port, p.tag, what].join("\t"));
      } else if (!this.started && !this.counting && data.startsWith("local\t")) {
        // How many play on that console (1-3): the room's places permitting.
        const n = parseInt(data.slice(6), 10);
        if (n >= 1 && n <= 3 && this.total() - p.local + n <= this.size) {
          p.local = n;
          this.roster();
          this.list();
        }
      } else if (p.playing && data === "back") {
        // Out of the game, into the lobby: the others are told it left; the room waits again once nobody plays.
        this.quit(p);
        this.roster();
        if (!this.playing().length) this.reopen();
        else this.list();
      } else if (!this.started && host && data === "start" && this.players.length >= 2 && !this.counting) {
        this.counting = true;
        for (const o of this.players) this.send(o, "count\t" + ROOM_COUNTDOWN_S);
        setTimeout(() => {
          if (this.open && !this.started && this.players.length >= 2) this.start();
          else this.counting = false;
        }, ROOM_COUNTDOWN_S * 1000);
      } else if (!this.started && host && data.startsWith("kick\t")) {
        // Out of the room, not banned: it may come back.
        const o = this.players[parseInt(data.slice(5), 10)];
        if (o && o !== p) {
          this.send(o, "kicked");
          try {
            o.ws.close(1000, "kicked");
          } catch (e) {}
          this.leave(o, "kicked");
        }
      }
      return;
    }
    if (!p.playing || !p.port || !(data instanceof ArrayBuffer)) return;
    const msg = new Uint8Array(data.slice(0));
    msg[0] = p.port;
    for (const o of this.playing()) if (o !== p) this.send(o, msg);
  }

  // The game: each console its ports from the next free one (as many as play on it); every one is told how
  // many play, its first port, how many it has, and each port's console (its first port: "1,1,3").
  async start() {
    const owner = [];
    let next = 1;
    this.started = true;
    this.counting = false;
    this.startedAt = now();
    for (const p of this.players) {
      p.port = next;
      p.playing = true;
      p.since = this.startedAt;
      for (let k = 0; k < p.local; k++) owner.push(next);
      next += p.local;
    }
    this.inGame = new Map(this.players.map((p) => [p.port, p.tag]));   // who may come back to it (Room.rejoin)
    for (const p of this.players) this.send(p, ["start", owner.length, p.port, p.local, owner.join(",")].join("\t"));
    await this.together();
    await this.list();
    await this.state.storage.setAlarm(Date.now() + ROOM_BEAT_MS);
  }

  // A game started: one more played together for each two of its players (the profile's "played together").
  async together() {
    const lower = [...new Set(this.players.map((p) => (p.tag ?? "").toLowerCase()).filter((x) => x))];
    if (lower.length < 2) return;
    try {
      const db = this.env.DB;
      const { results } = await db.prepare("SELECT id FROM users WHERE lower IN (" + lower.map(() => "?").join(",") + ")").bind(...lower).all();
      const ids = results.map((r) => r.id).sort((x, y) => x - y), pairs = [];
      for (let i = 0; i < ids.length; i++)
        for (let j = i + 1; j < ids.length; j++)
          pairs.push(db.prepare("INSERT INTO together (a, b, games) VALUES (?, ?, 1) ON CONFLICT (a, b) DO UPDATE SET games = games + 1").bind(ids[i], ids[j]));
      if (pairs.length) await db.batch(pairs);
    } catch (e) { /* a count missed changes nothing of the game */ }
  }

  // A console out of the game: the others play on without it, its port's controller let go; its time in the game
  // added to this week's ranking.
  quit(p) {
    if (!p.playing) return;
    p.playing = false;
    if (this.startedAt) this.week(p.tag, now() - (p.since ?? this.startedAt));   // (since it came back, Room.rejoin)
    const bye = new Uint8Array(16);
    bye[0] = p.port;
    bye[3] = 4;
    for (const o of this.playing()) this.send(o, bye);
    p.port = 0;
  }

  // A console's time in a game: this week's ranking of players (weekly) and of games played online (online_week, by
  // the game's code as /v1/played names it: "NALE / 34EE4749" - a hack's art key is its CRC, so "XXXX" for its code).
  async week(tag, secs) {
    secs = Math.min(secs, WEEK_GAME_S);
    if (!tag || secs < WEEK_MIN_S) return;
    const wk = weekOf(now()), lower = tag.toLowerCase(), g = this.game ?? {};
    const code = g.crc1 ? ((g.art ?? "").length === 4 ? g.art : "XXXX") + " / " + g.crc1 : "";
    try {
      const db = this.env.DB;
      await db.batch([
        db.prepare("INSERT INTO weekly (week, user, seconds) SELECT ?, id, ? FROM users WHERE lower = ? " +
          "ON CONFLICT (week, user) DO UPDATE SET seconds = seconds + excluded.seconds").bind(wk, secs, lower),
        ...(code ? [db.prepare("INSERT INTO online_week (week, code, user, game, minutes) SELECT ?, ?, id, ?, ? FROM users WHERE lower = ? " +
          "ON CONFLICT (week, code, user) DO UPDATE SET minutes = minutes + excluded.minutes, game = excluded.game")
          .bind(wk, code, (g.name ?? "").slice(0, 40), Math.max(1, Math.round(secs / 60)), lower)] : []),
      ]);
    } catch (e) {
      console.log("week: " + e);
    }
  }

  // Nobody plays any more: the room waits for its next game with whoever came back to it.
  async reopen() {
    this.started = false;
    this.counting = false;
    this.roster();
    await this.list();
    await this.state.storage.setAlarm(Date.now() + ROOM_BEAT_MS);
  }

  async leave(p, why) {
    const i = this.players.indexOf(p);
    if (i < 0) return;
    why = why || p.why || "left";
    this.players.splice(i, 1);
    // Those in the lobby (from protocol 6 on) read who went and why.
    for (const o of this.players) if (!o.playing && o.proto >= 6) this.send(o, ["left", p.tag, why].join("\t"));
    await this.log(p.tag, (i === 0 ? "host " : "") + why);
    if (this.started) {
      this.quit(p);
      if (!this.players.length) this.end("over");
      else if (!this.playing().length) {
        await this.reopen();
        return;
      } else this.roster();
    } else if (i === 0) {
      // The host went: the room with it.
      for (const o of this.players) this.send(o, "closed");
      this.end("closed");
    } else this.roster();
    await this.list();
  }

  // Every ROOM_BEAT_MS: consoles gone without a word (silent too long) out of the room, a room nobody starts
  // or talks in closed after ROOM_WAIT_MS, and its row in the list freshened.
  async alarm() {
    if (!this.open) return;
    const t = Date.now();
    for (const p of [...this.players]) {
      if (t - p.seen < (p.playing ? ROOM_PLAY_QUIET_MS : ROOM_QUIET_MS)) continue;
      try {
        p.ws.close(1000, "gone");
      } catch (e) {}
      await this.leave(p, "gone");
      if (!this.open) return;
    }
    if (!this.started && t - this.last >= ROOM_WAIT_MS) {
      for (const p of this.players) this.send(p, "expired");
      this.end("expired");
      await this.list();
      return;
    }
    await this.list(true);   // (the beat: its row freshened)
    await this.state.storage.setAlarm(t + ROOM_BEAT_MS);
  }
  end(why) {
    const all = this.players;
    this.players = [];
    this.open = false;
    this.started = false;
    for (const p of all) {
      try {
        p.ws.close(1000, why);
      } catch (e) {}
    }
  }
}
