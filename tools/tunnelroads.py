# DbgSpawn points that drive into a tunnel or under an overpass, for testing
# SFXCTL_Tunnel and CARSFX_DriveBy (carsfx.c, TASKS.md #298).
#
# For each TrackPathZone of type 5 / 6 / 7 (TPZ_TUNNEL, TPZ_OVERPASS,
# TPZ_OVERPASS_SMALL) in PathsFreeRoam.bin, finds the free-roam routes
# (RoutesFreeRoam.bin, trackroute.c's layout) that run into it below its entry
# Z, walks each one back <back> metres from the mouth, and prints that point as
# a DbgSpawn (z one metre up, yaw along the road in DbgSpawn's frame: 0 = +X,
# counter-clockwise), with how far the road runs on inside the zone and its
# sharpest kink on the way in. Hold A from a low-kink one and the car drives
# straight in. The zone walk is shopzones.py's.
#
# Usage: python tools/tunnelroads.py "PC Files/TRACKS/ROUTESL4RA" [back]
import math
import struct
import sys

ROOT = sys.argv[1]
back = float(sys.argv[2]) if len(sys.argv) > 2 else 70.0
NAMES = {5: "TUNNEL", 6: "OVERPASS", 7: "OVERPASS_SMALL"}


def chunks(buf, off, end):
    while off + 8 <= end:
        t, s = struct.unpack_from("<II", buf, off)
        if t == 0 and s == 0:
            return
        yield t, off + 8, s
        off += 8 + s


def zones():
    d = open(ROOT + "/PathsFreeRoam.bin", "rb").read()
    for t, o, s in chunks(d, 0, len(d)):
        if t == 0x80034147:
            for st, so, ss in chunks(d, o, o + s):
                if st == 0x3414A:
                    o, size = so, ss
            break
    p = o
    out = []
    while p + 0x44 <= o + size:
        typ = struct.unpack_from("<I", d, p)[0]
        ez = struct.unpack_from("<f", d, p + 0x14)[0]
        npts, stride = struct.unpack_from("<hh", d, p + 0x40)
        if stride <= 0:
            break
        if typ in NAMES and npts > 0:
            poly = [struct.unpack_from("<2f", d, p + 0x44 + 8 * i) for i in range(npts)]
            out.append((typ, ez, poly))
        p += stride
    return out


def routes():
    d = open(ROOT + "/RoutesFreeRoam.bin", "rb").read()
    rs = []
    off = 0
    while off + 8 <= len(d):
        t, csz = struct.unpack_from("<II", d, off)
        if csz > len(d) - (off + 8):
            break
        if t == 0x34121:
            r = off + 8
            end = r + csz
            while r + 0x8C <= end:
                n = struct.unpack_from("<h", d, r + 0x34)[0]
                if n < 0 or n > 0x400 or r + 0x8C + n * 0x38 > end:
                    break
                pts = []
                for k in range(n):
                    p = r + 0x8C + k * 0x38
                    x, y = struct.unpack_from("<2f", d, p)
                    z = struct.unpack_from("<h", d, p + 0x12)[0] * (8192.0 / 65536.0)
                    fx, fy = struct.unpack_from("<2h", d, p + 0x18)
                    pts.append((x, y, z, fx / 32767.0, fy / 32767.0))
                rs.append(pts)
                r += 0x8C + n * 0x38
        off += 8 + csz
    return rs


def inside(poly, x, y):
    c = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c


zs = zones()
rs = routes()
print("%d zones, %d routes, %d points" % (len(zs), len(rs), sum(len(r) for r in rs)))
seen = set()
for zi, (typ, ez, poly) in enumerate(zs):
    for ri, pts in enumerate(rs):
        for k, (x, y, z, fx, fy) in enumerate(pts):
            if not inside(poly, x, y) or not (ez == 0 or ez >= z):
                continue
            if k == 0 or inside(poly, pts[k - 1][0], pts[k - 1][1]):
                continue
            # The route enters the zone at point k: walk back `back` m.
            dist, j, turn = 0.0, k, 0.0
            while j > 0 and dist < back:
                a, b = pts[j - 1], pts[j]
                dist += math.hypot(b[0] - a[0], b[1] - a[1])
                turn = max(turn, abs(math.degrees(math.atan2(a[3] * b[4] - a[4] * b[3], a[3] * b[3] + a[4] * b[4]))))
                j -= 1
            if dist < back * 0.8 or (zi, ri) in seen:
                continue
            seen.add((zi, ri))
            s = pts[j]
            yaw = math.degrees(math.atan2(s[4], s[3])) % 360
            # How far the road runs on inside the zone.
            m, inlen = k, 0.0
            while m + 1 < len(pts) and inside(poly, pts[m + 1][0], pts[m + 1][1]):
                inlen += math.hypot(pts[m + 1][0] - pts[m][0], pts[m + 1][1] - pts[m][1])
                m += 1
            print("%-14s ez %5.1f  route %3d pt %3d: z %.1f at the mouth, %.0f m inside; "
                  "spawn %.1f,%.1f,%.1f yaw %.0f (%.0f m back, max kink %.0f deg)"
                  % (NAMES[typ], ez, ri, k, z, inlen, s[0], s[1], s[2] + 1.0, yaw, dist, turn))
