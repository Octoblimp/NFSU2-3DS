#!/usr/bin/env python3
r"""Car neon: the colour table and the tube texture, out of GLOBAL/GLOBALB.BUN.

A saved car's five neon slots hold part-name hashes, not colours, exactly as
its paint slots do (paint_pack.py). What makes neon its own file rather than
four more rows of PAINT.nfsp is that a neon colour is *two* RGBA values, not
one RGB: the retail renderer interpolates between them, and the eleven PULSE
colours are the pair where they differ.

  UpdateNeonColours  Speed.elf 0x80233348

reads eight attributes off the part -- RED, GREEN, BLUE, ALPHA and RED2,
GREEN2, BLUE2, ALPHA2 -- and hands the renderer the first colour plus the
*difference* between the two. For the eleven plain colours the pair is
identical, so the difference is zero and the tube is a steady colour. For
"BLUE PULSE" and its ten siblings only the alpha differs, 16 against 64: the
pulse is an alpha oscillation, not a hue change. "NO NEON" is all zeroes, and
is how a car with the slot filled says it has none -- the slot is never empty
once the player has visited the shop.

The RGB of all 23 is already in PAINT.nfsp, because paint_pack.py's sweep
picks up anything with RED/GREEN/BLUE. That table is left alone: it is keyed
to one RGB per part and the paint path depends on it, so neon carries its own
copy of the colours it needs rather than changing the format under paint.

The texture is NEONBLUR, a 64x64 RGBA image in the same global TPK the car
body textures come from. The renderer draws each tube as a screen-aligned
quad stretched between two position markers, with this as the cross-section
falloff -- white, with the alpha ramp that makes the strip read as a round
tube rather than a flat band. Without it the tube would be a hard-edged
rectangle, so it is shipped rather than approximated.

The markers themselves need no packing: kit_pack.py already writes every
part's position markers into <KIT>.nfsk, neon ones included.

Layout (little-endian; 3DS Files/source/carneon.c mirrors it):

    0x00  char magic[4] "NFSN"
    0x04  u32  version = 1
    0x08  u32  numColours
    0x0c  u32  coloursOffset
    0x10  u32  texWidth
    0x14  u32  texHeight
    0x18  u32  texFormat      car_pack.tex_fmt_field (format | maxLevel << 8)
    0x1c  u32  texOffset
    0x20  u32  texSize
    0x24  u32  reserved[3]
    0x30  Colour[numColours] { u32 partHash; u8 r, g, b, a; u8 r2, g2, b2, a2; }
          sorted by hash, so the runtime can binary-search it
          <texture blob>     PICA200-swizzled, the mip chain a .nfsc's uses

Usage:
    python tools/neon_pack.py "PC Files" --out "<sd>/nfsu2data/GLOBAL"
    python tools/neon_pack.py "PC Files" --list
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import car_pack
import paint_pack
from track_pack import bstring_hash

NEON_MAGIC = b"NFSN"
NEON_VERSION = 1
NEON_HDR = 0x30
OUT_NAME = "NEON.nfsn"

GLOBAL_BUN = os.path.join("GLOBAL", "GLOBALB.BUN")

# The texture the tube is drawn with, by the name the CarRenderInfo
# constructor hashes at Speed.elf 0x80231d38.
NEONBLUR = "NEONBLUR"

# UpdateNeonColours' eight attributes, in its own order.
ATTRS = ("RED", "GREEN", "BLUE", "ALPHA", "RED2", "GREEN2", "BLUE2", "ALPHA2")


def neon_table(parts, verbose=False):
    """part hash -> (name, (r, g, b, a), (r2, g2, b2, a2)).

    A neon colour part is one carrying ALPHA: ordinary paint parts have
    RED/GREEN/BLUE and no alpha at all, so the one attribute the renderer
    needs and paint never has is what separates the two sets cleanly.
    """
    keys = [bstring_hash(a) for a in ATTRS]
    alpha = keys[3]
    table = {}
    for h, name, attrs in parts:
        if alpha not in attrs:
            continue
        v = [attrs.get(k, 0) for k in keys]
        if any(x < 0 or x > 255 for x in v):
            if verbose:
                print("  %08x %-22s out-of-range, skipped" % (h, name))
            continue
        table[h] = (name, tuple(v[:4]), tuple(v[4:]))
    if verbose:
        print("  %d neon colour parts" % len(table))
    return table


def load_neonblur(pc_root, verbose=False):
    """(w, h, fmtField, blob) for NEONBLUR, or None if the TPK has no such
    texture -- in which case nothing is written, because a tube drawn without
    its falloff is not the retail tube."""
    textures = car_pack.load_global_car_textures(pc_root, verbose=verbose)
    entry = textures.get(bstring_hash(NEONBLUR))
    if entry is None:
        return None
    name, w, h, rgba = entry[0], entry[1], entry[2], entry[3]
    fmt, blob, maxlvl = car_pack.encode_texture_chain(rgba, w, h)
    if verbose:
        print("  %s %dx%d fmt %d maxLevel %d, %d B"
              % (name, w, h, fmt, maxlvl, len(blob)))
    return w, h, car_pack.tex_fmt_field(fmt, maxlvl), blob


def pack_neon(table, tex):
    body = bytearray()
    for h in sorted(table):
        _n, c1, c2 = table[h]
        body += struct.pack("<I8B", h, *(c1 + c2))
    tex_off = NEON_HDR + len(body)
    w, hgt, fmt_field, blob = tex
    hdr = struct.pack("<4sIIIIIIII12x", NEON_MAGIC, NEON_VERSION, len(table),
                      NEON_HDR, w, hgt, fmt_field, tex_off, len(blob))
    assert len(hdr) == NEON_HDR, len(hdr)
    return bytes(hdr) + bytes(body) + bytes(blob)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the PC Files directory")
    ap.add_argument("--out", metavar="DIR", help="write %s into DIR" % OUT_NAME)
    ap.add_argument("--list", action="store_true", help="print the table")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    loud = args.verbose or args.list

    data = open(os.path.join(args.pc_root, GLOBAL_BUN), "rb").read()
    parts = paint_pack.read_parts(data, loud)
    table = neon_table(parts, loud)
    print("%d neon colour parts" % len(table))

    if args.list:
        for h in sorted(table, key=lambda k: table[k][0]):
            n, c1, c2 = table[h]
            print("  %08x  %-22s  %3d %3d %3d /%3d   %3d %3d %3d /%3d%s"
                  % ((h, n) + c1 + c2 + ("   pulse" if c1 != c2 else "",)))

    tex = load_neonblur(args.pc_root, loud)
    if tex is None:
        print("NEONBLUR is not in the global textures -- nothing written")
        return 1

    if args.out:
        blob = pack_neon(table, tex)
        os.makedirs(args.out, exist_ok=True)
        path = os.path.join(args.out, OUT_NAME)
        with open(path, "wb") as fh:
            fh.write(blob)
        print("wrote %s: %d colours, %dx%d texture, %d B"
              % (path, len(table), tex[0], tex[1], len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
