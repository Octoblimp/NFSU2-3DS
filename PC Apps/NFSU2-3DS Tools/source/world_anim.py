#!/usr/bin/env python3
"""The track's world animations (CWorldAnim*, TASKS.md #239) for the 3DS port.

Retail draws the jets, the elevated graffiti trains, the tower cranes, the
sweeping spotlights and the pub's sails through the world-animation system,
not the scenery renderer: a WorldAnimInstance (chunk 0x37090) places a tree of
WorldAnimEntityInfo records (0x37080, closed by a 0x37110 marker), and each
entity with a model draws it through a WorldModel at its node's world matrix,
which its CWorldAnimCtrl drives from an EAGL anim (bank chunk 0x00E34010).
The spec and the addresses are in tools/re/wip239/NOTES.md.

track_pack.py uses this module twice:
  * `WorldAnims.door_models` -- the models of the zone-triggered trees (the
    shop roll-up doors). Retail's scenery never draws their instances (0x10
    with no group); the door trees do, and since #408 so does the port, so
    track_pack.py leaves those scenery instances out like the rest.
  * the distinct models of every tree -- the moving ones and the doors -- are
    baked into one pinned section, WANIM.nfsc, each at a dummy translation of
    its own, and WORLD.NFSW (written here) tells the runtime (worldanim.c) the
    trees, the per-frame poses and the instances. A door tree holds frame 0
    until a shop NIS's DOOR_POSITION event names its trigger zone (#408).

WORLD.NFSW, little-endian:
    +0   "NFWA"
    +4   u32 version (4)
    +8   u32 model count, u32 tree count, u32 entity count,
         u32 instance count, u32 key count
    +28  pad to 32
    models     x {f32 dummy translation[3], u32 model hash,
                  f32 box min[3], f32 box max[3] (model space)}         40 B
    trees      x {u16 first entity, u16 entity count}                   4 B
    entities   x {s16 model (-1: draws nothing), s8 parent (-1: root),
                  u8 flags (1: the quaternion is a raw pose channel's,
                  lerped to the nearer sign and normalised),
                  u16 frames (0: no anim), u16 length,
                  u32 first key, f32 local[12]}                         60 B
    instances  x {u16 tree, u16 scenario (+3c), u32 flags, f32 speed,
                  f32 start delay, f32 loop delay,
                  f32 world[12], f32 bbmin[3], f32 bbmax[3],
                  u32 race gate (+38)}                                  96 B
    keys       x {f32 S[3], f32 Q[4] (x y z w), f32 T[3]}               40 B
    zones      x {u32 trigger zone (+1c / +20, the two are equal)}      4 B
               one per instance (v3): nonzero for a door, which only a
               shop NIS's DOOR_POSITION event for that zone plays
    names      x {u32 node-name hash (WorldAnimEntityInfo +4)}          4 B
               one per entity (v4)
    ids        x {u32 instance id (WorldAnimInstance +0)}               4 B
               one per instance (v4): GetAnimPosition (Speed.elf
               0x80055E80) finds an entity by these two, which the world's
               plane and train sounds follow (TASKS.md #509)

A matrix is retail's bMatrix4 minus its last column: four rows of three, the
X/Y/Z axes then the translation, applied to row vectors (v' = v * M).
`frames` keys per animated entity, one per integer frame, each the SQT of the
bone its anim writes (bone 1's global is that bone's local: the other is
identity); the runtime lerps between two of them as the EAGL channels do.
`length` is the ctrl's +0x60: the frame count of the last channel made, in
t, q, s order.
"""
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "re", "nis"))
import nisanim  # noqa: E402

CH_ENT, CH_INST, CH_MARK, CH_BANK = 0x37080, 0x37090, 0x37110, 0x00E34010

NFSW_NAME = "WORLD.NFSW"
NFSW_MAGIC = b"NFWA"
NFSW_VERSION = 4
SECTION_NAME = "WANIM"   # the pinned section holding the moving trees' models

# The dummy translations the models are baked at in WANIM.nfsc: far enough
# apart that model3d_find_instance (1 cm) can never confuse two of them.
DUMMY_STEP = 100.0


def bstring_hash(s, h=0xFFFFFFFF):
    for c in s.encode("latin1"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def _chunks(d):
    off = 0
    while off + 8 <= len(d):
        t, n = struct.unpack_from("<II", d, off)
        yield off, t, n
        off += 8 + n


def _rec(off):
    # Each payload starts at (chunk + 0x17) & ~0xF (Speed.elf 0x80049040).
    return (off + 0x17) & ~0xF


def _mat(d, o):
    return [list(struct.unpack_from("<4f", d, o + 16 * r)) for r in range(4)]


def _qmat(q):
    """The rotation rows of quaternion (x, y, z, w), crowd.c's qmat."""
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
            [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
            [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]]


def sqt_matrix(s, q, t):
    r = _qmat(q)
    return [[r[i][j] * s[i] for j in range(3)] for i in range(3)] + [list(t)]


IDENT = [1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0]


class WorldAnims(object):
    def __init__(self, bun_path):
        d = open(bun_path, "rb").read()
        self.banks = {}
        for off, t, n in _chunks(d):
            if t == CH_BANK:
                dd, syms = nisanim.load(d, off + 8, n)
                bk = syms["__AnimationBank:::bank"]
                for i in range(nisanim.u32(dd, bk + 4)):
                    a = nisanim.cstr(dd, nisanim.u32(dd, nisanim.u32(dd, bk + 0x14) + 4 * i))
                    self.banks[bstring_hash(a)] = (a, dd, nisanim.u32(dd, nisanim.u32(dd, bk + 0x10) + 4 * i))

        self.instances = []
        self.trees = {}       # tree hash -> [entity dict]
        pend = []
        for off, t, n in _chunks(d):
            if t == CH_INST:
                r = _rec(off)
                w = struct.unpack_from("<16I", d, r)
                f = struct.unpack_from("<16f", d, r)
                self.instances.append(dict(
                    id=w[0], tree=w[1], flags=w[3] or 0x28, speed=f[4] or 1.0,
                    start_delay=f[5], loop_delay=f[6], zone_a=w[8], zone_b=w[9],
                    range=w[12], ev_track=w[13], race_gate=w[14],
                    scenario=struct.unpack_from("<H", d, r + 0x3C)[0],
                    world=_mat(d, r + 0x40),
                    bbmin=struct.unpack_from("<3f", d, r + 0x80),
                    bbmax=struct.unpack_from("<3f", d, r + 0x90)))
            elif t == CH_ENT:
                r = _rec(off)
                w = struct.unpack_from("<12I", d, r)
                pend.append(dict(
                    name=w[1], model=w[3],
                    # CWorldAnimEntity::Init 0x800485d8: a WorldModel only if
                    # +0c is set and +20 is not the node's own name.
                    drawn=w[3] != 0 and w[8] != w[1],
                    anim=w[9], channels=d[r + 0x28],
                    parent=struct.unpack_from("<b", d, r + 0x29)[0],
                    local=_mat(d, r + 0x30)))
            elif t == CH_MARK:
                self.trees[struct.unpack_from("<I", d, _rec(off))[0]] = pend
                pend = []
        for th, ents in self.trees.items():
            for i, e in enumerate(ents):
                if not (e["parent"] < i and (e["parent"] >= 0) == (i > 0)):
                    raise ValueError("tree %08x: entity %d has parent %d"
                                     % (th, i, e["parent"]))

        # Zone-triggered: Init does flags = (flags & ~8) | 0x810 and nothing
        # plays them but a shop NIS (NOTES.md 5).
        zone_trees = set(i["tree"] for i in self.instances if i["zone_a"] or i["zone_b"])
        self.door_models = set(e["model"] for th in zone_trees
                               for e in self.trees.get(th, ()) if e["drawn"])
        self.moving = [i for i in self.instances
                       if not (i["zone_a"] or i["zone_b"]) and i["tree"] in self.trees]
        for i in self.moving:
            # No set uses these (NOTES.md 2); the runtime has no path for
            # them, so a track that did would need one. The race gate and the
            # FwdRev scenario (L4RB) go to the runtime, which knows the race.
            if i["flags"] & 0x640 or i["range"] or i["ev_track"]:
                raise ValueError("world anim instance of tree %08x uses an "
                                 "unported feature (flags 0x%x)" % (i["tree"], i["flags"]))
        self.moving_trees = sorted(set(i["tree"] for i in self.moving))
        # The doors (#408): CWorldAnimEntityTree +1c / +20 is the trigger
        # zone's hash, which BroadcastEventToWorldAnims (0x8004a104) matches
        # against the shop's TrackPathZone +0x30.
        self.doors = [i for i in self.instances
                      if (i["zone_a"] or i["zone_b"]) and i["tree"] in self.trees]
        for i in self.doors:
            if i["flags"] & 0x640 or i["range"] or i["ev_track"]:
                raise ValueError("door instance of tree %08x uses an unported feature "
                                 "(flags 0x%x)" % (i["tree"], i["flags"]))
        self.door_trees = sorted(set(i["tree"] for i in self.doors) - set(self.moving_trees))
        models = []
        for th in self.moving_trees + self.door_trees:
            for e in self.trees[th]:
                if e["drawn"] and e["model"] not in models:
                    models.append(e["model"])
        self.models = models
        self.bounds = [((0.0, 0.0, 0.0), (0.0, 0.0, 0.0))] * len(models)

    def set_bounds(self, k, lo, hi):
        """Model k's box in its own space, which worldanim.c culls with
        (track_pack.py reads it off the mesh)."""
        self.bounds[k] = (tuple(lo), tuple(hi))

    def dummy_pos(self, k):
        return (DUMMY_STEP * (k + 1), 0.0, 0.0)

    def _bake(self, e):
        """(frames, length, [10-float SQT per frame]) for one entity."""
        chans = []
        for k, suf in enumerate(("_t", "_q", "_s")):
            if e["channels"] & (1 << k):
                h = bstring_hash(suf, e["anim"])
                if h not in self.banks:
                    raise ValueError("anim channel %08x missing" % h)
                chans.append(self.banks[h])
        if not chans:
            return 0, 0, []
        lens = [nisanim.u16(dd, m + 0xA) for (_a, dd, m) in chans]
        length = lens[-1]
        frames = max(lens)
        keys = []
        bone = None
        for f in range(frames):
            # The ctrl's pose buffer starts as the ROOT skeleton's still pose
            # (both bones identity) and a channel only writes its own floats,
            # so an entity with no _t keeps T = 0 (Vista_Spotlight_97).
            pose = IDENT + IDENT
            for (_a, dd, m) in chans:
                nisanim.eval_map(dd, m, float(f), pose)
            moved = [b for b in (0, 1)
                     if any(abs(pose[12 * b + i] - IDENT[i]) > 1e-6 for i in range(12))]
            if len(moved) > 1:
                raise ValueError("%s writes both bones" % chans[0][0])
            if moved:
                if bone is not None and moved[0] != bone:
                    raise ValueError("%s changes bone" % chans[0][0])
                bone = moved[0]
            keys.append(pose)
        b = bone or 0
        out = [k[12 * b:12 * b + 3] + k[12 * b + 4:12 * b + 8] + k[12 * b + 8:12 * b + 11]
               for k in keys]
        return frames, length, out

    def _nlerp(self, e):
        """Whether the entity's quaternion comes from a raw pose channel
        (EAGL type 0), whose QuatF4Interp normalises between keys."""
        if not e["channels"] & 2:
            return False
        _a, dd, m = self.banks[bstring_hash("_q", e["anim"])]

        def has_raw(m):
            ty = nisanim.u16(dd, m)
            if ty == 0xF:
                return any(has_raw(nisanim.u32(dd, m + 0xC + 4 * i))
                           for i in range(nisanim.u16(dd, m + 8)))
            return ty == 0
        return has_raw(m)

    def check_frame0(self):
        """(matching, animated): how many animated entities of the moving
        trees have a frame 0 equal to their record's local (+30) within 1 cm.

        A check on the conventions, which BuildSQT (Speed.elf 0x802a7078)
        fixes: each axis row is the quaternion's row (crowd.c qmat) times its
        S, and T goes in unscaled. The records are only the node's pose
        before its first update, and several were exported from another
        frame or with the translation divided by the scale (the cranes, three
        jets), so not every one matches; the trains, the sails and the bridge
        spotlight do, rotation and all."""
        match = total = 0
        for th in self.moving_trees:
            for e in self.trees[th]:
                frames, _len, keys = self._bake(e)
                if not frames:
                    continue
                k = keys[0]
                m = sqt_matrix(k[0:3], k[3:7], k[7:10])
                total += 1
                if all(abs(m[i][j] - e["local"][i][j]) < 0.01
                       for i in range(4) for j in range(3)):
                    match += 1
        return match, total

    def write(self, out_dir):
        ents, trees, keys, names = [], [], [], []
        tree_index = {}
        for th in self.moving_trees + self.door_trees:
            tree_index[th] = len(trees)
            trees.append(struct.pack("<HH", len(ents), len(self.trees[th])))
            for e in self.trees[th]:
                frames, length, k = self._bake(e)
                model = self.models.index(e["model"]) if e["drawn"] else -1
                ents.append(struct.pack("<hbBHHI12f", model, e["parent"],
                                        1 if self._nlerp(e) else 0, frames,
                                        length, len(keys),
                                        *[v for row in e["local"] for v in row[:3]]))
                keys.extend(k)
                names.append(e["name"])
        insts = []
        zones, ids = [], []
        for i in self.moving + self.doors:
            zones.append(i["zone_a"] or i["zone_b"])
            ids.append(i["id"])
            insts.append(struct.pack("<HHIfff12f3f3fI", tree_index[i["tree"]],
                                     i["scenario"], i["flags"], i["speed"],
                                     i["start_delay"], i["loop_delay"],
                                     *([v for row in i["world"] for v in row[:3]] +
                                       list(i["bbmin"]) + list(i["bbmax"]) +
                                       [i["race_gate"]])))
        out = bytearray(struct.pack("<4sI5I4x", NFSW_MAGIC, NFSW_VERSION, len(self.models),
                                    len(trees), len(ents), len(insts), len(keys)))
        for k, h in enumerate(self.models):
            out += struct.pack("<3fI6f", *(self.dummy_pos(k) + (h,) +
                                           self.bounds[k][0] + self.bounds[k][1]))
        for t in trees:
            out += t
        for e in ents:
            out += e
        for i in insts:
            out += i
        for k in keys:
            out += struct.pack("<10f", *k)
        for z in zones:
            out += struct.pack("<I", z)
        for n in names:
            out += struct.pack("<I", n)
        for n in ids:
            out += struct.pack("<I", n)
        with open(os.path.join(out_dir, NFSW_NAME), "wb") as fh:
            fh.write(out)
        return len(out)


if __name__ == "__main__":
    wa = WorldAnims(sys.argv[1])
    print("%d instances (%d moving, %d doors), %d trees, %d models, %d door models"
          % (len(wa.instances), len(wa.moving), len(wa.doors), len(wa.trees), len(wa.models),
             len(wa.door_models)))
    print("models: " + " ".join("%08x" % h for h in wa.models))
    print("frame 0 equal to the record's local: %d of %d animated entities"
          % wa.check_frame0())
    if len(sys.argv) > 2:
        print("wrote %s, %d B" % (NFSW_NAME, wa.write(sys.argv[2])))
