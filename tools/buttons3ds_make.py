#!/usr/bin/env python3
r"""Draws the port's 3DS button icons: `3DS Files/data/buttons3ds.bin`.

NFSU2 draws this sheet in the frontend's $JOY_EVENT_FENG_*$ slots, the help
text's icons and the engage widget (main.c's drawPadGlyph*).

#467 (the user: "UG2-looking icons"): the style is the GameCube UG2's own
button set (GlobalTexturesGAMECUBE.tpk's GAMECUBE_*; tools/gc_tpk_dump.py
dumps them for reference) -- flat outlines in UG2's lime green, see-through
inside, bold upright letters, every stroke edged in a thin dark halo so it
reads over any background. Nothing is copied from those textures: every icon
here is drawn from shapes, and the shapes are the New 3DS's buttons (round
A / B / X / Y, corner L / R, ZL / ZR pills, the Circle Pad, the C-Stick, the
+Control Pad, SELECT and START), which the GameCube pad does not have.

Each icon is drawn 26 x 23 and set 3 rows down a 26 x 28 cell with an
advance of 26, as before, so a line of text keeps its width. The sheet is
128 x 128 RGBA8, row 0 at the top, cell i at ((i % 4) * 26, (i / 4) * 28) in
ICONS order, which main.c's PAD_GLYPH_KEYS mirrors.

The letters are set in Arial Bold (Windows' arialbd.ttf).

    python tools/buttons3ds_make.py            # writes the .bin
    python tools/buttons3ds_make.py --preview out/buttons3ds.png
    python tools/buttons3ds_make.py --face-colours ...   # A/B/X/Y letters in
                                                         # the New 3DS colours
"""
import argparse
import math
import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "3DS Files" / "data" / "buttons3ds.bin"
FONT = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts" / "arialbd.ttf"

SHEET = 128
CELL_W, CELL_H = 26, 28
ART_H, ART_TOP = 23, 3                   # the icon's rows in its cell
SS = 8                                   # supersampling

ICONS = ["A", "B", "X", "Y", "L", "R", "ZL", "ZR", "CPAD", "CSTICK", "DPAD",
         "SELECT", "START"]

LIME = (204, 255, 136)                   # UG2's GC icon green (0xCCFF88)
HALO = (0, 0, 0)
HALO_ALPHA = 200
HALO_PX = 1.1                            # how far the dark edge reaches
LINE = 1.9                               # an outline's width, in pixels
FACE_RGB = {                             # --face-colours: the New 3DS's
    "A": (240, 72, 60),
    "B": (250, 206, 52),
    "X": (80, 140, 250),
    "Y": (80, 200, 100),
}

W, H = CELL_W * SS, ART_H * SS
CX, CY = 13.0 * SS, 11.5 * SS


def P(v):
    return v * SS


class Ink:
    """Layers of strokes: each layer a mask and a colour, all edged by one
    halo drawn under them."""

    def __init__(self):
        self.layers = []

    def layer(self, rgb, alpha=255):
        m = Image.new("L", (W, H), 0)
        self.layers.append((m, rgb, alpha))
        return ImageDraw.Draw(m)

    def render(self):
        union = Image.new("L", (W, H), 0)
        for m, _, _ in self.layers:
            union = Image.composite(m, union, m)
        r = int(round(HALO_PX * SS))
        halo = union.filter(ImageFilter.MaxFilter(2 * r + 1))
        img = Image.new("RGBA", (W, H), HALO + (0,))
        img.putalpha(halo.point(lambda a: a * HALO_ALPHA // 255))
        for m, rgb, alpha in self.layers:
            top = Image.new("RGBA", (W, H), rgb + (0,))
            top.putalpha(m.point(lambda a, k=alpha: a * k // 255))
            img.alpha_composite(top)
        return img.resize((CELL_W, ART_H), Image.LANCZOS)


def ring(d, cx, cy, r, width=LINE):
    """A circle's outline, `r` to the middle of the line."""
    w = P(width)
    R = P(r) + w / 2
    d.ellipse([cx - R, cy - R, cx + R, cy + R], outline=255, width=int(round(w)))


def outline(d, mask_fn, width=LINE):
    """The outline of a filled shape: the shape less the shape shrunk by the
    line's width."""
    full = Image.new("L", (W, H), 0)
    mask_fn(ImageDraw.Draw(full))
    k = int(round(P(width))) * 2 + 1
    inner = full.filter(ImageFilter.MinFilter(k))
    edge = Image.eval(Image.merge("LA", (full, inner)).getchannel(0), lambda a: a)
    edge = Image.composite(Image.new("L", (W, H), 0), full, inner)
    d.bitmap((0, 0), edge, fill=255)


_fonts = {}


def text(d, s, cx, cy, cap_px, track=0.0, max_w=None):
    """`s` in Arial Bold, its capitals `cap_px` tall (less, if that would be
    wider than `max_w` pixels), centred on cx, cy."""
    while True:
        size = int(round(P(cap_px) / 0.716))  # Arial's cap height is 0.716 em
        f = _fonts.get(size)
        if f is None:
            f = _fonts[size] = ImageFont.truetype(str(FONT), size)
        widths = [f.getlength(c) for c in s]
        tw = sum(widths) + P(track) * (len(s) - 1)
        if max_w is None or tw <= P(max_w) or cap_px < 2.0:
            break
        cap_px *= 0.96
    x = cx - tw / 2.0
    for c, w in zip(s, widths):
        # The caps sit on the baseline; centre their height on cy.
        d.text((x, cy + P(cap_px) / 2.0), c, font=f, fill=255, anchor="ls")
        x += w + P(track)


def shoulder(right):
    """The New 3DS's L / R: a wide key with the outer top corner rounded
    right round, as the shoulder wraps the case."""
    x0, y0, x1, y1 = P(1.6), P(3.4), P(24.4), P(19.6)
    big, small = P(9.0), P(3.0)

    def fn(d):
        d.rounded_rectangle([x0, y0, x1, y1], radius=small, fill=255)
        # Cut the outer top corner back to a big radius.
        cx = x1 - big if right else x0 + big
        corner = [x1 - big, y0, x1, y0 + big] if right else [x0, y0, x0 + big, y0 + big]
        d.rectangle(corner, fill=0)
        d.pieslice([cx - big, y0, cx + big, y0 + 2 * big],
                   start=270 if right else 180, end=360 if right else 270,
                   fill=255)
    return fn


def pill(x0, y0, x1, y1):
    def fn(d):
        d.rounded_rectangle([P(x0), P(y0), P(x1), P(y1)],
                            radius=(P(y1) - P(y0)) / 2.0, fill=255)
    return fn


def cross(arm, half):
    def fn(d):
        d.rounded_rectangle([CX - P(arm), CY - P(half), CX + P(arm), CY + P(half)],
                            radius=P(1.2), fill=255)
        d.rounded_rectangle([CX - P(half), CY - P(arm), CX + P(half), CY + P(arm)],
                            radius=P(1.2), fill=255)
    return fn


def icon(name, face_colours):
    ink = Ink()
    if name in ("A", "B", "X", "Y"):
        ring(ink.layer(LIME), CX, CY, 9.5)
        rgb = FACE_RGB[name] if face_colours else LIME
        text(ink.layer(rgb), name, CX, CY, 9.6)
    elif name in ("L", "R"):
        outline(ink.layer(LIME), shoulder(name == "R"))
        text(ink.layer(LIME), name, CX + P(-0.6 if name == "R" else 0.6),
             CY - P(0.0), 9.0)
    elif name in ("ZL", "ZR"):
        outline(ink.layer(LIME), pill(0.8, 4.2, 25.2, 18.8))
        text(ink.layer(LIME), name, CX, CY, 8.0, track=0.2, max_w=13.6)
    elif name == "CPAD":
        ring(ink.layer(LIME), CX, CY, 9.5)
        ring(ink.layer(LIME, 170), CX, CY, 5.6, 1.5)
        ring(ink.layer(LIME, 120), CX, CY, 2.2, 1.4)
    elif name == "CSTICK":
        ring(ink.layer(LIME), CX, CY, 9.5)
        ring(ink.layer(LIME, 150), CX, CY, 6.6, 1.3)
        text(ink.layer(LIME), "C", CX, CY, 7.0)
    elif name == "DPAD":
        outline(ink.layer(LIME), cross(3.7, 10.0))
        d = ink.layer(LIME)
        a, t = P(7.0), P(1.9)  # the arrows' distance out and half-width
        for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)):
            tip = (CX + dx * (a + t * 0.8), CY + dy * (a + t * 0.8))
            bx, by = CX + dx * (a - t * 0.4), CY + dy * (a - t * 0.4)
            d.polygon([tip, (bx - dy * t, by + dx * t), (bx + dy * t, by - dx * t)],
                      fill=255)
    elif name in ("SELECT", "START"):
        outline(ink.layer(LIME), pill(0.6, 5.0, 25.4, 18.0), 1.6)
        text(ink.layer(LIME), name, CX, CY, 5.0, track=0.15, max_w=18.0)
    return ink.render()


def sheet(face_colours):
    out = Image.new("RGBA", (SHEET, SHEET), (0, 0, 0, 0))
    for i, name in enumerate(ICONS):
        out.paste(icon(name, face_colours),
                  ((i % 4) * CELL_W, (i // 4) * CELL_H + ART_TOP))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", help="also write a 4x PNG of the sheet")
    ap.add_argument("--face-colours", action="store_true",
                    help="A / B / X / Y letters in the New 3DS colours")
    ap.add_argument("--no-bin", action="store_true",
                    help="write only the preview")
    a = ap.parse_args()
    s = sheet(a.face_colours)
    if not a.no_bin:
        OUT.parent.mkdir(parents=True, exist_ok=True)
        OUT.write_bytes(s.tobytes())
        print("%s: %d bytes, %d icons" % (OUT, SHEET * SHEET * 4, len(ICONS)))
    if a.preview:
        bg = Image.new("RGBA", s.size, (40, 40, 48, 255))
        bg.alpha_composite(s)
        bg.resize((SHEET * 4, SHEET * 4), Image.NEAREST).save(a.preview)


if __name__ == "__main__":
    main()
