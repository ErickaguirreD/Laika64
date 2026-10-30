#!/usr/bin/env python3
"""LAIKA 64 - the main logo and the title art, from "LAIKA VARIANTES.png".

Writes:
  platform/xbox360/tools/laika_logo.png   the original (multicolour) logo, 192 high: gen_logo.py makes xb_logo.h of it
  platform/xbox360/title/art/icon.png     64 x 64   the XEX's title image (X_IMAGEID_GAME): the wolf emblem
  platform/xbox360/title/art/cover.png    900 x 1270, cover_small.png 219 x 300, background.png 1280 x 720

    python platform/xbox360/tools/gen_laika_art.py "LAIKA VARIANTES.png"
    python platform/xbox360/tools/gen_logo.py platform/xbox360/tools/laika_logo.png
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_logo_variants as gv  # noqa: E402  (the cut-out)

ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
ART = os.path.join(ROOT, "platform", "xbox360", "title", "art")
BG, BG2 = (14, 16, 23), (30, 34, 48)


def original_logo(sheet):
    """The top-left cell (the multicolour logo), cut as the variants are."""
    cw, ch = sheet.width // 2, sheet.height // 3
    alpha = np.asarray(sheet)[..., 3]
    solid = (alpha[:, :cw] > 170).sum(1)
    b = ch - 50 + int(np.argmin(solid[ch - 50:ch + 50]))
    return gv.cut(sheet.crop((0, 0, cw, b)))


def emblem(logo):
    """The wolf emblem: the logo's left part, up to the first empty columns after it."""
    a = np.asarray(logo)[..., 3] > 60
    cols = a.sum(0)
    x = int(logo.height * 0.6)
    while x < logo.width and cols[x] > 2:
        x += 1
    e = logo.crop((0, 0, x, logo.height))
    ys, xs = np.nonzero(np.asarray(e)[..., 3] > 8)
    return e.crop((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1))


def backdrop(w, h):
    """The UI's dark background: a soft light in the middle."""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt(((xx - w / 2) / (w * 0.6)) ** 2 + ((yy - h * 0.45) / (h * 0.6)) ** 2).clip(0, 1)
    rgb = np.zeros((h, w, 4), np.float32)
    for k in range(3):
        rgb[..., k] = BG2[k] * (1 - d) + BG[k] * d
    rgb[..., 3] = 255
    return Image.fromarray(rgb.astype(np.uint8), "RGBA")


def paste_fit(dst, img, box):
    x0, y0, x1, y1 = box
    k = min((x1 - x0) / img.width, (y1 - y0) / img.height)
    im = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
    glow = im.split()[3].filter(ImageFilter.GaussianBlur(max(2, im.height // 12)))
    halo = Image.new("RGBA", im.size, (90, 140, 255, 0))
    halo.putalpha(glow.point(lambda v: v * 0.35))
    pos = (x0 + (x1 - x0 - im.width) // 2, y0 + (y1 - y0 - im.height) // 2)
    dst.alpha_composite(halo, pos)
    dst.alpha_composite(im, pos)


def main():
    sheet = Image.open(sys.argv[1]).convert("RGBA")
    logo = original_logo(sheet)
    logo.save(os.path.join(HERE, "laika_logo.png"))
    emb = emblem(logo)
    # The icon: the emblem on the dark background, rounded off.
    icon = backdrop(256, 256)
    paste_fit(icon, emb, (22, 22, 234, 234))
    mask = Image.new("L", (256, 256), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 255, 255), 44, fill=255)
    icon.putalpha(mask)
    icon.resize((64, 64), Image.LANCZOS).save(os.path.join(ART, "icon.png"))
    # The cover: the emblem big, the logo under it.
    cover = backdrop(900, 1270)
    paste_fit(cover, emb, (150, 230, 750, 830))
    paste_fit(cover, logo, (60, 900, 840, 1110))
    cover.convert("RGB").save(os.path.join(ART, "cover.png"))
    cover.convert("RGB").resize((219, 300), Image.LANCZOS).save(os.path.join(ART, "cover_small.png"))
    # The dashboard background: the logo in the middle.
    bg = backdrop(1280, 720)
    paste_fit(bg, logo, (200, 240, 1080, 480))
    bg.convert("RGB").save(os.path.join(ART, "background.png"))
    print("logo %dx%d, emblem %dx%d" % (logo.width, logo.height, emb.width, emb.height))


if __name__ == "__main__":
    main()
