#!/usr/bin/env python3
"""
GEOMETRY.BIN -> simple runtime mesh extractor for NFSU2 TEXTURES/GEOMETRY.

Walks a GEOMETRY.BIN chunk stream (see docs/formats/bin-archive.md for the
generic bChunkHeader{type,size} format) and, for every 0x134900 mesh-object
container, extracts:

  - 0x134b01 leaf: vertex buffer, one record per vertex:
        struct Vertex { float pos[3]; float normal[3]; uint32 color; float uv[2]; };
    (36 bytes/vertex -- the classic D3D8 FVF
     D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1, confirmed empirically,
     see docs/formats/geometry-bin.md "Session 7 (cont.)")
  - 0x134b03 leaf: index buffer, raw uint16 indices (confirmed in
    FUN_005cc280's decompile: puVar8[7] holds a byte size, halved for index
    count elsewhere in the loader)

Track/scenery streaming data (TRACKS/STREAML4R*.BUN) reuses the exact same 0x134900/
0x134b01/0x134b03 chunk types as car GEOMETRY.BIN, but with a *different*, simpler FVF:
36-byte stride doesn't evenly divide these vertex buffers. Empirically decoded (see
HANDOFF.md) as a 24-byte D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1 record (no per-vertex
normal -- scenery lighting is presumably baked into the vertex color/lightmap instead):
    struct SceneryVertex { float pos[3]; uint32 color; float uv[2]; };
extract_mesh() tries the car stride (36) first, falling back to the scenery stride (24)
if the buffer size isn't evenly divisible by 36.

The loader 128-byte-aligns the vertex buffer's *runtime* pointer before use
(FUN_005cc280: `(int)piVar1 + 0x87U & 0xffffff80`), which introduces up to 127
bytes of leading padding inside the raw file leaf that isn't part of the
vertex data. That padding is a run of 0x11 filler bytes counted from the start
of the leaf payload up to the next 128-byte-aligned *file* offset -- this
happens to reproduce the runtime alignment because the file layout tool chain
apparently preserves the same alignment relationship. We skip it the same way:
scan forward from the payload start until a 128-byte-aligned offset is hit.
"""
import argparse
import json
import os
import struct

VERTEX_STRIDE_CAR = 36
VERTEX_STRIDE_SCENERY = 24


def find_leaves(data, base, end, wanted_types, depth=0):
    off = base
    out = []
    while off + 8 <= end:
        ttype, tsize = struct.unpack_from("<II", data, off)
        payload = off + 8
        if tsize == 0 and ttype == 0:
            off += 8
            continue
        if payload + tsize > end:
            break
        is_container = bool(ttype & 0x80000000)
        if ttype in wanted_types:
            out.append((off, ttype, tsize, payload))
        if is_container:
            out.extend(find_leaves(data, payload, payload + tsize, wanted_types, depth + 1))
        off = payload + tsize
    return out


def skip_alignment_padding(data, payload_start, leaf_size):
    """Skip a leading run of 0x11 filler bytes. For car GEOMETRY.BIN vertex buffers this
    run happens to always land exactly on the next 128-byte-aligned file offset (the
    runtime alignment the loader applies to the vertex pointer), but track/scenery
    STREAM*.BUN leaves use shorter, not-128-aligned filler runs -- so just measure the
    actual run length directly rather than assuming a fixed alignment target."""
    i = 0
    while i < leaf_size and data[payload_start + i] == 0x11:
        i += 1
    return payload_start + i


def leaf_start(data, payload_start, leaf_size, stride):
    """Like skip_alignment_padding(), but for a leaf of fixed-size records.

    The filler is a run of 0x11 bytes, and a record whose own first byte is
    0x11 is indistinguishable from one more filler byte -- so the run-length
    scan happily eats it and shifts every field in the leaf by one. That is not
    hypothetical: L4RA's terrain parts are full of subset records that start
    0x11 (the low byte of a float), and the shift turned their texture slot
    into 0xff000000 and their index count into a billion, which is what made
    ~11% of the converted world draw untextured.

    The records are fixed-stride and the leaf holds a whole number of them, so
    the true filler length is congruent to the leaf size modulo the stride.
    Take the longest run the scan allows that still satisfies that -- which is
    the run itself whenever the scan did not overshoot, so this only ever
    corrects a leaf it was getting wrong.
    """
    i = skip_alignment_padding(data, payload_start, leaf_size) - payload_start
    if stride > 1:
        i -= (i - leaf_size) % stride
        if i < 0:
            i = 0
    return payload_start + i


def find_mesh_objects(data):
    """Yield (vb_offset, vb_size, ib_offset, ib_size) for every mesh object.

    0x134900/0x134b01/0x134b02/0x134b03 are *flat siblings* within the same
    parent container, not nested -- FUN_005cc280 walks one container's child
    stream in a single loop, starting a new mesh object each time it sees
    0x134900 and attaching whichever 0x134b01/b02/b03 leaves follow (until the
    next 0x134900) to that object. So: do one in-order (pre-order) traversal
    of the whole file collecting every occurrence of these four types in
    document order, then group each 0x134900 with the 0x134b01/0x134b03
    leaves that immediately follow it before the next 0x134900.
    """
    hits = find_leaves(data, 0, len(data), {0x134900, 0x134b01, 0x134b02, 0x134b03})
    hits.sort(key=lambda h: h[0])
    results = []
    vb = ib = None
    for off, ttype, tsize, payload in hits:
        if ttype == 0x134900:
            if vb and ib:
                results.append((vb[0], vb[1], ib[0], ib[1]))
            vb = ib = None
        elif ttype == 0x134b01:
            vb = (payload, tsize)
        elif ttype == 0x134b03:
            ib = (payload, tsize)
    if vb and ib:
        results.append((vb[0], vb[1], ib[0], ib[1]))
    return results


def _decode_verts_car(data, vstart, nverts):
    verts = []
    for i in range(nverts):
        rec = data[vstart + i * VERTEX_STRIDE_CAR: vstart + (i + 1) * VERTEX_STRIDE_CAR]
        x, y, z, nx, ny, nz = struct.unpack_from("<6f", rec, 0)
        color = struct.unpack_from("<I", rec, 24)[0]
        u, v = struct.unpack_from("<2f", rec, 28)
        verts.append({"pos": [x, y, z], "normal": [nx, ny, nz], "color": color, "uv": [u, v]})
    return verts


def _decode_verts_scenery(data, vstart, nverts):
    verts = []
    for i in range(nverts):
        rec = data[vstart + i * VERTEX_STRIDE_SCENERY: vstart + (i + 1) * VERTEX_STRIDE_SCENERY]
        x, y, z = struct.unpack_from("<3f", rec, 0)
        color = struct.unpack_from("<I", rec, 12)[0]
        u, v = struct.unpack_from("<2f", rec, 16)
        verts.append({"pos": [x, y, z], "normal": [0.0, 1.0, 0.0], "color": color, "uv": [u, v]})
    return verts


def _car_verts_sane(verts):
    """The car FVF's normal slot should hold a roughly-unit-length vector; if this data is
    actually scenery-FVF misread with the wrong (36-byte) stride, that slot lands on
    unrelated position/uv floats, giving wildly non-unit or non-finite magnitudes."""
    import math
    for v in verts:
        nx, ny, nz = v["normal"]
        if not all(math.isfinite(c) for c in (nx, ny, nz)):
            return False
        mag = math.sqrt(nx * nx + ny * ny + nz * nz)
        if not (0.1 < mag < 10.0):
            return False
    return True


def extract_mesh(data, vb_off, vb_size, ib_off, ib_size):
    vstart = skip_alignment_padding(data, vb_off, vb_size)
    vbytes = vb_size - (vstart - vb_off)

    verts = None
    if vbytes % VERTEX_STRIDE_CAR == 0:
        candidate = _decode_verts_car(data, vstart, vbytes // VERTEX_STRIDE_CAR)
        if _car_verts_sane(candidate):
            verts = candidate
    if verts is None and vbytes % VERTEX_STRIDE_SCENERY == 0:
        verts = _decode_verts_scenery(data, vstart, vbytes // VERTEX_STRIDE_SCENERY)
    if verts is None:
        return None

    istart = skip_alignment_padding(data, ib_off, ib_size)
    ibytes = ib_size - (istart - ib_off)
    if ibytes % 2 != 0:
        return None
    nidx = ibytes // 2
    indices = list(struct.unpack_from(f"<{nidx}H", data, istart))

    return {"vertices": verts, "indices": indices}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("geometry_bin")
    ap.add_argument("out_dir")
    ap.add_argument("--limit", type=int, default=None, help="only extract the first N meshes")
    args = ap.parse_args()

    with open(args.geometry_bin, "rb") as f:
        data = f.read()

    meshes = find_mesh_objects(data)
    os.makedirs(args.out_dir, exist_ok=True)
    ok, skipped = 0, 0
    for i, (vb_off, vb_size, ib_off, ib_size) in enumerate(meshes):
        if args.limit and i >= args.limit:
            break
        mesh = extract_mesh(data, vb_off, vb_size, ib_off, ib_size)
        if mesh is None:
            skipped += 1
            continue
        with open(os.path.join(args.out_dir, f"mesh_{i:04d}.json"), "w") as f:
            json.dump(mesh, f)
        ok += 1
    print(f"Found {len(meshes)} mesh objects: {ok} extracted, {skipped} skipped "
          f"(vertex buffer not evenly divisible by 36 or 24 bytes -- different FVF?)")


if __name__ == "__main__":
    main()
