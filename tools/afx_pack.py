#!/usr/bin/env python3
"""Pack the retail Acid particle effects the free-roam world uses into
ACIDFX.nfsa, for 3DS Files/source/afx.c (TASKS.md #174).

What the effects are. Every event marker, shop and bank in free roam is drawn
by the game's particle system ("Acid", after the AcidEmitter / AcidEmitterData
names in the .BUN chunks), not by geometry:

  * a floating mode icon (MODE_ICON_CIRCUIT, SHOP_ICON_BODY, ...) re-spawned
    twice a second, each one zooming in from 15 m and fading,
  * a ground ring (XE_RINGWAVEA, a 2x2 atlas, laid flat and spun at random)
    that rises off the road at 0.4 m/s, and
  * for the shops and the SUV / showcase / DVD / world-challenge markers, smoke
    (SFX_SMOKE_A_CS / SFX_SMOKE_B_CS) that sprays out of the ring.

There are two ways an effect gets into the world, and this packs both:

  1. Trigger effects. UpdateTriggerEffects (SPEED2.EXE 0x605780) keeps twelve
     slots, one per entry of the table at 0x802300 -- XEE_EVENT_CIRCUIT ..
     XEE_BANK -- and each frame puts each slot on the nearest unlocked zone of
     its type within 200 m of the car (SetEffectLocation, 0x6054b0). A slot's
     emitters are copied from every emitter whose data names that group
     (0x48b8b0), which in the shipped data are the state-3 "template"
     instances in GLOBAL/GLOBALB.BUN's 0x35020 chunk. Their own matrices are
     thrown away: the copy gets identity plus the zone position, so the
     template's +0x60 offset is what lifts the icon 3 m off the road.
  2. Placed emitters. The shops and the industrial smoke stacks are ordinary
     0x35020 instances in the TRACKS/STREAM<track>.BUN sections, with their own
     matrices, most of them gated on the FREE_ROAM condition (0xea5e404f).
     Sections overlap, so the same emitter appears in several of them; those
     are deduplicated on (name, position).
  3. The props' emitters (TASKS.md #318). A SmokeableInfo names two groups
     the same way, +0x84 (fired when the prop's anchor gives) and +0x88
     (fired when it first wakes): the crash barrels' BARREL_WATERA water,
     the greenhouse door's glass. Smokeable::Init (Speed.elf) copies every
     member into the prop, state 1, and AddAutoEmitter starts them; afx.c's
     afx_attach() does both. They follow the trigger groups, by hash.

Their AcidEmitterData (chunk 0x35021, 0x220-byte records) come from
GLOBALB.BUN and TRACKS/<track>.BUN. Only the 0x180-byte runtime prefix is kept
-- the rest is the three name strings -- and it is stored byte for byte, so
afx.c reads the retail field offsets directly. The textures come from
GLOBALB.BUN's Global\\GlobalTextures.tpk (the icons) and
GLOBAL/InGameCommon.bun's Global\\InGameTextures.tpk (ring and smoke), each
with its record's alphaBlendType: the ring is additive, the rest normal blend.

File layout, little-endian:

    header   'NFSA' u32 version, u16 nTex nTmpl nGroup nInst,
             u32 texOff tmplOff groupOff instOff pixOff
    tex      nTex x  { u32 hash; u16 w, h; u16 fmt|maxLevel<<8; u8 blend;
                       u8 pad; u32 offset from pixOff }
    tmpl     nTmpl x { u8 data[0x180]; u16 tex (0xffff none); u16 pad }
    group    nGroup x { u32 hash; u8 count; u8 pad[3]; u16 tmpl[4] } -- the
             twelve trigger slots, then the props' groups
    inst     nInst x { u16 tmpl; s16 hold; u32 cond; u32 name;
                       f32 m[16]; f32 inherit[3] }
    pixels   car_pack.encode_texture_chain() output, 128-byte aligned

    python tools/afx_pack.py "PC Files" --out <sdmc>/nfsu2data/TRACKS/L4RA
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import car_pack
import extract_global_textures as egt
import smokeable_pack
import track_pack
from dxt_transcode import decode_bgra, decode_dxt

AFX_NAME = "ACIDFX.nfsa"
AFX_VERSION = 1

EMITTER_CHUNK = 0x35020         # AcidEmitter instances, version 6, 0xd0 each
EMITTER_DATA_CHUNK = 0x35021    # AcidEmitterData, version 4, 0x220 each
EMITTER_VERSION = 6
EMITTER_SIZE = 0xD0
DATA_VERSION = 4
DATA_SIZE = 0x220
DATA_RUNTIME = 0x180            # everything before the name strings
STATE_TEMPLATE = 3

# SPEED2.EXE 0x802300: { category, zone type, group name } x 12, in slot order.
# afx.c and main.c index the slots by this order.
TRIGGER_GROUPS = [
    "XEE_EVENT_CIRCUIT", "XEE_EVENT_DRAG", "XEE_EVENT_STREET",
    "XEE_EVENT_DRIFT", "XEE_EVENT_SPRINT", "XEE_EVENT_URL", "XEE_SHOWCASE",
    "XEE_WORLD_CHALLENGE", "XEE_EVENT_SUV", "XEE_DVD", "XEE_GAME_TIP",
    "XEE_BANK",
]
GROUP_MAX = 4

NO_TEX = 0xFFFF
PIX_ALIGN = 128


def walk(d, start, end):
    """Every chunk in [start, end), descending into containers (bit 31)."""
    o = start
    while o + 8 <= end:
        cid, sz = struct.unpack_from("<II", d, o)
        if o + 8 + sz > end:
            break
        yield o, cid, sz
        if cid & 0x80000000:
            yield from walk(d, o + 8, o + 8 + sz)
        o += 8 + sz


def chunk_records(d, cid_want, version, size):
    """The fixed-size records of every `cid_want` chunk, in file order. The
    payload is 0x11-padded to 16 bytes, then { u32 ?, u32 ?, u32 version,
    u32 count } and the records."""
    for co, cid, sz in walk(d, 0, len(d)):
        if cid != cid_want:
            continue
        p = (co + 8 + 15) & ~15
        _, _, ver, count = struct.unpack_from("<4I", d, p)
        if ver != version:
            continue
        p += 16
        for i in range(count):
            r = d[p + i * size:p + (i + 1) * size]
            if len(r) == size:
                yield r


def cstr(b):
    return b.split(b"\0")[0].decode("ascii", "replace")


def load_data(paths):
    """{data name hash: 0x220-byte record}. The first file to define a hash
    wins, as in retail, where SetEmitterData (0x48bc00) takes the first match
    off a list that each bundle appends to as it loads -- GLOBALB first."""
    out = {}
    for p in paths:
        with open(p, "rb") as fh:
            d = fh.read()
        for r in chunk_records(d, EMITTER_DATA_CHUNK, DATA_VERSION, DATA_SIZE):
            out.setdefault(struct.unpack_from("<I", r, 8)[0], r)
    return out


def load_emitters(path):
    with open(path, "rb") as fh:
        d = fh.read()
    return list(chunk_records(d, EMITTER_CHUNK, EMITTER_VERSION, EMITTER_SIZE))


def emitter_fields(e):
    name_hash, data_hash, state = struct.unpack_from("<3I", e, 8)
    hold = struct.unpack_from("<h", e, 0x28)[0]
    cond = struct.unpack_from("<I", e, 0x2C)[0]
    m = struct.unpack_from("<16f", e, 0x40)
    inherit = struct.unpack_from("<3f", e, 0x80)
    return dict(name_hash=name_hash, data_hash=data_hash, state=state,
                hold=hold, cond=cond, m=m, inherit=inherit,
                name=cstr(e[0x90:0xD0]))


def find_textures(paths, wanted):
    """{texture hash: (name, w, h, rgba, alphaBlendType)} for each hash in
    `wanted`, from the .tpk containers inside the given bundles."""
    found = {}
    for p in paths:
        with open(p, "rb") as fh:
            data = fh.read()
        for coff, csize in egt.find_containers(data):
            parsed = egt.parse_container(data, coff, csize)
            for t in parsed["textures"]:
                h = t["hash"]
                if h not in wanted or h in found:
                    continue
                px = t["pixel_data"][:t["expected_size"]]
                if t["fourcc"] == b"RAWB":
                    rgba = decode_bgra(px, t["width"], t["height"])
                else:
                    rgba = decode_dxt(px, t["width"], t["height"], t["fourcc"])
                found[h] = (t["name"], t["width"], t["height"], bytes(rgba),
                            t["alpha_blend"], t["fourcc"])
    return found


def wants_rgba8(fourcc):
    """32bpp only for the art that was 32bpp to begin with. A DXT texture's
    colour is 565 and its alpha (DXT3) four bits already, so RGBA4/RGB565
    keeps all of the alpha and nearly all of the colour; raw ARGB art carries
    smooth 8-bit alpha that 16 levels would band across a stack of particles."""
    return fourcc == b"RAWB"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_files", help='the "PC Files" directory')
    ap.add_argument("--out", metavar="DIR", required=True,
                    help="write %s into DIR" % AFX_NAME)
    ap.add_argument("--track", default="L4RA")
    args = ap.parse_args()

    gdir = os.path.join(args.pc_files, "GLOBAL")
    tdir = os.path.join(args.pc_files, "TRACKS")
    globalb = os.path.join(gdir, "GLOBALB.BUN")
    track_bun = os.path.join(tdir, args.track + ".BUN")
    stream_bun = os.path.join(tdir, "STREAM" + args.track + ".BUN")
    for p in (globalb, track_bun, stream_bun):
        if not os.path.isfile(p):
            sys.exit("missing %s" % p)

    data = load_data([globalb, track_bun])
    tmpl_index = {}         # data hash -> template slot
    templates = []          # data records, in slot order

    def template(data_hash):
        if data_hash not in data:
            return None
        if data_hash not in tmpl_index:
            tmpl_index[data_hash] = len(templates)
            templates.append(data[data_hash])
        return tmpl_index[data_hash]

    # 1. The trigger groups: every emitter loaded at init whose data names the
    #    group, in list (= file) order -- GLOBALB, then the track bundle.
    loaded = [emitter_fields(e) for e in load_emitters(globalb)]
    loaded += [emitter_fields(e) for e in load_emitters(track_bun)]
    groups = []
    for name in TRIGGER_GROUPS:
        gh = track_pack.bstring_hash(name)
        members = []
        for e in loaded:
            r = data.get(e["data_hash"])
            if r is None or struct.unpack_from("<I", r, 0x10)[0] != gh:
                continue
            members.append(template(e["data_hash"]))
        if len(members) > GROUP_MAX:
            sys.exit("%s has %d emitters, the format holds %d"
                     % (name, len(members), GROUP_MAX))
        groups.append((gh, members))
        print("slot %-2d %-20s %d emitter(s)" % (len(groups) - 1, name, len(members)))

    # The props' groups: every emitter key the track's SmokeableInfos name
    # (+0x84 anchor, +0x88 awaken) that some loaded emitter's data answers to.
    keys = set()
    for name, r in smokeable_pack.read_infos(open(track_bun, "rb").read()).values():
        keys.update(k for k in struct.unpack_from("<2I", r, 0x84) if k)
    for gh in sorted(keys):
        members = []
        for e in loaded:
            r = data.get(e["data_hash"])
            if r is not None and struct.unpack_from("<I", r, 0x10)[0] == gh:
                members.append(template(e["data_hash"]))
        if not members:
            print("prop group %08x: no emitters" % gh)
            continue
        if len(members) > GROUP_MAX:
            sys.exit("prop group %08x has %d emitters, the format holds %d"
                     % (gh, len(members), GROUP_MAX))
        groups.append((gh, members))
        print("prop group %08x %d emitter(s)" % (gh, len(members)))

    # 2. The placed emitters, deduplicated across overlapping sections.
    insts = []
    seen = set()
    conds = {}
    for raw in load_emitters(stream_bun):
        e = emitter_fields(raw)
        if e["state"] == STATE_TEMPLATE:
            continue
        key = (e["name"], tuple(round(v, 2) for v in e["m"][12:15]))
        if key in seen:
            continue
        seen.add(key)
        ti = template(e["data_hash"])
        if ti is None:
            print("  skip %s: no data %08x" % (e["name"], e["data_hash"]))
            continue
        conds[e["cond"]] = conds.get(e["cond"], 0) + 1
        insts.append((ti, e))
    print("%d placed emitter(s); conditions %s" % (len(insts), ", ".join(
        "%08x x%d" % kv for kv in sorted(conds.items()))))

    # 3. The textures the templates name.
    wanted = {}
    for r in templates:
        wanted.setdefault(struct.unpack_from("<I", r, 0x0C)[0], len(wanted))
    tex = find_textures([globalb, os.path.join(gdir, "InGameCommon.bun")],
                        set(wanted))
    tex_order = [h for h in sorted(wanted, key=wanted.get) if h in tex]
    for h in wanted:
        if h not in tex:
            print("  WARNING: texture %08x not found" % h)
    tex_slot = {h: i for i, h in enumerate(tex_order)}

    tex_entries = []
    pixels = bytearray()
    for h in tex_order:
        name, w, hgt, rgba, blend, fourcc = tex[h]
        fmt, blob, maxlvl = car_pack.encode_texture_chain(
            rgba, w, hgt, rgba8=wants_rgba8(fourcc))
        while len(pixels) % PIX_ALIGN:
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
        tmpl_blob += r[:DATA_RUNTIME]
        tmpl_blob += struct.pack("<HH", tex_slot.get(th, NO_TEX), 0)
    group_blob = bytearray()
    for gh, members in groups:
        ids = members + [0xFFFF] * (GROUP_MAX - len(members))
        group_blob += struct.pack("<IB3x4H", gh, len(members), *ids)
    inst_blob = bytearray()
    for ti, e in insts:
        inst_blob += struct.pack("<HhII16f3f", ti, e["hold"], e["cond"],
                                 e["name_hash"], *e["m"], *e["inherit"])

    hdr_size = 4 + 4 + 8 + 20
    tex_off = (hdr_size + 15) & ~15
    tmpl_off = tex_off + len(tex_entries) * 16
    group_off = tmpl_off + len(tmpl_blob)
    inst_off = group_off + len(group_blob)
    pix_off = (inst_off + len(inst_blob) + PIX_ALIGN - 1) & ~(PIX_ALIGN - 1)
    out = bytearray()
    out += b"NFSA" + struct.pack("<I4H5I", AFX_VERSION, len(tex_entries),
                                 len(templates), len(groups), len(insts),
                                 tex_off, tmpl_off, group_off, inst_off, pix_off)
    out += bytes(tex_off - len(out))
    for t in tex_entries:
        out += t
    out += tmpl_blob + group_blob + inst_blob
    out += bytes(pix_off - len(out))
    out += pixels

    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, AFX_NAME)
    tmp = path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(out)
    os.replace(tmp, path)
    print("wrote %s: %d textures, %d templates, %d groups, %d placed, %d B"
          % (path, len(tex_entries), len(templates), len(groups), len(insts),
             len(out)))


if __name__ == "__main__":
    main()
