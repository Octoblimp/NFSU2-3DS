#!/usr/bin/env python3
"""
GEOMETRY.BIN / STREAML4R*.BUN -> single flat runtime binary "scene" file.

Replaces the per-mesh JSON intermediate from geometry_transcode.py with one
file the 3DS-side C loader can read directly with no parsing beyond struct
offsets. Both source FVFs (car 36-byte XYZ+normal+color+uv, scenery 24-byte
XYZ+color+uv -- see docs/formats/geometry-bin.md) are normalized here into a
single shared 36-byte runtime vertex layout:

    struct RuntimeVertex {
        float pos[3];
        float uv[2];
        float rgba[4];   // unpacked from D3DCOLOR (ARGB8), 0..1 floats
    };

File layout, all little-endian:
    char magic[4]   = "NFSM"
    u32  version    = 1
    u32  num_meshes
    MeshEntry[num_meshes]:
        u32 vertex_offset   // into the vertex pool, in vertices (not bytes)
        u32 vertex_count
        u32 index_offset    // into the index pool, in indices (not bytes)
        u32 index_count
    RuntimeVertex[total_vertex_count]
    uint16 index_pool[total_index_count]

Usage:
    python mesh_pack.py <GEOMETRY.BIN | STREAML4R*.BUN> <out.nfsm>
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from geometry_transcode import find_mesh_objects, extract_mesh  # noqa: E402

MAGIC = b"NFSM"
VERSION = 1


def _unpack_d3dcolor(color):
    a = (color >> 24) & 0xFF
    r = (color >> 16) & 0xFF
    g = (color >> 8) & 0xFF
    b = color & 0xFF
    return (r / 255.0, g / 255.0, b / 255.0, a / 255.0)


def pack_scene(data):
    meshes = find_mesh_objects(data)
    entries = []
    vertex_pool = bytearray()
    index_pool = bytearray()
    vcount_total = 0
    icount_total = 0
    ok = skipped = 0

    for vb_off, vb_size, ib_off, ib_size in meshes:
        mesh = extract_mesh(data, vb_off, vb_size, ib_off, ib_size)
        if mesh is None:
            skipped += 1
            continue
        verts = mesh["vertices"]
        indices = mesh["indices"]
        entries.append((vcount_total, len(verts), icount_total, len(indices)))
        for v in verts:
            r, g, b, a = _unpack_d3dcolor(v["color"])
            vertex_pool += struct.pack("<9f", *v["pos"], *v["uv"], r, g, b, a)
        for idx in indices:
            index_pool += struct.pack("<H", idx)
        vcount_total += len(verts)
        icount_total += len(indices)
        ok += 1

    header = struct.pack("<4sII", MAGIC, VERSION, len(entries))
    table = b"".join(struct.pack("<IIII", *e) for e in entries)
    return header + table + bytes(vertex_pool) + bytes(index_pool), ok, skipped


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("geometry_file")
    ap.add_argument("out_file")
    args = ap.parse_args()

    with open(args.geometry_file, "rb") as f:
        data = f.read()

    packed, ok, skipped = pack_scene(data)
    with open(args.out_file, "wb") as f:
        f.write(packed)

    print(f"Packed {ok} meshes ({skipped} skipped) -> {args.out_file} "
          f"({len(packed)} bytes)")


if __name__ == "__main__":
    main()
