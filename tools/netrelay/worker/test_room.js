// Checks the rooms and chats of a running server (wrangler dev, or the deployed one):
//   node test_room.js http://127.0.0.1:8799
// It registers four test players (rm<random><n>), so against the deployed server they stay in its lists.
const WebSocket = require("ws");
const base = (process.argv[2] || "http://127.0.0.1:8799").replace(/\/$/, "");
const wsBase = base.replace(/^http/, "ws") + "/v1/room";

async function api(path) {
  const text = await (await fetch(base + "/v1/" + path)).text();
  return text.split("\n").filter((l) => l && l !== ".");
}

const open = (query) => new Promise((resolve, reject) => {
  const ws = new WebSocket(wsBase + query);
  ws.queue = [];
  ws.waiting = [];
  ws.on("message", (data, binary) => {
    const m = binary ? Buffer.from(data) : data.toString();
    if (ws.waiting.length) ws.waiting.shift()(m);
    else ws.queue.push(m);
  });
  ws.next = () => new Promise((r) => (ws.queue.length ? r(ws.queue.shift()) : ws.waiting.push(r)));
  // The next text line starting with `word` (others before it are dropped).
  ws.until = async (word) => { for (;;) { const m = await ws.next(); if (typeof m === "string" && m.startsWith(word)) return m; } };
  ws.on("open", () => resolve(ws));
  ws.on("error", reject);
});

(async () => {
  const tag = (k) => "rm" + Math.random().toString(36).slice(2, 8) + k;
  const me = [];
  for (let k = 0; k < 4; k++) {
    const name = tag(k), r = await api(`register?tag=${name}&country=MX&avatar=${k * 7}`);
    if (!r[0].startsWith("OK")) throw new Error("register: " + r[0]);
    me.push({ name, token: r[0].split("\t")[1] });
  }
  const auth = (k) => `&g=${me[k].name}&t=${me[k].token}`;
  console.log("old protocol:", await (await open("?op=create&proto=1" + auth(0))).next());
  console.log("bad token:", await (await open("?op=create&proto=2&g=" + me[0].name + "&t=nope")).next());

  const host = await open(`?op=create&proto=2&name=Sala%20de%20prueba&pass=secreto&game=SMASH%20REMIX&crc1=34EE4749&crc2=AABBCCDD${auth(0)}`);
  const created = await host.until("created");
  const code = created.split(" ")[1];
  console.log("host:", created, "|", await host.until("game"), "|", await host.until("players"));
  const rooms = await api("rooms?" + auth(0).slice(1));
  console.log("public rooms:", rooms.slice(1).filter((r) => r.startsWith(code)));
  console.log("wrong password:", await (await open(`?op=join&code=${code}&pass=otra&proto=2${auth(1)}`)).next());
  const guests = [];
  for (let k = 1; k < 4; k++) {
    const g = await open(`?op=join&code=${code}&pass=secreto&proto=2${auth(k)}`);
    console.log("guest " + k + ":", await g.until("game"), "|", await g.until("players"));
    guests.push(g);
  }
  console.log("fifth:", await (await open(`?op=join&code=${code}&pass=secreto&proto=2${auth(1)}`)).next());
  guests[1].send("chat\thola a todos");
  console.log("chat seen by host:", await host.until("chat"), "| by guest 3:", await guests[2].until("chat"));
  guests[0].send("ping\t42");
  console.log("pong:", await guests[0].until("pong"));
  guests[0].send("rtt\t87");
  console.log("roster with a time:", await host.until("players"));
  host.send("kick\t2");
  console.log("kicked:", await guests[1].until("kicked"), "| host sees:", await host.until("players"));
  guests[1] = await open(`?op=join&code=${code}&pass=secreto&proto=2${auth(2)}`);
  console.log("back after the kick:", await guests[1].until("players"));
  guests[0].send("start");   // not the host: nothing happens
  host.send("start");
  console.log("countdown:", await host.until("count"));
  const t0 = Date.now();
  const starts = [await host.until("start"), ...(await Promise.all(guests.map((g) => g.until("start"))))];
  console.log("start after", ((Date.now() - t0) / 1000).toFixed(1), "s:", starts.join(" | "));
  host.send("status\tmenu");
  console.log("status seen by a guest:", await guests[0].until("status"));
  host.send("p2p\tcand 1.2.3.4:5000 192.168.1.5:5000");
  console.log("p2p seen by a guest:", await guests[0].until("p2p"));
  guests[2].send("chat\tgg");
  console.log("chat in the game:", await host.until("chat"));
  console.log("public rooms after the start:", (await api("rooms?" + auth(0).slice(1))).slice(1).filter((r) => r.startsWith(code)).length);
  // Each console's message reaches the three others with the sender's port in its first byte.
  const all = [host, ...guests];
  for (let k = 0; k < 4; k++) {
    const m = Buffer.alloc(16);
    m.writeUInt32BE(2, 0);
    m.writeUInt32BE(100 + k, 4);
    all[k].send(m);
    for (let j = 0; j < 4; j++) {
      if (j === k) continue;
      let got;
      do got = await all[j].next(); while (typeof got === "string");   // text lines (chat) passed over
      if (got[0] !== parseInt(starts[k].split("	")[2], 10) || got.readUInt32BE(4) !== 100 + k || got[3] !== 2) throw new Error(`message of ${k} at ${j}: ${got.toString("hex")}`);
    }
  }
  console.log("binary relay: every message reached the 3 others, stamped with its port");
  guests[2].close();
  const bye = await host.next();
  console.log("host told guest 3 left: port", bye[0], "type", bye.readUInt32BE(0) & 0xffffff);
  // The general chat.
  console.log("say:", (await api("say?text=hola%20LAIKA" + auth(0)))[0], "| too soon:", (await api("say?text=otra" + auth(0)))[0]);
  const chat = await api("chat?since=0" + auth(1));
  console.log("chat:", chat[0], chat.slice(-1)[0]);
  process.exit(0);
})().catch((e) => { console.log("FAILED:", e.message); process.exit(1); });
