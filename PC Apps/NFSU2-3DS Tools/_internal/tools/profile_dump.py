#!/usr/bin/env python3
"""
profile_dump.py -- host-side reader for real NFSU2 PC save files (MC02 container).

Everything here is derived from PC Files/SPEED2.EXE, not guessed; see
docs/formats/savefile.md Part 2 for the decompiled evidence behind every offset.

Usage:
    python tools/profile_dump.py                      # dump every save under PC Files/SAVE
    python tools/profile_dump.py <path-to-save-file>  # dump one profile
    python tools/profile_dump.py --png <collectible>  # decode a magazine/DVD to PNG
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAVE_ROOT = os.path.join(ROOT, "PC Files", "SAVE", "NFS Underground 2")
HASHES = os.path.join(ROOT, "tools", "_refs", "nfsu2-re", "docs", "dumps", "hashes.txt")

MC02_MAGIC = 0x4D433032
HDR_SIZE = 0x1C
BLOCKA_SIZE = 8
PAYLOAD_OFF = HDR_SIZE + BLOCKA_SIZE          # 0x24
PROFILE_PAYLOAD_SIZE = 0xD692                 # UserProfile::GetSaveBufferSize (FUN_0052a440)
COLLECTIBLE_PAYLOAD_SIZE = 512 * 512 * 4      # raw BGRA8888 cover screenshot

# ---- block B field map (docs/formats/savefile.md Part 2) ---------------------
OFF_STOCKCARS      = 0x000000   # FEStockCar[48], stride 0x18
OFF_NUM_STOCKCARS  = 0x000480
OFF_TUNEDCARS      = 0x000484   # FECarConfig[20], stride 0x430
OFF_CAREERCARS     = 0x005844   # FECareerCar[5],  stride 0x7F2
OFF_NUM_CAREERCARS = 0x007FFE
OFF_ONLINECARS     = 0x008002   # FECarConfig[6],  stride 0x434
OFF_NUM_ONLINECARS = 0x00993A
OFF_CAREERSTATE    = 0x00993E   # PlayerCareerState
OFF_STAGEINFO      = 0x00993E   # RuntimeCareerStageInfo[8], stride 0x101
OFF_CAREERBODY     = 0x00A146
OFF_SMSQUEUE       = 0x00AE76
OFF_CAREERTAIL     = 0x00C27F
OFF_MFGCATEGORY    = 0x00C423   # 27 x {u32 index, u8 category}
OFF_AUDIOSETTINGS  = 0x00C4FB
OFF_QUICKRACE      = 0x00C5A7
OFF_CAREERSETTINGS = 0x00C723
OFF_HIGHSCORES     = 0x00C731
OFF_PROFILENAME    = 0x00D201   # char[32]
OFF_PROFILEFLAGS   = 0x00D221   # 0x33
OFF_INPUTDEVICES   = 0x00D254   # 6 x 0xB5 -- PC only, the 3DS port replaces this

# PlayerCareerState body, relative to OFF_CAREERBODY
B_CASH             = 0x000      # u32  CareerWallet balance
B_FLAG_8868        = 0x004      # u8
B_COUNT_872C       = 0x005      # u32
B_FLAG_873E        = 0x009      # u8
B_FLAG_873F        = 0x00A      # u8
B_CAREER_STAGE     = 0x00B      # u32
B_NUM_RECORDS      = 0x00F      # u32
B_STATE_RECORDS    = 0x013      # StateRecord[384], 8 bytes each
B_FLAG_873C        = 0xC13      # u8
B_FLAG_873D        = 0xC14      # u8
B_CURRENT_CAR      = 0xC15      # u32 career-car record hash
B_FLAG_8744        = 0xC19      # u8
B_RESPAWN_POS      = 0xC1A      # bVector3 + pad (0x10)
B_RESPAWN_HEADING  = 0xC2A      # u16
B_FLOAT_8764       = 0xC2C      # u32/float
B_BANK_FLAGS       = 0xC30      # u8[256]

NUM_STOCK_SLOTS   = 48
NUM_TUNED_SLOTS   = 20
NUM_CAREER_SLOTS  = 5
NUM_ONLINE_SLOTS  = 6
NUM_STATE_RECORDS = 384
NUM_BANK_FLAGS    = 256

STRIDE_STOCK  = 0x18
STRIDE_TUNED  = 0x430
STRIDE_CAREER = 0x7F2
STRIDE_ONLINE = 0x434
STRIDE_STAGE  = 0x101

# FECarRecord filter bits (MatchesFilter__11FECarRecordi)
FILTER_STOCK  = 1
FILTER_TUNED  = 2
FILTER_CAREER = 4
FILTER_ONLINE = 16


# ---------------------------------------------------------------- CRC (MC02)
def _make_table(poly=0x04C11DB7):
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ poly) & 0xFFFFFFFF if (c & 0x80000000) else (c << 1) & 0xFFFFFFFF
        t.append(c)
    return t


CRC_TABLE = _make_table()


def mc_crc(buf):
    """FUN_006fe173 @ 0x6fe173 -- CRC-32/0x04C11DB7 with the first 4 bytes preloaded."""
    if len(buf) < 4:
        return 0
    v = (~((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3])) & 0xFFFFFFFF
    for b in buf[4:]:
        v = (((v << 8) & 0xFFFFFFFF) | b) ^ CRC_TABLE[v >> 24]
    return (~v) & 0xFFFFFFFF


def mc02_unpack(data):
    """Validate an MC02 file exactly as FUN_006fd2f6 does. Returns (payload, notes)."""
    notes = []
    if len(data) < PAYLOAD_OFF:
        raise ValueError("too short for an MC02 header")
    magic, total, sizeA, sizeB, sumA, sumB, sumH = struct.unpack_from("<7I", data, 0)
    if magic != MC02_MAGIC:
        raise ValueError("bad magic 0x%08X (want 'MC02')" % magic)
    if total != len(data):
        notes.append("header totalSize %d != file length %d" % (total, len(data)))
    blockA = data[HDR_SIZE:HDR_SIZE + sizeA]
    blockB = data[HDR_SIZE + sizeA:HDR_SIZE + sizeA + sizeB]
    for label, want, got in (("hdr", sumH, mc_crc(data[:0x18])),
                             ("A", sumA, mc_crc(blockA)),
                             ("B", sumB, mc_crc(blockB))):
        notes.append("crc%s %08x/%08x %s" % (label, want, got, "OK" if want == got else "BAD"))
    return blockB, notes


def mc02_pack(payload):
    """Build an MC02 file around a payload -- the inverse of FUN_006fc226."""
    sizeB = len(payload)
    blockA = struct.pack("<2I", 0x10D, sizeB)
    total = HDR_SIZE + len(blockA) + sizeB
    hdr = struct.pack("<6I", MC02_MAGIC, total, len(blockA), sizeB,
                      mc_crc(blockA), mc_crc(payload))
    return hdr + struct.pack("<I", mc_crc(hdr)) + blockA + payload


# ------------------------------------------------------------- hash resolving
_HASHES = None

# The car list in the shipped car-data table, in carType order 0..30 as it appears
# in every retail save. Kept here only to reverse EA hashes for display -- nothing
# is decoded from it.
STOCK_CAR_NAMES = [
    "PEUGOT", "FOCUS", "COROLLA", "240SX", "MIATA", "CIVIC", "PEUGOT106", "CORSA",
    "HUMMER", "NAVIGATOR", "ESCALADE", "TIBURON", "SENTRA", "CELICA", "IS300",
    "SUPRA", "GOLF", "A3", "RSX", "ECLIPSE", "TT", "RX8", "350Z", "G35", "3000GT",
    "GTO", "MUSTANGGT", "SKYLINE", "LANCEREVO8", "RX7", "IMPREZAWRX",
]


def ea_hash(s):
    """bStringHash: h = 0xFFFFFFFF, then h = h * 33 + c for every byte.

    Verified against the shipped hash dump (TIBURON -> 0x04FD0522) and against the
    car-type hash stored in CAREER_SLOT_0 of the user's own save (CORSA -> 0x0293FAD7,
    a name the dump does not contain)."""
    v = 0xFFFFFFFF
    for c in s.encode("latin1"):
        v = (v * 33 + c) & 0xFFFFFFFF
    return v


def hashname(v):
    global _HASHES
    if _HASHES is None:
        _HASHES = {}
        try:
            with open(HASHES, encoding="latin1") as fh:
                for ln in fh:
                    p = ln.rstrip("\n").split("\t")
                    if len(p) >= 3 and p[0] == "hash" and p[1]:
                        _HASHES.setdefault(int(p[2], 16), p[1])
        except OSError:
            pass
        for n in STOCK_CAR_NAMES:
            _HASHES.setdefault(ea_hash(n), n)
            _HASHES.setdefault(ea_hash("STOCK_" + n), "STOCK_" + n)
    return _HASHES.get(v)


def hashstr(v):
    n = hashname(v)
    return n if n else ("0x%08X" % v)


# -------------------------------------------------------------------- parsing
class CarRecord(object):
    """FECarRecord -- 5 u32 on disk (the 6th word in memory is the vtable ptr)."""

    __slots__ = ("slot", "unk0", "nameHash", "perf", "inUse", "filter", "carType", "carTypeHash")

    def __init__(self, slot):
        self.slot = slot
        self.unk0 = self.nameHash = self.inUse = self.filter = 0
        self.perf = 0.0
        self.carType = None
        self.carTypeHash = None

    @property
    def used(self):
        return self.inUse != 0 or self.nameHash != 0

    def __str__(self):
        car = ""
        if self.carTypeHash is not None:
            car = " car=%s" % hashstr(self.carTypeHash)
        elif self.carType is not None:
            car = " carType=%d" % self.carType
        return "slot %2d %-22s perf=%8.2f inUse=%d filter=0x%02X%s" % (
            self.slot, hashstr(self.nameHash), self.perf, self.inUse, self.filter, car)


class Profile(object):
    pass


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _f32(b, o):
    return struct.unpack_from("<f", b, o)[0]


def _parse_records(b, base, stride, count, cartype_off=None, cartype_is_hash=False):
    out = []
    for i in range(count):
        o = base + i * stride
        r = CarRecord(i)
        r.unk0, r.nameHash, perfbits, r.inUse, r.filter = struct.unpack_from("<5I", b, o)
        r.perf = struct.unpack_from("<f", b, o + 8)[0]
        if cartype_off is not None:
            v = _u32(b, o + cartype_off)
            if cartype_is_hash:
                r.carTypeHash = v
            else:
                r.carType = v
        out.append(r)
    return out


def parse_payload(b):
    if len(b) < PROFILE_PAYLOAD_SIZE:
        raise ValueError("payload is %d bytes, expected %d" % (len(b), PROFILE_PAYLOAD_SIZE))
    p = Profile()
    p.raw = b
    p.name = b[OFF_PROFILENAME:OFF_PROFILENAME + 32].split(b"\0")[0].decode("latin1")

    p.stockCars = _parse_records(b, OFF_STOCKCARS, STRIDE_STOCK, NUM_STOCK_SLOTS,
                                 cartype_off=0x14, cartype_is_hash=False)
    p.numStockCars = _u32(b, OFF_NUM_STOCKCARS)
    p.tunedCars = _parse_records(b, OFF_TUNEDCARS, STRIDE_TUNED, NUM_TUNED_SLOTS,
                                 cartype_off=0x1C, cartype_is_hash=True)
    p.careerCars = _parse_records(b, OFF_CAREERCARS, STRIDE_CAREER, NUM_CAREER_SLOTS,
                                  cartype_off=0x1C, cartype_is_hash=True)
    p.numCareerCars = _u32(b, OFF_NUM_CAREERCARS)
    p.onlineCars = _parse_records(b, OFF_ONLINECARS, STRIDE_ONLINE, NUM_ONLINE_SLOTS,
                                  cartype_off=0x1C, cartype_is_hash=True)
    p.numOnlineCars = _u32(b, OFF_NUM_ONLINECARS)

    body = OFF_CAREERBODY
    p.cash = _u32(b, body + B_CASH)
    p.careerStage = _u32(b, body + B_CAREER_STAGE)
    p.numStateRecords = _u32(b, body + B_NUM_RECORDS)
    p.currentCarHash = _u32(b, body + B_CURRENT_CAR)
    p.respawnPos = struct.unpack_from("<3f", b, body + B_RESPAWN_POS)
    p.respawnHeading = struct.unpack_from("<H", b, body + B_RESPAWN_HEADING)[0]
    p.float8764 = _f32(b, body + B_FLOAT_8764)
    p.count872c = _u32(b, body + B_COUNT_872C)
    p.bodyFlags = {
        "+0x8868": b[body + B_FLAG_8868],
        "+0x873e": b[body + B_FLAG_873E],
        "+0x873f": b[body + B_FLAG_873F],
        "+0x873c": b[body + B_FLAG_873C],
        "+0x873d": b[body + B_FLAG_873D],
        "+0x8744": b[body + B_FLAG_8744],
    }

    rec = body + B_STATE_RECORDS
    p.stateRecords = []
    for i in range(NUM_STATE_RECORDS):
        o = rec + i * 8
        h = _u32(b, o)
        if h:
            p.stateRecords.append((i, h, b[o + 4], b[o + 5], b[o + 6], b[o + 7]))

    bank = body + B_BANK_FLAGS
    p.bankFlags = [i for i in range(NUM_BANK_FLAGS) if b[bank + i]]

    p.careerSettings = _u32(b, OFF_CAREERSETTINGS)
    p.profileFlags = b[OFF_PROFILEFLAGS:OFF_PROFILEFLAGS + 0x33]

    p.mfgCategories = []
    for i in range(27):
        o = OFF_MFGCATEGORY + i * 8
        p.mfgCategories.append((_u32(b, o), b[o + 4]))

    tail = OFF_CAREERTAIL
    p.tailFlags = list(b[tail:tail + 0x10])
    p.careerSpeech = [v for v in struct.unpack_from("<100I", b, tail + 0x10) if v]
    p.activeEvent = _u32(b, tail + 0x10 + 400)
    return p


def load(path):
    with open(path, "rb") as fh:
        data = fh.read()
    payload, notes = mc02_unpack(data)
    return parse_payload(payload), notes


# --------------------------------------------------------------------- output
def report(path, p, notes):
    print("=" * 78)
    print(os.path.relpath(path, ROOT))
    print("  container: " + "  ".join(notes))
    print("-" * 78)
    print("  profile name          : %s" % p.name)
    print("  cash                  : $%d" % p.cash)
    print("  career stage          : %d" % p.careerStage)
    print("  career event records  : %d in use / %d actually non-empty (of %d slots)"
          % (p.numStateRecords, len(p.stateRecords), NUM_STATE_RECORDS))
    print("  bank pickups claimed  : %d (of %d)" % (len(p.bankFlags), NUM_BANK_FLAGS))
    print("  CareerSettings        : %d" % p.careerSettings)
    print("  respawn position      : (%.2f, %.2f, %.2f)  heading 0x%04X (%.2f deg)"
          % (p.respawnPos[0], p.respawnPos[1], p.respawnPos[2],
             p.respawnHeading, p.respawnHeading * 360.0 / 65536.0))
    print("  active career event   : %s" % (hashstr(p.activeEvent) if p.activeEvent else "none"))
    print("  career speech heard   : %d lines" % len(p.careerSpeech))
    print("  undecoded body fields : counter=%d  float=%.2f  flags=%s"
          % (p.count872c, p.float8764,
             " ".join("%s=%d" % kv for kv in sorted(p.bodyFlags.items()))))

    used = [r for r in p.stockCars if r.used]
    print("\n  stock cars known      : %d (numStockCars=%d)" % (len(used), p.numStockCars))
    for r in used:
        print("    " + str(r))

    used = [r for r in p.tunedCars if r.used]
    print("\n  tuned garage slots    : %d used of %d" % (len(used), NUM_TUNED_SLOTS))
    for r in used:
        print("    " + str(r))

    print("\n  career cars           : %d slots unlocked" % p.numCareerCars)
    for r in p.careerCars:
        mark = " <- current" if r.nameHash == p.currentCarHash else ""
        print("    %s%s" % (r, mark) if r.used else "    slot %2d empty" % r.slot)
    print("  current career car    : %s" % hashstr(p.currentCarHash))

    print("\n  online career cars    : %d" % p.numOnlineCars)
    for r in p.onlineCars:
        print("    " + str(r))

    print("\n  completed events (first 50 of %d):" % len(p.stateRecords))
    for idx, h, s0, s1, comp, s3 in p.stateRecords[:50]:
        print("    [%3d] %-28s s0=%d s1=%d complete=%d s3=%d" % (idx, hashstr(h), s0, s1, comp, s3))
    if len(p.stateRecords) > 50:
        print("    ... %d more" % (len(p.stateRecords) - 50))


def collectibles_for(profile_path):
    """Sibling directories named '<Profile> Magazine <N>' / '<Profile> DVD <N>'."""
    d = os.path.dirname(profile_path)
    parent = os.path.dirname(d)
    name = os.path.basename(d)
    out = []
    if not os.path.isdir(parent):
        return out
    for entry in sorted(os.listdir(parent)):
        if entry != name and entry.startswith(name + " "):
            inner = os.path.join(parent, entry, entry)
            if os.path.isfile(inner):
                out.append(inner)
    return out


def dump_png(path, out_png):
    """Collectible payload is 512x512 BGRA8888 (verified: EA logo reads blue EA / green GAMES)."""
    from PIL import Image
    with open(path, "rb") as fh:
        payload, _ = mc02_unpack(fh.read())
    if len(payload) != COLLECTIBLE_PAYLOAD_SIZE:
        raise ValueError("not a 512x512 collectible (payload %d bytes)" % len(payload))
    img = Image.frombytes("RGBA", (512, 512), payload)
    b, g, r, a = img.split()
    Image.merge("RGBA", (r, g, b, a)).save(out_png)
    print("wrote %s" % out_png)


def main(argv):
    if len(argv) > 1 and argv[1] == "--png":
        for path in argv[2:]:
            dump_png(path, os.path.splitext(path)[0] + ".png")
        return 0

    targets = argv[1:]
    if not targets:
        if not os.path.isdir(SAVE_ROOT):
            print("no saves found under %s" % SAVE_ROOT)
            return 1
        for entry in sorted(os.listdir(SAVE_ROOT)):
            inner = os.path.join(SAVE_ROOT, entry, entry)
            if os.path.isfile(inner) and os.path.getsize(inner) == PAYLOAD_OFF + PROFILE_PAYLOAD_SIZE:
                targets.append(inner)

    for path in targets:
        try:
            p, notes = load(path)
        except ValueError as e:
            print("%s: %s" % (os.path.relpath(path, ROOT), e))
            continue
        report(path, p, notes)
        cols = collectibles_for(path)
        print("\n  collectibles          : %d" % len(cols))
        for c in cols:
            print("    %s (%d bytes)" % (os.path.basename(c), os.path.getsize(c)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
