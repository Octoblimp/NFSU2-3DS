#!/usr/bin/env python3
"""
CARS/<car>/GEOMETRY.BIN collision volumes  ->  GLOBAL/CARHULLS.nfsg for the 3DS.

Each car's GEOMETRY.BIN carries its collision hulls in a 0x80034020 container:
one 0x39200 header per hull, each followed by a 0x39201 vertex chunk (0x30
bytes a vertex, the position at +0x10) and a 0x39202 adjacency chunk.
LoaderCollisionVolumes (GC 0x801B8BFC) links every hull into one list, and the
CarCollisionBody constructor (0x801AFAD4) takes the car's hull by name hash --
RideInfo::GetCollisionVolumeNameHash, which is bStringHash("<CAR>_CV") unless a
fitted body kit's part names its own ("CV" attribute) -- falling back to
GetDefaultCollisionVolumeNameHash, the plain "<CAR>_CV".

This packs the default hull of every car that has one, model space as authored
(+X front, +Y left, +Z up). carsim.c takes the centre of gravity off it, as
MoveCenterOfGravity does. The kit hulls are not packed yet (nothing on the 3DS
reads a kit's CV attribute).

Output layout (little-endian):

    0x00  char  magic[4]   "NFSG"
    0x04  u32   version    = 1
    0x08  u32   count
    0x0c  u32   0
    then count entries: { u32 hash; u32 numVerts; f32 pos[numVerts][3]; }

Usage: python tools/hull_pack.py [PC Files/CARS] [out.nfsg]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CARS = os.path.join(os.environ.get("NFSU2_PC_ROOT") or os.path.join(ROOT, "PC Files"), "CARS")
OUT = os.path.join(os.environ.get("APPDATA", ""), "Azahar", "sdmc", "nfsu2data", "GLOBAL",
                   "CARHULLS.nfsg")

CONTAINER = 0x80034020
HULL = 0x39200
VERT_SIZE = 0x30
VERT_POS = 0x10
MAX_VERTS = 64   # carsim.h CARSIM_HULL_MAX


def string_hash(s, h=0xFFFFFFFF):
    """bStringHash, and its carry-on form bStringHash(s, h)."""
    for c in s.encode("ascii"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def hulls(data):
    """Every hull in a GEOMETRY.BIN, as (hash, [(x, y, z), ...])."""
    out = []
    at = data.find(struct.pack("<I", CONTAINER))
    while at >= 0:
        size = struct.unpack_from("<I", data, at + 4)[0]
        end = at + 8 + size
        p = at + 8
        while p + 8 <= end:
            cid, csz = struct.unpack_from("<II", data, p)
            if cid != HULL:
                p += 8 + csz
                continue
            # The loader's own alignment rules: the header and each array
            # start 16-aligned past their chunk's 8-byte header.
            hdr = (p + 0x17) & ~0xF
            name = struct.unpack_from("<I", data, hdr + 0x18)[0]
            v = p + 8 + csz
            vsz = struct.unpack_from("<I", data, v + 4)[0]
            va = (v + 8 + 0xF) & ~0xF
            n = (vsz - (va - (v + 8))) // VERT_SIZE
            verts = [struct.unpack_from("<3f", data, va + k * VERT_SIZE + VERT_POS)
                     for k in range(n)]
            out.append((name, verts))
            adj = v + 8 + vsz
            p = adj + 8 + struct.unpack_from("<I", data, adj + 4)[0]
        at = data.find(struct.pack("<I", CONTAINER), end)
    return out


def main():
    cars = sys.argv[1] if len(sys.argv) > 1 else CARS
    out = sys.argv[2] if len(sys.argv) > 2 else OUT
    entries = []
    for car in sorted(os.listdir(cars)):
        path = os.path.join(cars, car, "GEOMETRY.BIN")
        if not os.path.isfile(path):
            continue
        want = string_hash("_CV", string_hash(car))
        found = [v for h, v in hulls(open(path, "rb").read()) if h == want]
        if not found:
            continue
        verts = found[0]
        if not 4 <= len(verts) <= MAX_VERTS:
            print("%s: %d vertices, skipped" % (car, len(verts)))
            continue
        entries.append((want, verts))
        zs = [q[2] for q in verts]
        print("%-12s %08x %2d vertices, z %+.3f..%+.3f" % (car, want, len(verts), min(zs),
                                                          max(zs)))
    blob = struct.pack("<4sIII", b"NFSG", 1, len(entries), 0)
    for h, verts in entries:
        blob += struct.pack("<II", h, len(verts))
        for q in verts:
            blob += struct.pack("<3f", *q)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out + ".new", "wb") as f:
        f.write(blob)
    os.replace(out + ".new", out)
    print("%d hulls, %d bytes -> %s" % (len(entries), len(blob), out))


if __name__ == "__main__":
    main()
