#!/usr/bin/env python3
"""
NIS/Scene_EndingNis0N_BundleB.bun  ->  NIS/EndingNis0N.crowd, the race-end
crowd scene for the 3DS (TASKS.md #354; every offset is decoded there and in
tools/re/nis/).

What the bundle holds and what is packed from it:

  - The crowd solids (0x80134000): CROWD_FEMALE_01/02, CROWD_MALE_01/02, LISHA,
    NICK. Each is written as an ordinary lit .nfsc (car_pack.pack_nfsc, the car
    shader's pos / normal / uv vertex) in its bind pose, with its texture from
    the bundle's own TPKs, followed by a skin table in the same vertex order:
    three skeleton bone indices (the subset's palette already applied) and
    three weights.
  - The skeletons (0x00E34009, Bip02 / Bip03): parent and inverse bind matrix
    per bone, plus the bind SQT for bones an anim leaves alone.
  - The anims (0x00E34010) the entities name, baked one key a 30 Hz anim frame:
    per bone the local quaternion (x, y, z, w) and translation. The runtime
    lerps between keys (nlerp for the quaternion).
  - The entities (0x37040, CBasicCharacterAnimEntity): model, skeleton, anim,
    flags, the placement matrix (scene-relative), speed, delay and the loop
    range (flag 0x40: SetLoopRange(start, end), #422).
  - The four ZPM_CarN tracks (pose float 16 quat, 20 translation), baked the
    same way: where the scene's cars stand and how ZPM_Car1 drives.

Format (little endian, every block 4-aligned):
  u32 'CRWD', u32 version 2, u32 nModel, nSkel, nAnim, nEnt, nCar
  model x nModel: u32 hash, u32 skel index, u32 nVerts, u32 nfscBytes, nfsc,
                  then nVerts x { u8 bone[3], u8 0, f32 w[3] }
  skel  x nSkel:  u32 hash, u32 nBones, nBones x { s32 parent, f32 ib[16],
                  f32 q[4], f32 t[3] }
  anim  x nAnim:  u32 hash, u32 nBones, u32 nKeys, nKeys x nBones x
                  { f32 q[4], f32 t[3] }
  ent   x nEnt:   u32 hash, s32 model, s32 skel, s32 anim, u32 flags,
                  f32 m[12] (rows 0..3, xyz), f32 speed, f32 delay, u32 loopStart,
                  u32 loopEnd (version 1 had no loopStart)
  car   x nCar:   u32 nKeys, nKeys x { f32 q[4], f32 t[3] }
  u32 sceneBytes, then the bundle's 0x80037020 chunk as it is (its 8-byte
  header included), where nis.c finds the 0x37030 scene header: the scene's
  hash, name and length, so the 1 MB bundle itself need not be on the card.
  (version 3) u32 w, u32 h, then w x h u8: the alpha of the bundle's
  CHARACTERSHADOW (its colour is black), the people's ground shadow
  (InitCharacterEffects, Speed.elf 0x8004F420; #497). 0 x 0 when the bundle
  has none, and retail then draws no shadow.

usage: crowd_pack.py <PC Files> <out dir> [01 02 03 04 ShopEnter1 ShopExit1]

The shop enter / exit scenes (#408) pack the same way: no entities, the one
ZPM_Car1 track and the scene chunk.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "re", "nis"))
import car_pack  # noqa: E402
import scene_pack  # noqa: E402
import niscrowd  # noqa: E402
import skel as skelmod  # noqa: E402
import nisanim  # noqa: E402
import nisent  # noqa: E402

VERSION = 3
MAXDIM = 128
# Females use Bip03, everyone else Bip02 (the entities say so too; this is
# only the fallback for a model no entity names).
FEMALE = ("CROWD_FEMALE_01", "CROWD_FEMALE_02")


def bh(s):
    return nisent.h(s)


def bind_pose(bones):
    pose = [0.0] * (12 * max(32, len(bones)))
    for i, bn in enumerate(bones):
        pose[i * 12:i * 12 + 12] = list(bn["scale"]) + [1.0] + list(bn["q"]) + list(bn["t"][:3]) + [0.0]
    return pose


def norm_q(q):
    n = sum(c * c for c in q) ** 0.5 or 1.0
    return [c / n for c in q]


def bake_anim(d, m, bones):
    frames = nisanim.u16(d, m + 0xA)
    keys = []
    worst_scale = 0.0
    for f in range(frames):
        pose = bind_pose(bones)
        nisanim.eval_map(d, m, float(f), pose)
        row = []
        for i in range(len(bones)):
            p = pose[i * 12:i * 12 + 12]
            worst_scale = max(worst_scale, max(abs(s - 1.0) for s in p[0:3]))
            row.append((norm_q(p[4:8]), p[8:11]))
        keys.append(row)
    return keys, worst_scale


def bake_car(an, n):
    """ZPM_Car<n>_q / _t into one track: pose floats 16..19 and 20..22."""
    tracks = [an.get("ZPM_Car%d_q" % n), an.get("ZPM_Car%d_t" % n)]
    if not all(tracks):
        return []
    frames = max(nisanim.u16(d, m + 0xA) for d, m in tracks)
    out = []
    for f in range(frames):
        pose = [0.0] * (12 * 32)
        pose[16:20] = [0.0, 0.0, 0.0, 1.0]
        for d, m in tracks:
            nisanim.eval_map(d, m, float(min(f, nisanim.u16(d, m + 0xA) - 1)), pose)
        out.append((norm_q(pose[16:20]), pose[20:23]))
    return out


def model_blob(b, part, textures, bones):
    V, I, subs = posefig_load(b, part)
    skin = [None] * len(V)
    clash = 0
    for first, cnt, _npal, pal in subs:
        for vi in set(I[first:first + cnt]):
            v = V[vi]
            bi = []
            for k in range(3):
                slot = int(v[12 + k])
                bi.append(pal[slot] if slot < len(pal) else 0)
            w = [max(0.0, v[9 + k]) for k in range(3)]
            s = sum(w) or 1.0
            ent = (tuple(bi), tuple(x / s for x in w))
            if skin[vi] is not None and skin[vi][0] != ent[0]:
                clash += 1
            skin[vi] = ent
    for i in range(len(skin)):
        if skin[i] is None:
            skin[i] = ((0, 0, 0), (1.0, 0.0, 0.0))
        for bix in skin[i][0]:
            assert bix < len(bones), (part["name"], bix)
    verts = []
    bbmin, bbmax = [1e30] * 3, [-1e30] * 3
    for v in V:
        x, y, z, nx, ny, nz = v[0:6]
        u, vv = v[7], v[8]
        verts.append((x, y, z, nx, ny, nz, u, -vv))
        for k, c in enumerate((x, y, z)):
            bbmin[k] = min(bbmin[k], c)
            bbmax[k] = max(bbmax[k], c)
    th = part["textures"][0] if part["textures"] else 0
    tex_entries, tex_blob, ti = [], bytearray(), -1
    if th in textures:
        name, w, h, rgba = textures[th]
        rgba, w, h = car_pack.box_downscale(rgba, w, h, MAXDIM)
        fmt, px, maxlvl = car_pack.encode_texture_chain(rgba, w, h)
        tex_entries.append((th, w, h, car_pack.tex_fmt_field(fmt, maxlvl), 0))
        tex_blob.extend(px)
        ti = 0
    batches = [(first, cnt, ti, 0) for first, cnt, _n, _p in subs]
    batches, idx = car_pack.merge_batches(batches, list(I))
    nfsc = car_pack.pack_nfsc(tex_entries, tex_blob, batches, verts, idx, bbmin, bbmax,
                              car_pack.SHADING_LIT)
    return nfsc, skin, clash, th in textures


def posefig_load(b, part):
    m = part["mesh"]
    sub = part["_sub"]
    vs = car_pack.leaf_start(sub, m["vb"][0], m["vb"][1], 60)
    V = [struct.unpack_from("<6fI8f", sub, vs + i * 60) for i in range(m["numVerts"])]
    ss = car_pack.leaf_start(sub, m["subsets"][0], m["subsets"][1], 60)
    ist = car_pack.leaf_start(sub, m["ib"][0], m["ib"][1], 2)
    I = struct.unpack_from("<%dH" % (m["numTris"] * 3), sub, ist)
    subs = []
    for s in range(m["numSubsets"]):
        r = sub[ss + s * 60:ss + s * 60 + 60]
        cnt = struct.unpack_from("<I", r, 0xC)[0]
        first = struct.unpack_from("<I", r, 0x34)[0]
        subs.append((first, cnt, r[0x20], list(r[0x21:0x21 + 19])))
    return V, I, subs


def pad4(buf):
    while len(buf) % 4:
        buf.append(0)


def build(bun_path):
    b = open(bun_path, "rb").read()
    textures = scene_pack.load_scene_textures(b)
    sks = skelmod.skeletons(b)
    # The entities name the skeleton by bStringHash("Bip0N").
    skel_index = {bh(nm): i for i, (_o, nm, _h, _b) in enumerate(sks)}
    skel_by_name = {nm: i for i, (_o, nm, _h, _b) in enumerate(sks)}
    an = nisanim.banks(bun_path)
    anim_by_hash = {bh(k): k for k in an}

    parts = []
    for off, sub, ps in niscrowd.solids(b):
        for p in ps:
            p["_sub"] = sub
            parts.append(p)
    part_index = {p["hash"]: i for i, p in enumerate(parts)}

    ents = []
    used_anims = []
    for e in nisent.entities(b):
        if len(e) < 0x88 or struct.unpack_from("<I", e, 0)[0] != 1:
            continue
        ehash, _x, flags = struct.unpack_from("<III", e, 4)
        mat = struct.unpack_from("<16f", e, 0x10)
        model_h = struct.unpack_from("<I", e, 0x50)[0]
        anim_h, skel_h = struct.unpack_from("<II", e, 0x6C)
        speed, delay = struct.unpack_from("<ff", e, 0x74)
        # CBasicCharacterAnimEntity +0x80 / +0x84, SetLoopRange's two (anim
        # frames; endnis3_ps.c): the anim plays to the end once, then loops
        # from the start (#422: the walkers no longer walk in again).
        loop_start, loop_end = struct.unpack_from("<II", e, 0x80)
        aname = anim_by_hash.get(anim_h)
        ai = -1
        if aname:
            if aname not in used_anims:
                used_anims.append(aname)
            ai = used_anims.index(aname)
        ents.append((ehash, part_index.get(model_h, -1), skel_index.get(skel_h, -1), ai, flags,
                     [mat[r * 4 + c] for r in range(4) for c in range(3)], speed or 1.0, delay,
                     loop_start, loop_end))

    out = bytearray()
    out += struct.pack("<4sIIIIII", b"CRWD", VERSION, len(parts), len(sks), len(used_anims),
                       len(ents), 4)
    report = []
    for p in parts:
        # The skeleton an entity plays the model on; failing one, the crowd's
        # usual Bip03 / Bip02, and failing those (IntroNis08 has no Bip02) the
        # bundle's first.
        sk = skel_by_name.get("Bip03" if p["name"] in FEMALE else "Bip02", 0)
        for e in ents:
            if e[1] == part_index[p["hash"]] and e[2] >= 0:
                sk = e[2]
                break
        nfsc, skin, clash, tex_ok = model_blob(b, p, textures, sks[sk][3])
        out += struct.pack("<IIII", p["hash"], sk, len(skin), len(nfsc))
        out += nfsc
        pad4(out)
        for bi, w in skin:
            out += struct.pack("<BBBB3f", bi[0], bi[1], bi[2], 0, *w)
        report.append("  model %-16s %4d verts, skel %s, palette clashes %d, texture %s"
                      % (p["name"], len(skin), sks[sk][1], clash, "ok" if tex_ok else "MISSING"))
    for _o, nm, _h, bones in sks:
        out += struct.pack("<II", bh(nm), len(bones))
        for bn in bones:
            out += struct.pack("<i", bn["parent"])
            out += struct.pack("<16f", *bn["ib"].reshape(16).tolist())
            out += struct.pack("<4f", *norm_q(bn["q"]))
            out += struct.pack("<3f", *bn["t"][:3])
    for aname in used_anims:
        d, m = an[aname]
        sk = None
        for e in ents:
            if e[3] == used_anims.index(aname):
                if sk is not None and e[2] != sk:
                    report.append("  NOTE anim %s is played on two skeletons" % aname)
                sk = e[2] if sk is None else sk
        assert sk is not None and sk >= 0, aname
        bones = sks[sk][3]
        keys, ws = bake_anim(d, m, bones)
        out += struct.pack("<III", bh(aname), len(bones), len(keys))
        for row in keys:
            for q, t in row:
                out += struct.pack("<4f3f", *(list(q) + list(t)))
        report.append("  anim %-34s %3d keys, skel %s, worst scale off 1: %.4f"
                      % (aname, len(keys), sks[sk][1], ws))
    for e in ents:
        out += struct.pack("<IiiiI", e[0], e[1], e[2], e[3], e[4])
        out += struct.pack("<12f", *e[5])
        out += struct.pack("<ffII", e[6], e[7], e[8], e[9])
    for n in range(1, 5):
        tr = bake_car(an, n)
        out += struct.pack("<I", len(tr))
        for q, t in tr:
            out += struct.pack("<4f3f", *(list(q) + list(t)))
        report.append("  car %d: %d keys, first t %s" % (
            n, len(tr), ["%.2f" % x for x in tr[0][1]] if tr else "-"))
    scene = None
    for t, p, sz in niscrowd.top_chunks(b):
        if t == 0x80037020:
            scene = b[p - 8:p + sz]
    assert scene, "no 0x80037020 scene chunk"
    out += struct.pack("<I", len(scene))
    out += scene
    pad4(out)
    # CHARACTERSHADOW: DXT3, alpha-blended, so its alpha is kept as it is.
    recs = {}
    shadow = scene_pack.load_scene_textures(b, recs).get(bh("CHARACTERSHADOW"))
    if shadow:
        _n, sw, sh, rgba = shadow
        out += struct.pack("<II", sw, sh)
        out += bytes(rgba[3::4])
        pad4(out)
        report.append("  CHARACTERSHADOW %dx%d, alpha peak %d" % (sw, sh, max(rgba[3::4])))
    else:
        out += struct.pack("<II", 0, 0)
        report.append("  no CHARACTERSHADOW: no ground shadows")
    report.insert(0, "  %d entities, %d anims, scene chunk %d bytes" % (len(ents), len(used_anims), len(scene)))
    return bytes(out), report


def main():
    pc, outdir = sys.argv[1], sys.argv[2]
    which = sys.argv[3:] or ["01", "02", "03", "04", "ShopEnter1", "ShopExit1"]
    os.makedirs(outdir, exist_ok=True)
    for n in which:
        # "01".."04" are the race-end scenes; any other name is a scene of its
        # own, e.g. the shop enter / exit NIS (#408): no crowd, ZPM_Car1 alone.
        name = "EndingNis%s" % n if n.isdigit() else n
        src = os.path.join(pc, "NIS", "Scene_%s_BundleB.bun" % name)
        blob, rep = build(src)
        dst = os.path.join(outdir, "%s.crowd" % name)
        tmp = dst + ".tmp"
        with open(tmp, "wb") as f:
            f.write(blob)
        os.replace(tmp, dst)
        print("%s.crowd %d bytes" % (name, len(blob)))
        for line in rep:
            print(line)


if __name__ == "__main__":
    main()
