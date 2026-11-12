"""Rebuild 3DS Files/data/helvetica.bcfnt, the UI font (TASKS.md #425), from the
user's Helvetica 67 Condensed Medium Oblique .ttf in the repo root.

The .ttf stores its descender as +176 in both hhea and OS/2 (typo), where the
OpenType spec wants it negative. mkbcfnt then takes ascender - descender =
714 - 176 as the line height and writes a line feed of 20 px instead of 33,
and every point-size text size in the port (g_fontUnitPx, the HUD, the bank
strip, the chyron, the title screen) came out 1.7x too big. This fixes the
sign in a temporary copy before converting; nothing else in the font changes.

Then (#529) the glyph sheet is laid out again, one line of glyphs to a
1024x64 sheet and a 58 px cell stride, by relayout(). mkbcfnt packs the
cells 1 px apart, and the port mipmaps the sheet (fontmip.c, levels 0..3):
at the small sizes text is drawn at, a glyph's quad then picked up the
glyph above it in the sheet as faint lines over its top (worst under '['),
and the one beside it at its edges. The glyph indices, widths and CMAP stay
as they are; only TGLP's cell width, row count, line count, sheet height,
sheet size and sheet count change, which citro2d reads as it always does.

Usage (from the repo root, PowerShell):  python tools/make_ui_font.py
"""
import os
import subprocess
import sys
import tempfile

from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TTF = os.path.join(ROOT, "Helvetica67-CondensedMedium MediumItalic.ttf")
OUT = os.path.join(ROOT, "3DS Files", "data", "helvetica.bcfnt")
# DEVKITPRO is often MSYS-style (/opt/devkitpro), which Windows cannot open.
MKBCFNT = next((p for p in (
    os.path.join(os.environ.get("DEVKITPRO", ""), "tools", "bin", "mkbcfnt.exe"),
    r"C:\devkitPro\tools\bin\mkbcfnt.exe") if os.path.isfile(p)), "mkbcfnt")
SIZE = "28"  # a 41x44 cell, the size conduit.bcfnt's was


def main():
    t = TTFont(TTF)
    for table, field in (("hhea", "descent"), ("OS/2", "sTypoDescender")):
        v = getattr(t[table], field)
        if v > 0:
            setattr(t[table], field, -v)
    with tempfile.TemporaryDirectory() as d:
        fixed = os.path.join(d, "ui.ttf")
        t.save(fixed)
        tmp = OUT + ".tmp"
        subprocess.run([MKBCFNT, "-s", SIZE, "-o", tmp, fixed], check=True)
        relayout(tmp)
        os.replace(tmp, OUT)
    b = open(OUT, "rb").read()
    finf = int.from_bytes(b[6:8], "little")
    print("wrote %s: %d bytes, line feed %d" % (OUT, len(b), b[finf + 9]))
    for name in ("helvetica.bcfnt", "conduit.bcfnt"):
        print("  %s: H is %d px tall in its cell (main.c's cap-height match uses 27 and 24)"
              % (name, cap_ink(os.path.join(os.path.dirname(OUT), name))))
    return 0


STRIDE_X = 58     # cell stride across: 17 px between two glyphs' ink (41 px cells)
SHEET_H = 64      # one line of 45 px cells, 19 blank rows under it


def _morton(x, y):
    m = 0
    for i in range(3):
        m |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1)
    return m


def relayout(path):
    """One line of glyphs to a sheet, cells STRIDE_X apart (#529).

    citro2d places glyph k of a sheet at x = (k % nRows) * (cellWidth + 1) + 1,
    y = (k // nRows) * (cellHeight + 1) + 1 and draws it cellHeight tall, so
    the cell height (and the line feed and text scale) stays 44 and only the
    width and the line count change. A 64-row sheet leaves 19 blank rows under
    each glyph and the edge clamp over it; the stride leaves 17 px across.
    Mip level 3 (8 px texels, bilinear) reaches 12 px past a quad's edge."""
    import struct
    b = bytearray(open(path, "rb").read())
    finf = struct.unpack_from("<H", b, 6)[0]
    # FINF's default width (left, glyph width, char width), which this
    # mkbcfnt leaves uninitialised; the font shipped through #528 had zeros.
    b[finf + 0xC:finf + 0xF] = bytes(3)
    tglp, cwdh, cmap = struct.unpack_from("<III", b, finf + 0x10)
    cw, ch = b[tglp], b[tglp + 1]
    size, = struct.unpack_from("<I", b, tglp + 4)
    n, fmt, cols, rows, sw, sh = struct.unpack_from("<HHHHHH", b, tglp + 8)
    data, = struct.unpack_from("<I", b, tglp + 0x14)
    assert fmt == 11 and size == sw * sh // 2, "an A4 sheet expected (fmt %d)" % fmt
    assert STRIDE_X >= cw + 1 + 16 and SHEET_H >= ch + 2 + 16
    end = data + n * size
    assert cwdh > end and cmap > end, "CWDH/CMAP expected after the sheets"

    def texel(sheet, x, y):
        t = ((y // 8) * (sw // 8) + x // 8) * 64 + _morton(x & 7, y & 7)
        v = b[data + sheet * size + t // 2]
        return v >> 4 if t & 1 else v & 15

    # Every glyph cell, by index: the sheets hold cols * rows apiece.
    count = n * cols * rows
    per = sw // STRIDE_X
    nsh = -(-count // per)
    nsize = sw * SHEET_H // 2
    out = bytearray(nsh * nsize)
    for g in range(count):
        s, k = divmod(g, cols * rows)
        ox, oy = (k % cols) * (cw + 1) + 1, (k // cols) * (ch + 1) + 1
        ns, nk = divmod(g, per)
        nx, ny = nk * STRIDE_X + 1, 1
        for y in range(ch):
            for x in range(cw):
                v = texel(s, ox + x, oy + y)
                if v:
                    t = (((ny + y) // 8) * (sw // 8) + (nx + x) // 8) * 64 + _morton((nx + x) & 7, (ny + y) & 7)
                    out[ns * nsize + t // 2] |= v << 4 if t & 1 else v
    # Trim the trailing sheets no glyph uses (the last CWDH index decides).
    last = 0
    p = cwdh
    while p:
        lo, hi, nxt = struct.unpack_from("<HHI", b, p)
        last = max(last, hi)
        p = nxt
    nsh = last // per + 1
    out = out[:nsh * nsize]
    delta = len(out) - (end - data)

    def fix(off):
        v, = struct.unpack_from("<I", b, off)
        if v:
            struct.pack_into("<I", b, off, v + delta)

    p = cwdh
    while p:
        q = p
        p, = struct.unpack_from("<I", b, q + 4)
        fix(q + 4)
    p = cmap
    while p:
        q = p
        p, = struct.unpack_from("<I", b, q + 8)
        fix(q + 8)
    fix(finf + 0x14)
    fix(finf + 0x18)
    b[tglp] = STRIDE_X - 1
    struct.pack_into("<I", b, tglp + 4, nsize)
    struct.pack_into("<HHHHHH", b, tglp + 8, nsh, fmt, per, 1, sw, SHEET_H)
    b = b[:data] + out + b[end:]
    struct.pack_into("<I", b, 0x0C, len(b))
    open(path, "wb").write(b)
    print("relaid %s: %d sheet(s) of %dx%d, %d cells a sheet, %d bytes of sheets (was %d)"
          % (os.path.basename(path), nsh, sw, SHEET_H, per, len(out), end - data))


def cap_ink(path, code=ord("H")):
    """Ink rows of one glyph in a .bcfnt's A4 sheet (8x8 tiles, Morton order)."""
    import struct
    b = open(path, "rb").read()
    finf = struct.unpack_from("<H", b, 6)[0]
    tglp, _w, cmap = struct.unpack_from("<III", b, finf + 0x10)
    cw, ch = b[tglp], b[tglp + 1]
    sheet_size, = struct.unpack_from("<I", b, tglp + 4)
    _n, _fmt, cols, rows, sw, _sh = struct.unpack_from("<HHHHHH", b, tglp + 8)
    data, = struct.unpack_from("<I", b, tglp + 0x14)
    idx, p = None, cmap
    while p and idx is None:
        lo, hi, meth, _r, nxt = struct.unpack_from("<HHHHI", b, p)
        if lo <= code <= hi:
            if meth == 0:
                idx = struct.unpack_from("<H", b, p + 12)[0] + code - lo
            elif meth == 1:
                idx = struct.unpack_from("<H", b, p + 12 + 2 * (code - lo))[0]
            else:
                n, = struct.unpack_from("<H", b, p + 12)
                for k in range(n):
                    c, i = struct.unpack_from("<HH", b, p + 14 + 4 * k)
                    if c == code:
                        idx = i
        p = nxt
    sheet, k = divmod(idx, cols * rows)
    x0, y0 = (k % cols) * (cw + 1), (k // cols) * (ch + 1)

    def texel(x, y):
        m = 0
        for i in range(3):
            m |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1)
        t = ((y // 8) * (sw // 8) + x // 8) * 64 + m
        v = b[data + sheet * sheet_size + t // 2]
        return v >> 4 if t & 1 else v & 15

    ys = [y for y in range(ch + 1)
          if any(texel(x0 + x, y0 + y) > 7 for x in range(cw + 1))]
    return ys[-1] - ys[0] + 1


if __name__ == "__main__":
    sys.exit(main())
