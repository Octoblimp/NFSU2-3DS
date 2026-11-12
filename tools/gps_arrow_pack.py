#!/usr/bin/env python3
"""Convert the retail GPS arrow into GPSARROW.nfsc for the 3DS (TASKS.md #275).

Player::RenderGPSArrow (Speed.elf 0x8021800C) draws one model, MARKER_GPS_AID
(bStringHash 0x43825817), through Player::RenderArrow (0x802181AC). It ships in
GLOBAL/InGameCommon.bun beside the other in-game markers, in the same
0x80134000 geometry tree as a car or a frontend platform, at the scenery
vertex stride (pos3, D3DCOLOR, uv2) -- so tools/scene_pack.py's reader takes it
as is, baked colour and all, and the runtime draws it on the baked path.

Two subsets, two textures, both from the same file's own TPK:

    ARROW_MARKERA  0x985C190C  DXT1 128x256  opaque, writes depth   the arrow
    SFX_NEONBARA   0x1E232D55  DXT3  64x64   additive, no depth     its glow

Each keeps its TPK render state (car_pack.texture_material), so model3d.c
draws the body in the opaque pass and the glow additively after it, the way
the record says. The glow's alpha is kept as authored: it never reaches 255,
which is exactly what scene_pack.force_opaque() would take for a gloss mask,
but on an additive texture it is the falloff.

The part's own translation (0, -0.348, 0) is folded into the vertices, as
scene_pack.py does for a platform, so the model's origin is the point
RenderArrow's matrix places.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import car_pack  # noqa: E402
import extract_global_textures as egt  # noqa: E402
import scene_pack  # noqa: E402
from dxt_transcode import decode_dxt, decode_bgra  # noqa: E402

ARROW_MODEL = 0x43825817  # MARKER_GPS_AID
ARROW_BUN = os.path.join("GLOBAL", "InGameCommon.bun")
ARROW_NAME = "GPSARROW.nfsc"


def load_textures(data, want):
    """hash -> (name, w, h, rgba, mat) for the hashes in `want`."""
    out = {}
    for off, size in egt.find_containers(data):
        try:
            parsed = egt.parse_container(data, off, size)
        except Exception:
            continue
        for t in parsed["textures"]:
            if t["hash"] not in want or t["hash"] in out:
                continue
            if not t["width"] or not t["height"] or not t["pixel_data"]:
                continue
            px = t["pixel_data"][:t["expected_size"]]
            if t["fourcc"] == b"RAWB":
                rgba = decode_bgra(px, t["width"], t["height"])
            else:
                rgba = decode_dxt(px, t["width"], t["height"], t["fourcc"])
            out[t["hash"]] = (t["name"], t["width"], t["height"], rgba,
                              car_pack.texture_material(t))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the PC data root (holds GLOBAL/)")
    ap.add_argument("--out", metavar="DIR", required=True,
                    help="write GPSARROW.nfsc into DIR")
    ap.add_argument("--maxdim", type=int, default=128,
                    help="downscale textures to at most this many texels on a "
                         "side (default 128: the arrow is a few dozen pixels "
                         "tall on the top screen)")
    args = ap.parse_args()

    path = os.path.join(args.pc_root, ARROW_BUN)
    data = open(path, "rb").read()
    part = next((p for p in car_pack.parse_geometry(data) if p["hash"] == ARROW_MODEL), None)
    if part is None:
        sys.exit("no MARKER_GPS_AID (0x%08x) in %s" % (ARROW_MODEL, path))
    pv, indices, subsets = scene_pack.read_scene_mesh(data, part)

    tx, ty, tz = part["matrix"][12:15]
    verts = []
    bbmin = [1e30] * 3
    bbmax = [-1e30] * 3
    for (x, y, z, r, g, b, u, v) in pv:
        p = (x + tx, y + ty, z + tz)
        for i in range(3):
            bbmin[i] = min(bbmin[i], p[i])
            bbmax[i] = max(bbmax[i], p[i])
        # V negated for the PICA200's flipped texture V axis (see car_pack).
        verts.append((p[0], p[1], p[2], r, g, b, u, -v))

    textures = load_textures(data, set(t for t in part["textures"] if t))
    tex_entries, tex_blob, tex_index = [], bytearray(), {}
    batches = []
    for (first, count, slot, flags) in subsets:
        th = part["textures"][slot] if slot < len(part["textures"]) else 0
        if th not in textures:
            sys.exit("texture 0x%08x of MARKER_GPS_AID is not in %s" % (th, path))
        if th not in tex_index:
            name, w, h, rgba, mat = textures[th]
            rgba, w, h = car_pack.box_downscale(rgba, w, h, args.maxdim)
            fmt, pixels = car_pack.encode_texture(rgba, w, h)
            tex_index[th] = len(tex_entries)
            tex_entries.append((th, w, h, fmt, len(tex_blob), mat))
            tex_blob.extend(pixels)
            print("  tex 0x%08x %-14s %dx%d mat 0x%03x" % (th, name, w, h, mat))
        batches.append((first, count, tex_index[th], flags))

    blob = car_pack.pack_nfsc(tex_entries, tex_blob, batches, verts, indices,
                              bbmin, bbmax, car_pack.SHADING_BAKED)
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, ARROW_NAME)
    with open(out, "wb") as fh:
        fh.write(blob)
    print("wrote %s: %d verts, %d tris, %d batches, bounds (%.2f %.2f %.2f)-(%.2f %.2f %.2f), %d B"
          % (out, len(verts), len(indices) // 3, len(batches), *bbmin, *bbmax, len(blob)))


if __name__ == "__main__":
    main()
