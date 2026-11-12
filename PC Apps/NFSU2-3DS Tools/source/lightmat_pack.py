"""Packs the retail car light materials for the 3DS port (TASKS.md #166).

    python tools/lightmat_pack.py "PC Files" <GameDir> [-v]

writes <GameDir>/GLOBAL/LIGHTMATS.nfsl from PC Files/GLOBAL/GLOBALB.BUN.

What they are: an eLightMaterial per surface kind -- every paint colour
(REGPAINT*, METPAINT*, PEARL*, CUSTOMPAINT_*), CHROME, CARBONFIBER, the
glasses, the window tints -- each one chunk 0x135200 of GLOBALB.BUN (186 of
them, 0xa0 bytes each). GC LoaderLights (Speed.elf 0x800c8714) takes the same
chunk id and byte-swaps it, so the PC records are the GameCube's too. The
per-vertex pass that lights a car (epCalculateSphericalHarmonicsPOS16,
0x801e1c90) and the paint's combiner (ps_Model mode 7, 0x801e6d3c) read the
values UpdatePlatInfo (0x801e2a64) derives from the 28 floats; carpaint.c
does that derivation, so the floats go through untouched.

Record, as GLOBALB.BUN has it (0xa0 bytes):
    0x0c u32 nameHash (bStringHash of the name)     0x14 char name[...]
    0x30 f32 v[28]:
        v[0]  DiffMin scale      v[1..3]   DiffMin rgb
        v[4]  DiffMax scale      v[5..7]   DiffMax rgb
        v[8]  MinA               v[9]      MaxA
        v[10] specular power
        v[11] SpecMin scale      v[12..14] SpecMin rgb
        v[15] SpecMax scale      v[16..18] SpecMax rgb
        v[19] envmap power
        v[20] K0 scale           v[21..23] K0 rgb
        v[24] K1 scale           v[25..27] K1 rgb
    (the names are from what UpdatePlatInfo does with each value)

LIGHTMATS.nfsl, little-endian:
    0x00 char magic[4] "NFSL"   0x04 u32 version = 1
    0x08 u32 count              0x0c u32 entriesOffset (0x10)
    Entry[count], sorted by nameHash, 144 bytes:
        u32 nameHash; char name[28] (NUL-padded); f32 v[28]
"""
import argparse
import os
import struct
import sys

CHUNK_LIGHT_MATERIAL = 0x135200
RECORD = 0xa0
NAME_LEN = 28
VERSION = 1


def chunks(data, off, end):
    while off + 8 <= end:
        cid, size = struct.unpack_from("<II", data, off)
        yield cid, off + 8, size
        if cid & 0x80000000:
            yield from chunks(data, off + 8, off + 8 + size)
        off += 8 + size


def read_materials(path):
    data = open(path, "rb").read()
    mats = {}
    for cid, p, size in chunks(data, 0, len(data)):
        if cid != CHUNK_LIGHT_MATERIAL:
            continue
        if size < RECORD:
            raise SystemExit("light material chunk at 0x%x is %d bytes" % (p - 8, size))
        h = struct.unpack_from("<I", data, p + 0x0c)[0]
        name = data[p + 0x14:p + 0x30].split(b"\0")[0]
        vals = struct.unpack_from("<28f", data, p + 0x30)
        if h in mats:
            raise SystemExit("light material 0x%08x twice" % h)
        mats[h] = (name, vals)
    return mats


def pack(mats):
    out = bytearray(b"NFSL" + struct.pack("<III", VERSION, len(mats), 0x10))
    for h in sorted(mats):
        name, vals = mats[h]
        out += struct.pack("<I", h) + name[:NAME_LEN - 1].ljust(NAME_LEN, b"\0")
        out += struct.pack("<28f", *vals)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pc_root")
    ap.add_argument("out", help="the game data root to write GLOBAL/ under")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    mats = read_materials(os.path.join(args.pc_root, "GLOBAL", "GLOBALB.BUN"))
    blob = pack(mats)
    dst = os.path.join(args.out, "GLOBAL", "LIGHTMATS.nfsl")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".tmp", "wb") as f:
        f.write(blob)
    os.replace(dst + ".tmp", dst)
    print("%s: %d bytes, %d light materials" % (dst, len(blob), len(mats)))
    if args.verbose:
        for h in sorted(mats, key=lambda k: mats[k][0]):
            print("  0x%08x %s" % (h, mats[h][0].decode("latin-1")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
