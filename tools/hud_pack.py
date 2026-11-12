#!/usr/bin/env python3
"""Convert the retail in-race HUD art into HUD.nfsh for the 3DS HUD renderer.

TASKS.md #147(b). The tachometer/speedometer cluster HUD_SingleRace.fng draws
is real authored art, but it is spread across two containers in two different
TPK variants, and the half that matters most is unreachable from C:

  Global/HudTexturesSingleRace.tpk   (in GLOBAL/InGameFreeRoam.bun, one
      container at 0x13800, 43 textures) -- the frontend/global variant: raw
      uncompressed DXT in a shared pixel leaf. 3DS Files/source/texpack.c
      already reads this one at runtime.

  GLOBAL/HUD_CustomTextures_00.bin -- the third variant: a 0x33310003 table of
      24-byte records, each texture independently JDLZ- *or HUFF*-compressed.
      The port has a C JDLZ decoder (source/jdlz.c) but no C HUFF decoder at
      all: tools/huff.py gets HUFF blocks open by emulating SPEED2.EXE's
      FUN_00573840 under Unicorn, which is not something to port to ARM for
      four textures. Two of the cluster's pieces are HUFF --
      3RDPERSON_RPMDIAL_CUSTOM_00 (the tach needle, 0x95479060) and
      3RDPERSON_TURBODIAL_CUSTOM_00 (0x6E85FB5D).

So the HUFF stays offline, exactly as every other compressed asset in this
port already does: decode both variants here, transcode to the same PICA200
16bpp swizzled form car_pack.py emits, and write one flat file the runtime
uploads with no decoder of any kind. Real art, real hashes, no recreation.

Note on the art itself: the custom set is authored grey and tinted at
runtime -- Tachometer's constructor (Speed.elf 0x80108D1C) colours the fill,
the needles and the tick plates from the RED/GREEN/BLUE attributes of the
car's HUD parts (RideInfo slots 0xA4-0xA6), white when a slot is empty. The
needle is a tapered bar in the lower half of its 16x128 texture, transparent
above the pivot. Entries whose texels are all identical are flagged UNIFORM in
the pack so the renderer can skip sampling them and draw a tinted quad; with
the P8 palettes read from the right place (TASKS.md #208) none of the shipped
ones is.

What goes in, and why each piece (Speed.elf, the GameCube build with symbols):

  * All eleven custom sets, HUD_CustomTextures_00..10.bin. The car's HUD part
    in RideInfo slot 0xA3 carries a HUDINDEX attribute, and Tachometer's
    constructor (0x80108D1C) hands it to ChooseLoadableTextures (0x80225918),
    which sprintf()s it into every custom name: TACH_FILL_CUSTOM_%02d,
    3RDPERSON_RPMDIAL_CUSTOM_%02d and so on. Eleven styles ship (STOCK,
    FLAMES, GRIFFIN, TABBY, BULLS EYE, SUPERSPORT, RETRO, PHANTOM, INVADER,
    CIRCUIT, BLING), so all eleven sets are packed and the runtime uploads only
    the one the car uses.
  * The eight 3RDPERSON_REDLINE_5500..9000 plates out of
    Global/HudTexturesGlobal.tpk in GLOBAL/InGameCommon.bun. The same function
    picks one by GetTachStyle(): the plate whose red band starts where the
    car's own redline does. The plain 3RDPERSON_REDLINE the .fng authors is
    never drawn in the third-person tach -- the constructor always replaces it.
  * The in-race pieces of Global/HudTexturesSingleRace.tpk the cluster draws,
    including DRIFT_ANGLE_BACKING2, which TurboMeter (0x801098B0) shows and
    hides together with the boost gauge.
  * The style table: every part in GLOBALB.BUN's car-parts database that
    carries a HUDINDEX attribute, as (part hash, HUDINDEX). That is the lookup
    Tachometer's constructor does on the car's slot-0xA3 part, and shipping it
    here saves the runtime a second parts-database reader for eleven rows.

Usage:
    python tools/hud_pack.py "PC Files" --out "<sd>/nfsu2data/GLOBAL"
    python tools/hud_pack.py "PC Files" --out ... --dump /tmp/hud   # + PNGs
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import car_pack
import dxt_transcode
import extract_global_textures as egt
import huff
import paint_pack

HUD_MAGIC = b"NFSH"
HUD_VERSION = 2
HUD_HDR = 0x20
HUD_REC = 16
HUD_STYLE_REC = 8

HUD_TEX_UNIFORM = 1 << 0

OUT_NAME = "HUD.nfsh"

CUSTOM_BIN = os.path.join("GLOBAL", "HUD_CustomTextures_%02d.bin")
CUSTOM_SETS = 11
INRACE_BUN = os.path.join("GLOBAL", "InGameFreeRoam.bun")
GLOBAL_HUD_BUN = os.path.join("GLOBAL", "InGameCommon.bun")
EXE = "SPEED2.EXE"
PARTS_BUN = os.path.join("GLOBAL", "GLOBALB.BUN")

# The nine records of each HUD_CustomTextures_%02d.bin, all of them part of
# the cluster, as ChooseLoadableTextures spells them. The info record clips its
# name field at 20 characters, so the full names come from the code; the pack
# is keyed by the hash each file's 0x33310003 table stores, and load_custom()
# checks that every one of those is bStringHash() of a name below.
CUSTOM_STEMS = [
    "3RDPERSON_RPMDIAL_CUSTOM_%02d",
    "TACH_FILL_CUSTOM_%02d",
    "3RDPERSON_10500LINES_CUSTOM_%02d",
    "3RDPERSON_7500LINES_CUSTOM_%02d",
    "3RDPERSON_8500LINES_CUSTOM_%02d",
    "3RDPERSON_9500LINES_CUSTOM_%02d",
    "3RDPERSON_TURBODIAL_CUSTOM_%02d",
    "3RDPERSON_TURBOFILL_CUSTOM_%02d",
    "3RDPERSON_TURBOLINES_CUSTOM_%02d",
]

# What the cluster additionally needs out of Global/HudTexturesSingleRace.tpk.
# The other textures in that pack are the SMS/phone, d-pad, drift and getaway
# overlays -- not this task, and left out rather than packed unused.
INRACE_NAMES = [
    "3RDPERSON_REDLINE",
    "SHIFT_UP_ICON",
    "TACH_NOS",
    "TACH_NOS_BACKING",
    "TACH_NOS_ALPHA",
    "3RDPERSON_TURBORED_1",
    "3RDPERSON_TURBORED_2",
    "DRIFT_ANGLE_BACKING2",
    # HUD_Drift.fng's angle box (TASKS.md #268 E): DriftGauge registers 1
    # with a turbo, 3 without.
    "DRIFT_ANGLE_BACKING1",
    "DRIFT_ANGLE_BACKING3",
    # LapCounter's lap box, LapCounterBacking_1.
    "3RDPERSON_BOXLAPS",
    # WrongWIndi's WrongWayImage (TASKS.md #247).
    "ICON_WRONGWAY",
    # RacePosition's PositionBacking, behind the place on the bottom screen
    # (TASKS.md #268 D, hud_draw_race_stats).
    "3RDPERSON_BOXCORNER",
]

# GetTachStyle's eight redline plates, out of Global/HudTexturesGlobal.tpk.
GLOBAL_HUD_NAMES = ["3RDPERSON_REDLINE_%d" % rpm
                    for rpm in (5500, 6000, 6500, 7000, 7500, 8000, 8500, 9000)]
# And LapCounter's time box, LapCounterBacking_2, which ships in the same
# common pack rather than beside its lap box.
GLOBAL_HUD_NAMES.append("3RDPERSON_BOXTIME")
# NitrousGauge's NOS_Mask: the half disc Update turns over the TACH_NOS arc
# (FEngSetRotationZ, 152 * nos - 180). hud.c reads its alpha on the CPU and is
# never drawn as an image of its own.
GLOBAL_HUD_NAMES.append("NOS_MASK")
# The rear-view mirror's frame and cut-out (TASKS.md #501): SPEED2.EXE's
# FUN_005b98b0 loads both out of this pack (0x8707e8 / 0x8707ec). After the
# mirror's scene, FUN_005caad0 blends RVM (black, alpha ~136 inside the
# trapezoid) over the capture and then writes RVM_MASK's alpha into it, so
# the mirror is the darkened view inside the trapezoid. Both 128x64 DXT3.
GLOBAL_HUD_NAMES += ["RVM", "RVM_MASK"]

# HUD_Drag.fng's art, out of GLOBAL/InGameDrag.bun's HudTexturesDrag.tpk
# (TASKS.md #268 D): ChooseLoadableTextures' drag plates and redline bands,
# and the textures the package authors itself. The tpk's info records clip
# names at 23 characters (DRAG_POSITION_BACKING_01..03 all read
# "DRAG_POSITION_BACKING_0"), so these are matched by bStringHash of the full
# name. Left out: the turbo gauge (the port has none), the SS_* split-screen
# set and DRAG_STYLE_POINTS_BACKING, which no drag HUD object authors.
DRAG_BUN = os.path.join("GLOBAL", "InGameDrag.bun")
DRAG_NAMES = (["DRAG_RPM_%d_LINES" % rpm for rpm in (7000, 8000, 9000, 10000)] +
              ["DRAG_RPM_REDLINES_%d" % rpm
               for rpm in (5500, 6000, 6500, 7000, 7500, 8000, 8500, 9000)] +
              ["DRAG_RPM_BACKING", "DRAG_RPM_RED_LINES", "DRAG_RPM_NEEDLE",
               "DRAG_SHIFT_LIGHT", "DRAG_HEAT_FILL", "DRAG_NOS_BACKING",
               "DRAG_NOS_FILL", "DRAG_POSITION_BACKING_01",
               "DRAG_POSITION_BACKING_02", "DRAG_POSITION_BACKING_03"])
# The drag tach is authored 256 x 512 and drawn half size, 128 x 256 on the
# top screen; the default cap would halve it again.
DRAG_PLATE_MAXDIM = 256
DRAG_PLATES = set(DRAG_NAMES[:4] + ["DRAG_RPM_BACKING"])

# HUD_Drift.fng's BestDriftBacking (TASKS.md #268 E), in the same pack as the
# cluster's art, whose info record clips it to "DRIFT_STYLE_POINTS_BACK": so
# matched by bStringHash of the full name, as the drag set is.
DRIFT_HASHED_NAMES = ["DRIFT_STYLE_POINTS_BACKING",
                      # The Outrun's GetAwayMeter (TASKS.md #268 H step 2),
                      # clipped the same way ("GETAWAY_DISTANCE_BACK...").
                      "GETAWAY_DISTANCE_BACKING",
                      "GETAWAY_DISTANCE_BAR",
                      "GETAWAY_DISTANCE_LINES"]


def bstring_hash(name):
    """The engine's bStringHash: h = h * 33 + c from 0xFFFFFFFF."""
    h = 0xFFFFFFFF
    for c in name.encode("ascii"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def load_styles(pc_root):
    """[(part hash, HUDINDEX)] for every part that carries a HUDINDEX, sorted
    by hash so the runtime can binary-search it."""
    data = open(os.path.join(pc_root, PARTS_BUN), "rb").read()
    key = bstring_hash("HUDINDEX")
    out = {}
    for name_hash, _name, attrs in paint_pack.read_parts(data):
        if key in attrs:
            out[name_hash] = attrs[key]
    if not out:
        sys.exit("%s: no part carries a HUDINDEX attribute" % PARTS_BUN)
    return sorted(out.items())


def is_uniform(rgba):
    """True when every texel is the same RGBA value. Same question
    texpack_is_uniform() answers at runtime for the frontend packs, asked here
    instead because the answer is a property of the art, not of the load."""
    return len(rgba) >= 4 and rgba.count(rgba[:4]) * 4 == len(rgba)


def load_custom(pc_root, dec, index):
    """hash -> (name, w, h, fmt, rgba) for HUD_CustomTextures_<index>.bin."""
    path = os.path.join(pc_root, CUSTOM_BIN % index)
    data = open(path, "rb").read()
    names = {bstring_hash(s % index): s % index for s in CUSTOM_STEMS}
    out = {}
    for hsh, name, w, h, fmt, rgba in dxt_transcode.iter_textures_with_hash(
            data, huff_dec=dec):
        if hsh not in names:
            sys.exit("%s: %08x (%s) is not one of the names "
                     "ChooseLoadableTextures builds" % (path, hsh, name))
        out[hsh] = (names[hsh], w, h, fmt, rgba)
    missing = [n for hh, n in sorted(names.items()) if hh not in out]
    if missing:
        sys.exit("%s did not yield %s" % (path, ", ".join(missing)))
    return out


def load_tpk_textures(pc_root, bun, by_hash=False):
    """name -> (hash, w, h, fourcc, rgba) for every ordinary TPK in a .bun;
    keyed by the hash instead with `by_hash`, for packs whose clipped names
    collide."""
    data = open(os.path.join(pc_root, bun), "rb").read()
    out = {}
    for off, size in egt.find_containers(data):
        for t in egt.parse_container(data, off, size)["textures"]:
            # RAWB covers both D3DFMT_A8R8G8B8 and the P8 entries
            # parse_container() expands through their CLUT; neither is
            # DXT-block encoded, so decoding them as DXT reads the wrong
            # number of bytes and comes out as static.
            if t["fourcc"] == b"RAWB":
                rgba = dxt_transcode.decode_bgra(
                    t["pixel_data"][:t["expected_size"]], t["width"], t["height"])
            else:
                rgba = dxt_transcode.decode_dxt(t["pixel_data"], t["width"],
                                                t["height"], t["fourcc"])
            out[t["hash"] if by_hash else t["name"]] = (t["hash"], t["width"], t["height"],
                              t["fourcc"].decode("ascii", "replace"), rgba)
    return out


def pack_hud(entries, blob, styles):
    """Serialise one HUD.nfsh.

    The 16-byte texture record is deliberately the same shape pack_nfsc()
    writes, so the runtime loader is the texture loop out of model3d.c with
    the geometry taken away -- except for the spare u16, which an .nfsc leaves
    zero and this uses for flags.

        0x00  magic 'NFSH'
        0x04  version
        0x08  texture count
        0x0c  pixel blob offset
        0x10  pixel blob size
        0x14  style count
        0x18  style table offset
        0x1c  (4 bytes reserved, zero)
        0x20  count * { u32 hash, u16 w, u16 h, u16 fmt, u16 flags,
                        u32 offset-into-blob }
              styles * { u32 part hash, u32 HUDINDEX }, sorted by hash
              the pixel blob
    """
    body = bytearray()
    for (hsh, w, h, fmt, flags, off) in entries:
        body += struct.pack("<IHHHHI", hsh, w, h, fmt, flags, off)
    style_off = HUD_HDR + len(body)
    for hsh, index in styles:
        body += struct.pack("<II", hsh, index)
    blob_off = HUD_HDR + len(body)
    hdr = struct.pack("<4sIIIIII4x", HUD_MAGIC, HUD_VERSION, len(entries),
                      blob_off, len(blob), len(styles), style_off)
    assert len(hdr) == HUD_HDR, len(hdr)
    return bytes(hdr) + bytes(body) + bytes(blob)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the PC Files directory")
    ap.add_argument("--out", metavar="DIR", required=True,
                    help="write %s into DIR" % OUT_NAME)
    ap.add_argument("--maxdim", type=int, default=128,
                    help="box-downscale by halves until both axes fit "
                         "(default 128: the cluster draws at roughly 128px on "
                         "a 400x240 top screen, so the 256px plates are pure "
                         "resident cost above that)")
    ap.add_argument("--dump", metavar="DIR",
                    help="also write each decoded texture as a PNG, for "
                         "eyeballing the art before it ships")
    args = ap.parse_args()

    picks = []  # (hash, name, w, h, srcfmt, rgba)
    exe = os.path.join(args.pc_root, EXE)
    if not os.path.isfile(exe):
        sys.exit("need %s to decode the HUFF-compressed records" % exe)
    dec = huff.HuffDecompressor(exe)
    for index in range(CUSTOM_SETS):
        custom = load_custom(args.pc_root, dec, index)
        for hsh in sorted(custom):
            name, w, h, fmt, rgba = custom[hsh]
            picks.append((hsh, name, w, h, fmt, rgba))

    for bun, names in ((INRACE_BUN, INRACE_NAMES),
                       (GLOBAL_HUD_BUN, GLOBAL_HUD_NAMES)):
        found = load_tpk_textures(args.pc_root, bun)
        for name in names:
            if name not in found:
                sys.exit("%s has no %s" % (bun, name))
            hsh, w, h, fmt, rgba = found[name]
            if hsh != bstring_hash(name):
                sys.exit("%s: %s is hashed %08x, not %08x"
                         % (bun, name, hsh, bstring_hash(name)))
            picks.append((hsh, name, w, h, fmt, rgba))

    by_hash = load_tpk_textures(args.pc_root, INRACE_BUN, by_hash=True)
    for name in DRIFT_HASHED_NAMES:
        hsh = bstring_hash(name)
        if hsh not in by_hash:
            sys.exit("%s has no %s (%08x)" % (INRACE_BUN, name, hsh))
        _h, w, h, fmt, rgba = by_hash[hsh]
        picks.append((hsh, name, w, h, fmt, rgba))

    by_hash = load_tpk_textures(args.pc_root, DRAG_BUN, by_hash=True)
    for name in DRAG_NAMES:
        hsh = bstring_hash(name)
        if hsh not in by_hash:
            sys.exit("%s has no %s (%08x)" % (DRAG_BUN, name, hsh))
        _h, w, h, fmt, rgba = by_hash[hsh]
        picks.append((hsh, name, w, h, fmt, rgba))

    if args.dump:
        os.makedirs(args.dump, exist_ok=True)
        try:
            from PIL import Image
        except ImportError:
            Image = None
        if Image is None:
            print("warning: Pillow not installed -- skipping --dump")
        else:
            for hsh, name, w, h, fmt, rgba in picks:
                Image.frombytes("RGBA", (w, h), rgba).save(
                    os.path.join(args.dump, "%s.png" % name))

    entries, blob = [], bytearray()
    for hsh, name, w, h, srcfmt, rgba in picks:
        flags = HUD_TEX_UNIFORM if is_uniform(rgba) else 0
        sw, sh = w, h
        cap = max(args.maxdim, DRAG_PLATE_MAXDIM) if name in DRAG_PLATES else args.maxdim
        rgba, w, h = car_pack.box_downscale(rgba, w, h, cap)
        fmt, pixels = car_pack.encode_texture(rgba, w, h)
        entries.append((hsh, w, h, fmt, flags, len(blob)))
        blob.extend(pixels)
        print("  %-34s %08x %4dx%-4d %-8s -> %3dx%-3d %-8s%s"
              % (name, hsh, sw, sh, srcfmt, w, h,
                 "RGB565" if fmt == car_pack.FMT_RGB565 else "RGBA4444",
                 "  UNIFORM" if flags & HUD_TEX_UNIFORM else ""))

    styles = load_styles(args.pc_root)
    print("  %d HUD styles: %s" % (len(styles), ", ".join(
        "%08x=%d" % st for st in styles)))
    out = pack_hud(entries, blob, styles)
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, OUT_NAME)
    with open(path, "wb") as fh:
        fh.write(out)
    print("wrote %s: %d textures, %d B pixels, %d B total"
          % (path, len(entries), len(blob), len(out)))


if __name__ == "__main__":
    main()
