#!/usr/bin/env python3
r"""Car lights: the three flare textures, the headlights' light on the road
and the headlight bulb colours, out of GLOBAL/GLOBALB.BUN.

A car's head- and brake-lights are not lamps in the model. The HEADLIGHT and
BRAKELIGHT parts carry position markers, and

  CreateCarLightFlares  Speed.elf 0x80235e50 / SPEED2.EXE 0x6204d0
  RenderFlaresOnCar     Speed.elf 0x80237158
  eRenderLightFlare     Speed.elf 0x800cbfcc

turn each marker into a camera-facing textured quad whose size and alpha
follow how squarely the light points at the eye. The markers need no packing:
kit_pack.py already writes every part's markers into <KIT>.nfsk. What the
renderer needs besides them is here.

Textures, by eLightFlareTextureNameHashes (Speed.elf 0x80358e30), in its
order so the index a flare parameter row carries (its eleventh word) reads
straight into this table, and then the one flare-less texture:

    0  HEADLIGHTFLAREINNER   128x64   the bright core of a headlight
    1  HEADLIGHTFLAREOUTER   128x64   its wider, dimmer halo
    2  HEADLIGHTGLOW         128x128  the brake-light glow
    3  2PLAYERHEADLIGHT1     128x128  the headlights' light on the road:
                                      pTextureInfo2PlayerHeadlight1, which
                                      RenderTextureHeadlights (Speed.elf
                                      0x80236f98 / SPEED2.EXE 0x60d5d0) lays
                                      flat ahead of the car
    4  HEADLIGHTS            128x64   the player's projected headlight: the
                                      PC's WORLDHEADLIGHT effects'
                                      HeadlightTex, thrown onto the world
                                      from a camera over the car's nose
                                      (TASKS.md #296, carlights_headlight())

Bulb colours: RenderFlaresOnCar draws the two headlight flares in the colour
of the car's HEADLIGHT_BULB part (save slot 0xA0, GC slot 0x9F), read off its
RED, GREEN and BLUE attributes. CarPartSlotMap (Speed.elf 0x803713ec) sends
that slot to part id 0x38, and the records carrying it are the fourteen bulbs
the shop sells: STOCK, XENON, five colours, seven BRIGHT colours. Their name
hashes are *not* unique in the database -- RED, BLUE, GREEN, PURPLE and YELLOW
are also part id 0x41's names, with other values -- so the table is built
from part id 0x38 records only, not by name. A car with the slot empty takes
the default the CarRenderInfo constructor stores, 0x0090aa90; the runtime
holds that, not this file.

Layout (little-endian; 3DS Files/source/carlights.c mirrors it):

    0x00  char magic[4] "NFSL"
    0x04  u32  version = 3
    0x08  u32  numBulbs
    0x0c  u32  bulbsOffset
    0x10  u32  numTextures = 5
    0x14  u32  texturesOffset
    0x18  u32  reserved[6]
    0x30  Bulb[numBulbs] { u32 partHash; u8 r, g, b, level; }
          sorted by hash, so the runtime can binary-search it
          Tex[numTextures] { u32 nameHash, width, height, format, offset, size; }
          format is car_pack.tex_fmt_field (format | maxLevel << 8)
          <texture blobs>    PICA200-swizzled, the mip chain a .nfsc's uses

Usage:
    python tools/carlights_pack.py "PC Files" --out "<sd>/nfsu2data/GLOBAL"
    python tools/carlights_pack.py "PC Files" --list
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import car_pack
import paint_pack
from track_pack import bstring_hash

LIGHTS_MAGIC = b"NFSL"
LIGHTS_VERSION = 3
LIGHTS_HDR = 0x30
BULB_SIZE = 8
TEX_SIZE = 24
OUT_NAME = "CARLIGHTS.nfsl"

GLOBAL_BUN = os.path.join("GLOBAL", "GLOBALB.BUN")

# eLightFlareTextureNameHashes, Speed.elf 0x80358e30, then InitCarEffects'
# (0x80230e80) pTextureInfo2PlayerHeadlight1, then the PC's HeadlightTex
# (GLOBALB.BUN's HEADLIGHTS 0x28eefa9c; the GameCube's HEADLIGHTS2 and
# HEADLIGHTFADE are not on the PC disc).
TEXTURES = ("HEADLIGHTFLAREINNER", "HEADLIGHTFLAREOUTER", "HEADLIGHTGLOW",
            "2PLAYERHEADLIGHT1", "HEADLIGHTS")

# CarPartSlotMap[0x9f]: the part id a HEADLIGHT_BULB record carries at +4.
BULB_PART_ID = 0x38


def bulb_table(data, verbose=False):
    """part hash -> (name, r, g, b, level) for every part id 0x38 record."""
    ch = paint_pack.find_parts_db(data)
    roff, rsz = ch[paint_pack.CH_PARTS]
    keys = [bstring_hash(a) for a in ("RED", "GREEN", "BLUE")]
    parts = paint_pack.read_parts(data)
    if rsz // paint_pack.PART_RECORD_SIZE != len(parts):
        raise SystemExit("part chunk disagrees with read_parts")
    table = {}
    for i, (h, name, attrs) in enumerate(parts):
        o = roff + i * paint_pack.PART_RECORD_SIZE
        if data[o + 4] != BULB_PART_ID:
            continue
        if not all(k in attrs for k in keys):
            raise SystemExit("bulb %s (0x%08x) has no RGB" % (name, h))
        rgb = [attrs[k] for k in keys]
        if any(v < 0 or v > 255 for v in rgb):
            raise SystemExit("bulb %s (0x%08x) colour out of range" % (name, h))
        if h in table:
            raise SystemExit("bulb 0x%08x appears twice" % h)
        table[h] = (name,) + tuple(rgb) + (data[o + 5] >> 5,)
    if verbose:
        print("  %d bulb parts" % len(table))
    return table


def load_textures(pc_root, verbose=False):
    """[(nameHash, w, h, fmtField, blob)] in TEXTURES order, or None if any is
    missing -- a flare drawn without its texture is not the retail flare."""
    textures = car_pack.load_global_car_textures(pc_root, verbose=verbose)
    out = []
    for n in TEXTURES:
        h = bstring_hash(n)
        entry = textures.get(h)
        if entry is None:
            print("%s is not in the global textures" % n)
            return None
        name, w, ht, rgba = entry[0], entry[1], entry[2], entry[3]
        fmt, blob, maxlvl = car_pack.encode_texture_chain(rgba, w, ht)
        if verbose:
            print("  %s %dx%d fmt %d maxLevel %d, %d B"
                  % (name, w, ht, fmt, maxlvl, len(blob)))
        out.append((h, w, ht, car_pack.tex_fmt_field(fmt, maxlvl), blob))
    return out


def pack_lights(table, textures):
    bulbs = bytearray()
    for h in sorted(table):
        _n, r, g, b, level = table[h]
        bulbs += struct.pack("<I4B", h, r, g, b, level)
    tex_off = LIGHTS_HDR + len(bulbs)
    blob_off = tex_off + TEX_SIZE * len(textures)
    entries = bytearray()
    blobs = bytearray()
    for h, w, ht, fmt_field, blob in textures:
        entries += struct.pack("<6I", h, w, ht, fmt_field, blob_off + len(blobs), len(blob))
        blobs += blob
        blobs += b"\0" * (-len(blobs) & 0x7f)   # each level-0 image 128-aligned
    hdr = struct.pack("<4sIIIII24x", LIGHTS_MAGIC, LIGHTS_VERSION, len(table),
                      LIGHTS_HDR, len(textures), tex_off)
    assert len(hdr) == LIGHTS_HDR, len(hdr)
    return bytes(hdr) + bytes(bulbs) + bytes(entries) + bytes(blobs)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the PC Files directory")
    ap.add_argument("--out", metavar="DIR", help="write %s into DIR" % OUT_NAME)
    ap.add_argument("--list", action="store_true", help="print the bulb table")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    loud = args.verbose or args.list

    data = open(os.path.join(args.pc_root, GLOBAL_BUN), "rb").read()
    table = bulb_table(data, loud)
    print("%d headlight bulbs" % len(table))
    if args.list:
        for h in sorted(table, key=lambda k: (table[k][4], table[k][0])):
            n, r, g, b, level = table[h]
            print("  %08x  %-14s  L%d  %3d %3d %3d" % (h, n, level, r, g, b))

    textures = load_textures(args.pc_root, loud)
    if textures is None:
        print("nothing written")
        return 1

    if args.out:
        blob = pack_lights(table, textures)
        os.makedirs(args.out, exist_ok=True)
        path = os.path.join(args.out, OUT_NAME)
        with open(path, "wb") as fh:
            fh.write(blob)
        print("wrote %s: %d bulbs, %d textures, %d B"
              % (path, len(table), len(textures), len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
