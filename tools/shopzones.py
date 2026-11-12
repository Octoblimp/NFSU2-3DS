# The shop trigger zones nearest a point, out of TRACKS/ROUTESL4RA/PathsFreeRoam.bin.
#
# For picking a DbgSpawn that actually lands inside a shop: the GPS point a zone
# carries (+0x34, what DbgGpsShop logs) is where the GPS leads, not inside the
# trigger polygon, so a spawn there never enters the shop. Spawn on the
# polygon's centroid instead.
#
# Same walk as trackpath_open(): chunk 0x80034147, its 0x3414A zone list, each
# record a 0x44-byte header (type, entry, ..., bbox at +0x20, handle and GPS
# point at +0x30, numPoints/stride at +0x40) and numPoints XY pairs. Type 14 is
# TPZ_SHOP_TRIGGER.
#
# Usage: python tools/shopzones.py <PathsFreeRoam.bin> <x> <y> [count]
import math
import struct
import sys

TPZ_SHOP_TRIGGER = 14


def chunks(buf, off, end):
    while off + 8 <= end:
        t, s = struct.unpack_from("<II", buf, off)
        if t == 0 and s == 0:
            return
        yield t, off + 8, s
        off += 8 + s


def main():
    path, px, py = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    d = open(path, "rb").read()
    payload = None
    for t, o, s in chunks(d, 0, len(d)):
        if t == 0x80034147:
            for st, so, ss in chunks(d, o, o + s):
                if st == 0x3414A:
                    payload = (so, ss)
            break
    if not payload:
        raise SystemExit("no 0x3414A zone chunk in %s" % path)
    o, size = payload
    p = o
    rows = []
    while p + 0x44 <= o + size:
        typ = struct.unpack_from("<I", d, p)[0]
        bb = struct.unpack_from("<4f", d, p + 0x20)
        handle, g1, g2, g3 = struct.unpack_from("<4i", d, p + 0x30)
        npts, stride = struct.unpack_from("<hh", d, p + 0x40)
        if stride <= 0:
            break
        if typ == TPZ_SHOP_TRIGGER and npts > 0:
            pts = [struct.unpack_from("<2f", d, p + 0x44 + 8 * i) for i in range(npts)]
            cx = sum(q[0] for q in pts) / npts
            cy = sum(q[1] for q in pts) / npts
            rows.append((math.hypot(cx - px, cy - py), handle & 0xFFFFFFFF, cx, cy, (g1, g2, g3), bb))
        p += stride
    rows.sort()
    for dist, h, cx, cy, g, bb in rows[:count]:
        print("handle %08X  %.0f m away  centroid %.1f,%.1f  gps %d,%d,%d  bbox %.1f,%.1f .. %.1f,%.1f"
              % (h, dist, cx, cy, g[0], g[1], g[2], bb[0], bb[1], bb[2], bb[3]))


if __name__ == "__main__":
    main()
