#!/usr/bin/env python3
"""LAIKA 64 - builds the menus' other fonts (platform/xbox360/xb_ui_fonts.h).

Settings > Interface > Font picks the menus' letters: Inter (xb_ui_atlas.h,
gen_ui_atlas.py) or one of these. Each is a whole atlas like Inter's - the
same glyphs by the same code points (s_atlasCode), in two weights, the
Arabic of Noto Sans Arabic and the logo layers - so the UI only swaps the
texture and the glyph table.

Fonts (third_party/fonts/, SIL OFL 1.1, from github.com/google/fonts at the
commit in third_party/fonts/COMMIT.txt): Roboto, Montserrat, Nunito,
Orbitron, Press Start 2P. Variable fonts are drawn at weight 400 and 600
(SemiBold, the UI's bold); one with a single weight uses it for both.

Each font is scaled so that a line of text is as wide as in Inter: the
layouts (and gen_lang.py's width checks, made with Inter) stay right. A
glyph a font lacks (Orbitron has no Cyrillic) is Inter's.

Needs Pillow (with FreeType) and NumPy. Run from anywhere:
    python platform/xbox360/tools/gen_ui_fonts.py
"""
import os
import sys

import numpy as np
from PIL import ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_ui_atlas as A  # noqa: E402

ROOT = A.ROOT
OUT = os.path.join(ROOT, "platform", "xbox360", "xb_ui_fonts.h")
FONTS_DIR = os.path.join(ROOT, "third_party", "fonts")
# name shown, file, weights (regular, bold) for a variable font's wght axis.
FONTS = [
    ("Roboto", "roboto/Roboto[wdth,wght].ttf"),
    ("Montserrat", "montserrat/Montserrat[wght].ttf"),
    ("Nunito", "nunito/Nunito[wght].ttf"),
    ("Orbitron", "orbitron/Orbitron[wght].ttf"),
    ("Press Start 2P", "pressstart2p/PressStart2P-Regular.ttf"),
]
WEIGHTS = (400, 600)
SAMPLE = "The quick brown fox jumps over the lazy dog. Settings, Graphics 0123456789"


def load(path, size, weight):
    """The font at `size` pixels, its weight axis (if any) at `weight`, every other axis at its default."""
    f = ImageFont.truetype(path, size)
    try:
        axes = f.get_variation_axes()
    except Exception:
        return f
    vals = []
    for ax in axes:
        name = ax.get("name", b"")
        name = name.decode("latin-1") if isinstance(name, bytes) else str(name)
        if name.lower().startswith("weight") or name.lower() == "wght":
            vals.append(max(ax["minimum"], min(ax["maximum"], weight)))
        else:
            vals.append(ax["default"])
    f.set_variation_by_axes(vals)
    return f


def missing(font, ch):
    """1 when the font has no glyph for ch (it would draw its .notdef box)."""
    if ch == " ":
        return False
    a = font.getmask(ch)
    b = font.getmask("\U0010FFFD")   # private use: never in these fonts
    return a.size == b.size and bytes(a) == bytes(b)


def build(name, path):
    atlas = np.zeros((A.ATLAS_MAX_H, A.ATLAS_W), dtype=np.uint8)
    pk = A.Packer(A.ATLAS_W, A.ATLAS_MAX_H)
    first = [c for c in range(32, 127)] + A.EXTRAS
    later = A.LATIN + A.CYRILLIC
    logos = []
    for sd in A.logo_layers():
        x, y = pk.place(sd.shape[1], sd.shape[0])
        atlas[y:y + sd.shape[0], x:x + sd.shape[1]] = sd
        logos.append((x, y, sd.shape[1], sd.shape[0]))

    def put(font_hi, code, glyphs):
        sd, left, top, adv = A.glyph_sdf(font_hi, chr(code))
        if sd is None:
            glyphs[code] = (0, 0, 0, 0, 0.0, 0.0, adv)
            return
        x, y = pk.place(sd.shape[1], sd.shape[0])
        atlas[y:y + sd.shape[0], x:x + sd.shape[1]] = sd
        glyphs[code] = (x, y, sd.shape[1], sd.shape[0], left, top, adv)

    fonts = []
    fallbacks = 0
    for wi, weight in enumerate(WEIGHTS):
        inter = ImageFont.truetype(A.FONTS[wi], A.BASE * A.SS)
        probe = load(path, A.BASE * A.SS, weight)
        scale = inter.getlength(SAMPLE) / probe.getlength(SAMPLE)
        hi = load(path, int(round(A.BASE * A.SS * scale)), weight)
        asc, desc = hi.getmetrics()
        cap = -hi.getbbox("H", anchor="ls")[1] / A.SS
        xh = -hi.getbbox("x", anchor="ls")[1] / A.SS
        glyphs = {}
        for code in first + later:
            if missing(hi, chr(code)):
                put(inter, code, glyphs)   # Inter's own
                fallbacks += 1
            else:
                put(hi, code, glyphs)
        # Arabic: Noto, as tall as this font's capitals (as gen_ui_atlas.py does for Inter).
        ap = A.ARABIC_FONTS[wi]
        aprobe = ImageFont.truetype(ap, A.BASE * A.SS)
        alef = -aprobe.getbbox(chr(0xFE8D), anchor="ls")[1] / A.SS
        ar = ImageFont.truetype(ap, int(round(A.BASE * A.SS * cap / alef)))
        for code in A.ARABIC:
            put(ar, code, glyphs)
        fonts.append((asc / A.SS, desc / A.SS, cap, xh, glyphs))
        print("%s %d: scale %.3f, cap %.1f, x %.1f" % (name, weight, scale, cap, xh))
    used = pk.y + pk.row
    h = 512
    while h < used + 1:
        h *= 2
    print("%s: atlas %dx%d, %d glyphs from Inter" % (name, A.ATLAS_W, h, fallbacks))
    return fonts, logos, A.rle(atlas[:h].tobytes()), h


def main():
    codes = sorted(set([c for c in range(32, 127)] + A.EXTRAS + A.LATIN + A.CYRILLIC + A.ARABIC))
    # The same glyphs, in the same order, as Inter's atlas (the UI looks them up in s_atlasCode).
    src = open(A.OUT, encoding="utf-8").read()
    count = int(src.split("#define UI_ATLAS_COUNT ")[1].split()[0])
    assert count == len(codes), "xb_ui_atlas.h does not match gen_ui_atlas.py's glyph lists"
    lines = []
    lines.append("// LAIKA 64 - the menus' other fonts. GENERATED by platform/xbox360/tools/gen_ui_fonts.py: do not edit.")
    lines.append("// Each one an atlas like xb_ui_atlas.h's (same glyphs, by s_atlasCode), RLE-coded the same way.")
    lines.append("// Fonts (third_party/fonts/, SIL Open Font License 1.1): Roboto (c) 2011 Google Inc.; Montserrat (c) 2011 The")
    lines.append("// Montserrat Project Authors; Nunito (c) 2014 The Nunito Project Authors; Orbitron (c) 2018 The Orbitron Project")
    lines.append("// Authors; Press Start 2P (c) 2012 The Press Start 2P Project Authors. Arabic: Noto Sans Arabic (see xb_ui_atlas.h).")
    lines.append("#ifndef XB_UI_FONTS_H")
    lines.append("#define XB_UI_FONTS_H")
    lines.append("")
    lines.append('#include "xb_ui_atlas.h"')
    lines.append("")
    lines.append("struct UiExtraAtlas")
    lines.append("{")
    lines.append("    const char *name;")
    lines.append("    int height;                                    // atlas rows (UI_ATLAS_W wide)")
    lines.append("    const UiAtlasFont *font;                       // [2]: regular, bold")
    lines.append("    const UiAtlasGlyph (*glyph)[UI_ATLAS_COUNT];   // [2]")
    lines.append("    const UiAtlasRect *logo;                       // [3]")
    lines.append("    const unsigned char *rle;")
    lines.append("    unsigned rleSize;")
    lines.append("};")
    lines.append("")
    table = []
    for fi, (name, rel) in enumerate(FONTS):
        fonts, logos, packed, h = build(name, os.path.join(FONTS_DIR, rel))
        p = "s_font%d" % fi
        lines.append("// ---- %s ----" % name)
        lines.append("static const UiAtlasFont %sInfo[2] = {" % p)
        for asc, desc, cap, xh, _ in fonts:
            lines.append("    { %.3ff, %.3ff, %.3ff, %.3ff }," % (asc, desc, cap, xh))
        lines.append("};")
        lines.append("static const UiAtlasGlyph %sGlyph[2][UI_ATLAS_COUNT] = {" % p)
        for _, _, _, _, glyphs in fonts:
            lines.append("  {")
            for code in codes:
                g = glyphs.get(code, (0, 0, 0, 0, 0.0, 0.0, 0.0))
                lines.append("    { %d, %d, %d, %d, %.3ff, %.3ff, %.3ff }," % g)
            lines.append("  },")
        lines.append("};")
        lines.append("static const UiAtlasRect %sLogo[%d] = {" % (p, len(logos)))
        for r in logos:
            lines.append("    { %d, %d, %d, %d }," % r)
        lines.append("};")
        lines.append("static const unsigned char %sRle[%d] = {" % (p, len(packed)))
        for i in range(0, len(packed), 32):
            lines.append("    " + ",".join(str(b) for b in packed[i:i + 32]) + ",")
        lines.append("};")
        lines.append("")
        table.append('    { "%s", %d, %sInfo, %sGlyph, %sLogo, %sRle, sizeof(%sRle) },' % (name, h, p, p, p, p, p))
    lines.append("#define UI_EXTRA_FONTS %d" % len(FONTS))
    lines.append("static const UiExtraAtlas s_extraAtlas[UI_EXTRA_FONTS] = {")
    lines.extend(table)
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
