#!/usr/bin/env python3
"""
FRONTEND/PLATFORMS/<scene>.BIN  ->  .nfsc runtime scene model for the 3DS.

The frontend "platform" files (Showroom, Crib01, CarLot01, the four shops) are
the 3D sets the retail menus render behind the UI. Showroom.BIN is the main
menu's, and despite the name it is not an indoor showroom: it is the outdoor
night alley from the retail main menu -- brick walls, a red neon storefront,
chain-link fence, street lamps and a city skyline behind fog, with three
turntable discs (DISC01..03) in front of it, DISC01 being the one the player's
car stands on. They reuse the exact same 0x80134000 geometry tree as
CARS/<car>/GEOMETRY.BIN, so parse_geometry() from car_pack is shared verbatim;
only two things differ:

  - The FVF is 24 bytes, not 36: { f32 pos[3]; u32 D3DCOLOR; f32 uv[2] }. There
    are no normals -- scenery carries baked vertex lighting instead, which is
    why the retail alley reads as night, with hot pools under the street lamps
    and dark asphalt between them. Confirmed empirically: vbPayload/numVerts is
    exactly 24.000 for every part of every file in FRONTEND/PLATFORMS.
  - Textures live in a 0xb3300000 TPK appended to the same file rather than in
    a sibling TEXTURES.BIN, so they come out through extract_global_textures.

The D3DCOLOR alpha byte is *not* opacity: across a whole scene file it only ever
takes four values (0, 110, 115, 255), i.e. it is a per-material class, not a
gradient. The RGB bytes are a real 0..255 gradient and are the baked lighting.
So alpha is dropped and RGB is emitted into the vertex's second float3 slot --
the same slot the car FVF uses for its normal. That keeps the on-disk vertex
identical in size and attribute layout to a car's (pos3 / f32x3 / uv2), so the
runtime shares one loader and one buffer setup and only swaps the vertex
shader: car.v.pica lights the normal, scene.v.pica passes the baked colour
through. The header's `shading` field says which.

Output is the same .nfsc container car_pack.py writes -- see its docstring for
the field layout, and 3DS Files/source/model3d.h for the C mirror.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import car_pack  # noqa: E402
import extract_global_textures as egt  # noqa: E402
from dxt_transcode import decode_dxt, decode_bgra  # noqa: E402
from geometry_transcode import skip_alignment_padding  # noqa: E402

SCENE_VERTEX_STRIDE = 24

# SHD_SHADOWTILE_A, the soft shadow on the floor around each turntable disc
# (TASKS.md #105). It is a fan under every disc: the centre, a ring at r 2.48
# and the rim at r 3.11. Its TPK record says blended (alphaBlendType 1, real
# alpha, no depth write), the texture is black at alpha 102 (column 0 is 0),
# and its vertex colour is black too. Here the D3DCOLOR alpha *is* opacity:
# Showroom and PartsShop01 set it to 155 inside and 0 at the rim, which fades
# the shadow out across the last 0.63 m, and the other six rooms leave all of
# it at 0, so in them retail draws nothing (texture alpha x 0). Every other
# blended room texture has vertex alpha 255, so the rule costs nothing there.
#
# So the tile is packed only where some vertex alpha is non-zero, its vertices
# carry that alpha (0..1) in the colour slot in place of the black RGB, and
# its batches are tagged BATCH_FE_VALPHA: model3d.c's room draw then lets the
# scene shader put the slot's first channel out as the vertex alpha, and the
# TEV's modulate gives texture alpha x vertex alpha, as the PC draws it.
SHADOW_TILE_HASH = 0xb323bfb1

# The turntable the player's car stands on. Retail turns DISC01 with the car,
# so its batches carry this flag and model3d_draw_scene_turned() draws them
# rotated about the disc's centre. Bit 30 of the sub-mesh flags: the platform
# files only ever set 0x40, and a room has no other use for the high bits.
TURNTABLE_PART = "DISC01"
BATCH_TURNTABLE = 0x40000000
# Bit 26: the batch's colour slot holds its vertex alpha (see SHADOW_TILE_HASH;
# model3d.c BATCH_FE_VALPHA, honoured on the frontend room draw only).
BATCH_FE_VALPHA = 0x04000000

# Every room texture carries its own TPK render state in the .nfsc
# (car_pack.texture_material(), as world sections do; #105). Before, a room
# had no material data and model3d.c drew every textured batch alpha-blended
# *and* cut at 0x40, so an opaque texture whose alpha is a grunge mask --
# the Showroom's asphalt, SHRM_ROAD, opaque with real alpha -- was punched
# full of black holes and washed over the clear colour. Its record says
# opaque, so it is drawn opaque, alpha ignored; punchthrough art is cut,
# blended art (the fence, the skyline, the neon, the disc shadow) blends.


def read_scene_mesh(data, part):
    """Same shape as car_pack.read_mesh, at the 24-byte scenery stride, with
    the D3DCOLOR unpacked into the normal slot as linear 0..1 RGB."""
    m = part["mesh"]
    vs = skip_alignment_padding(data, *m["vb"])
    verts = []
    for i in range(m["numVerts"]):
        b = vs + i * SCENE_VERTEX_STRIDE
        x, y, z = struct.unpack_from("<3f", data, b)
        colour, = struct.unpack_from("<I", data, b + 12)
        u, v = struct.unpack_from("<2f", data, b + 16)
        r = ((colour >> 16) & 0xff) / 255.0
        g = ((colour >> 8) & 0xff) / 255.0
        bl = (colour & 0xff) / 255.0
        verts.append((x, y, z, r, g, bl, u, v))

    istart = skip_alignment_padding(data, *m["ib"])
    indices = list(struct.unpack_from("<%dH" % (m["numTris"] * 3), data, istart))

    sstart = skip_alignment_padding(data, *m["subsets"])
    subsets = []
    for i in range(m["numSubsets"]):
        b = sstart + i * car_pack.SUBSET_STRIDE
        index_count = struct.unpack_from("<I", data, b + 0x0c)[0]
        tex_slot = struct.unpack_from("<I", data, b + 0x1c)[0]
        first_index = struct.unpack_from("<I", data, b + 0x34)[0]
        flags = struct.unpack_from("<I", data, b + 0x38)[0]
        subsets.append((first_index, index_count, tex_slot, flags))
    return verts, indices, subsets


def load_scene_textures(data, records=None):
    """hash -> (name, w, h, rgba) for every 0xb3300000 pack in the file.

    Platform files carry two: the room's art pack and a 6-face environment cube
    map for car reflections. Both are read; the cube map's faces simply never
    match a geometry texture hash and so never make it into the output.
    """
    out = {}
    for off, size in egt.find_containers(data):
        try:
            parsed = egt.parse_container(data, off, size)
        except Exception:
            continue
        for t in parsed["textures"]:
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
            if records is not None and t["alpha_blend"] != 0:
                # A blended texture's alpha is its coverage however faint
                # (the disc shadow peaks at 102), never a gloss mask.
                out[t["hash"]] = (t["name"], t["width"], t["height"], rgba)
            else:
                out[t["hash"]] = (t["name"], t["width"], t["height"], force_opaque(rgba))
            if records is not None:
                records[t["hash"]] = t
    return out


def force_opaque(rgba, always=False):
    """Blank the alpha channel of any texture that is nowhere fully opaque.

    Several of the crib's DXT3 maps use alpha as a gloss/specular mask rather
    than as coverage -- GFE_LINOLEUM_1A (the floor) peaks at alpha 51, and
    GRG_SUBDISK (the turntable top) at 119, so an alpha test would throw the
    whole floor and the discs away. A cutout mask always reaches 255 somewhere
    (GFE_POSTERS and GFE_GLASSRAIL both do, and both keep their alpha), so
    "never reaches 255" is the test for a mask that is not coverage.

    `always` skips that test, for a caller that already knows the answer from
    real data: tools/track_pack.py reads the TPK record's alphaUsageType, and
    type 0 means "this alpha channel is not coverage" outright.
    """
    if not always and max(rgba[3::4]) == 255:
        return rgba
    out = bytearray(rgba)
    out[3::4] = b"\xff" * (len(out) // 4)
    return bytes(out)


def build(bin_path, maxdim, verbose):
    data = open(bin_path, "rb").read()
    parts = car_pack.parse_geometry(data)
    records = {}
    textures = load_scene_textures(data, records)

    verts, indices, batches = [], [], []
    tex_order, tex_index = [], {}
    bbmin = [1e30] * 3
    bbmax = [-1e30] * 3
    unresolved = {}
    skipped = 0

    for part in parts:
        pv, pi, psubs = read_scene_mesh(data, part)
        base = len(verts)
        # Translation only, for the same reason as car_pack: the discs are one
        # authored mesh instanced three times by their part matrices.
        tx, ty, tz = part["matrix"][12:15]
        for (x, y, z, r, g, b, u, v) in pv:
            x += tx
            y += ty
            z += tz
            bbmin = [min(bbmin[i], c) for i, c in enumerate((x, y, z))]
            bbmax = [max(bbmax[i], c) for i, c in enumerate((x, y, z))]
            # V negated for the PICA200's flipped texture V axis (see car_pack).
            verts.append((x, y, z, r, g, b, u, -v))

        ibase = len(indices)
        indices.extend(i + base for i in pi)
        turntable = BATCH_TURNTABLE if (part["name"] or "").upper() == TURNTABLE_PART else 0

        for (first_index, index_count, tex_slot, flags) in psubs:
            flags |= turntable
            th = part["textures"][tex_slot] if tex_slot < len(part["textures"]) else 0
            if th == SHADOW_TILE_HASH:
                # The D3DCOLOR alpha bytes of this batch's vertices.
                m = part["mesh"]
                vs = skip_alignment_padding(data, *m["vb"])
                used = pi[first_index:first_index + index_count]
                alpha = {i: data[vs + i * SCENE_VERTEX_STRIDE + 15] for i in set(used)}
                if not any(alpha.values()):
                    skipped += 1      # all 0: retail draws nothing here
                    continue
                # Its own copies of the vertices, the alpha in the colour
                # slot, so no other batch sharing them is affected.
                remap = {}
                for i in sorted(alpha):
                    x, y, z, _r, _g, _b, u, v = verts[base + i]
                    a = alpha[i] / 255.0
                    remap[i] = len(verts)
                    verts.append((x, y, z, a, a, a, u, v))
                first_index = len(indices) - ibase
                indices.extend(remap[i] for i in used)
                flags |= BATCH_FE_VALPHA
            ti = -1
            if th in textures:
                if th not in tex_index:
                    tex_index[th] = len(tex_order)
                    tex_order.append(th)
                ti = tex_index[th]
            elif th:
                unresolved[th] = unresolved.get(th, 0) + 1
            batches.append((ibase + first_index, index_count, ti, flags))

        if verbose:
            print("  %-24s v=%-5d tri=%-5d subsets=%d"
                  % (part["name"] or "(unnamed)", len(pv), len(pi) // 3, len(psubs)))

    if len(verts) > 65535:
        raise SystemExit("%d vertices exceeds the u16 index range" % len(verts))

    tex_entries, tex_blob = [], bytearray()
    for th in tex_order:
        name, w, h, rgba = textures[th]
        rgba, w, h = car_pack.box_downscale(rgba, w, h, maxdim)
        fmt, pixels = car_pack.encode_texture(rgba, w, h)
        mat = car_pack.texture_material(records[th])
        tex_entries.append((th, w, h, fmt, len(tex_blob), mat))
        tex_blob.extend(pixels)
        if verbose:
            print("  tex 0x%08x %-24s %dx%d %s %dB"
                  % (th, name, w, h,
                     "RGB565" if fmt == car_pack.FMT_RGB565 else "RGBA4444", len(pixels)))

    if unresolved and verbose:
        print("  unresolved texture hashes:")
        for th, n in sorted(unresolved.items(), key=lambda kv: -kv[1]):
            print("    0x%08x x%d" % (th, n))

    blob = car_pack.pack_nfsc(tex_entries, tex_blob, batches, verts, indices,
                              bbmin, bbmax, car_pack.SHADING_BAKED)
    return blob, {
        "parts": len(parts), "verts": len(verts), "indices": len(indices),
        "batches": len(batches), "textures": len(tex_entries),
        "bbmin": bbmin, "bbmax": bbmax, "texbytes": len(tex_blob), "skipped": skipped,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin_file", help="PC Files/FRONTEND/PLATFORMS/<scene>.BIN")
    ap.add_argument("out", help="output .nfsc path")
    ap.add_argument("--max-texture", type=int, default=128)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    blob, stats = build(args.bin_file, args.max_texture, args.verbose)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(blob)
    print("%s: %d bytes  %d parts, %d verts, %d indices, %d batches, %d textures (%d B)"
          % (args.out, len(blob), stats["parts"], stats["verts"], stats["indices"],
             stats["batches"], stats["textures"], stats["texbytes"]))
    if stats["skipped"]:
        print("  %d disc shadow subset(s) left out (vertex alpha all 0: retail draws nothing)" % stats["skipped"])
    print("  bbox min=%s max=%s"
          % ([round(c, 3) for c in stats["bbmin"]], [round(c, 3) for c in stats["bbmax"]]))


if __name__ == "__main__":
    main()
