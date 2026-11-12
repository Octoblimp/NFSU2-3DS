#!/usr/bin/env python3
"""Pack the car's own particle effects into GLOBAL/CARFX.nfsa, for
3DS Files/source/carfx.c (TASKS.md #372: the Nitrous Purge spray).

What retail does. CarRenderInfo keeps one emitter list per CarEffect
(CarEffectParameters, Speed.elf 0x803712FC). The purge spray is effect 13,
CARFX_NOS_BLOWOFF, which also fires 14, CARFX_NOS_BLOWOFF_TIRES. Their lists
are not filled from the CARFX_ group names like the other effects; the fitted
purge part (car slot 0xA2) names them. UpdateCarParts (Speed.elf 0x802334D0)
reads its attributes:

  * HOODEMITTER, WHEELEMITTER  the emitter groups for list 13 and list 14:
    XES_NOSBO_A/B/C_Hood / _Wheel, by the TYPE 1/2/3 part families.
  * HOODLEFT, HOODRIGHT, WHEELLEFT, WHEELRIGHT  which nozzles spray. A hood
    nozzle sits on marker 0xE3DE69DB (left) or 0x601A6CCE (right) of the
    BASE model; a wheel nozzle at the front wheel's leading edge (the
    CarPhysicsInfo wheel position plus its radius forward). The left hood and
    right wheel nozzles take the emitter's own matrix, the other two mirror
    it across y.
  * HOODHUE/SAT/LUM, TIREHUE/SAT/LUM  a colour shift: afxShiftEmitterColours
    (0x800C0E54) moves each of the four colour keys in HSL and rebuilds the
    colour cubic with hermite_basis (0x800BF93C).

GarageMainScreen::TriggerCarEffect calls it every tick while Y is held on the
Nitrous Purge list: each emitter copy bursts once (TriggerEmitter with a
negative step) at every position of its kind.

This file is an ordinary afx.c pack (see afx_pack.py: header, textures,
templates; no groups or placed emitters) with the purge table appended and
found from the last eight bytes:

    purge    'NFSP' u16 nGroup, u16 nPart
             group x nGroup  { u32 hash; u8 count; u8 pad[3];
                               4 x { u16 tmpl; u16 posType; f32 rot[9] } }
             part  x nPart   { u32 hash; u32 hoodGroup; u32 wheelGroup;
                               u8 flags (1 HOODLEFT, 2 HOODRIGHT,
                               4 WHEELLEFT, 8 WHEELRIGHT); u8 pad[3];
                               f32 hoodColour[16]; f32 tireColour[16] }
             sorted by part hash
    trailer  u32 purgeOff; 'PRGT'

The colour cubics are worked out here, once per part: every NOSBO template
carries the same keys, and a check against the stored template cubics keeps
the maths honest (hermite_basis reproduces them to 0.0003).

    python tools/carfx_pack.py "PC Files" --out <sdmc>/nfsu2data/GLOBAL
"""
import argparse
import colorsys
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import afx_pack as A
import car_pack
import stockparts_pack as S
import track_pack

OUT_NAME = "CARFX.nfsa"
PURGE_SLOT = 0xA2
H = track_pack.bstring_hash
# CARFX_NITRO the car's nitrous flame (#317); XECH_FLAG_GIRL_FLARE the flag
# girl's two hand flares in the race-start scenes (#497, FlagGirlFlareEffect,
# Speed.elf 0x8004EF84: afxGetEmitters of that group): GlowA, SmokeA, SprksA.
WORLD_GROUPS = ["CARFX_NITRO", "XECH_FLAG_GIRL_FLARE",
                # The breakable items' glass (#513): CarDamage::ApplyDamage's
                # TriggerEffect. The PC data has emitters for these ten of the
                # fifteen CARFX_BREAK_* (none for the mirrors, the plates and
                # the centre brakelight).
                "CARFX_BREAK_HEADLIGHT_LEFT", "CARFX_BREAK_HEADLIGHT_RIGHT",
                "CARFX_BREAK_BRAKELIGHT_LEFT", "CARFX_BREAK_BRAKELIGHT_RIGHT",
                "CARFX_BREAK_WINDSHIELD", "CARFX_BREAK_WINDOW_REAR",
                "CARFX_BREAK_WINDOW_LEFT_FRONT", "CARFX_BREAK_WINDOW_LEFT_REAR",
                "CARFX_BREAK_WINDOW_RIGHT_FRONT", "CARFX_BREAK_WINDOW_RIGHT_REAR"]
FLAG_KEYS = [("HOODLEFT", 1), ("HOODRIGHT", 2), ("WHEELLEFT", 4), ("WHEELRIGHT", 8)]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(len(b))) for j in range(len(b[0]))]
            for i in range(len(a))]


def inv4(m):
    n = 4
    a = [row[:] + [1.0 if i == j else 0.0 for j in range(n)] for i, row in enumerate(m)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        pv = a[c][c]
        a[c] = [x / pv for x in a[c]]
        for r in range(n):
            if r != c:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [row[n:] for row in a]


def hermite_basis(t, keys):
    """hermite_basis (Speed.elf 0x800BF93C): the cubic through four keys at
    times t, as afx.c's cubic() reads it (rows t^3, t^2, t, 1)."""
    P = [[ti ** 3, ti ** 2, ti, 1.0] for ti in t]
    E = [[0, 0, 0, 1], [1, 1, 1, 1], [0, 0, 1, 0], [3, 2, 1, 0]]
    Hm = [[2, -2, 1, 1], [-3, 3, -2, -1], [0, 0, 1, 0], [1, 0, 0, 0]]
    return mat_mul(Hm, mat_mul(mat_mul(E, inv4(P)), keys))


def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def shift_colours(rec, hue, sat, lum):
    """afxShiftEmitterColours (0x800C0E54) on one template's record: the
    16-float colour cubic it leaves in the override slot."""
    hue = clamp(hue, -1.0, 1.0)
    sat = clamp(sat, -1.0, 1.0)
    times = struct.unpack_from("<4f", rec, 0x90)
    keys = []
    for i in range(4):
        r, g, b, a = struct.unpack_from("<4f", rec, 0xA0 + 16 * i)
        h, l, s = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
        h += hue
        if h > 1.0:
            h -= 1.0
        elif h < 0.0:
            h += 1.0
        s = clamp(s + sat, 0.0, 1.0)
        l = clamp(l + lum, 0.0, 1.0)
        r, g, b = colorsys.hls_to_rgb(h, l, s)
        keys.append([r * 255.0, g * 255.0, b * 255.0, a])
    c = hermite_basis([x / 3.0 for x in times], keys)
    return [v for row in c for v in row]


def attr_float(v):
    return struct.unpack("<f", struct.pack("<I", v))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_files", help='the "PC Files" directory')
    ap.add_argument("--out", metavar="DIR", required=True,
                    help="write %s into DIR" % OUT_NAME)
    args = ap.parse_args()

    gdir = os.path.join(args.pc_files, "GLOBAL")
    globalb = os.path.join(gdir, "GLOBALB.BUN")
    common = os.path.join(gdir, "InGameCommon.bun")
    for p in (globalb, common):
        if not os.path.isfile(p):
            sys.exit("missing %s" % p)

    data = A.load_data([globalb])
    with open(globalb, "rb") as fh:
        db = S.Db(fh.read())
    loaded = [A.emitter_fields(e) for e in A.load_emitters(globalb)]

    # The purge parts and the groups they name.
    want = {H(k): k for k in ["HOODEMITTER", "WHEELEMITTER", "HOODHUE", "HOODSAT",
                              "HOODLUM", "TIREHUE", "TIRESAT", "TIRELUM"] +
            [k for k, _ in FLAG_KEYS]}
    pid = S.SLOT_MAP[PURGE_SLOT]
    parts = []
    for p in db.parts:
        if p.pid != pid:
            continue
        av = {}
        for k, v in p.attrs:
            if k in want and want[k] not in av:
                av[want[k]] = v
        parts.append((p, av))
    group_hashes = []
    for _, av in parts:
        for k in ("HOODEMITTER", "WHEELEMITTER"):
            g = av.get(k, 0)
            if g and g not in group_hashes:
                group_hashes.append(g)

    # The world's car effects (#317): CarRenderInfo's other lists come from
    # the CARFX_ group names (CarEffectParameters, Speed.elf 0x803712FC);
    # afxGetEmitters fills list N with every emitter of group N's hash.
    # Effect 10, CARFX_NITRO, is the nitrous flame TriggerCarTerrainEffects
    # (0x80221D38) fires at the exhaust tips while the bottle burns (and the
    # gear-change backfire at level / 3.5).
    for name in WORLD_GROUPS:
        if H(name) not in group_hashes:
            group_hashes.append(H(name))

    tmpl_index, templates = {}, []
    groups = {}
    for gh in group_hashes:
        members = []
        for e in loaded:
            r = data.get(e["data_hash"])
            if r is None or struct.unpack_from("<I", r, 0x10)[0] != gh:
                continue
            if e["data_hash"] not in tmpl_index:
                tmpl_index[e["data_hash"]] = len(templates)
                templates.append(r)
            m = e["m"]
            rot = [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]]
            members.append((tmpl_index[e["data_hash"]],
                            struct.unpack_from("<I", r, 0x5C)[0], rot, r))
        if not members:
            sys.exit("group %08x: no emitters" % gh)
        if len(members) > A.GROUP_MAX:
            sys.exit("group %08x has %d emitters, the format holds %d"
                     % (gh, len(members), A.GROUP_MAX))
        groups[gh] = members
        if os.environ.get("CARFX_DUMP"):
            for ti, pt, rot, r in members:
                f = lambda o, n=1: struct.unpack_from("<%df" % n, r, o)
                print("  DUMP %s: rate %s life %s speed %s inherit %s maxDist %s grav %s "
                      "drag %s maxScreen %s orient %d lowDetail %d sizeIdx %d cone %d"
                      % (A.cstr(r[0x180:0x1A0]), f(0x34, 2), f(0x3c, 2), f(0x44, 2),
                         f(0x54, 2), f(0x80), f(0x84), f(0x88), f(0x8c), r[0x20], r[0x21],
                         struct.unpack_from("<b", r, 0x1f)[0], r[0x18]))
                print("    colour", ["%.1f" % v for v in f(0x100, 16)])
                print("    size  ", ["%.3f" % v for v in f(0x140, 16)])
                print("    rot", ["%.2f" % v for v in rot])
        print("group %08x: %s" % (gh, ", ".join(
            "%s pos %d" % (A.cstr(r[0x180:0x1A0]), pt) for _, pt, _, r in members)))

    # The maths check: no shift has to give back each template's own cubic.
    worst = 0.0
    for r in templates:
        calc = shift_colours(r, 0.0, 0.0, 0.0)
        stored = struct.unpack_from("<16f", r, 0x100)
        worst = max(worst, max(abs(a - b) for a, b in zip(calc, stored)))
    print("colour cubic check: worst error %.5f" % worst)
    if worst > 0.05:
        sys.exit("the colour maths does not reproduce the templates")

    # The texture(s).
    wanted = {}
    for r in templates:
        wanted.setdefault(struct.unpack_from("<I", r, 0x0C)[0], len(wanted))
    tex = A.find_textures([globalb, common], set(wanted))
    tex_order = [h for h in sorted(wanted, key=wanted.get) if h in tex]
    for h in wanted:
        if h not in tex:
            print("  WARNING: texture %08x not found" % h)
    tex_slot = {h: i for i, h in enumerate(tex_order)}
    tex_entries, pixels = [], bytearray()
    for h in tex_order:
        name, w, hgt, rgba, blend, fourcc = tex[h]
        fmt, blob, maxlvl = car_pack.encode_texture_chain(
            rgba, w, hgt, rgba8=A.wants_rgba8(fourcc))
        while len(pixels) % A.PIX_ALIGN:
            pixels.append(0)
        tex_entries.append(struct.pack("<IHHHBBI", h, w, hgt,
                                       car_pack.tex_fmt_field(fmt, maxlvl),
                                       blend, 0, len(pixels)))
        pixels += blob
        print("  tex %-26s %3dx%-3d %s fmt %d mips %d blend %d %6d B"
              % (name, w, hgt, fourcc.decode(), fmt, maxlvl, blend, len(blob)))

    tmpl_blob = bytearray()
    for r in templates:
        th = struct.unpack_from("<I", r, 0x0C)[0]
        tmpl_blob += r[:A.DATA_RUNTIME]
        tmpl_blob += struct.pack("<HH", tex_slot.get(th, A.NO_TEX), 0)

    # The purge table.
    group_order = list(groups)
    purge = bytearray(b"NFSP")
    rows = []
    for p, av in parts:
        hg, wg = av.get("HOODEMITTER", 0), av.get("WHEELEMITTER", 0)
        flags = 0
        for k, bit in FLAG_KEYS:
            if av.get(k, 0):
                flags |= bit
        hood = [0.0] * 16
        tire = [0.0] * 16
        if hg in groups:
            hood = shift_colours(groups[hg][0][3], attr_float(av.get("HOODHUE", 0)),
                                 attr_float(av.get("HOODSAT", 0)),
                                 attr_float(av.get("HOODLUM", 0)))
        if wg in groups:
            tire = shift_colours(groups[wg][0][3], attr_float(av.get("TIREHUE", 0)),
                                 attr_float(av.get("TIRESAT", 0)),
                                 attr_float(av.get("TIRELUM", 0)))
        rows.append((p.hash, hg, wg, flags, hood, tire, p.name))
    rows.sort(key=lambda x: x[0])
    purge += struct.pack("<HH", len(group_order), len(rows))
    for gh in group_order:
        mem = groups[gh]
        purge += struct.pack("<IB3x", gh, len(mem))
        for k in range(A.GROUP_MAX):
            if k < len(mem):
                ti, pt, rot, _ = mem[k]
                purge += struct.pack("<HH9f", ti, pt, *rot)
            else:
                purge += struct.pack("<HH9f", 0xFFFF, 0, *([0.0] * 9))
    for ph, hg, wg, flags, hood, tire, name in rows:
        purge += struct.pack("<IIIB3x16f16f", ph, hg, wg, flags, *hood, *tire)
    shifted = sum(1 for r in rows if any(abs(a - b) > 0.5 for a, b in
                                         zip(r[4] + r[5], rows[0][4] + rows[0][5])))
    print("%d purge parts (%d tinted away from the first), %d groups"
          % (len(rows), shifted, len(group_order)))

    hdr_size = 4 + 4 + 8 + 20
    tex_off = (hdr_size + 15) & ~15
    tmpl_off = tex_off + len(tex_entries) * 16
    group_off = tmpl_off + len(tmpl_blob)
    inst_off = group_off
    pix_off = (inst_off + A.PIX_ALIGN - 1) & ~(A.PIX_ALIGN - 1)
    out = bytearray()
    out += b"NFSA" + struct.pack("<I4H5I", A.AFX_VERSION, len(tex_entries),
                                 len(templates), 0, 0,
                                 tex_off, tmpl_off, group_off, inst_off, pix_off)
    out += bytes(tex_off - len(out))
    for t in tex_entries:
        out += t
    out += tmpl_blob
    out += bytes(pix_off - len(out))
    out += pixels
    while len(out) % 4:
        out.append(0)
    purge_off = len(out)
    out += purge
    out += struct.pack("<I", purge_off) + b"PRGT"

    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, OUT_NAME)
    tmp = path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(out)
    os.replace(tmp, path)
    print("wrote %s: %d textures, %d templates, %d B" % (path, len(tex_entries),
                                                        len(templates), len(out)))


if __name__ == "__main__":
    main()
