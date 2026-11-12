#!/usr/bin/env python3
"""Reads the GameCube disc's NFSUNDER/ZDIR.BIN index and pulls files out of
the ZZDATA0..3.BIN archives beside it (#467: the GC button art).

ZDIR.BIN is 24-byte little-endian entries sorted by name hash:
    u32 hash      bStringHash of the path, upper case, backslashes
    u32 archive   which ZZDATA<n>.BIN
    u32 sector    2048-byte sector in that archive
    u32 global    the same sector counted across all four archives
    u32 size      bytes
    u32 check     a checksum, not read
Names come from PC Files/filelist.txt (the PC's own list), hashed the same way.

    python tools/gc_zdir.py --list
    python tools/gc_zdir.py --get FRONTEND\\FRONTB.LZC --out out/gc
"""
import argparse
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GC = ROOT / "Gamecube Files" / "NFSUNDER"


def bhash(s):
    h = 0xFFFFFFFF
    for c in s.encode("ascii"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def entries():
    d = (GC / "ZDIR.BIN").read_bytes()
    out = {}
    for i in range(0, len(d), 24):
        h, arc, sec, glob, size, chk = struct.unpack_from("<6I", d, i)
        out[h] = (arc, sec, size)
    return out


def names():
    out = {}
    for line in (ROOT / "PC Files" / "filelist.txt").read_text(errors="replace").splitlines():
        parts = line.split(",", 2)
        if len(parts) == 3:
            n = parts[2].strip().upper()
            out[bhash(n)] = n
    return out


def read(ent):
    arc, sec, size = ent
    with open(GC / ("ZZDATA%d.BIN" % arc), "rb") as f:
        f.seek(sec * 2048)
        return f.read(size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--get", action="append", default=[])
    ap.add_argument("--out", default=str(ROOT / "out" / "gc"))
    a = ap.parse_args()
    ents, nm = entries(), names()
    if a.list:
        known = 0
        for h, e in sorted(ents.items(), key=lambda kv: nm.get(kv[0], "~")):
            n = nm.get(h)
            known += n is not None
            print("%08x  zz%d  %9d  %s" % (h, e[0], e[2], n or "?"))
        print("%d entries, %d named" % (len(ents), known))
    for g in a.get:
        h = bhash(g.upper())
        if h not in ents:
            print("not on the disc:", g)
            continue
        out = Path(a.out) / g.replace("\\", "/")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(read(ents[h]))
        print("%s -> %s (%d bytes)" % (g, out, ents[h][2]))


if __name__ == "__main__":
    main()
