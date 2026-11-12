#!/usr/bin/env python3
"""Convert the retail collision world (TopologyTree) to WORLD.NFST.

TASKS.md #236. Until now the port collided against the *drawn* geometry --
model3d's triangle scans over whatever the renderer happens to hold, with one
steepness threshold (WALK_NZ 0.35) deciding drivable from solid -- which is not
what retail does and cannot be made to match it. The real game ships a separate
collision dataset, authored alongside the art, and this is it.

What it is, in retail's own terms
---------------------------------
`TrackLoader::InitTopologyAndSceneryGroups` registers two chunk loaders,
`TopologyManager` and `TopologyTree`. `TopologyTree::Loader` (Speed.elf
0x801f2678) reads one container per *drivable* section -- 0x80034130, 255 of
them in STREAML4RA.BUN -- holding four leaves:

    0x00034131  header, 48 bytes, 16-byte aligned payload
    0x00034134  polygons, 32-byte records
    0x00034135  u16 table        (absent from every L4RA container)
    0x00034136  cell entries, 4-byte records
    0x00034137  one record per grid cell, 4 bytes

The loader's byte-swap loops give the field widths exactly: it swaps the
header's u16s at +0, +4 and +6 and the three bVectors at +8/+0x10/+0x20, and in
a polygon record the u16s at +4, +6 and every u16 from +8 to +0x1f, leaving
+0..+3 as four bytes. Everything below was then confirmed a second time
against `TopologyCoordinate::NewGetElevation` (0x801f1484), which walks the
same structures at runtime and so pins every offset, stride and scale.

Header (0x00034131)
    +0x00 u16   section number, letter*100 + n (A21 -> 121), the same
                numbering L4RA.BUN's visibility table uses
    +0x02 u8    "already byte-swapped" marker; 0 on disk
    +0x03 u8    unused as far as the loader is concerned
    +0x04 u16   grid width in cells     (NewGetElevation reads it at obj+0x2c)
    +0x06 u16   grid height in cells    (obj+0x2e)
    +0x08 f32x2 grid origin (x, y)
    +0x10 f32x3 bbox min
    +0x20 f32x3 bbox max

The cell is 32 m and the sub-cell 8 m, both straight out of NewGetElevation:
it multiplies (pos - origin) by 0.03125 (0x80336130) to get the cell, scales
the cell index back by 32.0 (0x80336140) to get the cell's own origin, and
multiplies (pos - cellOrigin) by 0.125 (0x80336144) to get the sub-cell.

Polygon (0x00034134), 32 bytes. `TrackPolygon::TrackPolygon` (0x801f0150)
decodes it and NewGetElevation reads the result back:
    +0x00 u8    topology-group index; 0 means "always collidable", otherwise
                NewGetElevation and BuildTrackPolygons look up a 32-byte record
                in the table at *(0x803EFD8C) and skip the polygon when that
                record's byte +8 is zero. 12,049 of L4RA's polygons carry one,
                and every group is a set of race barriers: BARRIERS_<track>,
                PLAYER_BARRIERS_<track>, BARRIERS_CAREER<stage>. Retail turns
                them all off at load and back on only for the race being run
                (TASKS.md #232), so this table is written out too, below.
    +0x01 u8    four 2-bit selectors saying which vertex carries each side of
                the polygon's XY bounding box: minX = x[(b>>0)&3],
                minY = y[(b>>2)&3], maxX = x[(b>>4)&3], maxY = y[(b>>6)&3]
    +0x02 s8    terrain type   (GetTerrainType, 0x801b0c14, returns it, and
                NewGetElevation hands it back to the caller)
    +0x03 u8    flags, below
    +0x04 s16   z base, in metres
    +0x06 u16   runtime slot, written back by the constructor; 0xFFFF on disk
    +0x08 s16x4 x, +0x10 s16x4 y, +0x18 s16x4 z offset

and the arithmetic is `(v << 13) * 2^-16` for x and y -- eighths of a metre --
and `((zbase << 16) + (z << 8)) * 2^-16` for z, i.e. zbase metres plus the
offset in 256ths.

The flags byte is the part the port most needed, and every bit of it is
accounted for by L4RA's own data:

    0x01  the polygon is one level of a stack, so the elevation query may only
          take it when the query point is above it: z <= pos.z + 1 and within
          4 m of that (0x80336128, 0x8033612c). An unflagged polygon is taken
          on sight, because there is nothing above or below it to confuse it.
          15,608 polygons, every one of them ground.
    0x02  wall. 39,487 polygons, and all 39,487 are wallish by geometry too
          (|normal.z| < 0.1) -- the authored flag and the measured normal
          agree exactly, which is the clearest possible sign this is the set
          the game means by "solid".
    0x04  the wall has no top: the constructor throws the four z values away
          and substitutes -2048 / +2048 / +2048 / -2048 (0x80335efc,
          0x80335f00). Only ever set together with 0x02. 21,699 polygons.
    0x08  ground that the elevation query must not stand anything on --
          NewGetElevation rejects `flags & 0x0A` before it does anything else.
          11,137 polygons, 91% of them flat. Roofs, verges behind fences, the
          insides of things: surfaces that exist for the collision solver but
          are not road.
    0x10  four vertices rather than three.
    0x40  found, but keep looking: the elevation search records the polygon as
          its best so far and does *not* take the early exit it would take
          otherwise. Only ever set together with 0x01. 2,259 polygons.
    0x20, 0x80  never set anywhere in L4RA.

Validation: the polygons under the three positions the hardware log recorded
the car at give 7.51 against a car z of 7.90, 3.14 against 3.10 and 2.52
against 2.50 -- a car sitting on its suspension, on the surface the data says
is there.

What this writes
----------------
The port does not page collision. All 153,625 polygons of L4RA come to 4.7 MB,
which the linear heap has room for a dozen times over, and holding them
resident is half the point: it takes every per-section physics cost off the
loader (`model3d_build_phys_grid()` alone was 15.4 ms of a 57.9 ms section
commit on hardware) and makes the answer independent of what the renderer
happens to have paged in. A wall does not appear when its section's art
arrives, and does not vanish when it leaves.

Versions 1 and 2 flattened the 255 per-section grids into one world grid. That
was a mistake, and version 3 keeps retail's trees (TASKS.md #238). The stacked
flag is not a property of a polygon in the world; it belongs to the polygon in
one section's tree. Under the overpass east of O28, tree N28 holds the deck at
z 29.7 and nothing below it, so there the deck is unstacked. O28 and O22 hold
the deck and the street at z 15 together, both stacked. Retail asks only the
tree of the drivable section the car is in, so on that street it gets the
street. The merged grid asked all of them, and N28's unstacked deck won "on
sight, whatever its height". A car driving under the bridge was lifted 14 m
onto a road that only races use.

Which tree is asked is `TopologyCoordinate::SetSectionPosition` (0x801f1bb0).
It walks the drivable section outlines from L4RA.BUN's chunk 0x00034152 and
picks tree `section + ScenerySectionLODOffset`, the first u32 of chunk
0x00034151 (20 for L4RA). Only the drivable outlines take part:
`VisibleSectionManager::Loader` (0x8020b628) lists a record as drivable when
its letter is below U and 0 < n % 100 < the LOD offset. Those are 255 of
L4RA's 351. Both lists are copied here, so the runtime does not need the BUN
to answer.

    header 0x40
      +0x00 u32 'NFST', +0x04 u32 version
      +0x08 u32 polyCount, +0x0c u32 cellCount, +0x10 u32 entryCount
      +0x14 f32x2 grid origin, +0x1c f32 cell size
      +0x20 u32 gridW, +0x24 u32 gridH
      +0x28 f32x3 bbox min, +0x34 f32x3 bbox max
    polygons   polyCount x 32, every tree's in stream order
    cells      cellCount x 8   {u32 first, u32 count}   the merged grid
    entries    entryCount x 4  u32 polygon index        the merged grid
    groups     version 2 on: u32 groupCount, 12 bytes of zero, then
               groupCount x 32, retail's own records from <track>.BUN's
               chunk 0x0003412F, verbatim (tools/topo_groups.py):
                 +0x00 u32 bStringHash of the name
                 +0x04 u32 near the group's distinct polygon count (exact for
                           117 of L4RA's 125; sections repeat border polygons,
                           so the stream holds more); the port does not use it
                 +0x08 u8  enabled (1 on disk)       +0x09 the name, NUL-ended
    trees      version 3 on: u32 treeCount, u32 LOD offset, u32 treeCellCount,
               u32 treeEntryCount, then treeCount x 32:
                 +0x00 s16 the tree's section number (drivable + LOD offset)
                 +0x04 u16 grid width   +0x06 u16 grid height
                 +0x08 f32x2 grid origin
                 +0x10 u32 index of the tree's polygon 0 in the array above
                 +0x14 u32 its polygon count
                 +0x18 u32 its first cell   +0x1c u32 its first entry
               then the cells, treeCellCount x {u16 first, u16 count}, and
               the entries, treeEntryCount x {u16 polygon, u16 sub-cell mask},
               retail's own records, verbatim and relative to their tree
    outlines   version 3 on: u32 count, u32 pointCount, 8 bytes of zero, then
               count x 32 {s16 section, u16 numPoints, u32 firstPoint,
               f32x2 bbox min, f32x2 bbox max, 8 bytes of zero}, then
               pointCount x f32x2. The drivable ones only, in file order.

A version 1 file is the same without the groups. The runtime still reads it,
and then has no way to tell one race's barriers from another's. A version 2
file has no trees, so the runtime falls back to the merged grid and the stacked
roads can lift the car again. Both are logged.

The trees' cells and entries are kept exactly, sub-cell masks included,
because the elevation search's answer depends on their order. Retail stops a
cell's search at its first wall or non-road polygon (`flags & 0x0a`), and
every L4RA cell lists its road first. So a polygon that cannot be used, one
with no area, is kept in its place as a record of kind 0 rather than dropped.
Dropping it would shift every index after it. The merged grid leaves those
records out. Nothing reads them there. L4RA has none, but the packer does not
rely on that.

A packed polygon keeps retail's quantisation, because dequantising costs two
multiplies and keeping it halves the memory:

    +0x00 s16x4 x       eighths of a metre
    +0x08 s16x4 y       eighths of a metre
    +0x10 s16x4 z       256ths of a metre, relative to zbase
    +0x18 s16   zbase   metres
    +0x1a u8    kind    the derived classification, below
    +0x1b s8    terrain
    +0x1c u8    group   record byte +0
    +0x1d u8    bbsel   record byte +1
    +0x1e u8    rflags  record byte +3, verbatim, so nothing is lost
    +0x1f u8    pad

`kind` is worked out here once instead of per query:

    0x01 quad                 rflags & 0x10
    0x02 wall                 rflags & 0x02
    0x04 elevation candidate  (rflags & 0x0a) == 0
    0x08 unbounded height     rflags & 0x04
    0x10 stacked              rflags & 0x01
    0x20 do not early-out     rflags & 0x40
    0x40 wallish              |normal.z| < 0.1   (IsWallish, 0x801b0c24)
    0x80 groundish            |normal.z| > 0.7   (IsGroundish, 0x801b0c48)

Note that those last two leave a band in the middle that is *neither* -- a
thing retail's own code is content with and the port's single threshold could
not express. They are carried for the collision response to use; what is
solid and what is standable are the authored bits, not these.

Usage:
    python tools/topo_pack.py <PC Files/TRACKS> <out dir> [--track L4RA]
"""
import argparse
import math
import os
import struct

import nfst_lz4

from topo_groups import GROUP_TABLE_TAG, walk as walk_chunks

TOPO_GROUP_TAG = 0x80034130
TOPO_HEADER_TAG = 0x00034131
TOPO_POLY_TAG = 0x00034134
TOPO_ENTRY_TAG = 0x00034136
TOPO_CELL_TAG = 0x00034137

POLY_STRIDE = 32
CELL_SIZE = 32.0          # 0x80336130 is 1/32

# TrackPolygon's flags, as the constructor and NewGetElevation read them.
RF_STACKED = 0x01
RF_WALL = 0x02
RF_UNBOUNDED = 0x04
RF_NOT_ROAD = 0x08
RF_QUAD = 0x10
RF_NO_EARLY_OUT = 0x40

# The constructor's substitute z for an unbounded wall (0x80335efc/0x80335f00).
UNBOUNDED_Z = 2048.0

# IsWallish / IsGroundish (0x8033200c, 0x80332010).
WALLISH_NZ = 0.1
GROUNDISH_NZ = 0.7

KIND_QUAD = 0x01
KIND_WALL = 0x02
KIND_GROUND = 0x04
KIND_UNBOUNDED = 0x08
KIND_STACKED = 0x10
KIND_NO_EARLY_OUT = 0x20
KIND_WALLISH = 0x40
KIND_GROUNDISH = 0x80

NFST_MAGIC = b"NFST"
NFST_VERSION = 3
NFST_HDR = 0x40
GROUP_RECORD = 32
TREE_RECORD = 32
OUTLINE_RECORD = 32

# L4RA.BUN's visible-section chunks (VisibleSectionManager::Loader, 0x8020b628).
VIS_INFO_TAG = 0x00034151      # +0: ScenerySectionLODOffset
VIS_BOUNDARY_TAG = 0x00034152  # the section outlines
VIS_LETTERS = 20               # (number / 100 - 1) & 0xff < 0x14


def walk_groups(blob):
    """Every TOPO_GROUP_TAG container in the stream file, as {tag: (off, size)}."""
    out = []
    off, end = 0, len(blob)
    while off + 8 <= end:
        tag, size = struct.unpack_from("<II", blob, off)
        if off + 8 + size > end:
            break
        if tag == TOPO_GROUP_TAG:
            parts = {}
            c, e = off + 8, off + 8 + size
            while c + 8 <= e:
                t, s = struct.unpack_from("<II", blob, c)
                if c + 8 + s > e:
                    break
                parts[t] = (c + 8, s)
                c += 8 + s
            out.append(parts)
        off += 8 + size
    return out


def read_group_table(path):
    """Retail's topology-group records from <track>.BUN, as one bytes object.

    The table is TopologyManager's (Loader, 0x801f35bc), one chunk in the
    track's own BUN rather than in the stream file the polygons come from."""
    with open(path, "rb") as f:
        blob = f.read()
    found = walk_chunks(blob, GROUP_TABLE_TAG)
    if len(found) != 1:
        raise SystemExit("%s: %d group tables (0x%08X), expected one"
                         % (path, len(found), GROUP_TABLE_TAG))
    off, size = found[0]
    # 16-byte aligned behind 0x11 filler, like the other topology leaves.
    start = off
    while start < off + size and blob[start] == 0x11:
        start += 1
    n = (off + size - start) // GROUP_RECORD
    return blob[start:start + n * GROUP_RECORD], n


def read_outlines(path):
    """ScenerySectionLODOffset and the drivable section outlines, from
    <track>.BUN, as (lod, [(number, (x0, y0), (x1, y1), [(x, y), ...])]).

    The records are VisibleSectionBoundary as the loader leaves them: a list
    node at +0, s16 number at +8, s16 point count at +0xa, the bbox at +0xc
    and +0x14, the points from +0x24. The drivable test is the loader's."""
    with open(path, "rb") as f:
        blob = f.read()
    info = walk_chunks(blob, VIS_INFO_TAG)
    bounds = walk_chunks(blob, VIS_BOUNDARY_TAG)
    if len(info) != 1 or len(bounds) != 1:
        raise SystemExit("%s: %d section-info and %d outline chunks, expected one each"
                         % (path, len(info), len(bounds)))
    lod = struct.unpack_from("<I", blob, info[0][0])[0]
    off, size = bounds[0]
    end = off + size
    out = []
    total = 0
    while off + 0x24 <= end:
        number, npts = struct.unpack_from("<hh", blob, off + 8)
        bbmin = struct.unpack_from("<2f", blob, off + 0x0c)
        bbmax = struct.unpack_from("<2f", blob, off + 0x14)
        pts = [struct.unpack_from("<2f", blob, off + 0x24 + 8 * k) for k in range(npts)]
        total += 1
        if ((number // 100 - 1) & 0xFF) < VIS_LETTERS and 0 < number % 100 < lod:
            out.append((number, bbmin, bbmax, pts))
        off += 0x24 + 8 * npts
    return lod, out, total


def section_name(number):
    return "%c%02d" % (chr(ord("A") + number // 100 - 1), number % 100)


def group_name(table, i):
    rec = table[i * GROUP_RECORD:(i + 1) * GROUP_RECORD]
    return rec[9:].split(b"\0")[0].decode("ascii", "replace")


def polygon_vertices(blob, rec):
    """The polygon's 3 or 4 world-space vertices, retail's arithmetic exactly."""
    flags = blob[rec + 3]
    n = 4 if (flags & RF_QUAD) else 3
    zbase = struct.unpack_from("<h", blob, rec + 4)[0]
    verts = []
    for k in range(n):
        x = struct.unpack_from("<h", blob, rec + 0x08 + 2 * k)[0] / 8.0
        y = struct.unpack_from("<h", blob, rec + 0x10 + 2 * k)[0] / 8.0
        if flags & RF_UNBOUNDED:
            # The constructor overwrites all four, as -2048 / +2048 / +2048 /
            # -2048, turning the footprint into a barrier of unbounded height
            # rather than a quad lying in space.
            z = -UNBOUNDED_Z if k in (0, 3) else UNBOUNDED_Z
        else:
            z = zbase + struct.unpack_from("<h", blob, rec + 0x18 + 2 * k)[0] / 256.0
        verts.append((x, y, z))
    return verts


def polygon_normal(verts):
    """normalise((v1 - v0) x (v2 - v0)), the constructor's own normal."""
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = verts[0], verts[1], verts[2]
    ax, ay, az = x1 - x0, y1 - y0, z1 - z0
    bx, by, bz = x2 - x0, y2 - y0, z2 - z0
    nx = ay * bz - az * by
    ny = az * bx - ax * bz
    nz = ax * by - ay * bx
    n = math.sqrt(nx * nx + ny * ny + nz * nz)
    if n <= 0.0:
        return (0.0, 0.0, 0.0)
    return (nx / n, ny / n, nz / n)


def pack_polygon(blob, rec, keep_dead=False):
    """One 32-byte runtime record plus its vertices, or None if it has no area.

    With `keep_dead` a polygon with no area still gets a record, of kind 0,
    and None for its vertices, so
    a tree's indices stay retail's. Kind 0 is neither road nor wall, so no
    query uses it, but its rflags are kept for the search's `flags & 0x0a`
    stop. Retail could not use such a polygon either. With a zero normal,
    GetAmountOutside comes out exactly 0 and the plane's z is not a number,
    so it fails every test that would take it."""
    rflags = blob[rec + 3]
    verts = polygon_vertices(blob, rec)
    normal = polygon_normal(verts)
    dead = normal == (0.0, 0.0, 0.0)
    if dead and not keep_dead:
        return None
    anz = abs(normal[2])

    kind = 0
    if rflags & RF_QUAD:
        kind |= KIND_QUAD
    if rflags & RF_WALL:
        kind |= KIND_WALL
    if not (rflags & (RF_WALL | RF_NOT_ROAD)):
        kind |= KIND_GROUND
    if rflags & RF_UNBOUNDED:
        kind |= KIND_UNBOUNDED
    if rflags & RF_STACKED:
        kind |= KIND_STACKED
    if rflags & RF_NO_EARLY_OUT:
        kind |= KIND_NO_EARLY_OUT
    if anz < WALLISH_NZ:
        kind |= KIND_WALLISH
    if anz > GROUNDISH_NZ:
        kind |= KIND_GROUNDISH
    if dead:
        kind = 0

    out = bytearray(POLY_STRIDE)
    for k in range(4):
        struct.pack_into("<h", out, 0x00 + 2 * k,
                         struct.unpack_from("<h", blob, rec + 0x08 + 2 * k)[0])
        struct.pack_into("<h", out, 0x08 + 2 * k,
                         struct.unpack_from("<h", blob, rec + 0x10 + 2 * k)[0])
        struct.pack_into("<h", out, 0x10 + 2 * k,
                         struct.unpack_from("<h", blob, rec + 0x18 + 2 * k)[0])
    struct.pack_into("<h", out, 0x18, struct.unpack_from("<h", blob, rec + 4)[0])
    out[0x1A] = kind
    out[0x1B] = blob[rec + 2]
    out[0x1C] = blob[rec + 0]
    out[0x1D] = blob[rec + 1]
    out[0x1E] = rflags
    return bytes(out), (None if dead else verts)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tracks_dir", help="PC Files/TRACKS")
    ap.add_argument("out_dir", help="where WORLD.NFST goes (TRACKS/<track>/)")
    ap.add_argument("--track", default="L4RA")
    ap.add_argument("--raw", action="store_true",
                    help="write WORLD.NFST unwrapped (default: LZ4 blocks, nfst_lz4.py, #270)")
    args = ap.parse_args()

    stream_path = os.path.join(args.tracks_dir, "STREAM%s.BUN" % args.track)
    with open(stream_path, "rb") as f:
        blob = f.read()
    groups = walk_groups(blob)
    if not groups:
        raise SystemExit("%s: no 0x%08X topology containers" % (stream_path,
                                                                TOPO_GROUP_TAG))
    print("%s: %d topology containers" % (os.path.basename(stream_path), len(groups)))
    bun_path = os.path.join(args.tracks_dir, "%s.BUN" % args.track)
    group_table, group_count = read_group_table(bun_path)
    print("%s: %d topology groups" % (os.path.basename(bun_path), group_count))
    per_group = [0] * 256  # polygons per group index, before degenerate drops

    lod, outlines, outline_total = read_outlines(bun_path)
    print("%s: LOD offset %d, %d of %d section outlines drivable"
          % (os.path.basename(bun_path), lod, len(outlines), outline_total))

    polys = []            # packed 32-byte records
    boxes = []            # (minx, maxx, miny, maxy) in metres, None if dead
    kinds = {"wall": 0, "road": 0, "other": 0}
    agree = [0, 0]        # wall flag vs wallish normal: agreements, disagreements
    stacked = grouped = 0
    terrains = {}
    dropped = 0
    trees = bytearray()       # TREE_RECORD per tree
    tree_cells = bytearray()  # retail's {u16 first, u16 count}, per tree
    tree_entries = bytearray()  # retail's {u16 polygon, u16 mask}, per tree
    tree_numbers = []

    for parts in groups:
        if TOPO_HEADER_TAG not in parts or TOPO_POLY_TAG not in parts:
            continue
        if TOPO_CELL_TAG not in parts or TOPO_ENTRY_TAG not in parts:
            raise SystemExit("a topology container has no cells or entries")
        hoff = parts[TOPO_HEADER_TAG][0]
        number, = struct.unpack_from("<h", blob, hoff)
        tw, th = struct.unpack_from("<HH", blob, hoff + 4)
        tox, toy = struct.unpack_from("<2f", blob, hoff + 8)
        poff, psize = parts[TOPO_POLY_TAG]
        coff, csize = parts[TOPO_CELL_TAG]
        eoff, esize = parts[TOPO_ENTRY_TAG]
        npolys = psize // POLY_STRIDE
        nentries = esize // 4
        if csize // 4 < tw * th:
            raise SystemExit("tree %d: %d cells for a %dx%d grid"
                             % (number, csize // 4, tw, th))
        # Every reference has to land inside its own tree, or the runtime
        # would read the next tree's polygons.
        for k in range(tw * th):
            first, count = struct.unpack_from("<HH", blob, coff + 4 * k)
            if first + count > nentries:
                raise SystemExit("tree %d: cell %d overruns its entries" % (number, k))
        for k in range(nentries):
            if struct.unpack_from("<H", blob, eoff + 4 * k)[0] >= npolys:
                raise SystemExit("tree %d: entry %d overruns its polygons" % (number, k))

        trees += struct.pack("<hHHH2fIIII", number, 0, tw, th, tox, toy,
                             len(polys), npolys, len(tree_cells) // 4,
                             len(tree_entries) // 4)
        tree_cells += blob[coff:coff + 4 * tw * th]
        tree_entries += blob[eoff:eoff + 4 * nentries]
        tree_numbers.append(number)

        for i in range(npolys):
            rec = poff + i * POLY_STRIDE
            per_group[blob[rec]] += 1
            rec_bytes, verts = pack_polygon(blob, rec, keep_dead=True)
            polys.append(rec_bytes)
            if verts is None:
                dropped += 1
                boxes.append(None)
                continue
            xs = [v[0] for v in verts]
            ys = [v[1] for v in verts]
            boxes.append((min(xs), max(xs), min(ys), max(ys)))

            kind = rec_bytes[0x1A]
            if kind & KIND_WALL:
                kinds["wall"] += 1
                agree[0 if (kind & KIND_WALLISH) else 1] += 1
            elif kind & KIND_GROUND:
                kinds["road"] += 1
            else:
                kinds["other"] += 1
            if kind & KIND_STACKED:
                stacked += 1
            if rec_bytes[0x1C]:
                grouped += 1
            t = struct.unpack("<b", rec_bytes[0x1B:0x1C])[0]
            terrains[t] = terrains.get(t, 0) + 1

    if not polys:
        raise SystemExit("no polygons decoded")

    # The two files are checked against each other before the table is
    # trusted to say which polygon is whose: every index a polygon carries has
    # to have a record, and every record past 0 should be carried by some.
    used_max = max(i for i in range(256) if per_group[i])
    if used_max >= group_count:
        raise SystemExit("polygons use group %d but %s has %d records"
                         % (used_max, os.path.basename(bun_path), group_count))
    unused = [group_name(group_table, i) for i in range(1, group_count)
              if not per_group[i]]

    # And the outlines against the trees: SetSectionPosition connects drivable
    # section n to tree n + LOD, and a section with no tree is one where the
    # car has no ground at all.
    have = set(tree_numbers)
    if len(have) != len(tree_numbers):
        raise SystemExit("two trees share a section number")
    no_tree = [section_name(o[0]) for o in outlines if o[0] + lod not in have]
    no_outline = [section_name(n) for n in tree_numbers
                  if n - lod not in set(o[0] for o in outlines)]

    outline_recs = bytearray()
    outline_pts = bytearray()
    for number, bbmin, bbmax, pts in outlines:
        outline_recs += struct.pack("<hHI2f2f8x", number, len(pts),
                                    len(outline_pts) // 8, bbmin[0], bbmin[1],
                                    bbmax[0], bbmax[1])
        for p in pts:
            outline_pts += struct.pack("<2f", p[0], p[1])

    live = [b for b in boxes if b is not None]
    minx = min(b[0] for b in live)
    maxx = max(b[1] for b in live)
    miny = min(b[2] for b in live)
    maxy = max(b[3] for b in live)

    # One world grid at retail's own cell, anchored on a multiple of it so the
    # cell a point falls in is a floor() and nothing else.
    ox = math.floor(minx / CELL_SIZE) * CELL_SIZE
    oy = math.floor(miny / CELL_SIZE) * CELL_SIZE
    gw = int(math.floor((maxx - ox) / CELL_SIZE)) + 1
    gh = int(math.floor((maxy - oy) / CELL_SIZE)) + 1

    buckets = [[] for _ in range(gw * gh)]
    for idx, box in enumerate(boxes):
        if box is None:
            continue
        bx0, bx1, by0, by1 = box
        cx0 = max(0, int(math.floor((bx0 - ox) / CELL_SIZE)))
        cx1 = min(gw - 1, int(math.floor((bx1 - ox) / CELL_SIZE)))
        cy0 = max(0, int(math.floor((by0 - oy) / CELL_SIZE)))
        cy1 = min(gh - 1, int(math.floor((by1 - oy) / CELL_SIZE)))
        for cy in range(cy0, cy1 + 1):
            row = cy * gw
            for cx in range(cx0, cx1 + 1):
                buckets[row + cx].append(idx)

    cells = bytearray()
    entries = bytearray()
    used = 0
    worst = 0
    for b in buckets:
        cells += struct.pack("<II", len(entries) // 4, len(b))
        for idx in b:
            entries += struct.pack("<I", idx)
        if b:
            used += 1
            worst = max(worst, len(b))

    # The bbox's z spans the unbounded walls, which is not a useful number to
    # hand the runtime, so report the real geometry's range instead.
    zlo, zhi = 1e30, -1e30
    for p in polys:
        if p[0x1A] & KIND_UNBOUNDED:
            continue
        zb = struct.unpack_from("<h", p, 0x18)[0]
        for k in range(4):
            z = zb + struct.unpack_from("<h", p, 0x10 + 2 * k)[0] / 256.0
            zlo = min(zlo, z)
            zhi = max(zhi, z)

    poly_blob = b"".join(polys)
    hdr = bytearray(NFST_HDR)
    hdr[0:4] = NFST_MAGIC
    struct.pack_into("<I", hdr, 0x04, NFST_VERSION)
    struct.pack_into("<I", hdr, 0x08, len(polys))
    struct.pack_into("<I", hdr, 0x0C, gw * gh)
    struct.pack_into("<I", hdr, 0x10, len(entries) // 4)
    struct.pack_into("<2f", hdr, 0x14, ox, oy)
    struct.pack_into("<f", hdr, 0x1C, CELL_SIZE)
    struct.pack_into("<II", hdr, 0x20, gw, gh)
    struct.pack_into("<3f", hdr, 0x28, minx, miny, zlo)
    struct.pack_into("<3f", hdr, 0x34, maxx, maxy, zhi)

    os.makedirs(args.out_dir, exist_ok=True)
    out_path = os.path.join(args.out_dir, "WORLD.NFST")
    tmp = out_path + ".tmp"
    raw = b"".join([
        bytes(hdr), poly_blob, cells, entries,
        struct.pack("<I12x", group_count), group_table,
        struct.pack("<IIII", len(tree_numbers), lod,
                    len(tree_cells) // 4, len(tree_entries) // 4),
        trees, tree_cells, tree_entries,
        struct.pack("<II8x", len(outlines), len(outline_pts) // 8),
        outline_recs, outline_pts])
    stored = raw if args.raw else nfst_lz4.wrap(raw)
    with open(tmp, "wb") as f:
        f.write(stored)
    os.replace(tmp, out_path)
    if not args.raw:
        print("stored              LZ4, %.2f MB of %.2f MB (%.0f%%)"
              % (len(stored) / 1048576.0, len(raw) / 1048576.0, 100.0 * len(stored) / len(raw)))

    total = (NFST_HDR + len(poly_blob) + len(cells) + len(entries) + 16 +
             len(group_table) + 16 + len(trees) + len(tree_cells) +
             len(tree_entries) + 16 + len(outline_recs) + len(outline_pts))
    print("trees               %d, %d cells, %d entries (%.2f MB)"
          % (len(tree_numbers), len(tree_cells) // 4, len(tree_entries) // 4,
             (len(trees) + len(tree_cells) + len(tree_entries)) / (1024.0 * 1024.0)))
    print("drivable outlines   %d, %d points%s%s"
          % (len(outlines), len(outline_pts) // 8,
             "" if not no_tree else "; no tree for " + ", ".join(no_tree),
             "" if not no_outline else "; no outline for tree " + ", ".join(no_outline)))
    print("polygons            %d (%d kept dead as degenerate)" % (len(polys), dropped))
    print("  wall      0x02    %d, of which %d are wallish by normal and %d are not"
          % (kinds["wall"], agree[0], agree[1]))
    print("  road              %d (%d stacked, height-gated)" % (kinds["road"], stacked))
    print("  not road  0x08    %d" % kinds["other"])
    print("  in a topology group %d, across %d groups%s"
          % (grouped, group_count - 1,
             "" if not unused else "; no polygons in " + ", ".join(unused)))
    print("terrain types       %s" % ", ".join(
        "%d:%d" % kv for kv in sorted(terrains.items(), key=lambda kv: -kv[1])))
    print("grid                %dx%d @ %.0f m from (%.0f, %.0f), %d/%d cells used, "
          "worst %d" % (gw, gh, CELL_SIZE, ox, oy, used, gw * gh, worst))
    print("entries             %d (%.1f per used cell)"
          % (len(entries) // 4, (len(entries) // 4) / float(used or 1)))
    print("bbox                x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]"
          % (minx, maxx, miny, maxy, zlo, zhi))
    print("wrote %s, %.2f MB" % (out_path, total / (1024.0 * 1024.0)))


if __name__ == "__main__":
    main()
