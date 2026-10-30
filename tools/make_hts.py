#!/usr/bin/env python3
"""Builds an HD texture pack (.hts) for LAIKA 64 from a folder of PNG files.

The PNGs use the usual Rice / GLideN64 hi-res texture names, the ones texture
packs are shared with (and GLideN64 or Project64 dump):

    <GAME NAME>#<texture crc>#<format>#<size>_<anything>.png
    <GAME NAME>#<texture crc>#<format>#<size>#<palette crc>_<anything>.png

for example "SUPER MARIO 64#3C2D8A1F#0#2_all.png". The crc is the texture's
checksum (8 hex digits), format and size the N64 texture format (0 RGBA, 1 YUV,
2 CI, 3 IA, 4 I) and texel size (0 4-bit .. 3 32-bit), and the palette crc is
there for colour-indexed textures. Sub-folders are read too, and files with
other names are skipped (listed with -v).

The pack is written in the layout LAIKA 64 reads (render/xenos/h64_hts.cpp):
a 16-byte header with the index's position, one zlib-compressed RGBA record
per texture, then the index (count, then checksum and offset per texture).
Put it in hires_texture\\<GAME NAME>\\ on the console, next to the .xex.

Needs Pillow (pip install pillow).

    python tools/make_hts.py "C:\\packs\\SUPER MARIO 64" "SUPER MARIO 64.hts"
    python tools/make_hts.py <folder> <out.hts> [--max 2048] [-v]

--max N scales down textures larger than N pixels on their longer side (the
console reads at most 4096, and smaller ones load faster and take less memory).
"""
import argparse
import os
import re
import struct
import sys
import zlib

from PIL import Image

NAME = re.compile(r'#([0-9A-Fa-f]{8})#([0-4])#([0-3])(?:#([0-9A-Fa-f]{8}))?_[^#]*\.png$', re.IGNORECASE)
GLF_GZ, GLF_RGBA, GLF_RGBA8, GLT_UBYTE = 0x80000000, 0x1908, 0x8058, 0x1401


def scan(folder, verbose):
    """(checksum, formatSize, path) for every PNG named like a texture, the first one kept on a repeat."""
    found, skipped = {}, 0
    for root, _dirs, files in os.walk(folder):
        for f in sorted(files):
            if not f.lower().endswith('.png'):
                continue
            m = NAME.search(f)
            if not m:
                skipped += 1
                if verbose:
                    print('  skipped (not a texture name): %s' % f)
                continue
            crc, fmt, size, pal = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), m.group(4)
            key = (int(pal, 16) << 32 if pal else 0) | crc
            if key in found:
                if verbose:
                    print('  repeated %016X, kept %s' % (key, os.path.basename(found[key][1])))
                continue
            found[key] = (fmt << 8 | size, os.path.join(root, f))
    return found, skipped


def record(path, format_size, max_side):
    img = Image.open(path).convert('RGBA')
    w, h = img.size
    if max_side and max(w, h) > max_side:
        k = max_side / float(max(w, h))
        img = img.resize((max(1, round(w * k)), max(1, round(h * k))), Image.LANCZOS)
        w, h = img.size
    if w > 4096 or h > 4096:
        raise ValueError('%s is %dx%d: the console reads 4096 at most (use --max)' % (path, w, h))
    data = zlib.compress(img.tobytes(), 9)
    head = struct.pack('<IIIHHBHI', w, h, GLF_RGBA8 | GLF_GZ, GLF_RGBA, GLT_UBYTE, 1, format_size, len(data))
    return head + data, w, h


def main():
    ap = argparse.ArgumentParser(description='Builds a LAIKA 64 .hts texture pack from Rice/GLideN64-named PNG files.')
    ap.add_argument('folder', help='folder with the PNG textures (sub-folders too)')
    ap.add_argument('out', help='the .hts file to write')
    ap.add_argument('--max', type=int, default=0, help='scale textures down to at most this many pixels on their longer side')
    ap.add_argument('-v', '--verbose', action='store_true', help='list skipped and repeated files')
    a = ap.parse_args()

    found, skipped = scan(a.folder, a.verbose)
    if not found:
        sys.exit('No textures found in %s (names like "GAME#1234ABCD#0#2_all.png")' % a.folder)
    print('%d textures%s' % (len(found), ', %d other files skipped' % skipped if skipped else ''))

    index, failed = [], 0
    with open(a.out, 'wb') as f:
        f.write(b'\0' * 16)   # the header, written at the end
        for n, key in enumerate(sorted(found), 1):
            format_size, path = found[key]
            try:
                rec, w, h = record(path, format_size, a.max)
            except Exception as e:   # one bad file does not stop the pack
                failed += 1
                print('  cannot use %s: %s' % (path, e))
                continue
            index.append((key, f.tell() | format_size << 48))
            f.write(rec)
            if n % 200 == 0 or n == len(found):
                print('  %d / %d' % (n, len(found)))
        index_pos = f.tell()
        f.write(struct.pack('<I', len(index)))
        for key, off in index:
            f.write(struct.pack('<QQ', key, off))
        f.seek(0)
        f.write(struct.pack('<IIq', 0, 0, index_pos))
    size = os.path.getsize(a.out)
    print('wrote %s: %d textures, %.1f MB%s' % (a.out, len(index), size / 1048576.0, ', %d failed' % failed if failed else ''))


if __name__ == '__main__':
    main()
