"""Packs a track's event-manager triggers for the 3DS port (TASKS.md #298).

    python tools/emevents_pack.py "PC Files" <GameDir> [--track L4RA] [-v]

writes <GameDir>/TRACKS/<track>/WORLD.NFSE from PC Files/TRACKS/<track>.BUN
(the section table) and STREAM<track>.BUN (the sections).

What it is: the spheres a car sets off as it drives -- in L4RA every one is a
sound sphere (EVENT_ID 0x10007) round a lamp post, a pillar or a tree, which
EAXTunerCar::ProcessSoundSphere (Speed.elf 0x8007F404) turns into a woosh.
Each streamed section carries one chunk 0x80036000 holding its section's
triggers and an AABB tree over them. LoaderEventManager (0x8020D284) hangs it
on the section's VisibleSectionManager user info when the section loads;
emTriggerEventsInSection (0x8020DB34) asks the tree of the section the car's
topology is in for the leaf holding the point, and sets off every trigger of
that leaf the point is inside. The GC loader byte-swaps the PC's
little-endian chunk, so the PC data is the GameCube's.

Pack, as STREAML4RA.BUN has it (each child payload starts on the next 16-byte
boundary after its chunk header, the gap filled with 0x11):
    0x36001 header: +0x08 u32 kind (2), +0x0c u32 section number
                    (letter * 100 + n, the section that carries it),
                    +0x10 u32 trigger count; +0x00/+0x04 the list node and
                    +0x14..+0x1c the loader's flag and pointers, 0 on disk
    0x36002 vAABBTree, as the light packs' (lightdb_pack.py): 0x30-byte nodes
                    from +0x10, a leaf's indices naming triggers
    0x36003 trigger[count], 0x20 bytes:
                    +0x00 u32 id (ProcessSoundSphere is handed it, reads
                    nothing of it)  +0x04 u32 EVENT_ID  +0x08 u32 name hash
                    (bStringHash: LAMPPOST, PILLAR, TREE, ...)  +0x0c u32 (0)
                    +0x10 f32 pos[3]  +0x1c f32 radius

WORLD.NFSE, little-endian:
    0x00 char magic[4] "NFSE"   0x04 u32 version = 1
    0x08 u32 packs  0x0c u32 nodes  0x10 u32 events
    0x14 u32 packsOffset  0x18 u32 nodesOffset  0x1c u32 eventsOffset
    Pack[packs], 16 bytes, in STREAM file order:
        u16 section; u16 nodes; u32 firstNode; u32 firstEvent; u32 events
    Node[nodes]: the retail 0x30-byte node as it is (indices pack-relative)
    Event[events], 24 bytes -- what TriggerEvents and the trigger test read:
        u32 eventId; u32 nameHash; f32 pos[3]; f32 radius
"""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import track_pack  # noqa: E402

PACK_TAG = 0x80036000
HEADER_TAG = 0x36001
TREE_TAG = 0x36002
EVENTS_TAG = 0x36003
KIND_SECTION = 2
NODE = 0x30
EVENT = 0x20
VERSION = 1

NAMES = ["INTERSECTION", "LAMPPOST", "PILLAR", "TRAFFIC_LIGHT", "FREEWAY_SIGN", "TREE",
         "TUNNEL_ENTRY"]


def payload(blob, base, tag, kids):
    """A child's payload with the 0x11 alignment run in front of it taken off."""
    if tag not in kids:
        raise SystemExit("event pack at 0x%x has no 0x%x child" % (base, tag))
    off, size = kids[tag]
    pad = 0
    while pad < size and pad < 16 and (off + pad) % 16:
        if blob[off + pad] != 0x11:
            raise SystemExit("event pack at 0x%x: 0x%x child padding is 0x%02x"
                             % (base, tag, blob[off + pad]))
        pad += 1
    return off + pad, size - pad


def read_packs(stream_path, sections):
    """Every event pack with triggers in the track's sections, in file order."""
    packs = []
    with open(stream_path, "rb") as f:
        for sec in sections:
            # Aligned to 16 relative to the section, as the game loads it.
            blob = track_pack.read_section(f, sec)

            def walk(o, end, depth):
                while o + 8 <= end:
                    tag, size = struct.unpack_from("<II", blob, o)
                    if o + 8 + size > end:
                        return
                    if tag == PACK_TAG:
                        kids = {}
                        q = o + 8
                        while q + 8 <= o + 8 + size:
                            kt, ks = struct.unpack_from("<II", blob, q)
                            kids[kt] = (q + 8, ks)
                            q += 8 + ks
                        p = parse_pack(blob, o, kids, sec.name)
                        if p:
                            packs.append(p)
                    elif tag & 0x80000000 and depth < 4:
                        walk(o + 8, o + 8 + size, depth + 1)
                    o += 8 + size
            walk(0, len(blob), 0)
    return packs


def parse_pack(blob, base, kids, name):
    ho, hs = payload(blob, base, HEADER_TAG, kids)
    if hs < 0x20:
        raise SystemExit("%s: event pack header is %d bytes" % (name, hs))
    kind, section, count = struct.unpack_from("<3I", blob, ho + 0x08)
    # LoaderEventManager takes only kind 2, and leaves a pack without
    # triggers, a tree or a list off the list it queries.
    if kind != KIND_SECTION or not count or TREE_TAG not in kids or EVENTS_TAG not in kids:
        return None

    to, ts = payload(blob, base, TREE_TAG, kids)
    nodes = struct.unpack_from("<H", blob, to + 0x08)[0]
    if ts < 0x10 + nodes * NODE:
        raise SystemExit("%s: tree of %d nodes in %d bytes" % (name, nodes, ts))
    tree = []
    for i in range(nodes):
        n = blob[to + 0x10 + i * NODE:to + 0x10 + (i + 1) * NODE]
        cnt = struct.unpack_from("<h", n, 0x0e)[0]
        idx = struct.unpack_from("<10h", n, 0x1c)
        k = abs(cnt)
        if k > 10:
            raise SystemExit("%s: node %d lists %d" % (name, i, k))
        lim = count if cnt <= 0 else nodes
        if any(not 0 <= j < lim for j in idx[:k]):
            raise SystemExit("%s: node %d index out of range" % (name, i))
        tree.append(bytes(n))
    if not tree:
        return None

    eo, es = payload(blob, base, EVENTS_TAG, kids)
    if es < count * EVENT:
        raise SystemExit("%s: %d triggers in %d bytes" % (name, count, es))
    events = []
    for i in range(count):
        r = struct.unpack_from("<4I4f", blob, eo + i * EVENT)
        events.append(dict(event=r[1], hash=r[2], pos=r[4:7], radius=r[7]))
    return dict(section=section, tree=tree, events=events, name=name)


def pack(packs):
    nodes = sum(len(p["tree"]) for p in packs)
    events = sum(len(p["events"]) for p in packs)
    po = 0x20
    no = po + 16 * len(packs)
    eo = no + NODE * nodes
    out = bytearray(b"NFSE" + struct.pack("<7I", VERSION, len(packs), nodes, events,
                                          po, no, eo))
    fn = fe = 0
    for p in packs:
        out += struct.pack("<HHIII", p["section"], len(p["tree"]), fn, fe,
                           len(p["events"]))
        fn += len(p["tree"])
        fe += len(p["events"])
    for p in packs:
        for n in p["tree"]:
            out += n
    for p in packs:
        for e in p["events"]:
            out += struct.pack("<II4f", e["event"], e["hash"], *e["pos"], e["radius"])
    assert len(out) == eo + 24 * events
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pc_root")
    ap.add_argument("out", help="the game data root to write TRACKS/<track>/ under")
    ap.add_argument("--track", default="L4RA")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    tracks = os.path.join(args.pc_root, "TRACKS")
    sections = track_pack.read_section_table(os.path.join(tracks, args.track + ".BUN"))
    packs = read_packs(os.path.join(tracks, "STREAM" + args.track + ".BUN"), sections)
    if not packs:
        raise SystemExit("no event packs in STREAM%s.BUN" % args.track)
    blob = pack(packs)
    dst = os.path.join(args.out, "TRACKS", args.track, "WORLD.NFSE")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".tmp", "wb") as f:
        f.write(blob)
    os.replace(dst + ".tmp", dst)
    ne = sum(len(p["events"]) for p in packs)
    print("%s: %d bytes, %d packs, %d nodes, %d triggers" % (
        dst, len(blob), len(packs), sum(len(p["tree"]) for p in packs), ne))
    if args.verbose:
        names = {track_pack.bstring_hash(n): n for n in NAMES}
        kinds = collections.Counter()
        for p in packs:
            for e in p["events"]:
                kinds[(e["event"], names.get(e["hash"], "%08x" % e["hash"]), e["radius"])] += 1
        for (eid, nm, r), c in kinds.most_common():
            print("  event 0x%x %-14s radius %g: %d" % (eid, nm, r, c))
    return 0


if __name__ == "__main__":
    sys.exit(main())
