"""xb_players.h: how many players each original N64 game is for, by its header CRCs (mupen64plus.ini's rom
catalog: CRC= and Players=, a dump without its own Players taking its RefMD5's). Hacks are left out (their
GoodName tagged [h...] or "Hack"): a hack is let into rooms whatever its original is for.
    python -I gen_players.py <mupen64plus.ini> <out.h>"""
import re, sys

src, out = sys.argv[1], sys.argv[2]
entries, cur = {}, None
for line in open(src, encoding="latin-1"):
    line = line.strip()
    m = re.match(r"^\[([0-9A-Fa-f]{32})\]$", line)
    if m:
        cur = entries.setdefault(m.group(1).upper(), {})
        continue
    if cur is None or "=" not in line or line.startswith(";"):
        continue
    k, v = line.split("=", 1)
    cur[k.strip()] = v.strip()

def players(e, depth=0):
    if "Players" in e:
        return e["Players"]
    ref = e.get("RefMD5", "").upper()
    if ref and ref in entries and depth < 4:
        return players(entries[ref], depth + 1)
    return None

table = {}
for md5, e in entries.items():
    name = e.get("GoodName", "")
    if re.search(r"\[h|hack", name, re.I):
        continue
    crc = e.get("CRC", "").split()
    p = players(e)
    if len(crc) != 2 or not p or not p.isdigit():
        continue
    n = int(p)
    if not 1 <= n <= 4:
        continue
    key = (int(crc[0], 16), int(crc[1], 16))
    table[key] = max(table.get(key, 0), n)   # two dumps with the same header: the larger

rows = sorted(table.items())
with open(out, "w", newline="\n") as f:
    f.write("// LAIKA 64 - how many players each original N64 game is for, by its header CRCs (sorted, for a binary\n")
    f.write("// search). Made by platform/xbox360/tools/gen_players.py from mupen64plus.ini's rom catalog (CRC= and Players=);\n")
    f.write("// hacks are not in it: a ROM whose CRCs are not here (a hack, homebrew, an unknown dump) is for 4.\n")
    f.write("#ifndef XB_PLAYERS_H\n#define XB_PLAYERS_H\n\n")
    f.write("static const struct { unsigned crc1, crc2; unsigned char players; } s_gamePlayers[] = {\n")
    for (c1, c2), n in rows:
        f.write("    { 0x%08Xu, 0x%08Xu, %d },\n" % (c1, c2, n))
    f.write("};\n\n#endif\n")
counts = {}
for _, n in rows:
    counts[n] = counts.get(n, 0) + 1
print(len(rows), "games;", "by players:", dict(sorted(counts.items())))
