"""WORLD.NFST's LZ4 wrapper (TASKS.md #270), shared by the tools that write
and read it.

topo_pack.py writes the topology as LZ4-HC blocks so the world load reads
about 64% of it off the card (7.1 -> 4.5 MB for L4RA); topology.c's TopoIn
decodes it block by block as it reads, with no whole-file buffer.

    +0x00 u32 'NFTL', +0x04 u32 version (1), +0x08 u32 raw size,
    +0x0c u32 raw block size, then a u32 stored size per block (equal to
    the block's raw length: stored as is), then the blocks, LZ4 block format
    with no size prefix.

unwrap() hands back an unwrapped file unchanged, so every reader takes both.
"""

import struct

import lz4.block

MAGIC = b"NFTL"
VERSION = 1
BLOCK = 256 * 1024


def wrap(raw, block=BLOCK):
    sizes, blocks = [], []
    for i in range(0, len(raw), block):
        b = raw[i:i + block]
        c = lz4.block.compress(b, mode="high_compression", compression=12, store_size=False)
        if len(c) >= len(b):
            c = b
        sizes.append(len(c))
        blocks.append(c)
    return (MAGIC + struct.pack("<III", VERSION, len(raw), block) +
            struct.pack("<%dI" % len(sizes), *sizes) + b"".join(blocks))


def unwrap(blob):
    if blob[:4] != MAGIC:
        return blob
    version, raw_size, block = struct.unpack_from("<III", blob, 4)
    if version != VERSION:
        raise ValueError("NFTL version %d" % version)
    n = (raw_size + block - 1) // block
    sizes = struct.unpack_from("<%dI" % n, blob, 16)
    pos = 16 + 4 * n
    out = []
    left = raw_size
    for c in sizes:
        want = min(block, left)
        b = blob[pos:pos + c]
        pos += c
        out.append(b if c == want else lz4.block.decompress(b, uncompressed_size=want))
        left -= want
    raw = b"".join(out)
    if len(raw) != raw_size:
        raise ValueError("NFTL decodes to %d bytes, header says %d" % (len(raw), raw_size))
    return raw


def read(path):
    with open(path, "rb") as f:
        return unwrap(f.read())
