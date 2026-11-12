#!/usr/bin/env python3
"""
Car-part kits: the save's own parts, put together on the 3DS the way the
retail renderer puts them together (TASKS.md #204).

car_pack.py bakes one fixed set of parts per car -- every car's _KIT00_ body
kit -- into a single .nfsc. That is the showroom car, not the player's: the
career TIBURON in the reference save wears a KIT02 front bumper, a STYLE26
hood, a carbon STYLE37 spoiler out of the shared SPOILER library, a roof scoop
and aftermarket mirrors, and a fixed bake can show none of them. This tool
packs the pieces instead, and 3DS Files/source/carkit.c assembles the save's
picks into one model at career load.

Three kinds of file come out:

  GLOBAL/PARTMODELS.nfsm   part hash -> the LOD-A model(s) it draws and their
                           LOD B/C/D models, which kit each one lives in, its
                           upgrade level, and the light texture base the
                           headlight/brakelight parts carry.
                           Resolved offline out of GLOBALB.BUN's parts
                           database the way CarPartModelTable::GetModelNameHash
                           does it (PartsDb.model_hash), so the runtime never
                           needs the string pool or the name templates. Also
                           the two attributes UpdateDecalTextures reads.

  CARS/<KIT>.nfsk          the parts of one GEOMETRY*.BIN -- a car's own
                           folder or a shared library (SPOILER, ROOF,
                           MIRRORS_BODY, ...) -- as ready-to-copy vertex and
                           index runs, sub-meshes still keyed by their retail
                           *logical* texture slot, the textures those slots
                           resolve to, and every part's position markers.

  CARS/DECALS.nfsk         a kit of textures only: every decal's art, out of
                           CARS/TEXTURES.BIN (TASKS.md #296).

The slot stays logical because what fills it depends on the rest of the car:
the light slots take the texture of whichever headlight part is fitted, and
every body slot -- a library spoiler's or mirror's too -- takes the car's
composited skin. carkit.c makes those choices; this only resolves the
defaults.

Layouts (little-endian; carkit.c mirrors both):

PARTMODELS.nfsm
    0x00 char magic[4] "NFSM"      0x04 u32 version = 3
    0x08 u32 numEntries            0x0c u32 numKits
    0x10 u32 entriesOffset         0x14 u32 kitsOffset
    0x18 u32 numDecalAttrs         0x1c u32 decalAttrsOffset
    Entry[numEntries], sorted by partHash, 44 bytes:
        u32 partHash; u32 model[2]; u32 lightBase;
        u8 kit[2] (index into the kit names, 0xff = no model); u8 level; u8 partSlot
        u32 lod[2][3]: model[i]'s LOD B, C and D models, 0 = none
    char kitName[numKits][32]
    DecalAttr[numDecalAttrs], sorted by partHash: { u32 partHash; u32 value; }
        value: a decal layout part's SHAPE (only when it also has a SIZE, which
        UpdateDecalTextures requires), a decal part's NAME, a paint or tint
        part's LIGHT_MATERIAL, or a wheel part's INNER_RADIUS (the rim's size
        in inches, which RenderTireMask sizes the frontend's disc by). No part
        carries two of them.
    lod[i][l] is GetModelNameHash(part, i, l + 1), kept only when that model
    is in the same geometry file as model[i] (every shipped one is), so it is
    a part of kit[i]'s .nfsk too. CarRenderInfo::Render draws a part at the
    level it picks for the car out of that same row, and not at all at a
    level the row has no model for (TASKS.md #296). The links are the
    part's, not the model's: 700-odd A models are shared by two parts of
    which only one carries LOD models.
    Version 2 is the same file with a 20-byte Entry and no LOD models, which
    carkit.c draws at LOD A whatever the distance; version 1 is version 2
    without the decal attributes (0x18 and 0x1c zero), and carkit.c draws no
    decals with it.

<KIT>.nfsk
    0x00 char magic[4] "NFSK"      0x04 u32 version = 4
    0x08 u32 numTextures           0x0c u32 numParts
    0x10 u32 numBatches            0x14 u32 numVerts
    0x18 u32 numIndices            0x1c u32 numMarkers
    0x20 u32 numSlots              0x24 u32 texBlobOffset
    0x28 u32 texBlobSize           0x2c u32 flags (bit 0: wheel table present)
    0x30 Wheel[4] { f32 pos[3]; f32 radius; f32 width; }    (car kits only)
    0x80 WheelLine[4] { f32 fender; f32 wide; }               (car kits only)
    0xa0 TexEntry[numTextures] { u32 hash; u16 w, h; u16 fmt; u16 mat; u32 dataOffset; }
         SlotMap[numSlots]     { u32 slotHash; u32 texHash; }       sorted by slotHash
         Part[numParts]        { u32 hash; u32 firstBatch, numBatches; u32 firstVert,
                                 numVerts; u32 firstIndex, numIndices; f32 yCentre;
                                 f32 pivot[3]; }                    sorted by hash
         Batch[numBatches]     { u32 firstIndex; u32 indexCount; u32 slotHash; u32 flags;
                                 u32 lightMaterial; }
         Marker[numMarkers]    { u32 partHash; u32 markerHash; f32 m[16]; }
         Vertex[numVerts]      { f32 pos[3]; f32 nrm[3]; f32 uv[2]; }   (v already negated)
         u16 indices[numIndices]                                       (4-byte padded)
         <texture blob>         the same PICA200 encodings and mip chains a .nfsc uses
    A TexEntry's mat is the texture's retail render state, packed as a .nfsc's
    is (car_pack.texture_material), so carkit.c copies the entry through whole.
    A Batch's flags are the retail sub-mesh flags plus, in bits 28-29, which
    of Render's repaintable light materials the sub-mesh is authored with
    (MAT_CLASS_*). Its lightMaterial is the name hash of that light material
    as the model has it (car_pack.subset_materials): one of
    GLOBAL/LIGHTMATS.nfsl's (tools/lightmat_pack.py), or a placeholder Render
    swaps for the car's own -- 0xd6d6080a for the paint, 0xa7366ae6 for
    carbon fibre -- or a name the table lacks, which elGetLightMaterial
    answers with DEFAULT (TASKS.md #166). 0 for a subset with none.
    A Part's pivot is its record matrix's translation, eSolid +0x70, which
    eModel::GetPivotPosition (Speed.elf 0x800c5a60) answers. CarRenderInfo
    takes the BODY's (+0x30) off every light flare (CreateCarLightFlares,
    0x80235e50 / SPEED2.EXE 0x6204d0). Of the shipped BODY_A parts only
    COROLLA_KITW02's is off zero, by 1 cm on each axis.
    Version 3 is the same file with a 32-byte Part and no pivot, which
    carkit.c reads as a zero pivot.
    Version 2 is version 3 with a 16-byte Batch and no lightMaterial,
    which carkit.c draws in the DEFAULT material. Version 1 is version 2
    without WheelLine (a 0x80 header) and with bits 28-29 clear; carkit.c
    still reads it.

A batch's firstIndex is relative to its part's firstIndex and a part's indices
are relative to its own firstVert, so the runtime can drop a part anywhere in
the assembled buffers with one add. Vertices are stored exactly as authored:
no part matrix is applied (see pack_kit for why that is right).
"""
import argparse
import fnmatch
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import car_pack as cp  # noqa: E402
import paint_pack as pp  # noqa: E402
from fng_parse import b_string_hash  # noqa: E402

NFSM_VERSION = 3
NFSK_VERSION = 4
NFSK_HDR = 0xA0
NO_KIT = 0xFF
KIT_NAME_LEN = 32
KIT_FLAG_WHEELS = 1

# GLOBALB.BUN parts-database chunks beyond the ones paint_pack.py reads.
CH_MODEL_TABLE = 0x0003460A    # CarPartModelTable, 36-byte records
CH_CAR_TYPES = 0x0003460B      # u32 name hashes: the selector-1 base names
MODEL_TABLE_STRIDE = 36
NO_HASH = 0xFFFFFFFF
NO_MODEL = 0xFFFF

# Part attributes (keys are bStringHash of the attribute name).
ATTR_BASE_NAME = 0xebb03e66    # selector >= 2: the part's own base-name hash
ATTR_LIGHT_BASE = 0x10c98090   # headlight/brakelight parts: their texture base
# What UpdateDecalTextures (SPEED2.EXE 0x61FF40) reads: a decal layout part's
# SIZE and SHAPE, and the NAME of each decal fitted to it.
ATTR_SIZE = b_string_hash("SIZE")
ATTR_SHAPE = b_string_hash("SHAPE")
ATTR_NAME = b_string_hash("NAME")
# A paint or window-tint part's light material (TASKS.md #166): the name hash
# CarRenderInfo's constructor hands elGetLightMaterial for the body paint
# (GC slot 0x3f), the window tint (0x99) and the eight component paints.
ATTR_LIGHT_MATERIAL = 0x6ba02c05
# A wheel part's rim size in inches (KEY_INNER_RADIUS): RenderTireMask
# (SPEED2.EXE 0x620600) sizes the frontend's black disc behind each rim by the
# front wheel's. Every wheel part carries it but the six traffic tyres.
ATTR_INNER_RADIUS = 0xeb0101e2
# A decal's art is NAME carried on with its layout's shape. No SQUARE layout
# or art ships; the suffix is kept because retail checks for it.
DECAL_SUFFIXES = ("_SQUARE", "_RECT", "_WIDE")
DECAL_KIT = "DECALS"
# The cracked panes (TASKS.md #324). UpdateCarReplacementTextures (Speed.elf
# 0x802353FC, SPEED2.EXE 0x615280) points a window slot at
# DamagedWindowHashes[level] (0x8037127C): WINDOW, then these three, all in
# GLOBALB's Global\GlobalTextures.tpk. Only a car whose glass can break carries
# them (carkit.c's carkit_glass), so they ship once, in their own kit.
GLASS_KIT = "GLASS"
GLASS_TEXTURES = ("WINDOW_DAMAGE0", "WINDOW_DAMAGE1", "WINDOW_DAMAGE2")

# Batch flag bits the runtime borrows: 24-27 carry the paint slot carkit.c
# assigns, 28-29 the material class below, 31 is model3d.c's BATCH_NO_COLLIDE.
# Retail car sub-meshes must not use any of them (every one shipped ORs to
# 0x0001dd80; bit 16 is set but carkit.c masks the LOD bits), and pack_kit
# checks that they do not.
RESERVED_FLAG_BITS = 0xBF000000
MAT_CLASS_SHIFT = 28

# The light materials Render__13CarRenderInfo repoints at a component paint
# (FUN_0048d860(key, material)), named by the bStringHash each one is:
#   MAT_CLASS_RIM    MAGSILVER, MAGCHROME, MAGGUNMETAL -> the rim paint, on
#                    model 0 of the front and rear wheels
#   MAT_CLASS_BRAKE  CALIPER, CALLIPER -> the brake paint, on the brakes
# carkit.c decides per instance whether the swap happens; this only records
# which sub-meshes it would touch.
MAT_CLASS_RIM = 1
MAT_CLASS_BRAKE = 2
MAT_CLASS = {
    0x22719fa9: MAT_CLASS_RIM,     # MAGSILVER
    0xfd102a92: MAT_CLASS_RIM,     # MAGCHROME
    0x12049251: MAT_CLASS_RIM,     # MAGGUNMETAL
    0xd6640dff: MAT_CLASS_BRAKE,   # CALIPER
    0xa3186e2b: MAT_CLASS_BRAKE,   # CALLIPER
}

# Every body slot SwitchSkin (SPEED2.EXE 0x636840) points at the composited
# skin, and its size: car_pack.py's, whose stock bake takes the same
# placeholder.
SKIN_SLOTS = cp.SKIN_SLOTS
SKIN_SIZE = cp.SKIN_SIZE

# Shared libraries: every one a player car's parts draw from, so a part change
# in the save needs no repack -- the spoilers and mirrors (the SUVs and the
# HUMMER have their own), the roof scoops, exhaust tips, plates, every rim
# brand and the aftermarket brakes. A name with a * is matched against the kit
# names geometry_files() finds. AUDIO is the trunk install's components (the
# amps, LCDs, NOS bottles, crossovers, speakers and the EMPTY placeholders):
# Render draws them on the frontend car and the player's car, open trunk or
# not (TASKS.md #314; the world car takes the EMPTY ones).
DEFAULT_LIBS = ("SPOILER", "SPOILER_HATCH", "SPOILER_SUV", "ROOF", "MIRRORS_BODY",
                "MIRRORS_POST", "MIRRORS_SUV", "MIRRORS_HUMMER", "EXHAUST", "PLATES",
                "BRAKES", "WHEELS_*", "AUDIO")
# AUDIO_COMP_EMPTY_{SMALL,MEDIUM,LARGE}_A and _PAINT_A: the world path asks
# for these by name whatever is installed, so AUDIO.nfsk must hold all six.
AUDIO_KIT = "AUDIO"
AUDIO_EMPTY = tuple("AUDIO_COMP_EMPTY_%s%s_A" % (s, p)
                    for s in ("SMALL", "MEDIUM", "LARGE") for p in ("", "_PAINT"))
# The cars: all 31 a player can own, the same set car_pack.py makes a stock
# CARS/<CAR>.nfsc of. A career car can be any of them, and the frontend's
# DDAY_PLAYER_CAR preset is a 350Z (TASKS.md #206, #207).
DEFAULT_CARS = ("240SX", "3000GT", "350Z", "A3", "CELICA", "CIVIC", "COROLLA", "CORSA",
                "ECLIPSE", "ESCALADE", "FOCUS", "G35", "GOLF", "GTO", "HUMMER",
                "IMPREZAWRX", "IS300", "LANCEREVO8", "MIATA", "MUSTANGGT", "NAVIGATOR",
                "PEUGOT", "PEUGOT106", "RSX", "RX7", "RX8", "SENTRA", "SKYLINE", "SUPRA",
                "TIBURON", "TT")


def ext(h, s):
    """bStringHash carried on from `h`: what the model-name templates chain.
    ext(0xFFFFFFFF, s) is plain bStringHash(s)."""
    for c in s.upper().encode("latin1"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


# ------------------------------------------------------------- parts database

class PartsDb:
    """GLOBALB.BUN's car-parts database, as far as model names go.

    A part record (chunk 0x34604, 14 bytes) carries, past its hash:
      +4  u16  low byte: the part's slot; byte +5 >> 5: upgrade level, 0 = stock
      +6  u16  low byte: base selector; high byte: car-type index (selector 1)
      +c  u16  model-table index, 0xffff = the part draws nothing
    and the model table (0x3460a, 36 bytes) carries a templated flag at +0, a
    string-pool index /4 at +2, and eight u32 at +4: [model * 4 + lod].
    """

    def __init__(self, data):
        ch = pp.find_parts_db(data)
        if not ch:
            raise SystemExit("no 0x%08x parts container in the bundle" % pp.PARTS_CONTAINER)
        for need in (pp.CH_STRINGS, pp.CH_PARTS, CH_MODEL_TABLE, CH_CAR_TYPES):
            if need not in ch:
                raise SystemExit("parts container has no 0x%08x chunk" % need)

        poff, psz = ch[pp.CH_STRINGS]
        self.pool = data[poff:poff + psz]

        moff, msz = ch[CH_MODEL_TABLE]
        if msz % MODEL_TABLE_STRIDE:
            raise SystemExit("model table is %d B, not a whole number of %d-byte records"
                             % (msz, MODEL_TABLE_STRIDE))
        self.models = []
        for i in range(msz // MODEL_TABLE_STRIDE):
            o = moff + i * MODEL_TABLE_STRIDE
            templated, _b1, s2 = struct.unpack_from("<BBH", data, o)
            vals = struct.unpack_from("<8I", data, o + 4)
            self.models.append((templated, s2, vals))

        coff, csz = ch[CH_CAR_TYPES]
        self.car_types = struct.unpack_from("<%dI" % (csz // 4), data, coff)

        roff, rsz = ch[pp.CH_PARTS]
        self.records = {}
        for i in range(rsz // pp.PART_RECORD_SIZE):
            h, w4, w6, _w8, _wa, mc = struct.unpack_from("<IHHHHH", data,
                                                         roff + i * pp.PART_RECORD_SIZE)
            prev = self.records.get(h)
            if prev is None:
                self.records[h] = (w4, w6, mc)
            elif mc != NO_MODEL or prev[2] != NO_MODEL:
                # Seven hashes repeat in the retail table (RED, GREEN, ..., STOCK),
                # all colour parts with no model. A repeat that draws something
                # would make "which record wins" matter, so refuse to guess.
                raise SystemExit("part 0x%08x appears twice and draws a model" % h)

        self.attrs, self.names = {}, {}
        for h, name, attrs in pp.read_parts(data):
            self.attrs.setdefault(h, attrs)
            self.names.setdefault(h, name)

    def pstr(self, index4):
        o = index4 * 4
        return self.pool[o:self.pool.index(b"\0", o)].decode("latin1")

    def level(self, ph):
        return (self.records[ph][0] >> 8) >> 5

    def part_slot(self, ph):
        return self.records[ph][0] & 0xFF

    def model_hash(self, ph, model, lod=0):
        """GetModelNameHash(part, model, lod): the bStringHash of the model's
        full part name, or 0 when the part has no such model."""
        w4, w6, mc = self.records[ph]
        if mc == NO_MODEL:
            return 0
        sel = w6 & 0xFF
        if sel == 0:
            base = NO_HASH
        elif sel == 1:
            base = self.car_types[w6 >> 8]
        else:
            base = self.attrs.get(ph, {}).get(ATTR_BASE_NAME, 0)
        templated, s2, vals = self.models[mc]
        v = vals[model * 4 + lod]
        if v == NO_HASH:
            return 0
        if not templated:
            return v
        if s2 != NO_MODEL:
            base = ext(base, self.pstr(s2))
        return ext(ext(base, self.pstr(v)), "_" + chr(ord("A") + lod))


# ---------------------------------------------------------------- geometry

def geometry_files(cars_root):
    """kit name -> GEOMETRY*.BIN path for every geometry file under CARS/.
    A folder's GEOMETRY.BIN is named after the folder; WHEELS/GEOMETRY_BBS.BIN
    becomes WHEELS_BBS."""
    out = {}
    for d in sorted(os.listdir(cars_root)):
        full = os.path.join(cars_root, d)
        if not os.path.isdir(full):
            continue
        for f in sorted(glob.glob(os.path.join(full, "GEOMETRY*.BIN"))):
            if os.path.getsize(f) < 1024:
                continue
            stem = os.path.splitext(os.path.basename(f))[0].upper()
            name = d.upper() + stem[len("GEOMETRY"):]
            if len(name) >= KIT_NAME_LEN:
                raise SystemExit("kit name %s does not fit %d bytes" % (name, KIT_NAME_LEN))
            out[name] = f
    return out


def model_index(files, verbose=False):
    """model (part) hash -> kit name, over every geometry file."""
    index = {}
    for kit, path in sorted(files.items()):
        try:
            parts = cp.parse_geometry(open(path, "rb").read())
        except SystemExit as e:
            if verbose:
                print("  (skipping %s: %s)" % (kit, e))
            continue
        for p in parts:
            other = index.setdefault(p["hash"], kit)
            if other != kit and verbose:
                print("  model 0x%08x %s is in both %s and %s; using %s"
                      % (p["hash"], p["name"], other, kit, other))
    return index


# ---------------------------------------------------------------- textures

def texture_sources(pc_root, kit, path, exe, globals_):
    """The texture tables a kit's slots resolve against, most specific first:
    its own folder's TEXTURES.BIN, the rim pack for a wheel kit, the shared
    CARS/TEXTURES.BIN, then the global car textures."""
    cars = os.path.join(pc_root, "CARS")
    srcs = [cp.load_textures(os.path.join(os.path.dirname(path), "TEXTURES.BIN"), exe)]
    if kit.startswith("WHEELS_"):
        srcs.append(cp.load_textures(os.path.join(cars, "WHEELS", "TEXTURES.BIN"), exe))
    srcs.append(cp.load_textures(os.path.join(cars, "TEXTURES.BIN"), exe))
    srcs.append(globals_)
    return [s for s in srcs if s]


def find_tex(sources, th):
    for s in sources:
        if th in s:
            return s[th]
    return None


def find_mat(sources, th):
    """The render state of the texture find_tex() picks for `th`."""
    for s in sources:
        if th in s:
            return s.mat.get(th, 0)
    return 0


def resolve_default(slot, sources, car):
    """A logical slot's default texture: (texHash, (name, w, h, rgba), skin)
    or None for a slot the runtime shades as flat paint.

    The same order car_pack.resolve_slots() follows for a stock car -- the
    global pane a window slot is swapped to, a texture the kit ships under the
    slot's own hash, the global pack, the per-car light pattern, then the
    skin -- except that only a car's own kit gets the two car-specific
    rules. A shared library part has no car to take a light pattern or a
    skin from: carkit.c points its skin slots at the car's at assembly."""
    to = cp.SLOT_REPLACEMENT.get(slot)
    if to is not None:
        hit = find_tex(sources, to)
        return (to, hit, False) if hit is not None else None
    hit = find_tex(sources, slot)
    if hit is not None:
        return slot, hit, False
    name = cp.CAR_SLOT_NAMES.get(slot)
    if car:
        pattern = cp.SLOT_TO_CAR_TEXTURE.get(name)
        if pattern:
            th = b_string_hash(pattern % car)
            hit = find_tex(sources, th)
            if hit is not None:
                return th, hit, False
    if car and (slot in SKIN_SLOTS or slot == b_string_hash(car + "_SKIN1")):
        return b_string_hash(car + "_SKIN1"), None, True
    return None


# ---------------------------------------------------------------- packing

def build_partmodels(db, index, verbose=False):
    """(entries, kit names) for PARTMODELS.nfsm."""
    kits, kit_id = [], {}
    entries = []
    missing = 0
    lod_count, lod_unshipped, lod_elsewhere = [0, 0, 0, 0], 0, 0
    for ph in sorted(db.records):
        if db.records[ph][2] == NO_MODEL:
            continue
        models = [db.model_hash(ph, m) for m in (0, 1)]
        ks = []
        for mh in models:
            k = index.get(mh) if mh else None
            if k is None:
                # Not in any geometry file -- TIBURON's KITW00_BODY and FULLROOF
                # among them. Retail skips a model it cannot find, and so does
                # carkit.c; the hash stays in the table so its log can name it.
                if mh:
                    missing += 1
                ks.append(NO_KIT)
                continue
            if k not in kit_id:
                kit_id[k] = len(kits)
                kits.append(k)
            ks.append(kit_id[k])
        light = db.attrs.get(ph, {}).get(ATTR_LIGHT_BASE, 0)
        lods = []
        for m, mh in enumerate(models):
            for lod in (1, 2, 3):
                h = db.model_hash(ph, m, lod) if mh and ks[m] != NO_KIT else 0
                if h and index.get(h) != kits[ks[m]]:
                    # A LOD model the table names but no file ships (3,287 of
                    # them), or one in another file than its A model (none).
                    lod_unshipped += index.get(h) is None
                    lod_elsewhere += index.get(h) is not None
                    h = 0
                lods.append(h)
                lod_count[lod] += bool(h)
        entries.append((ph, models[0], models[1], light, ks[0], ks[1],
                        db.level(ph), db.part_slot(ph)) + tuple(lods))
    if len(kits) >= NO_KIT:
        raise SystemExit("%d kits do not fit a u8 index" % len(kits))
    if verbose:
        print("  partmodels: %d parts with a model, %d kits, %d model names not in any file"
              % (len(entries), len(kits), missing))
        print("  partmodels: LOD models B %d, C %d, D %d; %d named but in no file, "
              "%d in another file than their A model"
              % (lod_count[1], lod_count[2], lod_count[3], lod_unshipped, lod_elsewhere))
    return entries, kits


def build_decal_attrs(db, verbose=False):
    """({partHash: value} for PARTMODELS.nfsm's attribute table, the set of
    decal NAMEs). UpdateDecalTextures skips a layout without both SIZE and
    SHAPE, so only those get their SHAPE recorded. A paint or tint part
    carries its LIGHT_MATERIAL instead, and a wheel part its INNER_RADIUS; no
    part has two of the four."""
    table, names, decals, lights, rims = {}, set(), 0, 0, 0
    for ph in sorted(db.attrs):
        attrs = db.attrs[ph]
        shape = attrs.get(ATTR_SHAPE) if ATTR_SIZE in attrs else None
        name = attrs.get(ATTR_NAME)
        light = attrs.get(ATTR_LIGHT_MATERIAL)
        inner = attrs.get(ATTR_INNER_RADIUS)
        if sum(v is not None for v in (shape, name, light, inner)) > 1:
            raise SystemExit("part 0x%08x has more than one of SHAPE, NAME, "
                             "LIGHT_MATERIAL and INNER_RADIUS" % ph)
        if shape is not None:
            table[ph] = shape
        elif name is not None:
            table[ph] = name
            names.add(name)
            decals += 1
        elif light is not None:
            table[ph] = light
            lights += 1
        elif inner is not None:
            table[ph] = inner
            rims += 1
    if verbose:
        print("  part attributes: %d decal layouts, %d decals, %d light materials, "
              "%d rim sizes" % (len(table) - decals - lights - rims, decals, lights, rims))
    return table, names


def pack_partmodels(entries, kits, decal_attrs):
    hdr = 0x20
    ents = b"".join(struct.pack("<IIIIBBBB6I", *e) for e in entries)
    names = b"".join(k.encode("ascii").ljust(KIT_NAME_LEN, b"\0") for k in kits)
    attrs = b"".join(struct.pack("<II", h, decal_attrs[h]) for h in sorted(decal_attrs))
    head = struct.pack("<4s7I", b"NFSM", NFSM_VERSION, len(entries), len(kits),
                       hdr, hdr + len(ents), len(decal_attrs), hdr + len(ents) + len(names))
    return head.ljust(hdr, b"\0") + ents + names + attrs


def kit_light_bases(entries, kits, kit):
    """Every light texture base a part drawn out of `kit` carries."""
    if kit not in kits:
        return set()
    kid = kits.index(kit)
    return {e[3] for e in entries if e[3] and kid in (e[4], e[5])}


def pack_kit(kit, path, wanted, pc_root, exe, globals_, light_bases, maxdim, verbose):
    """One .nfsk for the parts of `path` whose hash is in `wanted`.

    No part matrix is applied to the vertices. Every car-own part but one is
    authored at the identity, and the one that is not -- each car's
    KIT00_SPOILER_A -- carries exactly its base's spoiler marker, which is
    where Render__13CarRenderInfo draws GC slot 12 from anyway; library parts
    are authored at the origin of the marker they hang on. So the matrix is
    the exporter's record of the pivot, and applying it as well as the marker
    would put the spoiler on twice over. carkit.c places by marker. The
    matrix's translation is still written, as the part's pivot: retail reads
    the BODY's for the light flares.
    """
    data = open(path, "rb").read()
    parts = [p for p in cp.parse_geometry(data) if p["hash"] in wanted]
    parts.sort(key=lambda p: p["hash"])
    markers = cp.parse_markers(data)
    car = kit if os.path.basename(os.path.dirname(path)).upper() == kit else None
    # A car folder is one that has a CarTypeInfo wheel table; the libraries
    # (and the traffic-only folders car_pack never packs) do not.
    wheels = cp.load_wheel_table(pc_root, kit) if car else None
    if car and wheels is None:
        car = None
    sources = texture_sources(pc_root, kit, path, exe, globals_)

    verts, indices, batches, part_recs, marker_recs = [], [], [], [], []
    slots = set()
    for p in parts:
        pv, pi, psubs = cp.read_mesh(data, p)
        pmats = cp.subset_materials(data, p)
        ys = [v[1] for v in pv] or [0.0]
        first_batch, first_vert, first_index = len(batches), len(verts), len(indices)
        for (x, y, z, nx, ny, nz, u, v) in pv:
            verts.append((x, y, z, nx, ny, nz, u, -v))
        indices.extend(pi)
        for (fi, ic, tex_slot, flags), mat in zip(psubs, pmats):
            if flags & RESERVED_FLAG_BITS:
                raise SystemExit("%s sub-mesh flags 0x%08x use a bit the runtime borrows"
                                 % (p["name"], flags))
            th = p["textures"][tex_slot] if tex_slot < len(p["textures"]) else 0
            slots.add(th)
            flags |= MAT_CLASS.get(mat, 0) << MAT_CLASS_SHIFT
            batches.append((fi, ic, th, flags, mat))
        part_recs.append((p["hash"], first_batch, len(batches) - first_batch,
                          first_vert, len(pv), first_index, len(pi),
                          (min(ys) + max(ys)) / 2.0) + tuple(p["matrix"][12:15]))
        for (mh, m) in markers.get(p["hash"], []):
            marker_recs.append((p["hash"], mh, m))
        if verbose > 1:
            print("    %-28s v=%-5d tri=%-5d subsets=%d markers=%d"
                  % (p["name"], len(pv), len(pi) // 3, len(psubs),
                     len(markers.get(p["hash"], []))))
    if len(verts) > 0xFFFFFFFF or any(r[4] > 0xFFFF for r in part_recs):
        raise SystemExit("a part in %s exceeds the u16 index range" % kit)

    # Textures: every slot's default, plus the light states carkit.c can swap
    # in and, for a car, the skin carskin.c composites.
    texs = {}         # texHash -> (name, w, h, rgba, skin); rgba None = skin
    slot_map = {}
    unresolved = set()
    for s in sorted(slots):
        if not s:
            continue
        r = resolve_default(s, sources, car)
        if r is None:
            unresolved.add(s)
            continue
        th, hit, skin = r
        slot_map[s] = th
        if hit is not None:
            texs.setdefault(th, hit + (skin,))
    if car:
        texs[b_string_hash(car + "_SKIN1")] = ("<%s_SKIN1 composite>" % car,
                                              SKIN_SIZE, SKIN_SIZE, None, True)
        for base in sorted(light_bases):
            for suffix in ("_OFF", "_GLASS_OFF"):
                th = ext(base, suffix)
                hit = find_tex(sources, th)
                if hit is not None:
                    texs.setdefault(th, hit + (False,))

    tex_entries, tex_blob = encode_textures(texs, sources, maxdim, verbose)
    blob = nfsk_file(tex_entries, tex_blob, slot_map, part_recs, batches, marker_recs,
                     verts, indices, wheels)
    if verbose:
        print("  %-14s %3d parts %6d verts %6d idx %4d batches %3d markers %3d tex (%d B)%s"
              % (kit, len(part_recs), len(verts), len(indices), len(batches),
                 len(marker_recs), len(tex_entries), len(tex_blob),
                 "" if not unresolved else "  flat-paint slots: " +
                 " ".join("%08x" % s for s in sorted(unresolved))))
    missing = set(wanted) - {p["hash"] for p in parts}
    if missing and verbose:
        print("    (%d wanted models are not in %s)" % (len(missing), path))
    return blob


def encode_textures(texs, sources, maxdim, verbose):
    """(TexEntry tuples, blob) for {texHash: (name, w, h, rgba, skin)}, in
    hash order."""
    tex_entries, tex_blob = [], bytearray()
    for th in sorted(texs):
        name, w, h, rgba, skin = texs[th]
        if skin:
            while w > maxdim or h > maxdim:
                w, h = w // 2, h // 2
            fmt, pixels, maxlvl = cp.FMT_SKIN, b"", cp.mip_max_level(w, h)
        else:
            rgba, w, h = cp.box_downscale(rgba, w, h, maxdim)
            fmt, pixels, maxlvl = cp.encode_texture_chain(rgba, w, h)
        if skin:
            mat = cp.MAT_SKIN
        else:
            mat = find_mat(sources, th)
        tex_entries.append((th, w, h, cp.tex_fmt_field(fmt, maxlvl), mat, len(tex_blob)))
        tex_blob.extend(pixels)
        if verbose > 1:
            print("    tex 0x%08x %-30s %dx%d fmt %d +%d mip %d B"
                  % (th, name, w, h, fmt, maxlvl, len(pixels)))
    return tex_entries, bytes(tex_blob)


def nfsk_file(tex_entries, tex_blob, slot_map, part_recs, batches, marker_recs,
              verts, indices, wheels):
    body = bytearray()
    for (th, w, h, fmt, mat, off) in tex_entries:
        body += struct.pack("<IHHHHI", th, w, h, fmt, mat, off)
    for s in sorted(slot_map):
        body += struct.pack("<II", s, slot_map[s])
    for r in part_recs:
        body += struct.pack("<7I4f", *r)
    for b in batches:
        body += struct.pack("<5I", *b)
    for (ph, mh, m) in marker_recs:
        body += struct.pack("<II16f", ph, mh, *m)
    for v in verts:
        body += struct.pack("<8f", *v)
    ib = struct.pack("<%dH" % len(indices), *indices)
    body += ib
    if len(ib) % 4:
        body += b"\0" * (4 - len(ib) % 4)
    tex_off = NFSK_HDR + len(body)
    body += tex_blob

    hdr = struct.pack("<4s11I", b"NFSK", NFSK_VERSION, len(tex_entries), len(part_recs),
                      len(batches), len(verts), len(indices), len(marker_recs),
                      len(slot_map), tex_off, len(tex_blob),
                      KIT_FLAG_WHEELS if wheels else 0)
    if wheels:
        for c in wheels:
            hdr += struct.pack("<5f", c["pos"][0], c["pos"][1], c["pos"][2],
                               c["radius"], c["width"])
        for c in wheels:
            hdr += struct.pack("<2f", c["fender"], c["wide"])
    hdr = hdr.ljust(NFSK_HDR, b"\0")
    assert len(hdr) == NFSK_HDR, len(hdr)
    return bytes(hdr) + bytes(body)


def pack_decals(pc_root, exe, globals_, names, maxdim, verbose):
    """CARS/DECALS.nfsk: the art UpdateDecalTextures (SPEED2.EXE 0x61FF40)
    points a layout's DUMMY_DECAL1..8 at, NAME carried on with the layout's
    shape suffix. Only _RECT (256x64) and _WIDE (256x32) art ships, all in
    CARS/TEXTURES.BIN; every name is looked up under all three suffixes
    anyway, as retail would."""
    sources = [s for s in (cp.load_textures(os.path.join(pc_root, "CARS", "TEXTURES.BIN"),
                                            exe), globals_) if s]
    texs, bare = {}, 0
    for n in sorted(names):
        hits = 0
        for suffix in DECAL_SUFFIXES:
            th = ext(n, suffix)
            hit = find_tex(sources, th)
            if hit is not None:
                texs[th] = hit + (False,)
                hits += 1
        bare += not hits
    tex_entries, tex_blob = encode_textures(texs, sources, maxdim, verbose)
    if verbose:
        print("  %s: %d decal names, %d textures (%d B), %d names with no art"
              % (DECAL_KIT, len(names), len(tex_entries), len(tex_blob), bare))
    return nfsk_file(tex_entries, tex_blob, {}, [], [], [], [], [], None)


def pack_glass(globals_, maxdim, verbose):
    """CARS/GLASS.nfsk: the three cracked panes, a texture-only kit."""
    texs = {}
    for n in GLASS_TEXTURES:
        th = cp.b_string_hash(n)
        hit = find_tex([globals_], th)
        if hit is None:
            raise SystemExit("GLOBALB.BUN has no %s" % n)
        texs[th] = hit + (False,)
    tex_entries, tex_blob = encode_textures(texs, [globals_], maxdim, verbose)
    if verbose:
        print("  %s: %d textures (%d B)" % (GLASS_KIT, len(tex_entries), len(tex_blob)))
    return nfsk_file(tex_entries, tex_blob, {}, [], [], [], [], [], None)


def write_atomic(path, blob):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the retail PC tree (holds CARS/ and GLOBAL/)")
    ap.add_argument("out", help="the game data root to write GLOBAL/ and CARS/ under")
    ap.add_argument("--cars", nargs="*", default=list(DEFAULT_CARS),
                    help="car kits to pack (default: %s)" % " ".join(DEFAULT_CARS))
    ap.add_argument("--libs", nargs="*", default=list(DEFAULT_LIBS),
                    help="library kits to pack (default: %s)" % " ".join(DEFAULT_LIBS))
    ap.add_argument("--exe", default=None, help="SPEED2.EXE, for HUFF textures")
    # Same default and the same reasoning as car_pack.py's: the player's car
    # is always a few metres from the camera, and 512 is the retail source.
    ap.add_argument("--max-texture", type=int, default=512)
    ap.add_argument("-v", "--verbose", action="count", default=0)
    args = ap.parse_args()

    exe = args.exe or os.path.join(args.pc_root, "SPEED2.EXE")
    bun = open(os.path.join(args.pc_root, "GLOBAL", "GLOBALB.BUN"), "rb").read()
    db = PartsDb(bun)
    files = geometry_files(os.path.join(args.pc_root, "CARS"))
    index = model_index(files, args.verbose > 1)
    entries, kits = build_partmodels(db, index, args.verbose)
    decal_attrs, decal_names = build_decal_attrs(db, args.verbose)

    blob = pack_partmodels(entries, kits, decal_attrs)
    out = os.path.join(args.out, "GLOBAL", "PARTMODELS.nfsm")
    write_atomic(out, blob)
    print("%s: %d bytes, %d parts, %d kits, %d decal attributes"
          % (out, len(blob), len(entries), len(kits), len(decal_attrs)))

    globals_ = cp.load_global_car_textures(args.pc_root)
    blob = pack_decals(args.pc_root, exe, globals_, decal_names, args.max_texture,
                       args.verbose)
    out = os.path.join(args.out, "CARS", DECAL_KIT + ".nfsk")
    write_atomic(out, blob)
    print("%s: %d bytes" % (out, len(blob)))
    blob = pack_glass(globals_, args.max_texture, args.verbose)
    out = os.path.join(args.out, "CARS", GLASS_KIT + ".nfsk")
    write_atomic(out, blob)
    print("%s: %d bytes" % (out, len(blob)))
    kit_list = []
    for k in [k.upper() for k in args.cars + args.libs]:
        hits = sorted(n for n in files if fnmatch.fnmatchcase(n, k)) if "*" in k else [k]
        if not hits:
            raise SystemExit("no geometry file matches kit pattern %s" % k)
        kit_list.extend(h for h in hits if h not in kit_list)
    for kit in kit_list:
        if kit not in files:
            raise SystemExit("no geometry file for kit %s" % kit)
        wanted = set()
        if kit in kits:
            kid = kits.index(kit)
            for e in entries:
                if e[4] == kid:
                    wanted.add(e[1])
                    wanted.update(h for h in e[8:11] if h)
                if e[5] == kid:
                    wanted.add(e[2])
                    wanted.update(h for h in e[11:14] if h)
        if kit == AUDIO_KIT:
            empties = {ext(0xFFFFFFFF, n) for n in AUDIO_EMPTY}
            lost = [n for n in AUDIO_EMPTY if ext(0xFFFFFFFF, n) not in index
                    or index[ext(0xFFFFFFFF, n)] != AUDIO_KIT]
            if lost:
                raise SystemExit("AUDIO has no %s" % ", ".join(lost))
            wanted |= empties
        if not wanted:
            print("  %s: no part draws a model out of it -- skipped" % kit)
            continue
        blob = pack_kit(kit, files[kit], wanted, args.pc_root, exe, globals_,
                        kit_light_bases(entries, kits, kit), args.max_texture,
                        args.verbose)
        out = os.path.join(args.out, "CARS", kit + ".nfsk")
        write_atomic(out, blob)
        print("%s: %d bytes" % (out, len(blob)))


if __name__ == "__main__":
    main()
