#!/usr/bin/env python3
r"""A car's drop shadow and neon ground glow: three textures out of its TEXTURES.BIN.

Retail draws two things on the road under a car, both in
CarRenderInfo::Render (Speed.elf 0x80239018):

  * a 4x4 grid laid over the road under the car's body box, drawn with
    <CAR>_SHADOWIG in the world (<CAR>_SHADOWFE in the frontend), and
  * the same grid again with <CAR>_NEON when the car has neon fitted -- the
    pool of light under it.

The CarRenderInfo constructor (0x802314c0) looks up all three by name in the
car's own texture pack. Every car the player can own ships them, 128x64 each;
traffic cars do not, and retail then draws no grid for them.

The formats:

  * SHADOWIG and SHADOWFE are black, with the shadow's shape in the alpha. They
    ship as GPU_A8, half the size of car_pack.py's 16bpp formats. The soft edge
    also keeps its 256 steps; RGBA4444 would band it into 16.
  * NEON is opaque grey: the glow's shape is its brightness. It is DXT1 in the
    TPK, so its texels are already 565 values (green differs from red and blue
    by up to 6/255, the extra green bit). RGB565 keeps it exactly. An exactly
    grey texture would go to GPU_L8, but no shipped car has one.

A texture that breaks its pattern falls back to car_pack's own encoder, so
nothing is lost if a car turns out different.

The render state is carried along as car_pack.texture_material() packs it:
SHADOWIG is alphaBlendType 1 (normal blend), NEON is 2 (additive).

Layout (little-endian; 3DS Files/source/carshadow.c mirrors it):

    0x00  char magic[4] "NFSS"
    0x04  u32  version = 1
    0x08  u32  count = 3     the slots below, in this order: SHADOWIG, SHADOWFE, NEON
    0x0c  u32  reserved
    0x10  Tex[3] { u32 width, height;
                   u32 format;       GPU_TEXCOLOR (3 RGB565, 4 RGBA4, 7 L8, 8 A8)
                   u32 maxLevel;     mip levels below level 0, as C3D_TexCalcMaxLevel
                   u32 offset, size; 0 size: the car has no such texture
                   u32 material; }   car_pack.texture_material()
    0x64  <texture blobs>  PICA200-tiled, each with its whole mip chain

Usage:
    python tools/carshadow_pack.py "PC Files" --out "<sd>/nfsu2data/CARS"
    python tools/carshadow_pack.py "PC Files" --out DIR --car TIBURON -v
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import car_pack
from track_pack import bstring_hash

MAGIC = b"NFSS"
VERSION = 1
SUFFIXES = ("_SHADOWIG", "_SHADOWFE", "_NEON")
ENTRY = struct.Struct("<7I")
HDR = 0x10 + ENTRY.size * len(SUFFIXES)
EXT = ".nfss"

# GPU_TEXCOLOR, as citro3d numbers it.
GPU_RGB565 = 3
GPU_RGBA4 = 4
GPU_L8 = 7
GPU_A8 = 8


def tile8(plane, w, h):
    """One byte a texel, in the PICA200's 8x8 Morton tiles. Same order as
    car_pack.encode_texture(), one byte wide instead of two."""
    out = bytearray(w * h)
    tiles_x = w // 8
    for y in range(h):
        ty, iy = y >> 3, y & 7
        for x in range(w):
            tx, ix = x >> 3, x & 7
            out[(ty * tiles_x + tx) * 64 + car_pack._TILE[iy * 8 + ix]] = plane[y * w + x]
    return bytes(out)


def pick_format(rgba):
    """A8 for a black texture, L8 for an opaque grey one, else None."""
    px = range(0, len(rgba), 4)
    if all(rgba[i] == 0 and rgba[i + 1] == 0 and rgba[i + 2] == 0 for i in px):
        return GPU_A8
    if all(rgba[i + 3] == 255 and rgba[i] == rgba[i + 1] == rgba[i + 2] for i in px):
        return GPU_L8
    return None


def encode_chain(rgba, w, h):
    """(GPU_TEXCOLOR, maxLevel, blob): level 0 and every mip after it, each a
    box-halving of the one above, the way car_pack.encode_texture_chain()
    builds its chains."""
    fmt = pick_format(rgba)
    if fmt is None:
        f, blob, maxlvl = car_pack.encode_texture_chain(rgba, w, h)
        return (GPU_RGB565 if f == car_pack.FMT_RGB565 else GPU_RGBA4), maxlvl, blob
    chan = 3 if fmt == GPU_A8 else 0
    maxlvl = car_pack.mip_max_level(w, h)
    blob = bytearray()
    cur, cw, ch = rgba, w, h
    for lvl in range(maxlvl + 1):
        if lvl:
            cur, cw, ch = car_pack.box_downscale(cur, cw, ch, max(cw, ch) // 2)
        blob += tile8(cur[chan::4], cw, ch)
    return fmt, maxlvl, bytes(blob)


def pack_car(car, textures):
    """The .nfss bytes for one car, or None when it has none of the three."""
    entries, blobs, off = [], [], HDR
    for suffix in SUFFIXES:
        h = bstring_hash(car + suffix)
        t = textures.get(h)
        if t is None:
            entries.append(ENTRY.pack(0, 0, 0, 0, 0, 0, 0))
            continue
        _name, w, hgt, rgba = t
        fmt, maxlvl, blob = encode_chain(rgba, w, hgt)
        entries.append(ENTRY.pack(w, hgt, fmt, maxlvl, off, len(blob),
                                  textures.mat.get(h, 0)))
        blobs.append(blob)
        off += len(blob)
    if not blobs:
        return None
    hdr = struct.pack("<4sIII", MAGIC, VERSION, len(SUFFIXES), 0) + b"".join(entries)
    assert len(hdr) == HDR, len(hdr)
    return hdr + b"".join(blobs)


def describe(blob):
    out = []
    for i, suffix in enumerate(SUFFIXES):
        w, h, fmt, lvl, _off, size, mat = ENTRY.unpack_from(blob, 0x10 + i * ENTRY.size)
        if size:
            name = {GPU_A8: "A8", GPU_L8: "L8", GPU_RGBA4: "RGBA4",
                    GPU_RGB565: "RGB565"}[fmt]
            out.append("%s %dx%d %s +%d mips blend %d" % (suffix[1:], w, h, name, lvl,
                                                           mat & 3))
    return ", ".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the PC Files directory")
    ap.add_argument("--out", metavar="DIR", required=True,
                    help="write <CAR>%s into DIR (the card's nfsu2data/CARS)" % EXT)
    ap.add_argument("--car", action="append", help="only this car (repeatable)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    exe = os.path.join(args.pc_root, "SPEED2.EXE")
    cars_dir = os.path.join(args.pc_root, "CARS")
    cars = args.car or sorted(os.listdir(cars_dir))
    os.makedirs(args.out, exist_ok=True)
    wrote = total = 0
    for car in cars:
        tex_path = os.path.join(cars_dir, car, "TEXTURES.BIN")
        if not os.path.isfile(tex_path):
            continue
        blob = pack_car(car, car_pack.load_textures(tex_path, exe))
        if blob is None:
            if args.verbose:
                print("  %-12s none of the three (a traffic car or a part library)" % car)
            continue
        with open(os.path.join(args.out, car + EXT), "wb") as fh:
            fh.write(blob)
        wrote += 1
        total += len(blob)
        if args.verbose:
            print("  %-12s %6d B  %s" % (car, len(blob), describe(blob)))
    print("wrote %d %s files, %d B" % (wrote, EXT, total))
    return 0 if wrote else 1


if __name__ == "__main__":
    sys.exit(main())
