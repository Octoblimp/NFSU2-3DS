#!/usr/bin/env python3
"""Dump retail's topology group table (chunk 0x0003412F) for a track.

TASKS.md #232. Every TopologyTree polygon carries a group index in its byte +0
(tools/topo_pack.py). 0 is "always there"; any other value indexes a 32-byte
record of the table `TopologyManager::Loader` (Speed.elf 0x801f35bc) keeps at
TheTopologyManager+0x82c, i.e. *(0x803EFD8C):

    +0x00 u32  bStringHash of the group's name    (the loader byte-swaps it)
    +0x04 u32  how many polygons carry the index -- exact for 117 of L4RA's
               125 groups, near it for the rest (byte-swapped too)
    +0x08 u8   enabled; BuildTrackPolygons (0x801f2d40) and NewGetElevation
               (0x801f1484) both drop a polygon whose group has this at 0.
               The file has 1 everywhere; TopologyManager::Init (0x801f33fc)
               clears all but record 0 before anything reads it
    +0x09      the name, NUL-terminated ("NONE" in record 0)

`GetGroupInfo` (0x801f39e8) searches from record 1, so record 0 is never a
group. `TrackLoader::InitTopologyAndSceneryGroups` (0x80196cb8) is what sets
+0x08 at track load; see topology.c's topo_set_groups().

Each line checks the stored name against the stored hash, which is the test
topology.c relies on when it looks a group up by the name retail would build.

Usage: python tools/topo_groups.py "PC Files/TRACKS" [--track L4RA]
"""
import argparse
import os
import struct

GROUP_TABLE_TAG = 0x0003412F


def bstring_hash(s):
    """bStringHash (Speed.elf 0x8005dbe0): h = h*33 + c from 0xFFFFFFFF."""
    h = 0xFFFFFFFF
    for c in s.encode("ascii"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def walk(blob, want, off=0, end=None, depth=0, out=None):
    """Every chunk tagged `want`, at any depth, as (payload offset, size)."""
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tracks_dir")
    ap.add_argument("--track", default="L4RA")
    args = ap.parse_args()
    for fn in ("%s.BUN" % args.track, "STREAM%s.BUN" % args.track):
        p = os.path.join(args.tracks_dir, fn)
        if not os.path.exists(p):
            continue
        blob = open(p, "rb").read()
        for off, size in walk(blob, GROUP_TABLE_TAG):
            # The payload is 16-byte aligned behind 0x11 filler, like the
            # other topology leaves; a record is 32 bytes.
            start = off
            while start < off + size and blob[start] == 0x11:
                start += 1
            n = (off + size - start) // 32
            print("%s: group table at 0x%x, %d records" % (fn, start, n))
            for i in range(n):
                r = start + i * 32
                h, w1 = struct.unpack_from("<II", blob, r)
                name = blob[r + 9:r + 32].split(b"\0")[0].decode("ascii")
                match = "ok" if bstring_hash(name) == h or i == 0 else "HASH MISMATCH"
                print("  [%3d] hash %08x  polys %5d  +8 %d  %-22s %s" % (
                    i, h, w1, blob[r + 8], name, match))


if __name__ == "__main__":
    main()
