#!/usr/bin/env python3
"""LAIKA 64 - the custom avatars (platform/xbox360/xb_avatar_custom.h), drawn for one player each.

Each one comes from a GIF in the repository's top folder: AVATAR_CUSTOM_BASE + its index in CUSTOM below is the
number the server keeps for its player. Its frames are cropped square, made round and put side by side in one
PNG strip, as the LAIKA team's animated avatar (gen_brand_anim.py).

    python platform/xbox360/tools/gen_custom_avatars.py
"""
import os, io
from PIL import Image, ImageDraw, ImageSequence

R = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
X = os.path.join(R, 'platform', 'xbox360')
S, N = 96, 40                     # frame size, frames kept
CUSTOM = ['GOKUBLACK.gif']        # 1000: the first custom avatar (a test profile's)


def c_array(name, data, note):
    out = ['static const unsigned char %s[%d] = {   // %s' % (name, len(data), note)]
    for i in range(0, len(data), 32):
        out.append('    ' + ','.join(str(b) for b in data[i:i + 32]) + ',')
    out.append('};')
    return out


def strip_of(path):
    src = Image.open(path)
    frames = [f.convert('RGB') for f in ImageSequence.Iterator(src)]
    durations = [f.info.get('duration', 100) for f in ImageSequence.Iterator(Image.open(path))]
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
    b = io.BytesIO()
    # 255 colours and a transparent one (the corners outside the circle): a quarter of the size
    alpha = strip.getchannel('A')
    pal = strip.convert('RGB').quantize(colors=255, method=Image.MEDIANCUT, dither=Image.NONE)
    # entry 255 must exist in the palette: an index past it reads opaque black (h64_png.cpp), square corners
    colours = pal.getpalette()[:255 * 3]
    pal.putpalette(colours + [0] * (256 * 3 - len(colours)))
    pal.paste(255, mask=alpha.point(lambda a: 255 if a < 128 else 0))
    pal.info['transparency'] = 255
    pal.save(b, 'PNG', optimize=True, transparency=255)
    # the GIF's own speed, spread over the frames kept
    return b.getvalue(), sum(durations) / 1000.0 / N


def main():
    out = ['// LAIKA 64 - the custom avatars (platform/xbox360/tools/gen_custom_avatars.py): do not edit.',
           '#ifndef XB_AVATAR_CUSTOM_H', '#define XB_AVATAR_CUSTOM_H', '',
           '#define AVATAR_CUSTOM_SIZE %d' % S, '#define AVATAR_CUSTOM_FRAMES %d' % N, '#define AVATAR_CUSTOM_COUNT %d' % len(CUSTOM), '']
    names, sizes, speeds = [], [], []
    for i, name in enumerate(CUSTOM):
        data, frame_s = strip_of(os.path.join(R, name))
        out += c_array('s_avatarCustomPng%d' % i, data, '%s: %d frames of %dx%d side by side' % (name, N, S, S))
        out.append('')
        names.append('s_avatarCustomPng%d' % i)
        sizes.append(str(len(data)))
        speeds.append('%.4ff' % frame_s)
        print('%s: %d frames of %dx%d, %d bytes, %.0f ms a frame' % (name, N, S, S, len(data), frame_s * 1000))
    out.append('static const unsigned char *const s_avatarCustomPng[AVATAR_CUSTOM_COUNT] = { %s };' % ', '.join(names))
    out.append('static const unsigned s_avatarCustomPngSize[AVATAR_CUSTOM_COUNT] = { %s };' % ', '.join(sizes))
    out.append('static const float s_avatarCustomFrameS[AVATAR_CUSTOM_COUNT] = { %s };   // seconds a frame' % ', '.join(speeds))
    out += ['', '#endif', '']
    open(os.path.join(X, 'xb_avatar_custom.h'), 'w', encoding='utf-8', newline='\r\n').write('\n'.join(out))


main()
