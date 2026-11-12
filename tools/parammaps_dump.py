"""Dump a layer of a track's ParameterMaps group (chunk 0x8003B600), as parammaps.c reads it.

usage: python tools/parammaps_dump.py "PC Files/TRACKS/L4RA.BUN" [layer name]

With no name, lists the layers (by matched name where known). With a name, prints every
set's fields and a coarse map of which set covers which part of the layer's bounds.
"""
import struct
import sys

NAMES = ["Night Normal Fog", "Normal Fog", "Night Screen Tint", "Sunrise Screen Tint", "Rain",
         "Wind", "Night Raining Fog", "Horizon Fog Visibility", "Volumes"]


def shash(s):
    h = 0xFFFFFFFF
    for c in s:
        h = (h * 33 + ord(c)) & 0xFFFFFFFF
    return h


HN = {shash(n): n for n in NAMES}


def chunks(d, o, end):
    while o + 8 <= end:
        i, n = struct.unpack_from("<II", d, o)
        if n > end - o - 8:
            return
        yield i, o + 8, n
        o += 8 + n


def layers(path):
    d = open(path, "rb").read()
    for i, p, n in chunks(d, 0, len(d)):
        if i != 0x8003B600:
            continue
        for li, lp, ln in chunks(d, p, p + n):
            if li != 0x8003B601:
                continue
            L = {"quad": None}
            for ci, cp, cn in chunks(d, lp, lp + ln):
                if ci == 0x3B602:
                    (L["hash"], L["x0"], L["y0"], L["x1"], L["y1"], L["nodes"], L["sets"],
                     L["stride"], L["fields"]) = struct.unpack_from("<I4f4i", d, cp)
                elif ci == 0x3B603:
                    L["types"] = struct.unpack_from("<%dI" % (cn // 4), d, cp)
                elif ci == 0x3B604:
                    L["offs"] = struct.unpack_from("<%dI" % (cn // 4), d, cp)
                elif ci == 0x3B605:
                    L["rows"] = d[cp:cp + cn]
                elif ci in (0x3B607, 0x3B608):
                    L["quad"] = (d[cp:cp + cn], ci == 0x3B608)
            yield L
        return


def field(L, s, f):
    off = L["offs"][f] if "offs" in L else f * 4
    t = L["types"][f] if "types" in L else 0
    return struct.unpack_from("<i" if t == 1 else "<f", L["rows"], s * L["stride"] + off)[0]


def set_at(L, x, y):
    if L["quad"] is None or not (L["x0"] <= x < L["x1"] and L["y0"] <= y < L["y1"]):
        return 0
    q, q16 = L["quad"]
    w = 2 if q16 else 1
    fmt = "<H" if q16 else "<B"
    x0, y0, x1, y1 = L["x0"], L["y0"], L["x1"], L["y1"]
    node = 0
    for _ in range(24):
        c = [struct.unpack_from(fmt, q, (node * 4 + k) * w)[0] for k in range(4)]
        if c[0] == 0:
            return c[1]
        mx, my = (x0 + x1) * 0.5, (y0 + y1) * 0.5
        hx, hy = x >= mx, y >= my
        node = c[(2 if hx else 0) + (1 if hy else 0)]
        x0, x1 = (mx, x1) if hx else (x0, mx)
        y0, y1 = (my, y1) if hy else (y0, my)
    return 0


def main():
    want = sys.argv[2] if len(sys.argv) > 2 else None
    for L in layers(sys.argv[1]):
        name = HN.get(L["hash"], "0x%08x" % L["hash"])
        if not want:
            print("%-24s sets %3d fields %2d bounds (%.0f,%.0f)-(%.0f,%.0f) nodes %d"
                  % (name, L["sets"], L["fields"], L["x0"], L["y0"], L["x1"], L["y1"], L["nodes"]))
            continue
        if name != want:
            continue
        for s in range(L["sets"]):
            print("set %3d:" % s, " ".join("%.3g" % field(L, s, f) for f in range(L["fields"])))
        cols, rows = 64, 32
        print("map (x right, y up), set index mod 36:")
        for r in range(rows):
            y = L["y1"] - (r + 0.5) * (L["y1"] - L["y0"]) / rows
            line = ""
            for c in range(cols):
                x = L["x0"] + (c + 0.5) * (L["x1"] - L["x0"]) / cols
                s = set_at(L, x, y)
                line += "0123456789abcdefghijklmnopqrstuvwxyz"[s % 36]
            print(line)


if __name__ == "__main__":
    main()
