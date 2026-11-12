#!/usr/bin/env python3
"""Convert the retail car-shadow ramp into SHADOW.nfsc for the 3DS renderer.

Without a shadow the car reads as hovering as soon as the camera gets low
(TASKS.md #148), and the game already ships the art for one: SHADOWRAMP, a
128x8 D3DFMT_A8R8G8B8 texture in GLOBAL/GLOBALB.BUN's Global\\GlobalTextures.tpk
whose RGB is solid black and whose alpha falls 255 -> 0 along U and is constant
down V. It is a *ramp*, not a blob: the engine gets the blob's shape from
geometry and only the falloff curve from the texture.

So this emits the same thing -- a unit-radius triangle fan in the XY plane,
centre vertex at U = 0 (the opaque end of the ramp) and rim vertices at U = 1
(the transparent end) -- with the retail ramp as its one texture. The runtime
scales that unit disc by the car's real body box and puts it under the contact
point (see model3d_draw_baked's MODEL3D_PASS_DECAL and main.c's WORLD_SHADOW_*
constants), which is what makes the blob an ellipse the length and width of the
car rather than a circle.

Two things here are port-authored and not retail:

  * SHADOW_STRENGTH, which scales the ramp's alpha down. The retail ramp is
    fully opaque at its centre because the engine modulates it with a light
    term this port has not decoded; taken raw it paints an ink-black hole under
    the car.
  * the rim U stopping short of 1.0. Textures are sampled GPU_REPEAT with a
    linear filter, so U exactly at 1.0 lands on the seam and blends the
    transparent last texel with the opaque first one -- a hard dark ring right
    around the edge of the shadow.
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import car_pack
import extract_global_textures as egt
import track_pack
from dxt_transcode import decode_bgra, decode_dxt

RAMP_NAME = "SHADOWRAMP"
SHADOW_NAME = "SHADOW.nfsc"

# How far round the disc, in vertices. The shadow is a few dozen pixels across
# on a 400x240 panel, so this is already past the point where more segments
# change the silhouette.
SEGMENTS = 24

# PORT. See the docstring.
SHADOW_STRENGTH = 0.62
RIM_U = 0.99
CENTRE_U = 0.004


def find_ramp(path, name):
    """(w, h, rgba) for one named texture in a GLOBAL .BUN, or None."""
    with open(path, "rb") as fh:
        data = fh.read()
    for coff, csize in egt.find_containers(data):
        parsed = egt.parse_container(data, coff, csize)
        for t in parsed["textures"]:
            if (t["name"] or "").upper() != name:
                continue
            px = t["pixel_data"][:t["expected_size"]]
            if t["fourcc"] == b"RAWB":
                rgba = decode_bgra(px, t["width"], t["height"])
            else:
                rgba = decode_dxt(px, t["width"], t["height"], t["fourcc"])
            return parsed["name"], t["width"], t["height"], bytearray(rgba)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("global_bun", help="PC Files/GLOBAL/GLOBALB.BUN")
    ap.add_argument("--out", metavar="DIR", required=True,
                    help="write SHADOW.nfsc into DIR")
    ap.add_argument("--strength", type=float, default=SHADOW_STRENGTH,
                    help="scale the ramp's alpha by this (default %.2f)"
                         % SHADOW_STRENGTH)
    ap.add_argument("--segments", type=int, default=SEGMENTS)
    args = ap.parse_args()

    found = find_ramp(args.global_bun, RAMP_NAME)
    if not found:
        sys.exit("no %s in %s" % (RAMP_NAME, args.global_bun))
    container, w, h, rgba = found
    print("%s: %s %dx%d" % (container, RAMP_NAME, w, h))

    for i in range(3, len(rgba), 4):
        rgba[i] = int(rgba[i] * args.strength + 0.5)
    fmt, pixels = car_pack.encode_texture(bytes(rgba), w, h)
    tex_entries = [(track_pack.bstring_hash(RAMP_NAME), w, h, fmt, 0)]

    n = max(3, args.segments)
    # Vertex colour is white: the scene shader hands it to the fragment stage
    # as the primary colour and the texture MODULATEs it, so white is "the ramp
    # unchanged" -- and the ramp's RGB is already black.
    verts = [(0.0, 0.0, 0.0, 1.0, 1.0, 1.0, CENTRE_U, 0.5)]
    for i in range(n):
        a = 2.0 * math.pi * i / n
        verts.append((math.cos(a), math.sin(a), 0.0, 1.0, 1.0, 1.0, RIM_U, 0.5))
    indices = []
    for i in range(n):
        indices += [0, 1 + i, 1 + (i + 1) % n]

    batches = [(0, len(indices), 0, 0)]
    blob = car_pack.pack_nfsc(tex_entries, pixels, batches, verts, indices,
                              [-1.0, -1.0, 0.0], [1.0, 1.0, 0.0],
                              car_pack.SHADING_BAKED)
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, SHADOW_NAME)
    with open(path, "wb") as fh:
        fh.write(blob)
    print("wrote %s: %d verts, %d indices, %d B (alpha x%.2f)"
          % (path, len(verts), len(indices), len(blob), args.strength))


if __name__ == "__main__":
    main()
