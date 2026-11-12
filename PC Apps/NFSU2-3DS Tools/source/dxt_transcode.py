#!/usr/bin/env python3
"""
DXT1/DXT3 -> RGBA8888 transcoder for NFSU2 TEXTURES.BIN textures.

Locates every texture's base mip level inside a TEXTURES.BIN's 0x33320002
pixel-data leaf (see docs/formats/texture-tpk.md for the trailer layout this
relies on), decompresses it (JDLZ or HUFF -- HUFF via CPU emulation of the
real decoder, see tools/huff.py), decodes DXT1/DXT3 block-compressed pixel
data to plain RGBA8888,
and writes each one out as a PNG for visual verification (and optionally as
raw RGBA8888 bytes, which is the simplest format citro3d/PICA200 can consume
directly on the 3DS -- see the "architectural implication" note in
texture-tpk.md; a proper ETC1 encode is a later optimization, not required to
get a texture on screen).

Uncompressed (non-DXT) textures found in the trailer's format slot are copied
through with a straight channel-order fix (BGRA8888 -> RGBA8888), matching
D3DFMT_A8R8G8B8's Direct3D byte order.

Usage:
    python dxt_transcode.py <TEXTURES.BIN> <output_dir>   # decode every texture
    python dxt_transcode.py <TEXTURES.BIN> <output_dir> --raw   # also write .rgba
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import jdlz  # noqa: E402
import huff  # noqa: E402
from scan_texture_formats import iter_table_entries, find_leaves_by_type  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None


def _unpack565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    r = (r << 3) | (r >> 2)
    g = (g << 2) | (g >> 4)
    b = (b << 3) | (b >> 2)
    return r, g, b


def decode_dxt1_block(data, off):
    c0, c1 = struct.unpack_from("<HH", data, off)
    bits = struct.unpack_from("<I", data, off + 4)[0]
    r0, g0, b0 = _unpack565(c0)
    r1, g1, b1 = _unpack565(c1)
    colors = [(r0, g0, b0, 255), (r1, g1, b1, 255)]
    if c0 > c1:
        colors.append(((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255))
        colors.append(((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255))
    else:
        colors.append(((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255))
        colors.append((0, 0, 0, 0))
    out = [None] * 16
    for i in range(16):
        idx = (bits >> (2 * i)) & 3
        out[i] = colors[idx]
    return out


def decode_dxt3_block(data, off):
    # Standard DXT3 order: [8B 4-bit alpha][8B DXT1 colour]. If these ever
    # decode as coherent-but-horizontally-wrapped images, the base offset is
    # wrong by 8 bytes (half a block), not the half ordering -- see the note on
    # decode_dxt3_block() in 3DS Files/source/texpack.c.
    alpha_bits = struct.unpack_from("<Q", data, off)[0]
    rgb = decode_dxt1_block(data, off + 8)
    out = [None] * 16
    for i in range(16):
        a4 = (alpha_bits >> (4 * i)) & 0xF
        a = (a4 << 4) | a4
        r, g, b, _ = rgb[i]
        out[i] = (r, g, b, a)
    return out


def decode_dxt(data, width, height, fourcc):
    if fourcc not in (b"DXT1", b"DXT3"):
        raise ValueError(f"unsupported DXT variant {fourcc!r}")
    block_size = 8 if fourcc == b"DXT1" else 16
    decode_block = decode_dxt1_block if fourcc == b"DXT1" else decode_dxt3_block
    out = bytearray(width * height * 4)
    blocks_w = (width + 3) // 4
    blocks_h = (height + 3) // 4
    off = 0
    for by in range(blocks_h):
        for bx in range(blocks_w):
            pixels = decode_block(data, off)
            off += block_size
            for py in range(4):
                y = by * 4 + py
                if y >= height:
                    continue
                for px in range(4):
                    x = bx * 4 + px
                    if x >= width:
                        continue
                    r, g, b, a = pixels[py * 4 + px]
                    o = (y * width + x) * 4
                    out[o:o + 4] = bytes((r, g, b, a))
    return bytes(out)


def decode_bgra(data, width, height):
    out = bytearray(width * height * 4)
    for i in range(width * height):
        b, g, r, a = data[i * 4:i * 4 + 4]
        out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return bytes(out)


def decode_p8(data, palette, width, height):
    """8-bit palettised (D3DFMT_P8, format code 0x29): one index byte per
    pixel into a 256-entry BGRA8888 CLUT. Unlike the frontend/global TPK
    variant -- where one CLUT is shared by a whole container and lives in the
    first 1024 bytes of its pixel leaf (see 3DS Files/source/texpack.h) --
    this variant gives every texture its own CLUT inside its own compressed
    block, immediately after that texture's pixel run."""
    out = bytearray(width * height * 4)
    for i in range(width * height):
        e = data[i] * 4
        b, g, r, a = palette[e], palette[e + 1], palette[e + 2], palette[e + 3]
        out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return bytes(out)

def _find_speed2_exe_upward(start_dir):
    """Search start_dir and its parents (a few levels up) for SPEED2.EXE, so
    callers pointed at a nested TEXTURES.BIN (e.g. PC Files/CARS/240SX/) can
    still locate the exe up near the PC Files root."""
    d = os.path.abspath(start_dir)
    for _ in range(6):
        candidate = os.path.join(d, "SPEED2.EXE")
        if os.path.isfile(candidate):
            return candidate
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return None


def _decode_block_trailer(out):
    """Given one decompressed texture block's bytes, return (name, width,
    height, fmt_label, rgba) or None if the trailer/pixel data don't parse."""
    if len(out) < 156:
        return None
    trailer = out[-156:]
    name = trailer[12:32].split(b"\x00")[0].decode("ascii", "replace")
    # Trailer fields, measured across all 9 records of
    # GLOBAL/HUD_CustomTextures_00.bin: +48 is the pixel run's offset in the
    # pack and +52 the palette's, +56 the total pixel byte count (base mip
    # *plus* every smaller mip), +60 the palette size (1024 for P8, 0
    # otherwise) and +64 the base mip alone -- +64 comes out as width*height
    # for P8 and as the DXT block-byte count for DXT3, on every record.
    #
    # The block starts with its pixels, and a palettised block's CLUT comes
    # AFTER them: +52 - +48 is +56 rounded up to 128 bytes on all four P8
    # records (TACH_FILL_CUSTOM_00's 87360 pixel bytes are followed by 64 of
    # padding, then the CLUT). Reading the CLUT from in front of the pixels
    # instead decodes pixel bytes as palette entries, which is how the needle
    # once came out as one flat (75,75,75,75) texel -- its real art is a
    # tapered bar in the lower half of the texture.
    pixel_data_size = struct.unpack_from("<I", trailer, 64)[0]
    pixels_off, palette_off = struct.unpack_from("<II", trailer, 48)
    palette_size = struct.unpack_from("<I", trailer, 60)[0]
    width, height = struct.unpack_from("<HH", trailer, 68)
    fmt_field = trailer[144:148]
    if pixel_data_size <= 0 or pixel_data_size > len(out) or width == 0 or height == 0:
        return None
    region = out[:-156]
    pixel_data = region[:pixel_data_size]
    palette = None
    pal_at = palette_off - pixels_off
    if palette_size and pixel_data_size <= pal_at <= len(region) - palette_size:
        palette = region[pal_at:pal_at + palette_size]
    try:
        if all(32 <= b < 127 for b in fmt_field):
            fourcc = fmt_field
            rgba = decode_dxt(pixel_data, width, height, fourcc)
            fmt_label = fourcc.decode("ascii")
        else:
            d3dfmt = struct.unpack_from("<I", fmt_field)[0]
            if d3dfmt == 21 and pixel_data_size >= width * height * 4:
                rgba = decode_bgra(pixel_data, width, height)
                fmt_label = "A8R8G8B8"
            elif (d3dfmt == 0x29 and palette is not None and
                  len(palette) >= 1024 and pixel_data_size >= width * height):
                rgba = decode_p8(pixel_data, palette, width, height)
                fmt_label = "P8"
            else:
                return None
    except Exception:
        return None
    return name, width, height, fmt_label, rgba


def iter_textures_with_hash(data, exe_path=None, huff_dec=None):
    """Yield (hash, name, width, height, fourcc_or_d3dfmt, rgba_bytes) for every
    JDLZ- or HUFF-compressed texture's base mip. Tries the authoritative
    0x33310003 per-texture table first (see docs/formats/texture-tpk.md);
    car/track TEXTURES.BIN files usually have this. Some track .BUN files
    (no 0x33310003 table, e.g. TRACKS/*.BUN's embedded texture container)
    instead pack their JDLZ/HUFF blocks back-to-back inside a single
    0x33320002 leaf with no offset table, so as a fallback this also walks
    every 0x33320002 leaf sequentially, using each block's own header
    compressed_len to find the next one (JDLZ's compressed_len includes its
    16-byte header; HUFF's excludes it -- see docs/formats/texture-tpk.md).
    HUFF blocks are decoded via CPU emulation of the real decoder
    (tools/huff.py); pass exe_path (path to SPEED2.EXE) or a pre-built
    huff_dec (a huff.HuffDecompressor) to enable this -- if neither is given,
    HUFF-compressed textures are skipped."""
    if huff_dec is None and exe_path is not None:
        huff_dec = huff.HuffDecompressor(exe_path)

    found_any = False
    for h, off, csize, usize in iter_table_entries(data):
        magic = data[off:off + 4]
        if magic == b"JDLZ":
            try:
                out, complen = jdlz.decompress(data, off)
            except Exception:
                continue
        elif magic == b"HUFF" and huff_dec is not None:
            try:
                out = huff_dec.decompress_block(data[off:off + csize])
            except Exception:
                continue
        else:
            continue
        found_any = True
        result = _decode_block_trailer(out)
        if result:
            yield (h,) + result

    if found_any:
        return

    # Fallback: no 0x33310003 table found -- walk concatenated blocks inside
    # every 0x33320002 leaf directly.
    for leaf in find_leaves_by_type(data, 0x33320002):
        pos = 0
        while pos + 16 <= len(leaf):
            magic = leaf[pos:pos + 4]
            if magic == b"JDLZ":
                try:
                    out, complen = jdlz.decompress(leaf, pos)
                except Exception:
                    break
                pos += complen
            elif magic == b"HUFF" and huff_dec is not None:
                csize = struct.unpack_from("<I", leaf, pos + 12)[0]
                try:
                    out = huff_dec.decompress_block(leaf[pos:pos + 16 + csize])
                except Exception:
                    break
                pos += 16 + csize
            else:
                break
            result = _decode_block_trailer(out)
            if result:
                yield (None,) + result


def iter_textures(data, exe_path=None, huff_dec=None):
    """iter_textures_with_hash() without the hash -- (name, width, height,
    fourcc_or_d3dfmt, rgba_bytes), the long-standing tuple shape every existing
    caller expects."""
    for rec in iter_textures_with_hash(data, exe_path=exe_path, huff_dec=huff_dec):
        yield rec[1:]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin_file")
    ap.add_argument("out_dir")
    ap.add_argument("--raw", action="store_true", help="also write raw .rgba files")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    with open(args.bin_file, "rb") as f:
        data = f.read()

    exe_path = _find_speed2_exe_upward(os.path.dirname(os.path.abspath(args.bin_file)))
    if exe_path is None:
        print("warning: SPEED2.EXE not found near input file -- HUFF-compressed textures will be skipped")

    count = 0
    for i, (name, w, h, fmt, rgba) in enumerate(iter_textures(data, exe_path=exe_path)):
        base = name if name else f"tex_{i}"
        base = f"{base}_{fmt}_{w}x{h}"
        if Image is not None:
            Image.frombytes("RGBA", (w, h), rgba).save(
                os.path.join(args.out_dir, base + ".png"))
        if args.raw or Image is None:
            with open(os.path.join(args.out_dir, base + ".rgba"), "wb") as f:
                f.write(rgba)
        count += 1
    print(f"Decoded {count} textures to {args.out_dir}")


if __name__ == "__main__":
    main()
