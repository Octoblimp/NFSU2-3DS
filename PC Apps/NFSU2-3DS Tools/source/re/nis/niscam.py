"""Dump the NIS camera groups (chunk 0x8003B201) of a PC bundle.

Layout (LoadCameraSet 0x80069660, PlatEndianSwap__15DragCameraTrack/14DragCameraData):
  group sub-chunk: +0 id, +4 size, +8 u32 scene hash, +0xC u32 track count, then tracks
  track: +0xC float length, +0x10 s8 key count, +0x12 name[14], +0x20 keys (0x88 each);
         stored size 0x20 + n*0x88
  key:   +0 smooth (cubic tangents), +1 blend (no hard cut), +2 eye/fov cubic, +3 look cubic,
         +4 eye space, +5 look space (0 Car, 1 World, 2 Hybrid, 3 Scene),
         +0xC start parameter (0..1 of the scene), +0x10[2] tangent scale,
         +0x18 eye[2], +0x30 look[2], +0x48 lens[2], +0x50 near[2], +0x58 noise amp[2],
         +0x60 noise freq[2], +0x68 focal[2], +0x70.. bytes (aperture, letterbox, fx...)
"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from niself import chunks

SPACE = ["Car", "World", "Hybrid", "Scene"]


def hash_(s):
    h = 0xFFFFFFFF
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def groups(b):
    out = []
    chunks(b, 0, len(b), 0, out)
    res = []
    for dp, cid, off, size in out:
        if cid != 0x8003B201:
            continue
        p = off + 8
        end = p + size
        while p + 8 <= end:
            sid, ssz = struct.unpack_from("<II", b, p)
            h, n = struct.unpack_from("<II", b, p + 8)
            t = p + 0x10
            tracks = []
            for i in range(n):
                ln, = struct.unpack_from("<f", b, t + 0xC)
                cnt = struct.unpack_from("<b", b, t + 0x10)[0]
                name = b[t + 0x12:t + 0x20].split(b'\0')[0].decode(errors='replace')
                keys = [b[t + 0x20 + k * 0x88: t + 0x20 + (k + 1) * 0x88] for k in range(cnt)]
                tracks.append((name, ln, keys))
                t += 0x20 + cnt * 0x88
            res.append((h, tracks))
            p += 8 + ssz
    return res


def show_key(k):
    f = lambda o, n=1: struct.unpack_from("<%df" % n, k, o)
    print("      flags %s space eye %s look %s  t=%.4f tan=%.2f,%.2f" % (
        k[:4].hex(), SPACE[k[4]] if k[4] < 4 else k[4], SPACE[k[5]] if k[5] < 4 else k[5],
        f(0xC)[0], *f(0x10, 2)))
    print("        eye %s -> %s" % (["%.2f" % v for v in f(0x18, 3)], ["%.2f" % v for v in f(0x24, 3)]))
    print("        look %s -> %s" % (["%.2f" % v for v in f(0x30, 3)], ["%.2f" % v for v in f(0x3C, 3)]))
    print("        lens %.1f->%.1f near %.2f->%.2f amp %.2f->%.2f freq %.2f->%.2f focal %.2f->%.2f" % (
        *f(0x48, 2), *f(0x50, 2), *f(0x58, 2), *f(0x60, 2), *f(0x68, 2)))
    print("        tail %s" % k[0x70:].hex())


if __name__ == "__main__":
    b = open(sys.argv[1], "rb").read()
    want = sys.argv[2:] or None
    names = {hash_(n): n for n in (want or [])}
    for h, tracks in groups(b):
        if want and h not in names:
            continue
        print("group 0x%08X %s: %d tracks" % (h, names.get(h, ""), len(tracks)))
        for name, ln, keys in tracks:
            print("    track '%s' len %.3f keys %d" % (name, ln, len(keys)))
            if want:
                for k in keys:
                    show_key(k)
