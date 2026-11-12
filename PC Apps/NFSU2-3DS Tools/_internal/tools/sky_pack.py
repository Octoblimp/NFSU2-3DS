#!/usr/bin/env python3
"""Convert the retail skydome into SKYDOME.nfsc for the 3DS world renderer.

The world's sky is a real model in the retail data, not something the port has
to invent: `SKYDOME` (bStringHash 0xbe43edbb) is authored in L4RA's X0 model
library and placed exactly once, in Z0. tools/track_pack.py cannot emit it,
and should not -- X/Y/Z sections are resource libraries rather than places, so
FIRST_RESOURCE_SECTION drops them, and a 16 km-wide dome converted as a place
would read to the pager as permanently underfoot. The dome is not a place: it
is drawn centred on the camera with depth writes off, so it comes out of the
pipeline as its own standalone file instead.

Its two textures are in no stream section's TPK and in no GLOBAL one either.
They live in TRACKS/LOC4DYNTEX.BIN, the *dynamic* texture pack -- the third
TPK variant (0x33310003, 24-byte records, independently JDLZ/HUFF-compressed
blobs) that tools/dxt_transcode.py already reads. Retail swaps a time-of-day
set into the dome's two slots from there:

    band (512x128, DXT1)   SKY_SUNRISE_A  SKY_SUNSET_A  SKY_NIGHT_A
    cap  (256x256, DXT3)   ..._A_CAP      ..._A_CAP     ..._A_CAP

The model's own slots name the sunrise pair, but NFSU2's world is always
night, so --time defaults to night. Pass --time sunrise/sunset to bake one of
the others instead.

The emitted mesh is normalised to unit horizontal radius with its base at
z = 0, so the runtime can pick the dome's size against its own far plane (see
SKY_RADIUS in world.c) rather than inheriting the authored 9.7 km, which no
3DS depth range wants.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import car_pack
import dxt_transcode
import huff
import track_pack

SKY_MODEL = "SKYDOME"
DYNTEX_NAME = "LOC4DYNTEX.BIN"
SKY_SETS = {
    "night": ("SKY_NIGHT_A", "SKY_NIGHT_A_CAP"),
    "sunrise": ("SKY_SUNRISE_A", "SKY_SUNRISE_A_CAP"),
    "sunset": ("SKY_SUNSET_A", "SKY_SUNSET_A_CAP"),
}
SKY_NAME = "SKYDOME.nfsc"


def find_model(stream, sections, want_hash):
    """(section, section_data, part) for one model hash, libraries first."""
    order = ([s for s in sections if s.name.startswith(("X", "Y", "Z"))] +
             [s for s in sections if not s.name.startswith(("X", "Y", "Z"))])
    for sec in order:
        data = track_pack.read_section(stream, sec)
        for p in track_pack.parse_section_geometry(data):
            if p["hash"] == want_hash:
                return sec, data, p
    return None, None, None


def load_dyntex(path, exe_path):
    """name -> (w, h, rgba) for every texture in LOC4DYNTEX.BIN."""
    data = open(path, "rb").read()
    dec = huff.HuffDecompressor(exe_path) if os.path.isfile(exe_path) else None
    out = {}
    for name, w, h, _fmt, rgba in dxt_transcode.iter_textures(data, huff_dec=dec):
        out[name] = (w, h, rgba)
    return out


def horizon_rgb(rgba, w, h, rows=8):
    """Mean colour of the band texture's bottom `rows` -- the colour the world
    fades into at the draw radius. Printed so the runtime's fog colour can be
    taken from the art rather than guessed."""
    tot = [0, 0, 0]
    n = 0
    for y in range(max(0, h - rows), h):
        for x in range(w):
            p = (y * w + x) * 4
            tot[0] += rgba[p]
            tot[1] += rgba[p + 1]
            tot[2] += rgba[p + 2]
            n += 1
    return tuple(c // max(1, n) for c in tot)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("track_dir", help="PC Files/TRACKS")
    ap.add_argument("track", help="track code, e.g. L4RA")
    ap.add_argument("--out", metavar="DIR", required=True,
                    help="write SKYDOME.nfsc into DIR")
    ap.add_argument("--time", choices=sorted(SKY_SETS), default="night",
                    help="which time-of-day set to bake (default night)")
    ap.add_argument("--maxdim", type=int, default=512,
                    help="downscale the sky textures to at most this many "
                         "texels on a side (default 512 -- the dome is "
                         "permanently resident, but it is also the whole "
                         "upper half of the frame, so it is worth the 256 KB)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    bun = os.path.join(args.track_dir, "%s.BUN" % args.track)
    stream_path = os.path.join(args.track_dir, "STREAM%s.BUN" % args.track)
    sections = track_pack.read_section_table(bun)

    want = track_pack.bstring_hash(SKY_MODEL)
    with open(stream_path, "rb") as stream:
        sec, data, part = find_model(stream, sections, want)
    if part is None:
        sys.exit("no %s (0x%08x) in %s" % (SKY_MODEL, want, bun))
    print("%s found in %s: %d verts, %d tris, %d subsets"
          % (SKY_MODEL, sec.name, part["mesh"]["numVerts"],
             part["mesh"]["numTris"], part["mesh"]["numSubsets"]))

    verts, indices, subsets = track_pack.read_track_mesh(data, part)

    # Unit horizontal radius, base at z = 0, so the runtime can scale the dome
    # to whatever its far plane allows and sit the horizon at eye level.
    radius = max(1e-6, max((v[0] * v[0] + v[1] * v[1]) ** 0.5 for v in verts))
    zmin = min(v[2] for v in verts)
    height = max(v[2] for v in verts) - zmin
    scaled = []
    bbmin = [1e30] * 3
    bbmax = [-1e30] * 3
    for (x, y, z, cr, cg, cb, u, v) in verts:
        p = (x / radius, y / radius, (z - zmin) / radius)
        for i in range(3):
            bbmin[i] = min(bbmin[i], p[i])
            bbmax[i] = max(bbmax[i], p[i])
        # V negated for the PICA200's flipped texture V axis (see car_pack).
        scaled.append((p[0], p[1], p[2], cr, cg, cb, u, -v))
    print("authored radius %.0f m, height %.0f m -> unit dome, %d verts"
          % (radius, height, len(scaled)))

    dyntex = load_dyntex(os.path.join(args.track_dir, DYNTEX_NAME),
                         os.path.join(os.path.dirname(
                             os.path.abspath(args.track_dir)), "SPEED2.EXE"))
    band_name, cap_name = SKY_SETS[args.time]

    # The dome's slots name the sunrise pair; substitute the chosen set by
    # role, band for band and cap for cap, keeping the authored hashes as the
    # table keys so each batch still resolves through its own slot. The roles
    # are matched by hashing the sunrise names rather than by slot order --
    # the dome happens to list its cap first.
    roles = {track_pack.bstring_hash(n): i
             for i, n in enumerate(SKY_SETS["sunrise"])}
    picks = []
    for th in part["textures"]:
        if not th:
            continue
        if th not in roles:
            sys.exit("%s slot 0x%08x is not one of the sky textures" %
                     (SKY_MODEL, th))
        role = roles[th]
        src = SKY_SETS["sunrise"][role]
        pick = (band_name, cap_name)[role]
        if pick not in dyntex:
            sys.exit("%s has no %s" % (DYNTEX_NAME, pick))
        picks.append((th, pick, src))

    tex_entries, tex_blob = [], bytearray()
    tex_index = {}
    for th, pick, src in picks:
        w, h, rgba = dyntex[pick]
        print("  slot 0x%08x (%s) <- %s %dx%d" % (th, src, pick, w, h))
        if pick == band_name:
            print("  horizon colour: #%02x%02x%02x" % horizon_rgb(rgba, w, h))
        rgba, w, h = car_pack.box_downscale(rgba, w, h, args.maxdim)
        fmt, pixels = car_pack.encode_texture(rgba, w, h)
        tex_index[th] = len(tex_entries)
        tex_entries.append((th, w, h, fmt, len(tex_blob)))
        tex_blob.extend(pixels)

    batches = []
    for (first_index, index_count, tex_slot, flags) in subsets:
        th = part["textures"][tex_slot] if tex_slot < len(part["textures"]) else 0
        batches.append((first_index, index_count, tex_index.get(th, -1), flags))
    batches, indices = car_pack.merge_batches(batches, indices)

    blob = car_pack.pack_nfsc(tex_entries, tex_blob, batches, scaled, indices,
                              bbmin, bbmax, car_pack.SHADING_BAKED)
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, SKY_NAME)
    with open(path, "wb") as fh:
        fh.write(blob)
    print("wrote %s: %d verts, %d indices, %d batches, %d textures, %d B"
          % (path, len(scaled), len(indices), len(batches), len(tex_entries),
             len(blob)))


if __name__ == "__main__":
    main()
