// Checks the friendships, messages and news of a running server (wrangler dev):
//   node test_social.js http://127.0.0.1:8799
// It registers two test players (sc<random><n>).
const base = (process.argv[2] || "http://127.0.0.1:8799").replace(/\/$/, "");
async function api(path) {
  const text = await (await fetch(base + "/v1/" + path)).text();
  return text.split("\n").filter((l) => l && l !== ".");
}
(async () => {
  const me = [];
  for (let k = 0; k < 2; k++) {
    const name = "sc" + Math.random().toString(36).slice(2, 8) + k, r = await api(`register?tag=${name}&country=--&avatar=${k}`);
    if (!r[0].startsWith("OK")) throw new Error("register: " + r[0]);
    me.push({ name, token: r[0].split("\t")[1] });
  }
  const a = (k) => `g=${me[k].name}&t=${me[k].token}`;
  console.log("whereami:", (await api("whereami"))[0]);
  console.log("ask:", (await api(`friend?${a(0)}&name=${me[1].name}&add=1`))[0], "| users shows 2:", (await api(`users?${a(0)}&offset=0`)).find((l) => l.startsWith(me[1].name)));
  console.log("dm before friends:", (await api(`dm?${a(0)}&to=${me[1].name}&text=hola`))[0]);
  console.log("requests of 1:", await api(`requests?${a(1)}`));
  console.log("ping of 1 (news):", await api(`ping?${a(1)}&status=&since=0`));
  console.log("answer yes:", (await api(`answer?${a(1)}&name=${me[0].name}&yes=1`))[0], "| friends of 0:", await api(`friends?${a(0)}`));
  console.log("dm:", (await api(`dm?${a(0)}&to=${me[1].name}&text=hola%20amigo`))[0], "| too soon:", (await api(`dm?${a(0)}&to=${me[1].name}&text=otra`))[0]);
  console.log("ping of 1 (a message):", await api(`ping?${a(1)}&status=SMASH&since=0`));
  console.log("conv of 1:", await api(`conv?${a(1)}&with=${me[0].name}&since=0`));
  const newName = me[0].name + "x";
  console.log("rename:", (await api(`rename?${a(0)}&tag=${newName}`))[0], "| taken:", (await api(`rename?${a(1)}&tag=${newName}`))[0]);
  me[0].name = newName;
  console.log("still friends after the rename:", await api(`friends?${a(1)}`));
  console.log("lobby:", (await api(`lobby?${a(0)}&since=0`)).slice(0, 3));
  console.log("unfriend:", (await api(`friend?${a(1)}&name=${me[0].name}&add=0`))[0], "| friends of 0:", await api(`friends?${a(0)}`));
})().catch((e) => { console.log("FAILED:", e.message); process.exit(1); });
