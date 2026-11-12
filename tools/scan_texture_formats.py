#!/usr/bin/env python3
"""
Survey the pixel-format fourCC used by every texture across a set of
TEXTURES.BIN files, using the authoritative 0x33310003 per-texture table
(see docs/formats/texture-tpk.md) to locate each texture's compressed block
by absolute file offset, then decompressing (JDLZ via tools/jdlz.py, HUFF
via CPU emulation of the real decoder in tools/huff.py) and reading the
per-texture trailer's format field.

Usage:
    python scan_texture_formats.py "PC Files"     # scans every TEXTURES.BIN
                                                    # under the given root
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import jdlz  # noqa: E402
import huff  # noqa: E402


def find_leaves_by_type(data, wanted_type):
    """Return raw bytes of every leaf of `wanted_type` in this .BIN file."""
    leaves = []

    def collect(d, base, end):
        off = base
        while off + 8 <= end:
            ttype, tsize = struct.unpack_from("<II", d, off)
            payload = off + 8
            if tsize == 0 and ttype == 0:
                off += 8
                continue
            if payload + tsize > end:
                break
            if ttype & 0x80000000:
                collect(d, payload, payload + tsize)
            elif ttype == wanted_type:
                leaves.append(d[payload:payload + tsize])
            off = payload + tsize

    collect(data, 0, len(data))
    return leaves


def iter_table_entries(data):
    """Yield (hash, file_offset, compressed_size, uncompressed_size) for
    every texture listed in the 0x33310003 table."""
    for leaf in find_leaves_by_type(data, 0x33310003):
        for i in range(len(leaf) // 24):
            h, off, csize, usize, _u4, _u5 = struct.unpack_from("<6I", leaf, i * 24)
            yield h, off, csize, usize


def _find_speed2_exe(root):
    candidate = os.path.join(root, "SPEED2.EXE")
    if os.path.isfile(candidate):
        return candidate
    for dirpath, _, filenames in os.walk(root):
        for fn in filenames:
            if fn.upper() == "SPEED2.EXE":
                return os.path.join(dirpath, fn)
    return None


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "PC Files"
    formats = {}
    errors = []
    file_count = 0
    tex_count = 0
    huff_count = 0

    exe_path = _find_speed2_exe(root)
    huff_dec = huff.HuffDecompressor(exe_path) if exe_path else None

    for dirpath, _, filenames in os.walk(root):
        for fn in filenames:
            if fn.upper() != "TEXTURES.BIN":
                continue
            path = os.path.join(dirpath, fn)
            file_count += 1
            with open(path, "rb") as f:
                data = f.read()
            for h, off, csize, usize in iter_table_entries(data):
                magic = data[off:off + 4]
                if magic == b"HUFF":
                    huff_count += 1
                    if huff_dec is None:
                        errors.append(f"{path} hash={h:#x} off={off:#x}: HUFF block but SPEED2.EXE not found")
                        continue
                    try:
                        out = huff_dec.decompress_block(data[off:off + csize])
                    except Exception as e:
                        errors.append(f"{path} hash={h:#x} off={off:#x}: HUFF decode failed: {e}")
                        continue
                elif magic == b"JDLZ":
                    try:
                        out, complen = jdlz.decompress(data, off)
                    except Exception as e:
                        errors.append(f"{path} hash={h:#x} off={off:#x}: {e}")
                        continue
                else:
                    errors.append(f"{path} hash={h:#x} off={off:#x}: unrecognized magic {magic!r}")
                    continue
                if len(out) < 156:
                    errors.append(f"{path} hash={h:#x} off={off:#x}: decompressed output too short for a trailer")
                    continue
                trailer = out[-156:]
                fmt_field = trailer[144:148]
                if all(32 <= b < 127 for b in fmt_field):
                    key = fmt_field.decode("ascii")
                else:
                    fmt_val = struct.unpack_from("<I", fmt_field)[0]
                    d3dfmt_names = {21: "D3DFMT_A8R8G8B8", 22: "D3DFMT_X8R8G8B8",
                                    25: "D3DFMT_R5G6B5", 32: "D3DFMT_A8"}
                    name = d3dfmt_names.get(fmt_val, f"D3DFORMAT={fmt_val}")
                    key = f"<uncompressed {name}>"
                formats[key] = formats.get(key, 0) + 1
                tex_count += 1

    print(f"Scanned {file_count} TEXTURES.BIN files, {tex_count} texture blocks "
          f"({huff_count} HUFF-compressed, decoded via CPU emulation -- see tools/huff.py)")
    print("Format fourCC counts (JDLZ + HUFF):")
    for k, v in sorted(formats.items(), key=lambda kv: -kv[1]):
        print(f"  {k!r}: {v}")
    if errors:
        print(f"\n{len(errors)} decode errors (first 10):")
        for e in errors[:10]:
            print(f"  {e}")


if __name__ == "__main__":
    main()
