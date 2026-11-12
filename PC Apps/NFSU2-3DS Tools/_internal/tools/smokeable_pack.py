"""Packs a track's smokeable props for the 3DS port (TASKS.md #318).

    python tools/smokeable_pack.py "PC Files" <GameDir> [--track L4RA] [-v]

writes <GameDir>/TRACKS/<track>/WORLD.NFSK from PC Files/TRACKS/<track>.BUN
(the SmokeableInfo table, the GJK collision volumes, the section table) and
STREAM<track>.BUN (the sections' spawner packs).

What it is: the props a car knocks over -- the highway's crash barrels
(ZCS_Barrel_A, 227 of them), street signs, garbage cans, the orange bumpers,
water barrels. Retail draws each as an ordinary scenery instance and puts a
VirginSmokeable on it, a static collision box (Speed.elf 0x801D2BF0). A car
touching the box converts it (ConvertToSmokeable, 0x801D2ED8) into a Smokeable,
a RigidBody of the info's mass and inertia that the car's contacts push
(Contact::CollisionImpulse 0x801B24F4, Smokeable::ConsiderImpulse 0x801D47C8),
and hides the scenery instance for good.

SmokeableInfo, chunk 0x34026 in <track>.BUN, 0x100-byte records from the next
16-byte boundary (MemoryImageLoad__13SmokeableInfo 0x801D1790):
    +0x08 char name[24]            (the info's hash is bStringHash(name), case kept)
    +0x20 RigidBodyInfo: +0x00 f32 mass (tonnes, as the cars' CPI_MASS),
          +0x10 bMatrix4 inertia, +0x50 f32 the scale its [2][2] takes
          (RigidBody::CalculateInertiaTensor 0x801CD058)
    +0x80 u32 GJK polytope hash    +0x84/+0x88 emitter keys  +0x8c piece count
    +0xb0 f32 centre of gravity[3] +0xc0 f32 lifetime (s, 0 = for ever)
    +0xc4 f32 anchor (ConsiderImpulse's threshold; 0 = wakes on any touch)
    +0xcc f32 bounciness (Smokeable::Init clamps it to <= 0.005)
    +0xdc u32 the category's name hash, which MatchSmokeableCategory turns into
          +0xd8, its index in SmokeableCategoryNameTable (0x803602BC; 0 when
          none matches) -- what the collision sounds play (stSmokableCollision)
    +0xe0 f32 friction   (Smokeable::Init clamps it to >= 0.3)

GJK polytopes, chunk 0x80034020 in <track>.BUN (LoaderCollisionVolumes
0x801B8BFC): per volume a 0x39200 header (+0x18 u32 hash), a 0x39201 run of
0x30-byte GJKVertex records (position at +0x10), a 0x39202 u16 index run.
Smokeable::Init asks FindGJKPolytope for the info's hash with the centre of
gravity, which MoveCenterOfGravity stores so GetPoint answers relative to it,
and keeps GetBoundingBox's box over those points. With no polytope the box is
the model's own, halved about the body (GetLocalBoundingBox * 0.5, +-).

Spawner pack, chunk 0x34027 in the streamed sections (Spawn 0x801D1528):
    header from the next 16-byte boundary: +0x08 s16 section, +0x0a s16 the
    first spawner id, +0x0c s16 count; spawners from +0x10, 0x70 bytes each:
    +0x00 u32 info hash  +0x04 u16 scenery override  +0x08 u32 model hash
    +0x10 f32 world box min[3]  +0x20 max[3]  +0x30 bMatrix4 (rows; the
    translation at +0x60 is the scenery instance's)
A spawner becomes a VirginSmokeable when its override's byte 6 is 1 (every
L4RA one is), or when a scenery group other than SMOKEABLE holds it.

WORLD.NFSK, little-endian:
    0x00 char magic[4] "NFSK"  0x04 u32 version = 3
    0x08 u32 infos  0x0c u32 spawners  0x10 u32 infosOffset
    0x14 u32 spawnersOffset  0x18 u32 spawner id limit  0x1c u32 0
    Info[infos], 0x90 bytes:
        u32 hash; f32 mass; f32 invMass; f32 invInertia[9] (body space, rows);
        f32 cg[3]; f32 lo[3]; f32 hi[3] (the box, relative to the cg);
        f32 anchor; f32 lifetime; f32 bounce; f32 friction (clamped as Init
        does); u32 flags (1 = the box is the polytope's, 2 = pieces -- not
        ported, 4 = emitters); char name[24] (for the log);
        u32 anchor emitter group (+0x84), u32 awaken emitter group (+0x88),
        both afx_pack.py's prop groups; u32 category (+0xd8); u32 0
    Spawner[spawners], 0x50 bytes, by id:
        u16 section (the one whose scenery draws the prop, see place()); u16 info; u32 id; f32 rot[9] (the matrix's rows);
        f32 pos[3]; f32 lo[3]; f32 hi[3] (the world box)
"""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import track_pack  # noqa: E402

INFO_TAG = 0x34026
SPAWNER_TAG = 0x34027
VOLUMES_TAG = 0x80034020
POLY_TAG = 0x39200
INFO = 0x100
SPAWNER = 0x70
VERSION = 3
OUT_INFO = 0x90
FLAG_POLYTOPE, FLAG_PIECES, FLAG_EMITTERS = 1, 2, 4
# SmokeableCategoryNameTable (Speed.elf 0x803602BC), in index order.
CATEGORIES = [
    "NONE", "GARBAGE_BAG", "SHOPPING_CART", "METAL_FENCE", "TRAFFIC_CONE",
    "WATER_DRUM", "OIL_DRUM", "PARKING_METER", "ROAD_SIGN_METAL", "ROAD_SIGN_WOOD",
    "MARKET_STALL", "SANDWICH_BOARD", "SAW_HORSE", "METAL_SMALL", "METAL_BIG",
    "WOODBOX", "TRASHCAN", "GLASS", "CHAIR",
]


def walk(blob, o, end, tags, out, depth=0):
    while o + 8 <= end:
        tag, size = struct.unpack_from("<II", blob, o)
        if o + 8 + size > end:
            return
        if tag in tags:
            out.append((tag, o, size))
        elif tag & 0x80000000 and depth < 4:
            walk(blob, o + 8, o + 8 + size, tags, out, depth + 1)
        o += 8 + size


def read_infos(l4):
    found = []
    walk(l4, 0, len(l4), (INFO_TAG,), found)
    if not found:
        raise SystemExit("no SmokeableInfo chunk")
    _, o, n = found[0]
    base = (o + 8 + 15) & ~15
    infos = {}
    for i in range((n - (base - (o + 8))) // INFO):
        r = l4[base + i * INFO:base + (i + 1) * INFO]
        name = r[8:0x20].split(b"\0")[0].decode("latin1")
        infos[bstring_hash(name)] = (name, r)
    return infos


def bstring_hash(name):
    """bStringHash as FindSmokeableInfo keys it: over the name as written. (track_pack's
    upper-cases, which is right for model names and wrong for ZCS_Barrel_A.)"""
    h = 0xffffffff
    for c in name.encode("latin1"):
        h = (h * 33 + c) & 0xffffffff
    return h


def read_polytopes(l4):
    """hash -> (lo, hi) over the volume's points, as GetBoundingBox walks them."""
    found = []
    walk(l4, 0, len(l4), (VOLUMES_TAG,), found)
    polys = {}
    for _, o, n in found:
        q, end = o + 8, o + 8 + n
        while q + 8 <= end:
            tag, size = struct.unpack_from("<II", l4, q)
            if tag != POLY_TAG:
                q += 8 + size
                continue
            hdr = (q + 0x17) & ~0xF
            h = struct.unpack_from("<I", l4, hdr + 0x18)[0]
            v = q + 8 + size
            vsize = struct.unpack_from("<I", l4, v + 4)[0]
            vb = (v + 0x17) & ~0xF
            count = (vsize - (vb - (v + 8))) // 0x30
            pts = [struct.unpack_from("<3f", l4, vb + i * 0x30 + 0x10) for i in range(count)]
            if pts:
                polys[h] = ([min(p[k] for p in pts) for k in range(3)],
                            [max(p[k] for p in pts) for k in range(3)])
            ix = v + 8 + vsize
            q = ix + 8 + struct.unpack_from("<I", l4, ix + 4)[0]
    return polys


def invert3(m):
    a, b, c, d, e, f, g, h, i = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-12:
        raise SystemExit("singular inertia")
    return [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
            (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
            (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]


def read_spawners(stream_path, sections, overrides_raw, groups):
    spawners = []
    skipped = collections.Counter()
    with open(stream_path, "rb") as f:
        for sec in sections:
            blob = track_pack.read_section(f, sec)
            found = []
            walk(blob, 0, len(blob), (SPAWNER_TAG,), found)
            for _, o, n in found:
                b = (o + 8 + 15) & ~15
                section, first, count = struct.unpack_from("<hhh", blob, b + 8)
                for i in range(count):
                    r = b + 0x10 + i * SPAWNER
                    h, ov = struct.unpack_from("<IH", blob, r)
                    virgin = overrides_raw[ov * 8 + 6] == 1 or any(
                        groups.names[g] != "SMOKEABLE" for g in groups.members.get(ov, ()))
                    if not virgin:
                        skipped["not virgin"] += 1
                        continue
                    m = struct.unpack_from("<16f", blob, r + 0x30)
                    spawners.append(dict(
                        section=section, id=first + i, hash=h,
                        model=struct.unpack_from("<I", blob, r + 8)[0],
                        lo=struct.unpack_from("<3f", blob, r + 0x10),
                        hi=struct.unpack_from("<3f", blob, r + 0x20),
                        rot=m[0:3] + m[4:7] + m[8:11], pos=m[12:15]))
    return spawners, skipped


def place(stream_path, sections, spawners, skipped):
    """Points each spawner at the section whose scenery draws its prop.

    The pack's own section mostly (447 of L4RA's 514), else the one 20 below it, the
    section its scenery override names (65) -- the port's world blobs carry an instance
    in the section whose scenery lists it, so that is where smokeable.c looks for it.
    A spawner with no instance at its translation in either is dropped: the port would
    have nothing to hide when it converts. Returns the spawners kept."""
    by_num = {s.number: s for s in sections}
    wanted = collections.defaultdict(list)
    for s in spawners:
        wanted[s["section"]].append(s)
    kept = []
    with open(stream_path, "rb") as f:
        for n, lst in sorted(wanted.items()):
            pos = {}
            for m in (n, n - 20):
                if m in by_num:
                    data = track_pack.read_section(f, by_num[m])
                    pos[m] = [tuple(i["pos"]) for _, insts in
                              track_pack.read_scenery_groups(data) for i in insts]
            for s in lst:
                at = next((m for m, ps in pos.items() if any(
                    max(abs(a - b) for a, b in zip(p, s["pos"])) < 0.01 for p in ps)), None)
                if at is None:
                    skipped["no scenery instance"] += 1
                    continue
                s["section"] = at
                kept.append(s)
    kept.sort(key=lambda s: s["id"])
    return kept


def model_boxes(stream_path, sections, models):
    """hash -> the model's own (lo, hi) over its vertices, for infos with no polytope."""
    with open(stream_path, "rb") as f:
        lib = track_pack.build_model_library(f, sections, set(models))
    out = {}
    for h, (data, part) in lib.items():
        verts = track_pack.read_track_mesh(data, part)[0]
        if verts:
            out[h] = ([min(v[k] for v in verts) for k in range(3)],
                      [max(v[k] for v in verts) for k in range(3)])
    return out


def build(l4_path, stream_path, verbose):
    l4 = open(l4_path, "rb").read()
    sections = track_pack.read_section_table(l4_path)
    infos = read_infos(l4)
    polys = read_polytopes(l4)
    groups = track_pack.SceneryGroups(l4_path)
    raw = None
    for tag, size, off in track_pack._top_chunks(l4):
        if tag == track_pack.SCENERY_OVERRIDE_TAG:
            raw = l4[off:off + size]
    if raw is None:
        raise SystemExit("no scenery override table")
    spawners, skipped = read_spawners(stream_path, sections, raw, groups)
    # FindSmokeableInfo failing disables the spawner (Spawn).
    for s in spawners:
        if s["hash"] not in infos:
            skipped["no info"] += 1
    spawners = place(stream_path, sections, [s for s in spawners if s["hash"] in infos],
                     skipped)

    used = sorted({s["hash"] for s in spawners}, key=lambda h: infos[h][0])
    need_model = {}
    for h in used:
        if struct.unpack_from("<I", infos[h][1], 0x80)[0] not in polys:
            for s in spawners:
                if s["hash"] == h:
                    need_model.setdefault(h, s["model"])
    mboxes = model_boxes(stream_path, sections, need_model.values()) if need_model else {}

    recs = []
    for h in used:
        name, r = infos[h]
        f = lambda o: struct.unpack_from("<f", r, o)[0]  # noqa: E731
        mass = f(0x20)
        m4 = struct.unpack_from("<16f", r, 0x30)
        inertia = [m4[0], m4[1], m4[2], m4[4], m4[5], m4[6], m4[8], m4[9], m4[10] * f(0x70)]
        cg = struct.unpack_from("<3f", r, 0xb0)
        poly, emit_a, emit_b, pieces = struct.unpack_from("<4I", r, 0x80)
        flags = 0
        if poly in polys:
            plo, phi = polys[poly]
            lo = [plo[k] - cg[k] for k in range(3)]
            hi = [phi[k] - cg[k] for k in range(3)]
            flags |= FLAG_POLYTOPE
        else:
            mb = mboxes.get(need_model[h])
            if not mb:
                raise SystemExit("%s: no polytope and no model %08x" % (name, need_model[h]))
            half = [(mb[1][k] - mb[0][k]) * 0.5 for k in range(3)]
            lo = [-x for x in half]
            hi = half
        if pieces:
            flags |= FLAG_PIECES
        if emit_a or emit_b:
            flags |= FLAG_EMITTERS
        # MatchSmokeableCategory.
        cat_hash = struct.unpack_from("<I", r, 0xdc)[0]
        category = next((i for i, c in enumerate(CATEGORIES) if bstring_hash(c) == cat_hash), 0)
        rec = dict(hash=h, name=name, mass=mass, inv_inertia=invert3(inertia), cg=cg,
                   lo=lo, hi=hi, anchor=f(0xc4), lifetime=f(0xc0),
                   bounce=min(f(0xcc), 0.005), friction=max(f(0xe0), 0.3), flags=flags,
                   inertia=inertia, emit_a=emit_a, emit_b=emit_b, category=category)
        recs.append(rec)
    index = {r["hash"]: i for i, r in enumerate(recs)}
    if verbose:
        count = collections.Counter(s["hash"] for s in spawners)
        for r in recs:
            off = [x for i, x in enumerate(r["inertia"]) if i not in (0, 4, 8)]
            print("  %-22s x%-3d mass %.3g  I %s%s  cg %s  box %s..%s  anchor %g life %g "
                  "bounce %g friction %g flags %d  %s  emitters %08x/%08x" % (
                      r["name"], count[r["hash"]], r["mass"],
                      "/".join("%.4g" % r["inertia"][i] for i in (0, 4, 8)),
                      " (off-diagonal!)" if any(abs(x) > 1e-6 for x in off) else "",
                      "/".join("%.3g" % x for x in r["cg"]),
                      "/".join("%.3g" % x for x in r["lo"]),
                      "/".join("%.3g" % x for x in r["hi"]),
                      r["anchor"], r["lifetime"], r["bounce"], r["friction"], r["flags"],
                      CATEGORIES[r["category"]], r["emit_a"], r["emit_b"]))
        for k, v in skipped.items():
            print("  skipped (%s): %d" % (k, v))
    return recs, spawners, index


def pack(recs, spawners, index):
    io = 0x20
    so = io + OUT_INFO * len(recs)
    limit = max(s["id"] for s in spawners) + 1
    out = bytearray(b"NFSK" + struct.pack("<7I", VERSION, len(recs), len(spawners), io, so,
                                          limit, 0))
    for r in recs:
        out += struct.pack("<I2f9f3f3f3f4fI24s4I", r["hash"], r["mass"], 1.0 / r["mass"],
                           *r["inv_inertia"], *r["cg"], *r["lo"], *r["hi"], r["anchor"],
                           r["lifetime"], r["bounce"], r["friction"], r["flags"],
                           r["name"].encode("latin1")[:23], r["emit_a"], r["emit_b"],
                           r["category"], 0)
    for s in spawners:
        out += struct.pack("<HHI9f3f3f3f", s["section"], index[s["hash"]], s["id"], *s["rot"],
                           *s["pos"], *s["lo"], *s["hi"])
    assert len(out) == so + 0x50 * len(spawners)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pc_root")
    ap.add_argument("out", help="the game data root to write TRACKS/<track>/ under")
    ap.add_argument("--track", default="L4RA")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    tracks = os.path.join(args.pc_root, "TRACKS")
    recs, spawners, index = build(os.path.join(tracks, args.track + ".BUN"),
                                  os.path.join(tracks, "STREAM" + args.track + ".BUN"),
                                  args.verbose)
    if not spawners:
        raise SystemExit("no smokeable spawners in STREAM%s.BUN" % args.track)
    blob = pack(recs, spawners, index)
    dst = os.path.join(args.out, "TRACKS", args.track, "WORLD.NFSK")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".tmp", "wb") as f:
        f.write(blob)
    os.replace(dst + ".tmp", dst)
    print("%s: %d bytes, %d infos, %d spawners" % (dst, len(blob), len(recs), len(spawners)))


if __name__ == "__main__":
    main()
