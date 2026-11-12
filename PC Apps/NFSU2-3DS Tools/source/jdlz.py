#!/usr/bin/env python3
"""
Decoder for EA Blackbox's "JDLZ" compression format, used for texture pack
(`.tpk`) pixel data embedded in TEXTURES.BIN's 0x33320002 leaf chunks (see
docs/formats/texture-tpk.md). Ported from the reference C++/C# implementations
at https://github.com/nfs-tools/tpktool (JDLZ.cpp) and
https://github.com/MWisBest/OpenNFSTools (LibNFS/Compression/JDLZ.cs).

Header (16 bytes, all little-endian):
    0x0-0x3  magic "JDLZ"
    0x4      version major (0x02 observed)
    0x5      version minor (0x10 observed)
    0x6-0x7  reserved (0x0000 observed)
    0x8-0xB  uncompressed_len (u32)
    0xC-0xF  compressed_len (u32, includes this header)

Usage:
    python jdlz.py <file> [offset]   # decompress one JDLZ block starting at
                                      # `offset` (default 0) in `file`, write
                                      # <file>.decompressed next to it
"""
import struct
import sys

HEADER_SIZE = 16


def find_header(data, offset=0):
    if len(data) - offset < HEADER_SIZE or data[offset:offset + 4] != b"JDLZ" or data[offset + 4] != 0x02:
        raise ValueError(f"no valid JDLZ header at offset {offset}")
    uncompressed_len, compressed_len = struct.unpack_from("<II", data, offset + 8)
    return uncompressed_len, compressed_len


def decompress(data, offset=0):
    """Decompress a single JDLZ block starting at `offset` in `data`.
    Returns (decompressed_bytes, compressed_len) so callers can advance past
    this block to find the next one in a concatenated stream."""
    uncompressed_len, compressed_len = find_header(data, offset)

    out = bytearray(uncompressed_len)
    in_pos = offset + HEADER_SIZE
    in_end = offset + compressed_len
    out_pos = 0
    flags1 = 1
    flags2 = 1

    while in_pos < in_end and out_pos < uncompressed_len:
        if flags1 == 1:
            flags1 = data[in_pos] | 0x100
            in_pos += 1
        if flags2 == 1:
            flags2 = data[in_pos] | 0x100
            in_pos += 1

        if flags1 & 1:
            b0 = data[in_pos]
            b1 = data[in_pos + 1]
            if flags2 & 1:
                # short match: 1-16 byte backtrack, 3-4098 byte length
                length = (b1 | ((b0 & 0xF0) << 4)) + 3
                t = (b0 & 0x0F) + 1
            else:
                # long match: 17-2064 byte backtrack, 3-34 byte length
                t = (b1 | ((b0 & 0xE0) << 3)) + 17
                length = (b0 & 0x1F) + 3
            in_pos += 2

            for i in range(length):
                out[out_pos + i] = out[out_pos + i - t]
            out_pos += length
            flags2 >>= 1
        else:
            out[out_pos] = data[in_pos]
            out_pos += 1
            in_pos += 1
        flags1 >>= 1

    return bytes(out), compressed_len


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    offset = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0

    with open(path, "rb") as f:
        data = f.read()

    uncompressed_len, compressed_len = find_header(data, offset)
    print(f"JDLZ block at 0x{offset:x}: compressed_len=0x{compressed_len:x} "
          f"uncompressed_len=0x{uncompressed_len:x}")

    out, _ = decompress(data, offset)
    out_path = path + ".decompressed"
    with open(out_path, "wb") as f:
        f.write(out)
    print(f"wrote {len(out)} bytes to {out_path}")


if __name__ == "__main__":
    main()
