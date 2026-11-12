#!/usr/bin/env python3
"""
What a sub-section frustum cull would actually save in the world3d slot.

TASKS.md #68 says to port the GameCube build's scenery culling rather than
invent one, and Speed.elf's answer is a per-section tree
(ScenerySectionHeader::TreeCull, 0x80209724) whose leaves are single scenery
instances, each tested with its own centre/extent box by
eViewPlatInterface::GetVisibleStateSB (0x801e9608) -- a 6-plane test returning
0 outside / 1 straddling / 2 fully inside, where state 2 is inherited by a
node's children so they are never tested again.

The port cannot copy that leaf granularity directly: tools/track_pack.py merges
a section's subsets by (texture, flags) so a section is tens of draw calls
instead of "over a thousand", and a merged batch's box is therefore the whole
section's box -- a per-batch test against it would never reject anything.

The middle ground is retail's tree with coarser leaves and one level instead of
many: cut each merged batch's triangles into a grid of XY cells, give each cell
its own box, and coalesce neighbouring kept cells back into one draw call
because they are contiguous in the index pool. Retail's three states carry over
exactly -- a batch whose box is outside costs one test, one fully inside is
drawn whole with no cell tests at all, and only a straddling batch walks its
cells.

This script measures what that would buy, straight out of the packed
TRACKS/<code>/*.nfsc and WORLD.NFSX the runtime actually reads, so the decision
rests on the shipped data rather than on an estimate. It reproduces world.c's
own draw rules: the DRAW_RADIUS 400 m XY test against the section's draw box,
the four-plane frustum test, and the LOD_SWITCH 40 m near/far choice
(SECTION_FAR sections are always far). Retail's visibility lists (visHidden)
are left out -- they only remove whole sections, equally from both columns.

Usage:
    python tools/world_cullstudy.py <TRACKS/L4RA dir> [--cell 64] [--min-tris 192]

The loose sections are deleted from the SD once WORLD.NFSZ is packed
(world_archive.py --prune), so write them to a scratch directory first:
    python tools/track_pack.py "PC Files/TRACKS" L4RA --maxdim 256 --out <dir>
"""
import argparse
import math
import os
import struct
import sys

import numpy as np

WORLD_INDEX = "WORLD.NFSX"
WORLD_MAGIC = b"NFSW"
WORLD_HDR = 0x40
WORLD_REC = 0x60
SECTION_FAR = 1

# world.c's own constants.
DRAW_RADIUS = 400.0
LOD_SWITCH = 40.0

NFSC_MAGIC = b"NFSC"
NFSC_HDR = 0x40
TEX_ENTRY = 16
BATCH_ENTRY = 16
VERTEX_STRIDE = 32


def load_index(dirpath):
    with open(os.path.join(dirpath, WORLD_INDEX), "rb") as f:
        blob = f.read()
    if blob[:4] != WORLD_MAGIC:
        raise SystemExit("not a WORLD.NFSX")
    n, rec = struct.unpack_from("<II", blob, 8)
    if rec != WORLD_REC:
        raise SystemExit("unexpected record size 0x%x" % rec)
    out = []
    for i in range(n):
        r = WORLD_HDR + i * rec
        name = blob[r:r + 16].split(b"\0")[0].decode("ascii", "replace")
        out.append({
            "name": name,
            "bbmin": np.array(struct.unpack_from("<3f", blob, r + 0x10), np.float64),
            "bbmax": np.array(struct.unpack_from("<3f", blob, r + 0x1c), np.float64),
            "coremin": np.array(struct.unpack_from("<3f", blob, r + 0x40), np.float64),
            "coremax": np.array(struct.unpack_from("<3f", blob, r + 0x4c), np.float64),
            "flags": struct.unpack_from("<I", blob, r + 0x58)[0],
        })
    return out


def load_nfsc(path):
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:4] != NFSC_MAGIC:
        raise SystemExit("%s is not a .nfsc" % path)
    (ver, nTex, nBatch, nVert, nIdx) = struct.unpack_from("<5I", blob, 4)
    lodSplit = struct.unpack_from("<I", blob, 0x3c)[0]
    off = NFSC_HDR + nTex * TEX_ENTRY
    batches = np.array(np.frombuffer(blob, "<u4", count=nBatch * 4,
                                     offset=off).reshape(nBatch, 4))
    off += nBatch * BATCH_ENTRY
    verts = np.frombuffer(blob, "<f4", count=nVert * 8,
                          offset=off).reshape(nVert, 8)[:, 0:3].astype(np.float32)
    off += nVert * VERTEX_STRIDE
    idx = np.array(np.frombuffer(blob, "<u2", count=nIdx, offset=off))
    nearFirst = (lodSplit & 0xFFFF) - 1 if lodSplit else nBatch
    farFirst = (lodSplit >> 16) - 1 if lodSplit else nBatch
    return {"ver": ver, "batches": batches, "verts": verts, "idx": idx,
            "nearFirst": nearFirst, "farFirst": farFirst, "nBatch": nBatch}


def batch_cells(m, cell, min_tris, max_side):
    """Per batch: (cellLo, cellHi, cellTris, batchLo, batchHi), or None.

    Cells are keyed on the triangle centroid's XY grid square and emitted in
    key order, so neighbouring cells stay adjacent in the index pool -- which
    is what lets a kept run coalesce into one draw call. A batch under
    `min_tris` triangles is left as a single cell: splitting it would cost more
    table than the triangles are worth, and it is what keeps a skyline shell
    whose few triangles are kilometres apart from becoming one cell each.

    The grid square is at least `cell` metres and is widened so no batch has
    more than `max_side` of them across, which bounds the table on the same
    sections.
    """
    out = []
    v = m["verts"]
    idx = m["idx"]
    for b in range(m["nBatch"]):
        first, count = int(m["batches"][b][0]), int(m["batches"][b][1])
        if count < 3 or first + count > idx.shape[0]:
            out.append(None)
            continue
        tri = idx[first:first + count // 3 * 3].reshape(-1, 3).astype(np.int32)
        p = v[tri]                      # (t,3,3)
        lo, hi, c = p.min(axis=1), p.max(axis=1), p.mean(axis=1)
        blo, bhi = lo.min(axis=0), hi.max(axis=0)
        if tri.shape[0] < min_tris:
            out.append((blo[None, :], bhi[None, :],
                        np.array([tri.shape[0]], np.int64), blo, bhi))
            continue
        step = max(cell, (bhi[0] - blo[0]) / max_side, (bhi[1] - blo[1]) / max_side)
        gx = np.floor((c[:, 0] - blo[0]) / step).astype(np.int64)
        gy = np.floor((c[:, 1] - blo[1]) / step).astype(np.int64)
        key = gx * (max_side + 2) + gy
        order = np.argsort(key, kind="stable")
        key, lo, hi = key[order], lo[order], hi[order]
        edge = np.flatnonzero(np.diff(key)) + 1
        starts = np.concatenate(([0], edge))
        ends = np.concatenate((edge, [key.shape[0]]))
        clo = np.stack([lo[s:e].min(axis=0) for s, e in zip(starts, ends)])
        chi = np.stack([hi[s:e].max(axis=0) for s, e in zip(starts, ends)])
        out.append((clo, chi, (ends - starts).astype(np.int64), blo, bhi))
    return out


def frustum(eye, look, half_x, half_y):
    """The four side planes, in world space, as (4,4) [nx,ny,nz,d] rows.

    The same four planes world.c pulls out of the view-projection matrix; built
    here from the angles instead, because the study only needs their shape.
    """
    f = np.array(look, np.float64) - np.array(eye, np.float64)
    f /= np.linalg.norm(f)
    r = np.cross(f, np.array([0.0, 0.0, 1.0]))
    r /= np.linalg.norm(r)
    u = np.cross(r, f)
    planes = []
    for n in (f * math.tan(half_x) + r, f * math.tan(half_x) - r,
              f * math.tan(half_y) + u, f * math.tan(half_y) - u):
        n = n / np.linalg.norm(n)
        planes.append(np.concatenate((n, [-np.dot(n, eye)])))
    return np.array(planes)


def boxes_state(planes, lo, hi):
    """GetVisibleStateSB, vectorised: 0 outside, 1 straddling, 2 fully inside.

    Centre and half-extent rather than lo/hi, the way the GameCube build writes
    it: `r` is the box's radius along the plane normal and `d` the centre's
    signed distance, so d+r < 0 is outside and d-r < 0 is a straddle.
    """
    c = (lo + hi) * 0.5
    e = (hi - lo) * 0.5
    state = np.full(lo.shape[0], 2, np.int8)
    for p in planes:
        d = c[:, 0] * p[0] + c[:, 1] * p[1] + c[:, 2] * p[2] + p[3]
        r = e[:, 0] * abs(p[0]) + e[:, 1] * abs(p[1]) + e[:, 2] * abs(p[2])
        state = np.where(d + r < 0.0, 0, np.where(d - r < 0.0,
                                                  np.minimum(state, 1), state))
    return state


def dist2_xy(lo, hi, pos):
    dx = max(lo[0] - pos[0], 0.0, pos[0] - hi[0])
    dy = max(lo[1] - pos[1], 0.0, pos[1] - hi[1])
    return dx * dx + dy * dy


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--cell", type=float, default=64.0)
    ap.add_argument("--min-tris", type=int, default=192)
    ap.add_argument("--max-side", type=int, default=16)
    ap.add_argument("--fov", type=float, default=55.0,
                    help="vertical FOV in degrees, as main.c's WORLD_FOV")
    ap.add_argument("--aspect", type=float, default=400.0 / 240.0)
    ap.add_argument("--yaws", type=int, default=8)
    ap.add_argument("--eye-z", type=float, default=2.7)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    half_y = math.radians(args.fov) * 0.5
    half_x = math.atan(math.tan(half_y) * args.aspect)
    secs = load_index(args.dir)

    models, cells = {}, {}

    def get(sec):
        n = sec["name"]
        if n not in models:
            p = os.path.join(args.dir, n + ".nfsc")
            if not os.path.exists(p):
                models[n] = cells[n] = None
            else:
                models[n] = load_nfsc(p)
                cells[n] = batch_cells(models[n], args.cell, args.min_tris,
                                       args.max_side)
                # The pools are only needed to build the cells; dropping them
                # keeps a whole-track sweep inside a sane amount of memory.
                models[n]["verts"] = models[n]["idx"] = None
        return models[n], cells[n]

    spots = [s for s in secs if not (s["flags"] & SECTION_FAR)]
    if args.limit:
        spots = spots[:args.limit]

    tot = np.zeros(5, np.float64)
    samples = 0
    for spot in spots:
        eye = np.array([(spot["coremin"][0] + spot["coremax"][0]) * 0.5,
                        (spot["coremin"][1] + spot["coremax"][1]) * 0.5,
                        spot["coremin"][2] + args.eye_z])
        for k in range(args.yaws):
            a = 2.0 * math.pi * k / args.yaws
            look = eye + np.array([math.cos(a), math.sin(a), 0.0]) * 50.0
            pl = frustum(eye, look, half_x, half_y)
            now_t = now_d = cut_t = cut_d = tests = 0
            for s in secs:
                far_sec = bool(s["flags"] & SECTION_FAR)
                d2 = dist2_xy(s["bbmin"], s["bbmax"], eye)
                if not far_sec and d2 > DRAW_RADIUS * DRAW_RADIUS:
                    continue
                if boxes_state(pl, s["bbmin"][None, :], s["bbmax"][None, :])[0] == 0:
                    continue
                m, cl = get(s)
                if m is None:
                    continue
                far = far_sec or d2 > LOD_SWITCH * LOD_SWITCH
                for b in range(m["nBatch"]):
                    if (not (b < m["farFirst"])) if not far else \
                            (m["nearFirst"] <= b < m["farFirst"]):
                        continue
                    g = cl[b]
                    if g is None:
                        continue
                    clo, chi, cnt, blo, bhi = g
                    now_t += int(cnt.sum())
                    now_d += 1
                    tests += 1
                    st = boxes_state(pl, blo[None, :], bhi[None, :])[0]
                    if st == 0:
                        continue
                    if st == 2 or clo.shape[0] == 1:
                        cut_t += int(cnt.sum())
                        cut_d += 1
                        continue
                    tests += clo.shape[0]
                    keep = boxes_state(pl, clo, chi) != 0
                    if not keep.any():
                        continue
                    cut_t += int(cnt[keep].sum())
                    cut_d += int(np.count_nonzero(np.diff(np.concatenate(
                        ([False], keep, [False])).astype(np.int8)) > 0))
            tot += (now_t, now_d, cut_t, cut_d, tests)
            samples += 1

    now_t, now_d, cut_t, cut_d, tests = tot / max(samples, 1)
    groups = sum(sum(0 if g is None else g[2].shape[0] for g in c)
                 for c in cells.values() if c)
    nsec = len([c for c in cells.values() if c])
    print("%d sections, cell %.0f m, min %d tris, max %d cells a side, "
          "FOV %.0f x %.0f deg" % (len(secs), args.cell, args.min_tris,
                                   args.max_side, math.degrees(half_x) * 2,
                                   math.degrees(half_y) * 2))
    print("%d camera samples (%d spots x %d yaws)" % (samples, len(spots), args.yaws))
    print("  as drawn today : %9.0f tris  %6.0f draw calls" % (now_t, now_d))
    print("  with cells     : %9.0f tris  %6.0f draw calls  %5.0f box tests"
          % (cut_t, cut_d, tests))
    if now_t:
        print("  change         : %+8.1f%% tris  %+6.1f%% draw calls"
              % (100.0 * (cut_t - now_t) / now_t,
                 100.0 * (cut_d - now_d) / max(now_d, 1)))
    if nsec:
        print("  cell table     : %.0f cells a section, %.1f KB at 32 B each"
              % (groups / nsec, groups / nsec * 32 / 1024.0))


if __name__ == "__main__":
    main()
