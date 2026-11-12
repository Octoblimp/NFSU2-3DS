"""Does a race's barrier ring actually close around its route? (TASKS.md #247)

A race turns on two topology groups -- BARRIERS_<track> and
PLAYER_BARRIERS_<track> -- and those are the only walls between the route and
the rest of the city (topo_groups.py, topology.c's topo_set_groups). If a car
can drive off the route and end up somewhere the race never meant it to be,
either the authored ring has an opening there or the port is not making one of
its walls solid. This tells the two apart.

It does three things, all against the files the 3DS reads:

  * --cross X0,Y0,X1,Y1 -- every wall whose XY segment crosses that line, with
    the group it belongs to and whether the race has that group on. This is
    the direct question for a known excursion: the car went from here to
    there, what did it pass through?
  * --ring -- walks the track's route and casts a ray each way across it,
    reporting the first solid wall on each side and flagging the samples with
    none inside --reach metres. Those are the openings.
  * --map -- the same thing as a PNG: route, the race's own barriers, the
    always-on world walls, and any --cross line drawn over them.

Usage:
    python tools/race_barrier_gap.py <dir with WORLD.NFST> <Paths dir> 4081 \
        --cross -744.7,-170.8,-986.1,17.7 --map out.png
"""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from topo_probe import Topo, K_WALL, plane_z, point_in_poly  # noqa: E402

PATHS_CONTAINER = 0x80034147
CH_POINTS = 0x00034148
CH_PATHS = 0x00034149
CH_GROUPS = 0x0003414C


def walk(blob, want, off=0, end=None, depth=0, out=None):
    if out is None:
        out = []
    end = len(blob) if end is None else end
    while off + 8 <= end:
        tag, size = struct.unpack_from("<II", blob, off)
        if off + 8 + size > end:
            break
        if tag == want:
            out.append((off + 8, size))
        elif tag & 0x80000000 and depth < 4:
            walk(blob, want, off + 8, off + 8 + size, depth + 1, out)
        off += 8 + size
    return out


def load_route(paths_dir, track):
    """The track's route group, its waypoints, and the whole point graph.

    A point carries its three neighbours at +0x0C (race.h), which is the same
    adjacency BuildPathConnectionsRecursive walks. That graph is enough to lay
    the route down point by point without reimplementing the connection tree.
    """
    path = os.path.join(paths_dir, "Paths%d.bin" % track)
    with open(path, "rb") as f:
        blob = f.read()

    pts = []
    for off, size in walk(blob, CH_POINTS):
        for i in range(size // 0x18):
            r = off + i * 0x18
            x, y = struct.unpack_from("<2f", blob, r)
            (p,) = struct.unpack_from("<H", blob, r + 8)
            nb = struct.unpack_from("<3h", blob, r + 0x0C)
            pts.append((x, y, p, [n for n in nb if n >= 0]))

    # +0x0C holds the links that leave the path -- to the next path along and
    # to the lane going the other way. Inside a path the points are simply
    # consecutive (race.h: first, end, count), so that adjacency is added here
    # or the graph has no way along a path at all.
    for i in range(len(pts) - 1):
        if pts[i][2] != pts[i + 1][2]:
            continue
        if i + 1 not in pts[i][3]:
            pts[i][3].append(i + 1)
        if i not in pts[i + 1][3]:
            pts[i + 1][3].append(i)

    paths = []
    for off, size in walk(blob, CH_PATHS):
        for i in range(size // 0xDC):
            r = off + i * 0xDC
            name = blob[r:r + 0x1C].split(bytes(1))[0].decode("ascii", "replace")
            first, end_, count = struct.unpack_from("<3h", blob, r + 0x1C)
            na, nd = blob[r + 0xAC], blob[r + 0xAD]
            lst = struct.unpack_from("<%dH" % (na + nd), blob, r + 0xAE)                 if na + nd else ()
            paths.append(dict(name=name, first=first, end=end_, count=count,
                              allow=list(lst[:na]), deny=list(lst[na:])))

    for i, rec in enumerate(pts):
        for n in list(rec[3]):
            if 0 <= n < len(pts) and i not in pts[n][3]:
                pts[n][3].append(i)

    for off, size in walk(blob, CH_GROUPS):
        for i in range(size // 0x110):
            r = off + i * 0x110
            (tid,) = struct.unpack_from("<H", blob, r)
            nwp = struct.unpack_from("<b", blob, r + 2)[0]
            loop = blob[r + 3]
            if tid != track:
                continue
            wps = [struct.unpack_from("<2f", blob, r + 8 + k * 8)
                   for k in range(nwp)]
            return wps, bool(loop), pts, paths
    return None, False, pts, paths


def nearest_point(pts, x, y):
    best, bi = 1e30, -1
    for i, rec in enumerate(pts):
        px, py = rec[0], rec[1]
        d = (px - x) ** 2 + (py - y) ** 2
        if d < best:
            best, bi = d, i
    return bi, math.sqrt(best)


def route_polyline(wps, loop, pts, paths, track):
    """The waypoints joined through the point graph, in order.

    Shortest hop count between one waypoint's nearest point and the next,
    over points whose own path the track is allowed on. The engine splines
    this; for a geometry question the point chain is the same line to within
    a metre or so, and it is the line the authored barriers sit beside.
    """
    ok = [p["allow"] == [] or track in p["allow"] for p in paths]
    for i, p in enumerate(paths):
        if track in p["deny"]:
            ok[i] = False

    def usable(i):
        return ok[pts[i][2]] if pts[i][2] < len(ok) else True

    def hop(a, b):
        seen = {a: None}
        q = [a]
        while q:
            nxt = []
            for u in q:
                for v in pts[u][3]:
                    if v in seen or not usable(v):
                        continue
                    seen[v] = u
                    if v == b:
                        chain = [b]
                        while chain[-1] != a:
                            chain.append(seen[chain[-1]])
                        return chain[::-1]
                    nxt.append(v)
            q = nxt
        return None

    chain = []
    n = len(wps)
    for k in range(n if loop else n - 1):
        a = nearest_point(pts, *wps[k])[0]
        b = nearest_point(pts, *wps[(k + 1) % n])[0]
        leg = hop(a, b) if a >= 0 and b >= 0 else None
        if leg is None:
            leg = [a, b]
        if chain and chain[-1] == leg[0]:
            leg = leg[1:]
        chain.extend(leg)
    return [(pts[i][0], pts[i][1], i) for i in chain]


def seg_cross(ax, ay, bx, by, cx, cy, dx, dy):
    """Where AB crosses CD, as t along AB, or None."""
    rx, ry = bx - ax, by - ay
    sx, sy = dx - cx, dy - cy
    den = rx * sy - ry * sx
    if abs(den) < 1e-12:
        return None
    t = ((cx - ax) * sy - (cy - ay) * sx) / den
    u = ((cx - ax) * ry - (cy - ay) * rx) / den
    if 0.0 <= t <= 1.0 and 0.0 <= u <= 1.0:
        return t
    return None


def group_name(t, g):
    if g == 0:
        return "(always on)"
    if g < len(t.group_names):
        return t.group_names[g]
    return "group %d" % g


def race_groups(t, track):
    """The group indices topo_set_groups turns on for a race on `track`."""
    want = {"BARRIERS_%d" % track, "PLAYER_BARRIERS_%d" % track}
    return {i for i, n in enumerate(t.group_names) if n in want}


def walls(t):
    for p in t.polys:
        if p.kind & K_WALL:
            yield p


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("topo_dir")
    ap.add_argument("paths_dir")
    ap.add_argument("track", type=int)
    ap.add_argument("--cross", help="X0,Y0,X1,Y1 -- what a straight run hits")
    ap.add_argument("--ring", action="store_true", help="find the openings")
    ap.add_argument("--reach", type=float, default=60.0,
                    help="how far either way across the route to look")
    ap.add_argument("--map", help="write a PNG here")
    ap.add_argument("--centre", help="X,Y the map is drawn around")
    ap.add_argument("--extent", type=float, default=0.0,
                    help="half-width of the map in metres (0 = the route)")
    args = ap.parse_args()

    t = Topo(os.path.join(args.topo_dir, "WORLD.NFST"))
    on = race_groups(t, args.track)
    print("%d polys, %d groups; race %d turns on: %s" % (
        len(t.polys), len(t.group_names), args.track,
        ", ".join(sorted(t.group_names[i] for i in on)) or "(none)"))

    wps, loop, pts, paths = load_route(args.paths_dir, args.track)
    if wps is None:
        raise SystemExit("track %d has no route group" % args.track)
    line = route_polyline(wps, loop, pts, paths, args.track)
    print("route: %d waypoints, loop=%d, %d points on the chain" % (
        len(wps), loop, len(line)))

    bar = [p for p in walls(t) if p.group in on]
    always = [p for p in walls(t) if p.group == 0]
    other = [p for p in walls(t) if p.group and p.group not in on]
    print("walls: %d on this race's barriers, %d always on, %d in groups this "
          "race leaves off" % (len(bar), len(always), len(other)))

    if args.cross:
        ax, ay, bx, by = [float(v) for v in args.cross.split(",")]
        print("\nwhat the run %.1f,%.1f -> %.1f,%.1f passes through "
              "(%.0f m):" % (ax, ay, bx, by, math.hypot(bx - ax, by - ay)))
        hits = []
        for p in walls(t):
            x0, y0, x1, y1 = p.segment()
            tt = seg_cross(ax, ay, bx, by, x0, y0, x1, y1)
            if tt is None:
                continue
            lo, hi = p.raw_band()
            hits.append((tt, p.idx, p, lo, hi))
        hits.sort(key=lambda h: (h[0], h[1]))
        if not hits:
            print("  nothing -- no wall of any group stands on that line")
        for tt, _i, p, lo, hi in hits:
            x0, y0, x1, y1 = p.segment()
            state = "SOLID" if (p.group == 0 or p.group in on) else "off"
            print("  %6.1f m  %-5s  %-22s  z %7.1f..%-7.1f  %s"
                  "\n            seg %.1f,%.1f -> %.1f,%.1f" % (
                      tt * math.hypot(bx - ax, by - ay), state,
                      group_name(t, p.group), lo, hi,
                      "NO-TOP" if p.rflags & 0x04 else "",
                      x0, y0, x1, y1))

    if args.ring:
        ring(t, line, on, args)

    if args.map:
        write_map(t, line, bar, always, args, on)


def route_heights(t, line):
    """The surface the route itself runs on, point by point.

    Most of this circuit is stacked two and three deep -- the leg past the
    Bronx lot has road at z 8, a deck at 15 and another at 21 -- so a question
    about what is beside the route has to know which of them the route is.
    The first point takes the lowest candidate; after that each point takes
    whichever candidate is nearest the last, which is what following a ramp
    down through a stack looks like.
    """
    zs = []
    last = None
    for x, y, _i in line:
        cand = [plane_z(p, x, y) for p in t.polys
                if not (p.kind & K_WALL) and point_in_poly(p, x, y)]
        if not cand:
            zs.append(last)
            continue
        last = min(cand) if last is None else min(cand, key=lambda z: abs(z - last))
        zs.append(last)
    return zs


def ring(t, line, on, args):
    """Where the race's own ring leaves the route open.

    A ray each way across the route, as far as --reach, against every wall the
    race makes solid. The port's own band test is the one applied, no-top
    substitution and all, so what this reports is what the car would hit.
    """
    print("\nthe ring across the route (rays %.0f m each way):" % args.reach)
    solid = [(p,) + p.segment() for p in walls(t)
             if p.group == 0 or p.group in on]
    zs = route_heights(t, line)
    gaps = 0
    print("   point        x        y      z   left                 right")
    for i in range(len(line) - 1):
        ax, ay, idx = line[i]
        bx, by, _ = line[i + 1]
        z = zs[i]
        dx, dy = bx - ax, by - ay
        ln = math.hypot(dx, dy)
        if ln < 1e-3 or z is None:
            continue
        lo, hi = z + 0.35, z + 3.0
        nx, ny = -dy / ln, dx / ln
        cell = []
        for side in (-1, 1):
            ex, ey = ax + nx * args.reach * side, ay + ny * args.reach * side
            best = None
            for p, x0, y0, x1, y1 in solid:
                tt = seg_cross(ax, ay, ex, ey, x0, y0, x1, y1)
                if tt is None:
                    continue
                blo, bhi = p.band()
                if bhi < lo or blo > hi:
                    continue
                if best is None or tt < best[0]:
                    best = (tt, p)
            if best is None:
                cell.append("%-20s" % "OPEN")
            else:
                cell.append("%5.0f m %-13s" % (
                    best[0] * args.reach,
                    "barrier" if best[1].group else "world"))
        if "OPEN" in "".join(cell):
            gaps += 1
        print("  %5d %8.1f %8.1f %6.1f   %s %s" % (
            idx, ax, ay, z, cell[0], cell[1]))
    print("  %d of %d route samples open on at least one side"
          % (gaps, len(line) - 1))


def write_map(t, line, bar, always, args, on):
    xs = [p[0] for p in line]
    ys = [p[1] for p in line]
    if args.centre:
        cx, cy = [float(v) for v in args.centre.split(",")]
    else:
        cx, cy = (min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0
    ext = args.extent or (max(max(xs) - min(xs), max(ys) - min(ys)) / 2.0 + 80)
    size = 1400
    scale = size / (2.0 * ext)

    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("map needs Pillow")
        return
    img = Image.new("RGB", (size, size), (16, 16, 20))
    d = ImageDraw.Draw(img)

    def to_px(x, y):
        return ((x - cx) * scale + size / 2.0, size / 2.0 - (y - cy) * scale)

    for p in always:
        x0, y0, x1, y1 = p.segment()
        d.line([to_px(x0, y0), to_px(x1, y1)], fill=(54, 54, 62), width=1)
    for p in bar:
        x0, y0, x1, y1 = p.segment()
        d.line([to_px(x0, y0), to_px(x1, y1)], fill=(255, 92, 60), width=3)
    d.line([to_px(x, y) for x, y, _i in line], fill=(255, 255, 255), width=2)
    if args.cross:
        ax, ay, bx, by = [float(v) for v in args.cross.split(",")]
        d.line([to_px(ax, ay), to_px(bx, by)], fill=(90, 200, 255), width=3)
        for x, y in ((ax, ay), (bx, by)):
            px, py = to_px(x, y)
            d.ellipse([px - 5, py - 5, px + 5, py + 5], fill=(90, 200, 255))
    img.save(args.map)
    print("\nmap: %s  centre %.0f,%.0f  half-width %.0f m" % (
        args.map, cx, cy, ext))


if __name__ == "__main__":
    main()
