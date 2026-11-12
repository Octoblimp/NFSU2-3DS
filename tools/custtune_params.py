#!/usr/bin/env python3
"""Print GLOBALB.BUN's custom-tuning params table as the C rows custtune.c bakes.

The table is the 0x34A1A sub-chunk of the 0x80034A10 container in
GLOBAL/GLOBALB.BUN. LoaderMostWantedCareerChunk (Speed.elf 0x80123520) hands
it to CreateCustomTuningParamsTable (0x801C9D14), which byte-swaps it and keeps
the pointer and size / 0x18 in CustomPhysicsTuningParamsTable (r13-0x6944,
0x8046E49C) and its count (r13-0x6940). GLOBALB holds two such containers with
identical copies; the first one is printed, and the second is checked against
it.

Each 0x18-byte record (CustomTuningParams):
    +0x00 s32  ePhysicsPerformanceParts the row needs fitted
    +0x04 s8   its level (GetInstalledParams keeps the highest)
    +0x05 s8   ePhysicsTuningType
    +0x06 u8   range mode (AdjustForTuneableRange / GetTuneableRangeMinMax)
    +0x07      pad
    +0x08 f32  low  adjustment
    +0x0C f32  high adjustment
    +0x10 f32  display low  (-99.99 = none, GetTotalRangeMinMax)
    +0x14 f32  display high (-99.99 = none)

Usage: python tools/custtune_params.py [path/to/GLOBALB.BUN]
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUN = os.path.join(ROOT, 'PC Files', 'GLOBAL', 'GLOBALB.BUN')


def tables(data):
    out = []
    o = 0
    while o + 8 <= len(data):
        cid, sz = struct.unpack_from('<II', data, o)
        if cid == 0x80034A10:
            p = o + 8
            while p + 8 <= o + 8 + sz:
                sid, ss = struct.unpack_from('<II', data, p)
                if sid == 0x34A1A:
                    out.append(data[p + 8:p + 8 + ss])
                p += 8 + ss
        o += 8 + sz
    return out


def cfloat(x):
    # The shortest text that reads back as the same float32.
    for prec in range(6, 10):
        s = '%.*g' % (prec, x)
        if struct.unpack('<f', struct.pack('<f', float(s)))[0] == x:
            break
    if 'e' not in s and '.' not in s:
        s += '.0'
    if x == 0.0 and str(x).startswith('-'):
        s = '-0.0'
    return s + 'f'


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else BUN
    data = open(path, 'rb').read()
    found = tables(data)
    if not found:
        sys.exit('no 0x34A1A table in %s' % path)
    for k, t in enumerate(found[1:], 1):
        if t != found[0]:
            print('// WARNING: copy %d differs from the first' % k)
    d = found[0]
    n = len(d) // 0x18
    print('// %d rows, GLOBALB.BUN 0x34A1A' % n)
    for i in range(n):
        r = d[i * 0x18:(i + 1) * 0x18]
        part, = struct.unpack_from('<i', r, 0)
        lvl, typ, mode = struct.unpack_from('<bbB', r, 4)
        f = struct.unpack_from('<4f', r, 8)
        print('\t{ %2d, %d, %2d, %d, { %s } },' % (
            part, lvl, typ, mode, ', '.join(cfloat(x) for x in f)))


if __name__ == '__main__':
    main()
