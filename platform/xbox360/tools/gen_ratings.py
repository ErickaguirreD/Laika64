#!/usr/bin/env python3
"""LAIKA 64 - builds data/ratings.txt, the critics' scores of every N64 game.

The ROM list's stars come from this one file (xb_art.cpp downloads it from the
repository and keeps it in art\\games\\db\\): no Wikipedia request per game on
the console. Each line is a game code without its region letter and the score
(1-100): "SM6 94". The scores are read like the console used to read them:
the English Wikipedia article's {{Video game reviews}} box, Metacritic ("MC")
first, else GameRankings ("GR"), the N64 figure when the box lists several
systems ("MC_N64", or "N64: 85/100" in the field).

The games and their titles come from GameDB-N64 (the same list the console
uses). Run from anywhere, then commit data/ratings.txt:
    python platform/xbox360/tools/gen_ratings.py
"""
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "data", "ratings.txt")
GAMEDB = "https://github.com/niemasd/GameDB-N64/releases/latest/download/N64.data.json"
WIKI = "https://en.wikipedia.org/w/api.php"
AGENT = "LAIKA64-ratings/1.0 (https://github.com/ErickaguirreD/Laika64)"
REGION_ORDER = {"USA": 0, "EUR": 1, "AUS": 2, "GER": 3, "FRA": 4, "JPN": 5}


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": AGENT})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except Exception:
            if attempt == 3:
                raise
            time.sleep(2 + attempt * 3)


def review_number(seg, metacritic):
    """The score in one field of the box ("94/100" or "96%"); 0: none."""
    # The value without its references (whose titles may name "N64") and comments.
    seg = re.sub(r"<ref[^>]*/>|<ref[^>]*>.*?</ref>|<!--.*?-->|\{\{[^{}]*\}\}", "", seg, flags=re.S)
    seg = re.split(r"<ref|\{\{", seg)[0]   # a reference cut off by the field's end
    n64 = seg.find("N64")
    if n64 >= 0:
        seg = seg[n64:]
    elif re.match(r"\s*[A-Za-z ]+:", seg):
        return 0   # "NS: 76/100": another system's figure, not the N64 game's
    # Metacritic as "94/100" (a few boxes write "90%"), GameRankings as "96%".
    pat = r"(\d+)(?:\.\d+)?\s*(?:/\s*100|%)" if metacritic else r"(\d+)(?:\.\d+)?\s*%"
    m = re.search(pat, seg)
    if m and 1 <= int(m.group(1)) <= 100:
        return int(m.group(1))
    return 0


def wikitext_score(text):
    box = text.find("{{Video game reviews")
    if box < 0:
        box = text.find("{{video game reviews")
    if box < 0:
        return 0
    end = text.find("\n}}", box)
    if end < 0:
        end = len(text)
    body = text[box:end]
    # Metacritic first, then GameRankings; a box listing several systems names the N64 one "MC_N64".
    for key, mc in (("MC_N64", True), ("MC", True), ("GR_N64", False), ("GR", False)):
        for m in re.finditer(r"[| ]" + key + r"\s*=", body):
            stop = re.search(r"\n\s*\||\|\s*\w+\s*=", body[m.end():])   # the next field
            v = review_number(body[m.end():m.end() + stop.start() if stop else len(body)], mc)
            if v:
                return v
    return 0


def fetch_pages(titles):
    """title -> wikitext (redirects followed), 50 at a time."""
    out = {}
    for i in range(0, len(titles), 50):
        chunk = titles[i:i + 50]
        q = urllib.parse.urlencode({"action": "query", "prop": "revisions", "rvprop": "content", "rvslots": "main",
                                    "format": "json", "formatversion": "2", "redirects": "1", "titles": "|".join(chunk)})
        data = json.loads(get(WIKI + "?" + q))
        back = {}
        for n in data["query"].get("normalized", []):
            back.setdefault(n["to"], []).append(n["from"])
        for r in data["query"].get("redirects", []):
            back.setdefault(r["to"], []).extend(back.get(r["from"], [r["from"]]))
        for p in data["query"]["pages"]:
            if "revisions" not in p:
                continue
            text = p["revisions"][0]["slots"]["main"]["content"]
            for name in [p["title"]] + back.get(p["title"], []):
                out[name] = text
        time.sleep(0.5)
    return out


def main():
    db = json.loads(get(GAMEDB))
    games = {}   # code without region -> (region order, title, year)
    for serial, g in db.items():
        parts = serial.split("-")
        if len(parts) < 3 or len(parts[1]) != 4 or not g.get("title"):
            continue
        code = parts[1][:3]
        rank = REGION_ORDER.get(parts[2], 9)
        year = (g.get("release_date") or "")[:4]
        if code not in games or rank < games[code][0]:
            games[code] = (rank, g["title"], year)
    scores = {}
    # The pages tried, best first, as the console tried them: the title, then
    # "... (video game)", then "... (YEAR video game)".
    for step in range(3):
        want = {}
        for code, (_, title, year) in games.items():
            if code in scores:
                continue
            if step == 1:
                title += " (video game)"
            elif step == 2:
                if not year:
                    continue
                title += " (%s video game)" % year
            want[code] = title
        pages = fetch_pages(sorted(set(want.values())))
        for code, title in want.items():
            v = wikitext_score(pages.get(title, ""))
            if v:
                scores[code] = v
        print("pass %d: %d of %d games scored" % (step + 1, len(scores), len(games)))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="ascii", newline="\n") as f:
        f.write("# LAIKA 64 - critics' scores (Metacritic, else GameRankings) from English Wikipedia.\n")
        f.write("# <game code without its region letter> <score 1-100>. Made by platform/xbox360/tools/gen_ratings.py\n")
        for code in sorted(scores):
            f.write("%s %d\n" % (code, scores[code]))
    print("wrote %s: %d games" % (OUT, len(scores)))


if __name__ == "__main__":
    sys.exit(main())
