#!/usr/bin/env python3
"""Check a car skin the 3DS composited against vinyl_pack.compose() (TASKS.md #296).

Run the game with `DbgDumpSkin=1;` in the config. Each car skin carskin.c
builds is then written raw to sdmc:/nfsu2_skin.bin (0xAARRGGBB texels) and
what it was built from goes to the log as "skin dump:" lines. This rebuilds
the last one dumped from the same .nfsv and the PC install's paint table,
counts the texels that differ, and saves the 3DS skin as a PNG.

Usage:
    python tools/skin_dump_check.py "<sd>" skin.png [--pc "PC Files"]

<sd> is the card's root, the folder holding nfsu2_debug.txt, nfsu2_skin.bin
and nfsu2data/.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
import paint_pack as pp  # noqa: E402
import vinyl_pack as vp  # noqa: E402
from PIL import Image  # noqa: E402
from track_pack import bstring_hash  # noqa: E402

# carskin.c's FALLBACK_PAINT, for a car with no BASE_PAINT (PORT).
FALLBACK = 0x001C4696
# What an empty VINYL_COLOUR slot remaps to, per remap colour.
REMAP_DEFAULT = (0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sd")
    ap.add_argument("png")
    ap.add_argument("--pc", default="PC Files")
    args = ap.parse_args()

    log = open(os.path.join(args.sd, "nfsu2_debug.txt"), encoding="utf-8",
               errors="replace").read()
    head = re.findall(r"skin dump: (\S+) (\d+)x(\d+) (\S+), base ([0-9a-f]+) "
                      r"doorline '([^']*)'", log)
    if not head:
        raise SystemExit("no skin dump in the log")
    car, w, h, state, base_part, doorline = head[-1]
    w, h = int(w), int(h)
    regions_p = [int(x, 16) for x in re.findall(
        r"skin dump: regions ((?:[0-9a-f]+ ?){7})", log)[-1].split()]
    vin = re.findall(r"skin dump: vinyl (\d) ([0-9a-f]+) colours ([0-9a-f]+) "
                     r"([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)", log)[-4:]
    print("dump:", car, w, h, state, "base", base_part, "doorline", repr(doorline))

    data = open(os.path.join(args.pc, pp.GLOBAL_BUN), "rb").read()
    table = pp.paint_table(pp.read_parts(data))
    nfsv = vp.Nfsv(os.path.join(args.sd, "nfsu2data", "CARS", car + ".nfsv"))
    bp = int(base_part, 16)
    base = vp.argb_of(table, bp, FALLBACK) if bp else FALLBACK
    regions = [vp.argb_of(table, p, base) if p else base for p in regions_p]

    layers = []
    parts = [int(v[1], 16) for v in vin]
    for k, v in enumerate(vin):
        ph = parts[k]
        if not ph or ph in parts[:k]:
            continue
        if ph not in nfsv.parts or not nfsv.parts[ph][1]:
            print("vinyl %08x: no NUMCOLOURS, left off" % ph)
            continue
        th, _numc, remap = nfsv.parts[ph]
        rc = None
        if remap:
            rc = list(REMAP_DEFAULT)
            for j in range(4):
                c = int(v[2 + j], 16)
                if c:
                    rc[j] = vp.argb_of(table, c, 0)
        if th in nfsv.tex and vp.kp.ext(th, "_MASK") in nfsv.tex:
            layers.append((th, rc))
    dl = bstring_hash(car + "_DOORLINE" + doorline)
    if dl in nfsv.tex:
        layers.append((dl, None))

    ref = vp.compose(nfsv, base, regions, layers)
    got = np.fromfile(os.path.join(args.sd, "nfsu2_skin.bin"), "<u4")
    print("layers", len(layers), "texels", got.size, "differ", int((got != ref).sum()))
    for j in np.nonzero(got != ref)[0][:8]:
        print("  texel %d (x %d y %d): 3ds %08x ref %08x" % (j, j % w, j // w, got[j], ref[j]))
    a = got.reshape(h, w)
    rgb = np.stack([(a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF], axis=-1).astype(np.uint8)
    Image.fromarray(rgb).save(args.png)
    print("wrote", args.png)


if __name__ == "__main__":
    main()
