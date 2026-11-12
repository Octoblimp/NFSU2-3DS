#!/usr/bin/env python3
"""
NFSU2 .BIN chunk-stream inspector/extractor.

Format (see docs/formats/bin-archive.md): a .BIN file is a raw stream of
bChunk records starting at file offset 0 -- there is no top-level file
header. Each record is:

    struct bChunkHeader { uint32_t type; uint32_t size; };  // little-endian on PC

If (type & 0x80000000) the chunk is a *container*: its `size`-byte payload is
itself a sequence of child bChunk records, tightly packed, no padding.
Otherwise it's a *leaf*: `size` bytes of raw subsystem-specific payload.
type == 0 is the null/terminator chunk type.

Usage:
    python bin_unpack.py <file.BIN>                 # print the chunk tree
    python bin_unpack.py <file.BIN> --extract DIR    # also dump every leaf's
                                                      # raw payload to DIR as
                                                      # <offset>_<type>.bin
"""
import argparse
import os
import struct

# Chunk-type -> short label, built empirically by walking real PC .BIN files
# across CARS/, TRACKS/, FRONTEND/, LANGUAGES/ (see scratchpad pc_bin_types.txt
# from the session that derived this). Not exhaustive -- extend as more types
# are identified.
KNOWN_TYPES = {
    0x00000000: "NULL/terminator",
    0x80134000: "GEOMETRY: top container",
    0x80034020: "GEOMETRY: sub container",
    0x00039200: "GEOMETRY: sub-record",
    0x00039201: "GEOMETRY: sub-record",
    0x00039202: "GEOMETRY: sub-record",
    0xb3300000: "TEXTURES: top container",
    0xb3310000: "TEXTURES: info container",
    0x33310001: "TEXTURES: info leaf",
    0x33310002: "TEXTURES: info leaf",
    0x33310003: "TEXTURES: info leaf",
    0x33310004: "TEXTURES: info leaf (envmap-related)",
    0x33310005: "TEXTURES: info leaf (envmap-related)",
    0xb3320000: "TEXTURES: pixel-data container",
    0x33320001: "TEXTURES: pixel-data leaf (small/header)",
    0x33320002: "TEXTURES: pixel-data leaf (concatenated JDLZ-compressed textures, see docs/formats/texture-tpk.md)",
    0x00e34010: "PARTS_ANIMATIONS: leaf",
    0x00037100: "PARTS_ANIMATIONS: leaf",
    0x80034147: "TRACKS/Paths: top container",
    0x00034148: "TRACKS/Paths: sub-record",
    0x00034149: "TRACKS/Paths: sub-record",
    0x0003414a: "TRACKS/Paths: sub-record",
    0x0003414c: "TRACKS/Paths: sub-record",
    0x0003414d: "TRACKS/Paths: sub-record",
    0x00034121: "TRACKS/Routes: record",
    0x00034122: "TRACKS/Routes: record",
    0x00034123: "TRACKS/Routes: record",
    0x00034146: "TRACKS/TrackPosMarkers: leaf",
    0x00039000: "LANGUAGES: string table leaf",
    0x00030201: "LANGUAGES: sub-record",
    0x00030230: "FRONTEND/MAGAZINES: leaf",
    0x00030231: "FRONTEND/MAGAZINES: leaf",
    0xb0300100: "Showroom/platform: container",
    0x30300101: "Showroom/platform: sub-record",
    0x30300102: "Showroom/platform: sub-record",
    0x30300103: "Showroom/platform: sub-record",
    0x00035020: "Showroom/platform: leaf",
    0x00035021: "Showroom/platform: leaf",
}


def label(t):
    if t in KNOWN_TYPES:
        return KNOWN_TYPES[t]
    return "container (unknown)" if (t & 0x80000000) else "leaf (unknown)"


def walk(data, base, end, depth, out, extract_dir):
    off = base
    while off + 8 <= end:
        ttype, tsize = struct.unpack_from("<II", data, off)
        payload = off + 8
        if tsize == 0 and ttype == 0:
            # An all-zero {type,size} word is not itself a real chunk -- some
            # container types (e.g. GEOMETRY's 0x80134000) have an 8-byte
            # reserved/unused field immediately after their header, before
            # the real child stream starts. Skip it and keep scanning rather
            # than treating it as end-of-stream.
            off += 8
            continue
        if payload + tsize > end:
            print("  " * depth + f"!! chunk at 0x{off:x} claims size 0x{tsize:x} "
                  f"which runs past the parent's extent (0x{end:x}) -- stopping")
            break
        is_container = bool(ttype & 0x80000000)
        print("  " * depth +
              f"0x{off:08x} type=0x{ttype:08x} size=0x{tsize:x} ({tsize:>9} bytes) "
              f"[{label(ttype)}]")
        if is_container:
            walk(data, payload, payload + tsize, depth + 1, out, extract_dir)
        elif extract_dir:
            os.makedirs(extract_dir, exist_ok=True)
            fn = os.path.join(extract_dir, f"0x{off:08x}_type0x{ttype:08x}.bin")
            with open(fn, "wb") as f:
                f.write(data[payload:payload + tsize])
        off = payload + tsize


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin_file")
    ap.add_argument("--extract", metavar="DIR", default=None,
                     help="also dump each leaf chunk's raw payload into DIR")
    args = ap.parse_args()

    with open(args.bin_file, "rb") as f:
        data = f.read()

    print(f"{args.bin_file}: {len(data)} bytes")
    walk(data, 0, len(data), 0, None, args.extract)


if __name__ == "__main__":
    main()
