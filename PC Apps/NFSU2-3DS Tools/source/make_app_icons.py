#!/usr/bin/env python3
"""
Draws the three NFSU2-3DS PC apps' icons (TASKS.md #472) into
tools/app_icons/: setup.ico (Data Setup: an SD card taking a download, blue),
mods.ico (Mod Packer: a wrench, orange) and cli.ico (the nfsu2-3ds command
line: a prompt, dark). Each .ico holds 16 to 256 px; a .png of each sits
beside it. tools/build_apps.py puts them on the exes. Needs Pillow.

    python tools/make_app_icons.py
"""

import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "app_icons")
N = 1024                      # drawn large, then scaled down smooth
SIZES = [16, 24, 32, 48, 64, 128, 256]


def base(top, bottom):
    """A rounded square with a top-to-bottom gradient."""
    grad = Image.new("RGBA", (N, N))
    d = ImageDraw.Draw(grad)
    for y in range(N):
        t = y / (N - 1)
        d.line([(0, y), (N, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    mask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(mask).rounded_rectangle([24, 24, N - 24, N - 24], radius=200, fill=255)
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    return img


def setup_icon():
    img = base((52, 132, 230), (18, 62, 150))
    # The SD card on its own layer: a rounded body, the top-left corner cut
    # off, gold contacts along the top.
    layer = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0, x1, y1 = 250, 230, 774, 860
    d.rounded_rectangle([x0, y0, x1, y1], radius=60, fill=(245, 248, 255, 255))
    d.polygon([(x0 - 40, y0 - 40), (x0 + 170, y0 - 40), (x0 + 170, y0),
               (x0, y0 + 170), (x0 - 40, y0 + 170)], fill=(0, 0, 0, 0))
    for i in range(4):
        cx = x0 + 200 + i * 80
        d.rounded_rectangle([cx, y0 + 40, cx + 50, y0 + 150], radius=12, fill=(232, 180, 40))
    # The download arrow on the card.
    blue = (24, 84, 190)
    d.rectangle([462, 380, 562, 620], fill=blue)
    d.polygon([(372, 600), (652, 600), (512, 760)], fill=blue)
    d.rounded_rectangle([352, 780, 672, 812], radius=16, fill=blue)
    img.alpha_composite(layer)
    return img


def mods_icon():
    img = base((250, 150, 50), (205, 85, 20))
    layer = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    white = (255, 250, 240, 255)
    # A wrench drawn upright, then turned 45 degrees: the handle, the head
    # and its open jaw.
    cx = N // 2
    d.rounded_rectangle([cx - 70, 420, cx + 70, 900], radius=70, fill=white)
    d.ellipse([cx - 210, 140, cx + 210, 560], fill=white)
    d.rectangle([cx - 80, 100, cx + 80, 330], fill=(0, 0, 0, 0))
    d.ellipse([cx - 80, 250, cx + 80, 410], fill=(0, 0, 0, 0))
    d.ellipse([cx - 40, 790, cx + 40, 870], fill=(0, 0, 0, 0))
    layer = layer.rotate(-45, resample=Image.BICUBIC, center=(N / 2, N / 2 + 20))
    # The bits cut from the head were drawn transparent onto the layer, so
    # paste only the layer's own opaque pixels.
    img.alpha_composite(layer)
    return img


def cli_icon():
    img = base((58, 64, 74), (22, 25, 30))
    d = ImageDraw.Draw(img)
    # A window bar with three dots, then the prompt.
    d.rounded_rectangle([120, 150, 904, 250], radius=40, fill=(80, 88, 100))
    for i, c in enumerate([(240, 90, 80), (240, 190, 70), (90, 200, 110)]):
        d.ellipse([170 + i * 80, 175, 220 + i * 80, 225], fill=c)
    green = (110, 235, 120)
    w = 70
    pts = [(250, 420), (480, 590), (250, 760)]
    d.line(pts, fill=green, width=w, joint="curve")
    for p in (pts[0], pts[2]):
        d.ellipse([p[0] - w / 2, p[1] - w / 2, p[0] + w / 2, p[1] + w / 2], fill=green)
    d.rounded_rectangle([540, 720, 800, 790], radius=30, fill=green)
    return img


def save(img, name):
    os.makedirs(OUT, exist_ok=True)
    big = img.resize((256, 256), Image.LANCZOS)
    big.save(os.path.join(OUT, name + ".png"))
    img.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, name + ".ico"),
                                               sizes=[(s, s) for s in SIZES])
    print("wrote app_icons/%s.ico and .png" % name)


def main():
    save(setup_icon(), "setup")
    save(mods_icon(), "mods")
    save(cli_icon(), "cli")


if __name__ == "__main__":
    main()
