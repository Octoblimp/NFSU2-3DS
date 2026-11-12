"""Packs a track's light database for the 3DS port (TASKS.md #166).

    python tools/lightdb_pack.py "PC Files" <GameDir> [--track L4RA] [-v]

writes <GameDir>/TRACKS/<track>/WORLD.NFSL from PC Files/TRACKS/<track>.BUN
(the section table) and STREAM<track>.BUN (the sections).

What it is: the street lamps that light a car. Each streamed section carries
one chunk 0x80135000 -- a light pack -- holding its section's eLights and an
AABB tree over them. LoaderLights (Speed.elf 0x800c8700) links a pack into
LightPackList when its section loads; QueryLightDatabase (0x800c9ee4) asks
every linked pack's tree for the leaf holding the car, and elSetupLights
(0x800cb290) folds what it finds into the car's harmonics beside the shaper
rig. The GC loader byte-swaps the PC's little-endian chunk, so the PC data is
the GameCube's.

Pack, as STREAML4RA.BUN has it (each child payload starts on the next 16-byte
boundary after its chunk header, the gap filled with 0x11):
    0x135001 header: +0x08 u16 version (4), +0x0c u32 section number
                     (letter * 100 + n), +0x1c u32 light count
    0x135002 vAABBTree: +0x04 u16 leaves, +0x06 u16 parents, +0x08 u16 nodes,
                     +0x0a u16 depth, then 0x30-byte nodes from +0x10:
                     +0x00 f32 centre[3], +0x0c s16 parent, +0x0e s16 count
                     (children; a leaf has -lights), +0x10 f32 half[3],
                     +0x1c s16 index[10] (child nodes, or a leaf's lights)
    0x135003 eLight[count], 0x60 bytes:
                     +0x00 u32 nameHash  +0x04 u8 type (0 ambient, 2 omni,
                     3 spot), +0x05/+0x06 u8, +0x07 u8 state (1 = on)
                     +0x0c u32 colour (R, G, B, A bytes)  +0x10 f32 pos[3]
                     +0x1c f32 radius  +0x20 f32 dir[3]  +0x2c f32 intensity
                     +0x30 f32 inner radius  +0x34 f32 cone  +0x3e name

WORLD.NFSL, little-endian:
    0x00 char magic[4] "NFLD"   0x04 u32 version = 1
    0x08 u32 packs  0x0c u32 nodes  0x10 u32 lights
    0x14 u32 packsOffset  0x18 u32 nodesOffset  0x1c u32 lightsOffset
    Pack[packs], 16 bytes, in STREAM file order:
        u16 section; u16 nodes; u32 firstNode; u32 firstLight; u32 lights
    Node[nodes]: the retail 0x30-byte node as it is (indices pack-relative)
    Light[lights], 48 bytes -- the eLight fields the query and elSetupLights
    read, name dropped:
        u32 nameHash; u8 type, b5, b6, state; u32 colour; f32 pos[3];
        f32 radius; f32 dir[3]; f32 inner; f32 cone
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import track_pack  # noqa: E402

PACK_TAG = 0x80135000
HEADER_TAG = 0x135001
TREE_TAG = 0x135002
LIGHTS_TAG = 0x135003
LIGHT_VERSION = 4
NODE = 0x30
LIGHT = 0x60
VERSION = 1


def payload(blob, base, tag, kids):
    """A child's payload with the 0x11 alignment run in front of it taken off."""
    if tag not in kids:
        raise SystemExit("light pack at 0x%x has no 0x%x child" % (base, tag))
    off, size = kids[tag]
    pad = 0
    while pad < size and pad < 16 and (off + pad) % 16:
        if blob[off + pad] != 0x11:
            raise SystemExit("light pack at 0x%x: 0x%x child padding is 0x%02x"
                             % (base, tag, blob[off + pad]))
        pad += 1
    return off + pad, size - pad


def read_packs(stream_path, sections):
    """Every light pack in the track's sections, in file order."""
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
                        packs.append(parse_pack(blob, o, kids, sec.name))
                    elif tag & 0x80000000 and depth < 4:
                        walk(o + 8, o + 8 + size, depth + 1)
                    o += 8 + size
            walk(0, len(blob), 0)
    return packs


def parse_pack(blob, base, kids, name):
    ho, hs = payload(blob, base, HEADER_TAG, kids)
    if hs < 0x20:
        raise SystemExit("%s: light pack header is %d bytes" % (name, hs))
    version = struct.unpack_from("<H", blob, ho + 0x08)[0]
    section = struct.unpack_from("<I", blob, ho + 0x0c)[0]
    count = struct.unpack_from("<I", blob, ho + 0x1c)[0]
    if version != LIGHT_VERSION:
        raise SystemExit("%s: light pack version %d" % (name, version))

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

    lo, ls = payload(blob, base, LIGHTS_TAG, kids)
    if ls < count * LIGHT:
        raise SystemExit("%s: %d lights in %d bytes" % (name, count, ls))
    lights = []
    for i in range(count):
        r = blob[lo + i * LIGHT:lo + (i + 1) * LIGHT]
        lights.append(dict(
            hash=struct.unpack_from("<I", r, 0x00)[0],
            bytes=r[4:8],
            colour=struct.unpack_from("<I", r, 0x0c)[0],
            pos=struct.unpack_from("<3f", r, 0x10),
            radius=struct.unpack_from("<f", r, 0x1c)[0],
            dir=struct.unpack_from("<3f", r, 0x20),
            inner=struct.unpack_from("<f", r, 0x30)[0],
            cone=struct.unpack_from("<f", r, 0x34)[0],
            name=r[0x3e:].split(b"\0")[0].decode("latin-1")))
    return dict(section=section, tree=tree, lights=lights, name=name)


def pack(packs):
    nodes = sum(len(p["tree"]) for p in packs)
    lights = sum(len(p["lights"]) for p in packs)
    po = 0x20
    no = po + 16 * len(packs)
    lo = no + NODE * nodes
    out = bytearray(b"NFLD" + struct.pack("<7I", VERSION, len(packs), nodes, lights,
                                          po, no, lo))
    fn = fl = 0
    for p in packs:
        out += struct.pack("<HHIII", p["section"], len(p["tree"]), fn, fl,
                           len(p["lights"]))
        fn += len(p["tree"])
        fl += len(p["lights"])
    for p in packs:
        for n in p["tree"]:
            out += n
    for p in packs:
        for L in p["lights"]:
            out += struct.pack("<I4sI3ff3fff", L["hash"], L["bytes"], L["colour"],
                               *L["pos"], L["radius"], *L["dir"], L["inner"],
                               L["cone"])
    assert len(out) == lo + 48 * lights
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
        raise SystemExit("no light packs in STREAM%s.BUN" % args.track)
    blob = pack(packs)
    dst = os.path.join(args.out, "TRACKS", args.track, "WORLD.NFSL")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".tmp", "wb") as f:
        f.write(blob)
    os.replace(dst + ".tmp", dst)
    nl = sum(len(p["lights"]) for p in packs)
    print("%s: %d bytes, %d packs, %d nodes, %d lights" % (
        dst, len(blob), len(packs), sum(len(p["tree"]) for p in packs), nl))
    if args.verbose:
        kinds = {}
        for p in packs:
            for L in p["lights"]:
                k = (L["bytes"][0], L["bytes"][3], L["radius"], L["inner"],
                     L["colour"] & 0xffffff)
                kinds.setdefault(k, []).append((p["name"], L["name"]))
        for k, v in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
            print("  type %d state %d R %g ri %g rgb %06x: %d, e.g. %s %s" % (
                k[0], k[1], k[2], k[3], k[4], len(v), v[0][0], v[0][1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
