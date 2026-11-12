#!/usr/bin/env python3
"""
Car part animations: the hood, trunk and door swings the car customisation
shops play (TASKS.md #372 step 7), out of each car's
CARS/<CAR>/PARTS_ANIMATIONS.bin into a flat CARS/<CAR>.nfsa that
3DS Files/source/carpartanim.c reads.

The retail file (LoaderCarPartsAnimHeader, SpeedPS.elf 0x800521e4) is a run of
0x00E34010 chunks, one EAGL animation bank each (a little-endian ELF with
.data / .rel.data after 8 bytes of padding), and one 0x00037100 chunk:
16-aligned data whose +4 is the car's bStringHash and whose +0x10 holds 21
bMatrix4s, the markers GetMarkerPosition hands out, in the entity order below.
Each bank holds one entity, <CAR>_ZAM_<NAME>, with a _q channel (a rotation
quaternion x, y, z, w per key) and a _t channel (a translation per key, or a
constant zero, in which case GetCarPartAnimMatrix (0x800537e4) takes the
marker's translation instead). A channel's header:
    +0x00 1, 0x10001, 0x3030301e (byte 0: 30 keys a second), 0x14000000
    +0x10 kind (2 keyed, 1 constant), components (4 q, 3 t, 0 zero),
          numKeys, hasKeys, 0, 1, 0
    +0x2c the keys, components floats each.
The entity descriptor beside it: { 0x1400000f, header, numFrames << 16 | 1,
header + 0xc }; the bank's own record holds the descriptors' and names'
pointer arrays.

<CAR>.nfsa (little-endian):
    0x00 char magic[4] "NFSA"     0x04 u32 version = 1
    0x08 u32 carHash              0x0c u32 numEntities = 21
    0x10 f32 marker[21][16]       row-major bMatrix4s, row vectors, as retail
    0x550 Entity[21] { u16 numQ; u16 numT; u32 qOffset; u32 tOffset; f32 fps; }
          numQ 0 = the car has no such entity; numT 0 = the constant zero
          translation (take the marker's); offsets from the file's start
    then the keys: numQ * 4 floats, numT * 3 floats per entity.

A second channel layout turns up (ESCALADE's TRUNK_REG): descriptor word
0x1414000f, header word +0xc 0x14140000, kind 3 for keyed and 2 for the
constant, and the keys 8 bytes later, at +0x34.

Usage: python tools/carpartanim_pack.py <pc_root> <out_root> [--cars TT ...]
"""
import argparse
import os
import struct
import sys

ENTITIES = (
    "HOOD_REG", "HOOD_FRONT", "LEFT_HOOD_SPLIT", "RIGHT_HOOD_SPLIT",
    "FRONT_HOOD_SPLIT", "BACK_HOOD_SPLIT",
    "LEFT_DOOR_REG", "LEFT_DOOR_SCISSOR", "LEFT_DOOR_SUICIDE",
    "RIGHT_DOOR_REG", "RIGHT_DOOR_SCISSOR", "RIGHT_DOOR_SUICIDE",
    "TRUNK_REG",
    "LEFT_DOOR_WIDE1", "LEFT_DOOR_WIDE2", "LEFT_DOOR_WIDE3", "LEFT_DOOR_WIDE4",
    "RIGHT_DOOR_WIDE1", "RIGHT_DOOR_WIDE2", "RIGHT_DOOR_WIDE3", "RIGHT_DOOR_WIDE4",
)
CH_BANK = 0x00E34010
CH_MARKERS = 0x00037100

DEFAULT_CARS = ("240SX", "3000GT", "350Z", "A3", "CELICA", "CIVIC", "COROLLA", "CORSA",
                "ECLIPSE", "ESCALADE", "FOCUS", "G35", "GOLF", "GTO", "HUMMER",
                "IMPREZAWRX", "IS300", "LANCEREVO8", "MIATA", "MUSTANGGT", "NAVIGATOR",
                "PEUGOT", "PEUGOT106", "RSX", "RX7", "RX8", "SENTRA", "SKYLINE", "SUPRA",
                "TIBURON", "TT")


def bstringhash(s, h=0xFFFFFFFF):
    for c in s.encode("latin1"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def chunks(d):
    off = 0
    while off + 8 <= len(d):
        t, n = struct.unpack_from("<II", d, off)
        yield t, d[off + 8:off + 8 + n]
        off += 8 + n


def cstr(b, o):
    return b[o:b.index(b"\0", o)].decode("latin1")


def read_bank(body):
    """{name: (fps, comps, [keys])} for every channel of one bank chunk."""
    b = body[8:]
    if b[:4] != b"\x7fELF":
        raise ValueError("no ELF in bank chunk")
    shoff, = struct.unpack_from("<I", b, 0x20)
    shnum, = struct.unpack_from("<H", b, 0x30)
    secs = [struct.unpack_from("<10I", b, shoff + i * 40) for i in range(shnum)]
    shstr = secs[struct.unpack_from("<H", b, 0x32)[0]]
    byname = {cstr(b, shstr[4] + s[0]): s for s in secs}
    data = byname[".data"]
    D = b[data[4]:data[4] + data[5]]
    w = lambda o: struct.unpack_from("<I", D, o)[0]
    # The descriptors' pointer array: the first run of pointers to
    # 0x1400000f descriptor words, the names' pointer array right after it
    # (the bank record points at both; its count is the run's length).
    # (0x1400000f, or 0x1414000f on the second channel layout below.)
    isdesc = lambda p: p % 4 == 0 and p + 4 <= len(D) and (w(p) & 0xFF00FFFF) == 0x1400000F
    descs = None
    for a in range(0, len(D) - 4, 4):
        if isdesc(w(a)):
            descs = a
            break
    if descs is None:
        raise ValueError("no channel descriptors")
    n = 0
    while isdesc(w(descs + n * 4)):
        n += 1
    names = descs + n * 4
    out = {}
    for i in range(n):
        name = cstr(D, w(names + i * 4))
        desc = w(descs + i * 4)
        hdr = w(desc + 4)
        fps = D[hdr + 8]
        kind, comps, nkeys = w(hdr + 0x10), w(hdr + 0x14), w(hdr + 0x18)
        # Two layouts: header word +0xc 0x14000000 (kind 2 keyed, keys at
        # +0x2c) and 0x14140000 (ESCALADE's trunk: kind 3 keyed, kind 2 the
        # constant, keys at +0x34).
        wide = w(hdr + 0xc) == 0x14140000
        keyed, first = (3, 0x34) if wide else (2, 0x2c)
        keys = []
        if kind == keyed and comps:
            for k in range(nkeys):
                keys.append(struct.unpack_from("<%df" % comps, D, hdr + first + k * comps * 4))
        out[name] = (fps, comps, keys)
    return out


def pack_car(path, car):
    d = open(path, "rb").read()
    ents = {}
    markers = None
    car_hash = None
    for t, body in chunks(d):
        if t == CH_BANK:
            for name, ch in read_bank(body).items():
                ents[name] = ch
        elif t == CH_MARKERS:
            # 0x11 padding up to the 16-byte boundary in the file, then the
            # record.
            o = 0
            while o < len(body) and body[o] == 0x11:
                o += 1
            car_hash, = struct.unpack_from("<I", body, o + 4)
            markers = [struct.unpack_from("<16f", body, o + 0x10 + i * 0x40)
                       for i in range(len(ENTITIES))]
    if markers is None:
        raise ValueError("%s: no 0x37100 marker chunk" % path)
    head = bytearray(b"NFSA" + struct.pack("<III", 1, car_hash, len(ENTITIES)))
    for m in markers:
        head += struct.pack("<16f", *m)
    table_off = len(head)
    keys = bytearray()
    base = table_off + len(ENTITIES) * 16
    table = bytearray()
    found = 0
    for e in ENTITIES:
        q = ents.get("%s_ZAM_%s_q" % (car, e))
        t = ents.get("%s_ZAM_%s_t" % (car, e))
        if not q or not q[2]:
            table += struct.pack("<HHIIf", 0, 0, 0, 0, 30.0)
            continue
        found += 1
        qoff = base + len(keys)
        for k in q[2]:
            keys += struct.pack("<4f", *k)
        toff, nt = 0, 0
        if t and t[1] == 3 and t[2]:
            toff, nt = base + len(keys), len(t[2])
            for k in t[2]:
                keys += struct.pack("<3f", *k)
        table += struct.pack("<HHIIf", len(q[2]), nt, qoff, toff, float(q[0] or 30))
    return bytes(head + table + keys), found, car_hash


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the retail PC tree (holds CARS/)")
    ap.add_argument("out", help="the game data root to write CARS/<CAR>.nfsa under")
    ap.add_argument("--cars", nargs="*", default=list(DEFAULT_CARS))
    ap.add_argument("--dump", action="store_true", help="print each entity's keys too")
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out, "CARS"), exist_ok=True)
    for car in a.cars:
        src = os.path.join(a.pc_root, "CARS", car, "PARTS_ANIMATIONS.bin")
        if not os.path.exists(src):
            print("%s: no PARTS_ANIMATIONS.bin -- skipped" % car)
            continue
        blob, found, h = pack_car(src, car)
        if h != bstringhash(car):
            print("%s: marker chunk car hash 0x%08x is not bStringHash(%s) 0x%08x"
                  % (car, h, car, bstringhash(car)))
        dst = os.path.join(a.out, "CARS", car + ".nfsa")
        tmp = dst + ".tmp"
        open(tmp, "wb").write(blob)
        os.replace(tmp, dst)
        print("%s: %d of %d entities, %d bytes" % (car, found, len(ENTITIES), len(blob)))
        if a.dump:
            for i, e in enumerate(ENTITIES):
                nq, nt, qo, to, fps = struct.unpack_from("<HHIIf", blob, 0x550 + i * 16)
                m = struct.unpack_from("<16f", blob, 0x10 + i * 64)
                q0 = struct.unpack_from("<4f", blob, qo) if nq else ()
                q1 = struct.unpack_from("<4f", blob, qo + (nq - 1) * 16) if nq else ()
                t1 = struct.unpack_from("<3f", blob, to + (nt - 1) * 12) if nt else ()
                print("  %-20s q%3d t%3d fps %g  marker rot %s t %s" % (
                    e, nq, nt, fps, " ".join("%.2f" % v for v in m[:3] + m[4:7] + m[8:11]),
                    " ".join("%.3f" % v for v in m[12:15])))
                print("      q0 %s  qN %s  tN %s" % (
                    " ".join("%.3f" % v for v in q0), " ".join("%.3f" % v for v in q1),
                    " ".join("%.3f" % v for v in t1)))


if __name__ == "__main__":
    main()
