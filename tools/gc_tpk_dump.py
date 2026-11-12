#!/usr/bin/env python3
"""Dumps the GameCube GLOBALB.BUN's GlobalTexturesGAMECUBE.tpk to PNGs (#467:
the GC button icons, a style reference only -- nothing here ships).

Get the .BUN first: python tools/gc_zdir.py --get "GLOBAL\\GLOBALB.BUN"

Records (0x33310004, 0x7c each, little-endian): the name at +0x0c; at +0x30 the
texel offset, +0x34 the palette offset (both into the 0x33320002 leaf's data,
which starts at the leaf's first 0x80 boundary), +0x44 width, +0x46 height.
The 0x33310005 table (0x3c each) has the GX format at +0x3b (8 = C4). Texels
are GX tiles, palettes big-endian RGB5A3.

    python tools/gc_tpk_dump.py out/gc/GLOBAL/GLOBALB.BUN out/gc/tex
"""
import struct
import sys
from pathlib import Path

from PIL import Image


def rgb5a3(v):
    if v & 0x8000:
        r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
        return (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255)
    a, r, g, b = (v >> 12) & 7, (v >> 8) & 15, (v >> 4) & 15, v & 15
    return (r * 17, g * 17, b * 17, a * 255 // 7)


def chunks(d, o, end):
    while o + 8 <= end:
        cid, sz = struct.unpack_from("<II", d, o)
        yield o, cid, sz
        o += 8 + sz


def find(d, o, end, want):
    for co, cid, sz in chunks(d, o, end):
        if cid == want:
            return co, sz
        if cid & 0x80000000:
            r = find(d, co + 8, co + 8 + sz, want)
            if r:
                return r
    return None


def decode(d, base, rec, fmt):
    tex_off, pal_off = struct.unpack_from("<II", rec, 0x30)
    w, h = struct.unpack_from("<HH", rec, 0x44)
    img = Image.new("RGBA", (w, h))
    px = img.load()
    if fmt in (8, 9):  # C4 / C8
        n = 16 if fmt == 8 else 256
        pal = [rgb5a3(struct.unpack_from(">H", d, base + pal_off + 2 * i)[0])
               for i in range(n)]
        tw, th = 8, (8 if fmt == 8 else 4)
        p = base + tex_off
        for ty in range(0, h, th):
            for tx in range(0, w, tw):
                for y in range(th):
                    for x in range(tw):
                        if fmt == 8:
                            byte = d[p + (y * tw + x) // 2]
                            i = (byte >> 4) if x % 2 == 0 else (byte & 15)
                        else:
                            i = d[p + y * tw + x]
                        if tx + x < w and ty + y < h:
                            px[tx + x, ty + y] = pal[i]
                p += tw * th // (2 if fmt == 8 else 1)
        return img
    return None


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    d = src.read_bytes()
    i = d.find(b"GlobalTexturesGAMECUBE.tpk")
    tpk = max(o for o, cid, sz in chunks(d, 0, len(d)) if o < i and cid == 0xB3300000)
    tsz = struct.unpack_from("<I", d, tpk + 4)[0]
    end = tpk + 8 + tsz
    ro, rsz = find(d, tpk + 8, end, 0x33310004)
    fo, fsz = find(d, tpk + 8, end, 0x33310005)
    lo, lsz = find(d, tpk + 8, end, 0x33320002)
    base = (lo + 8 + 0x7F) & ~0x7F
    out.mkdir(parents=True, exist_ok=True)
    n = rsz // 0x7C
    for k in range(n):
        rec = d[ro + 8 + k * 0x7C: ro + 8 + (k + 1) * 0x7C]
        name = rec[0x0C:0x24].split(b"\0")[0].decode()
        fmt = d[fo + 8 + k * 0x3C + 0x3B]
        img = decode(d, base, rec, fmt)
        w, h = struct.unpack_from("<HH", rec, 0x44)
        print("%-28s %3dx%-3d fmt %d %s" % (name, w, h, fmt, "" if img else "(not decoded)"))
        if img:
            img.save(out / (name + ".png"))
            img.resize((w * 8, h * 8), Image.NEAREST).save(out / (name + "_8x.png"))


if __name__ == "__main__":
    main()
