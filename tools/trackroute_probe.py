#!/usr/bin/env python3
"""Every posted road near a point, straight out of Routes<track><F|B>.bin.

tools/ailine_dump.py prints the road the matcher *chose*. When that choice
looks wrong there is no way to tell from its output whether a better road was
there and lost, or whether no better road exists -- so this reads the file
itself and lists all of them, with the fields the match is scored on.

    python tools/trackroute_probe.py <track> --at X Y [--reach 60] [--reverse]
    python tools/trackroute_probe.py <track> --widths

The layout is the one 3DS Files/source/trackroute.h documents, read off
Speed.elf: a flat {u32 type, u32 size} chunk list, chunk 0x34121 holding the
routes back to back, each 0x8C of header (s16 point count at +0x34) then that
many 0x38-byte points.
"""

import argparse
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(ROOT, "PC Files", "TRACKS", "ROUTESL4RA")

CHUNK_ROUTES = 0x34121
ROUTE_HDR = 0x8C
POINT_BYTES = 0x38


def read_points(path):
    """[(route, x, y, z, halfL, halfR, off0, off1, postF, postB)], file order."""
    with open(path, "rb") as f:
        buf = f.read()
    pts = []
    off, nr, size = 0, 0, len(buf)
    while off + 8 <= size:
        ctype, csz = struct.unpack_from("<II", buf, off)
        if csz > size - (off + 8):
            break
        if ctype == CHUNK_ROUTES:
            r, end = off + 8, off + 8 + csz
            while r + ROUTE_HDR <= end:
                n = struct.unpack_from("<h", buf, r + 0x34)[0]
                if n < 0 or r + ROUTE_HDR + n * POINT_BYTES > end:
                    break
                for k in range(n):
                    p = r + ROUTE_HDR + k * POINT_BYTES
                    x, y = struct.unpack_from("<ff", buf, p)
                    bits = struct.unpack_from("<I", buf, p + 0x0C)[0]
                    z = struct.unpack_from("<h", buf, p + 0x12)[0] * (8192.0 / 65536.0)
                    hl = struct.unpack_from("<H", buf, p + 0x14)[0] / 256.0
                    hr = struct.unpack_from("<H", buf, p + 0x16)[0] / 256.0
                    w = max(hl, hr)
                    o0 = struct.unpack_from("<b", buf, p + 0x22)[0] / 128.0 * w
                    o1 = struct.unpack_from("<b", buf, p + 0x23)[0] / 128.0 * w
                    pts.append((nr, x, y, z, hl, hr, o0, o1,
                                (bits >> 11) & 1, (bits >> 18) & 1))
                r += ROUTE_HDR + n * POINT_BYTES
                nr += 1
        off += 8 + csz
    return pts, nr


def near(pts, px, py, reach):
    """Segments whose closest point is within `reach` of (px, py)."""
    out = []
    for i in range(len(pts) - 1):
        a, e = pts[i], pts[i + 1]
        if a[0] != e[0]:
            continue
        ex, ey = e[1] - a[1], e[2] - a[2]
        l2 = ex * ex + ey * ey
        if l2 < 1e-6:
            continue
        u = ((px - a[1]) * ex + (py - a[2]) * ey) / l2
        u = max(0.0, min(1.0, u))
        qx, qy = a[1] + ex * u, a[2] + ey * u
        d = math.hypot(qx - px, qy - py)
        if d <= reach:
            out.append((d, i, u, qx, qy, math.sqrt(l2)))
    out.sort()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("track", type=int)
    ap.add_argument("--reverse", action="store_true")
    ap.add_argument("--at", nargs=2, type=float, metavar=("X", "Y"))
    ap.add_argument("--dir", nargs=2, type=float, metavar=("DX", "DY"),
                    help="the race's heading there, to report each road's "
                         "alignment and which way a racer would take it")
    ap.add_argument("--reach", type=float, default=60.0)
    ap.add_argument("--widths", action="store_true",
                    help="the distribution of road widths and line offsets")
    args = ap.parse_args()

    path = os.path.join(DATA, "Routes%d%s.bin" % (args.track, "B" if args.reverse else "F"))
    if not os.path.exists(path):
        sys.exit("no %s" % os.path.basename(path))
    pts, nr = read_points(path)
    print("%s: %d route(s), %d point(s)" % (os.path.basename(path), nr, len(pts)))

    if args.widths:
        ws = sorted(p[4] + p[5] for p in pts)
        fr = sorted(abs(p[6]) / max(max(p[4], p[5]), 1e-6) for p in pts)
        def pct(v, q):
            return v[min(len(v) - 1, int(q * len(v)))]
        print("road width (m)      p10 %5.1f  median %5.1f  p90 %5.1f  max %5.1f"
              % (pct(ws, .1), pct(ws, .5), pct(ws, .9), ws[-1]))
        print("|line 0| / halfwid  p10 %5.2f  median %5.2f  p90 %5.2f  max %5.2f"
              % (pct(fr, .1), pct(fr, .5), pct(fr, .9), fr[-1]))
        print("roads wider than 18 m: %d of %d point(s)"
              % (sum(1 for w in ws if w > 18.0), len(ws)))
        return

    if not args.at:
        sys.exit("--at X Y or --widths")
    px, py = args.at
    dx, dy = args.dir if args.dir else (0.0, 0.0)
    hdr = ("  dist  route  seg      lat    halfL    halfR     out   line0   line1"
           "  postF postB")
    if args.dir:
        hdr += "   align  drives  lineLat"
    print(hdr)
    for d, i, u, qx, qy, seglen in near(pts, px, py, args.reach)[:24]:
        a, e = pts[i], pts[i + 1]
        ex, ey = e[1] - a[1], e[2] - a[2]
        tx, ty = ex / seglen, ey / seglen
        # lateral of the query point across the segment, right of its direction

        lat = (px - a[1]) * ty - (py - a[2]) * tx
        hl = a[4] + (e[4] - a[4]) * u
        hr = a[5] + (e[5] - a[5]) * u
        out = lat - hr if lat > hr else (-hl - lat if lat < -hl else 0.0)
        o0 = a[6] + (e[6] - a[6]) * u
        o1 = a[7] + (e[7] - a[7]) * u
        row = ("%6.1f  %5d %4d  %7.1f  %7.1f  %7.1f %7.1f %7.1f %7.1f      %d     %d"
               % (d, a[0], i, lat, hl, hr, out, o0, o1, a[8], a[9]))
        if args.dir:
            al = tx * dx + ty * dy
            up = al < 0.0
            # where that road's racing line lands as a lateral across the
            # race's own heading, the way ailine_build works it out
            nx, ny = dy, -dx
            rx, ry = ty, -tx
            off = -o1 if up else o0
            q = (qx - px) * nx + (qy - py) * ny
            row += "  %6.2f  %-6s %7.1f" % (al, "up" if up else "down",
                                            q + (rx * nx + ry * ny) * off)
        print(row)


if __name__ == "__main__":
    main()
