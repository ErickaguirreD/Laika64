#!/usr/bin/env python3
"""Inspect a GlideHQ/GlideN64 .hts texture pack and check it against the layout
render/xenos/h64_hts.cpp assumes:

  u32 config; u32 ?; s64 indexPos;  ...records...  at indexPos: [u32 count] {u64 checksum; u64 offset}*
  record: u32 w, u32 h, u32 format, u16 texFormat, u16 pixType, u8 isHires, u16 formatSize, u32 dataSize, data

Usage: hts_dump.py pack.hts [--list N] [--find HEXCHECKSUM]
If the summary says "layout does not match", send the first 64 bytes it prints
(and the last 64) so the reader can be adjusted.
"""
import struct, sys

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    path = sys.argv[1]
    n_list = 20
    find = None
    a = sys.argv[2:]
    while a:
        if a[0] == '--list': n_list = int(a[1]); a = a[2:]
        elif a[0] == '--find': find = int(a[1], 16); a = a[2:]
        else: a = a[1:]
    d = open(path, 'rb').read()
    print("size %d bytes" % len(d))
    print("first 64 bytes:", d[:64].hex(' '))
    print("last 64 bytes: ", d[-64:].hex(' '))
    config, _u, pos = struct.unpack_from('<IIq', d, 0)
    print("config 0x%08X  indexPos %d" % (config, pos))
    if not (16 <= pos < len(d)):
        print("layout does not match: index position outside the file"); return 2
    idx = d[pos:]
    count = struct.unpack_from('<I', idx, 0)[0]
    if len(idx) >= 4 and (len(idx) - 4) % 16 == 0 and count * 16 == len(idx) - 4:
        base, n, kind = 4, count, "counted"
    elif len(idx) % 16 == 0:
        base, n, kind = 0, len(idx) // 16, "uncounted"
    else:
        print("layout does not match: index size %d fits neither variant" % len(idx)); return 2
    print("index: %s, %d entries" % (kind, n))
    shown = 0
    fmts = {}
    for i in range(n):
        ck, off = struct.unpack_from('<QQ', idx, base + i * 16)
        o = off & 0xFFFFFFFFFFFF
        tag = off >> 48
        if not (16 <= o < pos):
            print("entry %d: offset %d outside the data area" % (i, o)); continue
        w, h, fmt, tf, pt, hires, fs, size = struct.unpack_from('<IIIHHBHI', d, o)
        key = (hex(fmt), hex(tf), hex(pt))
        fmts[key] = fmts.get(key, 0) + 1
        if find is not None and ck == find:
            print("FOUND %016X: %dx%d format %08X texFormat %04X pixType %04X size %d tag %04X" % (ck, w, h, fmt, tf, pt, size, tag))
        if shown < n_list:
            print("  %016X  %4dx%-4d fmt %08X/%04X/%04X  %8d bytes  tag %04X" % (ck, w, h, fmt, tf, pt, size, tag)); shown += 1
    print("formats (format, texFormat, pixType): count")
    for k, v in sorted(fmts.items(), key=lambda kv: -kv[1]): print("  ", k, v)
    return 0

if __name__ == '__main__':
    sys.exit(main())
