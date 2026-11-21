#!/usr/bin/env python3
"""LAIKA 64 - Laika's animations for the setup assistant and the main menu (xb_laika_anim.h), from the drawings in
"LAIKA ANIMADA": the jump and the turn (LAIKA GIRANDO.gif, frames 0..15 and 16..24), standing to sitting and back
(DE PIE SENTADA.gif), lying down (ACOSTADA.gif), barking (LAIKA LADRANDO.gif: drawn bigger, 508 wide, so
scaled by BARK_K and set on a 380 x 239 picture as the others, its paws on their line) and sitting by a beating heart
(LAIKA DONACION.gif: the novelties asking for donations; kept as drawn, its own size and black background).

Each frame is cut out of its black background (pure black reaching the border, and enclosed pure black holes over 60
pixels reaching down to the paws: the gaps between the legs; the others are the fur's own black), scaled to LAIKA_W wide and kept as a
PNG. With each frame: where its paws stand (its ground, the jump's re-anchored so it takes off and lands on the same
line as the rest) and the middle of her body (so one pose follows another without a jump sideways). She faces right
in every frame; the code mirrors her to face left.

    python platform/xbox360/tools/gen_laika_anim.py "LAIKA ANIMADA"
"""
import io
import os
import sys
from collections import deque

from PIL import Image, ImageFilter, ImageSequence

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "platform", "xbox360", "xb_laika_anim.h")
SRC_W = 380            # the drawings' width
BARK_K = 0.70          # the barking drawings to the others' size (her body as long, crouching as she barks)
W = 240                # the frames' width as kept
K = W / SRC_W


LEG_GAP = 6           # an enclosed black region reaching this close (pixels) to the paws' line is a gap between legs
FRINGE_DARK = 40       # her outline's near-black fringe: any channel up to this ...
FRINGE_STEPS = 3       # ... up to this many pixels in from the background


def cut(frame, dark=0):
    x = frame.convert("RGB")
    w, h = x.size
    px = x.load()
    a = Image.new("L", (w, h), 255)
    pa = a.load()
    seen = [[False] * w for _ in range(h)]
    paws = max(y for y in range(h) if any(max(px[xx, y]) > dark for xx in range(0, w, 2)))   # her lowest row
    for sy in range(h):
        for sx in range(w):
            if seen[sy][sx] or max(px[sx, sy]) > dark:
                continue
            q = deque([(sx, sy)])
            seen[sy][sx] = True
            region, edge = [], False
            while q:
                cx, cy = q.popleft()
                region.append((cx, cy))
                if cx in (0, w - 1) or cy in (0, h - 1):
                    edge = True
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and max(px[nx, ny]) <= dark:
                        seen[ny][nx] = True
                        q.append((nx, ny))
            # an enclosed one only when it reaches down to the paws (the gap between the legs): her black fur's
            # patches higher up were cut as holes too, the background seen through her body
            if edge or (len(region) > 60 and max(cy for _, cy in region) >= paws - LEG_GAP):
                for cx, cy in region:
                    pa[cx, cy] = 0
    # The near-black fringe along her outline (the drawing's dithered dark edge: black specks round the tail, the paws,
    # the teeth): FRINGE_STEPS rounds, each dropping the pixels up to FRINGE_DARK next to the background - only the
    # edge, her black fur further in stays.
    for _ in range(FRINGE_STEPS):
        drop = []
        for cy in range(h):
            for cx in range(w):
                if pa[cx, cy] and max(px[cx, cy]) <= FRINGE_DARK and any(
                        0 <= nx < w and 0 <= ny < h and not pa[nx, ny]
                        for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1))):
                    drop.append((cx, cy))
        for cx, cy in drop:
            pa[cx, cy] = 0
    a = a.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.6))
    out = x.convert("RGBA")
    out.putalpha(a)
    return out


def frames_of(path, dark=0):
    return [cut(f, dark) for f in ImageSequence.Iterator(Image.open(path))]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "LAIKA ANIMADA")
    turning = frames_of(os.path.join(src, "LAIKA GIRANDO.gif"))
    jump, turn = turning[0:16], turning[16:25]
    sit = frames_of(os.path.join(src, "DE PIE SENTADA.gif"))
    lie = frames_of(os.path.join(src, "ACOSTADA.gif"))
    bark = []
    for f in frames_of(os.path.join(src, "LAIKA LADRANDO.gif")):
        g = f.resize((round(f.width * BARK_K), round(f.height * BARK_K)), Image.LANCZOS)
        canvas = Image.new("RGBA", (SRC_W, 239), (0, 0, 0, 0))
        canvas.alpha_composite(g, ((SRC_W - g.width) // 2, 239 - g.height))
        bark.append(canvas)
    bark_ground = 239 - round(337 * BARK_K) + 329 * BARK_K   # the GIF's paws at 329 of its 337
    # as drawn: neither cut out nor scaled (the novelties show it at its own size, its black background and all)
    give = [f.convert("RGBA") for f in ImageSequence.Iterator(Image.open(os.path.join(src, "LAIKA DONACION.gif")))]
    give_ground = give[0].height
    # the jump's ground: its frames on the ground set on line 240, those in the air between take-off and landing
    feet = [f.getchannel("A").getbbox()[3] for f in jump]
    off = {i: 240 - feet[i] for i in (0, 1, 2, 3, 11, 12, 13, 14, 15)}
    for i in range(4, 11):
        t = (i - 3) / 8.0
        off[i] = off[3] * (1 - t) + off[11] * t
    grounds = [240 - off[i] for i in range(16)] + [228] * len(turn) + [244] * len(sit) + [239] * len(lie) + [bark_ground] * len(bark) + [give_ground] * len(give)
    all_frames = jump + turn + sit + lie + bark + give
    pngs, mids = [], []
    for f in all_frames:
        bb = f.getchannel("A").getbbox()
        mids.append((bb[0] + bb[2]) / 2.0)
        g = f if len(pngs) >= len(all_frames) - len(give) else f.resize((W, round(f.height * K)), Image.LANCZOS)
        b = io.BytesIO()
        g.save(b, "PNG", optimize=True)
        pngs.append(b.getvalue())
    lines = ["// LAIKA 64 - Laika's animations (the setup assistant, the main menu). GENERATED by",
             "// platform/xbox360/tools/gen_laika_anim.py from \"LAIKA ANIMADA\": do not edit.",
             "#ifndef XB_LAIKA_ANIM_H", "#define XB_LAIKA_ANIM_H", "",
             "#define LAIKA_W %d                // the frames' width (their height: their own)" % W,
             "#define LAIKA_K %.6ff         // the frames' scale to the drawings' (380 wide)" % K,
             "enum { LAIKA_JUMP = 0, LAIKA_TURN = %d, LAIKA_SIT = %d, LAIKA_LIE = %d, LAIKA_BARK = %d, LAIKA_GIVE = %d, LAIKA_FRAMES = %d };" % (
                 16, 16 + len(turn), 16 + len(turn) + len(sit), 16 + len(turn) + len(sit) + len(lie),
                 16 + len(turn) + len(sit) + len(lie) + len(bark), len(all_frames)),
             "#define LAIKA_TURN_N %d" % len(turn), "#define LAIKA_BARK_N %d" % len(bark), "#define LAIKA_GIVE_N %d" % len(give),
             "#define LAIKA_GIVE_W %d             // its frames as drawn (not LAIKA_W wide): %d x %d" % (give[0].width, give[0].width, give[0].height), ""]
    for i, p in enumerate(pngs):
        lines.append("static const unsigned char s_laikaPng%d[%d] = {" % (i, len(p)))
        for j in range(0, len(p), 24):
            lines.append("    " + ",".join("0x%02X" % c for c in p[j:j + 24]) + ",")
        lines.append("};")
    lines.append("static const unsigned char *const s_laikaPng[LAIKA_FRAMES] = { " + ", ".join("s_laikaPng%d" % i for i in range(len(pngs))) + " };")
    lines.append("static const unsigned s_laikaPngSize[LAIKA_FRAMES] = { " + ", ".join(str(len(p)) for p in pngs) + " };")
    lines.append("// each frame's paws line and the middle of her body, in the drawings' pixels (facing right)")
    lines.append("static const float s_laikaGround[LAIKA_FRAMES] = { " + ", ".join("%.1ff" % g for g in grounds) + " };")
    lines.append("static const float s_laikaMid[LAIKA_FRAMES] = { " + ", ".join("%.1ff" % m for m in mids) + " };")
    lines += ["", "#endif", ""]
    with open(OUT, "w", newline="\r\n") as f:
        f.write("\n".join(lines))
    print("wrote %s: %d frames, %d KB of pictures" % (OUT, len(pngs), sum(len(p) for p in pngs) // 1024))


if __name__ == "__main__":
    main()
