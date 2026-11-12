#!/usr/bin/env python3
r"""Extract the car-paint colour table out of GLOBAL/GLOBALB.BUN.

The retail game does not store a colour in the save. A saved car's BASE_PAINT
slot holds the *hash of a part name* -- GLOSS_L3_TEST20, RIMS_L1_COLOR08 -- and
the RGB lives in that part's attribute list in the car-parts database. This
tool walks that database and writes the hash -> RGB table the port needs, so a
player's real paint can be applied at runtime instead of a baked-in stand-in.

Where the data is, all located by chunk id rather than by offset:

  0x80034602                        the car-parts database container
    0x00034603  header, 15 u32      u32[8] = attribute count, u32[14] = part count
    0x00034606  string pool         part names, NUL-separated
    0x0003460c  u16 array           attribute-index lists (see below)
    0x00034605  count x 8 B         { u32 keyHash, u32 value } attributes
    0x0003460a  1580 x 36 B         (not needed here)
    0x0003460b  300 B               (not needed here)
    0x00034604  count x 14 B        part records

A part record is { u32 nameHash, u16, u16, u16 nameOff4, u16 attrList, u16 },
where nameOff4 * 4 is the part name's byte offset in the string pool, and
attrList indexes 0x0003460c at a count followed by that many u16 indices into
the 0x00034605 attribute array. Attribute keys are bStringHash of their name,
so RED/GREEN/BLUE are recognised by hash and their values are plain 0..255.

All of that was recovered by resolving the observed key hashes back to strings
in this database's own string pool -- RED, GREEN, BLUE, TEXTURE, DISPRED,
NUM_DECALS and the rest come out of the retail data rather than from guesswork
-- and then checked by following the cars in a real save through to colours
that suit their part names (the GLOSS_* entries dark blues, RIMS_L1_COLOR08 a
bright orange).

Usage:
    python tools/paint_pack.py "PC Files" --out "<sd>/nfsu2data/GLOBAL"
    python tools/paint_pack.py "PC Files" --list      # print the table
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from track_pack import bstring_hash

PARTS_CONTAINER = 0x80034602
CH_HEADER = 0x00034603
CH_STRINGS = 0x00034606
CH_ATTRLISTS = 0x0003460C
CH_ATTRS = 0x00034605
CH_PARTS = 0x00034604

PART_RECORD_SIZE = 14
ATTR_RECORD_SIZE = 8

GLOBAL_BUN = os.path.join("GLOBAL", "GLOBALB.BUN")

PAINT_MAGIC = b"NFSP"
PAINT_VERSION = 2
PAINT_HDR = 0x10
OUT_NAME = "PAINT.nfsp"

# The three attribute keys that make a part a colour. Hashed rather than
# written out as constants, so the derivation stays visible.
KEY_RED = bstring_hash("RED")
KEY_GREEN = bstring_hash("GREEN")
KEY_BLUE = bstring_hash("BLUE")
# Not part of what makes a colour, but carried beside it: CompositeSkin
# (SPEED2.EXE 0x61D790) builds every paint as R | G<<8 | B<<16 | GLOSS<<24,
# so it is the alpha of the car skin. Absent reads as 0, as retail's
# attribute lookup does.
KEY_GLOSS = bstring_hash("GLOSS")


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def top_chunks(data):
    """(id, body_offset, size) for every chunk at the top level of a bundle."""
    out, off = [], 0
    while off + 8 <= len(data):
        cid, size = struct.unpack_from("<II", data, off)
        if size <= 0 or off + 8 + size > len(data):
            break
        out.append((cid, off + 8, size))
        off += 8 + size
    return out


def children(data, body, size):
    out, off, end = {}, body, body + size
    while off + 8 <= end:
        cid, sz = struct.unpack_from("<II", data, off)
        if sz <= 0 or off + 8 + sz > end:
            break
        out[cid] = (off + 8, sz)
        off += 8 + sz
    return out


def find_parts_db(data):
    for cid, body, size in top_chunks(data):
        if cid == PARTS_CONTAINER:
            return children(data, body, size)
    return None


def read_parts(data, verbose=False):
    """[(nameHash, name, {attrKey: value})] for every part in the database."""
    ch = find_parts_db(data)
    if not ch:
        raise SystemExit("no 0x%08x parts container in the bundle" % PARTS_CONTAINER)
    for need in (CH_HEADER, CH_STRINGS, CH_ATTRLISTS, CH_ATTRS, CH_PARTS):
        if need not in ch:
            raise SystemExit("parts container has no 0x%08x chunk" % need)

    hoff, _ = ch[CH_HEADER]
    n_attrs = u32(data, hoff + 8 * 4)
    n_parts = u32(data, hoff + 14 * 4)

    poff, psz = ch[CH_STRINGS]
    pool = data[poff:poff + psz]
    loff, lsz = ch[CH_ATTRLISTS]
    aoff, asz = ch[CH_ATTRS]
    roff, rsz = ch[CH_PARTS]

    # The header counts have to agree with the chunk sizes, or one of the
    # record strides above is wrong and everything downstream of it is noise.
    if asz // ATTR_RECORD_SIZE != n_attrs:
        raise SystemExit("attr chunk is %d B, header says %d records of %d"
                         % (asz, n_attrs, ATTR_RECORD_SIZE))
    if rsz // PART_RECORD_SIZE != n_parts:
        raise SystemExit("part chunk is %d B, header says %d records of %d"
                         % (rsz, n_parts, PART_RECORD_SIZE))
    if verbose:
        print("  parts DB: %d parts, %d attributes, %d B of names"
              % (n_parts, n_attrs, psz))

    out = []
    for i in range(n_parts):
        o = roff + i * PART_RECORD_SIZE
        name_hash = u32(data, o)
        name_off = u16(data, o + 8) * 4
        list_idx = u16(data, o + 10)
        end = pool.find(b"\0", name_off)
        name = pool[name_off:end].decode("ascii", "replace") if end > name_off else ""
        attrs = {}
        if (list_idx + 1) * 2 <= lsz:
            n = u16(data, loff + list_idx * 2)
            # A bad count would walk into the next chunk. The retail data never
            # does, so this is a guard rather than a normal path.
            if (list_idx + 1 + n) * 2 <= lsz:
                for k in range(n):
                    ai = u16(data, loff + (list_idx + 1 + k) * 2)
                    if ai >= n_attrs:
                        continue
                    key, val = struct.unpack_from("<II", data,
                                                  aoff + ai * ATTR_RECORD_SIZE)
                    attrs[key] = val
        out.append((name_hash, name, attrs))
    return out


def paint_table(parts, verbose=False):
    """hash -> (name, r, g, b, gloss) for every part that carries a colour."""
    table = {}
    skipped = 0
    display_named = 0
    for name_hash, name, attrs in parts:
        has = [k in attrs for k in (KEY_RED, KEY_GREEN, KEY_BLUE)]
        if not any(has):
            continue
        if not all(has):
            # Counted rather than silently dropped, so a data set that does
            # this says so out loud instead of quietly losing colours.
            skipped += 1
            continue
        r, g, b = attrs[KEY_RED], attrs[KEY_GREEN], attrs[KEY_BLUE]
        if max(r, g, b) > 255:
            skipped += 1
            continue
        # Keyed by the record's own hash field, which is what a save stores.
        # For 451 of the 494 colour parts that field is exactly
        # bStringHash(pool name) -- which is how the field was identified in
        # the first place -- but the other 43 are the neon colours and the
        # CUSTOMPAINT_* slots, whose pool string is a display name ("RED
        # PULSE") and not the name that was hashed. Requiring the two to agree
        # threw those away; the hash field is the key either way.
        if name and bstring_hash(name) != name_hash:
            display_named += 1
        # Only the low byte survives the <<24.
        table[name_hash] = (name, r, g, b, attrs.get(KEY_GLOSS, 0) & 0xFF)
    if verbose:
        if skipped:
            print("  %d colour-ish parts skipped (incomplete or out-of-range RGB)"
                  % skipped)
        if display_named:
            print("  %d entries whose pool string is a display name, keyed by "
                  "the record hash" % display_named)
    return table


def pack_paint(table):
    """One PAINT.nfsp.

        0x00  magic 'NFSP'
        0x04  version
        0x08  entry count
        0x0c  reserved
        0x10  count * { u32 nameHash, u8 r, u8 g, u8 b, u8 gloss }

    Version 1 had a zero pad byte where version 2 has GLOSS.

    Sorted by hash so the runtime can binary-search it.
    """
    body = bytearray()
    for h in sorted(table):
        _n, r, g, b, gloss = table[h]
        body += struct.pack("<IBBBB", h, r, g, b, gloss)
    hdr = struct.pack("<4sII4x", PAINT_MAGIC, PAINT_VERSION, len(table))
    assert len(hdr) == PAINT_HDR, len(hdr)
    return bytes(hdr) + bytes(body)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the PC Files directory")
    ap.add_argument("--out", metavar="DIR", help="write %s into DIR" % OUT_NAME)
    ap.add_argument("--list", action="store_true", help="print the table")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    data = open(os.path.join(args.pc_root, GLOBAL_BUN), "rb").read()
    parts = read_parts(data, args.verbose or args.list)
    table = paint_table(parts, args.verbose or args.list)
    print("%d paint parts" % len(table))

    if args.list:
        for h in sorted(table, key=lambda k: table[k][0]):
            n, r, g, b, gloss = table[h]
            print("  %08x  %-24s  %3d %3d %3d  gloss %3d" % (h, n, r, g, b, gloss))

    if args.out:
        blob = pack_paint(table)
        os.makedirs(args.out, exist_ok=True)
        path = os.path.join(args.out, OUT_NAME)
        with open(path, "wb") as fh:
            fh.write(blob)
        print("wrote %s: %d entries, %d B" % (path, len(table), len(blob)))


if __name__ == "__main__":
    main()
