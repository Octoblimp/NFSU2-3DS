#!/usr/bin/env python3
"""
TRACKS/L<track>.BUN + TRACKS/STREAML<track>.BUN  ->  per-section .nfsc world models.

The retail world is not one model. `L4RA.BUN` is the track's resident header
file, and `STREAML4RA.BUN` (118 MB for L4RA) is a flat container of *sections*
that the engine pages in and out around the car. The section table is
`L4RA.BUN`'s `0x00034110` leaf: 0x50-byte records, 393 of them for L4RA, of

    +0x00  char[8]  section name  ("C21", "Y0", "X3", ...; entry 0 is "--")
    +0x08  u32      section number (C21 -> 321, Y0 -> 2500, X0 -> 2400)
    +0x0c  u32      0
    +0x10  u32      1 for every real section (0 on the "--" entry)
    +0x14  u32      byte offset into STREAML<track>.BUN
    +0x18  u32      byte length there
    +0x1c  u32      a second, smaller length (resident portion; unused here)
    +0x20  u32      a per-section id
    +0x30  u32      a hash of the section name

Each section's payload is a plain bChunk stream -- the same one `bin_unpack.py`
walks everywhere else -- and, crucially, the geometry inside it is the **same
0x80134000 tree that cars and frontend platforms use**. So `car_pack`'s parser
is reused verbatim and nothing new had to be decoded to read the world:

  - 360 of L4RA's 393 sections carry geometry; the rest are texture-only.
  - Part names are the real authored ones (`TRN_UC_ROADA_CHOP_C1_R10`,
    i.e. terrain / Underground-City / road-A / chopped / chunk C1 / row 10).
  - Vertices are in **absolute world space** already -- a section's part
    matrices are identity-translation, and the vertex positions are the world
    coordinates. So sections need no placement pass: load two of them and they
    line up. Same +Z-up, 1-unit-per-metre world as the cars.
  - The FVF is the 24-byte scenery one (`pos3 / D3DCOLOR / uv2`, baked vertex
    lighting in the colour) for 10585 of 10704 parts. 115 parts carry the
    36-byte car FVF (`pos3 / normal3 / D3DCOLOR / uv2`) instead -- those are the
    lit props. Both are emitted into the same .nfsc vertex, whose second float3
    slot is either baked colour or a normal per the blob's `shading` field; a
    36-byte part in a baked section has its colour taken and its normal dropped,
    because the section as a whole is drawn by `scene.v.pica`.

Textures do **not** live in the geometry sections. They live in the sections
whose names start with a letter that carries a 0xb3300000 TPK -- for L4RA the
bulk is `Y0`, a single 25 MB section holding the shared road/terrain art. So the
texture index is built by scanning every section's TPKs once, and only the
hashes the packed geometry actually asks for are decoded.

Output is the same .nfsc container `car_pack.py` writes (see its docstring for
the field layout, and `3DS Files/source/model3d.h` for the C mirror), one file
per section, so the runtime can page sections the way retail does.

A section's blob carries its scenery at **two levels of detail**, which the
scenery records have always named and this converter used to discard (TASKS.md
#173). The batch list is laid out in three contiguous runs and the header's
`lodSplit` field says where each starts:

    [0, nearFirst)        terrain, and every prop that draws one model
                          whatever its level (A == B, always A, never A)
    [nearFirst, farFirst) the A model of everything else
    [farFirst, numBatches) the B model of the same props

Which level an instance draws is retail's per-instance, per-frame choice
(Speed.elf DrawAScenery 0x80208fa4; TASKS.md #247), so each blob also carries
an instance table after its indices -- every instance's box, translation,
radius and A-model density, and the runs of indices each one draws -- and
`3DS Files/source/model3d.c` makes that choice from it. The index pool is in
the same order as the batch list, so the ground and collision scans can stop
at `farFirst` and never see the B duplicates.

Alongside the sections goes `WORLD.NFSX`, the index the runtime pager reads
(`3DS Files/source/world.c` is the C mirror). The retail section table cannot be
used for that directly: it addresses byte ranges inside `STREAM<track>.BUN` and
says nothing about where a section *is* in the world, and the pager's whole job
is a distance test. So the index carries the world bounding box each emitted
blob actually turned out to have, measured from its own vertices:

    header, 0x40 bytes
      +0x00  char[4]  "NFSW"
      +0x04  u32      version (1)
      +0x08  u32      section count
      +0x0c  u32      record size (0x40)
      +0x10  f32[3]   bbmin over every section
      +0x1c  f32[3]   bbmax over every section
      +0x28  char[8]  track code ("L4RA")
      +0x30  u32[4]   reserved

    one 0x40-byte record per emitted .nfsc
      +0x00  char[16] file stem, no extension ("C21", "C22.1")
      +0x10  f32[3]   bbmin
      +0x1c  f32[3]   bbmax
      +0x28  u32      file size in bytes
      +0x2c  u32      vertex count
      +0x30  u32      index count
      +0x34  u32      batch count
      +0x38  u32      texture-payload bytes
      +0x3c  u32      reserved

The file-size and texture-byte fields are there for the pager's resident budget:
it has to know what a section costs before it opens it, because the decision it
makes is which sections *not* to load.
"""
import argparse
import array
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import car_pack  # noqa: E402
import etc1_tex  # noqa: E402
import extract_global_textures as egt  # noqa: E402
import scene_pack  # noqa: E402
import world_anim  # noqa: E402
from dxt_transcode import decode_dxt, decode_bgra  # noqa: E402
from geometry_transcode import skip_alignment_padding, leaf_start  # noqa: E402

SECTION_TABLE_TAG = 0x00034110
SECTION_RECORD_SIZE = 0x50

SCENE_STRIDE = 24
CAR_STRIDE = 36

# The scenery placement system. A section can carry *several* of these groups
# (L4RA's C22 has two: 29 world-space terrain pieces and 356 placed props), so
# every 0x80034100 in the section has to be walked, not just the first.
SCENERY_GROUP_TAG = 0x80034100
SCENERY_HEADER_TAG = 0x00034101
SCENERY_INFO_TAG = 0x00034102    # 68-byte records, 32-byte name at +0x00
SCENERY_INSTANCE_TAG = 0x00034103  # 64-byte records, see read_scenery_groups
SCENERY_INFO_STRIDE = 68
SCENERY_INSTANCE_STRIDE = 64
ROT_ONE = 8192.0  # the 3x3 is 1.13 fixed point

# The .nfsc index pool is u16, so a section wider than this cannot be emitted as
# one blob. Nothing in L4RA needs splitting at the moment; a section that does
# is reported rather than silently truncated.
MAX_VERTS = 65535

# Not every section in the table is a *place*. L4RA's 393 records split cleanly
# at section number 2400: below it are the 335 world sections (A21 is 121, C21
# is 321), and at or above it are the resource sections -- X0..X75 at 2400+ are
# the shared model libraries, Y0..Y25 at 2500+ are the texture packs, Z0 is
# 2600. Those carry geometry too, so they read as world sections and get
# emitted as such, but their contents sit at authoring origins rather than in
# the world: converted as places, X0 comes out claiming a 16 km bounding box
# reaching 8 km into the sky, which the pager then reads as permanently nearby.
# They are inputs to the conversion, not outputs of it.
FIRST_RESOURCE_SECTION = 2400

# The runtime pager's index. See the docstring above for the layout, and
# world.c's WORLD_MAGIC/WorldSection for the C side.
INDEX_NAME = "WORLD.NFSX"
INDEX_MAGIC = b"NFSW"
INDEX_VERSION = 2
INDEX_HDR_SIZE = 0x40
INDEX_RECORD_SIZE = 0x60
INDEX_NAME_LEN = 16

# The fraction of vertices trimmed off each end of each axis to get a section's
# *core* box -- where its content actually is, as opposed to how far its
# content reaches. See core_bbox().
CORE_TRIM = 0.02

# A section whose core box is still this wide after trimming is not a city
# tile: it is the skyline plate or a distant-LOD shell, and it is meant to be
# visible from everywhere in the world rather than paged around the car. L4RA
# has nine of them and they come to 3.0 MB between them against a 153 MB median
# street tile set, so the runtime pins them instead of letting them compete for
# the resident-section budget. See world.c's SECTION_FAR.
FAR_EXTENT = 1500.0
SECTION_FAR = 1
# The world animations' models (world_anim.py, TASKS.md #239): pinned like far
# scenery, but never drawn as a section -- worldanim.c draws each model through
# its entity's matrix.
SECTION_ANIM = 2

# The shared texture bank. See TextureBank for why this exists and world.c's
# WORLD_BANK_MAGIC for the C mirror.
#
#     header, 0x40 bytes
#       +0x00  char[4]  "NFSB"
#       +0x04  u32      version (1)
#       +0x08  u32      texture count
#       +0x0c  u32      record size (0x20)
#       +0x10  u32      payload offset from the start of the file
#       +0x14  u32      payload bytes
#       +0x18  char[8]  track code
#       +0x20  u32      texture-animation table bytes, after the records (#326)
#       +0x24  u32[7]   reserved
#
#     one 0x20-byte record per distinct texture, sorted by hash so the runtime
#     can binary-search it
#       +0x00  u32      bStringHash of the texture name
#       +0x04  u16      width
#       +0x06  u16      height
#       +0x08  u16      format (0 = RGB565, 1 = RGBA4)
#       +0x0a  u16      material (see texture_material)
#       +0x0c  u32      offset into the payload
#       +0x10  u32      byte length there
#       +0x14  u16      maxLevel (mip levels below level 0; 0 = no mip chain)
#       +0x16  s16      scrollTimeStep  } the TPK record's UV scroll, copied as
#       +0x18  s16      scrollSpeedS    } it is (see world_material); all zero
#       +0x1a  s16      scrollSpeedT    } for a texture that does not scroll,
#       +0x1c  s16      offsetS         } which is also what a bank written
#       +0x1e  s16      offsetT         } before TASKS.md #269 holds there
BANK_NAME = "WORLD.NFSB"
BANK_MAGIC = b"NFSB"
# 2: the u16 at +0x0a carries the TPK material state instead of reading zero.
# A version-1 bank has no material and would draw every neon barrier opaque,
# so world.c rejects one rather than silently rendering the old way.
# 3: the u16 at +0x14 carries maxLevel and `length` covers the whole mip chain
# rather than just level 0. A v2 bank read as v3 would be fine (maxLevel reads
# 0 from the reserved field), but a v3 bank read as v2 would upload a chain as
# if it were one image, so world.c requires the exact version.
BANK_VERSION = 3
BANK_HDR_SIZE = 0x40
BANK_RECORD_SIZE = 0x20
# The header's +0x20 (reserved, zero, in every bank before it) is the byte
# length of the texture-animation table (TASKS.md #326), which sits between
# the record table and the payload -- so a bank without one is laid out
# exactly as before. world_archive.py copies it with the records.
#   u32 count, then per animation:
#     +0x00  u32  base hash (the TextureAnim record's, = frame 0)
#     +0x04  u16  frames per second
#     +0x06  u8   frame count n
#     +0x07  u8   timeBase: 0 WorldTimeFrames (pauses), 1 RealTimeFrames
#     +0x08  u32  frame hash[n], frame 0 first
TEX_ANIM_MAX_FRAMES = 32

# The retail texture animations: chunk 0xB0300100 in a stream section, a
# 0x30300101 header, 0x30300102 TextureAnim records (0x34 bytes: name[0x18],
# hash, numFrames, fps, timeBase, then runtime fields) and 0x30300103 entries
# (16 bytes, the frame's texture hash first), each record's frames following
# the last's. UpdateTextureAnimations (Speed.elf 0x800cf2a4) gives entry i the
# pixels of entry (i + frame) mod n, frame = int(frames60 * fps / 60) mod n,
# so the base hash every model names shows frame k.
TEX_ANIM_RECORDS = 0x30300102
TEX_ANIM_ENTRIES = 0x30300103

# member hash -> the frame hashes of its animation, set by main() for
# _flush_blob(): a section that names any frame carries every frame in its
# texture table, unused by its batches, so the world's refcounted residency
# (world.c) keeps all of them up while the section is and model3d.c can swap
# between them by slot.
TEX_ANIM_FRAMES = {}


# The render state a texture carries in its TPK record, packed into the one
# u16 both the bank record and the section's own TexEntry already reserve.
# extract_global_textures.parse_container() documents where each byte comes
# from and what the values mean; this is only the packing.
#
#     bit 0-1   alphaBlendType  0 opaque, 1 normal blend, 2 additive
#     bit 2-3   alphaUsageType  0 unused, 1 punchthrough, 2 real alpha
#     bit 4     writeDepth
#     bit 5-7   renderingOrder, clamped to 7 (the highest value shipped)
#
# A blend type outside 0..2 (the four HUD outliers) is carried as a normal
# blend: that is the commonest state and the conservative one, and the note in
# parse_container() says why guessing at 3/6 is not on.
MAT_BLEND_OPAQUE = 0
MAT_BLEND_NORMAL = 1
MAT_BLEND_ADDITIVE = 2

# A world texture's material also says whether it scrolls: bits 8-9 are the
# TPK record's scrollType (TASKS.md #269), and the five numbers that go with
# it ride in the bank record (+0x16..+0x1f above), where the runtime reads
# them once at load. Only world art takes this: car_pack's texture_material()
# is shared with the cars and the shadow, none of which scroll.
MAT_SCROLL_SHIFT = 8
NO_SCROLL = (0, 0, 0, 0, 0)


def world_material(t):
    st = t.get("scroll_type", 0)
    mat = texture_material(t)
    if st in (1, 2, 3):
        mat |= st << MAT_SCROLL_SHIFT
    return mat


def world_scroll(t):
    return tuple(t["scroll"]) if t.get("scroll_type", 0) in (1, 2, 3) else NO_SCROLL


# SceneryInstance +0x1a flags. The low byte is what the scenery renderer reads:
# ScenerySectionHeader::DrawAScenery (Speed.elf 0x80208fa4) skips an instance
# when `(flags ^ 0x60) & mask` is non-zero, and SetupSceneryCullInfo
# (0x800c3c9c) builds `mask` per view (TASKS.md #237):
#
#   0x0010  always in the mask: never drawn. This is the scenery-group bit --
#           EnableGroup (0x80196bd8) clears it on every instance of a group it
#           turns on and CloseTopologyAndSceneryGroups (0x80196ec0) sets it
#           again. See SceneryGroups below.
#   0x0002  in the mask for the player views (eView +4 of 1 or 2): an instance
#           with it is never drawn where the player can see it.
#   0x0008  in the mask when TheRaceParameters +0x04 is 0, a race run forwards
#           and free roam; exclusive to ZPM_*_BW_StartFinGraphic* (77/77).
#   0x0004  in the mask when it is 1, a race run backwards; exclusive to
#           ZPM_*_FW_StartFinGraphic* (89/89).
#   0x0001  in the mask for view modes 3 and 4; no L4RA instance sets it.
#   0x0020  inverted (the `^ 0x60`): the rear-view mirror's mask (view 3) has
#           it, so only instances carrying it are drawn there.
#   0x0040  inverted the same way, for the passes whose mask has 0x1800:
#           the six env-map faces (views 0x10..0x15, TASKS.md #166), whose
#           SetupSceneryCullInfo mask is 0x1000 | 0x40 | 0x10. So an instance
#           is in a car's reflection only if it carries 0x40 and not 0x10;
#           0x02 is not in that mask, so the traffic lights the player view
#           never draws do turn up in it.
#   0x0400  for the env faces' pass, DrawAScenery takes the B model (+0x34)
#           where it would otherwise take the Z one (+0x38) -- the 2,942
#           terrain instances (flags 0x560) of L4RA's 14,789 env ones.
#
# The port draws the player view and the env faces, so 0x20 and 0x01 never
# decide anything here.
#
#   0x0200  carried by exactly 3 models, all track barriers --
#           XO_TrackBarrier_Player_00 (3828), XO_TrackBarrierB_A_1a_00 (1680),
#           XO_TrackBarrierB_B_1a_00 (114). The renderer does not read it; what
#           hides a barrier is its 0x10 and its group. Free roam used to be
#           packed by dropping this bit, which also dropped the barriers retail
#           DOES draw there: the career stage's own (BARRIERS_CAREER<n>).
INST_FLAG_NOT_PLAYER_VIEW = 0x0002
INST_FLAG_STARTFIN_FW     = 0x0004
INST_FLAG_STARTFIN_BW     = 0x0008
INST_FLAG_GROUP_HIDDEN    = 0x0010
INST_FLAG_ENV_VIEW        = 0x0040
INST_FLAG_TRACK_BARRIER   = 0x0200
INST_FLAG_ENV_USE_B       = 0x0400


# Street furniture you are meant to knock over rather than be stopped dead by.
#
# The port bakes every placed prop into the same merged geometry the collision
# scans read, so a road sign is exactly as solid as a brick wall -- the user
# drove into a signal-ahead sign and simply stopped (TASKS.md #183). Retail
# NFSU2 knocks these over and takes almost no speed off the car. There is no
# prop dynamics here to knock anything over WITH, so the closest honest
# approximation is to stop the sign blocking the car at all: it stays standing
# and the car drives through it. Wrong to look at, right to drive through, and
# the alternative -- keeping it solid -- is wrong in the way the user actually
# reported.
#
# Picking WHICH props by the instance flags at +0x1a does not work, and this is
# the thing that had to be checked before writing any rule: the sign that was
# hit, XS_SignalAhead_1a_00, carries 0x0060, and so do guardrails, wall guards
# and brick fences. Excluding on that bit would punch holes in every guardrail
# in the city and undo #180. Picking by footprint does not work either -- a
# 1.5 m cutoff catches the sign but also XW_Guardrail_A_1a_FWYEND_00 (0.92 m)
# and the wall-guard end caps, i.e. the ends of continuous barrier runs.
#
# What does separate cleanly is EA's own model-name prefix. Tabulated over all
# 48,257 instances in L4RA's 392 sections, `XS_` is 395 models and 1,505
# instances (3.1%) and it is signage throughout: road signs (SignalAhead,
# Speed35, WarnCurve, Chevron, NoParkAnytime), freeway gantry boards
# (Freeway_Green_B and friends) and building signage (BestBuy, Cingular,
# Pontiac). It contains no walls, no fences, no guardrails and no barriers --
# those are XW_ (179 models, 12,971 instances), XO_ (380 / 17,846) and TRN_.
#
# This is deliberately narrow. Traffic cones, bins and parking meters live in
# XO_ alongside bollards and lamp posts, which are NOT all knock-over props, so
# XO_ is left solid until it has been measured to the same standard.
SOFT_NAME_PREFIX = "XS_"

# The batch flag that says "draw this, never collide with it". The retail
# sub-mesh flags field at +0x38 is carried through into the blob's batch
# records untouched, so the bit borrowed here has to be one retail never sets:
# over all 28,018 sub-meshes in L4RA it takes exactly four values -- 0x00000000
# (22785), 0x00000040 (5090), 0x00004180 (141) and 0x00001000 (2) -- and not
# one of them sets any bit above 16. Bit 31 is free.
BATCH_NO_COLLIDE = 0x80000000


def is_soft_model(name):
    """True for a model the car should drive through rather than hit."""
    return name.startswith(SOFT_NAME_PREFIX)


# Retail's scenery groups (TASKS.md #237). Which race barriers, start/finish
# lines and free-roam shop beams are on screen is decided at runtime, so the
# instances that depend on it are baked like any other and tagged: a batch's
# flags carry, in bits 16-27, the id of the "set" its instances share -- 0 for
# always drawn -- and WORLD.NFSG lists what each set needs. The runtime
# (source/scenery.c) turns sets on and off the way InitTopologyAndSceneryGroups
# turns groups on. The bits are free for the same reason bit 31 is (see
# BATCH_NO_COLLIDE); bits 24-28 mean a car's paint slot, but only on a lit
# model, and a world section is baked.
#
# The data, all in <track>.BUN:
#   0x00034107  SceneryOverrideInfo, 8 bytes: i16 section number, i16 instance
#               index in that section's scenery, u16 the instance's flags, u16
#               -. Every instance a group can switch has one.
#   0x00034108  the groups, one after another: 8 bytes of list links, the name
#               (32 bytes, NUL-padded), u32 bStringHash of it, u32 enabled
#               (runtime), u32 count, u16 override index[count], padded to 4.
#               LoaderSceneryGroup (0x80207bbc) walks it.
#   0x00034105  in each section's 0x80034100 group: (u16 override index, u16
#               instance index) pairs. LoaderScenery (0x80207d50) copies the
#               override's flags onto the instance when both indices agree.
# The PC data already has every override's flags baked into its instance (all
# 6431 of L4RA's agree), so only the group membership is new information.
SCENERY_PAIRS_TAG = 0x00034105
SCENERY_OVERRIDE_TAG = 0x00034107
SCENERY_GROUP_LIST_TAG = 0x00034108
BATCH_SCENERY_SET_SHIFT = 16
BATCH_SCENERY_SET_MAX = 0xFFF
NFSG_VERSION = 1


class SceneryGroups(object):
    """The track's scenery groups and the visibility sets built from them.

    A set is what one baked batch needs to be drawn: `hidden` instances (flag
    0x10) need any one of `groups` on, and `direction` (0x04/0x08) must not be
    the start/finish line facing the other way. Ids are assigned walking the
    override table in order, so they do not depend on which sections a run
    converts -- a `--sections` repack writes ids the rest of the world agrees
    with.
    """

    def __init__(self, bun_path):
        blob = open(bun_path, "rb").read()
        self.overrides = []
        self.names = []
        self.members = {}   # override index -> group indices
        self.anims = world_anim.WorldAnims(bun_path)
        for tag, size, off in _top_chunks(blob):
            if tag == SCENERY_OVERRIDE_TAG:
                self.overrides = [struct.unpack_from("<hhH", blob, off + i * 8)
                                  for i in range(size // 8)]
            elif tag == SCENERY_GROUP_LIST_TAG:
                o = off
                while o + 0x34 <= off + size:
                    name = blob[o + 8:o + 0x28].split(b"\0")[0].decode("ascii")
                    h, _en, n = struct.unpack_from("<III", blob, o + 0x28)
                    if bstring_hash(name) != h:
                        raise ValueError("scenery group %r: hash %08x" % (name, h))
                    g = len(self.names)
                    self.names.append(name)
                    for oi in struct.unpack_from("<%dH" % n, blob, o + 0x34):
                        self.members.setdefault(oi, []).append(g)
                    o += (n * 2 + 0x37) & ~3
        self.sets = [(False, (), 0)]   # id 0: drawn whatever the groups
        self.set_ids = {self.sets[0]: 0}
        for oi, (_sec, _inst, flags) in enumerate(self.overrides):
            self._key(oi, flags, register=True)
        for d in (INST_FLAG_STARTFIN_FW, INST_FLAG_STARTFIN_BW):
            self._register((False, (), d))
        if len(self.sets) - 1 > BATCH_SCENERY_SET_MAX:
            raise ValueError("%d scenery sets do not fit the batch flags"
                             % len(self.sets))

    def _register(self, key):
        if key not in self.set_ids:
            self.set_ids[key] = len(self.sets)
            self.sets.append(key)
        return self.set_ids[key]

    def _key(self, oi, flags, register=False):
        hidden = bool(flags & INST_FLAG_GROUP_HIDDEN)
        groups = tuple(sorted(self.members.get(oi, ()))) if hidden else ()
        key = (hidden, groups, flags & (INST_FLAG_STARTFIN_FW | INST_FLAG_STARTFIN_BW))
        if register and (hidden and groups or key[2]):
            self._register(key)
        return key

    def _effective(self, inst):
        """(override index or None, the flags DrawAScenery reads)."""
        ref = inst.get("override")
        if ref is not None and ref[0] < len(self.overrides):
            osec, oinst, oflags = self.overrides[ref[0]]
            if (osec, oinst) == ref[1:]:
                # AssignOverrides: the override's flags become the instance's.
                # All of L4RA's 6431 already agree with what the PC data baked.
                return ref[0], oflags
        return None, inst["flags"]

    def flags_for(self, inst):
        return self._effective(inst)[1]

    def set_for(self, inst, info, env=False):
        """The instance's set id, 0 for always drawn, None for never drawn.

        `env` asks for the env faces' view instead of the player's: the same
        but for 0x02, which only the player views' mask holds.

        Never drawn is what retail's player view does with an instance it
        masks out whatever the game state: 0x02 (XO_TrafficLightC_1a_00 and
        XO_Banderolle_1a_00, 78 in L4RA), or 0x10 with no group to clear it.

        The one exception is port-side. Most of the 0x10-without-a-group
        instances are where a world animation's object stands -- the shop
        doors, the elevated train, the cranes, the sweeping spotlights -- and
        retail draws those through the animation system (CWorldAnimEntity,
        drawing its own model), not the scenery renderer. The moving ones are
        the port's worldanim.c now (TASKS.md #239, world_anim.py), and so are
        the shop doors since the shop NIS (#408: a door tree holds frame 0
        until a DOOR_POSITION event names its trigger zone), so all those
        instances are left out like retail leaves them. The roots' models (ANM_DoorAnim_RB_00,
        ANM_TowerCraneArm_1a_99xx) no entity draws (+20 == the node's name),
        and are left out with the rest.
        """
        oi, flags = self._effective(inst)
        if flags & INST_FLAG_NOT_PLAYER_VIEW and not env:
            return None
        key = self._key(oi, flags)
        if key[0] and not key[1]:
            return None
        if not key[0] and not key[2]:
            return 0
        return self.set_ids.get(key)

    def write(self, out_dir, track):
        """WORLD.NFSG: what the runtime needs to turn sets on and off.

            +0   "NFSG"
            +4   u32 version
            +8   u32 group count
            +12  u32 set count, set 0 included
            +16  group count x 32 bytes: the name, NUL-padded
            then set count x { u8 hidden, u8 direction bits, u16 n,
                               u16 group index[n] }, each padded to 4
        """
        body = bytearray(struct.pack("<4sIII", b"NFSG", NFSG_VERSION,
                                     len(self.names), len(self.sets)))
        for name in self.names:
            body += name.encode("ascii").ljust(32, b"\0")[:32]
        for hidden, groups, direction in self.sets:
            rec = struct.pack("<BBH", int(hidden), direction, len(groups))
            rec += struct.pack("<%dH" % len(groups), *groups)
            body += rec + b"\0" * (-len(rec) % 4)
        path = os.path.join(out_dir, "WORLD.NFSG")
        tmp = path + ".tmp"
        with open(tmp, "wb") as fh:
            fh.write(body)
        os.replace(tmp, path)
        return len(body)


def _top_chunks(blob):
    """(tag, size, payload offset) for every chunk, containers descended."""
    out = []

    def walk(o, end, depth):
        while o + 8 <= end:
            tag, size = struct.unpack_from("<II", blob, o)
            if o + 8 + size > end:
                return
            if tag & 0x80000000 and depth < 4:
                walk(o + 8, o + 8 + size, depth + 1)
            else:
                out.append((tag, size, o + 8))
            o += 8 + size

    walk(0, len(blob), 0)
    return out


# --mode: "retail" draws what the player view of retail draws, with the
# group-dependent instances tagged for the runtime; "all" places every instance
# the sections ship, untagged, for looking at the raw data.
PACK_MODES = ("retail", "all")


# One packing for every texture's render state; car_pack.py owns it because
# car textures carry it too (TASKS.md #205).
texture_material = car_pack.texture_material


class TextureBank(object):
    """Every distinct world texture, encoded once, in a file of its own.

    Embedding textures in each .nfsc is right for a car or a frontend room --
    one file, one self-contained thing to load. It is badly wrong for a world:
    L4RA places 1562 distinct textures but its sections between them reference
    those 20286 times, so the same road, kerb and brick art gets encoded into
    section after section. Whole-world that is a **13x** duplication -- roughly
    400 MB of pixels to carry 31 MB of distinct ones.

    It costs at runtime too, and that is the part that actually matters: with
    the pixels inside the section, paging a section in re-uploads textures that
    a neighbouring resident section already has sitting in VRAM. Hoisting them
    into one bank and refcounting residency (world.c) means a shared road
    texture is uploaded once for however many resident sections want it, and
    freed when the last of them drops.

    Sections still carry their own texture *table* -- hash, dimensions, format
    -- so a batch's texture index stays a local slot number and nothing about
    the .nfsc layout changes. Only the pixel payload moves out.
    """

    def __init__(self, etc1=False):
        self.entries = {}   # hash -> (w, h, fmt, pixels, mat, maxlvl)
        self.names = {}
        self.scroll = {}    # hash -> world_scroll(), for the ones that scroll
        # --etc1 (TASKS.md #427): ETC1 / ETC1A4 instead of RGB565 / RGBA4444.
        # The encodes are deferred to write(), where they run as one parallel
        # batch; `pending` holds their downscaled RGBA until then.
        self.etc1 = etc1
        self.pending = {}
        self.anims = []     # (base, fps, timeBase, frames) -- TASKS.md #326

    def add(self, th, name, w, h, rgba, maxdim, mat=0, scroll=NO_SCROLL):
        """Encode `th` if this is the first sight of it; return (w, h, fmt, mat)."""
        hit = self.entries.get(th)
        if hit is not None:
            return hit[0], hit[1], hit[2], hit[4]
        if scroll != NO_SCROLL:
            self.scroll[th] = scroll
        rgba, w, h = car_pack.box_downscale(rgba, w, h, maxdim)
        # World art is the whole reason mipmapping exists in this port: it is
        # the only thing drawn at every distance from under the bumper to the
        # far end of a block, so it is where a single bilinear tap aliases.
        if self.etc1:
            maxlvl = car_pack.mip_max_level(w, h)
            fmt = etc1_tex.pick_fmt(rgba)
            self.entries[th] = (w, h, fmt, None, mat, maxlvl)
            self.pending[th] = bytes(rgba)
        else:
            fmt, pixels, maxlvl = car_pack.encode_texture_chain(rgba, w, h)
            self.entries[th] = (w, h, fmt, pixels, mat, maxlvl)
        self.names[th] = name
        return w, h, fmt, mat

    def encode_pending(self):
        if not self.pending:
            return
        keys = sorted(self.pending)
        jobs = []
        for th in keys:
            w, h, fmt, _, _, maxlvl = self.entries[th]
            jobs.append((self.pending[th], w, h, maxlvl, fmt))
        for th, (fmt, pixels) in zip(keys, etc1_tex.encode_many(jobs)):
            w, h, want, _, mat, maxlvl = self.entries[th]
            assert fmt == want, (th, fmt, want)
            self.entries[th] = (w, h, fmt, pixels, mat, maxlvl)
        print("%s: %d texture(s) encoded ETC1/ETC1A4" % (BANK_NAME, len(keys)))
        self.pending = {}

    def write(self, out_dir, track):
        self.encode_pending()
        order = sorted(self.entries)
        payload = bytearray()
        recs = []
        for th in order:
            w, h, fmt, pixels, mat, maxlvl = self.entries[th]
            recs.append(struct.pack("<IHHHHIIH5h", th, w, h, fmt, mat,
                                    len(payload), len(pixels), maxlvl,
                                    *self.scroll.get(th, NO_SCROLL)))
            assert len(recs[-1]) == BANK_RECORD_SIZE, len(recs[-1])
            payload.extend(pixels)
        anim = b""
        if self.anims:
            anim = struct.pack("<I", len(self.anims))
            for base, fps, tbase, frames in self.anims:
                anim += struct.pack("<IHBB%dI" % len(frames), base, fps,
                                    len(frames), tbase, *frames)
        payload_off = BANK_HDR_SIZE + len(recs) * BANK_RECORD_SIZE + len(anim)
        hdr = struct.pack("<4sIIIII8sI28x", BANK_MAGIC, BANK_VERSION, len(recs),
                          BANK_RECORD_SIZE, payload_off, len(payload),
                          track.encode("ascii"), len(anim))
        assert len(hdr) == BANK_HDR_SIZE, len(hdr)
        with open(os.path.join(out_dir, BANK_NAME), "wb") as fh:
            fh.write(hdr)
            for r in recs:
                fh.write(r)
            fh.write(anim)
            fh.write(payload)
        for base, fps, tbase, frames in self.anims:
            print("%s: texture animation %s, %d frame(s) at %d fps"
                  % (BANK_NAME, self.names.get(base, "%08x" % base), len(frames), fps))
        base = sum(etc1_tex.level0_bytes(e[0], e[1], e[2]) if etc1_tex.is_etc(e[2])
                   else car_pack.encode_texture_level0_size(e[0], e[1], e[2])
                   for e in self.entries.values())
        print("%s: %d texture(s), %.1f MB (%.1f MB level 0 + %.1f MB mips), %d scrolling"
              % (BANK_NAME, len(recs), len(payload) / 1048576.0,
                 base / 1048576.0, (len(payload) - base) / 1048576.0,
                 len(self.scroll)))
        return len(payload)


class Section(object):
    __slots__ = ("name", "number", "offset", "size", "resident", "ident", "hash")

    def __init__(self, rec):
        self.name = rec[:8].split(b"\x00")[0].decode("ascii", "replace")
        (self.number, _zero, _one, self.offset, self.size,
         self.resident, self.ident) = struct.unpack_from("<7I", rec, 8)
        self.hash, = struct.unpack_from("<I", rec, 0x30)

    def __repr__(self):
        return "<Section %s #%d @0x%x+0x%x>" % (self.name, self.number,
                                                self.offset, self.size)


def find_leaf(data, tag):
    """Return (payload_offset, size) of the first `tag` chunk, walking containers."""
    def walk(base, end):
        o = base
        while o + 8 <= end:
            t, s = struct.unpack_from("<II", data, o)
            if s < 0 or o + 8 + s > end:
                return None
            if t == tag:
                return (o + 8, s)
            if t & 0x80000000:
                hit = walk(o + 8, o + 8 + s)
                if hit:
                    return hit
            o += 8 + s
        return None
    return walk(0, len(data))


def read_section_table(bun_path):
    data = open(bun_path, "rb").read()
    hit = find_leaf(data, SECTION_TABLE_TAG)
    if not hit:
        raise SystemExit("%s: no 0x%08x section table" % (bun_path, SECTION_TABLE_TAG))
    off, size = hit
    if size % SECTION_RECORD_SIZE:
        raise SystemExit("section table size 0x%x is not a multiple of 0x%x"
                         % (size, SECTION_RECORD_SIZE))
    out = []
    for i in range(size // SECTION_RECORD_SIZE):
        s = Section(data[off + i * SECTION_RECORD_SIZE:
                         off + (i + 1) * SECTION_RECORD_SIZE])
        if s.size and s.name != "--":
            out.append(s)
    return out


def read_section(stream, sec):
    stream.seek(sec.offset)
    return stream.read(sec.size)


def read_track_mesh(data, part):
    """(verts, indices, subsets) for one world part, at whichever FVF it uses.

    Emits the scenery vertex shape either way: the second float3 is the baked
    D3DCOLOR RGB, which both FVFs carry (a 36-byte part's normal is dropped --
    a world section is drawn by scene.v.pica, which has no light to give it).
    """
    m = part["mesh"]
    vs = skip_alignment_padding(data, *m["vb"])
    avail = m["vb"][1] - (vs - m["vb"][0])
    stride = SCENE_STRIDE
    if m["numVerts"] and avail // m["numVerts"] >= CAR_STRIDE:
        stride = CAR_STRIDE
    # Re-anchor now that the record size is known: the run-length scan above
    # eats a leading 0x11 that belongs to the first vertex (see leaf_start()).
    vs = leaf_start(data, m["vb"][0], m["vb"][1], stride)
    colour_off = 12 if stride == SCENE_STRIDE else 24
    uv_off = 16 if stride == SCENE_STRIDE else 28

    verts = []
    for i in range(m["numVerts"]):
        b = vs + i * stride
        x, y, z = struct.unpack_from("<3f", data, b)
        colour, = struct.unpack_from("<I", data, b + colour_off)
        u, v = struct.unpack_from("<2f", data, b + uv_off)
        verts.append((x, y, z,
                      ((colour >> 16) & 0xff) / 255.0,
                      ((colour >> 8) & 0xff) / 255.0,
                      (colour & 0xff) / 255.0,
                      u, v))

    istart = leaf_start(data, m["ib"][0], m["ib"][1], 2)
    indices = list(struct.unpack_from("<%dH" % (m["numTris"] * 3), data, istart))

    sstart = leaf_start(data, m["subsets"][0], m["subsets"][1],
                        car_pack.SUBSET_STRIDE)
    subsets = []
    for i in range(m["numSubsets"]):
        b = sstart + i * car_pack.SUBSET_STRIDE
        index_count, = struct.unpack_from("<I", data, b + 0x0c)
        tex_slot, = struct.unpack_from("<I", data, b + 0x1c)
        first_index, = struct.unpack_from("<I", data, b + 0x34)
        flags, = struct.unpack_from("<I", data, b + 0x38)
        subsets.append((first_index, index_count, tex_slot, flags))
    return verts, indices, subsets


def _index_tpks(data, wanted, out, maxdim, label, verbose=False):
    """Decode every wanted-and-not-yet-resolved texture in one buffer's TPKs.

    `data` is any buffer that might carry 0xb3300000 containers -- a stream
    section, or a whole `GLOBAL/*.BUN`. Returns the number newly resolved.
    """
    if b"\x00\x00\x30\xb3" not in data:
        return 0
    found = 0
    for coff, csize in egt.find_containers(data):
        try:
            parsed = egt.parse_container(data, coff, csize)
        except Exception:
            continue
        for t in parsed["textures"]:
            h = t["hash"]
            if h not in wanted or h in out:
                continue
            if not t["width"] or not t["height"] or not t["pixel_data"]:
                continue
            px = t["pixel_data"][:t["expected_size"]]
            try:
                if t["fourcc"] == b"RAWB":
                    rgba = decode_bgra(px, t["width"], t["height"])
                else:
                    rgba = decode_dxt(px, t["width"], t["height"], t["fourcc"])
            except Exception:
                continue
            # The TPK record says outright whether this texture's alpha channel
            # is coverage (alphaUsageType 1 = punchthrough, 2 = real alpha) or
            # not used at all (0), so that replaces scene_pack.force_opaque()'s
            # "is the max alpha 255?" guess for world art. A type-0 texture's
            # alpha is a gloss/detail mask the retail renderer never blends
            # with, so it is flattened; the other two keep their real alpha.
            if t["alpha_usage"] == 0:
                rgba = scene_pack.force_opaque(rgba, always=True)
            rgba, tw, th = car_pack.box_downscale(rgba, t["width"],
                                                  t["height"], maxdim)
            out[h] = (t["name"], tw, th, rgba, world_material(t), world_scroll(t))
            found += 1
            if verbose:
                print("  tex 0x%08x %-28s %dx%d -> %dx%d  from %s"
                      % (h, t["name"], t["width"], t["height"], tw, th, label))
    return found


def global_tpk_files(global_dir):
    """The `GLOBAL/` files worth scanning for a track texture, in the order
    they are most likely to hold one.

    A world section can reference art it does not ship: `GLOBALB.BUN` carries
    the always-resident pack, and the `InGame*.bun` overlays carry the
    per-mode ones (free-roam props, drag/drift dressing). None of them is a
    stream section, so the section pass cannot see them.
    """
    if not global_dir or not os.path.isdir(global_dir):
        return []
    preferred = ["GLOBALB.BUN", "GLOBALA.BUN", "InGameFreeRoam.bun",
                 "InGameCommon.bun", "InGameRace.bun", "InGameDrag.bun",
                 "InGameDrift.bun"]
    have = {}
    for n in os.listdir(global_dir):
        if n.lower().endswith((".bun", ".bin", ".tpk")):
            have[n.lower()] = os.path.join(global_dir, n)
    files = []
    for n in preferred:
        p = have.pop(n.lower(), None)
        if p:
            files.append(p)
    files.extend(p for _, p in sorted(have.items()))
    return files


def read_texture_anims(stream, sections):
    """base hash -> (name, fps, timeBase, [frame hashes]) for every retail
    texture animation in the stream's sections (see TEX_ANIM_RECORDS)."""
    out = {}
    for sec in sections:
        data = read_section(stream, sec)
        if struct.pack("<I", TEX_ANIM_RECORDS) not in data:
            continue
        chunks = _top_chunks(data)
        recs = [c for c in chunks if c[0] == TEX_ANIM_RECORDS]
        ents = [c for c in chunks if c[0] == TEX_ANIM_ENTRIES]
        if len(recs) != 1 or len(ents) != 1:
            continue
        _, rsize, ro = recs[0]
        _, esize, eo = ents[0]
        k = 0
        for i in range(rsize // 0x34):
            o = ro + i * 0x34
            name = data[o:o + 0x18].split(b"\0")[0].decode("ascii", "replace")
            h, n, fps, tbase = struct.unpack_from("<4I", data, o + 0x18)
            if not (1 < n <= TEX_ANIM_MAX_FRAMES) or (k + n) * 16 > esize:
                break
            frames = [struct.unpack_from("<I", data, eo + (k + j) * 16)[0]
                      for j in range(n)]
            k += n
            out.setdefault(h, (name, fps, tbase, frames))
    return out


def build_texture_index(stream, sections, wanted, maxdim, verbose=False,
                        global_dir=None):
    """hash -> (name, w, h, rgba) for every wanted hash found in any section's
    TPK, falling back to `GLOBAL/` for the ones no section carries.

    One pass over the whole stream file; only wanted hashes are decoded, which
    keeps this to a few hundred textures out of the thousands the world ships.

    The downscale to `maxdim` happens here rather than at pack time because a
    whole-world run wants 1562 of these live at once: at retail resolution that
    is well over a gigabyte of decoded RGBA held for the length of the run,
    against about a hundred megabytes once they are cut to 128px -- and every
    consumer downscales them to exactly this anyway.

    The `GLOBAL/` pass runs only on what the stream could not resolve, so a
    run whose art all lives in the stream pays nothing for it.
    """
    out = {}
    for sec in sections:
        if len(out) == len(wanted):
            break
        _index_tpks(read_section(stream, sec), wanted, out, maxdim, sec.name,
                    verbose)

    if len(out) < len(wanted):
        for path in global_tpk_files(global_dir):
            if len(out) == len(wanted):
                break
            try:
                with open(path, "rb") as fh:
                    data = fh.read()
            except OSError:
                continue
            n = _index_tpks(data, wanted, out, maxdim,
                            os.path.basename(path), verbose)
            if n:
                print("  %d texture(s) resolved from GLOBAL/%s"
                      % (n, os.path.basename(path)))
    return out


def bstring_hash(name):
    """NFS's bStringHash: h = 0xffffffff, then h = h * 33 + c over the
    upper-cased name. This is what a SceneryInfo stores to name its model and
    what a part record stores at +0x10, and it is the only reliable key between
    the two -- a part record's name field is clipped at 27 characters."""
    h = 0xffffffff
    for c in name.upper():
        h = (h * 33 + ord(c)) & 0xffffffff
    return h


def read_scenery_groups(data):
    """[(infos, instances)] for every 0x80034100 group in a section.

    A SceneryInfo (68 bytes) is a model reference: a 32-byte name followed at
    +0x20 by the bStringHash of the same name at each LOD -- [0] is the A (near)
    model, [1] the B, [2] the Z, and 0 where that LOD does not exist. +0x2c
    holds two u16s, +0x30/+0x34/+0x38 the A/B/Z model pointers LoaderScenery
    fills in at runtime, and +0x3c an f32 bounding radius about the model's
    origin.

    A SceneryInstance (64 bytes) places one of them:

        +0x00  f32[6]  world-space bounding box, min then max
        +0x18  u16     index into this group's SceneryInfo list
        +0x1a  u16     flags
        +0x1c  u32     runtime instance id (indexes the 0x00034106 scratch
                       array; NOT a model index, and it repeats)
        +0x20  f32[3]  translation
        +0x2c  i16[9]  3x3 rotation, 1.13 fixed, stored **column-major**
                       (verified against the stored bounding box)
        +0x3e  i16     pad

    Terrain instances are identity with a zero translation, because terrain
    vertices are already absolute world space; only props carry a real
    transform.
    """
    groups = []

    def walk(base, end):
        o = base
        while o + 8 <= end:
            t, sz = struct.unpack_from("<II", data, o)
            if sz < 0 or o + 8 + sz > end:
                return
            if t == SCENERY_GROUP_TAG:
                groups.append(read_scenery_group(data, o + 8, o + 8 + sz))
            elif t & 0x80000000:
                walk(o + 8, o + 8 + sz)
            o += 8 + sz

    walk(0, len(data))
    return [g for g in groups if g]


def read_scenery_group(data, base, end):
    chunks = {}
    o = base
    while o + 8 <= end:
        t, sz = struct.unpack_from("<II", data, o)
        if sz < 0 or o + 8 + sz > end:
            break
        chunks[t] = (o + 8, sz)
        o += 8 + sz
    if SCENERY_INFO_TAG not in chunks or SCENERY_INSTANCE_TAG not in chunks:
        return None

    off, sz = chunks[SCENERY_INFO_TAG]
    start = skip_alignment_padding(data, off, sz)
    infos = []
    for i in range((sz - (start - off)) // SCENERY_INFO_STRIDE):
        b = start + i * SCENERY_INFO_STRIDE
        infos.append({
            "name": data[b:b + 32].split(b"\x00")[0].decode("ascii", "replace"),
            "lods": struct.unpack_from("<3I", data, b + 0x20),
            # A bounding radius about the model's own origin -- which is the
            # world origin for terrain, so those read 3800-4045 m -- that
            # DrawAScenery sizes the instance on screen with (SCENERY_LOD_*).
            "radius": struct.unpack_from("<f", data, b + 0x3c)[0],
        })

    off, sz = chunks[SCENERY_INSTANCE_TAG]
    start = skip_alignment_padding(data, off, sz)
    instances = []
    by_index = {}
    for i in range((sz - (start - off)) // SCENERY_INSTANCE_STRIDE):
        b = start + i * SCENERY_INSTANCE_STRIDE
        idx, flags = struct.unpack_from("<2H", data, b + 0x18)
        if idx >= len(infos):
            continue
        inst = {
            "info": idx,
            "flags": flags,
            "box": struct.unpack_from("<6f", data, b),
            "pos": struct.unpack_from("<3f", data, b + 0x20),
            "rot": [v / ROT_ONE for v in struct.unpack_from("<9h", data, b + 0x2c)],
            "override": None,
        }
        instances.append(inst)
        by_index[i] = inst

    # The scenery-group overrides that name an instance here (TASKS.md #237).
    # LoaderScenery (Speed.elf 0x80207d50) reads the section number from the
    # header, which it finds at (chunk + 0x17) & ~0xf -- the same alignment
    # holds section-relative because every section sits 128-aligned in the
    # stream -- and applies a 0x00034105 (override, instance) pair only when
    # the override names this section and this instance. That check is
    # SceneryGroups.set_for()'s, so each instance carries the three numbers.
    if SCENERY_HEADER_TAG in chunks and SCENERY_PAIRS_TAG in chunks:
        hoff, _hsz = chunks[SCENERY_HEADER_TAG]
        section = struct.unpack_from("<i", data, ((hoff - 8 + 0x17) & ~0xF) + 0xC)[0]
        poff, psz = chunks[SCENERY_PAIRS_TAG]
        for p in range(psz // 4):
            oi, ii = struct.unpack_from("<2H", data, poff + p * 4)
            if ii in by_index:
                by_index[ii]["override"] = (oi, section, ii)
    return (infos, instances)


# Which detail level a placement was baked at. The retail scenery info record
# names the same model at three of them -- A (near), B (mid), Z (far), 0 where
# a level is absent -- and EA authored the chain for the whole city: of L4RA's
# 42,469 freeroam instances every single one ships a B and 68% ship a Z. This
# converter used to take info["lods"][0] for all of them at every distance and
# throw the other two away, so the port drew the entire world at maximum detail
# from any range (TASKS.md #173). Measured over the whole world, the far pick
# is 59.5% fewer triangles and 57.7% fewer vertices than the near one.
#
# Which of them retail draws is Speed.elf's DrawAScenery (0x80208fa4), decided
# per instance per frame from the instance's size on screen (TASKS.md #247):
#
#     R = SceneryInfo +0x3c + 6.0
#     d = |instance translation - camera|
#     behind the camera, dot(d, forward) < -R          not drawn
#     P = (int)(R * H / (d - R)) if d - R > R else (int)H,   H = 256 / tan(fov/2)
#     P <= 16                                          not drawn
#     A if A.polys <= 39, or A.volume > 0 and P / max(A.density, 6) >= 9.5
#     B otherwise, and nothing at all if the B slot is empty
#
# Z is never drawn in the player view; only the reflection and shadow views
# (view flags 0x800/0x1000) take it. The old far tier here preferred Z, which
# is the grey block-out the #247 flyover showed. The env faces (#166) are
# the port's one view that does draw it -- see TIER_ENV.
#
# So a placement is baked at A, at B, or once for both:
#
#   BASE  one model whatever the state says -- A and B are the same hash, or
#         the rule can only ever pick one of them (A of 39 polys or fewer is
#         always A; a missing A, or one with no volume, is always B). Drawn
#         while the instance is drawn at all, and collided with.
#   NEAR  the A model, drawn while the instance's state is A.
#   FAR   the B model, drawn while it is B.
#
# The rule's inputs ride along per instance (SCENERY_LOD_*, pack_instance_block)
# and model3d.c evaluates it at runtime; nothing here decides a distance.
#
# And a fourth, for the car's reflection (TASKS.md #166):
#
#   ENV   the model the six env-map faces draw -- Z, or B for a 0x400
#         instance (INST_FLAG_ENV_*) -- when it is not already baked into the
#         base or far run, which the env draw then reuses instead. Never drawn
#         in the player view and never collided with, so it goes last, past
#         the physics cutoff. DrawAScenery's env pass is the same P rule with
#         its own H (256: the faces are 90 degrees) and one threshold, P >= 32,
#         and no A/B choice.
TIER_BASE = 0
TIER_NEAR = 1
TIER_FAR = 2
TIER_ENV = 3
TIER_NAMES = ("base", "near", "far", "env")

# What an instance's env faces draw, in its record's last u32 (see
# pack_instance_block): nothing, its base or far run (the B or BASE model is
# the env one too), or its own run in the env tier. ENV_ONLY on top marks an
# instance the player view has no model for at all -- a Z-only one, or a 0x02
# traffic light -- so the runtime need not run the player view's pick for it.
ENV_NONE = 0
ENV_REUSE = 1
ENV_OWN = 2
ENV_ONLY = 4

# And how the reflection views (eViews 4 and 5, TASKS.md #296) draw it. Their
# SetupSceneryCullInfo mask (0x800 | 0x40 | 0x10) picks the same set as the env
# faces', at the same Z model and P >= 32 (with the player camera's H), but
# StuffScenery is called with 8 there, which leaves out every 0x400 instance --
# the terrain -- and DrawAScenery's 0x800 case negates the instance matrix's
# [10] before drawing it: each prop is mirrored about its own origin's plane,
# z' = z - 2 r8 z_local. A 0x100 instance has no matrix to negate and goes in
# as it is. The rest of the row is what the runtime needs to mirror baked
# world-space vertices: z' = rx x + ry y + rz z + rw, which for an upright
# instance is (0, 0, -1, 2 tz) and is worked out from its translation; any
# other has its row in the NFSR table after the runs (pack_instance_block),
# numbered from 1 in the env field's top 24 bits.
ENV_NO_REFL = 0x08
ENV_FLAT = 0x10
ENV_ROW_SHIFT = 8
INST_FLAG_NO_MATRIX = 0x0100

# DrawAScenery's constants, Speed.elf 0x80338804 onwards.
SCENERY_LOD_RADIUS_PAD = 6.0       # added to SceneryInfo +0x3c
SCENERY_LOD_DENSITY_MIN = 6.0      # the density floor in P / max(density, 6)
SCENERY_LOD_ALWAYS_A_POLYS = 0x27  # A of this many polys or fewer is always A

# What pack_instance_block() writes in an instance's density field when the
# ratio test is not reached: ALWAYS_A for the small-model shortcut, NEVER_A
# for a missing A or one with no volume (the ratio stays 0). Any positive
# value is max(density, 6) itself.
SCENERY_LOD_ALWAYS_A = 0.0
SCENERY_LOD_NEVER_A = -1.0
# The radius of an instance that has no scenery record behind it -- the parts
# of a section with no placement table, baked at their own matrix. Drawn
# whenever its box is in view, with no LOD.
SCENERY_LOD_NO_RECORD = -1.0


def _f32(v):
    """v rounded to single precision, which is what the GameCube adds in."""
    return struct.unpack("<f", struct.pack("<f", v))[0]


def scenery_lod_density(part):
    """The density field for an instance whose A model is `part` (None when
    the A slot is empty or does not resolve), following DrawAScenery's order:
    no solid, then the poly shortcut, then volume, then max(density, 6)."""
    if part is None:
        return SCENERY_LOD_NEVER_A
    if part["polys"] <= SCENERY_LOD_ALWAYS_A_POLYS:
        return SCENERY_LOD_ALWAYS_A
    if not part["volume"] > 0.0:
        return SCENERY_LOD_NEVER_A
    d = part["density"]
    # fcmpu + bge: a NaN density fails the compare and takes the floor too.
    return d if d >= SCENERY_LOD_DENSITY_MIN else SCENERY_LOD_DENSITY_MIN


def mirror_row(flags, pos, rot):
    """(env bits, row or None) for how the reflection views draw an instance
    (ENV_NO_REFL and the rest above): a row only when the instance is neither
    left out, nor flat, nor upright."""
    if flags & INST_FLAG_ENV_USE_B:
        return ENV_NO_REFL, None
    if flags & INST_FLAG_NO_MATRIX:
        return ENV_FLAT, None
    r = rot
    c0, c1, c2 = (r[0], r[1], r[2]), (r[3], r[4], r[5]), (r[6], r[7], r[8])
    n = (c0[1] * c1[2] - c0[2] * c1[1], c0[2] * c1[0] - c0[0] * c1[2],
         c0[0] * c1[1] - c0[1] * c1[0])
    det = c2[0] * n[0] + c2[1] * n[1] + c2[2] * n[2]
    if abs(det) < 1e-9:
        return ENV_NO_REFL, None
    # z_local = q . (p - t), q the inverse's last row: (c0 x c1) / det.
    q = [v / det for v in n]
    k = -2.0 * r[8]
    row = (k * q[0], k * q[1], 1.0 + k * q[2],
           -k * (q[0] * pos[0] + q[1] * pos[1] + q[2] * pos[2]))
    if (abs(row[0]) < 1e-6 and abs(row[1]) < 1e-6 and abs(row[2] + 1.0) < 1e-6
            and abs(row[3] - 2.0 * pos[2]) < 1e-4):
        return 0, None
    return 0, row


def instance_placements(data, parts, groups, library, missing=None,
                        scenery=None, dropped=None, lod=True, stats=None,
                        env=True):
    """[(source_data, part, pos, rot, tier, soft, sset, lodrec)] -- one per
    placed model, two for an instance whose A and B are different models.

    `soft` is is_soft_model() on the placement's name and rides along to
    _flush_blob(), which keeps the soft batches in their own run.

    `sset` is the instance's scenery set (SceneryGroups.set_for), 0 for always
    drawn. With `scenery` None every instance is placed, in set 0 -- --mode
    all. An instance retail's player view never draws is left out and counted
    in `dropped`.

    `lodrec` is the instance's DrawAScenery inputs, shared by its A and B
    placements: {"box", "pos", "radius", "density"}. build_blobs() turns each
    distinct one into an entry of the blob's instance table.

    `library` is hash -> (section_data, part) for the shared X* model sections;
    a section's own parts win over it, since a lot of a section's props are
    authored right there.

    `lod` False bakes one model per instance as TIER_BASE -- the A, or the B
    where there is no A -- which with no near or far run left draws whatever
    the runtime state says, i.e. no LOD. Kept for A/B measurement.

    `stats`, when given, counts the instances by what they were baked as.

    `env` adds the env faces' model (TIER_ENV) where an instance has one that
    is not already baked into its base or far run, and says in the lodrec's
    "env" which it is (ENV_*). An instance the player view never draws but
    the env faces do is placed for the env faces alone.
    """
    local = {}
    for p in parts:
        local.setdefault(p["hash"], (data, p))

    def resolve(h):
        """(data, part) for one LOD slot, or None. No fallback to another slot:
        LoaderScenery (Speed.elf 0x80207d50) resolves each of the three hashes
        on its own, and an empty or unknown slot stays a null model."""
        return (local.get(h) or library.get(h)) if h else None

    def count(key):
        if stats is not None:
            stats[key] = stats.get(key, 0) + 1

    out = []
    for infos, instances in groups:
        for inst in instances:
            info = infos[inst["info"]]
            if scenery is None:
                sset, eset, flags = 0, 0, inst["flags"]
            else:
                sset = scenery.set_for(inst, info)
                eset = scenery.set_for(inst, info, env=True)
                flags = scenery.flags_for(inst)
            # The env faces' model: Z, or B under 0x400 (INST_FLAG_ENV_*).
            # No fallback to another slot, as below; 2,292 of L4RA's env
            # instances name no Z and are simply not in a reflection.
            eh = 0
            if env and eset is not None and flags & INST_FLAG_ENV_VIEW:
                eh = info["lods"][1] if flags & INST_FLAG_ENV_USE_B else info["lods"][2]
            e = resolve(eh)
            if sset is None:
                if dropped is not None:
                    nm = info["name"]
                    dropped[nm] = dropped.get(nm, 0) + 1
                if e is None:
                    continue
            pos, rot = inst["pos"], inst["rot"]
            ah, bh = info["lods"][0], info["lods"][1]
            a, b = (resolve(ah), resolve(bh)) if sset is not None else (None, None)
            if a is None and b is None:
                # Nothing the player view could ever draw. Counted as missing
                # only if the record names a model at all -- a Z-only instance
                # is retail's own choice, not a hole in this converter.
                if sset is not None:
                    if ah or bh:
                        if missing is not None:
                            missing[info["name"]] = missing.get(info["name"], 0) + 1
                    else:
                        count("z_only")
                if e is None:
                    continue
                # Still in a car's reflection.
                lodrec = {
                    "box": inst["box"],
                    "pos": inst["pos"],
                    "radius": _f32(_f32(info["radius"]) + SCENERY_LOD_RADIUS_PAD),
                    "density": SCENERY_LOD_NEVER_A,
                    "env": ENV_OWN | ENV_ONLY,
                    "mirror": mirror_row(flags, pos, rot),
                }
                out.append((e[0], e[1], pos, rot, TIER_ENV, False, eset, lodrec))
                count("env_only")
                continue
            density = scenery_lod_density(a[1] if a else None)
            lodrec = {
                "box": inst["box"],
                "pos": inst["pos"],
                "radius": _f32(_f32(info["radius"]) + SCENERY_LOD_RADIUS_PAD),
                "density": density,
                "env": ENV_NONE,
                "mirror": mirror_row(flags, pos, rot),
            }
            soft = is_soft_model(info["name"])
            mark = len(out)

            if not lod:
                one = a or b
                out.append((one[0], one[1], pos, rot, TIER_BASE, soft, sset, lodrec))
                count("base")
                _place_env(out, mark, e, eh, pos, rot, eset, lodrec, count)
                continue
            both = None
            if density == SCENERY_LOD_NEVER_A:
                if b is None:
                    count("never_drawn")   # always B, and there is no B
                    _place_env(out, mark, e, eh, pos, rot, eset, lodrec, count)
                    if lodrec["env"]:
                        lodrec["env"] |= ENV_ONLY
                    continue
                both = b
                count("never_a")
            elif density == SCENERY_LOD_ALWAYS_A or ah == bh:
                both = a
                count("always_a" if density == SCENERY_LOD_ALWAYS_A else "same")
            if both is not None:
                # One model whatever the state. Normally that is TIER_BASE --
                # but the base run is inside the physics region and has to stay
                # collidable, so a soft prop is baked into the near and far runs
                # instead, which the A and B states between them cover exactly.
                if soft:
                    out.append((both[0], both[1], pos, rot, TIER_NEAR, True, sset, lodrec))
                    out.append((both[0], both[1], pos, rot, TIER_FAR, True, sset, lodrec))
                else:
                    out.append((both[0], both[1], pos, rot, TIER_BASE, False, sset, lodrec))
                _place_env(out, mark, e, eh, pos, rot, eset, lodrec, count)
                continue
            out.append((a[0], a[1], pos, rot, TIER_NEAR, soft, sset, lodrec))
            if b is not None:
                out.append((b[0], b[1], pos, rot, TIER_FAR, soft, sset, lodrec))
                count("a_b")
            else:
                count("a_only")
            _place_env(out, mark, e, eh, pos, rot, eset, lodrec, count)
    return out


def _place_env(out, mark, e, eh, pos, rot, eset, lodrec, count):
    """The env faces' share of an instance whose player-view placements are
    out[mark:]: its base or far run when that is the env model already, else
    the env model baked into the env tier."""
    if e is None:
        return
    for (_src, part, _p, _r, tier, _soft, sset, _rec) in out[mark:]:
        # A reused run is drawn as it is, scenery set and all, so the set has
        # to be the env one too. It always is but for 0x02, and an 0x02
        # instance has no player-view placement to reuse.
        if tier in (TIER_BASE, TIER_FAR) and part["hash"] == eh and sset == eset:
            lodrec["env"] = ENV_REUSE
            count("env_reuse")
            return
    out.append((e[0], e[1], pos, rot, TIER_ENV, False, eset, lodrec))
    lodrec["env"] = ENV_OWN
    count("env_own")


def build_model_library(stream, sections, wanted, seeded=()):
    """hash -> (section_data, part) for every wanted model hash.

    Props are shared across the world, so most of them live in the `X*` library
    sections rather than in the section that places them; those are searched
    first, then everything else.

    `seeded` is the (section_data, parts) of the sections this run has already
    parsed -- they are searched for free rather than re-read. It replaces an
    earlier `skip` parameter that *excluded* them from the search, which was
    fine while a run converted a handful of sections and silently catastrophic
    for a whole-world run: with all 360 geometry sections skipped there was
    nothing left to search and every one of the 841 shared props went
    unresolved, so the world came out as bare terrain.
    """
    lib = {}
    for data, parts in seeded:
        for p in parts:
            if p["hash"] in wanted and p["hash"] not in lib:
                lib[p["hash"]] = (data, p)
    order = ([s for s in sections if s.name.startswith("X")] +
             [s for s in sections if not s.name.startswith("X")])
    for sec in order:
        if not wanted - set(lib):
            break
        data = read_section(stream, sec)
        parts = parse_section_geometry(data)
        if not parts:
            continue
        keep = False
        for p in parts:
            if p["hash"] in wanted and p["hash"] not in lib:
                lib[p["hash"]] = (data, p)
                keep = True
        if not keep:
            del data
    return lib


def parse_section_geometry(data):
    try:
        return car_pack.parse_geometry(data)
    except SystemExit:
        return []
    except Exception:
        return []


def build_blobs(placements, textures, maxdim, bank=None, verbose=False):
    """[(blob, info)] for a section's placements, split at the u16 index pool.

    A section that places props is far wider than 65535 vertices once every
    instance is expanded, so the output is a list of blobs -- `<SEC>.nfsc`,
    `<SEC>.1.nfsc`, ... -- which the runtime loads together. Retail pages the
    section as one unit; we page it as a handful of them.
    """
    blobs = []
    state = _new_blob_state()
    unresolved = {}
    badslots = [0]

    # Tier order is the layout contract with the runtime: the base run first,
    # then the near-LOD one, then the far-LOD one, each contiguous in both the
    # batch list and the index pool. That is what lets model3d.c pick a detail
    # level with two integer compares against the batch index, and lets the
    # ground and wall scans stop where the far run starts instead of colliding
    # with every prop twice. Sorted, not partitioned, because the sort is
    # stable and placement order within a tier still decides draw order.
    #
    # Softness splits the near run in two, collidable first (see
    # BATCH_NO_COLLIDE): the physics scans read a flat index range and stop at
    # one cutoff, so everything they must not see has to sit past everything
    # they must. The far run is past the cutoff either way, so its softness
    # does not matter to the sort.
    #
    # Inside that, placements go in Morton order of where they stand. The
    # runtime picks A, B or nothing per instance (#247) and draws each merged
    # batch as the stretches of it whose instances are kept, one draw call per
    # unbroken stretch -- and which instances are kept is a matter of where
    # they are, so neighbours in the pool have to be neighbours in the world
    # for the stretches to stay long. The retail file order is not that.
    placements = sorted(placements, key=lambda p: (p[4], p[5] and p[4] != TIER_FAR,
                                                   _spatial_key(p)))

    for (src, part, pos, rot, tier, soft, sset, lodrec) in placements:
        pv, pi, psubs = read_track_mesh(src, part)
        if not pv or not pi:
            continue
        # A placement whose matrix did not decode lands its vertices at
        # 1e38-ish coordinates, and one such mesh poisons the whole section:
        # the blob's bounding box -- which is what the runtime pager measures
        # its load and drop radii against -- ends up spanning the float range,
        # so the section is always "near" and never drops. L4RA's C28 is a real
        # instance of this. Drop the placement rather than the section.
        if not _placement_sane(pos, rot):
            continue
        if state["verts"] and len(state["verts"]) + len(pv) > MAX_VERTS:
            blobs.append(_flush_blob(state, textures, maxdim, bank))
            state = _new_blob_state()

        r = rot
        tx, ty, tz = pos
        wv = []
        lo = [1e30] * 3
        hi = [-1e30] * 3
        for (x, y, z, cr, cg, cb, u, v) in pv:
            # 3x3 is column-major: world = R^T * local + t.
            wx = r[0] * x + r[3] * y + r[6] * z + tx
            wy = r[1] * x + r[4] * y + r[7] * z + ty
            wz = r[2] * x + r[5] * y + r[8] * z + tz
            for i, c in enumerate((wx, wy, wz)):
                if c < lo[i]: lo[i] = c
                if c > hi[i]: hi[i] = c
            # V negated for the PICA200's flipped texture V axis (see car_pack).
            wv.append((wx, wy, wz, cr, cg, cb, u, -v))
        # The bounding box is checked *after* the transform, not just on the
        # placement's own translation and rotation, because the garbage does
        # not only come from the matrix -- L4RA's A37, C28, G23 and H33 each
        # place a mesh whose own vertices are already 1e34-ish, which a sane
        # matrix faithfully carries into the world. One such mesh takes the
        # section's box to the float range, and the pager measures its load and
        # drop radii against exactly that box, so the section would read as
        # permanently underfoot and never drop.
        if not _bbox_sane(lo, hi):
            continue

        verts = state["verts"]
        base = len(verts)
        for i in range(3):
            state["bbmin"][i] = min(state["bbmin"][i], lo[i])
            state["bbmax"][i] = max(state["bbmax"][i], hi[i])
        verts.extend(wv)

        # This placement's entry in the blob's instance table. An instance's
        # A and B share one, so both runs answer to the same state; a part
        # with no scenery record (a section with no placement table) gets one
        # of its own that says so.
        if lodrec is None:
            lodrec = {"box": tuple(lo) + tuple(hi), "pos": tuple(pos),
                      "radius": SCENERY_LOD_NO_RECORD,
                      "density": SCENERY_LOD_ALWAYS_A}
        key = id(lodrec)
        local = state["inst_index"].get(key)
        if local is None:
            local = state["inst_index"][key] = len(state["insts"])
            state["insts"].append([lodrec, list(lo), list(hi)])
        else:
            geo = state["insts"][local]
            for i in range(3):
                geo[1][i] = min(geo[1][i], lo[i])
                geo[2][i] = max(geo[2][i], hi[i])

        ibase = len(state["indices"])
        state["indices"].extend(i + base for i in pi)

        for (first_index, index_count, tex_slot, flags) in psubs:
            # A slot past the end of the part's list is a decode failure, not a
            # texture the data is missing: the subset names a slot the part
            # really ships, so an out-of-range one means the texture leaf was
            # read short. Count those separately -- quietly folding them into
            # hash 0 skipped the `elif th` below, so they were reported as
            # nothing at all, and that is what hid the shifted 0x134012 leaves
            # for as long as they shipped (TASKS.md #225).
            if tex_slot < len(part["textures"]):
                th = part["textures"][tex_slot]
            else:
                th = 0
                badslots[0] += 1
            ti = -1
            if th in textures:
                if th not in state["tex_index"]:
                    state["tex_index"][th] = len(state["tex_order"])
                    state["tex_order"].append(th)
                ti = state["tex_index"][th]
            elif th:
                unresolved[th] = unresolved.get(th, 0) + 1
            # The scenery set rides in the flags, so merge_batches() -- which
            # keys on (texture, flags) -- never fuses two sets into one draw.
            if flags & (BATCH_SCENERY_SET_MAX << BATCH_SCENERY_SET_SHIFT):
                raise ValueError("%s: sub-mesh flags 0x%08x use the scenery "
                                 "set bits" % (part.get("name", "?"), flags))
            state["batches"].append((ibase + first_index, index_count, ti,
                                     flags | (sset << BATCH_SCENERY_SET_SHIFT),
                                     tier, soft, local))

    if state["verts"]:
        blobs.append(_flush_blob(state, textures, maxdim, bank))
    for blob, info in blobs:
        info["unresolved"] = unresolved
        info["badslots"] = badslots[0]
    return blobs


# The retail world is a few kilometres across, so anything past a hundred
# kilometres from the origin is a decode failure rather than distant scenery.
WORLD_SANE_LIMIT = 100000.0


def _placement_sane(pos, rot):
    for v in tuple(pos) + tuple(rot):
        if v != v or v in (float("inf"), float("-inf")):
            return False
    return all(abs(c) <= WORLD_SANE_LIMIT for c in pos)


def _bbox_sane(lo, hi):
    for v in tuple(lo) + tuple(hi):
        if v != v or v in (float("inf"), float("-inf")):
            return False
    return all(abs(c) <= WORLD_SANE_LIMIT for c in tuple(lo) + tuple(hi))


def _new_blob_state():
    return {"verts": [], "indices": [], "batches": [], "tex_order": [],
            "tex_index": {}, "bbmin": [1e30] * 3, "bbmax": [-1e30] * 3,
            "insts": [], "inst_index": {}}


# The grid the placements are Morton-ordered on (see build_blobs), m.
SPATIAL_CELL = 16.0


def _spatial_key(p):
    """Morton code of where placement `p` stands: its scenery box's centre,
    or its translation when it has no record or the box did not decode."""
    rec = p[7]
    x = y = None
    if rec is not None:
        b = rec["box"]
        if _bbox_sane(b[:3], b[3:]):
            x, y = (b[0] + b[3]) * 0.5, (b[1] + b[4]) * 0.5
    if x is None:
        x, y = p[2][0], p[2][1]
    if not (abs(x) <= WORLD_SANE_LIMIT and abs(y) <= WORLD_SANE_LIMIT):
        return 0
    ix = min(max(int(x // SPATIAL_CELL) + 0x8000, 0), 0xFFFF)
    iy = min(max(int(y // SPATIAL_CELL) + 0x8000, 0), 0xFFFF)
    key = 0
    for bit in range(16):
        key |= ((ix >> bit) & 1) << (2 * bit) | ((iy >> bit) & 1) << (2 * bit + 1)
    return key


def merge_batches_runs(batches, indices):
    """car_pack.merge_batches() over (first, count, tex, flags, inst) entries,
    which also says whose triangles each stretch of a merged batch is.

    Returns (batches, indices, runs). The first two are exactly what
    merge_batches() returns for the same entries; `runs` is [(first, count,
    inst)] in pool order, one per unbroken stretch of one instance's indices.
    A merged batch is its entries' indices concatenated in entry order, so an
    instance's share of it is contiguous -- which is what lets the runtime draw
    a batch as the stretches of it whose instances are kept.
    """
    groups = {}
    for (first, count, ti, flags, inst) in batches:
        if count <= 0:
            continue
        idxs, segs = groups.setdefault((ti, flags), ([], []))
        idxs.extend(indices[first:first + count])
        if segs and segs[-1][1] == inst:
            segs[-1][0] += count
        else:
            segs.append([count, inst])
    out_batches, out_indices, runs = [], [], []
    for (ti, flags), (idxs, segs) in groups.items():
        o = len(out_indices)
        out_batches.append((o, len(idxs), ti, flags))
        out_indices.extend(idxs)
        for count, inst in segs:
            runs.append((o, count, inst))
            o += count
    return out_batches, out_indices, runs


# The instance block (TASKS.md #247): what model3d.c needs to run
# DrawAScenery's pick per instance. It sits between the index pool and the
# texture blob, which is where a reader that predates it already stops, so an
# older build reads these blobs unchanged and simply draws without it.
#
#   0x00  char  magic[4]   "NFSI"
#   0x04  u32   numInstances
#   0x08  u32   numRuns
#   0x0c  u32   envFirst + 1: the batch the env tier starts at (TASKS.md
#               #166), 0 for a blob with no env model at all
#         Instance[numInstances] {
#             f32 lo[3], hi[3];  the scenery record's world box (+0x00), or the
#                                placed geometry's when that did not decode
#             f32 pos[3];        translation (+0x20): the LOD distance is to it
#             f32 radius;        SceneryInfo +0x3c + 6; -1 = no scenery record
#             f32 density;       max(A density, 6); 0 always A; -1 never A
#             u32 env;           what the env faces draw of it (ENV_*)
#         }                                                       // 48 bytes
#         Run[numRuns] { u32 firstIndex; u16 triangles; u16 instance; }
#                      in pool order, each inside one batch        // 8 bytes
#
# Then the reflection views' table (TASKS.md #296; mirror_row()), which a
# build that predates it never reads -- it stops at the runs:
#
#         char  magic[4]   "NFSR"
#         u32   numRows
#         f32   row[numRows][4]   z' = rx x + ry y + rz z + rw, for the
#                                 instances whose env field names it
INST_BLOCK_MAGIC = b"NFSI"
REFL_TABLE_MAGIC = b"NFSR"
INST_BLOCK_HDR = 16
INST_RECORD_SIZE = 48
INST_RUN_SIZE = 8
INST_RUN_MAX_TRIS = 0xFFFF


def pack_instance_block(insts, runs, env_first=None):
    """`insts` is the blob state's [lodrec, geo_lo, geo_hi] list, `runs` the
    merged (first, count, inst) list, `env_first` the env tier's first batch
    or None. Returns the bytes."""
    out = bytearray()
    recs = bytearray()
    rows = []
    for rec, glo, ghi in insts:
        # The record's own box, grown to hold what the instance draws. Across
        # L4RA 489 of 48,973 records do not hold their placed geometry -- by up
        # to 35.8 m, mostly in the A21+ LOD sections -- and a frustum test
        # against such a box drops geometry that is still on screen. Growing it
        # can only draw more off screen, never less on it.
        b = rec["box"]
        if _bbox_sane(b[:3], b[3:]):
            lo = [min(b[i], glo[i]) for i in range(3)]
            hi = [max(b[3 + i], ghi[i]) for i in range(3)]
        else:
            lo, hi = glo, ghi
        env = rec.get("env", ENV_NONE)
        bits, row = rec.get("mirror", (0, None))
        env |= bits
        if row is not None:
            rows.append(row)
            env |= len(rows) << ENV_ROW_SHIFT
        recs += struct.pack("<3f3f3fffI", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
                            rec["pos"][0], rec["pos"][1], rec["pos"][2],
                            rec["radius"], rec["density"], env)
    rr = bytearray()
    n = 0
    for (first, count, inst) in runs:
        tris = count // 3
        while tris > 0:
            take = min(tris, INST_RUN_MAX_TRIS)
            rr += struct.pack("<IHH", first, take, inst)
            first += take * 3
            tris -= take
            n += 1
    out += struct.pack("<4sIII", INST_BLOCK_MAGIC, len(insts), n,
                       0 if env_first is None else env_first + 1)
    out += recs
    out += rr
    out += struct.pack("<4sI", REFL_TABLE_MAGIC, len(rows))
    for row in rows:
        out += struct.pack("<4f", *row)
    return bytes(out)


def _flush_blob(state, textures, maxdim, bank=None):
    tex_entries, tex_blob = [], bytearray()
    # Texture animations (#326): the other frames as extra table entries.
    order = list(state["tex_order"])
    for th in state["tex_order"]:
        for fh in TEX_ANIM_FRAMES.get(th, ()):
            if fh in textures and fh not in order:
                order.append(fh)
    for th in order:
        name, w, h, rgba, mat, scroll = textures[th]
        if bank is not None:
            # Table only: the pixels go in the shared bank and the runtime
            # resolves this slot by hash. Offset 0 with an empty payload is
            # what tells model3d.c not to look for pixels in the section.
            w, h, fmt, mat = bank.add(th, name, w, h, rgba, maxdim, mat, scroll)
            tex_entries.append((th, w, h, fmt, 0, mat))
            continue
        rgba, w, h = car_pack.box_downscale(rgba, w, h, maxdim)
        fmt, pixels = car_pack.encode_texture(rgba, w, h)
        tex_entries.append((th, w, h, fmt, len(tex_blob), mat))
        tex_blob.extend(pixels)

    # One draw call per texture instead of one per prop instance -- see
    # car_pack.merge_batches. Without it a section is over a thousand draw
    # calls and four resident sections overrun the GPU command buffer.
    #
    # Merged per tier rather than over the whole blob, because a merge that
    # spanned tiers would put near-LOD and far-LOD triangles in one draw call
    # and there would be nothing left to select between. The cost is that a
    # texture used by both tiers is two batches instead of one; the tiers are
    # concatenated in order so each one stays a contiguous run.
    #
    # Four runs, not three: the near one is split into its collidable batches
    # and its soft ones (#183), so that the physics cutoff can sit between
    # them. merge_batches() already keys on (texture, flags) and so would never
    # have fused a soft batch with a hard one, but it returns them in
    # first-appearance order, which is not good enough -- they have to be a
    # contiguous run, hence a separate merge per run.
    #
    # The env tier (#166) goes last, past the physics cutoff at far_first; the
    # player view never draws it, the env faces draw it and the base and far
    # runs per instance (ENV_*). Its start rides in the instance block, since
    # the header's lodSplit has no room for a third boundary.
    batches, indices, runs = [], [], []
    near_first = far_first = 0
    env_first = None
    for (tier, soft) in ((TIER_BASE, False), (TIER_NEAR, False),
                         (TIER_NEAR, True), (TIER_FAR, None), (TIER_ENV, None)):
        if tier == TIER_NEAR and not soft:
            near_first = len(batches)
        elif tier == TIER_FAR:
            far_first = len(batches)
        elif tier == TIER_ENV:
            env_first = len(batches)
        group = [b[:4] + (b[6],) for b in state["batches"]
                 if b[4] == tier and (soft is None or bool(b[5]) == soft)]
        if not group:
            continue
        merged, midx, mruns = merge_batches_runs(group, state["indices"])
        base = len(indices)
        mark = BATCH_NO_COLLIDE if (soft and tier == TIER_NEAR) else 0
        batches.extend((fi + base, ic, ti, fl | mark) for (fi, ic, ti, fl) in merged)
        indices.extend(midx)
        runs.extend((fi + base, ic, inst) for (fi, ic, inst) in mruns)
    lod_split = (near_first, far_first)
    if env_first == len(batches):
        env_first = None
    # An env tier with no table to say which instances own it would be drawn
    # by an old reader's far pass; there is always a table, but say so.
    assert env_first is None or state["insts"]

    inst_block = pack_instance_block(state["insts"], runs, env_first)
    blob = car_pack.pack_nfsc(tex_entries, tex_blob, batches,
                              state["verts"], indices,
                              state["bbmin"], state["bbmax"],
                              car_pack.SHADING_BAKED, lod_split=lod_split,
                              extra=inst_block)
    return blob, {
        "verts": len(state["verts"]), "indices": len(indices),
        "batches": len(batches), "textures": len(tex_entries),
        "texbytes": len(tex_blob), "bbmin": state["bbmin"],
        "bbmax": state["bbmax"], "unresolved": {}, "badslots": 0,
        "lodsplit": lod_split,
        "instances": len(state["insts"]), "runs": len(runs),
        "instbytes": len(inst_block),
        # Indices per tier, for the conversion summary -- the far run is the
        # geometry a distant section draws *instead of* the near one, so the
        # pair is the saving this whole exercise is for.
        # Base / near / far. The near figure covers both of the near run's
        # halves: a soft batch is still drawn, it is only not collided with,
        # so it belongs in the near draw cost like any other near batch.
        "tiertris": tuple(
            sum(b[1] for b in batches[a:z]) // 3
            for a, z in ((0, near_first), (near_first, far_first),
                         (far_first, len(batches) if env_first is None else env_first),
                         (len(batches) if env_first is None else env_first,
                          len(batches)))),
    }


def core_bbox(path, hdr):
    """The box holding all but the outermost CORE_TRIM of a blob's vertices.

    The pager's whole decision is "how far is the car from this section", and
    measuring that against the section's true bounding box is wrong for the
    sections that carry the skyline plate and the distant-LOD shells. L4RA's
    A99 spans 9 km of X and A21 6.6 km; a car anywhere in Bayview is *inside*
    those boxes, so they read as permanently underfoot, never drop, and sit on
    the resident-section budget that the street tiles around the car need.

    Those shells are a handful of enormous triangles, though, and the section's
    real content is the few hundred metres of city the rest of its vertices
    describe. So the index carries both: the true box, which is what drawing
    culls against (the skyline should be drawn from far away -- that is its
    job), and this trimmed one, which is what paging measures.
    """
    n_vert = struct.unpack_from("<I", hdr, 0x10)[0]
    n_tex = struct.unpack_from("<I", hdr, 0x08)[0]
    n_batch = struct.unpack_from("<I", hdr, 0x0c)[0]
    vert_off = INDEX_HDR_SIZE + n_tex * 16 + n_batch * 16
    with open(path, "rb") as fh:
        fh.seek(vert_off)
        pool = fh.read(n_vert * 32)
    if len(pool) < n_vert * 32 or not n_vert:
        return None
    a = array.array("f")
    a.frombytes(pool)
    lo, hi = [], []
    cut = int(n_vert * CORE_TRIM)
    for axis in range(3):
        vals = sorted(a[axis::8])
        lo.append(vals[cut])
        hi.append(vals[n_vert - 1 - cut])
    return lo, hi


# .nfsc files that share the output directory with the sections but are NOT
# places: the skydome and the car shadow, written there by tools/sky_pack.py and
# tools/shadow_pack.py (TASKS.md #148). They are unit-sized models the renderer
# positions itself, so indexing them would hand the pager two sections sitting
# on the world origin -- and drag the world bbox back to it.
NOT_SECTIONS = ("SKYDOME.NFSC", "SHADOW.NFSC")


def write_index(out_dir, track):
    """(Re)build WORLD.NFSX from every .nfsc sitting in `out_dir`.

    The bounding boxes are read back out of the blobs' own headers rather than
    remembered from this run, so a run that converts a few more sections into a
    directory that already holds some leaves a whole, consistent index behind
    instead of one that only describes what it just wrote.
    """
    names = sorted(f for f in os.listdir(out_dir)
                   if f.endswith(".nfsc") and f.upper() not in NOT_SECTIONS)
    recs = []
    far = []
    wmin = [1e30] * 3
    wmax = [-1e30] * 3
    for fn in names:
        path = os.path.join(out_dir, fn)
        size = os.path.getsize(path)
        with open(path, "rb") as fh:
            hdr = fh.read(INDEX_HDR_SIZE)
        if len(hdr) < INDEX_HDR_SIZE or hdr[:4] != b"NFSC":
            print("  skipping %s: not an .nfsc" % fn)
            continue
        (n_tex, n_batch, n_vert, n_idx) = struct.unpack_from("<4I", hdr, 0x08)
        bbmin = list(struct.unpack_from("<3f", hdr, 0x18))
        bbmax = list(struct.unpack_from("<3f", hdr, 0x24))
        tex_bytes, = struct.unpack_from("<I", hdr, 0x34)
        stem = fn[:-len(".nfsc")]
        if len(stem) >= INDEX_NAME_LEN:
            print("  skipping %s: name too long for the index" % fn)
            continue
        for i in range(3):
            wmin[i] = min(wmin[i], bbmin[i])
            wmax[i] = max(wmax[i], bbmax[i])
        core = core_bbox(path, hdr)
        clo, chi = core if core else (bbmin, bbmax)
        extent = max(chi[0] - clo[0], chi[1] - clo[1])
        flags = SECTION_FAR if extent > FAR_EXTENT else 0
        if stem.split(".")[0] == world_anim.SECTION_NAME:
            flags = SECTION_FAR | SECTION_ANIM
        elif flags:
            far.append(stem)
        rec = struct.pack("<16s3f3fIIIIII3f3fI4x", stem.encode("ascii"),
                          bbmin[0], bbmin[1], bbmin[2],
                          bbmax[0], bbmax[1], bbmax[2],
                          size, n_vert, n_idx, n_batch, tex_bytes, 0,
                          clo[0], clo[1], clo[2], chi[0], chi[1], chi[2],
                          flags)
        assert len(rec) == INDEX_RECORD_SIZE, len(rec)
        recs.append(rec)
        del n_tex

    if not recs:
        print("no sections to index")
        return
    hdr = struct.pack("<4sII I 3f3f 8s4I", INDEX_MAGIC, INDEX_VERSION, len(recs),
                      INDEX_RECORD_SIZE,
                      wmin[0], wmin[1], wmin[2], wmax[0], wmax[1], wmax[2],
                      track.encode("ascii"), 0, 0, 0, 0)
    assert len(hdr) == INDEX_HDR_SIZE, len(hdr)
    with open(os.path.join(out_dir, INDEX_NAME), "wb") as fh:
        fh.write(hdr)
        for r in recs:
            fh.write(r)
    print("%s: %d section(s), world x[%.0f,%.0f] y[%.0f,%.0f] z[%.0f,%.0f]"
          % (INDEX_NAME, len(recs), wmin[0], wmax[0], wmin[1], wmax[1],
             wmin[2], wmax[2]))
    if far:
        print("  far scenery, pinned resident (%d): %s"
              % (len(far), ", ".join(far)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("track_dir", help='PC Files/TRACKS')
    ap.add_argument("track", help='track code, e.g. L4RA')
    ap.add_argument("--out", metavar="DIR", default=None,
                    help="write <SECTION>.nfsc into DIR")
    ap.add_argument("--sections", default=None,
                    help="comma-separated section names; default is every "
                         "geometry section")
    ap.add_argument("--near", default=None, metavar="X,Y,RADIUS",
                    help="only sections whose world bbox is within RADIUS "
                         "metres of (X,Y)")
    ap.add_argument("--list", action="store_true",
                    help="print the section table and exit")
    ap.add_argument("--index-only", action="store_true",
                    help="rebuild --out's WORLD.NFSX from the .nfsc files "
                         "already there and exit, converting nothing")
    ap.add_argument("--maxdim", type=int, default=128,
                    help="downscale textures to at most this many texels "
                         "on a side (default 128)")
    ap.add_argument("--budget-mb", type=float, default=None, metavar="MB",
                    help="fail if the converted track comes out larger than "
                         "this. The 3DS reads all of this off the SD card at "
                         "runtime, so the ceiling is the card, not the CIA")
    ap.add_argument("--embed-textures", action="store_true",
                    help="write each section's pixels into its own .nfsc "
                         "instead of into the shared WORLD.NFSB bank. Costs "
                         "~13x the disk for a whole world and re-uploads "
                         "shared art on every section page-in; only useful for "
                         "inspecting a section in isolation")
    ap.add_argument("--global-dir", default=None, metavar="DIR",
                    help="where to look for GLOBAL/*.BUN when a texture hash "
                         "is in no stream section (default: the GLOBAL "
                         "directory beside track_dir)")
    ap.add_argument("--mode", choices=PACK_MODES, default="retail",
                    help="which instances to place: 'retail' (the default) "
                         "places what retail's player view can draw, the race "
                         "barriers, start/finish lines and free-roam beams "
                         "tagged with the scenery set that turns them on, and "
                         "writes WORLD.NFSG; 'all' places every instance the "
                         "sections ship, untagged, for looking at the raw data")
    ap.add_argument("--no-props", action="store_true",
                    help="terrain only: ignore the scenery instance tables")
    ap.add_argument("--no-lod", dest="lod", action="store_false",
                    help="bake every instance at one model, its A (or its B "
                         "where there is no A), so the runtime has no A/B "
                         "choice to make. For A/B measurement; the shipped "
                         "world wants retail's per-instance pick")
    ap.add_argument("--no-env", dest="env", action="store_false",
                    help="leave out the env faces' models (TIER_ENV), so the "
                         "car's reflection shows only what the base and far "
                         "runs already hold")
    ap.add_argument("--etc1", action="store_true",
                    help="pack the world texture bank as ETC1 / ETC1A4 (4 / 8 "
                         "bits a texel, via devkitPro's tex3ds) instead of "
                         "RGB565 / RGBA4444 (TASKS.md #427)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.index_only:
        if not args.out:
            ap.error("--index-only needs --out")
        write_index(args.out, args.track)
        return

    bun = os.path.join(args.track_dir, "%s.BUN" % args.track)
    stream_path = os.path.join(args.track_dir, "STREAM%s.BUN" % args.track)
    sections = read_section_table(bun)

    if args.list:
        print("%-8s %-6s %-10s %-10s" % ("name", "num", "offset", "size"))
        for s in sections:
            print("%-8s %-6d 0x%08x 0x%08x" % (s.name, s.number, s.offset, s.size))
        print("%d sections" % len(sections))
        return

    stream = open(stream_path, "rb")

    pick = None
    if args.sections:
        pick = set(n.strip() for n in args.sections.split(",") if n.strip())

    # Pass 1: parse geometry and the scenery placement tables.
    geo = []
    for sec in sections:
        if pick is not None and sec.name not in pick:
            continue
        if pick is None and sec.number >= FIRST_RESOURCE_SECTION:
            continue   # model library / texture pack, not a place
        data = read_section(stream, sec)
        parts = parse_section_geometry(data)
        groups = [] if args.no_props else read_scenery_groups(data)
        # A section with no geometry of its OWN is still a place. It can carry
        # nothing but a scenery placement table, every entry of which points at
        # a model authored in one of the shared X* library sections -- pass 2
        # resolves those against `library` and does not care that the section
        # contributed no parts.
        #
        # Skipping on `not parts` alone silently deleted 23 real city sections
        # from L4RA: A98, C42, C44, C53, C57, C58, C66, C67, C69, C72, C73,
        # C76, C80, C85, C86, C88, C89, D42, D43, D46, E49, G99 and M41. C88 is
        # the one that made it obvious -- it holds XB_BodyShopA_1a_RB_00 and the
        # ~35 spotlights and bulbs that light it, i.e. the entire body shop the
        # career spawn point parks you in front of, and the user reported the
        # shop, its glow and its interaction marker all missing from a spot
        # where the retail game draws them.
        if not parts and not groups:
            continue
        geo.append([sec, data, parts, groups, None])

    if args.near:
        cx, cy, radius = (float(v) for v in args.near.split(","))
        kept = []
        for entry in geo:
            sec, data, parts, groups = entry[0], entry[1], entry[2], entry[3]
            hit = False
            # Placement-only sections have no vertices to test, so test where
            # their instances sit instead. Terrain instances are the origin with
            # an identity transform and say nothing about location, so skip
            # those the same way the spawn survey does.
            for infos, instances in groups:
                for inst in instances:
                    ix, iy, iz = inst["pos"]
                    if abs(ix) < 1e-6 and abs(iy) < 1e-6 and abs(iz) < 1e-6:
                        continue
                    if (ix - cx) ** 2 + (iy - cy) ** 2 <= radius * radius:
                        hit = True
                        break
                if hit:
                    break
            for p in parts:
                if hit:
                    break
                m = p["mesh"]
                if not m["numVerts"]:
                    continue
                vs = skip_alignment_padding(data, *m["vb"])
                avail = m["vb"][1] - (vs - m["vb"][0])
                stride = CAR_STRIDE if avail // m["numVerts"] >= CAR_STRIDE else SCENE_STRIDE
                step = max(1, m["numVerts"] // 16)
                for i in range(0, m["numVerts"], step):
                    x, y, _z = struct.unpack_from("<3f", data, vs + i * stride)
                    if (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius:
                        hit = True
                        break
                if hit:
                    break
            if hit:
                kept.append(entry)
        geo = kept

    # Pass 2: resolve every placed model. Most props are authored once in a
    # shared X* section and instanced from all over the world, so a section's
    # own part list is only half the answer.
    # Every level of the chain, not just the first that exists: with LOD
    # selection on, a prop needs its far model resolved as well as its near
    # one, and the two are usually authored in different library sections.
    # (`break` after the first hash was right only while the far ones were
    # being thrown away.)
    need = set()
    for sec, data, parts, groups, _ in geo:
        for infos, instances in groups:
            used = set(i["info"] for i in instances)
            have = set(p["hash"] for p in parts)
            for idx in used:
                for h in infos[idx]["lods"]:
                    if h and h not in have:
                        need.add(h)
                        if not args.lod:
                            break
    scenery = SceneryGroups(bun) if args.mode == "retail" else None
    if scenery is not None:
        print("scenery groups: %d groups over %d overrides, %d sets"
              % (len(scenery.names), len(scenery.overrides), len(scenery.sets)))
    # The moving world animations' models (#239): worldanim.c draws them out of
    # WANIM.nfsc. Retail's WorldModel takes the entity's +0c alone -- no LOD.
    anims = scenery.anims if scenery is not None and not args.no_props else None
    if anims is not None:
        need.update(anims.models)
    library = {}
    if need:
        library = build_model_library(stream, sections, need,
                                      seeded=[(e[1], e[2]) for e in geo])
        print("model library: %d/%d shared props resolved"
              % (len(set(library) & need), len(need)))

    # One placement per model at its own dummy translation, identity rotation:
    # the runtime finds each by that translation (model3d_find_instance) and
    # draws it through D = entity world * T(-dummy). No scenery record, so the
    # instance pick never culls it; worldanim.c does retail's culling.
    anim_placements = []
    if anims is not None:
        for k, h in enumerate(anims.models):
            if h not in library:
                raise ValueError("world anim model %08x is in no section" % h)
            src, part = library[h]
            pv, _pi, _ps = read_track_mesh(src, part)
            if pv:
                anims.set_bounds(k, [min(v[i] for v in pv) for i in range(3)],
                                 [max(v[i] for v in pv) for i in range(3)])
            anim_placements.append((src, part, anims.dummy_pos(k),
                                    [1, 0, 0, 0, 1, 0, 0, 0, 1], TIER_BASE, False,
                                    0, None))
        print("world animations: %d moving instance(s) of %d tree(s), %d model(s)"
              "; frame 0 equal to the record's local for %d of %d entities"
              % ((len(anims.moving), len(anims.moving_trees), len(anims.models)) +
                 anims.check_frame0()))

    missing_models = {}
    wanted = set()
    dropped_instances = {}
    lod_stats = {}
    set_counts = {}
    for entry in geo:
        sec, data, parts, groups = entry[0], entry[1], entry[2], entry[3]
        if groups:
            placements = instance_placements(data, parts, groups, library,
                                             missing_models, scenery,
                                             dropped_instances, args.lod,
                                             lod_stats, args.env)
        else:
            placements = [(data, p, p["matrix"][12:15],
                           [1, 0, 0, 0, 1, 0, 0, 0, 1], TIER_BASE, False, 0, None)
                          for p in parts]
        entry[4] = placements
        for (_src, part, _pos, _rot, tier, _soft, sset, _rec) in placements:
            wanted.update(h for h in part["textures"] if h)
            if sset and tier != TIER_FAR:
                set_counts[sset] = set_counts.get(sset, 0) + 1
    for (_src, part, _pos, _rot, _tier, _soft, _sset, _rec) in anim_placements:
        wanted.update(h for h in part["textures"] if h)

    print("%d geometry sections, %d placements, %d distinct texture hashes"
          % (len(geo), sum(len(e[4]) for e in geo), len(wanted)))
    if args.lod:
        # What DrawAScenery's rule can make of each instance, given its A.
        print("LOD: %d instance(s) with distinct A and B, %d A only, %d with "
              "A == B, %d always A (<= %d polys), %d always B (no A or no "
              "volume); %d never drawn (always B, no B), %d Z only"
              % (tuple(lod_stats.get(k, 0) for k in (
                  "a_b", "a_only", "same", "always_a")) +
                 (SCENERY_LOD_ALWAYS_A_POLYS,) +
                 tuple(lod_stats.get(k, 0) for k in (
                     "never_a", "never_drawn", "z_only"))))
    else:
        print("LOD: off (--no-lod), every instance baked at one model")
    if args.env:
        print("env faces: %d instance(s) reuse their base or far model, %d "
              "bake their own (%d of them the player view never draws)"
              % tuple(lod_stats.get(k, 0) for k in ("env_reuse", "env_own", "env_only")))
    if set_counts:
        print("scenery sets: %d instance(s) in %d of the sets, drawn only "
              "while their groups are on" % (sum(set_counts.values()), len(set_counts)))
    if dropped_instances:
        print("mode %s: left out %d instance(s) retail's player view never "
              "draws, %d distinct model(s):"
              % (args.mode, sum(dropped_instances.values()), len(dropped_instances)))
        for n, c in sorted(dropped_instances.items(), key=lambda kv: -kv[1])[:10]:
            print("    %-44s x%d" % (n, c))
    if missing_models:
        print("unplaced models (%d distinct):" % len(missing_models))
        for n, c in sorted(missing_models.items(), key=lambda kv: -kv[1])[:10]:
            print("  %-32s x%d" % (n, c))
    # Texture animations (#326): every frame of one a placement names.
    tex_anims = {h: a for h, a in read_texture_anims(stream, sections).items()
                 if any(f in wanted for f in a[3])}
    TEX_ANIM_FRAMES.clear()
    for h, (name, fps, tbase, frames) in sorted(tex_anims.items()):
        wanted.update(frames)
        for f in frames:
            TEX_ANIM_FRAMES[f] = tuple(frames)
        print("texture animation %s: %d frame(s) at %d fps" % (name, len(frames), fps))
    global_dir = args.global_dir
    if global_dir is None:
        global_dir = os.path.join(os.path.dirname(
            os.path.abspath(args.track_dir)), "GLOBAL")
    textures = build_texture_index(stream, sections, wanted, args.maxdim,
                                   args.verbose, global_dir=global_dir)
    print("resolved %d/%d textures" % (len(textures), len(wanted)))
    if len(textures) < len(wanted):
        print("no TPK anywhere carries: %s"
              % ", ".join("0x%08x" % h for h in sorted(wanted - set(textures))))

    if not args.out:
        return
    os.makedirs(args.out, exist_ok=True)
    if scenery is not None:
        print("wrote WORLD.NFSG, %d B" % scenery.write(args.out, args.track))

    bank = None if args.embed_textures else TextureBank(etc1=args.etc1)

    total = 0
    files = 0
    tier_tris = [0, 0, 0]   # triangles a section draws near / far / env-only
    inst_all = [0, 0, 0]   # instances, runs, block bytes
    unresolved_all = {}
    badslots_all = 0
    for sec, data, parts, groups, placements in geo:
        blobs = build_blobs(placements, textures, args.maxdim, bank,
                            args.verbose)
        for n, (blob, info) in enumerate(blobs):
            name = sec.name if n == 0 else "%s.%d" % (sec.name, n)
            with open(os.path.join(args.out, "%s.nfsc" % name), "wb") as fh:
                fh.write(blob)
            total += len(blob)
            files += 1
            near_tris = info["tiertris"][TIER_BASE] + info["tiertris"][TIER_NEAR]
            far_tris = info["tiertris"][TIER_BASE] + info["tiertris"][TIER_FAR]
            tier_tris[0] += near_tris
            tier_tris[1] += far_tris
            tier_tris[2] += info["tiertris"][TIER_ENV]
            inst_all[0] += info["instances"]
            inst_all[1] += info["runs"]
            inst_all[2] += info["instbytes"]
            print("  %-8s %5d v %6d i %4d b %3d tex %8d B  lod %6d/%-6d/%-5d t  "
                  "bb x[%.0f,%.0f] y[%.0f,%.0f] z[%.0f,%.0f]"
                  % (name, info["verts"], info["indices"], info["batches"],
                     info["textures"], len(blob), near_tris, far_tris,
                     info["tiertris"][TIER_ENV],
                     info["bbmin"][0], info["bbmax"][0],
                     info["bbmin"][1], info["bbmax"][1],
                     info["bbmin"][2], info["bbmax"][2]))
        if blobs:
            for h, n in blobs[0][1]["unresolved"].items():
                unresolved_all[h] = unresolved_all.get(h, 0) + n
            badslots_all += blobs[0][1].get("badslots", 0)

    if anim_placements:
        blobs = build_blobs(anim_placements, textures, args.maxdim, bank,
                            args.verbose)
        for n, (blob, info) in enumerate(blobs):
            name = (world_anim.SECTION_NAME if n == 0
                    else "%s.%d" % (world_anim.SECTION_NAME, n))
            with open(os.path.join(args.out, "%s.nfsc" % name), "wb") as fh:
                fh.write(blob)
            total += len(blob)
            files += 1
            print("  %-8s %5d v %6d i %4d b %3d tex %8d B  (world animations)"
                  % (name, info["verts"], info["indices"], info["batches"],
                     info["textures"], len(blob)))
        print("wrote %s, %d B" % (world_anim.NFSW_NAME, anims.write(args.out)))

    if tier_tris[0]:
        print("LOD: %d triangles with every instance at A, %d with every one "
              "at B -- %.1f%% fewer"
              % (tier_tris[0], tier_tris[1],
                 100.0 * (tier_tris[0] - tier_tris[1]) / tier_tris[0]))
    if tier_tris[2]:
        print("env tier: %d triangles only the env faces draw" % tier_tris[2])
    print("instance tables: %d instance(s), %d run(s), %.1f KB"
          % (inst_all[0], inst_all[1], inst_all[2] / 1024.0))

    if bank is not None:
        bank.anims = [(h, a[1], a[2], a[3]) for h, a in sorted(tex_anims.items())
                      if h in bank.entries]
    bank_bytes = bank.write(args.out, args.track) if bank is not None else 0
    print("wrote %d files for %d sections, %.1f MB geometry + %.1f MB textures "
          "= %.1f MB"
          % (files, len(geo), total / 1048576.0, bank_bytes / 1048576.0,
             (total + bank_bytes) / 1048576.0))
    write_index(args.out, args.track)

    size_mb = (total + bank_bytes) / 1048576.0
    try:
        free_mb = shutil.disk_usage(args.out).free / 1048576.0
        print("SD budget: %.1f MB written, %.0f MB free on the target volume"
              % (size_mb, free_mb))
    except OSError:
        print("SD budget: %.1f MB written" % size_mb)
    if args.budget_mb is not None and size_mb > args.budget_mb:
        print("SD budget EXCEEDED: %.1f MB > %.1f MB" % (size_mb, args.budget_mb))
        sys.exit(1)

    if unresolved_all:
        print("unresolved texture hashes (%d):" % len(unresolved_all))
        for h, n in sorted(unresolved_all.items(), key=lambda kv: -kv[1])[:20]:
            print("  0x%08x x%d" % (h, n))
    if badslots_all:
        print("texture slot out of range on %d subsets -- a short texture leaf"
              % badslots_all)


if __name__ == "__main__":
    main()
