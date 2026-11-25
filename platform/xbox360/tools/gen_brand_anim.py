"""LAIKA 64 - the animated logo and the team's animated avatar.

    python platform/xbox360/tools/gen_brand_anim.py
reads "LAIKA LOGO ANIMADO.gif" and "LAIKA AVATAR.gif" (the repository's top folder) and writes:
  xb_logo_anim.h    the logo's frames (cut to what they show, 160 high), in an atlas of 4 columns, a palette PNG
  xb_avatar_laika.h the team's avatar (63): 16 frames of 96x96, round, side by side, a palette PNG
"""
import io
import os

from PIL import Image, ImageDraw, ImageSequence

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
X = os.path.join(ROOT, 'platform', 'xbox360')


def c_array(name, data, comment):
    out = ['static const unsigned char %s[%d] = {   // %s' % (name, len(data), comment)]
    for j in range(0, len(data), 32):
        out.append('    ' + ','.join(str(b) for b in data[j:j + 32]) + ',')
    out.append('};')
    return out


def png(img):
    buf = io.BytesIO()
    img.quantize(colors=255, method=Image.Quantize.FASTOCTREE).save(buf, 'PNG', optimize=True)
    return buf.getvalue()


def logo():
    src = Image.open(os.path.join(ROOT, 'LAIKA LOGO ANIMADO.gif'))
    frames = [f.convert('RGBA') for f in ImageSequence.Iterator(src)]
    durs = [f.info.get('duration', 80) for f in ImageSequence.Iterator(Image.open(os.path.join(ROOT, 'LAIKA LOGO ANIMADO.gif')))]
    box = None
    for f in frames:
        b = f.getchannel('A').point(lambda v: 255 if v > 16 else 0).getbbox()
        if b:
            box = b if not box else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
    H = 160
    W = round((box[2] - box[0]) * H / (box[3] - box[1]))
    cols = 4
    rows = (len(frames) + cols - 1) // cols
    atlas = Image.new('RGBA', (W * cols, H * rows), (0, 0, 0, 0))
    for k, f in enumerate(frames):
        atlas.paste(f.crop(box).resize((W, H), Image.LANCZOS), ((k % cols) * W, (k // cols) * H))
    data = png(atlas)
    out = ['// LAIKA 64 - the animated logo (platform/xbox360/tools/gen_brand_anim.py from "LAIKA LOGO ANIMADO.gif"): do not edit.',
           '#ifndef XB_LOGO_ANIM_H', '#define XB_LOGO_ANIM_H', '',
           '#define UI_LOGO_ANIM_W %d   // a frame' % W, '#define UI_LOGO_ANIM_H %d' % H, '#define UI_LOGO_ANIM_FRAMES %d' % len(frames),
           '#define UI_LOGO_ANIM_COLS %d   // frames across the atlas' % cols, '#define UI_LOGO_ANIM_MS %d   // a frame lasts' % (sum(durs) // len(durs)), '']
    out += c_array('s_logoAnimPng', data, 'the atlas, %dx%d' % atlas.size)
    out += ['', '#endif', '']
    open(os.path.join(X, 'xb_logo_anim.h'), 'w', encoding='utf-8', newline='\r\n').write('\n'.join(out))
    print('logo: %d frames of %dx%d, %d bytes' % (len(frames), W, H, len(data)))


def avatar():
    src = Image.open(os.path.join(ROOT, 'LAIKA AVATAR.gif'))
    frames = [f.convert('RGB') for f in ImageSequence.Iterator(src)]
    S, N = 96, 16
    big = Image.new('L', (S * 4, S * 4), 0)
    ImageDraw.Draw(big).ellipse([0, 0, S * 4 - 1, S * 4 - 1], fill=255)
    mask = big.resize((S, S), Image.LANCZOS)
    strip = Image.new('RGBA', (S * N, S), (0, 0, 0, 0))
    for k in range(N):
        f = frames[int(k * len(frames) / N)]
        side = min(f.size)
        f = f.crop(((f.width - side) // 2, (f.height - side) // 2, (f.width - side) // 2 + side, (f.height - side) // 2 + side))
        f = f.resize((S, S), Image.LANCZOS).convert('RGBA')
        f.putalpha(mask)
        strip.paste(f, (k * S, 0))
    data = png(strip)
    out = ['// LAIKA 64 - the LAIKA team\'s animated avatar, 63 (platform/xbox360/tools/gen_brand_anim.py from "LAIKA AVATAR.gif"): do not edit.',
           '#ifndef XB_AVATAR_LAIKA_H', '#define XB_AVATAR_LAIKA_H', '',
           '#define AVATAR_LAIKA_ANIM_SIZE %d' % S, '#define AVATAR_LAIKA_ANIM_FRAMES %d' % N, '']
    out += c_array('s_avatarLaikaPng', data, '16 frames of %dx%d side by side' % (S, S))
    out += ['', '#endif', '']
    open(os.path.join(X, 'xb_avatar_laika.h'), 'w', encoding='utf-8', newline='\r\n').write('\n'.join(out))
    print('avatar: %d frames of %dx%d, %d bytes' % (N, S, S, len(data)))


logo()
avatar()
