"""Packs a track's world light flares for the 3DS port (TASKS.md #323).

    python tools/flare_pack.py "PC Files" <GameDir> [--track L4RA] [-v]

writes <GameDir>/TRACKS/<track>/WORLD.NFSF from PC Files/TRACKS/<track>.BUN
(the section table) and STREAM<track>.BUN (the sections).

What it is: the glow on the street lamps, signs and buildings. Each streamed
section that has any carries one chunk 0x80135100 -- a light flare pack. When
the section loads, its header becomes the section's flare list
(DAT_803f50b0[section] +8 in Speed.elf), and eRenderWorldLightFlares
(0x800cd1bc) draws, for every section on the visible list of the drivable
section under the eye, the flares of a loaded one whose box is in view, each
through eRenderLightFlare (0x800cbfcc). The GC loader byte-swaps the PC's
little-endian chunk, so the PC data is the GameCube's.

Pack, as STREAML4RA.BUN has it (a child payload that does not start on a
16-byte boundary is padded to one with 0x11):
    0x135101 header, 0x60 bytes: +0x00 u32[3] (3, 3, 3), +0x30 f32 box min[3],
                     +0x40 f32 box max[3], +0x50 u32 flare count, +0x54 u32
                     section number (letter * 100 + n), +0x58 the list head
                     (a bTList: two pointers, rebuilt on load)
    0x135102 eLightFlare[count], 0x30 bytes:
                     +0x00 next, +0x04 prev (the bTNode, rebuilt on load)
                     +0x08 u32 nameHash  +0x0c u32 colour (unread: the flare
                     takes its colour from its parameter row)  +0x10 f32 pos[3]
                     +0x1c f32 the reflection's z, 999 until eRenderLightFlare
                     first works it out  +0x20 f32 dir[3]  +0x2c u8 type
                     (LightFlareParameterIndicies' row pair)  +0x2d u8 flags
                     (1 two-sided, 4 faded by facing)  +0x2e u16 section

The props' own lamps go a second way, through the light flare pool. A
scenery model (eSolid) may carry ePositionMarkers -- one 0x13401a chunk
after its 0x134011 record, 0x50 bytes each: +0x00 u32 nameHash, +0x04 u32
type, +0x10 f32 m[16], the marker's place in the model's own space, so
m[12..14] less the record's translation (+0x40 + 48, the eSolid's +0x70
pivot) is where it sits on the model. DrawAScenery (0x80208fa4) takes, for
every instance the player's view (view 1 or 2) draws, each marker of the
model it draws -- the A or the B -- out of the pool (eGetNextLightFlareInPool
0x800cbc58: two pools of 200, the second for type 3) as a flare of type
marker type + 10 at the instance matrix times (marker - pivot), with flags 4
and its direction the instance's centre out to the marker across the ground
for a type 3, flags 2 otherwise. eRenderLightFlarePool (0x800cbdb0) draws
them after the world's flares. In L4RA they are the freeway guardrails' and
the cat's eyes' amber reflectors (type 0), the post boots' red ones (1) and
the median poles' blinking lights (3); none is in a scenery group.

WORLD.NFSF, little-endian:
    0x00 char magic[4] "NFSF"   0x04 u32 version = 2
    0x08 u32 packs  0x0c u32 flares  0x10 u32 packsOffset  0x14 u32 flaresOffset
    0x18 u32 insts  0x1c u32 marks   0x20 u32 instsOffset  0x24 u32 marksOffset
    0x28 u32[2] 0
    Pack[packs], 32 bytes, in STREAM file order:
        u16 section; u16 count; u32 firstFlare; f32 min[3]; f32 max[3]
    Flare[flares], 32 bytes:
        f32 pos[3]; f32 dir[3]; u8 type; u8 flags; u16 section; u32 nameHash
    Inst[insts], 20 bytes, by section: a scenery instance with markers
        f32 pos[3] (its translation, which finds it in the section's
        instance table); u16 section; u8 nA; u8 nB; u32 firstMark -- nA
        marks for its A model, then nB for its B
    Mark[marks], 16 bytes: f32 pos[3] (world); u8 type (the marker's); u8[3] 0
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import car_pack  # noqa: E402
import lightdb_pack  # noqa: E402
import track_pack  # noqa: E402

PACK_TAG = 0x80135100
HEADER_TAG = 0x135101
FLARES_TAG = 0x135102
HEADER = 0x60
FLARE = 0x30
VERSION = 2
HDR = 0x30

PART_TAG = 0x134011
MARKER_TAG = 0x13401a
MARKER = 0x50


def read_markers(blob, markers):
    """Every model in `blob` that carries ePositionMarkers, into `markers`:
    model hash -> [(type, (x, y, z) less the model's pivot)]. The first copy
    of a model wins; a shared one is the same wherever it is stored."""
    cur = None
    for tag, payload, size in car_pack.walk_chunks(blob):
        if tag == PART_TAG:
            rec = car_pack.leaf_start(blob, payload, size, car_pack.PART_RECORD_SIZE)
            cur = (struct.unpack_from("<I", blob, rec + car_pack.PART_RECORD_HASH_OFF)[0],
                   struct.unpack_from("<3f", blob, rec + car_pack.PART_RECORD_MATRIX_OFF + 48))
        elif tag == MARKER_TAG and cur is not None and cur[0] not in markers:
            s = car_pack.leaf_start(blob, payload, size, MARKER)
            out = []
            for i in range((size - (s - payload)) // MARKER):
                o = s + i * MARKER
                mtype = struct.unpack_from("<I", blob, o + 4)[0]
                m = struct.unpack_from("<16f", blob, o + 0x10)
                out.append((mtype, tuple(m[12 + k] - cur[1][k] for k in range(3))))
            markers[cur[0]] = out


def read_packs(stream_path, sections, markers=None, scenery=None):
    """Every flare pack in the track's sections, in file order. With
    `markers` and `scenery` (lists), also every model's markers and every
    section's (section, scenery groups)."""
    packs = []
    with open(stream_path, "rb") as f:
        for sec in sections:
            blob = track_pack.read_section(f, sec)
            if markers is not None:
                read_markers(blob, markers)
            if scenery is not None:
                scenery.append((sec, track_pack.read_scenery_groups(blob)))

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
    ho, hs = lightdb_pack.payload(blob, base, HEADER_TAG, kids)
    if hs < HEADER:
        raise SystemExit("%s: flare pack header is %d bytes" % (name, hs))
    lo = struct.unpack_from("<3f", blob, ho + 0x30)
    hi = struct.unpack_from("<3f", blob, ho + 0x40)
    count, section = struct.unpack_from("<II", blob, ho + 0x50)
    fo, fs = lightdb_pack.payload(blob, base, FLARES_TAG, kids)
    if fs < count * FLARE:
        raise SystemExit("%s: %d flares in %d bytes" % (name, count, fs))
    flares = []
    for i in range(count):
        r = blob[fo + i * FLARE:fo + (i + 1) * FLARE]
        fl = dict(hash=struct.unpack_from("<I", r, 0x08)[0],
                  pos=struct.unpack_from("<3f", r, 0x10),
                  dir=struct.unpack_from("<3f", r, 0x20),
                  type=r[0x2c], flags=r[0x2d],
                  section=struct.unpack_from("<H", r, 0x2e)[0])
        if fl["section"] != section:
            raise SystemExit("%s: flare %d is section %d's, the pack %d's"
                             % (name, i, fl["section"], section))
        flares.append(fl)
    return dict(section=section, lo=lo, hi=hi, flares=flares, name=name)


def place_markers(scenery, markers, groups):
    """[(section, pos, [A marks], [B marks])] for every scenery instance the
    player's view can draw whose A or B model carries markers, by section.
    A mark is (type, world position): the instance's matrix -- row-vector,
    rows its three column-major i16 columns, as DrawAScenery builds it --
    times the marker less the model's pivot, plus its translation."""
    out = []
    for sec, sgroups in scenery:
        for infos, instances in sgroups:
            for inst in instances:
                info = infos[inst["info"]]
                a, b = info["lods"][0], info["lods"][1]
                if a not in markers and b not in markers:
                    continue
                # The never-drawn ones (the traffic lights, 0x02) and the
                # identity-matrix 0x100 path, which takes no flares.
                if groups.set_for(inst, info) is None:
                    continue
                if groups.flags_for(inst) & track_pack.INST_FLAG_NO_MATRIX:
                    continue
                r, t = inst["rot"], inst["pos"]

                def place(h):
                    return [(mt, tuple(x * r[k] + y * r[3 + k] + z * r[6 + k] + t[k]
                                       for k in range(3)))
                            for mt, (x, y, z) in markers.get(h, [])]
                out.append((sec.number, tuple(t), place(a), place(b)))
    out.sort(key=lambda e: e[0])
    return out


def pack(packs, insts):
    flares = sum(len(p["flares"]) for p in packs)
    marks = sum(len(e[2]) + len(e[3]) for e in insts)
    po = HDR
    fo = po + 32 * len(packs)
    io = fo + 32 * flares
    mo = io + 20 * len(insts)
    out = bytearray(b"NFSF" + struct.pack("<9I8x", VERSION, len(packs), flares, po, fo,
                                          len(insts), marks, io, mo))
    first = 0
    for p in packs:
        out += struct.pack("<HHI3f3f", p["section"], len(p["flares"]), first,
                           *p["lo"], *p["hi"])
        first += len(p["flares"])
    for p in packs:
        for fl in p["flares"]:
            out += struct.pack("<3f3fBBHI", *fl["pos"], *fl["dir"], fl["type"],
                               fl["flags"], fl["section"], fl["hash"])
    assert len(out) == io
    first = 0
    for sec, pos, ma, mb in insts:
        if len(ma) > 255 or len(mb) > 255:
            raise SystemExit("section %d: an instance with %d + %d markers" % (sec, len(ma), len(mb)))
        out += struct.pack("<3fHBBI", *pos, sec, len(ma), len(mb), first)
        first += len(ma) + len(mb)
    for _sec, _pos, ma, mb in insts:
        for mt, wp in ma + mb:
            out += struct.pack("<3fB3x", *wp, mt)
    assert len(out) == mo + 16 * marks
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pc_root")
    ap.add_argument("out", help="the game data root to write TRACKS/<track>/ under")
    ap.add_argument("--track", default="L4RA")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    tracks = os.path.join(args.pc_root, "TRACKS")
    bun = os.path.join(tracks, args.track + ".BUN")
    sections = track_pack.read_section_table(bun)
    markers, scenery = {}, []
    packs = read_packs(os.path.join(tracks, "STREAM" + args.track + ".BUN"), sections,
                       markers, scenery)
    if not packs:
        raise SystemExit("no flare packs in STREAM%s.BUN" % args.track)
    insts = place_markers(scenery, markers, track_pack.SceneryGroups(bun))
    blob = pack(packs, insts)
    dst = os.path.join(args.out, "TRACKS", args.track, "WORLD.NFSF")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".tmp", "wb") as f:
        f.write(blob)
    os.replace(dst + ".tmp", dst)
    n = sum(len(p["flares"]) for p in packs)
    nm = sum(len(e[2]) + len(e[3]) for e in insts)
    print("%s: %d bytes, %d packs, %d flares; %d marked models placed %d times, %d marks"
          % (dst, len(blob), len(packs), n, len(markers), len(insts), nm))
    if args.verbose:
        kinds = {}
        for p in packs:
            for fl in p["flares"]:
                k = (fl["type"], fl["flags"])
                kinds[k] = kinds.get(k, 0) + 1
        for k, v in sorted(kinds.items()):
            print("  type %d flags %d: %d" % (k[0], k[1], v))
        mk = {}
        for e in insts:
            for mt, _wp in e[2] + e[3]:
                mk[mt] = mk.get(mt, 0) + 1
        for k, v in sorted(mk.items()):
            print("  marker type %d (flare type %d): %d" % (k, k + 10, v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
