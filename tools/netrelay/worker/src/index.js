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
//   /v1/ping?g=&t=&status=&since=             "still here"; status: "" or the game played. Answered with the
//                                             player's news: "f name online" per friend, "r name avatar" per
//                                             friendship asked, "m id name text" per message after `since`
//   /v1/rename?g=&t=&tag=                     another gamertag (free, and valid)
//   /v1/users?g=&t=&offset=                   name country avatar online status friend (1 friends, 2 asked)
//   /v1/friends?g=&t=                         the same, friends only
//   /v1/friend?g=&t=&name=&add=1|0            asks for a friendship (OK sent; OK accepted when the other had
//                                             asked too), or ends one
//   /v1/requests?g=&t=                        friendships asked of this player: name country avatar
//   /v1/answer?g=&t=&name=&yes=1|0            yes or no to one
//   /v1/dm?g=&t=&to=&text=                    a message to a friend
//   /v1/conv?g=&t=&with=&since=               the messages with a friend after `since`: id mine text
//   /v1/lobby?g=&t=&since=                    the rooms ("R" code host avatar players game name locked) and the
//                                             general chat after `since` ("C" id tag avatar text ago) in one answer
//   /v1/played?g=&t=&code=&game=&minutes=     after a game
//   /v1/stats?g=&t=&name=                     total minutes, games, rank; then game minutes ...
//   /v1/rank?g=&t=&offset=                    position name country avatar minutes online
//   /v1/news?lang=                            date title text
//   /v1/rooms?g=&t=                           rooms waiting: code host avatar players game name locked
//   /v1/chat?g=&t=&since=                     the general chat after message `since`: id tag avatar text ago (seconds)
//   /v1/say?g=&t=&text=                       a message to it
//   /v1/room?op=create&g=&t=&name=&pass=&game=&crc1=&crc2= | op=join&code=&pass=&g=&t=   WebSocket: see Room below
//
// What is kept: gamertag, country, avatar, the token's hash, last time seen, friends, minutes per
// game. No addresses: a registration only leaves a salted hash of its address for an hour, to
// limit how many one place can make.

const ONLINE_S = 300;            // seen this recently: online (the consoles ping every two minutes)
const PAGE = 50;                 // rows of users and rank per request
const MAX_FRIENDS = 100;
const MAX_REGISTER_PER_HOUR = 5; // per address
const TAG_RE = /^[A-Za-z0-9_]{3,15}$/;
const PROTOCOLS = [2];           // main.cpp NET_PROTO values the rooms serve
const ROOM_PLAYERS = 4;
const ROOM_WAIT_MS = 900000;     // a room nobody starts or talks in is closed after this long
const ROOM_IDLE_MS = 90000;      // nothing from any console this long: the game is over
const ROOM_COUNTDOWN_S = 3;      // from the host's START to the game
const ROOM_LIST_S = 1200;        // public rooms older than this (a room that went away unseen) are not listed
const CHAT_KEEP = 500;           // general chat messages kept
const CHAT_GAP_S = 3;            // a player's messages at least this far apart
const CHAT_LEN = 80;
const DM_LEN = 120;
const MAX_REQUESTS = 50;         // friendships one player has asked and not been answered

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
    "SELECT u.name, u.country, u.avatar, u.seen, u.status, " +
    "(CASE WHEN EXISTS (SELECT 1 FROM friends f WHERE f.user = ?1 AND f.friend = u.id) THEN 1 " +
    "WHEN EXISTS (SELECT 1 FROM requests r WHERE r.from_id = ?1 AND r.to_id = u.id) THEN 2 ELSE 0 END) AS friend FROM users u " +
    where + " ORDER BY " + order + " LIMIT ? OFFSET ?";
  const { results } = await db.prepare(sql.replace(/\?(?!1)/g, () => "?" + (++n))).bind(me, ...args, PAGE, offset).all();
  return results.map((u) => {
    const online = t - u.seen < ONLINE_S;
    return [u.name, u.country, u.avatar, online ? 1 : 0, online ? u.status : "", u.friend].join("\t");
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
        .bind(tag, tag.toLowerCase(), await sha256(token), country(q.get("country") && q.get("country") !== "--" ? q.get("country") : request.cf?.country),
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
      await db.prepare("UPDATE users SET seen = ?, status = ? WHERE id = ?").bind(t, clean(q.get("status"), 40), me).run();
      const since = Math.max(0, parseInt(q.get("since") ?? "0", 10) || 0);
      const [friends, asked, msgs] = await db.batch([
        db.prepare("SELECT u.name, u.seen FROM friends f JOIN users u ON u.id = f.friend WHERE f.user = ?").bind(me),
        db.prepare("SELECT u.name, u.avatar FROM requests r JOIN users u ON u.id = r.from_id WHERE r.to_id = ? ORDER BY r.at LIMIT 20").bind(me),
        db.prepare("SELECT d.id, u.name, d.text FROM dm d JOIN users u ON u.id = d.from_id WHERE d.to_id = ? AND d.id > ? ORDER BY d.id LIMIT 20").bind(me, since),
      ]);
      return ["OK", ...friends.results.map((f) => ["f", f.name, t - f.seen < ONLINE_S ? 1 : 0].join("\t")),
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
    case "profile":
      await db.prepare("UPDATE users SET country = ?, avatar = ? WHERE id = ?").bind(country(q.get("country")), avatar(q.get("avatar")), me).run();
      return ["OK"];
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
      const [rooms, chat] = await db.batch([
        db.prepare("SELECT code, host, avatar, players, game, name, locked FROM rooms WHERE updated > ? ORDER BY updated DESC LIMIT 40").bind(t - ROOM_LIST_S),
        db.prepare("SELECT id, tag, avatar, text, at FROM chat WHERE id > ? ORDER BY id DESC LIMIT 30").bind(since),
      ]);
      return ["OK", ...rooms.results.map((r) => ["R", r.code, r.host, r.avatar, r.players, r.game, r.name, r.locked].join("\t")),
        ...chat.results.reverse().map((m) => ["C", m.id, m.tag, m.avatar, m.text, Math.max(0, t - m.at)].join("\t"))];
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
      const { results } = await db.prepare("SELECT code, host, avatar, players, game, name, locked FROM rooms WHERE updated > ? ORDER BY updated DESC LIMIT 40")
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
    try {
      return reply(await profiles(m[1], url.searchParams, request, env));
    } catch (e) {
      console.log("request failed: " + e);
      return reply(["ERR server"], 500);
    }
  },
};

// ---- Rooms: up to four consoles meet with a six-digit code ----
// A console opens a WebSocket with its gamertag and token: /v1/room?op=create&name=<room>&pass=<password>
// &game=<name>&crc1=<hex>&crc2=<hex>&proto=2 makes a room (text "created <code>"; with a password it is
// listed locked), /v1/room?op=join&code=<code>&pass=<password>&proto=2 joins one. Every room waiting is
// listed (/v1/rooms). While the room waits the server sends text lines:
//     game\t<crc1>\t<crc2>\t<game>\t<room>   the room's game and name (to whoever joins)
//     players\t<mine>\t<tag>\t<avatar>\t<ms>...   who is in it, the host first; <mine>: this console's place
//                                           (0..); <ms>: each one's time to the server and back (0: not known)
//     chat\t<tag>\t<text>                   a message of the room's chat (in the game too)
//     count\t<seconds>                       the host started: the game begins in that many seconds
//     start\t<players>\t<port>              the game starts: how many play, this console's N64 port (1..)
//     status\t<port>\t<tag>\t<what>         in the game: a player opened ("menu") or left ("play") its menu
//     pong\t<n>                             the answer to this console's "ping\t<n>"
//     p2p\t<port>\t<what>                    in the game: a console's word about talking directly (its addresses,
//                                           the ports it reaches, or "off"): the consoles agree among themselves
// and takes "chat\t<text>", "ping\t<n>", "rtt\t<ms>" (its time to the server, measured with the pings)
// from anyone, "status\t<what>" and "p2p\t<what>" in the game, and "start" (two players at least) and "kick\t<place>" from
// the host before the game. A player put out ("kicked") may come back. From the start on every binary
// message of a console goes to all the others with its first byte set to the sender's port: LAIKA's
// netplay messages (main.cpp NetMsg, whose type is small). A console that leaves the game is told to the
// others as a message of type BYE (4) from its port. A text word ("none", "full", "started", "closed",
// "old", "expired", "auth", "busy", "password", "kicked") comes before a close that says why. Each room
// is one Durable Object, named by its code, and listed in D1 while it waits.
async function room(request, url, env) {
  if (request.headers.get("upgrade") !== "websocket") return new Response("WebSocket only\n", { status: 426 });
  const q = url.searchParams;
  if (!PROTOCOLS.includes(parseInt(q.get("proto") ?? "0", 10))) return refuse("old");
  const me = await user(env.DB, q);
  if (me === null) return refuse("auth");
  const who = await env.DB.prepare("SELECT name, avatar FROM users WHERE id = ?").bind(me).first();
  const args = new URLSearchParams({ tag: who.name, avatar: String(who.avatar) });
  args.set("pass", clean(q.get("pass"), 16));
  if (q.get("op") === "create") {
    args.set("name", clean(q.get("name"), 24) || who.name);
    args.set("game", clean(q.get("game"), 40));
    args.set("crc1", clean(q.get("crc1"), 8));
    args.set("crc2", clean(q.get("crc2"), 8));
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
    this.players = [];        // { ws, tag, avatar, port }: the host first
    this.open = false;        // made, not closed
    this.started = false;
    this.last = 0;
  }

  async fetch(request) {
    const url = new URL(request.url), q = url.searchParams;
    const create = url.pathname === "/create";
    if (create && this.open) return new Response("taken", { status: 409 });
    if (!create) {
      if (!this.open) return refuse("none");
      if (this.started) return refuse("started");
      if (this.players.length >= ROOM_PLAYERS) return refuse("full");
      if (this.pass && (await sha256(this.code + (q.get("pass") ?? ""))) !== this.pass) return refuse("password");
    }
    const pair = new WebSocketPair();
    const ws = pair[1];
    ws.accept();
    ws.binaryType = "arraybuffer";   // the consoles' messages as they are (not Blobs, which arrive later)
    const p = { ws, tag: q.get("tag"), avatar: parseInt(q.get("avatar"), 10) || 0, port: 0, rtt: 0 };
    this.last = Date.now();
    if (create) {
      this.open = true;
      this.started = false;
      this.counting = false;
      this.code = q.get("code");
      this.name = q.get("name") ?? "";
      this.pass = q.get("pass") ? await sha256(this.code + q.get("pass")) : "";   // kept hashed
      this.game = { name: q.get("game") ?? "", crc1: q.get("crc1") ?? "", crc2: q.get("crc2") ?? "" };
      this.players = [p];
      ws.send("created " + this.code);
    } else this.players.push(p);
    ws.send(["game", this.game.crc1, this.game.crc2, this.game.name, this.name].join("\t"));
    this.roster();
    await this.list();
    await this.state.storage.setAlarm(Date.now() + ROOM_WAIT_MS);
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

  roster() {
    const list = this.players.flatMap((p) => [p.tag, p.avatar, p.rtt]);
    this.players.forEach((p, i) => this.send(p, ["players", i, ...list].join("\t")));
  }

  // The rooms' list in D1: this room as it is now, or not at all.
  async list() {
    const db = this.env.DB;
    try {
      if (this.open && !this.started && this.players.length)
        await db.prepare("INSERT INTO rooms (code, host, avatar, players, game, name, locked, updated) VALUES (?, ?, ?, ?, ?, ?, ?, ?) " +
          "ON CONFLICT (code) DO UPDATE SET host = excluded.host, avatar = excluded.avatar, players = excluded.players, game = excluded.game, " +
          "name = excluded.name, locked = excluded.locked, updated = excluded.updated")
          .bind(this.code, this.players[0].tag, this.players[0].avatar, this.players.length, this.game.name, this.name, this.pass ? 1 : 0, now()).run();
      else if (this.code) await db.prepare("DELETE FROM rooms WHERE code = ?").bind(this.code).run();
    } catch (e) {
      console.log("room list: " + e);
    }
  }

  message(p, data) {
    this.last = Date.now();
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
          if (!this.started) this.roster();
        }
      } else if (this.started && data.startsWith("p2p\t")) {
        const what = clean(data.slice(4), 120);
        for (const o of this.players) if (o !== p) this.send(o, ["p2p", p.port, what].join("\t"));
      } else if (this.started && data.startsWith("status\t")) {
        const what = clean(data.slice(7), 8);
        for (const o of this.players) if (o !== p) this.send(o, ["status", p.port, p.tag, what].join("\t"));
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
    if (!this.started || !p.port || !(data instanceof ArrayBuffer)) return;
    const msg = new Uint8Array(data.slice(0));
    msg[0] = p.port;
    for (const o of this.players) if (o !== p) this.send(o, msg);
  }

  async start() {
    this.started = true;
    this.players.forEach((p, i) => (p.port = i + 1));
    for (const p of this.players) this.send(p, ["start", this.players.length, p.port].join("\t"));
    await this.list();
    await this.state.storage.setAlarm(Date.now() + ROOM_IDLE_MS);
  }

  async leave(p) {
    const i = this.players.indexOf(p);
    if (i < 0) return;
    this.players.splice(i, 1);
    if (this.started) {
      // The others play on without it: its port's controller let go.
      const bye = new Uint8Array(16);
      bye[0] = p.port;
      bye[3] = 4;
      for (const o of this.players) this.send(o, bye);
      if (this.players.length < 1) this.end("over");
    } else if (i === 0) {
      // The host went: the room with it.
      for (const o of this.players) this.send(o, "closed");
      this.end("closed");
    } else this.roster();
    await this.list();
  }

  async alarm() {
    if (!this.open) return;
    const limit = this.started ? ROOM_IDLE_MS : ROOM_WAIT_MS;
    if (Date.now() - this.last < limit) {
      await this.state.storage.setAlarm(this.last + limit);
      return;
    }
    if (!this.started) for (const p of this.players) this.send(p, "expired");
    this.end("expired");
    await this.list();
  }

  end(why) {
    const all = this.players;
    this.players = [];
    this.open = false;
    for (const p of all) {
      try {
        p.ws.close(1000, why);
      } catch (e) {}
    }
  }
}
