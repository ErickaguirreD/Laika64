#!/usr/bin/env python3
"""LAIKA 64 - Laika at her laptop for the HD packs' download screen (xb_laika_pc.h), from "LAIKA ANIMADA/LAIKA PC.gif".

The GIF as drawn, its black background cut out:
  - its frames bob up and down, the laptop with them (2 and 7 pixels up in frames 4-6 and 7-9, measured by matching
    the laptop to the first frame): each set back in place, the laptop and the ground still;
  - the drawn shadow under her and the laptop goes: everything from row GROUND down (her paws' white ends at 294, the
    laptop's base at 291; from 295 only the shadow and the laptop's glow fading);
  - the background is the black joined to the border, and the black between her chest and the laptop (GAP);
  - her belly's black fur touches the background in some frames: inside BELLY, what is her in the first frame is her
    in all of them (cut frame by frame it came and went);
  - one round of the near-black specks on the edge.
Each frame scaled to PC_W wide and kept as a PNG.

    python platform/xbox360/tools/gen_laika_pc.py "LAIKA ANIMADA"
"""
import io
import os
import sys
from collections import deque

from PIL import Image, ImageFilter, ImageSequence

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "platform", "xbox360", "xb_laika_pc.h")
PC_W = 320                       # the frames' width as kept (the download screen shows them so)
DARK = 6                         # the background: black, any channel up to this
SHIFT = [0, 0, 0, 2, 2, 2, 7, 7, 7]
GROUND = 295
GAP = (300, 150, 430, 285)       # between her chest and the laptop's back (pixels of the 508 x 337 GIF)
BELLY = (110, 170, 400, 337)     # her body over the ground, between the tail and the paws on the laptop


def flood(px, w, h, seen, starts):
    q = deque()
    for x, y in starts:
        if 0 <= x < w and 0 <= y < h and not seen[y][x] and max(px[x, y]) <= DARK:
            seen[y][x] = True
            q.append((x, y))
    while q:
        cx, cy = q.popleft()
        for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
            if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and max(px[nx, ny]) <= DARK:
                seen[ny][nx] = True
                q.append((nx, ny))


def holes(px, w, h, seen):
    out = []
    done = [row[:] for row in seen]
    for sy in range(h):
        for sx in range(w):
            if done[sy][sx] or max(px[sx, sy]) > DARK:
                continue
            reg = []
            q = deque([(sx, sy)])
            done[sy][sx] = True
            while q:
                cx, cy = q.popleft()
                reg.append((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < w and 0 <= ny < h and not done[ny][nx] and max(px[nx, ny]) <= DARK:
                        done[ny][nx] = True
                        q.append((nx, ny))
            xs = [p[0] for p in reg]
            ys = [p[1] for p in reg]
            out.append((len(reg), (min(xs), min(ys), max(xs), max(ys)), reg))
    return out


def cut(frame, keep=None):
    w, h = frame.size
    px = frame.load()
    seen = [[False] * w for _ in range(h)]
    flood(px, w, h, seen, [(i, 0) for i in range(w)] + [(i, h - 1) for i in range(w)] + [(0, j) for j in range(h)] + [(w - 1, j) for j in range(h)])
    for size, box, reg in holes(px, w, h, seen):
        if size > 80 and box[0] >= GAP[0] and box[2] <= GAP[2] and box[1] >= GAP[1] and box[3] <= GAP[3]:
            for cx, cy in reg:
                seen[cy][cx] = True
    for yy in range(GROUND, h):
        for xx in range(w):
            seen[yy][xx] = True
    if keep is not None:
        for yy in range(BELLY[1], min(BELLY[3], h)):
            for xx in range(BELLY[0], BELLY[2]):
                if keep[yy][xx]:
                    seen[yy][xx] = False
    drop = [(xx, yy) for yy in range(h) for xx in range(w) if not seen[yy][xx] and max(px[xx, yy]) <= 22 and any(
        0 <= nx < w and 0 <= ny < h and seen[ny][nx] for nx, ny in ((xx + 1, yy), (xx - 1, yy), (xx, yy + 1), (xx, yy - 1)))]
    for xx, yy in drop:
        seen[yy][xx] = True
    a = Image.new("L", (w, h), 255)
    pa = a.load()
    for yy in range(h):
        for xx in range(w):
            if seen[yy][xx]:
                pa[xx, yy] = 0
    a = a.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.6))
    out = frame.convert("RGBA")
    out.putalpha(a)
    return out, [[not seen[yy][xx] for xx in range(w)] for yy in range(h)]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "LAIKA ANIMADA")
    frames = [f.convert("RGB") for f in ImageSequence.Iterator(Image.open(os.path.join(src, "LAIKA PC.gif")))]
    placed = []
    for f, dy in zip(frames, SHIFT):
        g = Image.new("RGB", f.size, (0, 0, 0))
        g.paste(f, (0, dy))
        placed.append(g)
    first, keep = cut(placed[0])
    cuts = [first] + [cut(f, keep)[0] for f in placed[1:]]
    k = PC_W / cuts[0].width
    pngs = []
    for c in cuts:
        g = c.resize((PC_W, round(c.height * k)), Image.LANCZOS)
        b = io.BytesIO()
        g.save(b, "PNG", optimize=True)
        pngs.append(b.getvalue())
    lines = ["// LAIKA 64 - Laika at her laptop (the HD packs' download screen). GENERATED by",
             "// platform/xbox360/tools/gen_laika_pc.py from \"LAIKA ANIMADA/LAIKA PC.gif\": do not edit.",
             "#ifndef XB_LAIKA_PC_H", "#define XB_LAIKA_PC_H", "",
             "#define LAIKA_PC_N %d" % len(pngs), "#define LAIKA_PC_MS 160          // a frame, as the GIF", ""]
    for i, p in enumerate(pngs):
        lines.append("static const unsigned char s_laikaPcPng%d[%d] = {" % (i, len(p)))
        for j in range(0, len(p), 24):
            lines.append("    " + ",".join("0x%02X" % c for c in p[j:j + 24]) + ",")
        lines.append("};")
    lines.append("static const unsigned char *const s_laikaPcPng[LAIKA_PC_N] = { " + ", ".join("s_laikaPcPng%d" % i for i in range(len(pngs))) + " };")
    lines.append("static const unsigned s_laikaPcPngSize[LAIKA_PC_N] = { " + ", ".join(str(len(p)) for p in pngs) + " };")
    lines += ["", "#endif", ""]
    with open(OUT, "w", newline="\r\n") as f:
        f.write("\n".join(lines))
    print("wrote %s: %d frames, %d KB" % (OUT, len(pngs), sum(len(p) for p in pngs) // 1024))


if __name__ == "__main__":
    main()
