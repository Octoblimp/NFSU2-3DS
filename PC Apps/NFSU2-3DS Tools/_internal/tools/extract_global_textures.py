#!/usr/bin/env python3
"""
Extractor for the frontend/global ".tpk" texture-pack variant used by
Global\\GlobalTextures.tpk / Global\\GlobalTexturesPC.tpk / GlobalTexturesNA.bin
/ GlobalTexturesROW.bin (embedded inside PC Files/GLOBAL/GLOBALB.BUN, or
standalone). See docs/formats/texture-tpk.md ("Frontend/global variant"
section) for the full format writeup this implements.

Unlike the car/track TEXTURES.BIN variant (tools/dxt_transcode.py), this
variant:
  - uses a different per-texture metadata chunk, 0x33310004 (124-byte
    records) instead of 0x33310003 (24-byte records), plus a parallel
    0x33310005 chunk (32-byte records, one per texture in the same order)
    that carries the compression fourcc ("DXT1"/"DXT3") at offset +0x14.
  - stores pixel data RAW (uncompressed by JDLZ/HUFF) in the 0x33320002
    leaf, back-to-back per texture, located by an absolute byte offset
    (record + 0x30) into that leaf -- not by a trailer or a JDLZ/HUFF magic.
  - a texture's byte run length isn't stored explicitly; it's recovered by
    sorting all texture offsets and taking the gap to the next one (the
    last texture's length is inferred from the leaf's end).

Also handles the container appearing at an arbitrary byte offset inside a
larger file (e.g. GLOBALB.BUN), unlike scan_texture_formats.py's leaf walker
which assumes the whole buffer is one chunk tree rooted at offset 0.

Usage:
    python extract_global_textures.py <bin_file> <out_dir> [--container N]
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from dxt_transcode import decode_dxt, decode_bgra  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None

CONTAINER_TAG = 0xB3300000
INFO_TAG = 0xB3310000
PIXEL_TAG = 0xB3320000
NAME_TAG = 0x33310001
HASH_TABLE_TAG = 0x33310002
INFO_RECORD_TAG = 0x33310004
FORMAT_RECORD_TAG = 0x33310005
PIXEL_LEAF_TAG = 0x33320002

# The boundary a pixel leaf's 0x11 filler run aligns its payload to. See the
# use in parse_container(); mirrored by PIXEL_LEAF_ALIGN in
# 3DS Files/source/texpack.c.
PIXEL_LEAF_ALIGN = 16

INFO_RECORD_SIZE = 0x7C
FORMAT_RECORD_SIZE = 0x20


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def u16(data, off):
    return struct.unpack_from("<H", data, off)[0]


def find_containers(data):
    """Scan the whole file for 0xb3300000 container tags (they can appear
    anywhere, e.g. embedded inside a larger .BUN alongside unrelated data)."""
    pos = 0
    found = []
    while True:
        pos = data.find(struct.pack("<I", CONTAINER_TAG), pos)
        if pos < 0:
            break
        size = u32(data, pos + 4)
        if pos + 8 + size <= len(data):
            found.append((pos, size))
        pos += 4
    return found


def walk_chunks(data, start, end):
    """Yield (tag, payload_start, payload_size) for each top-level chunk in
    [start, end); recurse isn't needed here since we know the exact known
    shape (container -> info/pixel sub-containers -> leaves)."""
    p = start
    while p < end:
        tag = u32(data, p)
        size = u32(data, p + 4)
        yield tag, p + 8, size
        p += 8 + size


def expand_p8(indices, palette):
    """A P8 texture's index bytes -> BGRA bytes, through its 256-entry CLUT.

    Returned as BGRA rather than as (indices, palette) so that every caller's
    existing raw-BGRA path decodes a palettized texture with no change: the
    container's own bytes are untouched, the expansion happens here. Palette
    entries are BGRA in the same order decode_bgra() expects -- confirmed on
    RDP_ROADSTRIP_COBBLEROA, whose crosswalk stripes come out yellow only in
    that order.
    """
    n = len(palette) // 4
    out = bytearray(len(indices) * 4)
    for j, v in enumerate(indices):
        if v >= n:
            v = 0
        out[j * 4:j * 4 + 4] = palette[v * 4:v * 4 + 4]
    return bytes(out)


def parse_container(data, container_off, container_size):
    """Parse one 0xb3300000 container, returning a dict with the resolved
    texture list (name, hash, width, height, fourcc, pixel bytes)."""
    end = container_off + 8 + container_size
    name_off = None
    info_records_off = None
    n_records = 0
    format_records_off = None
    pixel_leaf_off = None
    pixel_leaf_size = 0

    for tag, poff, psize in walk_chunks(data, container_off + 8, end):
        if tag == INFO_TAG:
            for itag, ioff, isize in walk_chunks(data, poff, poff + psize):
                if itag == NAME_TAG:
                    name_off = ioff
                elif itag == INFO_RECORD_TAG:
                    info_records_off = ioff
                    n_records = isize // INFO_RECORD_SIZE
                elif itag == FORMAT_RECORD_TAG:
                    format_records_off = ioff
        elif tag == PIXEL_TAG:
            for itag, ioff, isize in walk_chunks(data, poff, poff + psize):
                if itag == PIXEL_LEAF_TAG:
                    # The leaf payload starts with a run of 0x11 filler bytes
                    # that re-aligns the DXT block grid (the 8-byte chunk header
                    # leaves the payload at 8 mod 16). Info-record pixel offsets
                    # are relative to the position AFTER that filler -- 120 bytes
                    # in every leaf in the shipped game. Skipping it is what
                    # makes DXT3 decode correctly; without it the block grid is
                    # 8 bytes off and images come out coherent but horizontally
                    # wrapped.
                    #
                    # The run length alone cannot be trusted to find the end of
                    # it, for exactly the reason TASKS.md #139 documents for the
                    # geometry leaves: a *real* leading pixel byte of 0x11 is
                    # indistinguishable from one more filler byte, so the scan
                    # walks straight past it. One leaf in the game does this --
                    # Global\InGameTextures.tpk in GLOBAL/InGameCommon.bun,
                    # whose first three pixel bytes are 11 11 11 -- and the
                    # 3-byte overrun shifted its whole block grid, which is what
                    # rendered SFX_SMOKE_B_CS, SFX_NEONBARB, SFX_LIGHT_BEAMB and
                    # OBJ_TRACKBARRIERB_NEON_ as coloured scanline static in the
                    # world (TASKS.md #148).
                    #
                    # The filler exists to align, so the position after it is
                    # 16-byte aligned -- true of 1420 of the game's 1421 pixel
                    # leaves, the exception being the one the scan gets wrong.
                    # Backing the run off to the last aligned position inside it
                    # can therefore only ever shorten an overrun: a leaf whose
                    # genuine filler already ends aligned is left exactly as it
                    # was.
                    pad = 0
                    while pad < isize and data[ioff + pad] == 0x11:
                        pad += 1
                    back = (ioff + pad) % PIXEL_LEAF_ALIGN
                    if back <= pad:
                        pad -= back
                    pixel_leaf_off = ioff + pad
                    pixel_leaf_size = isize - pad

    container_name = None
    if name_off is not None:
        # record layout: u32 flag/count, then two null-padded strings
        # (short dir name, then full "Dir\Name.tpk" path) -- we want the
        # second one, which starts right after the first string's slot.
        raw = data[name_off:name_off + 124]
        parts = raw[4:].split(b"\x00")
        strings = [p.decode("ascii", "replace") for p in parts if p]
        path_strings = [s for s in strings if "." in s or "\\" in s]
        container_name = path_strings[0] if path_strings else (strings[0] if strings else None)

    if info_records_off is None or pixel_leaf_off is None:
        return {"name": container_name, "textures": []}

    entries = []
    for i in range(n_records):
        roff = info_records_off + i * INFO_RECORD_SIZE
        tex_name = data[roff + 0xC:roff + 0x24].split(b"\x00")[0].decode("ascii", "replace")
        tex_hash = u32(data, roff + 0x24)
        has_primary = u32(data, roff + 0x38) != 0
        pixel_off_in_leaf = u32(data, roff + 0x30)
        # Palette byte size: 0x400 (256 BGRA entries) on every P8 record and 0
        # on every DXT/raw one, over all 1032 records of L4RA's Y0 library and
        # all 396 textures in PC Files/GLOBAL/. This is what says a record is
        # palettized independently of the format code, and what gives each
        # CLUT its length for the offset walk below.
        palette_size = u32(data, roff + 0x3C)
        width = u16(data, roff + 0x44)
        height = u16(data, roff + 0x46)
        # The material bytes. NFSU2's TextureInfo carries its render state in
        # the record right after the dimensions, and nothing in this port read
        # them before -- which is why every world surface drew with one blend
        # state and the neon barriers came out as opaque black squares with a
        # red chevron on them instead of glowing (see docs/formats/texture-tpk.md).
        #
        # Derived by correlating all 22 records of the sections around the
        # reference capture against what each texture visibly is:
        #
        #   0x48 shiftWidth   log2(width)  -- 07 on every 128px, 08 on every 256px
        #   0x49 shiftHeight  log2(height)
        #   0x4A imageCompressionType  0x22 = DXT1, 0x24 = DXT3 (agrees with fourcc)
        #   0x4E numMipMapLevels (3..6)
        #   0x4F tilableUV
        #   0x51 renderingOrder  0 opaque; 1 blended; 5/7 on every glow
        #   0x54 applyAlphaSorting  1 on the transparent ones
        #   0x55 alphaUsageType   0 none, 1 punchthrough/cutout, 2 real alpha
        #   0x56 alphaBlendType   0 opaque, 1 normal blend, 2 ADDITIVE
        #   0x57 writeDepth
        #
        # Checked across all 396 distinct textures in PC Files/GLOBAL/: the
        # (alphaUsageType, alphaBlendType) pairs are (2,1) x214 blended, (2,2)
        # x128 additive, (0,0) x37 opaque and (1,0) x13 cutout, plus exactly
        # four HUD/2D outliers -- GETAWAY_DISTANCE_BAR, RADIALMASKBW, TACH_NOS
        # and SFX_FLARE_STREAK_A -- which are the only records with an
        # alphaBlendType outside 0..2 or a writeDepth that disagrees with it.
        # So writeDepth is carried as its own field rather than derived, and a
        # blend type this port has not seen is treated as a normal blend rather
        # than guessed at.
        #
        # alphaBlendType 2 covers OBJ_TRACKBARRIERB_NEON_, SFX_NEONBARA/B,
        # SFX_LIGHT_BEAMA/B, NEONBLUR and every DECAL_ICON_*GLOW -- the neon
        # and glow sprites. Type 1 covers OBJ_TRACKBARRIERB_B_BC,
        # SFX_SMOKE_A/B_CS and SS_NOS_GLOW. Type 0 covers the opaque art such
        # as OBJ_TRACKBARRIERB_BC.
        rendering_order = data[roff + 0x51]
        alpha_sorting = data[roff + 0x54]
        alpha_usage = data[roff + 0x55]
        alpha_blend = data[roff + 0x56]
        write_depth = data[roff + 0x57]
        mip_levels = data[roff + 0x4E]
        tilable_uv = data[roff + 0x4F]
        # The UV scroll (TASKS.md #269): the fields eDataRender::Render
        # (Speed.elf 0x8031164C..0x803117D4) reads off this same record and
        # hands to TextureInfo::GetScroll (0x800CFDAC) for a texture matrix.
        #
        #   0x52 scrollType      0 none, 1 smooth, 2 stepped, 3 a fixed offset
        #   0x58 scrollTimeStep  s16, x 1/256 s: how long a stepped scroll holds
        #   0x5A scrollSpeedS    s16, x 1/1024 texture widths per second (per
        #                        step for type 2)
        #   0x5C scrollSpeedT    s16, the same along V
        #   0x5E offsetS         s16, x 1/1024: type 3's fixed offset, negated
        #   0x60 offsetT         s16
        #
        # 23 textures across the PC data set a type. OBJ_TRACKBARRIERB_NEON_,
        # the race barriers' red chevrons, is type 2: S jumps by -0.3125 every
        # 76/256 s, which is what makes them run along the barrier.
        scroll_type = data[roff + 0x52]
        scroll = struct.unpack_from("<5h", data, roff + 0x58)
        fourcc = b"DXT1"
        unsupported = False
        palettized = False
        if format_records_off is not None:
            foff = format_records_off + i * FORMAT_RECORD_SIZE
            candidate = data[foff + 0x14:foff + 0x18]
            code = u32(data, foff + 0x14)
            if candidate in (b"DXT1", b"DXT3"):
                fourcc = candidate
            elif code == 0x15:
                # D3DFMT_A8R8G8B8 (21): raw, uncompressed BGRA8888 pixel data,
                # not DXT-block-encoded -- matches 3DS Files/source/texpack.c's
                # TEXFMT_RAW_BGRA8888 handling. Confirmed via PC_LOAD: its
                # format record has no ASCII fourcc, only this numeric D3DFMT
                # code, and decoding its pixel data as raw BGRA8888 (instead
                # of the wrong DXT1 fallback) reproduces the real boot-screen
                # background art matching example_load.webp exactly.
                fourcc = b"RAWB"
            elif code == 0x29 and palette_size:
                # D3DFMT_P8 (41): 8-bit palettized. The CLUTs are NOT stored
                # per texture next to its pixels -- they are packed together at
                # the very START of the 0x33320002 pixel leaf, before the first
                # texture's data, one per palettized record in record order.
                #
                # That is why the palette was never found: nothing in the info
                # record points at it, and a P8 texture's own byte run is
                # exactly its index mip chain with nothing appended (in Y0,
                # RDP_LANEA_YS_AA_KT at 64x512 with 4 mips occupies precisely
                # 32768+8192+2048+512 = 43520 bytes, the full gap to the next
                # texture). Locating them by elimination: after laying every
                # one of Y0's 1032 textures down at its recorded offset for its
                # computed run length, the only unclaimed region in the leaf is
                # the first 21504 bytes -- exactly 21 x 1024, against exactly 21
                # P8 records, each of which declares palette_size 0x400.
                #
                # Verified by decoding: pairing the k-th P8 record with the k-th
                # 1024-byte CLUT renders all 21 as coherent road art (asphalt
                # with dashed white and solid yellow lane lines, yellow crosswalk
                # stripes, cobbles, parking bays, gravel shoulders, sidewalk
                # paving). Any other pairing renders noise.
                #
                # Record order and pixel-offset order are the same for all 21
                # here, so this evidence cannot separate them; record order is
                # used because that is the order everything else in the record
                # arrays is keyed by.
                palettized = True
                fourcc = b"RAWB"
            elif code != 0 and candidate != b"DXT1":
                # No ASCII DXT fourcc, not raw BGRA8888 (0x15) and not P8 with
                # a declared CLUT. Skip rather than mis-decode as DXT1: a blind
                # DXT1 fallback is what used to give the main menu its
                # RGB-static/"interlaced" look, because the block math reads a
                # different number of bytes than the real format occupies.
                unsupported = True
        entries.append(dict(index=i, name=tex_name, hash=tex_hash,
                             has_primary=has_primary, poff=pixel_off_in_leaf,
                             width=width, height=height, fourcc=fourcc,
                             unsupported=unsupported,
                             palettized=palettized, palette_size=palette_size,
                             rendering_order=rendering_order,
                             alpha_sorting=alpha_sorting,
                             alpha_usage=alpha_usage,
                             alpha_blend=alpha_blend,
                             write_depth=write_depth,
                             mip_levels=mip_levels,
                             tilable_uv=tilable_uv,
                             scroll_type=scroll_type, scroll=scroll))

    # Recover each texture's byte run length: sort by pixel offset and take
    # the gap to the next texture (the format has no explicit size field).
    # Unsupported-format entries (no real CLUT available) are excluded here,
    # not just filtered from the output below -- matching texpack.c's `continue`,
    # which skips them before they ever enter its equivalent sorted array. This
    # keeps neighbor gap-length recovery identical between the two implementations.
    # Walk the packed CLUT block at the head of the leaf: the k-th palettized
    # record's palette starts where the previous one ended. If the block does
    # not fit in front of the first texture's pixels then this container does
    # not lay its palettes out the way Y0 does, and guessing would fabricate
    # colour -- so drop back to skipping those records rather than inventing a
    # palette, which is the rule this parser already applies to formats it
    # cannot read.
    pal_cursor = 0
    for e in entries:
        if e["palettized"]:
            e["paloff"] = pal_cursor
            pal_cursor += e["palette_size"]
    if pal_cursor:
        first_pixel = min((e["poff"] for e in entries
                           if e["has_primary"] and not e["unsupported"]),
                          default=0)
        if pal_cursor > first_pixel:
            for e in entries:
                if e["palettized"]:
                    e["palettized"] = False
                    e["unsupported"] = True

    active = [e for e in entries if e["has_primary"] and not e["unsupported"]]
    active.sort(key=lambda e: e["poff"])
    for idx, e in enumerate(active):
        if idx + 1 < len(active):
            e["length"] = active[idx + 1]["poff"] - e["poff"]
        else:
            e["length"] = pixel_leaf_size - e["poff"]

    textures = []
    for e in active:
        if e["palettized"]:
            expected = e["width"] * e["height"]        # one index byte per texel
        elif e["fourcc"] == b"RAWB":
            expected = e["width"] * e["height"] * 4
        else:
            expected = (e["width"] // 4) * (e["height"] // 4) * (16 if e["fourcc"] == b"DXT3" else 8)
        length = e["length"]
        if expected > 0 and length > expected:
            length = expected  # trailing gap can include unrelated padding
        pdata = data[pixel_leaf_off + e["poff"]: pixel_leaf_off + e["poff"] + length]
        if e["palettized"]:
            pal = data[pixel_leaf_off + e["paloff"]:
                       pixel_leaf_off + e["paloff"] + e["palette_size"]]
            pdata = expand_p8(pdata, pal)
            expected = e["width"] * e["height"] * 4   # now BGRA, as fourcc says
        textures.append(dict(name=e["name"], hash=e["hash"], width=e["width"],
                              height=e["height"], fourcc=e["fourcc"], pixel_data=pdata,
                              palettized=e["palettized"],
                              expected_size=expected, actual_size=len(pdata),
                              rendering_order=e["rendering_order"],
                              alpha_sorting=e["alpha_sorting"],
                              alpha_usage=e["alpha_usage"],
                              alpha_blend=e["alpha_blend"],
                              write_depth=e["write_depth"],
                              mip_levels=e["mip_levels"],
                              tilable_uv=e["tilable_uv"],
                              scroll_type=e["scroll_type"],
                              scroll=e["scroll"]))

    return {"name": container_name, "textures": textures}


# NOTE (resolved): this file used to carry a fix_u2_loading_bg_wrap() hack that
# circularly shifted U2_LOADING_BG (hash 0x70eb2ccb) left by 85px, with a comment
# admitting "the true root cause (why 85, specifically) is unconfirmed". The root
# cause is now known and fixed properly in parse_container(): pixel offsets are
# relative to the pixel leaf AFTER its leading 0x11 alignment filler, and reading
# from the unaligned payload start rolled every texture in the pack. With the
# filler skipped, U2_LOADING_BG decodes with no wrap at all, so the hack has been
# removed rather than kept -- applying it now would corrupt a correct image.


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin_file")
    ap.add_argument("out_dir")
    ap.add_argument("--container", type=int, default=None,
                     help="only extract the Nth container found (0-based); default: all")
    args = ap.parse_args()

    with open(args.bin_file, "rb") as f:
        data = f.read()

    containers = find_containers(data)
    if not containers:
        print("No 0xb3300000 texture-pack containers found.")
        return

    os.makedirs(args.out_dir, exist_ok=True)
    total = 0
    for ci, (coff, csize) in enumerate(containers):
        if args.container is not None and ci != args.container:
            continue
        parsed = parse_container(data, coff, csize)
        cname = parsed["name"] or f"container{ci}"
        safe_cname = cname.replace("\\", "_").replace("/", "_")
        print(f"Container {ci} @ 0x{coff:x}: {cname!r}, {len(parsed['textures'])} textures")
        cdir = os.path.join(args.out_dir, safe_cname)
        os.makedirs(cdir, exist_ok=True)
        for t in parsed["textures"]:
            if t["width"] == 0 or t["height"] == 0 or not t["pixel_data"]:
                continue
            base = f"{t['name'] or hex(t['hash'])}_{t['fourcc'].decode()}_{t['width']}x{t['height']}"
            try:
                if t["fourcc"] == b"RAWB":
                    rgba = decode_bgra(t["pixel_data"][:t["expected_size"]], t["width"], t["height"])
                else:
                    rgba = decode_dxt(t["pixel_data"][:t["expected_size"]], t["width"], t["height"], t["fourcc"])
            except Exception as ex:
                print(f"  skip {base}: {ex}")
                continue
            if Image is not None:
                Image.frombytes("RGBA", (t["width"], t["height"]), rgba).save(
                    os.path.join(cdir, base + ".png"))
            total += 1
    print(f"Decoded {total} textures to {args.out_dir}")


if __name__ == "__main__":
    main()
