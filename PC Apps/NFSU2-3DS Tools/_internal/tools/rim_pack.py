#!/usr/bin/env python3
r"""Pack what retail's rim and spinner composites read, as CARS/WHEELS.nfsr
(TASKS.md #296).

An aftermarket rim's paint is not a material colour on the PC. CompositeSkin
(SPEED2.EXE 0x61D790) ends by calling CompositeRim (0x6343A0) and
CompositeSpinner (0x61E040), which blend on the CPU, through FUN_0061DE90 and
its 32-bit core FUN_00612EE0, into the DUMMY_WHEELn and DUMMY_SPINNERn
textures; FUN_00631E20's swap table then hands those to every mesh that asks
for the front wheel part's own art:

  rim      image <TEXTURE_NAME>_WHEEL, mask <TEXTURE_NAME>_WHEEL_INNER_MASK,
           colour PAINT_RIM (CarSlotId 0x4A)
  spinner  image <SPINNER_TEXTURE>, mask <SPINNER_TEXTURE>_MASK,
           colour PAINT_SPINNER (CarSlotId 0x4B)

TEXTURE_NAME (0x10C98090) and SPINNER_TEXTURE are attributes of the front
wheel part, and both values are already hashes. The images are in
CARS/WHEELS/TEXTURES.BIN (or CARS/TEXTURES.BIN); FUN_0061DE90 composites
only when image, mask and the dummy are the same size, so anything else is
left out here and the runtime leaves that rim as it is.

Per texel, m being byte 0 (the blue byte) of the mask's dword -- all
FUN_00612EE0 reads of a mask -- and K = (float)(1/255):

  m == 0   the image's texel
  else     w1 = m * K, w0 = 1 - w1; mod = FUN_00611A30(image, colour), each
           byte trunc(colour_byte * K * image_byte); then FUN_00611B00 over
           {image, mod} with {w0, w1}: per byte, the sum of trunc(byte * w)
           over the weights above K, clamped to 255. Alpha stays the image's.

The colour is 0xFFFFFFFF with the paint slot empty, else the paint's
R | G<<8 | B<<16 | GLOSS<<24 swapped to 0xAARRGGBB (FUN_005BE040).

Layout, little-endian (the texture table is tools/vinyl_pack.py's .nfsv one):

  0x00  'NFSR', u32 version (1)
  0x08  u16 wheel w, u16 wheel h          (DUMMY_WHEEL1's)
  0x0c  u16 spinner w, u16 spinner h      (DUMMY_SPINNER1's)
  0x10  u32 texture count, u32 part count
  0x18  u32 texture table, part table and data offsets
  0x24  reserved to 0x30
  textures  count x { u32 hash, u16 w, u16 h, u8 kind, u8, u16,
                      u32 data offset, u32 zlib size }    sorted by hash
            kind 0: w*h u32 0xAARRGGBB (the D3D surface's own dwords)
            kind 2: w*h u8, byte 0 of each texel (a mask)
  parts     count x { u32 part hash, u32 TEXTURE_NAME, u32 SPINNER_TEXTURE }
            sorted by part hash, 0 for an attribute the part lacks
  data      the zlib streams

Usage:
    python tools/rim_pack.py "PC Files" --out "<sd>/nfsu2data/CARS"
    python tools/rim_pack.py "PC Files" --render rim.png --nfsr WHEELS.nfsr \
        --part 0x1234abcd [--paint 0xedcd9ac6] [--spinner]
"""

import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import huff  # noqa: E402
import kit_pack as kp  # noqa: E402
import paint_pack as pp  # noqa: E402
import vinyl_pack as vp  # noqa: E402
from track_pack import bstring_hash  # noqa: E402

NFSR_MAGIC = b"NFSR"
NFSR_VERSION = 1
NFSR_HDR = 0x30
NFSR_TEX = 20
NFSR_PART = 12

KIND_ARGB, KIND_B8 = 0, 2

# The front wheel parts' record slot in GLOBALB.BUN's part table.
RIM_RECORD_SLOT = 0x1C

KEY_TEXTURE_NAME = 0x10C98090
KEY_SPINNER_TEXTURE = bstring_hash("SPINNER_TEXTURE")
DUMMY_WHEEL1 = bstring_hash("DUMMY_WHEEL1")
DUMMY_SPINNER1 = bstring_hash("DUMMY_SPINNER1")


def rim_parts(db):
    """part hash -> (TEXTURE_NAME, SPINNER_TEXTURE), 0 for one it lacks, for
    every front wheel part carrying either."""
    out = {}
    for ph, (w4, _w6, _mc) in db.records.items():
        if (w4 & 0xFF) != RIM_RECORD_SLOT:
            continue
        a = db.attrs.get(ph, {})
        tn, sp = a.get(KEY_TEXTURE_NAME, 0), a.get(KEY_SPINNER_TEXTURE, 0)
        if tn or sp:
            out[ph] = (tn, sp)
    return out


def b_plane(w, h, px):
    """Byte 0 of every A8R8G8B8 texel: all of a mask FUN_00612EE0 reads."""
    return bytes(px[0:w * h * 4:4])


def pack(pc_root):
    exe = os.path.join(pc_root, "SPEED2.EXE")
    hd = huff.HuffDecompressor(exe)
    db = kp.PartsDb(open(os.path.join(pc_root, pp.GLOBAL_BUN), "rb").read())
    parts = rim_parts(db)

    cars = os.path.join(pc_root, "CARS")
    dummies = vp.read_table(os.path.join(cars, "TEXTURES.BIN"), {DUMMY_WHEEL1, DUMMY_SPINNER1}, hd)
    for name, th in (("DUMMY_WHEEL1", DUMMY_WHEEL1), ("DUMMY_SPINNER1", DUMMY_SPINNER1)):
        if th not in dummies:
            raise SystemExit("no %s in CARS/TEXTURES.BIN" % name)
    ww, wh = dummies[DUMMY_WHEEL1][:2]
    sw, sh = dummies[DUMMY_SPINNER1][:2]

    # (image, mask, size) per composite the parts can ask for.
    pairs = set()
    for tn, sp in parts.values():
        if tn:
            pairs.add((kp.ext(tn, "_WHEEL"), kp.ext(tn, "_WHEEL_INNER_MASK"), (ww, wh)))
        if sp:
            pairs.add((sp, kp.ext(sp, "_MASK"), (sw, sh)))
    want = {h for p in pairs for h in p[:2]}
    found = vp.read_table(os.path.join(cars, "WHEELS", "TEXTURES.BIN"), want, hd)
    for th, t in vp.read_table(os.path.join(cars, "TEXTURES.BIN"), want, hd).items():
        if th in found and found[th] != t:
            raise SystemExit("0x%08x differs between the two texture packs" % th)
        found.setdefault(th, t)

    recs, skipped = {}, []
    for img, msk, size in sorted(pairs):
        ti, tm = found.get(img), found.get(msk)
        ok = (ti and tm and ti[:2] == size and tm[:2] == size and
              ti[2] == vp.D3DFMT_A8R8G8B8 and tm[2] == vp.D3DFMT_A8R8G8B8)
        if not ok:
            skipped.append(img)
            continue
        w, h = size
        recs[img] = (w, h, KIND_ARGB, zlib.compress(bytes(ti[3][:w * h * 4]), 9))
        recs[msk] = (w, h, KIND_B8, zlib.compress(b_plane(w, h, tm[3]), 9))

    o_tex = NFSR_HDR
    o_part = o_tex + len(recs) * NFSR_TEX
    o_data = o_part + len(parts) * NFSR_PART
    body, blob = bytearray(), bytearray()
    for th in sorted(recs):
        w, h, kind, z = recs[th]
        body += struct.pack("<IHHBBHII", th, w, h, kind, 0, 0, len(blob), len(z))
        blob += z
    for ph in sorted(parts):
        body += struct.pack("<III", ph, *parts[ph])
    hdr = struct.pack("<4sIHHHHIIIII", NFSR_MAGIC, NFSR_VERSION, ww, wh, sw, sh, len(recs),
                      len(parts), o_tex, o_part, o_data).ljust(NFSR_HDR, b"\0")
    rims = sum(1 for p in pairs if p[2] == (ww, wh) and p[0] in recs)
    spins = sum(1 for p in pairs if p[2] == (sw, sh) and p[0] in recs)
    info = ("%d front wheel parts, %d rim and %d spinner composites (wheel %dx%d, spinner "
            "%dx%d), %d left out" % (len(parts), rims, spins, ww, wh, sw, sh, len(skipped)))
    return hdr + bytes(body) + bytes(blob), info


# ---------------------------------------------------------------- reference

def lerp_tables():
    """FUN_00612EE0's weights per mask byte m, in single precision (the x87
    under D3D9's default FPU mode): w1 = m * K, w0 = 1 - w1, each counting
    only above K."""
    import numpy as np
    k = np.float32(1.0) / np.float32(255.0)
    w1 = np.arange(256, dtype=np.float32) * k
    w0 = np.float32(1.0) - w1
    return w0, w1, w0 > k, w1 > k


def composite(img, mask, colour):
    """FUN_00612EE0 over numpy: `img` u32 0xAARRGGBB, `mask` u8, `colour`
    0xAARRGGBB."""
    import numpy as np
    T, _live = vp.remap_table()
    w0, w1, l0, l1 = lerp_tables()
    m = mask.astype(np.int32)
    out = img.copy()
    for shift in (0, 8, 16):
        c = ((img >> shift) & 0xFF).astype(np.int32)
        mod = T[(colour >> shift) & 0xFF, c]
        a = np.where(l0[m], (c.astype(np.float32) * w0[m]).astype(np.int32), 0)
        b = np.where(l1[m], (mod.astype(np.float32) * w1[m]).astype(np.int32), 0)
        v = np.minimum(a + b, 255).astype(np.uint32)
        out = (out & ~np.uint32(0xFF << shift)) | (v << shift)
    return np.where(m == 0, img, out)


def render(args):
    import numpy as np
    from PIL import Image
    d = open(args.nfsr, "rb").read()
    (_m, _v, _ww, _wh, _sw, _sh, ntex, npart, o_tex, o_part,
     o_data) = struct.unpack_from("<4sIHHHHIIIII", d, 0)
    tex = {}
    for i in range(ntex):
        th, w, h, kind, _a, _b, off, zs = struct.unpack_from("<IHHBBHII", d, o_tex + i * NFSR_TEX)
        tex[th] = (w, h, kind, zlib.decompress(d[o_data + off:o_data + off + zs]))
    parts = {}
    for i in range(npart):
        ph, tn, sp = struct.unpack_from("<III", d, o_part + i * NFSR_PART)
        parts[ph] = (tn, sp)
    tn, sp = parts[int(args.part, 16)]
    if args.spinner:
        img_h, msk_h = sp, kp.ext(sp, "_MASK")
    else:
        img_h, msk_h = kp.ext(tn, "_WHEEL"), kp.ext(tn, "_WHEEL_INNER_MASK")
    w, h, _k, raw = tex[img_h]
    img = np.frombuffer(raw, "<u4").copy()
    mask = np.frombuffer(tex[msk_h][3], np.uint8)
    colour = 0xFFFFFFFF
    if args.paint:
        data = open(os.path.join(args.pc_root, pp.GLOBAL_BUN), "rb").read()
        colour = vp.argb_of(pp.paint_table(pp.read_parts(data)), int(args.paint, 16), None)
        if colour is None:
            raise SystemExit("%s is not a colour part" % args.paint)
    a = composite(img, mask, colour).reshape(h, w)
    rgba = np.stack([(a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, a >> 24], axis=-1)
    Image.fromarray(rgba.astype(np.uint8), "RGBA").save(args.render)
    print("wrote %s" % args.render)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the PC Files directory")
    ap.add_argument("--out", metavar="DIR", help="write WHEELS.nfsr into DIR")
    ap.add_argument("--render", metavar="PNG", help="write a reference composite")
    ap.add_argument("--nfsr", help="the WHEELS.nfsr --render reads")
    ap.add_argument("--part", help="the front wheel part hash (hex) --render composites")
    ap.add_argument("--paint", help="the rim or spinner paint part hash (hex); none is white")
    ap.add_argument("--spinner", action="store_true", help="the spinner, not the rim")
    args = ap.parse_args()

    if args.render:
        render(args)
        return
    if not args.out:
        ap.error("--out or --render")
    blob, info = pack(args.pc_root)
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "WHEELS.nfsr")
    tmp = path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(blob)
    os.replace(tmp, path)
    print("%s: %s, %.1f MB" % (path, info, len(blob) / 1e6))


if __name__ == "__main__":
    main()
