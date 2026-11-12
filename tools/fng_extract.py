#!/usr/bin/env python3
"""
Extracts real `.fng` frontend-screen package files out of a decompressed
`FrontB.lzc`/`FrontEndMemoryFile`-style blob.

Each `.fng` file is stored as its own individually JDLZ-compressed sub-block
(see jdlz.py) concatenated back-to-back inside the outer JDLZ-decompressed
container. Decompressing each sub-block yields a raw `FEPackage` chunk stream
starting with the 4-byte magic `46 45 6E E7` ("FEn"+0xE7) followed by a `PkHd`
(Package Header) chunk whose payload holds the real on-disk Name (e.g.
`UI_Main.fng`) and original build Filename (e.g.
`Global/FEng_Screens/Stripped/UI_Main.fng`).

See docs/formats/frontend-menu-re.md for the full chunk/tag format writeup
(FEPackageReader-derived: outer FourCC+length chunks, inner Ot/OP/PA/etc.
per-widget tags).

Usage:
    python fng_extract.py <FrontB.lzc-or-similar> <output_dir>
"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jdlz

FEN_MAGIC = b"\x46\x45\x6e\xe7"


def find_fng_subblocks(outer_decompressed):
    """Yields (name, raw_fng_bytes) for every JDLZ sub-block that decompresses
    to a valid FEn-magic package chunk."""
    idx = 0
    while True:
        idx = outer_decompressed.find(b"JDLZ", idx)
        if idx == -1:
            return
        try:
            sub, compressed_len = jdlz.decompress(outer_decompressed, idx)
        except (ValueError, IndexError):
            idx += 1
            continue
        if sub[0:4] == FEN_MAGIC:
            name = sub[40:200].split(b"\x00", 1)[0].decode("latin1")
            yield name, sub
        idx += 1


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    src_path, out_dir = sys.argv[1], sys.argv[2]

    with open(src_path, "rb") as f:
        data = f.read()

    outer, _ = jdlz.decompress(data, 0)
    os.makedirs(out_dir, exist_ok=True)

    count = 0
    for name, raw in find_fng_subblocks(outer):
        safe_name = name if name else f"unnamed_{count}.fng"
        out_path = os.path.join(out_dir, safe_name)
        with open(out_path, "wb") as f:
            f.write(raw)
        print(f"{safe_name}  ({len(raw)} bytes)")
        count += 1

    print(f"\nExtracted {count} .fng files to {out_dir}")


if __name__ == "__main__":
    main()
