#!/usr/bin/env python3
"""
CARS/<car>/GEOMETRY.BIN + TEXTURES.BIN  ->  .nfsc runtime car model for the 3DS.

Builds the *stock* (KIT00, LOD A) visual part set of one car into a single
flat, GPU-ready blob the 3DS engine can read and draw with no parsing work:
one shared vertex pool, one shared u16 index pool, a batch list (one batch per
0x134b02 material subset), and PICA200-swizzled 16bpp textures.

Everything here comes out of the retail PC data -- see
docs/formats/geometry-bin.md for the chunk layout this walks. Nothing is
authored or approximated except the two things called out below:

  - Wheel and brake corner placements do not come from GEOMETRY.BIN, which
    authors one wheel at the origin and says nothing about where the four
    corners are. They come from the CarTypeInfo wheel table in
    GLOBAL/GLOBALB.BUN -- see load_wheel_table. If that table cannot be read
    the wheel and brake parts are skipped rather than guessed at.
  - The exhaust tip is authored at the origin as well. It hangs on the
    exhaust markers of the other stock parts (the rear bumper's), as
    Render__13CarRenderInfo hangs it -- see exhaust_markers. A car with no
    such marker gets no tip.
  - Untextured subsets (texture slot resolves to a hash that is not in the
    car's own TEXTURES.BIN -- the shared body/paint maps live in a global
    file that is not decoded yet) get texIndex -1, which the runtime shades as
    flat car paint. That is close to correct anyway: NFSU2 car bodies carry no
    diffuse paint texture, only detail maps.

Output layout (all little-endian, see 3DS Files/source/model3d.h for the C
mirror of these structs):

    0x00  char  magic[4]      "NFSC"
    0x04  u32   version       = 3
    0x08  u32   numTextures
    0x0c  u32   numBatches
    0x10  u32   numVerts
    0x14  u32   numIndices
    0x18  f32   bboxMin[3]
    0x24  f32   bboxMax[3]
    0x30  u32   texBlobOffset  (absolute file offset)
    0x34  u32   texBlobSize
    0x38  u32   shading        0 = lit (attr1 is a normal), 1 = baked (attr1 is RGB)
    0x3c  u32   lodSplit       0 = none; else (nearFirstBatch+1) | (farFirstBatch+1)<<16
    0x40  TexEntry[numTextures]  { u32 hash; u16 w, h; u16 fmt; u16 mat; u32 dataOffset; }
          Batch[numBatches]      { u32 firstIndex; u32 indexCount; s32 texIndex; u32 flags; }
          Vertex[numVerts]       { f32 pos[3]; f32 attr1[3]; f32 uv[2]; }  // 32 bytes
          u16 indices[numIndices]                                          // 4-byte padded
          <optional block, up to texBlobOffset>  a world section's instance
                                                 table, see track_pack.py's
                                                 pack_instance_block
          <texture pixel blob>

fmt: bits 0-7 are the format -- 0 = GPU_RGB565, 1 = GPU_RGBA4 (see the picking
rule in encode_texture), 2 = GPU_RGBA8, 3 = the car skin the runtime
composites (FMT_SKIN: a size and a mip count, no pixels). Bits 8-11 are
maxLevel: how many mip levels follow level 0 in the blob, 0 for a single image.
The two share one u16 because the header is exactly 0x40 bytes and full, and
because the field only ever held values 0-3 -- its top 14 bits have never been
written. Use
tex_fmt_field()/tex_fmt_of()/tex_fmt_level() rather than touching it directly;
the C mirror in model3d.c masks the same way.

mat: the texture's retail render state -- blend type, alpha usage, depth
write -- packed by texture_material(), the same packing track_pack.py gives a
world texture. A TEXTURES.BIN block ends in the TextureInfo record a TPK
carries, so a car texture's comes out of its own block (trailer_material).
Before it was packed, every car texture was an alpha cutout, which drew the
additive headlight lenses as solid black (TASKS.md #205).

attr1 is a per-vertex normal for cars (`shading` = 0) and baked vertex lighting
for the frontend platform scenes tools/scene_pack.py builds (`shading` = 1).
Both write this same container, so the runtime picks a vertex shader off that
field instead of carrying a second file format.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jdlz  # noqa: E402
import huff  # noqa: E402
from scan_texture_formats import iter_table_entries  # noqa: E402
from dxt_transcode import _decode_block_trailer  # noqa: E402
from geometry_transcode import leaf_start  # noqa: E402
import extract_global_textures as egt  # noqa: E402
from fng_parse import b_string_hash  # noqa: E402

# 3: the TexEntry fmt field carries maxLevel in bits 8-11 and a texture's blob
# run is its whole mip chain. A v2 reader would hand a chain to the GPU as one
# image, so model3d.c requires the exact version rather than a minimum.
NFSC_VERSION = 3
SHADING_LIT = 0              # attr1 is a normal, lit by car.v.pica
SHADING_BAKED = 1            # attr1 is baked vertex RGB, passed by scene.v.pica

VERTEX_STRIDE = 36           # car FVF: pos3 nrm3 color1 uv2
PART_RECORD_NAME_OFF = 0xa4  # 28-byte NUL-terminated name, truncated at 27 chars
PART_NAME_MAX = 27           # so any longer name is stored cut to this length
PART_RECORD_MATRIX_OFF = 0x40
# u32 bStringHash of the part's *full* (untruncated, upper-cased) name. The
# name field above is clipped at 27 characters, so the hash is the only
# reliable key when something outside the part list -- a track's scenery
# instance table, say -- wants to name a model.
PART_RECORD_HASH_OFF = 0x10
PART_RECORD_POLYS_OFF = 0x14     # s16
PART_RECORD_VOLUME_OFF = 0x94    # f32
PART_RECORD_DENSITY_OFF = 0x98   # f32, polys / volume
# The record proper, before the leaf pads it out to its runtime alignment.
# Passing these as the leaf "stride" lets leaf_start() measure the filler from
# the leaf size instead of trusting the 0x11 run-length scan, which cannot tell
# filler from a record that genuinely begins with an 0x11 byte.
PART_RECORD_SIZE = 0xc0
MESH_HEADER_SIZE = 0x44
SUBSET_STRIDE = 60

FMT_RGB565 = 0
FMT_RGBA4 = 1
# 32bpp. Only tools/afx_pack.py writes it now, for the effects art retail ships
# as raw 8-bit ARGB. A car's body used to carry a pack-time paint split in it;
# that gave way to the runtime composite, FMT_SKIN (TASKS.md #296).
FMT_RGBA8 = 2
# The car skin retail composites on the CPU (CompositeSkin32, SPEED2.EXE
# 0x611F10) and the runtime does too (3DS Files/source/carskin.c): a 32bpp
# RGBA8 texture whose entry carries a size and a mip count but no pixels.
# Its alpha is the paint's GLOSS, not coverage. tools/kit_pack.py writes one
# per car, as <CAR>_SKIN1.
FMT_SKIN = 3

# Parts that make up a showroom-stock car. Suffixes are matched against the
# 0x134011 name, which the exporter truncates at 27 chars -- hence the
# left/right entries that lose their trailing "_A".
STOCK_SUFFIXES = [
    "_BASE_A",
    "_KIT00_BODY_A",
    "_KIT00_BRAKELIGHT_A",
    "_KIT00_DOOR_LEFT_A", "_KIT00_DOOR_RIGHT_A",
    "_KIT00_DOOR_PANEL_LEFT_A", "_KIT00_DOOR_PANEL_RIGHT_A",
    "_KIT00_DOOR_SILL_LEFT_A", "_KIT00_DOOR_SILL_RIGHT_A",
    "_KIT00_EXHAUST_A",
    "_KIT00_FRONT_BRAKE_A", "_KIT00_REAR_BRAKE_A",
    "_KIT00_FRONT_BUMPER_A", "_KIT00_REAR_BUMPER_A",
    "_KIT00_FRONT_WHEEL_A",
    "_KIT00_FULLROOF_A",
    "_KIT00_HEADLIGHT_LEFT_A", "_KIT00_HEADLIGHT_RIGHT_A",
    "_KIT00_HOOD_A", "_KIT00_HOOD_UNDER_A",
    "_KIT00_LEFT_SIDE_MIRROR_A", "_KIT00_RIGHT_SIDE_MIRROR_A",
    "_KIT00_ROOF_A",
    "_KIT00_SKIRT_A",
    "_KIT00_SPOILER_A",
    "_KIT00_TRUNK_A", "_KIT00_TRUNK_UNDER_A",
]

# The traffic cars (CarTypeInfo +0x844 == 2; TASKS.md #110, #316) have no KIT00
# parts: a body at three LODs and one tyre at two, <CAR>_BODY_A..C and
# <CAR>_TIRE_FRONT_A/B. Each part carries the car LODs it is drawn at as
# MODEL3D_BATCH_LOD_* bits (model3d.h), so one file serves every distance.
# Their own TEXTURES.BIN is empty: the paint is <CAR>_SKIN1..8 in the shared
# CARS/TEXTURES.BIN, one picked per car (StreamingTrafficCar::SetRandomSkin),
# so every skin the car ships goes in the file and the runtime swaps SKIN1's
# slot for the one it drew.
TRAFFIC_PARTS = [
    ("_BODY_A", 0x1), ("_BODY_B", 0x2), ("_BODY_C", 0xC),
    ("_TIRE_FRONT_A", 0x1), ("_TIRE_FRONT_B", 0xE),
]
TRAFFIC_WHEEL_PARTS = ("_TIRE_FRONT_A", "_TIRE_FRONT_B")
TRAFFIC_SKINS = 8
BATCH_LOD_SHIFT = 16
# Retail does set bit 16 of the sub-mesh word (29,586 car sub-meshes, e.g.
# 4DR_SEDAN02_BODY_B's 0x14180). Neither renderer reads it -- SPEED2.EXE's
# strip draw (0x5cf420) tests 0x1 and its shader setter (0x5bd4a0) 0x80/0x100;
# Speed.elf's epRenderStrips keeps a u16 and tests 0x4180 -- but here it is the
# LOD-A bit of the mask below, and left in it drew 4DR_SEDAN02's BODY_B sub-meshes
# over its BODY_A up close. So it is cleared before the LODs go in (TASKS.md #107).
BATCH_LOD_MASK = 0x000F0000
# Batch flags bits 20-23 (model3d.h MODEL3D_BATCH_CORNER_* / _WHEEL_*): the
# tyre a batch is -- bits 22-23 MODEL3D_WHEEL_RIM, bits 20-21 its corner, 0 FL,
# 1 FR, 2 RR, 3 RL -- and 0 on every other batch. The runtime turns each tyre
# with its corner's wheel (CarState::CalculateWheelMatrices, Speed.elf
# 0x801aa974; RenderFast draws a traffic car's four by them) about the pivots
# in the extras below. GLOBALB's parts database puts a traffic car's one tyre
# part in the front wheel slot and has no rear wheel part at all, so its
# REAR_WHEEL models are the same <CAR>_TIRE_FRONT_A/B: in game, at the car's
# LOD A, CarRenderInfo::Render (0x8023d548 / 0x8023d6f8) draws a rear one whose
# inner side faces the camera with its B model (+0x1b60), and traffic.c gives
# the runtime the two rear tyres' inboard axes. Retail sub-mesh flags never use
# these bits (over every CARS GEOMETRY.BIN they OR to 0x0001dd80).
BATCH_CORNER_SHIFT = 20
BATCH_WHEEL_MASK = 0x00F00000
WHEEL_RIM = 1

# A traffic car's extras (#316), in the block between the index pool and the
# texture blob where a world section keeps its instance table:
#
#   char magic[4] "TRFX"; u32 version 2; u32 numBatches; u32 numLights;
#   u32 lightMaterial[numBatches]      the eLightMaterial each batch is authored
#                                      with, in batch order
#   { u8 type, side, pad[2]; f32 pos[3]; f32 dir[3]; } light[numLights]
#   f32 pivot[4][3]                    version 2: where each corner's tyre turns,
#                                      FL FR RR RL -- the wheel record's x and z
#                                      and the tyre's fender line, where a stock
#                                      tyre's wheel matrix sits (carkit.c
#                                      place_wheels); all 0 with no wheel table
#
# CarRenderInfo::Render lights a traffic car by its own materials -- TRAFFIC,
# TRAFFICWINDOWS, MOLDINGS, BOTTOM, PLAINNOTHING, RUBBER -- none of which is a
# paint it swaps, and CreateCarLightFlares (Speed.elf 0x80235e50) makes a
# flare of each LEFT_/RIGHT_/CENTRE_ HEADLIGHT/BRAKELIGHT marker on model 0
# of the car's slots, which for a traffic car is the BODY's: <CAR>_BODY_A
# (traffic.c reads both). A light's pos is the marker's translation less
# that BODY's pivot (CarRenderInfo+0x30, eModel::GetPivotPosition: its part
# record matrix's translation), as CreateCarLightFlares places it; BUS, COUPE
# and TAXI are the traffic bodies with one off zero, by up to 2.5 cm (BUS,
# like AMBULANCE and FIRETRUCK, weighs 0 in every zone, so never streams).
TRAFFIC_EXTRA_MAGIC = b"TRFX"
TRAFFIC_EXTRA_VERSION = 2
TRAFFIC_LIGHTS_MAX = 16        # carkit.h's CARKIT_LIGHTS
LIGHT_MARKERS = {              # hash -> (CARKIT_LIGHT_HEAD/_BRAKE, _LEFT/_RIGHT/_CENTRE)
    0x9db90133: (0, 0),        # LEFT_HEADLIGHT
    0xd09091c6: (0, 1),        # RIGHT_HEADLIGHT
    0x7a5bcf69: (0, 2),        # CENTRE_HEADLIGHT
    0x31a66786: (1, 0),        # LEFT_BRAKELIGHT
    0xbf700a79: (1, 1),        # RIGHT_BRAKELIGHT
    0xa2a2fc7c: (1, 2),        # CENTRE_BRAKELIGHT
}


def traffic_extra(batch_mats, body_markers, pivot, wheel_pivots):
    lights = [(LIGHT_MARKERS[h], m) for h, m in body_markers if h in LIGHT_MARKERS]
    lights = lights[:TRAFFIC_LIGHTS_MAX]
    out = struct.pack("<4sIII", TRAFFIC_EXTRA_MAGIC, TRAFFIC_EXTRA_VERSION, len(batch_mats),
                      len(lights))
    out += struct.pack("<%dI" % len(batch_mats), *batch_mats)
    for (ty, side), m in lights:
        out += struct.pack("<BBxx6f", ty, side, m[12] - pivot[0], m[13] - pivot[1],
                           m[14] - pivot[2], m[8], m[9], m[10])
    for c in range(4):
        out += struct.pack("<3f", *wheel_pivots.get(c, (0.0, 0.0, 0.0)))
    return out


# ---------------------------------------------------------------- chunk walk

def walk_chunks(data):
    """Pre-order (document-order) walk of the whole bChunkHeader tree.
    Returns a list of (type, payload_offset, payload_size)."""
    out = []

    def rec(base, end):
        off = base
        while off + 8 <= end:
            ttype, tsize = struct.unpack_from("<II", data, off)
            payload = off + 8
            if ttype == 0 and tsize == 0:
                off += 8      # 8-byte pad, not a terminator
                continue
            if payload + tsize > end:
                break
            out.append((ttype, payload, tsize))
            if ttype & 0x80000000:
                rec(payload, payload + tsize)
            off = payload + tsize

    rec(0, len(data))
    return out


def read_u32_table(data, payload, size, stride):
    """Read the first u32 of each `stride`-byte record in a leaf, after the
    leading 0x11 alignment filler."""
    start = leaf_start(data, payload, size, stride)
    n = (size - (start - payload)) // stride
    return [struct.unpack_from("<I", data, start + i * stride)[0] for i in range(n)]


def parse_geometry(data):
    """Return the list of parts in document order. 0x134011 part records and
    0x134900 mesh objects are strictly 1:1 and in the same order."""
    nodes = walk_chunks(data)
    parts, meshes = [], []
    cur = mcur = None
    for ttype, payload, size in nodes:
        if ttype == 0x134011:
            # A track section pads a part record up to its runtime alignment
            # with a leading run of 0x11 filler, exactly as it does its
            # vertex/index leaves -- so the record's fields are measured from
            # the first non-filler byte, not from the chunk payload. Car
            # GEOMETRY.BIN records carry no filler, so this is a no-op there.
            rec = leaf_start(data, payload, size, PART_RECORD_SIZE)
            name = data[rec + PART_RECORD_NAME_OFF:
                        rec + PART_RECORD_NAME_OFF + 28].split(b"\x00")[0]
            cur = {
                "name": name.decode("ascii", "replace"),
                "hash": struct.unpack_from("<I", data, rec + PART_RECORD_HASH_OFF)[0],
                "matrix": struct.unpack_from("<16f", data, rec + PART_RECORD_MATRIX_OFF),
                "textures": [],
                # The three eSolid fields Speed.elf's DrawAScenery (0x80208fa4)
                # picks a scenery LOD with: the polygon count, read signed
                # (lha), then the volume and polys-per-volume density. Only
                # track_pack.py reads them.
                "polys": struct.unpack_from("<h", data, rec + PART_RECORD_POLYS_OFF)[0],
                "volume": struct.unpack_from("<f", data, rec + PART_RECORD_VOLUME_OFF)[0],
                "density": struct.unpack_from("<f", data, rec + PART_RECORD_DENSITY_OFF)[0],
            }
            parts.append(cur)
        elif ttype == 0x134012 and cur is not None:
            # 8-byte records {u32 nameHash; u32 runtimeTexturePtr (0 in file)}.
            # Measured from the chunk payload with the leaf's own stride, which
            # is what read_u32_table() already does: pre-skipping ran the 0x11
            # run-length scan a second time, and a hash whose low byte is 0x11
            # (XW_CHAIN_FENCEA hashes to 0x70a13911) is indistinguishable from
            # one more filler byte -- so every slot in the leaf came out shifted
            # down a byte and matched no texture at all. 26 of L4RA's leaves
            # lead with such a hash, which is what drew the overpass fences and
            # their posts as untextured grey panels (TASKS.md #225).
            cur["textures"] = read_u32_table(data, payload, size, 8)
        elif ttype == 0x134013 and cur is not None:
            # The part's light-material keys, same 8-byte {key, runtime ptr}
            # shape; a subset's +0x20 indexes it (subset_materials()).
            cur["materials"] = read_u32_table(data, payload, size, 8)
        elif ttype == 0x134900:
            start = leaf_start(data, payload, size, MESH_HEADER_SIZE)
            mcur = {
                "numSubsets": struct.unpack_from("<I", data, start + 0x10)[0],
                "numTris": struct.unpack_from("<I", data, start + 0x24)[0],
                "numVerts": struct.unpack_from("<I", data, start + 0x34)[0],
            }
            meshes.append(mcur)
        elif ttype == 0x134b01 and mcur is not None:
            mcur["vb"] = (payload, size)
        elif ttype == 0x134b02 and mcur is not None:
            mcur["subsets"] = (payload, size)
        elif ttype == 0x134b03 and mcur is not None:
            mcur["ib"] = (payload, size)

    if len(parts) != len(meshes):
        raise SystemExit("part/mesh count mismatch: %d != %d" % (len(parts), len(meshes)))
    for p, m in zip(parts, meshes):
        p["mesh"] = m
    return parts


def read_mesh(data, part):
    m = part["mesh"]
    vs = leaf_start(data, m["vb"][0], m["vb"][1], VERTEX_STRIDE)
    verts = []
    for i in range(m["numVerts"]):
        b = vs + i * VERTEX_STRIDE
        x, y, z, nx, ny, nz = struct.unpack_from("<6f", data, b)
        u, v = struct.unpack_from("<2f", data, b + 28)
        verts.append((x, y, z, nx, ny, nz, u, v))

    # The index leaf is 4-byte aligned, so an odd triangle count leaves one
    # spurious trailing u16 in the leaf. 0x134900's +0x24 triangle count is
    # authoritative; everything is a plain triangle list (there are no strips).
    istart = leaf_start(data, m["ib"][0], m["ib"][1], 2)
    ni = m["numTris"] * 3
    indices = list(struct.unpack_from("<%dH" % ni, data, istart))

    sstart = leaf_start(data, m["subsets"][0], m["subsets"][1], SUBSET_STRIDE)
    subsets = []
    for i in range(m["numSubsets"]):
        b = sstart + i * SUBSET_STRIDE
        index_count = struct.unpack_from("<I", data, b + 0x0c)[0]
        tex_slot = struct.unpack_from("<I", data, b + 0x1c)[0]
        first_index = struct.unpack_from("<I", data, b + 0x34)[0]
        flags = struct.unpack_from("<I", data, b + 0x38)[0]
        subsets.append((first_index, index_count, tex_slot, flags))
    return verts, indices, subsets


def subset_materials(data, part):
    """The light-material key of each of the part's subsets, in read_mesh()
    order: subset +0x20 indexes the part's 0x134013 list. 0 for a subset
    whose index runs past the list (or a part that has none)."""
    m = part["mesh"]
    mats = part.get("materials", [])
    sstart = leaf_start(data, m["subsets"][0], m["subsets"][1], SUBSET_STRIDE)
    out = []
    for i in range(m["numSubsets"]):
        mi = struct.unpack_from("<I", data, sstart + i * SUBSET_STRIDE + 0x20)[0]
        out.append(mats[mi] if mi < len(mats) else 0)
    return out


# ------------------------------------------------- logical texture slots
#
# A 0x134012 entry is NOT a shipped texture name. It is a *logical slot* the
# retail loader fills in per car at load time. SPEED2.EXE builds the slot array
# inline around file offset 0x2385xx as a run of
#   mov dword ptr [esi+disp32], imm32
# writing one bStringHash constant per 16-byte record; decoding that run gives
# the table below, and the slot index is (disp - 0x668) / 0x10.
#
# Slots 2..7 are absent from that run because they are *not* constants: they are
# built at runtime by sprintf from the literals "%s_SKIN1", "%s_SKIN1B",
# "%s_SKIN2".."%s_SKIN4" with the car name, which is exactly why 240SX_SKIN1
# (0x77a20245) appears in GEOMETRY.BIN but nowhere in the executable.
#
# Names below are recovered by preimage search against bStringHash (djb2, seed
# 0xFFFFFFFF, multiplier 33) and each is verified to hash back to its constant --
# see docs/formats/geometry-bin.md. Slot 11 and slots 17..22 are the window
# slots UpdateCarReplacementTextures (Speed.elf 0x802353FC) walks; its two
# damage tables start at WINDOW and WINDOW2, which gave the pattern. Slots 0
# and 1, the body-paint layers below, are still unnamed and kept as raw hashes.
CAR_SLOT_NAMES = {
    0x5799e60b: "DRIVER",                    # slot  8
    0x7b220ddf: "WINDOW_FRONT",              # slot 10
    0x1b0763a0: "WINDOW_REAR",               # slot 11
    0xe7e4ef49: "WINDOW_LEFT_FRONT",         # slot 12
    0x4cdebfca: "WINDOW_LEFT_REAR",          # slot 13
    0x60f8b13c: "WINDOW_RIGHT_FRONT",        # slot 14
    0x0ab88f5d: "WINDOW_RIGHT_REAR",         # slot 15
    0xd195be56: "REAR_DEFROSTER",            # slot 16
    0x90e09671: "WINDOW2_FRONT",             # slot 17
    0x13ee21f2: "WINDOW2_REAR",              # slot 18
    0xd689a31b: "WINDOW2_LEFT_FRONT",        # slot 19
    0xb8f3415c: "WINDOW2_LEFT_REAR",         # slot 20
    0x2433df4e: "WINDOW2_RIGHT_FRONT",       # slot 21
    0xf95d432f: "WINDOW2_RIGHT_REAR",        # slot 22
    0xa7e6ea53: "HEADLIGHT_LEFT",            # slot 23
    0xa532fc46: "HEADLIGHT_RIGHT",           # slot 24
    0xd947f346: "BRAKELIGHT_LEFT",           # slot 25
    0x02b52399: "BRAKELIGHT_RIGHT",          # slot 26
    0x3617dcfc: "BRAKELIGHT_CENTRE",         # slot 27
    0x267d188c: "HEADLIGHT_GLASS_LEFT",      # slot 28
    0xf68ef19f: "HEADLIGHT_GLASS_RIGHT",     # slot 29
    0x000bcbbf: "BRAKELIGHT_GLASS_LEFT",     # slot 30
    0x01f40b32: "BRAKELIGHT_GLASS_RIGHT",    # slot 31
    0x1d33b7b5: "BRAKELIGHT_GLASS_CENTRE",   # slot 32
    0x54949afd: "CHROME",
    0xbd1f0e0a: "CARBOTTOM",
    0x69c032bb: "GRILL_03",
    0x69c032bc: "GRILL_04",
}
# DUMMY_DECAL1..8 -- the eight consecutive hashes every car's table carries.
CAR_SLOT_NAMES.update({0x910e6653 + i: "DUMMY_DECAL%d" % i for i in range(1, 9)})

# Slots 0 and 1 are the two body-paint layers. Neither is a shipped texture: the
# retail runtime composites them (CarLoadedSkinLayerSlotPool) from the player's
# paint colour, the vinyl layers and the car's DOORLINE maps. Slot 1 carries the
# outer painted shell (45% of a stock 240SX by triangle count) and slot 0 a small
# door/skirt band; <CAR>_SKIN1 carries everything else paintable. All three
# sample the one skin, which the runtime composites the way retail does
# (3DS Files/source/carskin.c), so the pack gives them a placeholder.
SLOT_BODY_OUTER = 0x3c84d757     # slot 1
SLOT_BODY_TRIM = 0xa7366ae6      # slot 0

# Every body slot SwitchSkin (SPEED2.EXE 0x636840) points at the composited
# skin: the two unnamed body-paint layers, GLOBAL_SKIN1, SPOILER_SKIN and
# ROOF_SKIN, beside the car's own <CAR>_SKIN1. The mirrors, brakes, exhaust
# tips, spoilers and roof scoops sample its colour swatches through them.
SKIN_SLOTS = (SLOT_BODY_OUTER, SLOT_BODY_TRIM, b_string_hash("GLOBAL_SKIN1"),
              b_string_hash("SPOILER_SKIN"), b_string_hash("ROOF_SKIN"))

# The skin is retail's 512x512 DUMMY_SKIN, capped at --max-texture.
SKIN_SIZE = 512

# Shipped car textures are the *unlit* light states -- the retail names all end in
# _OFF (240SX_KIT00_HEADLIGHT_OFF, 240SX_KIT00_BRAKELIGHT_GLASS_OFF, ...). There
# is no lit texture to swap in: UpdateLightStateTextures (Speed.elf 0x802356A8,
# SPEED2.EXE 0x60C940) only ever picks among the _OFF names, and a lit lamp is
# its flares. Speed.elf then points the two headlight lenses at WINDOW;
# SPEED2.EXE leaves them on _GLASS_OFF, and this is PC data, so they keep it.
SLOT_TO_CAR_TEXTURE = {
    "HEADLIGHT_LEFT":          "%s_KIT00_HEADLIGHT_OFF",
    "HEADLIGHT_RIGHT":         "%s_KIT00_HEADLIGHT_OFF",
    "HEADLIGHT_GLASS_LEFT":    "%s_KIT00_HEADLIGHT_GLASS_OFF",
    "HEADLIGHT_GLASS_RIGHT":   "%s_KIT00_HEADLIGHT_GLASS_OFF",
    "BRAKELIGHT_LEFT":         "%s_KIT00_BRAKELIGHT_OFF",
    "BRAKELIGHT_RIGHT":        "%s_KIT00_BRAKELIGHT_OFF",
    "BRAKELIGHT_GLASS_LEFT":   "%s_KIT00_BRAKELIGHT_GLASS_OFF",
    "BRAKELIGHT_GLASS_RIGHT":  "%s_KIT00_BRAKELIGHT_GLASS_OFF",
    "BRAKELIGHT_CENTRE":       "%s_CENTRE_BRAKELIGHT_OFF",
    "BRAKELIGHT_GLASS_CENTRE": "%s_CENTRE_BRAKELIGHT_GLASS_OFF",
}

# CHROME / CARBOTTOM / REAR_DEFROSTER / DRIVER are car-independent and live in
# GLOBAL/GLOBALB.BUN's Global\GlobalTextures.tpk, not in the car's TEXTURES.BIN.
GLOBAL_TEXTURE_BUN = os.path.join("GLOBAL", "GLOBALB.BUN")

# The glass. UpdateCarReplacementTextures (Speed.elf 0x802353FC, SPEED2.EXE
# 0x615280) points the six WINDOW_* slots at DamagedWindowHashes[damage] and
# the six WINDOW2_* slots at DamagedWindow2Hashes[damage] (Speed.elf
# 0x8037127C / 0x8037128C). An undamaged car is index 0: WINDOW, the dark pane
# with the black sun strip in GLOBALB, and WINDOW2, which no retail file ships
# and no retail car references. REAR_DEFROSTER keeps its own name until the
# rear window breaks, so it resolves from the global pack as it stands.
# The cracked panes (DAMAGE0..2) need car damage, which the port does not have.
WINDOW_TEXTURE = b_string_hash("WINDOW")
WINDOW2_TEXTURE = b_string_hash("WINDOW2")
SLOT_REPLACEMENT = {}
for _w in ("FRONT", "REAR", "LEFT_FRONT", "LEFT_REAR", "RIGHT_FRONT", "RIGHT_REAR"):
    SLOT_REPLACEMENT[b_string_hash("WINDOW_" + _w)] = WINDOW_TEXTURE
    SLOT_REPLACEMENT[b_string_hash("WINDOW2_" + _w)] = WINDOW2_TEXTURE

# A texture's TPK render state as one u16, the TexEntry `mat` model3d.c's
# MAT_BLEND/MAT_USAGE/MAT_DEPTH read. tools/extract_global_textures.py says
# where each field comes from and what its values mean; track_pack.py packs
# world textures with this same function.
MAT_BLEND_NORMAL = 1


def texture_material(t):
    blend = t["alpha_blend"]
    if blend not in (0, 1, 2):
        blend = MAT_BLEND_NORMAL
    usage = t["alpha_usage"] if t["alpha_usage"] in (0, 1, 2) else 2
    order = min(t["rendering_order"], 7)
    return (blend & 3) | ((usage & 3) << 2) | ((1 if t["write_depth"] else 0) << 4) \
        | ((order & 7) << 5)


# A TEXTURES.BIN block ends in its texture's TextureInfo record (0x7C bytes)
# and that texture's 0x20-byte format record. The record is the one a TPK
# carries: the name is at +0x0c, the dimensions at +0x44 and the fourcc at
# 0x7c + 0x14, the offsets extract_global_textures.py reads in a TPK, so the
# render state is at its offsets too.
#
# On the two career cars that gives: every _GLASS_OFF lens additive (blend 2,
# no depth write) over an opaque _OFF lamp, the badging and doorline art
# blended, the tyre punchthrough, and everything else opaque.
BLOCK_TRAILER_SIZE = 156
TI_RENDERING_ORDER = 0x51
TI_ALPHA_USAGE = 0x55
TI_ALPHA_BLEND = 0x56
TI_WRITE_DEPTH = 0x57


def trailer_material(block):
    t = block[-BLOCK_TRAILER_SIZE:]
    return texture_material({"rendering_order": t[TI_RENDERING_ORDER],
                             "alpha_usage": t[TI_ALPHA_USAGE],
                             "alpha_blend": t[TI_ALPHA_BLEND],
                             "write_depth": t[TI_WRITE_DEPTH]})


# The composited skin has no record, so it says what it is: the opaque body,
# its alpha the paint's gloss and never coverage.
MAT_SKIN = texture_material({"alpha_blend": 0, "alpha_usage": 0, "write_depth": 1,
                             "rendering_order": 0})


class TextureTable(dict):
    """hash -> (name, w, h, rgba), plus `mat`: hash -> texture_material() for
    every entry whose render state is known. resolve_slots() also fills
    `src`: slot -> the texture hash it resolved to, which several slots can
    share (both headlights, all six windows)."""

    def __init__(self):
        dict.__init__(self)
        self.mat = {}
        self.src = {}


def load_global_car_textures(pc_root, verbose=False):
    r"""hash -> (name, w, h, rgba) for Global\GlobalTextures.tpk."""
    path = os.path.join(pc_root, GLOBAL_TEXTURE_BUN)
    if not os.path.isfile(path):
        return TextureTable()
    data = open(path, "rb").read()
    out = TextureTable()
    for off, size in egt.find_containers(data):
        try:
            c = egt.parse_container(data, off, size)
        except Exception:
            continue
        for t in c.get("textures", []):
            if not t["pixel_data"] or not t["width"] or not t["height"]:
                continue
            try:
                if t["fourcc"] == b"RAWB":
                    rgba = egt.decode_bgra(t["pixel_data"][:t["expected_size"]],
                                           t["width"], t["height"])
                else:
                    rgba = egt.decode_dxt(t["pixel_data"][:t["expected_size"]],
                                          t["width"], t["height"], t["fourcc"])
            except Exception:
                continue
            out[t["hash"]] = (t["name"], t["width"], t["height"], rgba)
            out.mat[t["hash"]] = texture_material(t)
    if verbose:
        print("  global textures: %d from %s" % (len(out), GLOBAL_TEXTURE_BUN))
    return out


def textures_by_name(textures, name):
    """Look a shipped texture up by its *full* retail name.

    The name stored in a texture's block trailer is truncated at 20 bytes
    (240SX_KIT00_HEADLIGHT_OFF is only ever spelled "240SX_KIT00_HEADLIGH" on
    disk), so a literal string compare cannot find the long ones. The table's
    hash is over the untruncated name, so hashing the name we want and indexing
    by that is both exact and immune to the truncation.
    """
    return textures.get(b_string_hash(name))


def resolve_slots(car, slot_hashes, textures, globals_, verbose=False):
    """logical slot hash -> (name, w, h, rgba), or absent if unresolvable,
    with the render state of what each slot resolved to in `mat`.

    Resolution order mirrors what the retail loader does: a window slot takes
    the global pane UpdateCarReplacementTextures swaps in, whatever the car
    ships; otherwise a slot that names a texture the car ships wins outright,
    then the car-independent global pack, then the per-car pattern table (a
    light slot resolves to that car's _OFF state), then the layers the runtime
    composites rather than loads.
    """
    skin = b_string_hash(car + "_SKIN1")
    out = TextureTable()
    for th in sorted(slot_hashes):
        if not th:
            continue
        to = SLOT_REPLACEMENT.get(th)
        if to is not None:
            if to in globals_:
                out[th] = globals_[to]
                out.mat[th] = globals_.mat.get(to, 0)
                out.src[th] = to
            continue
        if th in textures:
            out[th] = textures[th]
            out.mat[th] = textures.mat.get(th, 0)
            out.src[th] = th
            continue
        if th in globals_:
            out[th] = globals_[th]
            out.mat[th] = globals_.mat.get(th, 0)
            out.src[th] = th
            continue
        name = CAR_SLOT_NAMES.get(th)
        pattern = SLOT_TO_CAR_TEXTURE.get(name)
        if pattern:
            hit = textures_by_name(textures, pattern % car)
            if hit is not None:
                out[th] = hit
                out.mat[th] = textures.mat.get(b_string_hash(pattern % car), 0)
                out.src[th] = b_string_hash(pattern % car)
                continue
        # The skin retail composites at car load (CompositeSkin) and the
        # runtime does too (carskin.c): every slot SwitchSkin points at it
        # shares the one placeholder, <CAR>_SKIN1, filled in at car load.
        if th in SKIN_SLOTS or th == skin:
            out[th] = ("<%s_SKIN1 composite>" % car, SKIN_SIZE, SKIN_SIZE, None)
            out.mat[th] = MAT_SKIN
            out.src[th] = skin
            continue
    if verbose:
        for th in sorted(slot_hashes):
            if th and th not in out:
                print("  UNRESOLVED slot 0x%08x %s"
                      % (th, CAR_SLOT_NAMES.get(th, "(name not recovered)")))
    return out


# ------------------------------------------------------------------ textures

def load_textures(path, exe_path):
    """hash -> (name, w, h, rgba_bytes) for every texture in a TEXTURES.BIN,
    with each one's render state in the table's `mat`."""
    if not os.path.isfile(path):
        return TextureTable()
    data = open(path, "rb").read()
    hd = huff.HuffDecompressor(exe_path) if exe_path and os.path.isfile(exe_path) else None
    out = TextureTable()
    for h, off, csize, _usize in iter_table_entries(data):
        magic = data[off:off + 4]
        try:
            if magic == b"JDLZ":
                blk, _ = jdlz.decompress(data, off)
            elif magic == b"HUFF" and hd is not None:
                blk = hd.decompress_block(data[off:off + csize])
            else:
                continue
        except Exception:
            continue
        dec = _decode_block_trailer(blk)
        if dec:
            name, w, h2, _fmt, rgba = dec
            out[h] = (name, w, h2, rgba)
            out.mat[h] = trailer_material(blk)
    return out


def box_downscale(rgba, w, h, maxdim):
    """Halve until both dimensions fit maxdim. Straight 2x2 box average --
    these are small detail maps, nothing here needs a fancy kernel."""
    while w > maxdim or h > maxdim:
        nw, nh = max(1, w // 2), max(1, h // 2)
        dst = bytearray(nw * nh * 4)
        for y in range(nh):
            for x in range(nw):
                acc = [0, 0, 0, 0]
                for dy in range(2):
                    for dx in range(2):
                        sx, sy = min(w - 1, x * 2 + dx), min(h - 1, y * 2 + dy)
                        s = (sy * w + sx) * 4
                        for c in range(4):
                            acc[c] += rgba[s + c]
                d = (y * nw + x) * 4
                for c in range(4):
                    dst[d + c] = acc[c] // 4
        rgba, w, h = bytes(dst), nw, nh
    return rgba, w, h


def _morton_xy(i):
    """PICA200 in-tile order: the even bits of i are x, the odd bits are y."""
    x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4)
    y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4)
    return x, y


_TILE = [0] * 64
for _i in range(64):
    _x, _y = _morton_xy(_i)
    _TILE[_y * 8 + _x] = _i


def encode_texture(rgba, w, h, rgba8=False):
    """RGBA8888 -> (fmt, PICA200-swizzled bytes).

    16bpp rather than RGBA8: these are 64..128px detail maps, so halving the
    linear-heap cost is free quality-wise here (the alternative, ETC1, needs a
    real encoder and hurts the sharp-edged light/badge decals more than a
    565/4444 quantise does). Picks RGB565 when the source is fully opaque and
    RGBA4444 otherwise.

    `rgba8` forces 32bpp RGBA8 instead, for art whose smooth 8-bit alpha 4444
    would band (tools/afx_pack.py's raw ARGB particles).
    """
    if rgba8:
        out = bytearray(w * h * 4)
        tiles_x = w // 8
        for y in range(h):
            ty, iy = y >> 3, y & 7
            for x in range(w):
                tx, ix = x >> 3, x & 7
                dst = ((ty * tiles_x + tx) * 64 + _TILE[iy * 8 + ix]) * 4
                s = (y * w + x) * 4
                # GPU_RGBA8 is ABGR in memory order.
                out[dst + 0] = rgba[s + 3]
                out[dst + 1] = rgba[s + 2]
                out[dst + 2] = rgba[s + 1]
                out[dst + 3] = rgba[s + 0]
        return FMT_RGBA8, bytes(out)

    opaque = all(rgba[i] == 255 for i in range(3, len(rgba), 4))
    fmt = FMT_RGB565 if opaque else FMT_RGBA4
    out = bytearray(w * h * 2)
    tiles_x = w // 8
    for y in range(h):
        ty, iy = y >> 3, y & 7
        for x in range(w):
            tx, ix = x >> 3, x & 7
            dst = ((ty * tiles_x + tx) * 64 + _TILE[iy * 8 + ix]) * 2
            s = (y * w + x) * 4
            r, g, b, a = rgba[s], rgba[s + 1], rgba[s + 2], rgba[s + 3]
            if fmt == FMT_RGB565:
                px = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            else:
                px = ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4)
            out[dst] = px & 0xff
            out[dst + 1] = px >> 8
    return fmt, bytes(out)


def mip_max_level(w, h):
    """How many mip levels below level 0 the PICA200 will accept.

    Mirrors citro3d's C3D_TexCalcMaxLevel exactly -- log2(min(w,h)) - 3 -- and
    that -3 is not arbitrary: the hardware will not sample a level smaller than
    8x8, so the chain stops there rather than at 1x1. Getting this wrong by one
    is not a visual bug, it is a GPU that reads past the end of the allocation,
    so it is derived from the same expression the runtime allocates with rather
    than recomputed independently.
    """
    m = min(w, h)
    lvl = 0
    while m > 8:
        m >>= 1
        lvl += 1
    return lvl


def encode_texture_level0_size(w, h, fmt):
    """Bytes level 0 alone occupies, for reporting the mip overhead."""
    return w * h * (4 if fmt == FMT_RGBA8 else 2)


def tex_fmt_field(fmt, maxlvl):
    """Pack a TexEntry's fmt + maxLevel into the one u16 the format has."""
    assert 0 <= fmt <= 0xff and 0 <= maxlvl <= 0xf, (fmt, maxlvl)
    return fmt | (maxlvl << 8)


def tex_fmt_of(field):
    return field & 0xff


def tex_fmt_level(field):
    return (field >> 8) & 0xf


def encode_texture_chain(rgba, w, h, rgba8=False):
    """encode_texture(), plus the whole mip chain after it.

    Returns (fmt, concatenated levels, maxLevel). Level n is level n-1
    box-downscaled by half, encoded with the same format, and appended -- which
    is the layout C3D_TexGetImagePtr walks, since each level is exactly a
    quarter of the one above it.

    **Why mips are generated here and not by C3D_TexGenerateMipmap:** the world
    streams sections in mid-drive, and generating a chain on the GPU at load
    time puts display-transfer work on exactly the frames that are already
    paying for a section load. Doing it offline costs 33% more bytes and zero
    runtime. It also means the downscale is the same box filter the rest of this
    tool uses, on the un-quantised RGBA source, rather than a repeated filter
    over already-quantised 565/4444 data.

    The 33% is also what buys the port a per-console quality setting for free:
    a memory-tight console can upload the chain starting at level 1 and get the
    old 128px world out of the same 256px file, with no second pack (world.c).
    """
    fmt, level0 = encode_texture(rgba, w, h, rgba8)
    blob = bytearray(level0)
    maxlvl = mip_max_level(w, h)
    cur, cw, ch = rgba, w, h
    for _ in range(maxlvl):
        # box_downscale halves while either dimension exceeds the cap, so a cap
        # of max(w,h)//2 is exactly one halving for any aspect ratio.
        cur, cw, ch = box_downscale(cur, cw, ch, max(cw, ch) // 2)
        f, px = encode_texture(cur, cw, ch, rgba8)
        assert f == fmt, "mip level changed format: %d != %d" % (f, fmt)
        blob.extend(px)
    return fmt, bytes(blob), maxlvl


# ---------------------------------------------------------------------- pack

def merge_batches(batches, indices):
    """Collapse a batch list to one batch per (texture, flags) pair.

    A track section is assembled from hundreds of prop instances, and each one
    contributes its own sub-mesh per texture slot -- so a city block that uses
    ninety textures still arrives as thirteen hundred draw calls, most of them a
    dozen triangles wide. That is not a throughput problem, it is a hard limit:
    every C3D_DrawElements() writes its own command words into citro3d's
    command buffer, and at that count the buffer overruns inside a single frame
    and the GPU never finishes it -- which on hardware and in Azahar alike looks
    like the app freezing on the last frame it managed to present.

    So the index pool is reordered here, at pack time, into one run per
    (texture, flags) pair, in first-appearance order. Nothing about the geometry
    changes: the same triangles reference the same vertices, they are simply
    grouped by what they are drawn with. Draw order within a texture is not
    meaningful either -- the runtime alpha-tests cutouts rather than blending
    them (see model3d.c's drawBatches), so it does not depend on the retail
    file order that this destroys.

    Returns (batches, indices), both new lists.
    """
    groups = {}
    for (first, count, ti, flags) in batches:
        if count <= 0:
            continue
        groups.setdefault((ti, flags), []).extend(indices[first:first + count])
    out_batches, out_indices = [], []
    for (ti, flags), idxs in groups.items():
        out_batches.append((len(out_indices), len(idxs), ti, flags))
        out_indices.extend(idxs)
    return out_batches, out_indices


def pack_nfsc(tex_entries, tex_blob, batches, verts, indices, bbmin, bbmax, shading,
              lod_split=None, extra=None):
    """Serialise one .nfsc. Shared with tools/scene_pack.py, which fills these
    same lists from a FRONTEND/PLATFORMS room instead of from a car.

    `lod_split` is (nearFirstBatch, farFirstBatch) for a world section that
    carries its scenery at two levels of detail (tools/track_pack.py), and None
    for everything else.

    `extra` is a block written between the index pool and the texture blob --
    a world section's instance table (track_pack.pack_instance_block) -- and
    must be a multiple of 4 bytes."""
    hdr_size = 0x40
    body = bytearray()
    for entry in tex_entries:
        # A 6th field is the packed TPK render state (texture_material()).
        # World sections and cars carry one; the frontend rooms, the sky and
        # the shadow do not yet, so they pass five and get material 0, which
        # model3d.c reads as "no material data" and draws as before.
        th, w, h, fmt, off = entry[:5]
        mat = entry[5] if len(entry) > 5 else 0
        body += struct.pack("<IHHHHI", th, w, h, fmt, mat, off)
    for (fi, ic, ti, fl) in batches:
        body += struct.pack("<IIiI", fi, ic, ti, fl)
    for v in verts:
        body += struct.pack("<8f", *v)
    idx_bytes = struct.pack("<%dH" % len(indices), *indices)
    body += idx_bytes
    if len(idx_bytes) % 4:
        body += b"\x00" * (4 - len(idx_bytes) % 4)
    if extra:
        assert len(extra) % 4 == 0, len(extra)
        body += bytes(extra)
    tex_blob_off = hdr_size + len(body)
    body += bytes(tex_blob)

    # 0x3c carries the LOD split, if the caller has one: the batch index the
    # near-LOD run starts at and the one the far-LOD run starts at. Both are
    # biased by one so that an all-zero field keeps meaning "no LOD split" --
    # which is every car, every room, the skydome, the shadow quad, and every
    # world blob built before this field meant anything.
    lod = 0
    if lod_split is not None:
        near_first, far_first = lod_split
        assert 0 <= near_first <= far_first <= len(batches) < 0xffff
        lod = ((near_first + 1) & 0xffff) | (((far_first + 1) & 0xffff) << 16)

    hdr = struct.pack("<4sIIIII3f3fIIII", b"NFSC", NFSC_VERSION, len(tex_entries), len(batches),
                      len(verts), len(indices), bbmin[0], bbmin[1], bbmin[2],
                      bbmax[0], bbmax[1], bbmax[2], tex_blob_off, len(tex_blob), shading, lod)
    assert len(hdr) == hdr_size, len(hdr)
    return bytes(hdr) + bytes(body)


# ------------------------------------------------------------- wheel corners

# GLOBAL/GLOBALB.BUN carries the CarTypeInfo array: 46 records of 0x890 bytes,
# each holding the "CARS\<car>\GEOMETRY.BIN" path string at a fixed offset.
# 0xe0 past that string sits a four-entry table of 0x30-byte wheel records --
# the corner placements, in the order front-left, front-right, rear-right,
# rear-left:
#
#   +0x00  f32[3]  wheel centre in car space; y is the wheel's *centre plane*
#   +0x10  f32     tyre radius
#   +0x14  f32     tyre width
#   +0x1c  f32     outer/fender line
#   +0x20  f32     a second, wider lateral reference (widebody kits)
#
# Both lines are signed like the corner's y (left +, right -). The kit path
# (carkit.c, TASKS.md #204) seats every rim's outer face on one of them, as
# Render__13CarRenderInfo does; this file's own stock bake still centres the
# wheel on the centre plane (part_placements()).
#
# Identified and checked three ways. The mirror invariants -- the front pair
# and the rear pair differ only in the sign of y, and front x > rear x -- hold
# for all 46 records. The radius and width agree with the car's own
# FRONT_WHEEL_A mesh bounds (HUMMER ships a 0.284-wide wheel against a 0.285
# table entry, TIBURON 0.318/0.217 against 0.307/0.210). And +0x1c tracks the
# body part's own max |Y| to within a few mm on every car sampled, which is
# what pins that field as the fender line.
#
# The anchor matters: the table belongs to the record it sits *inside*, 0xe0
# past that record's own path string. Reading it at the superficially neater
# -0x7b0 from the *next* record's path picks up the previous car's wheels and
# is wrong by exactly one record -- yet it still satisfies the mirror
# invariants for 45 of 46 cars, because every record is internally consistent.
# Only PEUGOT, the first record and so the one with no predecessor to borrow
# from, reads NaN under that anchor. The mesh-bounds check is what settled it:
# +0xe0 matched the car's own wheel mesh 4/4, the shifted anchor did not.
CAR_TYPE_INFO_BUN = os.path.join("GLOBAL", "GLOBALB.BUN")
WHEEL_TABLE_REL = 0xe0
WHEEL_RECORD_STRIDE = 0x30

# Which corners each origin-authored part stands in for. One wheel mesh serves
# all four; the brakes ship as a front and a rear pair.
STOCK_WHEEL_PART = "_KIT00_FRONT_WHEEL_A"
WHEEL_CORNER_PARTS = {
    STOCK_WHEEL_PART: (0, 1, 2, 3),
    "_KIT00_FRONT_BRAKE_A": (0, 1),
    "_KIT00_REAR_BRAKE_A": (2, 3),
    TRAFFIC_WHEEL_PARTS[0]: (0, 1, 2, 3),
    TRAFFIC_WHEEL_PARTS[1]: (0, 1, 2, 3),
}
FLIP_MIRROR = "mirror"
FLIP_TURN = "turn"

# The stock exhaust tip, the EXHAUST slot's model 0. Render__13CarRenderInfo
# draws it once per exhaust marker that FUN_00638190 found on model 0 of the
# drawn slots, at R * marker (TASKS.md #204 slice 2; carkit.c's place_rear()
# does the same for an assembled car). Placed by its own translation, which is
# zero, the tip hung under the middle of every stock car (the COROLLA's 56 cm
# pipe ran forward from the origin).
STOCK_EXHAUST_PART = "_KIT00_EXHAUST_A"
EXHAUST_MARKERS = (
    0xdf066a41,  # CENTRE_EXHAUST
    0xf21be91e,  # EXHAUST_RIGHT
    0xa5b28001,  # EXHAUST
    0xa2799c2b,  # EXHAUST_LEFT
    0xbcf8a18b,  # LEFT_EXHAUST
    0xbd7cf15e,  # RIGHT_EXHAUST
)
MAX_REAR_MARKERS = 8           # carkit.c's cap on each list

# Every rim is authored this wide (SPEED2.EXE DAT_008026c4), outer face at
# y = 0; a stock wheel is drawn at it unscaled.
RIM_AUTHORED_WIDTH = 0.184

# ePositionMarker lists: one 0x13401a chunk per part that has any.
MARKER_CHUNK = 0x13401a
MARKER_STRIDE = 0x50           # { u32 hash; u8 pad[12]; f32 m[16]; }
MARKER_BRAKE = 0xea8dfb05      # bStringHash("MARKER_BRAKE"), on a wheel's model 0


def parse_markers(data):
    """part hash -> [(marker hash, m[16])] for every part that carries any."""
    out = {}
    cur = None
    for ttype, payload, size in walk_chunks(data):
        if ttype == 0x134011:
            rec = leaf_start(data, payload, size, PART_RECORD_SIZE)
            cur = struct.unpack_from("<I", data, rec + PART_RECORD_HASH_OFF)[0]
        elif ttype == MARKER_CHUNK and cur is not None:
            s = leaf_start(data, payload, size, MARKER_STRIDE)
            n = (size - (s - payload)) // MARKER_STRIDE
            for i in range(n):
                o = s + i * MARKER_STRIDE
                h = struct.unpack_from("<I", data, o)[0]
                m = struct.unpack_from("<16f", data, o + 0x10)
                out.setdefault(cur, []).append((h, m))
    return out


def brake_depth(markers, wheel_hash):
    """How far inboard of the fender line a stock car's brakes sit: the stock
    wheel's MARKER_BRAKE y, or half the authored width when it has none."""
    for h, m in markers.get(wheel_hash, []):
        if h == MARKER_BRAKE and m[13] != 0.0:
            return m[13]
    return 0.5 * RIM_AUTHORED_WIDTH


def exhaust_markers(markers, parts):
    """(R, t) for every exhaust marker on `parts`, in walk order: the
    marker's rotation rows turned by R, a quarter turn about y, and its
    translation.

    R is FUN_005bada0(m, m, 0x4000), rows (c,0,-s) (0,1,0) (s,0,c) with c = 0
    and s = 1, applied as R * m: row 0 becomes -m row 2 and row 2 becomes
    m row 0. The markers are authored pitched a quarter turn (x up, z out of
    the back), and this turns them back, so the tip comes out unrotated.
    """
    out = []
    for part in parts:
        for h, m in markers.get(part["hash"], []):
            if h in EXHAUST_MARKERS and len(out) < MAX_REAR_MARKERS:
                rot = tuple(-c for c in m[8:11]) + tuple(m[4:7]) + tuple(m[0:3])
                out.append((rot, tuple(m[12:15])))
    return out


def rotate_rows(r, x, y, z):
    """(x, y, z) times the rows r[0:3], r[3:6], r[6:9]: the row-vector v * M
    every car-space matrix here uses."""
    return (x * r[0] + y * r[3] + z * r[6],
            x * r[1] + y * r[4] + z * r[7],
            x * r[2] + y * r[5] + z * r[8])


def load_wheel_table(pc_root, car, verbose=False):
    """The four wheel corner records for `car`, or None if they can't be read."""
    path = os.path.join(pc_root, CAR_TYPE_INFO_BUN)
    if not os.path.isfile(path):
        return None
    data = open(path, "rb").read()
    base = data.find(("CARS\\%s\\GEOMETRY.BIN" % car).encode("ascii"))
    if base < 0:
        return None
    corners = []
    for i in range(4):
        o = base + WHEEL_TABLE_REL + i * WHEEL_RECORD_STRIDE
        pos = struct.unpack_from("<3f", data, o)
        radius, width = struct.unpack_from("<2f", data, o + 0x10)
        fender, wide = struct.unpack_from("<2f", data, o + 0x1c)
        corners.append({"pos": pos, "radius": radius, "width": width,
                        "fender": fender, "wide": wide})
    # Catches a shifted anchor and the part-library folders (CARS/WHEELS and
    # friends) that have a GEOMETRY.BIN but no CarTypeInfo record; better to
    # ship a car with no wheels than one with wheels scattered off the body.
    if not all(c["radius"] > 0.01 and c["width"] > 0.01 for c in corners):
        return None
    if verbose:
        for i, c in enumerate(corners):
            print("  wheel %d (%7.3f,%7.3f,%7.3f) r=%.3f w=%.3f"
                  % (i, c["pos"][0], c["pos"][1], c["pos"][2],
                      c["radius"], c["width"]))
    return corners


def part_placements(suffix, part, wheels, depth, exhausts=()):
    """(tx, ty, tz, flip) for every instance of a part that should be emitted.
    flip is None, FLIP_MIRROR (y negated), FLIP_TURN (x and y negated: half
    a turn about z) or a marker's rotation rows (rotate_rows), applied to the
    vertices before the translation.

    Ordinary parts are placed once, by their 0x134011 translation alone. Every
    non-identity 3x3 in the retail car files is a mirror/scale the exporter
    already baked into the vertex positions (the "right" side parts ship with
    negative-Y vertices), whereas the translation is genuinely additive -- the
    spoiler's is what lifts it onto the trunk, and it matches the BASE part's
    0x13401a spoiler attachment point exactly.

    Wheels and brakes are the exception: one mesh authored at the origin stands
    in for two or four corners, placed from the CarTypeInfo wheel table the way
    Render__13CarRenderInfo places a stock wheel (TASKS.md #204 slice 3, and
    carkit.c's place_wheels() for the same rule on an assembled car). The
    wheel's outer face, its authored y = 0, sits on the corner's fender line
    and the mesh runs inboard; a stock wheel is never scaled. The left corners
    take it half a turn about z, which is a rotation and keeps a directional
    spoke pattern's handedness. The brakes sit `depth` inboard of the
    same line -- the wheel's MARKER_BRAKE depth, or half the authored 0.184
    width without one -- and are mirrored, not turned, on the left.

    The exhaust tip goes on every one of `exhausts` (exhaust_markers): its
    own translation, turned with it, then the marker's.
    """
    m = part["matrix"]
    if suffix == STOCK_EXHAUST_PART:
        out = []
        for rot, t in exhausts:
            ox, oy, oz = rotate_rows(rot, m[12], m[13], m[14])
            out.append((t[0] + ox, t[1] + oy, t[2] + oz, rot))
        return out
    corners = WHEEL_CORNER_PARTS.get(suffix)
    if corners is None:
        return [(m[12], m[13], m[14], None)]
    if not wheels:
        return []

    # Which side it is authored for is settled by the normals, not by the sign
    # of its vertices, and the two disagree here: a wheel's y runs 0 -> +0.18
    # (FOCUS) but every normal on it points -Y (mean -0.68 on the FOCUS, -0.65
    # on the COROLLA, -0.59 on the 240SX; the front brake matches at -0.49 /
    # -0.44 / -0.52). Only the wheel's outboard half is modelled -- it is an
    # open half-shell, which is why no normal on it is positive -- so the
    # modelled face is the -Y one and the mesh runs inboard from there. -Y is
    # the car's right: DOOR_RIGHT / RIGHT_SIDE_MIRROR ship at negative y with
    # negative normals on all three cars, against the +y / +normal left pair.
    #
    # So the authored corner is the *right* one, and it is the left corners
    # (cy > 0) that need flipping. This was inverted, which turned all four
    # wheels and all four brakes inside out: what you saw was the open back of
    # the half-shell -- the inside of the tyre and the reverse of the spoke
    # disc -- reading as a dark, detail-free blob on every car (#126).
    #
    # Until #204 slice 3 the mesh was centred on the corner's centre plane y
    # and mirrored. Retail seats it on the fender line instead, ~3 cm further
    # out on the TIBURON, and turns the left wheels rather than mirroring them.
    out = []
    for i in corners:
        cx, cy, cz = wheels[i]["pos"]
        line = wheels[i]["fender"]
        left = cy > 0.0
        if suffix == STOCK_WHEEL_PART or suffix in TRAFFIC_WHEEL_PARTS:
            ty, flip = line, (FLIP_TURN if left else None)
        else:
            ty, flip = (line - depth if left else line + depth), \
                (FLIP_MIRROR if left else None)
        out.append((cx + m[12], ty + m[13], cz + m[14], flip))
    return out


def build(car_dir, exe_path, maxdim, verbose):
    data = open(os.path.join(car_dir, "GEOMETRY.BIN"), "rb").read()
    parts = parse_geometry(data)
    car = os.path.basename(os.path.normpath(car_dir)).upper()

    # Parts are selected by the record's name *hash*, not by its name. The name
    # field is cut at PART_NAME_MAX, and on a car whose name is long enough the
    # cut lands before the part of the name that distinguishes it: all of
    # IMPREZAWRX_KIT00_HEADLIGHT_LEFT_A, _LEFT_B, _LEFT_C, _RIGHT_A, _RIGHT_B
    # and _RIGHT_C are stored as "IMPREZAWRX_KIT00_HEADLIGHT_". Matching that
    # cut name first-wins handed the LEFT part to the RIGHT lookup as well, so
    # the car was packed with its left headlight twice and no right one -- a
    # black hole where the right headlight should be, since the slot its
    # subsets name (HEADLIGHT_RIGHT, 0xa532fc46) was then never requested and
    # never resolved. The same alias hit the right door panel and door sill.
    # Reported on the Lancer Evolution and the Impreza WRX (TASKS.md #273).
    #
    # The hash at PART_RECORD_HASH_OFF is over the *untruncated* name, so it is
    # exact and immune to the cut -- the same reason textures_by_name() hashes
    # rather than compares. Checked over every car in PC Files/CARS: 1593
    # suffix lookups, the hash finds a part everywhere the name did (no
    # regression) and differs on exactly 9, each one a mirrored RIGHT part the
    # cut was aliasing to its LEFT twin -- IMPREZAWRX and LANCEREVO8 (headlight,
    # door panel, door sill), MUSTANGGT, NAVIGATOR and PEUGOT106 (door panel).
    #
    # Picking "_A" out of a LOD run needs no ordering rule any more either: the
    # suffix names the LOD outright, so MUSTANGGT's three front wheels, all
    # stored as "MUSTANGGT_KIT00_FRONT_WHEEL", are told apart by hash too.
    by_hash = {}
    for p in parts:
        by_hash.setdefault(p["hash"], p)

    traffic = (b_string_hash(car + "_BODY_A") in by_hash
               and b_string_hash(car + "_KIT00_BODY_A") not in by_hash)
    lods = dict(TRAFFIC_PARTS) if traffic else {}
    selected = []
    for suffix in ([s for s, _ in TRAFFIC_PARTS] if traffic else STOCK_SUFFIXES):
        part = by_hash.get(b_string_hash(car + suffix))
        if part is not None:
            selected.append((suffix, part))
        elif verbose:
            print("  (no %s%s)" % (car, suffix))
    if not selected:
        raise SystemExit("no stock parts found for %s" % car)

    textures = load_textures(os.path.join(car_dir, "TEXTURES.BIN"), exe_path)
    if traffic:
        textures = load_textures(os.path.join(os.path.dirname(os.path.normpath(car_dir)),
                                              "TEXTURES.BIN"), exe_path)
    pc_root = os.path.dirname(os.path.dirname(os.path.normpath(car_dir)))
    globals_ = load_global_car_textures(pc_root, verbose)
    wheels = load_wheel_table(pc_root, car, verbose)
    if wheels is None and verbose:
        print("  no wheel table for %s -- wheels and brakes skipped" % car)
    wanted = set()
    for _, part in selected:
        wanted.update(part["textures"])
    resolved = resolve_slots(car, wanted, textures, globals_, verbose)

    verts, indices, batches = [], [], []
    tex_order, tex_index = [], {}
    bbmin = [1e30] * 3
    bbmax = [-1e30] * 3
    unresolved = {}

    markers = parse_markers(data)
    wheel_part = [p for s, p in selected if s == STOCK_WHEEL_PART]
    depth = brake_depth(markers, wheel_part[0]["hash"]) if wheel_part \
        else 0.5 * RIM_AUTHORED_WIDTH
    exhausts = exhaust_markers(markers, [p for _, p in selected])
    batch_mats = []
    wheel_pivots = {}

    for suffix, part in selected:
        pv, pi, psubs = read_mesh(data, part)
        pmats = subset_materials(data, part) if traffic else [0] * len(psubs)
        places = part_placements(suffix, part, wheels, depth, exhausts)

        for pi_, (tx, ty, tz, flip) in enumerate(places):
            # The places run in WHEEL_CORNER_PARTS' corner order; a stock
            # tyre's wheel matrix is where it is placed.
            wheel = 0
            if traffic and suffix in TRAFFIC_WHEEL_PARTS:
                corner = WHEEL_CORNER_PARTS[suffix][pi_]
                wheel = WHEEL_RIM << 2 | corner
                wheel_pivots[corner] = (tx, ty, tz)
            base = len(verts)
            for (x, y, z, nx, ny, nz, u, v) in pv:
                # The left corners reuse the same mesh, turned or mirrored
                # across the car's centreline. The normal goes with it; a
                # mirror's winding needs no fixing because the runtime draws
                # with GPU_CULL_NONE.
                if flip == FLIP_MIRROR:
                    y, ny = -y, -ny
                elif flip == FLIP_TURN:
                    x, y, nx, ny = -x, -y, -nx, -ny
                elif flip is not None:
                    x, y, z = rotate_rows(flip, x, y, z)
                    nx, ny, nz = rotate_rows(flip, nx, ny, nz)
                x += tx
                y += ty
                z += tz
                bbmin = [min(bbmin[0], x), min(bbmin[1], y), min(bbmin[2], z)]
                bbmax = [max(bbmax[0], x), max(bbmax[1], y), max(bbmax[2], z)]
                # V is negated for the PICA200, whose texture V axis runs opposite
                # to D3D's. Under GPU_REPEAT, -v is identical to 1-v.
                verts.append((x, y, z, nx, ny, nz, u, -v))

            ibase = len(indices)
            indices.extend(i + base for i in pi)

            for (first_index, index_count, tex_slot, flags), mat in zip(psubs, pmats):
                batch_mats.append(mat)
                th = part["textures"][tex_slot] if tex_slot < len(part["textures"]) else 0
                ti = -1
                if th in resolved:
                    # One copy per texture, not per slot: all six windows
                    # draw the one WINDOW.
                    key = resolved.src[th]
                    if key not in tex_index:
                        tex_index[key] = len(tex_order)
                        tex_order.append(th)
                    ti = tex_index[key]
                elif th:
                    unresolved[th] = unresolved.get(th, 0) + 1
                if flags & BATCH_WHEEL_MASK:
                    raise SystemExit("%s: a sub-mesh uses flag bits 0x%08x"
                                     % (part["name"], flags & BATCH_WHEEL_MASK))
                batches.append((ibase + first_index, index_count, ti,
                                (flags & ~BATCH_LOD_MASK) | lods.get(suffix, 0) << BATCH_LOD_SHIFT |
                                wheel << BATCH_CORNER_SHIFT))

        if verbose:
            print("  %-30s v=%-5d tri=%-5d subsets=%d%s"
                  % (part["name"], len(pv), len(pi) // 3, len(psubs),
                     "" if len(places) == 1 else "  x%d" % len(places)))

    if len(verts) > 65535:
        raise SystemExit("%d vertices exceeds the u16 index range" % len(verts))

    # A traffic car's other skins: no batch names them, the runtime finds each
    # by its hash and puts it in SKIN1's slot.
    for n in range(2, TRAFFIC_SKINS + 1) if traffic else ():
        h = b_string_hash("%s_SKIN%d" % (car, n))
        if h in textures and h not in tex_index:
            resolved[h] = textures[h]
            resolved.mat[h] = textures.mat.get(h, 0)
            resolved.src[h] = h
            tex_index[h] = len(tex_order)
            tex_order.append(h)

    tex_entries, tex_blob = [], bytearray()
    fmt_name = {FMT_RGB565: "RGB565", FMT_RGBA4: "RGBA4444", FMT_SKIN: "SKIN"}
    for th in tex_order:
        name, w, h, rgba = resolved[th]
        if rgba is None:
            # The skin: a size and a mip count, no pixels.
            while w > maxdim or h > maxdim:
                w, h = w // 2, h // 2
            fmt, pixels, maxlvl = FMT_SKIN, b"", mip_max_level(w, h)
            tex_entries.append((resolved.src[th], w, h, tex_fmt_field(fmt, maxlvl),
                                len(tex_blob), resolved.mat.get(th, 0)))
            if verbose:
                print("  tex 0x%08x %-28s %dx%d SKIN +%d mip"
                      % (resolved.src[th], name, w, h, maxlvl))
            continue
        rgba, w, h = box_downscale(rgba, w, h, maxdim)
        fmt, pixels, maxlvl = encode_texture_chain(rgba, w, h)
        tex_entries.append((resolved.src[th], w, h, tex_fmt_field(fmt, maxlvl),
                            len(tex_blob), resolved.mat.get(th, 0)))
        tex_blob.extend(pixels)
        if verbose:
            print("  tex 0x%08x %-28s %dx%d %s +%d mip %dB"
                  % (resolved.src[th], name, w, h, fmt_name.get(fmt, "?"), maxlvl,
                     len(pixels)))

    if unresolved and verbose:
        print("  unresolved texture hashes (not in this car's TEXTURES.BIN -> paint):")
        for th, n in sorted(unresolved.items(), key=lambda kv: -kv[1]):
            print("    0x%08x x%d" % (th, n))

    extra = None
    if traffic:
        body = b_string_hash(car + "_BODY_A")
        extra = traffic_extra(batch_mats, markers.get(body, []), by_hash[body]["matrix"][12:15],
                              wheel_pivots)
        if verbose:
            print("  traffic extras: %d batch materials, %d light markers"
                  % (len(batch_mats), struct.unpack_from("<I", extra, 12)[0]))
    blob = pack_nfsc(tex_entries, tex_blob, batches, verts, indices,
                     bbmin, bbmax, SHADING_LIT, extra=extra)
    return blob, {
        "parts": len(selected), "verts": len(verts), "indices": len(indices),
        "batches": len(batches), "textures": len(tex_entries),
        "bbmin": bbmin, "bbmax": bbmax, "texbytes": len(tex_blob),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("car_dir", help="PC Files/CARS/<CAR>")
    ap.add_argument("out", help="output .nfsc path")
    ap.add_argument("--exe", default=None, help="path to SPEED2.EXE (for HUFF textures)")
    # 512 is the retail source resolution for a car's skin, so this default
    # means "do not downscale the player's car at all". Measured: TIBURON is
    # 0.60 MB at 128, 1.00 MB at 256 and 2.50 MB at 512. The car is a single
    # always-resident object held *outside* the world pager's budget (which is
    # linearGot/3, ~32 MB), in the other two thirds alongside the frontend
    # packs and the video decoder, so 2.5 MB there is not a meaningful cost --
    # and unlike world art the car is permanently a few metres from the camera,
    # which is exactly where the extra texels are visible.
    #
    # This is not a free lever for *every* car at once: a race grid of eight
    # would be ~20 MB. If opponent/traffic cars ever load real models, they
    # want a lower cap of their own rather than a lower cap here.
    ap.add_argument("--max-texture", type=int, default=512)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    exe = args.exe
    if exe is None:
        d = os.path.abspath(args.car_dir)
        for _ in range(6):
            c = os.path.join(d, "SPEED2.EXE")
            if os.path.isfile(c):
                exe = c
                break
            parent = os.path.dirname(d)
            if parent == d:
                break
            d = parent

    blob, stats = build(args.car_dir, exe, args.max_texture, args.verbose)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(blob)
    print("%s: %d bytes  %d parts, %d verts, %d indices, %d batches, %d textures (%d B)"
          % (args.out, len(blob), stats["parts"], stats["verts"], stats["indices"],
             stats["batches"], stats["textures"], stats["texbytes"]))
    print("  bbox min=%s max=%s"
          % ([round(c, 3) for c in stats["bbmin"]], [round(c, 3) for c in stats["bbmax"]]))


if __name__ == "__main__":
    main()
