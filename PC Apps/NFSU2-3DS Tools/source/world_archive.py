"""WORLD.NFSZ: a track's sections and texture bank in one compressed archive
(TASKS.md #270 P2+P3+P7).

    python tools/world_archive.py [WORLD_DIR] [--codec lz4|zlib|store] [--verify] [--prune]

WORLD_DIR defaults to Azahar's sdmc copy of TRACKS/L4RA. It must hold what
track_pack.py writes there: WORLD.NFSX, every section's .nfsc and WORLD.NFSB.
The archive is written next to them. The runtime (world.c) reads only the
archive, and only if it was built from the WORLD.NFSX on the card.

--prune deletes the loose sections and WORLD.NFSB once the archive has been
written and verified against them. Nothing reads those files after that. The
user zips Azahar's sdmc for testers, so files no one reads must not stay there
(2026-10-29). pack_world.py passes --prune. To build the archive again from the
loose files, run pack_world.py; it writes them again before it packs.

Why one file. On the SD card every fopen() is a filesystem walk and every
section was three of them (its .nfsc, then a seek per texture in the bank):
`world: io` showed 10-11 ms of every section's read before its first byte, 1.6
s of a 250 km/h race's 36 s of reads. Here the archive is opened once and kept
open, a section and the textures it is the first to use sit together, and the
two are one read.

Why compressed, and why LZ4 by default. The world reads at about 4.2 MB/s on
Azahar, and the hardware's SD is the reason the user asked for this to be
minded. LZ4-HC packs the sections to about 43% and the bank to about 42%
(zlib-9: 34%), and decodes at a cost the ARM11 can ignore; inflate is several
times slower per byte on that core, enough to eat what its smaller reads save
unless it overlaps the reads (P6). The codec is chosen per blob, so `--codec
zlib` packs an archive the same runtime reads, to measure the two against each
other on the console (`world: io` reports the decode time).

Nothing is lost: every blob decodes to the exact bytes of its loose file
(`--verify` checks that end to end).

Layout, little-endian:

    header, 0x40 bytes
      +0x00  char[4]  "NFSZ"
      +0x04  u32      version (1)
      +0x08  u32      section count (= WORLD.NFSX's)
      +0x0c  u32      texture count (= WORLD.NFSB's)
      +0x10  u32      CRC-32 of the WORLD.NFSX this was built from
      +0x14  u32      section table offset
      +0x18  u32      texture table offset
      +0x1c  u32      offset of WORLD.NFSB's header and record table, copied
      +0x20  u32      its length (0x40 + count * 0x20 + the texture-animation
                      table track_pack.py writes after the records, #326)
      +0x24  u32      default codec (informational)
      +0x28  u32[6]   reserved

    section table, one 0x30-byte entry per WORLD.NFSX record, in its order
      +0x00  u32      offset of the section's first piece; 0 = no file
      +0x04  u32      span: bytes from there through the textures stored after
                      it -- the one read that loads the section
      +0x08  u32      the .nfsc's size
      +0x0c  u8       codec: 0 stored, 1 zlib, 2 LZ4 block
      +0x0d  u8       piece count, 1 or 4
      +0x0e  u16      reserved
      +0x10  u32[4]   each piece's stored size
      +0x20  u32[4]   each piece's decoded size

    texture table, one 0x10-byte entry per bank record, in its (hash) order
      +0x00  u32      offset of the texture's whole mip chain
      +0x04  u32      stored size
      +0x08  u32      decoded size (= the record's byte length)
      +0x0c  u8       codec
      +0x0d  u8[3]    reserved

A section is cut into four pieces where the runtime puts its bytes: the header
and the two tables (heap), the vertices and the indices (each straight into its
GPU pool) and the instance table (#247). Each piece is a blob of its own, so
none needs the others to decode and nothing is staged twice. The 0-3 bytes of
padding between the indices and the instance table are zeros and are not
stored. A file the runtime would not stream -- one with embedded pixels, or
that model3d_plan() refuses -- is one piece, the whole file.

Pieces are stored back to back from a section's offset (aligned to 512 bytes,
the SD's sector), followed by the textures this section is the first in index
order to use, in its texture table's order. Textures no section uses go last.
"""
import argparse
import os
import struct
import sys
import zlib

try:
    import lz4.block
except ImportError:  # only --codec lz4 needs it
    lz4 = None

ARCHIVE_NAME = "WORLD.NFSZ"
ARCHIVE_MAGIC = b"NFSZ"
ARCHIVE_VERSION = 1
HDR_SIZE = 0x40
SEC_ENTRY = 0x30
TEX_ENTRY = 0x10
SECTION_ALIGN = 0x200

CODEC_STORE, CODEC_ZLIB, CODEC_LZ4 = 0, 1, 2
CODECS = {"store": CODEC_STORE, "zlib": CODEC_ZLIB, "lz4": CODEC_LZ4}

# The loose files' layouts: track_pack.py writes them, world.c and model3d.c
# read them.
INDEX_NAME, INDEX_MAGIC, INDEX_HDR, INDEX_REC = "WORLD.NFSX", b"NFSW", 0x40, 0x60
BANK_NAME, BANK_MAGIC, BANK_HDR, BANK_REC = "WORLD.NFSB", b"NFSB", 0x40, 0x20
NFSC_MAGIC, NFSC_HDR, VERTEX_SIZE = b"NFSC", 0x40, 32


def default_dir():
    appdata = os.environ.get("APPDATA", "")
    return os.path.join(appdata, "Azahar", "sdmc", "nfsu2data", "TRACKS", "L4RA")


def nfsc_pieces(blob):
    """Where model3d_plan() cuts a section: [(start, end)] x 4, or the whole
    file as one piece when the runtime would read it whole."""
    whole = [(0, len(blob))]
    if len(blob) < NFSC_HDR or blob[:4] != NFSC_MAGIC:
        return whole
    n_tex, n_batch, n_vert, n_idx = struct.unpack_from("<4I", blob, 0x08)
    tex_off, tex_size = struct.unpack_from("<2I", blob, 0x30)
    o_vert = NFSC_HDR + n_tex * 16 + n_batch * 16
    o_idx = o_vert + n_vert * VERTEX_SIZE
    idx_end = o_idx + n_idx * 2
    inst_off = (idx_end + 3) & ~3
    if (tex_size or not n_batch or not n_vert or not n_idx or n_vert > 65536 or
            idx_end > len(blob) or tex_off + tex_size > len(blob) or
            tex_off < inst_off or tex_off != len(blob)):
        return whole
    if any(blob[idx_end:inst_off]):
        return whole   # the padding is dropped, so it had better be zeros
    return [(0, o_vert), (o_vert, o_idx), (o_idx, idx_end), (inst_off, len(blob))]


def encode(data, codec):
    """(codec actually used, stored bytes): stored when packing would not
    make it smaller."""
    if not data or codec == CODEC_STORE:
        return CODEC_STORE, bytes(data)
    if codec == CODEC_ZLIB:
        c = zlib.compress(bytes(data), 9)
    else:
        c = lz4.block.compress(bytes(data), mode="high_compression",
                               compression=12, store_size=False)
    return (codec, c) if len(c) < len(data) else (CODEC_STORE, bytes(data))


def decode(data, codec, raw):
    if codec == CODEC_STORE:
        return bytes(data)
    if codec == CODEC_ZLIB:
        return zlib.decompress(data)
    return lz4.block.decompress(data, uncompressed_size=raw)


def read_index(path):
    blob = open(path, "rb").read()
    if blob[:4] != INDEX_MAGIC or struct.unpack_from("<I", blob, 0x0c)[0] != INDEX_REC:
        sys.exit("%s is not a v2 world index" % path)
    n = struct.unpack_from("<I", blob, 8)[0]
    names = []
    for i in range(n):
        r = INDEX_HDR + i * INDEX_REC
        names.append(blob[r:r + 16].split(b"\0")[0].decode("ascii"))
    return blob, names


def read_bank(path):
    f = open(path, "rb")
    hdr = f.read(BANK_HDR)
    if hdr[:4] != BANK_MAGIC or struct.unpack_from("<I", hdr, 0x0c)[0] != BANK_REC:
        sys.exit("%s is not a texture bank" % path)
    n, payload = struct.unpack_from("<2I", hdr, 8)[0], struct.unpack_from("<I", hdr, 0x10)[0]
    recs = f.read(n * BANK_REC)
    # The texture-animation table (track_pack.py, TASKS.md #326) rides with
    # the records; its length is the header's +0x20, zero in an older bank.
    recs += f.read(struct.unpack_from("<I", hdr, 0x20)[0])
    hashes, spans = [], []
    for i in range(n):
        r = i * BANK_REC
        hashes.append(struct.unpack_from("<I", recs, r)[0])
        off, size = struct.unpack_from("<2I", recs, r + 0x0c)
        spans.append((payload + off, size))
    return f, hdr + recs, hashes, spans


def build(world_dir, codec, verbose):
    index_blob, names = read_index(os.path.join(world_dir, INDEX_NAME))
    bank_f, bank_table, hashes, tex_spans = read_bank(os.path.join(world_dir, BANK_NAME))
    by_hash = {h: i for i, h in enumerate(hashes)}
    n_sec, n_tex = len(names), len(hashes)

    sec_table_off = HDR_SIZE
    tex_table_off = sec_table_off + n_sec * SEC_ENTRY
    bank_off = tex_table_off + n_tex * TEX_ENTRY
    data_off = (bank_off + len(bank_table) + SECTION_ALIGN - 1) & ~(SECTION_ALIGN - 1)

    out_path = os.path.join(world_dir, ARCHIVE_NAME)
    tmp_path = out_path + ".tmp"
    out = open(tmp_path, "wb")
    out.write(b"\0" * data_off)
    sec_entries = [b"\0" * SEC_ENTRY] * n_sec
    tex_entries = [None] * n_tex
    raw_total = stored_total = 0

    def put_texture(t):
        nonlocal raw_total, stored_total
        off, size = tex_spans[t]
        bank_f.seek(off)
        pixels = bank_f.read(size)
        if len(pixels) != size:
            sys.exit("%s: texture %08x is cut short" % (BANK_NAME, hashes[t]))
        c, stored = encode(pixels, codec)
        tex_entries[t] = struct.pack("<3IB3x", out.tell(), len(stored), size, c)
        out.write(stored)
        raw_total += size
        stored_total += len(stored)

    for i, name in enumerate(names):
        path = os.path.join(world_dir, name + ".nfsc")
        if not os.path.exists(path):
            print("  %s: no file -- left out (the runtime calls it missing)" % name)
            continue
        blob = open(path, "rb").read()
        pad = (-out.tell()) % SECTION_ALIGN
        out.write(b"\0" * pad)
        start = out.tell()
        pieces = nfsc_pieces(blob)
        coded = [encode(blob[a:b], codec) for a, b in pieces]
        # One codec per section: stored throughout if packing any piece did
        # not pay, which keeps the runtime's per-section path simple.
        sec_codec = codec
        if any(c != codec and b > a for (c, _), (a, b) in zip(coded, pieces)):
            sec_codec = CODEC_STORE
            coded = [(CODEC_STORE, blob[a:b]) for a, b in pieces]
        comp = [len(s) for _, s in coded] + [0] * (4 - len(coded))
        raw = [b - a for a, b in pieces] + [0] * (4 - len(pieces))
        for _, s in coded:
            out.write(s)
        raw_total += len(blob)
        stored_total += sum(comp)
        # The textures this section is the first to use, right behind it.
        n_tex_sec = struct.unpack_from("<I", blob, 0x08)[0] if blob[:4] == NFSC_MAGIC else 0
        for k in range(n_tex_sec):
            t = by_hash.get(struct.unpack_from("<I", blob, NFSC_HDR + k * 16)[0])
            if t is not None and tex_entries[t] is None:
                put_texture(t)
        span = out.tell() - start
        sec_entries[i] = struct.pack("<3IBBH4I4I", start, span, len(blob), sec_codec,
                                     len(pieces), 0, *(comp + raw))
        if verbose:
            print("  %-6s %8d -> %8d B, %d piece(s), span %d" %
                  (name, len(blob), sum(comp), len(pieces), span))
    for t in range(n_tex):
        if tex_entries[t] is None:
            put_texture(t)

    out.seek(0)
    out.write(struct.pack("<4s9I24x", ARCHIVE_MAGIC, ARCHIVE_VERSION, n_sec, n_tex,
                          zlib.crc32(index_blob) & 0xffffffff, sec_table_off,
                          tex_table_off, bank_off, len(bank_table), codec))
    out.write(b"".join(sec_entries))
    out.write(b"".join(tex_entries))
    out.write(bank_table)
    out.close()
    bank_f.close()
    os.replace(tmp_path, out_path)
    size = os.path.getsize(out_path)
    print("wrote %s: %d sections, %d textures, %.1f MB from %.1f MB (%.1f%%)" %
          (out_path, n_sec, n_tex, size / 1048576.0, raw_total / 1048576.0,
           100.0 * stored_total / max(raw_total, 1)))
    return out_path


def verify(world_dir):
    """Decode every blob in the archive and compare it with its loose file."""
    path = os.path.join(world_dir, ARCHIVE_NAME)
    a = open(path, "rb")
    hdr = a.read(HDR_SIZE)
    (magic, ver, n_sec, n_tex, crc, sec_off, tex_off, bank_off, bank_len,
     _codec) = struct.unpack_from("<4s9I", hdr)
    index_blob, names = read_index(os.path.join(world_dir, INDEX_NAME))
    bank_f, bank_table, hashes, tex_spans = read_bank(os.path.join(world_dir, BANK_NAME))
    assert magic == ARCHIVE_MAGIC and ver == ARCHIVE_VERSION
    assert n_sec == len(names) and n_tex == len(hashes)
    assert crc == zlib.crc32(index_blob) & 0xffffffff, "built from another WORLD.NFSX"
    a.seek(bank_off)
    assert a.read(bank_len) == bank_table, "bank table differs"
    a.seek(sec_off)
    secs = a.read(n_sec * SEC_ENTRY)
    for i, name in enumerate(names):
        e = struct.unpack_from("<3IBBH4I4I", secs, i * SEC_ENTRY)
        off, span, raw, codec, n_pieces = e[:5]
        comp, praw = e[6:10], e[10:14]
        p = os.path.join(world_dir, name + ".nfsc")
        if not off:
            assert not os.path.exists(p), name
            continue
        blob = open(p, "rb").read()
        assert raw == len(blob), name
        a.seek(off)
        got = b""
        cut = nfsc_pieces(blob)
        assert len(cut) == n_pieces, name
        for k in range(n_pieces):
            dec = decode(a.read(comp[k]), codec, praw[k])
            assert len(dec) == praw[k] == cut[k][1] - cut[k][0], (name, k)
            assert dec == blob[cut[k][0]:cut[k][1]], (name, k)
        assert sum(comp[:n_pieces]) <= span, name
    a.seek(tex_off)
    texs = a.read(n_tex * TEX_ENTRY)
    for t in range(n_tex):
        off, comp, raw, codec = struct.unpack_from("<3IB", texs, t * TEX_ENTRY)
        boff, size = tex_spans[t]
        assert raw == size, hashes[t]
        bank_f.seek(boff)
        a.seek(off)
        assert decode(a.read(comp), codec, raw) == bank_f.read(size), "%08x" % hashes[t]
    print("verified %s: %d sections, %d textures decode to their loose bytes" %
          (path, n_sec, n_tex))


def prune(world_dir):
    """Delete the loose files the archive was verified against. SKYDOME.nfsc
    and SHADOW.nfsc are not in WORLD.NFSX's index and are left alone."""
    _, names = read_index(os.path.join(world_dir, INDEX_NAME))
    gone = freed = 0
    for p in [os.path.join(world_dir, n + ".nfsc") for n in names] + \
            [os.path.join(world_dir, BANK_NAME)]:
        if os.path.exists(p):
            freed += os.path.getsize(p)
            os.remove(p)
            gone += 1
    print("pruned %d loose file(s), %.1f MB: the archive holds them" %
          (gone, freed / 1048576.0))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("world_dir", nargs="?", default=default_dir())
    ap.add_argument("--codec", choices=sorted(CODECS), default="lz4")
    ap.add_argument("--verify", action="store_true",
                    help="decode the whole archive afterwards and compare")
    ap.add_argument("--verify-only", action="store_true")
    ap.add_argument("--prune", action="store_true",
                    help="after verifying, delete the loose sections and WORLD.NFSB")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    codec = CODECS[args.codec]
    if codec == CODEC_LZ4 and lz4 is None:
        sys.exit("--codec lz4 needs the lz4 module: pip install lz4")
    if not os.path.exists(os.path.join(args.world_dir, BANK_NAME)):
        sys.exit("no %s in %s: the loose files are deleted once packed -- "
                 "run pack_world.py" % (BANK_NAME, args.world_dir))
    if not args.verify_only:
        build(args.world_dir, codec, args.verbose)
    if args.verify or args.verify_only or args.prune:
        verify(args.world_dir)
    if args.prune:
        prune(args.world_dir)


if __name__ == "__main__":
    main()
