#!/usr/bin/env python3
"""
Proves the sub-section cull (TASKS.md #68) never drops a triangle it should
have drawn.

world_cullstudy.py measures what the cull SAVES. This measures what it must
never cost, which is the half a screenshot cannot settle: a building that only
disappears when the camera is at one particular angle looks exactly like a
building that was never there, and driving past it once proves nothing.

The invariant, which is the whole reason retail's three-state test is safe to
copy: a box that is outside the frustum contains nothing that is inside it. So
for every camera, the triangles the cell cull keeps must be a SUPERSET of the
triangles an exact per-triangle test keeps. Anything else is a hole in the
world. The reference test is each triangle's own AABB against the same four
planes -- itself conservative, so a triangle counted visible here may still be
off screen, which only makes the superset requirement harder to meet, not
easier.

The cell table is built by world_cullstudy.batch_cells(), the same routine
whose output that script reports and which mirrors model3d.c's builder
(centroid binning into XY squares, the box then grown to hold each triangle
whole, CULL_MIN_TRIS batches left as one cell). What this checks is therefore
the algorithm and its constants, not the C compiler's rendering of them --
the run on Azahar is what covers that.

Usage:
    python tools/world_cullverify.py <TRACKS/L4RA dir> [--sections 12]

The loose sections are deleted from the SD once WORLD.NFSZ is packed
(world_archive.py --prune), so write them to a scratch directory first:
    python tools/track_pack.py "PC Files/TRACKS" L4RA --maxdim 256 --out <dir>
"""
import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from world_cullstudy import (load_index, load_nfsc, batch_cells, frustum,
                             boxes_state)


def tri_boxes(m, b):
    """Every triangle of batch b as (lo, hi), in the index pool's own order."""
    first, count = int(m["batches"][b][0]), int(m["batches"][b][1])
    if count < 3 or first + count > m["idx"].shape[0]:
        return None
    tri = m["idx"][first:first + count // 3 * 3].reshape(-1, 3).astype(np.int32)
    p = m["verts"][tri]
    return p.min(axis=1), p.max(axis=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--cell", type=float, default=48.0,
                    help="model3d.c's CULL_CELL")
    ap.add_argument("--min-tris", type=int, default=192,
                    help="model3d.c's CULL_MIN_TRIS")
    ap.add_argument("--max-side", type=int, default=16,
                    help="model3d.c's CULL_MAX_SIDE")
    ap.add_argument("--fov", type=float, default=55.0)
    ap.add_argument("--aspect", type=float, default=400.0 / 240.0)
    ap.add_argument("--yaws", type=int, default=16)
    ap.add_argument("--sections", type=int, default=12,
                    help="how many of the biggest sections to check")
    ap.add_argument("--eye-z", type=float, default=2.7)
    args = ap.parse_args()

    secs = load_index(args.dir)
    # The biggest sections on disk, because they are the ones the cull actually
    # cuts: a batch under CULL_MIN_TRIS is one cell and can only ever be kept
    # whole. The index carries no size, so the .nfsc on disk is the measure.
    def nfsc_size(s):
        p = os.path.join(args.dir, s["name"] + ".nfsc")
        return os.path.getsize(p) if os.path.exists(p) else 0
    secs = [s for s in secs if nfsc_size(s)]
    secs = sorted(secs, key=lambda s: -nfsc_size(s))[:args.sections]

    half_x = math.radians(args.fov) * 0.5
    half_y = math.atan(math.tan(half_x) / args.aspect)

    totalTris = 0
    keptTris = 0
    visTris = 0
    lost = 0
    worstSec = None
    checked = 0

    for s in secs:
        path = os.path.join(args.dir, s["name"] + ".nfsc")
        if not os.path.exists(path):
            continue
        m = load_nfsc(path)
        cells = batch_cells(m, args.cell, args.min_tris, args.max_side)
        tb = [tri_boxes(m, b) for b in range(m["nBatch"])]

        # Cameras inside the section's own box, which is where a straddling
        # batch actually happens -- a camera far outside sees whole batches in
        # or out and exercises nothing.
        lo, hi = np.array(s["bbmin"]), np.array(s["bbmax"])
        eyes = []
        for fx in (0.25, 0.5, 0.75):
            for fy in (0.25, 0.5, 0.75):
                eyes.append([lo[0] + (hi[0] - lo[0]) * fx,
                             lo[1] + (hi[1] - lo[1]) * fy,
                             lo[2] + args.eye_z])
        for eye in eyes:
            for yi in range(args.yaws):
                a = 2.0 * math.pi * yi / args.yaws
                look = [eye[0] + math.cos(a), eye[1] + math.sin(a), eye[2]]
                planes = frustum(eye, look, half_x, half_y)
                checked += 1
                for b in range(m["nBatch"]):
                    if cells[b] is None or tb[b] is None:
                        continue
                    clo, chi, ccount, blo, bhi = cells[b]
                    tlo, thi = tb[b]
                    totalTris += tlo.shape[0]
                    # Exactly what model3d.c's draw loop decides, in the same
                    # order: the batch's own box first (state 0 skips it
                    # whole, state 2 draws it whole with no cell tests), then
                    # the cells of a straddling one.
                    bstate = int(boxes_state(planes, blo[None, :], bhi[None, :])[0])
                    if bstate == 0:
                        keep = np.zeros(tlo.shape[0], bool)
                    elif bstate == 2:
                        keep = np.ones(tlo.shape[0], bool)
                    else:
                        cstate = boxes_state(planes, clo, chi) != 0
                        # Cells are emitted in key order and the triangles were
                        # sorted into that order, so cell i owns the i'th run.
                        keep = np.repeat(cstate, ccount)
                    # The reference: the triangle's own box against the same
                    # planes. batch_cells() sorted its triangles, so compare in
                    # that same sorted order.
                    if bstate != 2:
                        order = cell_order(m, b, args)
                        vis = boxes_state(planes, tlo[order], thi[order]) != 0
                    else:
                        vis = boxes_state(planes, tlo, thi) != 0
                    visTris += int(vis.sum())
                    keptTris += int(keep.sum())
                    missed = int(np.count_nonzero(vis & ~keep))
                    if missed:
                        lost += missed
                        if worstSec is None:
                            worstSec = (s["name"], b, eye, a, missed)

    print("sections %d, cameras %d, triangle tests %d"
          % (len(secs), checked, totalTris))
    print("  visible by an exact per-triangle test : %d" % visTris)
    print("  kept by the cell cull                 : %d (%.1f%% of all)"
          % (keptTris, 100.0 * keptTris / max(totalTris, 1)))
    if lost:
        print("  FAIL: %d visible triangle(s) dropped; first at %s batch %d "
              "eye %s yaw %.0f deg (%d)"
              % (lost, worstSec[0], worstSec[1],
                 ["%.0f" % v for v in worstSec[2]],
                 math.degrees(worstSec[3]), worstSec[4]))
        return 1
    print("  PASS: every visible triangle was kept (the cull is conservative)")
    return 0


def cell_order(m, b, args):
    """The permutation batch_cells() applies, so the reference lines up.

    model3d.c sorts the index pool itself, so at runtime the two are already
    in the same order; here the sort is only inside batch_cells(), and
    comparing an unsorted reference against a sorted keep mask would report
    losses that do not exist.
    """
    first, count = int(m["batches"][b][0]), int(m["batches"][b][1])
    tri = m["idx"][first:first + count // 3 * 3].reshape(-1, 3).astype(np.int32)
    p = m["verts"][tri]
    lo, hi, c = p.min(axis=1), p.max(axis=1), p.mean(axis=1)
    blo, bhi = lo.min(axis=0), hi.max(axis=0)
    if tri.shape[0] < args.min_tris:
        return np.arange(tri.shape[0])
    step = max(args.cell, (bhi[0] - blo[0]) / args.max_side,
               (bhi[1] - blo[1]) / args.max_side)
    gx = np.floor((c[:, 0] - blo[0]) / step).astype(np.int64)
    gy = np.floor((c[:, 1] - blo[1]) / step).astype(np.int64)
    return np.argsort(gx * (args.max_side + 2) + gy, kind="stable")


if __name__ == "__main__":
    sys.exit(main())
