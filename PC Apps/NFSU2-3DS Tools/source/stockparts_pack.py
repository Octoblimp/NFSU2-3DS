#!/usr/bin/env python3
r"""Work out every car's stock parts the way the retail car lot does, and ship
them as GLOBAL/STOCKPARTS.nfss.

Buying a car in the career car lot (UICareerCarLot::CommitBuy ->
CommitChooseCar -> CareerCarChosen -> ChooseCarForSlot -> FillFromStockCar)
fills the career record's PresetCarSlot from a fresh RideInfo:

    Init(rideInfo, carType, 1, 0, 0)       every part slot 0
    SetStockParts(rideInfo, 0)             below
    FillWithRide(presetSlot, rideInfo)     slot[0] = bStringHash(car name),
                                           slot[1 + i] = part i's name hash

SetStockParts__8RideInfoi (Speed.elf 0x80244784), for i in 0..0xa9 except the
vinyl layers 0x40..0x43 and 0x61..0x98:
    i == 0x3f   NewGetCarPart(car, 0x3f, CarTypeInfo+0x84c)  -- the stock paint
    otherwise   SetUpgradePart(i, 0) = FindPartWithLevel(car, i, 0)
and then the sixteen vinyl colour slots 0x4d..0x5c get, per layer,
VINYL_L1_COLOR09, VINYL_L1_COLOR02, VINYL_L2_COLOR11, VINYL_L1_COLOR01.
Every SetPart (0x80244e88) re-runs UpdatePartsEnabled (0x80244ee8), which
derives slot 0x19 (the hood underside, from slot 9's HOODUNDER attribute) and
slots 0x11..0x16 (the door/panel/sill pieces, from slot 6) -- and FillWithRide
copies those derived parts into the save too.

The part lookups are CarPartDatabase's (0x80243b90 GetCarPartDirect,
0x80243d60 NewGetCarPart, 0x80244694 FindPartWithLevel); the tables they read
are all GLOBALB.BUN data, located by chunk id the way LoaderCarInfo
(0x8024210c) finds them:

  0x00034600  CarTypeInfo[46], 0x890 each, 8 bytes into the chunk body
  0x00034607  u32[0xaa][2] default car-type names per slot (-1 = this car),
              then { u32 carName, u32 slot, u32 type[2] } overrides
  0x80034602  the parts database (see paint_pack.py / kit_pack.py)

CarPartSlotMap (0x803713ec, slot -> part id byte) is code, not data, so it is
copied below from Speed.elf.

A part database this tool cannot follow exactly stops it rather than letting
it guess: the output is checked against a real save with --check.

Usage:
    python tools/stockparts_pack.py "PC Files" --out "<sd>/nfsu2data/GLOBAL"
    python tools/stockparts_pack.py "PC Files" --list 350Z
    python tools/stockparts_pack.py "PC Files" --check "<save file>"
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import paint_pack as pp  # noqa: E402
from track_pack import bstring_hash  # noqa: E402

CH_CARINFO = 0x00034600
CH_SLOT_TYPES = 0x00034607
CH_CAR_TYPES = 0x0003460B
CARINFO_STRIDE = 0x890
CARINFO_COUNT = 0x2E
CARINFO_STOCK_PAINT = 0x84C

NUM_SLOTS = 0xAA
PROFILE_SLOTS = NUM_SLOTS + 1    # PresetCarSlot: [0] = the car's name hash
ANY_LEVEL = -1

SLOT_PAINT = 0x3F
SLOT_DOOR_FIRST = 0x11           # ..0x16, derived from slot 6
SLOT_BODY = 6
SLOT_HOOD = 9
SLOT_HOOD_UNDER = 0x19
SLOT_VINYL_COLOUR = 0x4D         # ..0x5c, four per layer

# CarPartSlotMap, Speed.elf 0x803713ec: CAR_SLOT_ID -> the part id a part
# record carries in its byte +4. Identity up to 27.
SLOT_MAP = list(range(28)) + [
    27, 28, 28, 29, 30, 31, 60, 61, 61, 62, 62, 63, 63, 61, 61, 61, 61, 62, 62,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    50, 50, 50, 50] + [48] * 9 + [49] * 16 + [51] * 4 + [52] * 56 + [
    53, 54, 54, 54, 54, 55, 56, 57, 58, 59, 64, 65, 65, 65, 66, 67, 68]
assert len(SLOT_MAP) == NUM_SLOTS, len(SLOT_MAP)

# IsUG1__FUi / IsUG2__FUi / IsSUV__FUi (0x80243914 / 0x80243a40 / 0x80243b44):
# the car-type names GetCarPartDirect checks a part's EXCLUDE_* attributes
# against.
UG1 = {0x3455d6b, 0x163087, 0xdc00, 0xdb7c, 0xac6d1, 0x290c04d, 0x150fb80,
       0x2ca04df, 0x72b2e513, 0x4fd0522, 0x3b8c48a, 0x545aec60, 0x79aebfec,
       0xbe48375e, 0x7502da84}
UG2 = {0x30236ee, 0xa86e, 0x453, 0x6e7, 0xdc01, 0xacc9, 0x293fad7, 0x35165819,
       0x9d2eff6, 0x2d7c9b5d, 0xcf4458f8, 0xf77c13eb, 0x4b10070a}
SUV = {0x6126dd4d, 0x219c94a, 0xcf704771}
EXCLUDE_UG1 = bstring_hash("EXCLUDE_UG1")
EXCLUDE_UG2 = bstring_hash("EXCLUDE_UG2")
EXCLUDE_SUV = bstring_hash("EXCLUDE_SUV")
HOODUNDER = bstring_hash("HOODUNDER")

VINYL_COLOURS = ("VINYL_L1_COLOR09", "VINYL_L1_COLOR02", "VINYL_L2_COLOR11",
                 "VINYL_L1_COLOR01")

MAGIC = b"NFSS"
VERSION = 1
HDR = 0x10
OUT_NAME = "STOCKPARTS.nfss"


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def chunk(data, cid):
    for c, body, size in pp.top_chunks(data):
        if c == cid:
            return body, size
    raise SystemExit("GLOBALB.BUN has no 0x%08x chunk" % cid)


class Part:
    __slots__ = ("index", "hash", "pid", "level", "type", "name", "attrs")


class Db:
    def __init__(self, data):
        # Car types: CarTypeInfo[i] starts with its name; the array sits at the
        # chunk header rounded up to 16 (LoaderCarInfo: (chunk + 0x17) & ~0xf).
        body, size = chunk(data, CH_CARINFO)
        base = ((body - 8) + 0x17) & ~0xF
        if base + CARINFO_COUNT * CARINFO_STRIDE > body + size:
            raise SystemExit("CarTypeInfo chunk is too small")
        self.cars = []
        for i in range(CARINFO_COUNT):
            o = base + i * CARINFO_STRIDE
            name = data[o:data.index(b"\0", o)].decode("latin1")
            self.cars.append((name, u32(data, o + CARINFO_STOCK_PAINT)))

        body, size = chunk(data, CH_SLOT_TYPES)
        if size < NUM_SLOTS * 8 or (size - NUM_SLOTS * 8) % 16:
            raise SystemExit("slot-type chunk is %d B" % size)
        self.slot_types = [struct.unpack_from("<2I", data, body + s * 8)
                           for s in range(NUM_SLOTS)]
        self.overrides = []
        for o in range(body + NUM_SLOTS * 8, body + size, 16):
            self.overrides.append(struct.unpack_from("<4I", data, o))

        ch = pp.find_parts_db(data)
        if not ch:
            raise SystemExit("no 0x%08x parts container" % pp.PARTS_CONTAINER)
        coff, csz = ch[CH_CAR_TYPES]
        self.car_types = list(struct.unpack_from("<%dI" % (csz // 4), data, coff))

        # Attributes in list order: GetFirstAppliedAttribute takes the first
        # match, which paint_pack.read_parts' dict does not keep.
        hoff, _ = ch[pp.CH_HEADER]
        n_attrs = u32(data, hoff + 8 * 4)
        n_parts = u32(data, hoff + 14 * 4)
        poff, psz = ch[pp.CH_STRINGS]
        pool = data[poff:poff + psz]
        loff, lsz = ch[pp.CH_ATTRLISTS]
        aoff, _asz = ch[pp.CH_ATTRS]
        roff, rsz = ch[pp.CH_PARTS]
        if rsz // pp.PART_RECORD_SIZE != n_parts:
            raise SystemExit("part chunk disagrees with the header")
        self.parts = []
        for i in range(n_parts):
            o = roff + i * pp.PART_RECORD_SIZE
            p = Part()
            p.index = i
            p.hash = u32(data, o)
            p.pid = data[o + 4]
            p.level = data[o + 5] >> 5
            p.type = data[o + 7]
            noff = struct.unpack_from("<H", data, o + 8)[0] * 4
            p.name = pool[noff:pool.index(b"\0", noff)].decode("latin1")
            li = struct.unpack_from("<H", data, o + 10)[0]
            p.attrs = []
            if (li + 1) * 2 <= lsz:
                n = struct.unpack_from("<H", data, loff + li * 2)[0]
                for k in range(n):
                    ai = struct.unpack_from("<H", data, loff + (li + 1 + k) * 2)[0]
                    if ai < n_attrs:
                        p.attrs.append(struct.unpack_from("<II", data, aoff + ai * 8))
            self.parts.append(p)
        # ScanHashTableKey8 binary-searches byte +7, so the table must be
        # sorted on it; if it is not, the retail lookup is not what this does.
        keys = [p.type for p in self.parts]
        if keys != sorted(keys):
            raise SystemExit("part records are not sorted by car type")

    # -------------------------------------------------- CarPartDatabase

    def attr(self, part, key, default=0):
        for k, v in part.attrs:
            if k == key:
                return v
        return default

    def type_hash(self, part):
        return self.car_types[part.type]

    def types_from_slot(self, slot, car_hash):
        for c, s, t0, t1 in self.overrides:
            if c == car_hash and s == slot:
                return (t0, t1)
        return tuple(car_hash if t == 0xFFFFFFFF else t for t in self.slot_types[slot])

    def first_of_type(self, type_hash):
        try:
            idx = self.car_types.index(type_hash)
        except ValueError:
            return None
        if idx > 0xFE:
            return None
        for p in self.parts:       # the lowest record with byte +7 == idx
            if p.type == idx:
                return p.index
        return None

    def get_direct(self, type_hash, pid, part_hash, prev, level):
        if prev is None:
            start = self.first_of_type(type_hash)
            if start is None:
                return None
        else:
            start = prev.index + 1
        ug1, ug2, suv = type_hash in UG1, type_hash in UG2, type_hash in SUV
        for p in self.parts[start:]:
            if self.type_hash(p) != type_hash or p.pid != pid:
                continue
            if part_hash and p.hash != part_hash:
                continue
            if ug1 and self.attr(p, EXCLUDE_UG1):
                continue
            if ug2 and self.attr(p, EXCLUDE_UG2):
                continue
            if suv and self.attr(p, EXCLUDE_SUV):
                continue
            if level == ANY_LEVEL or p.level == level:
                return p
        return None

    def get_part(self, car_hash, slot, part_hash=0, prev=None, level=ANY_LEVEL):
        types = self.types_from_slot(slot, car_hash)
        pid = SLOT_MAP[slot]
        first = 0
        if prev is not None:
            while first < 2 and self.type_hash(prev) != types[first]:
                first += 1
            if first == 2:
                return None
        for i in range(first, 2):
            if not types[i]:
                continue
            r = self.get_direct(types[i], pid, part_hash,
                                prev if i == first else None, level)
            if r:
                return r
        return None

    def find_with_level(self, car_hash, slot, level):
        p = self.get_part(car_hash, slot)
        while p is not None and p.level != level:
            p = self.get_part(car_hash, slot, prev=p)
        return p

    # ----------------------------------------------------------- RideInfo

    def stock_parts(self, car):
        name, stock_paint = self.cars[car]
        ch = bstring_hash(name)
        ride = [None] * NUM_SLOTS

        def update_enabled():
            for i in range(NUM_SLOTS):
                v = ride[i]
                if i == SLOT_HOOD and v is not None:
                    hu = self.attr(v, HOODUNDER, 0)
                    ride[SLOT_HOOD_UNDER] = self.get_part(ch, SLOT_HOOD_UNDER, hu)
                if i == SLOT_BODY:
                    if v is None or v.level == 0:
                        for k in range(6):
                            ride[SLOT_DOOR_FIRST + k] = self.get_part(ch, SLOT_DOOR_FIRST + k)
                    else:
                        raise SystemExit("%s: stock body part %s is not level 0"
                                         % (name, v.name))

        def set_part(slot, p):
            if 0x61 <= slot < 0x69 or slot == SLOT_HOOD_UNDER:
                return
            ride[slot] = p
            update_enabled()

        for i in range(NUM_SLOTS):
            if 0x40 <= i <= 0x43 or 0x61 <= i <= 0x98:
                continue
            if i == SLOT_PAINT:
                p = self.get_part(ch, SLOT_PAINT, stock_paint)
            else:
                p = self.find_with_level(ch, i, 0)
            if p is not None:
                set_part(i, p)
        hashes = [bstring_hash(n) for n in VINYL_COLOURS]
        for layer in range(4):
            for j in range(4):
                s = SLOT_VINYL_COLOUR + layer * 4 + j
                set_part(s, self.get_part(ch, s, hashes[j]))
        return [ch] + [p.hash if p else 0 for p in ride], ride


def pack(entries):
    """STOCKPARTS.nfss:

        0x00  magic 'NFSS'
        0x04  version
        0x08  car count
        0x0c  u32 per car (0xab)
        0x10  count * u32[0xab]: a PresetCarSlot, [0] the car's name hash

    In CarTypeInfo order; the runtime finds a car by [0].
    """
    body = b"".join(struct.pack("<%dI" % PROFILE_SLOTS, *e) for e in entries)
    hdr = struct.pack("<4sIII", MAGIC, VERSION, len(entries), PROFILE_SLOTS)
    assert len(hdr) == HDR
    return hdr + body


def check(db, table, save_path):
    import profile_dump as pd
    raw = open(save_path, "rb").read()
    b = raw[pd.PAYLOAD_OFF:pd.PAYLOAD_OFF + pd.PROFILE_PAYLOAD_SIZE]
    names = {p.hash: p.name for p in db.parts}
    for slot in range(5):
        rec = pd.OFF_CAREERCARS + slot * pd.STRIDE_CAREER
        arr = struct.unpack_from("<%dI" % PROFILE_SLOTS, b, rec + 0x1C)
        if not arr[0]:
            continue
        stock = table.get(arr[0])
        if stock is None:
            print("career slot %d: car 0x%08x not in the table" % (slot, arr[0]))
            continue
        same = sum(1 for i in range(1, PROFILE_SLOTS) if arr[i] == stock[i])
        print("career slot %d (%08x): %d of %d slots equal the stock fill"
              % (slot, arr[0], same, PROFILE_SLOTS - 1))
        for i in range(1, PROFILE_SLOTS):
            if arr[i] != stock[i]:
                print("   %3d  save %-34s stock %s"
                      % (i, names.get(arr[i], "%08x" % arr[i]),
                         names.get(stock[i], "%08x" % stock[i])))


def write_atomic(path, blob):
    tmp = path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(blob)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_root", help="the PC Files directory")
    ap.add_argument("--out", metavar="DIR", help="write %s into DIR" % OUT_NAME)
    ap.add_argument("--list", metavar="CAR", help="print one car's stock fill")
    ap.add_argument("--check", metavar="SAVE", help="compare with a save's career cars")
    args = ap.parse_args()

    data = open(os.path.join(args.pc_root, pp.GLOBAL_BUN), "rb").read()
    db = Db(data)
    entries, table = [], {}
    for i, (name, _paint) in enumerate(db.cars):
        if bstring_hash(name) not in db.car_types:
            continue
        slots, ride = db.stock_parts(i)
        entries.append(slots)
        table[slots[0]] = slots
        if args.list and args.list.upper() == name.upper():
            for s, p in enumerate(ride):
                if p:
                    print("  %3d  %-6s %s" % (s + 1, "L%d" % p.level, p.name))
    print("%d cars, %d parts in the database" % (len(entries), len(db.parts)))

    if args.check:
        check(db, table, args.check)
    if args.out:
        blob = pack(entries)
        os.makedirs(args.out, exist_ok=True)
        path = os.path.join(args.out, OUT_NAME)
        write_atomic(path, blob)
        print("wrote %s: %d cars, %d B" % (path, len(entries), len(blob)))


if __name__ == "__main__":
    main()
