"""Ask WORLD.NFST what the runtime would have found at a point.

The port logs every wall it stops the car against:

    world: wall 31 at -683,301,4 in F39 (2 near, 32.8 m/s lost)

which is a position, so a complaint about hitting nothing is answerable from
the data rather than by guesswork. This replays the two queries world.c makes
at such a point -- topo_ground_probe() and topo_walls_near() -- against the
same file the 3DS reads, and prints every polygon either of them would have
seen, with the authored flags that decided it.

The two things to look for, both of which produce "there is nothing there":

  * a wall whose band says it is nowhere near the car. 21,699 of L4RA's
    39,487 walls carry rflags 0x04, which throws the authored z values away
    and substitutes -2048/+2048, so the band test cannot reject them at any
    height. A barrier on an overpass is then solid on the street underneath.
  * a wall that is genuinely beside the road but whose reduction to one XY
    segment has swung it across the lane. wallSegment() takes the widest
    separation between any two vertices, which is the right line for a flat
    panel and the wrong one for an L.

Both are visible here as the gap between what the car hit and where the data
says the thing is.

Usage:
    python tools/topo_probe.py <dir with WORLD.NFST> -p X,Y,Z [-p ...]
    python tools/topo_probe.py <dir> --from-log 3DS\\Files\\nfsu2_debug.txt
    python tools/topo_probe.py <dir> --from-log <log> --summary
    python tools/topo_probe.py <dir> --from-log <log> --replay

--replay is for the ground rather than the walls (TASKS.md #238): it walks
the log's telemetry through the car's TopologyCoordinate as topology.c does,
prints each change of section tree, and every point where that tree's ground
and the merged grid's (what a version 1 or 2 file gets) disagree. The probe
and the map still read the merged grid.

Only the barrier groups the runtime would have on are solid (TASKS.md #232):
by default track 4000's, i.e. free roam, which has none of its own. Add
--stage N for career free roam's BARRIERS_CAREER<N>, --track N for a race's
two sets, or --all-groups for every one, which is what the port did before.
"""
import argparse
import math
import os
import re
import struct

import nfst_lz4

Q_XY = 0.125
Q_Z = 1.0 / 256.0
UNBOUNDED_Z = 2048.0

K_QUAD = 0x01
K_WALL = 0x02
K_GROUND = 0x04
K_UNBOUNDED = 0x08
K_STACKED = 0x10
K_NO_EARLY_OUT = 0x20
K_WALLISH = 0x40
K_GROUNDISH = 0x80

# world.c / main.c, the numbers the car actually asks with.
BODY_LO = 0.35
BODY_HI = 3.0
# halfLength 2.3 * WORLD_FOOTPRINT 0.92, plus a frame of travel and the 0.5 m
# slack world_collide() adds. 3.0 m covers a car standing still.
DEFAULT_RADIUS = 3.0


class Poly(object):
    __slots__ = ("idx", "x", "y", "z", "zbase", "kind", "terrain",
                 "group", "bbsel", "rflags")

    def n(self):
        return 4 if (self.kind & K_QUAD) else 3

    def px(self, k):
        return self.x[k] * Q_XY

    def py(self, k):
        return self.y[k] * Q_XY

    def pz(self, k):
        if self.kind & K_UNBOUNDED:
            return -UNBOUNDED_Z if k in (0, 3) else UNBOUNDED_Z
        return self.zbase + self.z[k] * Q_Z

    def box(self):
        b = self.bbsel
        return (self.x[(b >> 0) & 3] * Q_XY, self.y[(b >> 2) & 3] * Q_XY,
                self.x[(b >> 4) & 3] * Q_XY, self.y[(b >> 6) & 3] * Q_XY)

    def band(self):
        if self.kind & K_UNBOUNDED:
            return (-UNBOUNDED_Z, UNBOUNDED_Z)
        zs = [self.zbase + self.z[k] * Q_Z for k in range(self.n())]
        return (min(zs), max(zs))

    # The authored band, ignoring the no-top substitution: what the polygon
    # would cover if rflags 0x04 were not set. This is the number that says
    # whether an unbounded wall is anywhere near the car.
    def raw_band(self):
        zs = [self.zbase + self.z[k] * Q_Z for k in range(self.n())]
        return (min(zs), max(zs))

    def segment(self):
        n = self.n()
        best, bi, bj = -1.0, 0, 1 if n > 1 else 0
        for a in range(n):
            for b in range(a + 1, n):
                dx = self.px(b) - self.px(a)
                dy = self.py(b) - self.py(a)
                d = dx * dx + dy * dy
                if d > best:
                    best, bi, bj = d, a, b
        return (self.px(bi), self.py(bi), self.px(bj), self.py(bj))

    def normal(self):
        ax, ay, az = self.px(0), self.py(0), self.pz(0)
        bx, by, bz = self.px(1), self.py(1), self.pz(1)
        cx, cy, cz = self.px(2), self.py(2), self.pz(2)
        ux, uy, uz = bx - ax, by - ay, bz - az
        vx, vy, vz = cx - ax, cy - ay, cz - az
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        ln = math.sqrt(nx * nx + ny * ny + nz * nz)
        if ln < 1e-9:
            return (0.0, 0.0, 0.0)
        return (nx / ln, ny / ln, nz / ln)


class Topo(object):
    def __init__(self, path):
        blob = nfst_lz4.read(path)
        if blob[:4] != b"NFST":
            raise SystemExit("%s is not an NFST file" % path)
        (self.version, self.polyCount, self.cellCount, self.entryCount) = \
            struct.unpack_from("<4I", blob, 0x04)
        self.ox, self.oy, self.cell = struct.unpack_from("<3f", blob, 0x14)
        self.gw, self.gh = struct.unpack_from("<2I", blob, 0x20)
        self.bbmin = struct.unpack_from("<3f", blob, 0x28)
        self.bbmax = struct.unpack_from("<3f", blob, 0x34)

        base = 0x40
        self.polys = []
        for i in range(self.polyCount):
            rec = base + i * 32
            p = Poly()
            p.idx = i
            p.x = struct.unpack_from("<4h", blob, rec + 0x00)
            p.y = struct.unpack_from("<4h", blob, rec + 0x08)
            p.z = struct.unpack_from("<4h", blob, rec + 0x10)
            (p.zbase,) = struct.unpack_from("<h", blob, rec + 0x18)
            p.kind = blob[rec + 0x1a]
            p.terrain = struct.unpack_from("<b", blob, rec + 0x1b)[0]
            p.group = blob[rec + 0x1c]
            p.bbsel = blob[rec + 0x1d]
            p.rflags = blob[rec + 0x1e]
            self.polys.append(p)

        cbase = base + self.polyCount * 32
        self.cells = struct.unpack_from("<%dI" % (self.cellCount * 2),
                                        blob, cbase)
        ebase = cbase + self.cellCount * 8
        self.entries = struct.unpack_from("<%dI" % self.entryCount,
                                          blob, ebase)

        # Version 2's group table (topo_pack.py), as names by index. Without
        # it every group is solid, which is what a version 1 file gets on the
        # 3DS too.
        self.group_names = []
        gbase = ebase + self.entryCount * 4
        tbase = None
        if self.version >= 2 and gbase + 16 <= len(blob):
            (n,) = struct.unpack_from("<I", blob, gbase)
            for i in range(n):
                r = gbase + 16 + i * 32
                self.group_names.append(
                    blob[r + 9:r + 32].split(b"\0")[0].decode("ascii", "replace"))
            tbase = gbase + 16 + n * 32
        self.on = [True] * 256

        # Version 3's per-section trees and drivable outlines (TASKS.md #238),
        # laid out as topo_pack.py documents.
        self.trees = []
        self.outlines = []
        self.lod = 0
        if self.version >= 3 and tbase is not None and tbase + 16 <= len(blob):
            ntree, self.lod, ncell, nent = struct.unpack_from("<4I", blob, tbase)
            o = tbase + 16
            for i in range(ntree):
                (num, _pad, w, h, ox, oy, pbase, pcount, fcell,
                 fent) = struct.unpack_from("<hHHH2fIIII", blob, o + i * 32)
                self.trees.append(dict(number=num, w=w, h=h, ox=ox, oy=oy,
                                       pbase=pbase, pcount=pcount,
                                       fcell=fcell, fent=fent))
            o += ntree * 32
            self.tcells = struct.unpack_from("<%dH" % (ncell * 2), blob, o)
            o += ncell * 4
            self.tentries = struct.unpack_from("<%dH" % (nent * 2), blob, o)
            o += nent * 4
            nout, npts = struct.unpack_from("<2I", blob, o)
            o += 16
            recs = o
            pts = o + nout * 32
            for i in range(nout):
                (num, n, first, x0, y0, x1, y1) = struct.unpack_from(
                    "<hHI2f2f", blob, recs + i * 32)
                vs = [struct.unpack_from("<2f", blob, pts + (first + k) * 8)
                      for k in range(n)]
                self.outlines.append((num, (x0, y0, x1, y1), vs))
        self.mru = list(range(len(self.outlines)))

    def set_groups(self, track, stage):
        """topo_set_groups(): group 0, the track's two barrier sets and, given
        a career stage, BARRIERS_CAREER<stage>. Everything else off."""
        if not self.group_names:
            return []
        want = {"BARRIERS_%d" % track, "PLAYER_BARRIERS_%d" % track}
        if stage is not None and stage >= 0:
            want.add("BARRIERS_CAREER%d" % stage)
        self.on = [False] * 256
        self.on[0] = True
        names = []
        for i, nm in enumerate(self.group_names):
            if i and nm in want:
                self.on[i] = True
                names.append(nm)
        return names

    def group_name(self, i):
        if i < len(self.group_names):
            return self.group_names[i]
        return "#%d" % i

    def cell_polys(self, x, y, radius):
        inv = 1.0 / self.cell
        cx0 = int(math.floor((x - radius - self.ox) * inv))
        cx1 = int(math.floor((x + radius - self.ox) * inv))
        cy0 = int(math.floor((y - radius - self.oy) * inv))
        cy1 = int(math.floor((y + radius - self.oy) * inv))
        cx0 = max(cx0, 0)
        cy0 = max(cy0, 0)
        cx1 = min(cx1, self.gw - 1)
        cy1 = min(cy1, self.gh - 1)
        seen = set()
        for cy in range(cy0, cy1 + 1):
            for cx in range(cx0, cx1 + 1):
                c = cy * self.gw + cx
                first, n = self.cells[c * 2], self.cells[c * 2 + 1]
                for e in range(n):
                    seen.add(self.entries[first + e])
        return [self.polys[i] for i in sorted(seen)]


def seg_dist2(x0, y0, x1, y1, px, py):
    ex, ey = x1 - x0, y1 - y0
    e2 = ex * ex + ey * ey
    u = 0.0
    if e2 > 1e-8:
        u = ((px - x0) * ex + (py - y0) * ey) / e2
        u = max(0.0, min(1.0, u))
    dx, dy = px - (x0 + ex * u), py - (y0 + ey * u)
    return dx * dx + dy * dy


def point_in_poly(p, x, y):
    n = p.n()
    inside = False
    j = n - 1
    for i in range(n):
        xi, yi = p.px(i), p.py(i)
        xj, yj = p.px(j), p.py(j)
        if (yi > y) != (yj > y):
            t = (y - yi) / (yj - yi)
            if x < xi + t * (xj - xi):
                inside = not inside
        j = i
    return inside


def plane_z(p, x, y):
    nx, ny, nz = p.normal()
    if abs(nz) < 1e-6:
        return None
    ax, ay, az = p.px(0), p.py(0), p.pz(0)
    return az - (nx * (x - ax) + ny * (y - ay)) / nz


# ------------------------------------------------------------ the coordinate
#
# topology.c's TopologyCoordinate, line for line, so a hardware log can be
# replayed through the tree selection the 3DS now does (#238) and set against
# the merged grid it did before. See topology.c for where each number is from.

STACK_WINDOW = 4.0
NO_CEILING = 3.4028235e38


def amount_outside(p, x, y):
    n = p.n()
    worst = -1.0
    for k in range(n):
        j = 0 if k + 1 == n else k + 1
        x0, y0 = p.px(k), p.py(k)
        d = (p.py(j) - y0) * (x - x0) - (p.px(j) - x0) * (y - y0)
        if d > worst:
            worst = d
            if worst > 0.0:
                break
    return worst


def ol_inside(o, x, y):
    _num, (x0, y0, x1, y1), v = o
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    inside = False
    n = len(v)
    j = n - 1
    for i in range(n):
        xi, yi = v[i]
        xj, yj = v[j]
        if (((yi <= y and y < yj) or (yj <= y and y < yi)) and
                x < (xj - xi) * (y - yi) / (yj - yi) + xi):
            inside = not inside
        j = i
    return inside


def ol_edge(a, b, x, y):
    ex, ey = b[0] - a[0], b[1] - a[1]
    wx, wy = x - a[0], y - a[1]
    l2 = ex * ex + ey * ey
    if l2 <= 5e-11:
        return math.hypot(wx, wy)
    ln = math.sqrt(l2)
    ex, ey = ex / ln, ey / ln
    along = ex * wx + ey * wy
    across = ey * wx - ex * wy
    if along < 0.0:
        return math.hypot(along, across)
    if along > ln:
        return math.hypot(along - ln, across)
    return abs(across)


def ol_dist_inside(o, x, y):
    if not ol_inside(o, x, y):
        return 0.0
    v = o[2]
    d = math.hypot(x - v[0][0], y - v[0][1])
    for i in range(len(v)):
        d = min(d, ol_edge(v[i], v[(i + 1) % len(v)], x, y))
    return d


def ol_dist_outside(o, x, y, maxd):
    _num, (x0, y0, x1, y1), v = o
    if x < x0 - maxd or x > x1 + maxd or y < y0 - maxd or y > y1 + maxd:
        return maxd
    if ol_inside(o, x, y):
        return 0.0
    d = maxd
    for i in range(len(v)):
        d = min(d, ol_edge(v[i], v[(i + 1) % len(v)], x, y))
    return d


def closest_outline(t, x, y):
    best, bi = 9999999.0, -1
    for i, o in enumerate(t.mru):
        ol = t.outlines[o]
        if ol_inside(ol, x, y):
            t.mru.insert(0, t.mru.pop(i))
            return o, 0.0
        d = ol_dist_outside(ol, x, y, best)
        if bi < 0 or d < best or (d == best and ol[0] < t.outlines[bi][0]):
            bi, best = o, d
    return bi, best


def find_outline(t, x, y):
    for i, o in enumerate(t.mru):
        if ol_inside(t.outlines[o], x, y):
            t.mru.insert(0, t.mru.pop(i))
            return o
    o, d = closest_outline(t, x, y)
    return o if d < 0.1 else -1


def section_name(n):
    return "%c%02d" % (chr(ord("A") + n // 100 - 1), n % 100) if n > 0 else "none"


class Coord(object):
    def __init__(self):
        self.at = (0.0, 0.0)
        self.outline = -1
        self.dist = -999.0
        self.tree = -1
        self.cache = []

    def tree_for(self, t, section):
        if section <= 0:
            return -1
        for i, tr in enumerate(t.trees):
            if tr["number"] - t.lod == section:
                return i
        return -1

    def connect(self, tree):
        self.tree = tree
        self.cache = []

    def set_position(self, t, x, y, radius):
        """SetSectionPosition. True when the tree changed."""
        if not (self.dist + radius < abs(x - self.at[0]) + abs(y - self.at[1])):
            return False
        d2 = (x - self.at[0]) ** 2 + (y - self.at[1]) ** 2
        moved = math.sqrt(d2) if d2 > 5e-11 else 0.0
        if not (self.dist + radius < moved):
            return False
        self.at = (x, y)
        if self.outline >= 0 and ol_inside(t.outlines[self.outline], x, y):
            self.dist = ol_dist_inside(t.outlines[self.outline], x, y)
            return False
        self.outline = find_outline(t, x, y)
        if self.outline < 0:
            self.dist = closest_outline(t, x, y)[1]
        else:
            self.dist = ol_dist_inside(t.outlines[self.outline], x, y)
        section = t.outlines[self.outline][0] if self.outline >= 0 else 0
        before = self.tree
        if self.tree < 0:
            if section:
                tree = self.tree_for(t, section)
                if tree >= 0:
                    self.connect(tree)
        elif t.trees[self.tree]["number"] - t.lod != section:
            self.connect(self.tree_for(t, section))
        return self.tree != before

    def section(self, t):
        return t.trees[self.tree]["number"] - t.lod if self.tree >= 0 else 0


def merged_ground(t, x, y, ceiling):
    """topology.c groundAt(): one pass on the merged grid."""
    if not t.cell:
        return None
    cx = int(math.floor((x - t.ox) / t.cell))
    cy = int(math.floor((y - t.oy) / t.cell))
    if cx < 0 or cy < 0 or cx >= t.gw or cy >= t.gh:
        return None
    c = cy * t.gw + cx
    first, n = t.cells[c * 2], t.cells[c * 2 + 1]
    best, bestz = None, -1e30
    for e in range(n):
        p = t.polys[t.entries[first + e]]
        if not (p.kind & K_GROUND) or not t.on[p.group]:
            continue
        bx0, by0, bx1, by1 = p.box()
        if x < bx0 or x > bx1 or y < by0 or y > by1:
            continue
        out = amount_outside(p, x, y)
        if out > 0.0:
            continue
        z = plane_z(p, x, y)
        if z is None:
            continue
        if not (p.kind & K_STACKED) and out < 0.0:
            return z, p
        if z <= bestz or z > ceiling:
            continue
        best, bestz = p, z
        if not (p.kind & K_NO_EARLY_OUT) and ceiling - z < STACK_WINDOW:
            break
    return (bestz, best) if best else None


def tree_ground(t, c, x, y, ceiling):
    """topology.c treeGroundAt()."""
    for i, idx in enumerate(c.cache):
        p = t.polys[idx]
        if not (amount_outside(p, x, y) < 0.0):
            continue
        z = plane_z(p, x, y)
        if z is None:
            continue
        if (p.rflags & 0x01) and not (z <= ceiling and ceiling - z < STACK_WINDOW):
            continue
        c.cache.insert(0, c.cache.pop(i))
        return z, p
    tr = t.trees[c.tree]
    cx = int((x - tr["ox"]) * 0.03125)
    cy = int((y - tr["oy"]) * 0.03125)
    sx = int((x - (cx * 32.0 + tr["ox"])) * 0.125)
    sy = int((y - (cy * 32.0 + tr["oy"])) * 0.125)
    mask = (1 << sx) << (sy * 4) if (0 <= sx < 4 and 0 <= sy < 4) else 0
    cx = max(0, min(cx, tr["w"] - 1))
    cy = max(0, min(cy, tr["h"] - 1))
    ci = (tr["fcell"] + cy * tr["w"] + cx) * 2
    first, count = t.tcells[ci], t.tcells[ci + 1]
    hit = None
    best, bestz = None, -99999.0
    for _pass in range(2):
        for e in range(count):
            k = (tr["fent"] + first + e) * 2
            if not (t.tentries[k + 1] & mask):
                continue
            p = t.polys[tr["pbase"] + t.tentries[k]]
            if p.rflags & 0x0a:
                break
            bx0, by0, bx1, by1 = p.box()
            if x < bx0 or y < by0 or x > bx1 or y > by1:
                continue
            if not t.on[p.group] or not (p.kind & K_GROUND):
                continue
            out = amount_outside(p, x, y)
            if out > 0.0:
                continue
            z = plane_z(p, x, y)
            if z is None:
                continue
            if not (p.rflags & 0x01) and out < 0.0:
                hit = (z, p)
                break
            if bestz < z <= ceiling:
                best, bestz = p, z
                if not (p.rflags & 0x40) and ceiling - z < STACK_WINDOW:
                    break
        if not hit and best:
            hit = (bestz, best)
        if hit:
            break
        ceiling = NO_CEILING
    if hit and not (hit[1].rflags & 0x40):
        c.cache.insert(0, hit[1].idx)
        del c.cache[4:]
    return hit


def ground_v3(t, c, x, y, zmax):
    """topo_ground_probe() for one point through coordinate `c`."""
    if t.trees:
        c.set_position(t, x, y, 9.0)
    if t.trees and c.tree >= 0:
        return tree_ground(t, c, x, y, zmax)
    return merged_ground(t, x, y, zmax) or merged_ground(t, x, y, NO_CEILING)


def replay(t, log, jump):
    """Every telemetry point of a log through the car's coordinate: which tree
    it reads, the ground it gets there, and what the merged grid said."""
    tel = tel_from_log(log)
    if not t.trees:
        raise SystemExit("this WORLD.NFST has no section trees -- re-run topo_pack.py")
    c = Coord()
    differ = 0
    for (n, x, y, z, spd) in tel:
        if n == 1:
            c = Coord()
        before = c.section(t)
        v3 = ground_v3(t, c, x, y, z + 1.0)
        old = merged_ground(t, x, y, z + 1.0) or merged_ground(t, x, y, NO_CEILING)
        sec = c.section(t)
        if sec != before:
            print("tel %-4d %8.1f,%-8.1f z=%-6.1f tree %s -> %s"
                  % (n, x, y, z, section_name(before), section_name(sec)))
        gz = v3[0] if v3 else None
        oz = old[0] if old else None
        if gz is not None and oz is not None and abs(gz - oz) > jump:
            differ += 1
            print("tel %-4d %8.1f,%-8.1f z=%-6.1f  tree %s ground %.2f, merged grid %.2f"
                  % (n, x, y, z, section_name(sec), gz, oz))
    print("%d telemetry points, %d where the trees and the merged grid "
          "disagree by more than %.1f m" % (len(tel), differ, jump))


def flagstr(p):
    bits = []
    if p.rflags & 0x01:
        bits.append("stacked")
    if p.rflags & 0x02:
        bits.append("WALL")
    if p.rflags & 0x04:
        bits.append("NO-TOP")
    if p.rflags & 0x08:
        bits.append("not-road")
    if p.rflags & 0x10:
        bits.append("quad")
    if p.rflags & 0x40:
        bits.append("keep-looking")
    return ",".join(bits) if bits else "-"


def probe(t, x, y, z, radius, verbose=True):
    """Replay both queries at one point. Returns the list of walls that the
    runtime's band test would NOT have rejected."""
    zlo, zhi = z + BODY_LO, z + BODY_HI
    cands = t.cell_polys(x, y, radius)

    ground = []
    for p in cands:
        if not (p.kind & K_GROUND):
            continue
        if not t.on[p.group]:
            continue
        if not point_in_poly(p, x, y):
            continue
        pz = plane_z(p, x, y)
        if pz is not None:
            ground.append((pz, p))
    ground.sort(key=lambda g: -g[0])

    hits = []
    off = []              # walls that would have been hits but are switched off
    for p in cands:
        if not (p.kind & K_WALL):
            continue
        bx0, by0, bx1, by1 = p.box()
        if (x + radius < min(bx0, bx1) or x - radius > max(bx0, bx1) or
                y + radius < min(by0, by1) or y - radius > max(by0, by1)):
            continue
        lo, hi = p.band()
        if hi < zlo or lo > zhi:
            continue
        sx0, sy0, sx1, sy1 = p.segment()
        d2 = seg_dist2(sx0, sy0, sx1, sy1, x, y)
        if d2 > radius * radius:
            continue
        if not t.on[p.group]:
            off.append(p)
            continue
        hits.append((math.sqrt(d2), p))
    hits.sort(key=lambda h: h[0])

    if verbose:
        print("point %.1f,%.1f,%.1f   car band %.2f..%.2f   radius %.1f m"
              % (x, y, z, zlo, zhi, radius))
        if ground:
            pz, p = ground[0]
            print("  ground  z=%.2f terrain=%d %s  (%d candidate%s)"
                  % (pz, p.terrain, flagstr(p), len(ground),
                     "" if len(ground) == 1 else "s"))
        else:
            print("  ground  none found")
        if not hits:
            print("  walls   none")
        if off:
            groups = sorted(set(t.group_name(p.group) for p in off))
            print("  off     %d wall%s in groups switched off: %s"
                  % (len(off), "" if len(off) == 1 else "s", ", ".join(groups)))
        for d, p in hits:
            lo, hi = p.band()
            rlo, rhi = p.raw_band()
            nx, ny, nz = p.normal()
            note = ""
            if p.rflags & 0x04:
                gap = 0.0
                if rhi < zlo:
                    gap = zlo - rhi
                elif rlo > zhi:
                    gap = rlo - zhi
                note = ("  <-- NO-TOP; authored %.2f..%.2f, %.1f m from the car"
                        % (rlo, rhi, gap)) if gap > 0.0 else \
                       "  <-- NO-TOP, authored band does reach the car"
            sx0, sy0, sx1, sy1 = p.segment()
            print("  wall    %.2f m  band %8.1f..%-8.1f n.z=%+.2f grp=%s %s%s"
                  % (d, lo, hi, nz, t.group_name(p.group) if p.group else "0",
                     flagstr(p), note))
            print("          seg %.1f,%.1f -> %.1f,%.1f  (%.1f m long)"
                  % (sx0, sy0, sx1, sy1,
                     math.hypot(sx1 - sx0, sy1 - sy0)))
        print("")
    return hits, ground


LOG_RE = re.compile(
    r"world: wall (\d+) at (-?\d+),(-?\d+),(-?\d+) in (\S+) "
    r"\((\d+) near, ([\d.]+) m/s lost\)")

TEL_RE = re.compile(
    r"world: tel (\d+) pos (-?[\d.]+),(-?[\d.]+),(-?[\d.]+) .*?spd ([\d.]+)")


def tel_from_log(path):
    out = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = TEL_RE.search(line)
            if m:
                out.append((int(m.group(1)), float(m.group(2)),
                            float(m.group(3)), float(m.group(4)),
                            float(m.group(5))))
    return out


# --------------------------------------------------------------------- the map
#
# A picture is the shortest way to settle "there is nothing there": the walls
# the car is being stopped by, the ground the data says is under it, and where
# the car actually drove, all in one top-down view at the same scale.

SVG_HEAD = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" \
width="%d" height="%d">
<style>
  .bg   { fill: #11151c; }
  .road { fill: #3c4654; stroke: #4c586a; stroke-width: 0.3; }
  .other{ fill: #242a34; stroke: #2e3642; stroke-width: 0.3; }
  .wall { stroke: #ff6b57; stroke-width: 1.4; stroke-linecap: round;
          fill: none; }
  .notop{ stroke: #ffc14d; }
  .path { stroke: #5ad1ff; stroke-width: 1.2; fill: none;
          stroke-linejoin: round; }
  .hit  { fill: none; stroke: #ff3b2f; stroke-width: 1.2; }
  text  { fill: #93a1b5; font: 7px sans-serif; }
  .key  { fill: #cdd6e3; font: 9px sans-serif; }
</style>
<rect class="bg" x="0" y="0" width="%d" height="%d"/>
"""


def write_map(t, cx, cy, extent, out_path, telemetry, hits, scale=6.0):
    """Top-down SVG of everything the collision data holds near (cx, cy).

    World Y is drawn upward, which is how the minimap and every screenshot
    show the city, so north on this picture is north in the game."""
    span = extent * 2.0
    w = h = int(span * scale)

    def sx(x):
        return (x - (cx - extent)) * scale

    def sy(y):
        return (h - (y - (cy - extent)) * scale)

    parts = [SVG_HEAD % (w, h, w, h, w, h)]
    polys = t.cell_polys(cx, cy, extent * 1.5)

    # Ground first, so the walls draw over it.
    for p in polys:
        if not (p.kind & K_GROUND) or not t.on[p.group]:
            continue
        pts = " ".join("%.1f,%.1f" % (sx(p.px(k)), sy(p.py(k)))
                       for k in range(p.n()))
        parts.append('<polygon class="%s" points="%s"/>\n'
                     % ("road" if p.terrain == 1 else "other", pts))

    nwall = 0
    for p in polys:
        if not (p.kind & K_WALL) or not t.on[p.group]:
            continue
        x0, y0, x1, y1 = p.segment()
        cls = "wall notop" if (p.rflags & 0x04) else "wall"
        parts.append('<line class="%s" x1="%.1f" y1="%.1f" x2="%.1f" '
                     'y2="%.1f"/>\n'
                     % (cls, sx(x0), sy(y0), sx(x1), sy(y1)))
        nwall += 1

    pts = [(x, y) for (_, x, y, _z, _s) in telemetry
           if abs(x - cx) < extent * 1.4 and abs(y - cy) < extent * 1.4]
    if len(pts) > 1:
        parts.append('<polyline class="path" points="%s"/>\n'
                     % " ".join("%.1f,%.1f" % (sx(x), sy(y))
                                for x, y in pts))

    for (n, x, y, _z, _sec, _near, lost) in hits:
        if abs(x - cx) > extent or abs(y - cy) > extent:
            continue
        r = 3.0 + min(lost, 35.0) * 0.25
        parts.append('<circle class="hit" cx="%.1f" cy="%.1f" r="%.1f"/>\n'
                     % (sx(x), sy(y), r))
        parts.append('<text x="%.1f" y="%.1f">%d</text>\n'
                     % (sx(x) + r + 2, sy(y) + 3, n))

    parts.append('<text class="key" x="8" y="%d">%.0f,%.0f  %.0f m across  '
                 '%d walls  road grey-blue, other grey, wall red, '
                 'no-top amber, car path blue</text>\n'
                 % (h - 8, cx, cy, span, nwall))
    parts.append("</svg>\n")

    with open(out_path, "w") as f:
        f.write("".join(parts))
    print("wrote %s (%.0f m across, %d walls)" % (out_path, span, nwall))


def from_log(path):
    out = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = LOG_RE.search(line)
            if m:
                out.append((int(m.group(1)), float(m.group(2)),
                            float(m.group(3)), float(m.group(4)),
                            m.group(5), int(m.group(6)), float(m.group(7))))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="directory holding WORLD.NFST")
    ap.add_argument("-p", "--point", action="append", default=[],
                    help="X,Y,Z to probe; repeatable")
    ap.add_argument("--from-log", help="read every logged wall hit from this log")
    ap.add_argument("--radius", type=float, default=DEFAULT_RADIUS)
    ap.add_argument("--summary", action="store_true",
                    help="one line per hit instead of the full dump")
    ap.add_argument("--map", help="X,Y to draw a top-down SVG around")
    ap.add_argument("--extent", type=float, default=60.0,
                    help="half-width of the map in metres (default 60)")
    ap.add_argument("--out", default="topo_map.svg")
    ap.add_argument("--track", type=int, default=4000,
                    help="track whose barriers are on (default 4000, free roam)")
    ap.add_argument("--stage", type=int, default=None,
                    help="career stage whose BARRIERS_CAREER set is on; "
                         "leave out for a race")
    ap.add_argument("--all-groups", action="store_true",
                    help="every group solid, as before #232")
    ap.add_argument("--replay", action="store_true",
                    help="run --from-log's telemetry through the car's "
                         "section trees and compare with the merged grid")
    ap.add_argument("--jump", type=float, default=2.0,
                    help="--replay reports ground differences above this (m)")
    args = ap.parse_args()

    t = Topo(os.path.join(args.dir, "WORLD.NFST"))
    print("%d polys, %dx%d@%.0fm grid at %.1f,%.1f, %d entries"
          % (t.polyCount, t.gw, t.gh, t.cell, t.ox, t.oy, t.entryCount))
    nowall = sum(1 for p in t.polys if (p.rflags & 0x06) == 0x06)
    walls = sum(1 for p in t.polys if p.rflags & 0x02)
    print("%d walls, %d of them no-top (%.0f%%)"
          % (walls, nowall, 100.0 * nowall / max(walls, 1)))
    if not t.group_names:
        print("no group table (a version 1 file): every group is solid\n")
    elif args.all_groups:
        print("every group solid (--all-groups)\n")
    else:
        on = t.set_groups(args.track, args.stage)
        print("groups on for track %d%s: %s\n"
              % (args.track, "" if args.stage is None else
                 ", career stage %d" % args.stage,
                 " ".join(on) if on else "none past group 0"))

    if t.trees:
        print("%d section trees over %d drivable outlines (lod %d)\n"
              % (len(t.trees), len(t.outlines), t.lod))
    if args.replay:
        if not args.from_log:
            ap.error("--replay needs --from-log")
        replay(t, args.from_log, args.jump)
        return

    pts = []
    for s in args.point:
        a = [float(v) for v in s.split(",")]
        pts.append((0, a[0], a[1], a[2], "-", 0, 0.0))
    if args.from_log:
        pts.extend(from_log(args.from_log))

    if args.map:
        mx, my = [float(v) for v in args.map.split(",")[:2]]
        tel = tel_from_log(args.from_log) if args.from_log else []
        write_map(t, mx, my, args.extent, args.out, tel, pts)
        return

    if not pts:
        ap.error("give -p, --from-log or --map")

    ntop = 0
    for (n, x, y, z, sec, near, lost) in pts:
        hits, ground = probe(t, x, y, z, args.radius,
                             verbose=not args.summary)
        if args.summary:
            top = [p for _, p in hits if p.rflags & 0x04]
            far = 0
            for p in top:
                rlo, rhi = p.raw_band()
                if rhi < z + BODY_LO or rlo > z + BODY_HI:
                    far += 1
            gz = ("%.2f" % ground[0][0]) if ground else "none"
            gt = ("%d" % ground[0][1].terrain) if ground else "-"
            print("wall %-3d %-4s %8.1f,%-8.1f z=%-5.1f ground=%-6s terr=%-3s "
                  "walls=%-3d no-top=%-3d off-level=%-3d  %.1f m/s"
                  % (n, sec, x, y, z, gz, gt, len(hits), len(top), far, lost))
            ntop += far
    if args.summary:
        print("\n%d of the gathered walls are no-top walls whose authored "
              "height is nowhere near the car." % ntop)


if __name__ == "__main__":
    main()
