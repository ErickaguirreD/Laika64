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
//                                             name locked max hostms art), the games being played ("G" code game
//                                             seconds tag,tag... art; art: the box art's key), the players online ("U" name country avatar
//                                             status room ms conn) and the general chat after `since`
//                                             ("C" id tag avatar text ago)
//   /v1/invite?g=&t=&to=&code=                invites a player to the room `code` (waiting): told at once through
//                                             its inbox ("OK live"), or when it next opens one ("OK later")
//   /v1/inbox?g=&t=                           WebSocket kept open by each console: the server sends through it
//                                             "invite\t<from>\t<game>\t<code>\t<room>" as soon as one is made;
//                                             the console sends "p" every half minute (answered "q")
//   /v1/played?g=&t=&code=&game=&minutes=     after a game
//   /v1/stats?g=&t=&name=                     total minutes, games, rank; then game minutes ...
//   /v1/rank?g=&t=&offset=                    position name country avatar minutes online
//   /v1/news?lang=                            date title text
//   /v1/rooms?g=&t=                           rooms waiting: code host avatar players game name locked
//   /v1/chat?g=&t=&since=                     the general chat after message `since`: id tag avatar text ago (seconds)
//   /v1/say?g=&t=&text=                       a message to it
//   /v1/room?op=create&g=&t=&name=&pass=&game=&crc1=&crc2=&max= | op=join&code=&pass=&g=&t=   WebSocket: see Room below
//
// What is kept: gamertag, country, avatar, the token's hash, last time seen, friends, minutes per
// game. No addresses: a registration only leaves a salted hash of its address for an hour, to
// limit how many one place can make.

const ONLINE_S = 300;            // seen this recently: online (the consoles ping every two minutes)
const PAGE = 50;                 // rows of users and rank per request
const MAX_FRIENDS = 100;
const MAX_REGISTER_PER_HOUR = 5; // per address
const TAG_RE = /^[A-Za-z0-9_]{3,15}$/;
const PROTOCOLS = [2, 3, 4, 5];  // xb_online.cpp ONLINE_PROTO values the rooms serve (3: a room's size, back to its lobby;
                                 // 4: each player's country and connection in "players"; 5: two or three on one console)
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

const now = () => Math.floor(Date.now() / 1000);
const clean = (text, limit) => String(text ?? "").replace(/[\t\r\n\x00-\x1f]/g, " ").trim().slice(0, limit);
const country = (text) => (/^[A-Za-z]{2}$/.test(text ?? "") ? text.toUpperCase() : "--");
const avatar = (text) => Math.max(0, Math.min(parseInt(text ?? "0", 10) || 0, 63));
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

async function register(db, q, request, env) {
  const tag = q.get("tag") ?? "";
  if (!TAG_RE.test(tag)) return ["ERR name"];
  const t = now();
  const place = await sha256((env.SALT ?? "laika") + (request.headers.get("cf-connecting-ip") ?? ""));
  await db.prepare("DELETE FROM registered WHERE at < ?").bind(t - 3600).run();
  const made = await db.prepare("SELECT COUNT(*) AS n FROM registered WHERE place = ?").bind(place).first();
  if (made.n >= MAX_REGISTER_PER_HOUR) return ["ERR busy"];
  if (await db.prepare("SELECT 1 FROM users WHERE lower = ?").bind(tag.toLowerCase()).first()) return ["ERR taken"];
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
      const ms = Math.max(0, Math.min(parseInt(q.get("ms") ?? "0", 10) || 0, 9999));
      // conn 3: a LAIKA before 2.1, which tells neither its time nor its connection (shown as an old version).
      const conn = q.has("conn") ? Math.max(0, Math.min(parseInt(q.get("conn"), 10) || 0, 2)) : 3;
      // The country: where the connection comes from, kept up to date (told back as "c").
      const cc = country(request.cf?.country);
      await db.prepare("UPDATE users SET seen = ?, status = ?, ms = ?, conn = ?, room = ?, country = CASE WHEN ? <> '--' THEN ? ELSE country END WHERE id = ?")
        .bind(t, clean(q.get("status"), 40), ms, conn, q.get("room") === "1" ? 1 : 0, cc, cc, me).run();
      const since = Math.max(0, parseInt(q.get("since") ?? "0", 10) || 0);
      const [friends, asked, msgs] = await db.batch([
        db.prepare("SELECT u.name, u.seen FROM friends f JOIN users u ON u.id = f.friend WHERE f.user = ?").bind(me),
        db.prepare("SELECT u.name, u.avatar FROM requests r JOIN users u ON u.id = r.from_id WHERE r.to_id = ? ORDER BY r.at LIMIT 20").bind(me),
        db.prepare("SELECT d.id, u.name, d.text FROM dm d JOIN users u ON u.id = d.from_id WHERE d.to_id = ? AND d.id > ? ORDER BY d.id LIMIT 20").bind(me, since),
      ]);
      return ["OK", ...(cc !== "--" ? ["c\t" + cc] : []), ...friends.results.map((f) => ["f", f.name, t - f.seen < ONLINE_S ? 1 : 0].join("\t")),
        ...asked.results.map((r) => ["r", r.name, r.avatar].join("\t")), ...msgs.results.map((m) => ["m", m.id, m.name, m.text].join("\t"))];
    }
    case "rename": {
      const tag = q.get("tag") ?? "";
      if (!TAG_RE.test(tag)) return ["ERR name"];
      const taken = await db.prepare("SELECT id FROM users WHERE lower = ?").bind(tag.toLowerCase()).first();
      if (taken && taken.id !== me) return ["ERR taken"];
      try {
        await db.prepare("UPDATE users SET name = ?, lower = ? WHERE id = ?").bind(tag, tag.toLowerCase(), me).run();
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
      const { results } = await db.prepare("SELECT id, from_id, text FROM dm WHERE ((from_id = ?1 AND to_id = ?2) OR (from_id = ?2 AND to_id = ?1)) AND id > ?3 " +
        "ORDER BY id DESC LIMIT 40").bind(me, other.id, since).all();
      return ["OK", ...results.reverse().map((m) => [m.id, m.from_id === me ? 1 : 0, m.text].join("\t"))];
    }
    case "lobby": {
      const since = Math.max(0, parseInt(q.get("since") ?? "0", 10) || 0);
      // The host's time to the server with each room waiting (a console adds its own: its time to the host).
      const [rooms, chat, online] = await db.batch([
        db.prepare("SELECT r.code, r.host, r.avatar, r.players, r.game, r.name, r.locked, r.size, r.started, r.tags, r.art, " +
          "(SELECT u.ms FROM users u WHERE u.lower = lower(r.host)) AS ms FROM rooms r WHERE r.updated > ? ORDER BY r.started, r.updated DESC LIMIT 60")
          .bind(t - ROOM_LIST_S),
        db.prepare("SELECT id, tag, avatar, text, at FROM chat WHERE id > ? ORDER BY id DESC LIMIT 30").bind(since),
        db.prepare("SELECT name, country, avatar, status, room, ms, conn FROM users WHERE seen > ? ORDER BY (status <> '') DESC, room DESC, lower LIMIT ?")
          .bind(t - ONLINE_S, LOBBY_USERS),
      ]);
      return ["OK",
        ...rooms.results.filter((r) => !r.started).map((r) => ["R", r.code, r.host, r.avatar, r.players, r.game, r.name, r.locked, r.size, r.ms ?? 0, r.art].join("\t")),
        ...rooms.results.filter((r) => r.started).map((r) => ["G", r.code, r.game, Math.max(0, t - r.started), r.tags, r.art].join("\t")),
        ...online.results.map((u) => ["U", u.name, u.country, u.avatar, u.status, u.room, u.ms, u.conn].join("\t")),
        ...chat.results.reverse().map((m) => ["C", m.id, m.tag, m.avatar, m.text, Math.max(0, t - m.at)].join("\t"))];
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
      const line = ["invite", who.name, clean(room.game, 40), code, clean(room.name, 24)].join("\t");
      const res = await env.INBOX.get(env.INBOX.idFromName("u" + other.id))
        .fetch("https://inbox/push?" + new URLSearchParams({ from: String(me), line }));
      const how = await res.text();
      return how === "busy" ? ["ERR busy"] : ["OK\t" + how];
    }
    case "played": {
      const minutes = Math.max(0, Math.min(parseInt(q.get("minutes") ?? "0", 10) || 0, 24 * 60));   // one sitting: a day at most
      const code = clean(q.get("code"), 24), game = clean(q.get("game"), 40);
      if (minutes && code)
        await db.batch([
          db.prepare("INSERT INTO played (user, code, game, minutes) VALUES (?, ?, ?, ?) ON CONFLICT (user, code) DO UPDATE SET minutes = minutes + excluded.minutes")
            .bind(me, code, game, minutes),
          db.prepare("UPDATE users SET minutes = minutes + ? WHERE id = ?").bind(minutes, me),
        ]);
      return ["OK"];
    }
    case "stats": {
      const who = await db.prepare("SELECT id, minutes FROM users WHERE lower = ?").bind((q.get("name") ?? "").toLowerCase()).first();
      if (!who) return ["ERR name"];
      const games = await db.prepare("SELECT game, minutes FROM played WHERE user = ? ORDER BY minutes DESC LIMIT 20").bind(who.id).all();
      const count = await db.prepare("SELECT COUNT(*) AS n FROM played WHERE user = ?").bind(who.id).first();
      const above = await db.prepare("SELECT COUNT(*) AS n FROM users WHERE minutes > ?").bind(who.minutes).first();
      return ["OK", [who.minutes, count.n, above.n + 1].join("\t"), ...games.results.map((g) => g.game + "\t" + g.minutes)];
    }
    case "rooms": {
      const { results } = await db.prepare("SELECT code, host, avatar, players, game, name, locked FROM rooms WHERE updated > ? AND started = 0 ORDER BY updated DESC LIMIT 40")
        .bind(t - ROOM_LIST_S).all();
      return ["OK", ...results.map((r) => [r.code, r.host, r.avatar, r.players, r.game, r.name, r.locked].join("\t"))];
    }
    case "chat": {
      const since = Math.max(0, parseInt(q.get("since") ?? "0", 10) || 0);
      const { results } = await db.prepare("SELECT id, tag, avatar, text, at FROM chat WHERE id > ? ORDER BY id DESC LIMIT 40").bind(since).all();
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
      return ["OK"];
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
    db.prepare("INSERT OR IGNORE INTO friends (user, friend) VALUES (?, ?)").bind(a, b),
    db.prepare("INSERT OR IGNORE INTO friends (user, friend) VALUES (?, ?)").bind(b, a),
    db.prepare("DELETE FROM requests WHERE (from_id = ?1 AND to_id = ?2) OR (from_id = ?2 AND to_id = ?1)").bind(a, b),
  ]);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const m = /^\/v1\/([a-z]+)$/.exec(url.pathname);
    if (!m) return new Response("LAIKA 64 online\n", { headers: { "content-type": "text/plain" } });
    if (m[1] === "room") return room(request, url, env);
    if (m[1] === "inbox") return inbox(request, url, env);
    try {
      return reply(await profiles(m[1], url.searchParams, request, env));
    } catch (e) {
      console.log("request failed: " + e);
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
//     count\t<seconds>                       the host started: the game begins in that many seconds
//     start\t<players>\t<port>\t<local>\t<owners>   the game starts: how many play, this console's first N64 port
//                                           (1..), how many ports it has, each port's console ("1,1,3")
//     status\t<port>\t<tag>\t<what>         in the game: a player opened ("menu") or left ("play") its menu
//     pong\t<n>                             the answer to this console's "ping\t<n>"
//     p2p\t<port>\t<what>                    in the game: a console's word about talking directly (its addresses,
//                                           the ports it reaches, or "off"): the consoles agree among themselves
// and takes "chat\t<text>", "ping\t<n>", "rtt\t<ms>" (its time to the server, measured with the pings)
// from anyone, "local\t<1-3>" (how many play on it) before the game, "status\t<what>", "p2p\t<what>" and "back" in
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
    args.set("name", clean(q.get("name"), 24) || who.name);
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
      if (this.total() >= this.size) return refuse("full");
      if (this.pass && (await sha256(this.code + (q.get("pass") ?? ""))) !== this.pass) return refuse("password");
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
    ws.send(["game", this.game.crc1, this.game.crc2, this.game.name, this.name, this.size].join("\t"));
    this.roster();
    await this.list();
    await this.state.storage.setAlarm(Date.now() + ROOM_BEAT_MS);
    ws.addEventListener("message", (e) => this.message(p, e.data));
    const gone = () => this.leave(p);
    ws.addEventListener("close", gone);
    ws.addEventListener("error", gone);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  send(p, data) {
    try {
      p.ws.send(data);
    } catch (e) {}
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

  // The rooms' list in D1: this room as it is now (waiting, or its game and who plays it), or not at all.
  async list() {
    const db = this.env.DB;
    try {
      if (this.open && this.players.length)
        await db.prepare("INSERT INTO rooms (code, host, avatar, players, game, name, locked, updated, size, started, tags, art) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) " +
          "ON CONFLICT (code) DO UPDATE SET host = excluded.host, avatar = excluded.avatar, players = excluded.players, game = excluded.game, " +
          "name = excluded.name, locked = excluded.locked, updated = excluded.updated, size = excluded.size, started = excluded.started, tags = excluded.tags, art = excluded.art")
          .bind(this.code, this.players[0].tag, this.players[0].avatar, this.total(), this.game.name, this.name, this.pass ? 1 : 0, now(),
            this.size, this.started ? this.startedAt : 0, this.started ? this.playing().map((p) => p.tag).join(",") : "", this.game.art).run();
      else if (this.code) await db.prepare("DELETE FROM rooms WHERE code = ?").bind(this.code).run();
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
          this.leave(o);
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
      for (let k = 0; k < p.local; k++) owner.push(next);
      next += p.local;
    }
    for (const p of this.players) this.send(p, ["start", owner.length, p.port, p.local, owner.join(",")].join("\t"));
    await this.list();
    await this.state.storage.setAlarm(Date.now() + ROOM_BEAT_MS);
  }

  // A console out of the game: the others play on without it, its port's controller let go.
  quit(p) {
    if (!p.playing) return;
    p.playing = false;
    const bye = new Uint8Array(16);
    bye[0] = p.port;
    bye[3] = 4;
    for (const o of this.playing()) this.send(o, bye);
    p.port = 0;
  }

  // Nobody plays any more: the room waits for its next game with whoever came back to it.
  async reopen() {
    this.started = false;
    this.counting = false;
    this.roster();
    await this.list();
    await this.state.storage.setAlarm(Date.now() + ROOM_BEAT_MS);
  }

  async leave(p) {
    const i = this.players.indexOf(p);
    if (i < 0) return;
    this.players.splice(i, 1);
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
      await this.leave(p);
      if (!this.open) return;
    }
    if (!this.started && t - this.last >= ROOM_WAIT_MS) {
      for (const p of this.players) this.send(p, "expired");
      this.end("expired");
      await this.list();
      return;
    }
    await this.list();
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
