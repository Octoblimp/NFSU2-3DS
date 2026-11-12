#!/usr/bin/env python3
r"""Pack what retail's car-skin composite reads, as CARS/<CAR>.nfsv (TASKS.md #296).

NFSU2 paints a car by compositing one 512x512 skin on the CPU whenever the
car's look changes. CompositeSkin (SPEED2.EXE 0x61D790) gathers the layers and
CompositeSkin32 (0x611F10) blends them into one of the six DUMMY_SKINn
textures, which SwitchSkin (0x636840) then hands every body slot: <CAR>_SKIN1,
GLOBAL_SKIN1, SPOILER_SKIN, ROOF_SKIN and the two unnamed body slots. There is
no paint shader. A car's colour, its vinyls, its door lines and the colours of
its spoiler, roof scoop, mirrors, brakes, exhaust tips, engine and audio
(through seven 4x4 swatches the parts' UVs point at) are all texels of that
one texture.

The layers, in order, each an image plus an image named <image>_MASK:

  1-4  the VINYL_LAYER0..3 parts, deduplicated: <CAR>_<TEXTURE attribute>,
       in CARS/<CAR>/VINYLS.BIN (P8, 512x512)
  5    the doorline: <CAR>_DOORLINE, _DOORLINE_KIT or _DOORLINE_WIDEBODY
       (FUN_00612B70), in the car's TEXTURES.BIN (A8R8G8B8, 512x512)

This writes all of them for one car, the seven swatches' texel positions
(scanned out of DUMMY_SKIN1 in CARS/TEXTURES.BIN, as CompositeSkin32 does on
its first call), and the vinyl parts' TEXTURE / NUMCOLOURS / REMAP
attributes, so the runtime (3DS Files/source/carskin.c) can rebuild any skin
without GLOBALB.BUN resident.

Only what the composite reads is kept, losslessly, under zlib: the mask's
green byte is the only part of a mask CompositeSkin32 looks at, so masks are
a G plane; P8 images stay P8. All 893 vinyls of a car come to about 9 MB.

Layout, little-endian:

  0x00  'NFSV', u32 version (1)
  0x08  u16 skin width, u16 skin height   (DUMMY_SKIN1's)
  0x0c  u32 texture count, u32 part count
  0x14  u32 texture table, part table, swatch table and data offsets
  0x24  reserved to 0x30
  swatches  7 x { u32 count, u32 texel[16] }      texel = y * width + x
  textures  count x { u32 hash, u16 w, u16 h, u8 kind, u8, u16,
                      u32 data offset, u32 zlib size }    sorted by hash
            kind 0: w*h u32 0xAARRGGBB (the D3D surface's own dwords)
            kind 1: 256 u32 0xAARRGGBB palette, then w*h u8 indices
            kind 2: w*h u8, the green byte of each texel (a mask)
  parts     count x { u32 part hash, u32 texture hash, u16 NUMCOLOURS,
                      u8 REMAP, u8 }    sorted by part hash
  data      the zlib streams

Usage:
    python tools/vinyl_pack.py "PC Files" --out "<sd>/nfsu2data/CARS" [CAR ...]
    python tools/vinyl_pack.py "PC Files" --render skin.png --car TIBURON \
        --nfsv "<sd>/nfsu2data/CARS/TIBURON.nfsv" --paint 0xedcd9ac6 \
        [--vinyl 0x342b915d] [--doorline _KIT]
"""

import argparse
import os
import struct
import sys
import zlib
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import huff  # noqa: E402
import jdlz  # noqa: E402
import kit_pack as kp  # noqa: E402
import paint_pack as pp  # noqa: E402
from scan_texture_formats import iter_table_entries  # noqa: E402
from track_pack import bstring_hash  # noqa: E402

NFSV_MAGIC = b"NFSV"
NFSV_VERSION = 1
NFSV_HDR = 0x30
NFSV_TEX = 20
NFSV_PART = 12
SWATCHES = 7
SWATCH_MAX = 16

KIND_ARGB, KIND_P8, KIND_G8 = 0, 1, 2

D3DFMT_A8R8G8B8 = 21
D3DFMT_P8 = 0x29

DUMMY_SKIN1 = 0x530B82B0
# CompositeSkin32's swatch colours, compared as floats there (-1.9921875 is
# 0xBFFF0000 and so on); index i takes region colour i.
SWATCH_COLOURS = (0xBFFF0000, 0xBF00FF00, 0xBF0000FF, 0xBFFF00FF,
                  0xBF00FFFF, 0xBFFFFF00, 0xBFFFFFFF)
DOORLINE_VARIANTS = ("", "_KIT", "_WIDEBODY")

# The vinyl parts' attributes, by bStringHash of their names.
KEY_TEXTURE = bstring_hash("TEXTURE")
KEY_NUMCOLOURS = bstring_hash("NUMCOLOURS")
KEY_REMAP = bstring_hash("REMAP")


# ------------------------------------------------------------------ reading

def decompress(data, off, csize, hd):
    magic = data[off:off + 4]
    if magic == b"JDLZ":
        return jdlz.decompress(data, off)[0]
    if magic == b"HUFF":
        return hd.decompress_block(data[off:off + csize])
    return None


def raw_texture(block):
    """One texture block's base mip as it sits in the pack: (w, h, d3dfmt,
    pixels, palette). The trailer layout is dxt_transcode._decode_block_trailer's;
    this keeps the bytes instead of decoding them."""
    tr = block[-156:]
    pixels_off, palette_off = struct.unpack_from("<II", tr, 48)
    palette_size = struct.unpack_from("<I", tr, 60)[0]
    base_size = struct.unpack_from("<I", tr, 64)[0]
    w, h = struct.unpack_from("<HH", tr, 68)
    fmt = struct.unpack_from("<I", tr, 144)[0]
    region = block[:-156]
    pal = None
    if palette_size:
        at = palette_off - pixels_off
        pal = region[at:at + palette_size]
    return w, h, fmt, region[:base_size], pal


def read_table(path, hashes, hd):
    """hash -> raw_texture() for the entries of a TPK table in `hashes`, or for
    every entry when `hashes` is None."""
    data = open(path, "rb").read()
    out = {}
    for h, off, csize, _usize in iter_table_entries(data):
        if hashes is not None and h not in hashes:
            continue
        block = decompress(data, off, csize, hd)
        if block is not None:
            out[h] = raw_texture(block)
    return out


def g_plane(w, h, fmt, px, pal):
    """The green byte of every texel: all of a mask CompositeSkin32 reads."""
    if fmt == D3DFMT_P8:
        return px[:w * h].translate(bytes(pal[i * 4 + 1] for i in range(256)))
    if fmt == D3DFMT_A8R8G8B8:
        return px[1:w * h * 4:4]
    raise SystemExit("a mask in format 0x%x" % fmt)


def image_record(w, h, fmt, px, pal):
    if fmt == D3DFMT_P8:
        return KIND_P8, bytes(pal[:1024]) + bytes(px[:w * h])
    if fmt == D3DFMT_A8R8G8B8:
        return KIND_ARGB, bytes(px[:w * h * 4])
    raise SystemExit("an image in format 0x%x" % fmt)


def scan_swatches(pc_root, hd):
    """CompositeSkin32's first-call scan of the skin: for each swatch colour,
    the texels holding it, in memory order, at most 16."""
    t = read_table(os.path.join(pc_root, "CARS", "TEXTURES.BIN"), {DUMMY_SKIN1}, hd)
    if DUMMY_SKIN1 not in t:
        raise SystemExit("no DUMMY_SKIN1 in CARS/TEXTURES.BIN")
    w, h, fmt, px, _pal = t[DUMMY_SKIN1]
    if fmt != D3DFMT_A8R8G8B8:
        raise SystemExit("DUMMY_SKIN1 is format 0x%x, not A8R8G8B8" % fmt)
    dwords = struct.unpack("<%dI" % (w * h), px[:w * h * 4])
    lists = [[] for _ in SWATCH_COLOURS]
    for i, d in enumerate(dwords):
        if d in SWATCH_COLOURS:
            lists[SWATCH_COLOURS.index(d)].append(i)
    for c, l in zip(SWATCH_COLOURS, lists):
        # Retail has room for 16 and no bound check; more would be data this
        # port has never seen, so say so instead of truncating.
        if not l or len(l) > SWATCH_MAX:
            raise SystemExit("swatch 0x%08x has %d texels" % (c, len(l)))
    return w, h, lists


def vinyl_parts(pc_root):
    """part hash -> (TEXTURE string or None, NUMCOLOURS, REMAP) for every
    part carrying either of the vinyl attributes."""
    data = open(os.path.join(pc_root, pp.GLOBAL_BUN), "rb").read()
    db = kp.PartsDb(data)
    out = {}
    for h, attrs in db.attrs.items():
        if KEY_TEXTURE not in attrs and KEY_NUMCOLOURS not in attrs:
            continue
        tex = db.pstr(attrs[KEY_TEXTURE]) if KEY_TEXTURE in attrs else None
        out[h] = (tex, attrs.get(KEY_NUMCOLOURS, 0), 1 if attrs.get(KEY_REMAP, 0) else 0)
    return out


# ------------------------------------------------------------------ packing

def pack_car(pc_root, car, swatches, parts):
    exe = os.path.join(pc_root, "SPEED2.EXE")
    hd = huff.HuffDecompressor(exe)
    cars = os.path.join(pc_root, "CARS")
    sw, sh, lists = swatches

    want = {}
    for v in DOORLINE_VARIANTS:
        name = car + "_DOORLINE" + v
        want[bstring_hash(name)] = (name, False)
        want[bstring_hash(name + "_MASK")] = (name + "_MASK", True)
    own = read_table(os.path.join(cars, car, "TEXTURES.BIN"), set(want), hd)
    vin = read_table(os.path.join(cars, car, "VINYLS.BIN"), None, hd)
    masks = {kp.ext(h, "_MASK") for h in vin} & set(vin)

    recs = {}
    for th, t in list(own.items()) + list(vin.items()):
        w, h, fmt, px, pal = t
        if (w, h) != (sw, sh):
            # The composite walks image, mask and skin with one index, so a
            # layer of another size would be read as garbage. None ships.
            raise SystemExit("%s 0x%08x is %dx%d, the skin %dx%d" % (car, th, w, h, sw, sh))
        is_mask = want[th][1] if th in want else th in masks
        if is_mask:
            kind, raw = KIND_G8, g_plane(w, h, fmt, px, pal)
        else:
            kind, raw = image_record(w, h, fmt, px, pal)
        recs[th] = (w, h, kind, zlib.compress(raw, 9))

    prefix = car + "_"
    part_recs = []
    for ph in sorted(parts):
        tex, numc, remap = parts[ph]
        part_recs.append((ph, bstring_hash(prefix + tex) if tex else 0, numc, remap))

    o_tex = NFSV_HDR + SWATCHES * (4 + 4 * SWATCH_MAX)
    o_part = o_tex + len(recs) * NFSV_TEX
    o_data = o_part + len(part_recs) * NFSV_PART
    body = bytearray()
    for l in lists:
        body += struct.pack("<I%dI" % SWATCH_MAX, len(l), *(l + [0] * (SWATCH_MAX - len(l))))
    blob = bytearray()
    for th in sorted(recs):
        w, h, kind, z = recs[th]
        body += struct.pack("<IHHBBHII", th, w, h, kind, 0, 0, len(blob), len(z))
        blob += z
    for r in part_recs:
        body += struct.pack("<IIHBB", r[0], r[1], r[2], r[3], 0)
    hdr = struct.pack("<4sIHHIIIIII", NFSV_MAGIC, NFSV_VERSION, sw, sh, len(recs),
                      len(part_recs), o_tex, o_part, NFSV_HDR, o_data).ljust(NFSV_HDR, b"\0")
    found = sum(1 for r in part_recs if r[1] in recs)
    return hdr + bytes(body) + bytes(blob), len(recs), found, len(part_recs)


def _pack_one(args):
    pc_root, out_dir, car, swatches, parts = args
    blob, ntex, found, nparts = pack_car(pc_root, car, swatches, parts)
    if not ntex:
        return "%s: no vinyls and no doorlines, skipped" % car
    path = os.path.join(out_dir, car + ".nfsv")
    tmp = path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(blob)
    os.replace(tmp, path)
    return "%s: %d textures, %d of %d vinyl parts have art, %.1f MB" % (
        car, ntex, found, nparts, len(blob) / 1e6)


def player_cars(pc_root):
    """The 31 cars a player can own (kit_pack.DEFAULT_CARS). Traffic cars and
    part libraries carry a VINYLS.BIN too, empty, and some no TEXTURES.BIN."""
    cars = os.path.join(pc_root, "CARS")
    return sorted(c for c in kp.DEFAULT_CARS
                  if os.path.isfile(os.path.join(cars, c, "VINYLS.BIN")))


# ---------------------------------------------------------------- reference

class Nfsv:
    def __init__(self, path):
        d = open(path, "rb").read()
        if d[:4] != NFSV_MAGIC:
            raise SystemExit("%s is not an .nfsv" % path)
        (_m, _v, self.w, self.h, ntex, npart, o_tex, o_part, o_sw,
         o_data) = struct.unpack_from("<4sIHHIIIIII", d, 0)
        self.swatches = []
        for i in range(SWATCHES):
            n = struct.unpack_from("<I", d, o_sw + i * 68)[0]
            self.swatches.append(struct.unpack_from("<%dI" % n, d, o_sw + i * 68 + 4))
        self.tex = {}
        for i in range(ntex):
            th, w, h, kind, _a, _b, off, zs = struct.unpack_from("<IHHBBHII", d, o_tex + i * NFSV_TEX)
            self.tex[th] = (w, h, kind, d[o_data + off:o_data + off + zs])
        self.parts = {}
        for i in range(npart):
            ph, th, numc, remap, _p = struct.unpack_from("<IIHBB", d, o_part + i * NFSV_PART)
            self.parts[ph] = (th, numc, remap)

    def argb(self, th):
        import numpy as np
        w, h, kind, z = self.tex[th]
        raw = zlib.decompress(z)
        if kind == KIND_ARGB:
            return np.frombuffer(raw, "<u4").copy()
        if kind == KIND_P8:
            pal = np.frombuffer(raw[:1024], "<u4")
            return pal[np.frombuffer(raw[1024:], np.uint8)]
        raise SystemExit("0x%08x is a mask" % th)

    def g(self, th):
        import numpy as np
        w, h, kind, z = self.tex[th]
        if kind != KIND_G8:
            raise SystemExit("0x%08x is not a mask" % th)
        return np.frombuffer(zlib.decompress(z), np.uint8)


def remap_table():
    """T[c][b] = trunc(b * w(c)), w(c) = c * (float)(1/255) -- FUN_00611B00's
    products for an image channel c weighting a colour byte b, in single
    precision (the x87 under D3D9's default FPU mode)."""
    import numpy as np
    f = np.float32(1.0) / np.float32(255.0)
    w = np.arange(256, dtype=np.float32) * f
    b = np.arange(256, dtype=np.float32)
    return (b[None, :] * w[:, None]).astype(np.int32), w > f


def compose(nfsv, base, regions, layers):
    """CompositeSkin32 over numpy. `base` and `regions` are 0xAARRGGBB;
    `layers` is [(image hash, [4 remap colours] or None)], doorline last."""
    import numpy as np
    T, live = remap_table()
    skin = np.full(nfsv.w * nfsv.h, base, dtype=np.uint32)
    for th, remap in layers:
        img = nfsv.argb(th)
        m = nfsv.g(kp.ext(th, "_MASK")).astype(np.int32)
        if remap:
            sel = m != 0
            c = [(img[sel] >> s) & 0xFF for s in (16, 8, 0)]    # R, G, B weights
            out = np.zeros(int(sel.sum()), dtype=np.uint32)
            for shift in (0, 8, 16):
                acc = np.zeros(int(sel.sum()), dtype=np.int32)
                for k in range(3):
                    acc += np.where(live[c[k]], T[c[k], (remap[k] >> shift) & 0xFF], 0)
                out |= np.minimum(acc, 255).astype(np.uint32) << shift
            a = np.zeros(int(sel.sum()), dtype=np.int32)
            for k in range(3):
                a = np.maximum(a, np.where(live[c[k]], T[c[k], (remap[k] >> 24) & 0xFF], 0))
            img = img.copy()
            img[sel] = out | (a.astype(np.uint32) << 24)
        full = m >= 0x80
        skin[full] = img[full]
        part = (m > 0) & (m < 0x80)
        mi, si, mm = img[part], skin[part], m[part].astype(np.uint32)
        acc = np.zeros(int(part.sum()), dtype=np.uint32)
        for shift in (0, 8, 16, 24):
            acc |= ((((mi >> shift) & 0xFF) * mm >> 7) +
                    (((si >> shift) & 0xFF) * (128 - mm) >> 7)) << shift
        skin[part] = acc
    for i, texels in enumerate(nfsv.swatches):
        skin[list(texels)] = regions[i]
    return skin


def argb_of(table, part, fallback):
    """A colour part as CompositeSkin builds it: R | G<<8 | B<<16 | GLOSS<<24,
    then FUN_005BE040's swap to 0xAARRGGBB."""
    e = table.get(part)
    if e is None:
        return fallback
    _n, r, g, b, gloss = e
    return (gloss << 24) | (r << 16) | (g << 8) | b


def render(args):
    import numpy as np
    from PIL import Image
    nfsv = Nfsv(args.nfsv)
    data = open(os.path.join(args.pc_root, pp.GLOBAL_BUN), "rb").read()
    table = pp.paint_table(pp.read_parts(data))
    base = argb_of(table, int(args.paint, 16), None)
    if base is None:
        raise SystemExit("%s is not a colour part" % args.paint)
    layers = []
    for v in args.vinyl or ():
        ph, *cols = v.split(":")
        th, numc, remap = nfsv.parts[int(ph, 16)]
        if not numc:
            raise SystemExit("vinyl %s has no NUMCOLOURS: the composite fails" % ph)
        rc = None
        if remap:
            # An empty colour slot k is 0xFF in byte k: the image's own channel.
            rc = [0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000]
            for k, c in enumerate(cols):
                if c:
                    rc[k] = argb_of(table, int(c, 16), 0)
        if th in nfsv.tex:
            layers.append((th, rc))
    layers.append((bstring_hash(args.car + "_DOORLINE" + args.doorline), None))
    skin = compose(nfsv, base, [base] * SWATCHES, layers)
    a = skin.reshape(nfsv.h, nfsv.w)
    rgb = np.stack([(a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF], axis=-1).astype(np.uint8)
    Image.fromarray(rgb).save(args.render)
    print("wrote %s" % args.render)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the PC Files directory")
    ap.add_argument("cars", nargs="*", help="car folders (default: the 31 player cars)")
    ap.add_argument("--out", metavar="DIR", help="write <CAR>.nfsv into DIR")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--render", metavar="PNG", help="write a reference composite")
    ap.add_argument("--nfsv", help="the .nfsv --render reads")
    ap.add_argument("--car", help="the car --render composites")
    ap.add_argument("--paint", default="0", help="BASE_PAINT part hash (hex)")
    ap.add_argument("--vinyl", action="append",
                    help="vinyl part hash, optionally :colour0:colour1:colour2:colour3")
    ap.add_argument("--doorline", default="", choices=DOORLINE_VARIANTS)
    args = ap.parse_args()

    if args.render:
        render(args)
        return
    if not args.out:
        ap.error("--out or --render")
    cars = args.cars or player_cars(args.pc_root)
    exe = os.path.join(args.pc_root, "SPEED2.EXE")
    swatches = scan_swatches(args.pc_root, huff.HuffDecompressor(exe))
    parts = vinyl_parts(args.pc_root)
    print("skin %dx%d, swatches %s, %d vinyl parts" % (
        swatches[0], swatches[1], [len(l) for l in swatches[2]], len(parts)))
    os.makedirs(args.out, exist_ok=True)
    jobs = [(args.pc_root, args.out, c, swatches, parts) for c in cars]
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for line in ex.map(_pack_one, jobs):
            print(line, flush=True)


if __name__ == "__main__":
    main()
