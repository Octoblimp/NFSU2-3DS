#!/usr/bin/env python3
"""Decode `TRACKS/ROUTES<track>/Routes<id><F|B>.bin` -- retail's AI driving lines.

This is the data `AICarController` drives on: a set of `TrackRoute`s, each a
chain of `TrackRoutePoint`s carrying the road's centre line, its width, and one
lateral offset plus one target speed per driving line.

Where the layout comes from
---------------------------
`LoaderTrackRoutes__FP6bChunk` (Speed.elf 0x801FF230) dispatches the three
chunks a Routes file holds:

  0x34121  -> `TrackRouteManager::Loader` (0x801FD7B0), the routes themselves
  0x34122  -> a 0x20-byte record array, base = chunk+0xC, count = (size-4)/0x20
  0x34123  -> a 0x114-byte record array, base = chunk+8, count = size/0x114

Every field offset and width below is read off the GameCube loader's
endian-swap sequence, which has to name each field exactly; the PC files are
the same records left little-endian, which is why they can be read as-is.

`TrackRoute` is variable length: 0x8C of header then `numPoints` points of
0x38.  The loader's own stride proves it -- it advances by
`0xE08C - (0x400 - numPoints) * 0x38`, i.e. `0x8C + numPoints * 0x38`, and
`GetPreviousPoint__15TrackRoutePoint` (0x801F7D2C) divides
`(point - route - 0x8C)` by 0x38.

The point fields, with the accessor each was read from:

  +0x00 float x, +0x04 float y        bPlatEndianSwap(bVector2) at the point
  +0x08 u32                           swapped, not yet decoded
  +0x0C u32   bit word 0, via GetBits__15TrackRoutePoint (0x80302508):
                bits 0-10  route number     GetRoute          0x801F7C60
                bit  11    posted, racers forward             0x8030242C
                bit  18    posted, racers backward            0x80302458
                           (GetBits(0, 0x12, 1) -- not bit 12)
                bits 19-24 traffic intersection number        0x80302484
                bit  25    posted, traffic forward            0x803024B0
                bit  26    posted, traffic backward           0x803024DC
                bit  27    posted, players forward            0x80312BC0
                bit  28    posted, players backward           0x80312BEC
  +0x10 u16   length to the next point, /256 m  GetLength      0x80302278
  +0x12 u16                                  swapped, not decoded
  +0x14 u16   half width one side, /256 m     SetDrivingLineOffset 0x80312AF0
  +0x16 u16   half width the other side, /256 m
  +0x18 s16   forward direction x, /32767     GetForwardDirection 0x8030230C
  +0x1A s16   forward direction y, /32767
  +0x1C u16, +0x1E u16                        swapped, not decoded
  +0x20 u8                                    not swapped, not decoded
  +0x21 s8    slope, /128                     GetForwardSlopeFromRoutePoint
                                                                0x800440A8
  +0x22 s8 [10]  driving line offset, /128 of max(width14,width16) metres
  +0x2C u8 [10]  driving line speed            GetCurrentDrivingLineSpeed
                                                                0x800089D8
  +0x36, +0x37                                not swapped, not decoded

The two scale constants are the engine's own: 1/65536 (0x80315EE8, 0x80318CF8,
0x80339078) for the <<8 and <<16 encodings, and 1/32767 (0x80318D08) for a
direction.

Usage:
    python tools/routes_dump.py <Routes file> [--points N] [--route N]
    python tools/routes_dump.py <Routes file> --stats
"""
import os
import struct
import sys

ROUTE_HDR = 0x8C
POINT_SIZE = 0x38
NUM_DRIVING_LINES = 10


def chunks(data):
    """The flat {u32 type, u32 size} list a Routes file is."""
    off = 0
    while off + 8 <= len(data):
        typ, size = struct.unpack_from('<II', data, off)
        yield typ, off + 8, size
        off += 8 + size


class Point:
    __slots__ = ('x', 'y', 'w08', 'bits', 'length', 'w12', 'widthA', 'widthB',
                 'dirX', 'dirY', 'w1c', 'w1e', 'b20', 'slope', 'offsets',
                 'speeds', 'b36', 'b37')

    def __init__(self, b):
        (self.x, self.y, self.w08, self.bits, self.length, self.w12,
         self.widthA, self.widthB, self.dirX, self.dirY, self.w1c,
         self.w1e) = struct.unpack_from('<ffIIHHHHhhHH', b, 0)
        self.b20 = b[0x20]
        self.slope = struct.unpack_from('<b', b, 0x21)[0]
        self.offsets = struct.unpack_from('<10b', b, 0x22)
        self.speeds = struct.unpack_from('<10B', b, 0x2C)
        self.b36, self.b37 = b[0x36], b[0x37]

    # The accessors, in the engine's own units.
    @property
    def route(self):
        return self.bits & 0x7FF

    @property
    def posted_racers_fwd(self):
        return (self.bits >> 11) & 1

    @property
    def posted_racers_bwd(self):
        return (self.bits >> 18) & 1

    @property
    def intersection(self):
        return (self.bits >> 19) & 0x3F

    @property
    def posted_traffic_fwd(self):
        return (self.bits >> 25) & 1

    @property
    def posted_traffic_bwd(self):
        return (self.bits >> 26) & 1

    @property
    def posted_players_fwd(self):
        return (self.bits >> 27) & 1

    @property
    def posted_players_bwd(self):
        return (self.bits >> 28) & 1

    @property
    def length_m(self):
        return self.length / 256.0

    @property
    def width_m(self):
        return max(self.widthA, self.widthB) / 256.0

    @property
    def dir(self):
        return (self.dirX / 32767.0, self.dirY / 32767.0)

    @property
    def slope_f(self):
        return self.slope / 128.0

    def offset_m(self, line):
        return self.offsets[line] / 128.0 * self.width_m

    def speed(self, line):
        return float(self.speeds[line])


class Route:
    __slots__ = ('hdr', 'name', 'numPoints', 'points', 'u00', 'u04', 'u08',
                 'u0a', 'u0c', 'u30', 'u36', 'f38', 'f3c', 'f40', 'u54',
                 'u70', 'v60', 'v68', 'h44', 'h4c', 'h76', 'u78', 'h7e',
                 'u80')

    def __init__(self, b):
        self.hdr = b[:ROUTE_HDR]
        self.u00, self.u04 = struct.unpack_from('<II', b, 0)
        self.u08, self.u0a = struct.unpack_from('<HH', b, 8)
        self.u0c, = struct.unpack_from('<I', b, 0x0C)
        self.name = b[0x10:0x20].split(b'\0')[0].decode('latin1')
        self.u30, = struct.unpack_from('<I', b, 0x30)
        self.numPoints, self.u36 = struct.unpack_from('<hH', b, 0x34)
        self.f38, self.f3c, self.f40 = struct.unpack_from('<fff', b, 0x38)
        self.h44 = struct.unpack_from('<4H', b, 0x44)
        self.h4c = struct.unpack_from('<4H', b, 0x4C)
        self.u54, = struct.unpack_from('<I', b, 0x54)
        self.v60 = struct.unpack_from('<ff', b, 0x60)
        self.v68 = struct.unpack_from('<ff', b, 0x68)
        self.u70, = struct.unpack_from('<I', b, 0x70)
        self.h76, = struct.unpack_from('<H', b, 0x76)
        self.u78, = struct.unpack_from('<I', b, 0x78)
        self.h7e, = struct.unpack_from('<H', b, 0x7E)
        self.u80, = struct.unpack_from('<I', b, 0x80)
        self.points = [Point(b[ROUTE_HDR + i * POINT_SIZE:
                               ROUTE_HDR + (i + 1) * POINT_SIZE])
                       for i in range(max(0, self.numPoints))]

    @property
    def size(self):
        return ROUTE_HDR + max(0, self.numPoints) * POINT_SIZE


def load(path):
    data = open(path, 'rb').read()
    routes, pts34122, recs34123 = [], None, None
    for typ, base, size in chunks(data):
        if typ == 0x00034121:
            off = base
            end = base + size
            while off + ROUTE_HDR <= end:
                r = Route(data[off:off + ROUTE_HDR + 0x400 * POINT_SIZE])
                if r.numPoints < 0 or off + r.size > end:
                    break
                routes.append(r)
                off += r.size
        elif typ == 0x00034122:
            n = (size - 4) // 0x20
            pts34122 = [data[base + 4 + i * 0x20: base + 4 + (i + 1) * 0x20]
                        for i in range(n)]
        elif typ == 0x00034123:
            n = size // 0x114
            recs34123 = [data[base + i * 0x114: base + (i + 1) * 0x114]
                         for i in range(n)]
    return routes, pts34122, recs34123


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    path = argv[1]
    want_points = 0
    only = None
    stats = '--stats' in argv
    if '--points' in argv:
        want_points = int(argv[argv.index('--points') + 1])
    if '--route' in argv:
        only = int(argv[argv.index('--route') + 1])

    routes, p2, r3 = load(path)
    print('%s: %d routes, %s 0x34122 records, %s 0x34123 records'
          % (os.path.basename(path), len(routes),
             len(p2) if p2 else 0, len(r3) if r3 else 0))

    if stats:
        allspd, alloff, allw, alllen = [], [], [], []
        nlines = [0] * NUM_DRIVING_LINES
        for r in routes:
            for p in r.points:
                allw.append(p.width_m)
                alllen.append(p.length_m)
                for ln in range(NUM_DRIVING_LINES):
                    if p.speeds[ln]:
                        nlines[ln] += 1
                        allspd.append(p.speeds[ln])
                    alloff.append(p.offsets[ln])
        n = sum(len(r.points) for r in routes)
        print('points %d' % n)
        if allspd:
            print('speed byte  min %d max %d mean %.1f'
                  % (min(allspd), max(allspd), sum(allspd) / len(allspd)))
        print('width m     min %.2f max %.2f mean %.2f'
              % (min(allw), max(allw), sum(allw) / len(allw)))
        print('seg len m   min %.2f max %.2f mean %.2f'
              % (min(alllen), max(alllen), sum(alllen) / len(alllen)))
        print('offset byte min %d max %d' % (min(alloff), max(alloff)))
        print('points with a nonzero speed, per driving line: %s' % (nlines,))
        return 0

    for i, r in enumerate(routes):
        if only is not None and i != only:
            continue
        print('\n--- route %d "%s" %d points (%d B)' %
              (i, r.name, r.numPoints, r.size))
        print('    u00=%d u04=%d u08=%d u0a=%d u0c=%08x u30=%08x u36=%d'
              % (r.u00, r.u04, r.u08, r.u0a, r.u0c, r.u30, r.u36))
        print('    f38=%.2f f3c=%.2f f40=%.2f u54=%08x u70=%08x'
              % (r.f38, r.f3c, r.f40, r.u54, r.u70))
        print('    h44=%s h4c=%s v60=(%.1f,%.1f) v68=(%.1f,%.1f)'
              % (r.h44, r.h4c, r.v60[0], r.v60[1], r.v68[0], r.v68[1]))
        print('    h76=%d u78=%08x h7e=%d u80=%08x'
              % (r.h76, r.u78, r.h7e, r.u80))
        for j, p in enumerate(r.points[:want_points]):
            d = p.dir
            print('    [%3d] (%8.2f,%8.2f) rt=%-4d len=%6.2f w=%5.2f/%5.2f '
                  'dir=(%6.3f,%6.3f) slope=%6.3f post=%d%d/%d%d int=%d '
                  'u08=%08x h12=%d h1c=%d h1e=%d b20=%d b36=%d b37=%d'
                  % (j, p.x, p.y, p.route, p.length_m,
                     p.widthA / 256.0, p.widthB / 256.0, d[0], d[1],
                     p.slope_f, p.posted_racers_fwd, p.posted_racers_bwd,
                     p.posted_players_fwd, p.posted_players_bwd,
                     p.intersection, p.w08, p.w12, p.w1c, p.w1e, p.b20,
                     p.b36, p.b37))
            print('          off=%s spd=%s' % (list(p.offsets),
                                               list(p.speeds)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
