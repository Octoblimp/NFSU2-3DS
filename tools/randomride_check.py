#!/usr/bin/env python3
r"""The AI car's visual parts and skin, the way retail dresses them -- an
offline twin of 3DS Files/source/rideinfo.c, for checking it.

RaceStarter::AddCareerEventAIOpponents (Speed.elf 0x8011A22C) gives a RANDOM
opponent its parts in three steps:

  PickRandomCarsOfSomeSort (0x80119F24)  Init(car) + SetStockParts(0)
  SyncVisualPartsWithPhysics (0x80244A38, ride, true, random)
      MatchUpgradeLevel the aero path (9) to the mean of the tyre, weight
      and suspension levels, then BuildRandomRide (0x802470A8), then the
      ENGINE / FRONT_BRAKE parts by the engine and brake packages fitted
  BuildRandomRide ends in BuildRandomSkin (0x8024748C)

This file is the part of that which needs no physics: BuildRandomRide and
BuildRandomSkin given the ride's ten upgrade levels and the bRandom state.
The C logs both before it starts ("rideinfo: ... rng 0x..., levels ..."), so

    python tools/randomride_check.py "PC Files" IMPREZAWRX --rng 0x... \
        --levels 0,1,0,2,0,0,1,0,0,0

prints the parts the C should have chosen, slot by slot.

The tables are Speed.elf's, and SPEED2.EXE's RandomPartUpgrades (file offset
0x402C50) is byte-identical. The paint and vinyl colour jobs are built by a
static initializer (0x80254C6C) out of name strings that SPEED2.EXE carries
too; their hashes are checked against the PC parts database below.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import stockparts_pack as sp  # noqa: E402
from track_pack import bstring_hash  # noqa: E402

# GC CAR_SLOT_ID (CarSlotIDNames, Speed.elf 0x8037181C). A save's PresetCarSlot
# index is one more.
FRONT_BUMPER, REAR_BUMPER, ROOF, HOOD, SKIRT, SPOILER = 1, 2, 7, 9, 0xB, 0xC
ENGINE, HEADLIGHT, BRAKELIGHT, EXHAUST = 0xD, 0xE, 0xF, 0x10
FRONT_BRAKE, FRONT_WHEEL, REAR_WHEEL = 0x1B, 0x1D, 0x1E
BASE_PAINT, VINYL_LAYER0 = 0x3F, 0x40
PAINT_SPOILER, PAINT_EXHAUST = 0x45, 0x47
VINYL_COLOUR0_0 = 0x4D

# RandomPartUpgrades (0x80371694, NumRandomPartUpgrades 14): slot, "stock
# when the draw says 4", then per upgrade level 0..3 the four chances, in
# percent, of a level-0..3 part.
_STD = ((60, 30, 10, 0), (20, 60, 20, 0), (0, 20, 60, 20), (0, 10, 20, 70))
_PAINT = ((0, 70, 30, 0), (0, 70, 20, 10), (0, 20, 60, 20), (0, 10, 20, 70))
RANDOM_PART_UPGRADES = (
    (HOOD, 1, _STD), (ROOF, 1, _STD), (SPOILER, 1, _STD), (EXHAUST, 1, _STD),
    (0x20, 1, _STD), (BRAKELIGHT, 1, _STD), (HEADLIGHT, 1, _STD),
    (0x9F, 1, _STD), (0x99, 0, _STD), (BASE_PAINT, 1, _PAINT),
    (0x49, 0, ((70, 20, 10, 0), (50, 30, 20, 0), (20, 10, 60, 10), (0, 10, 20, 70))),
    (0x4A, 0, _PAINT),
    (0x9A, 0, ((90, 10, 0, 0), (40, 50, 10, 0), (30, 20, 50, 0), (30, 10, 30, 30))),
    (FRONT_BUMPER, 0, _STD),
)
# BuildRandomRide's wheel chances (0x8033CBB9), by the tyre level.
WHEEL_LEVELS = ((70, 20, 10, 0), (50, 40, 10, 0), (10, 20, 50, 20), (0, 10, 20, 70))

# BuildRandomSkin's tables (0x8033CBDC..0x8033CC64), by the mean level.
FULL_VINYL_CHANCE = (0, 0, 5, 10)
VINYL_L0_LEVELS = ((95, 0, 5, 0), (90, 0, 10, 0), (80, 0, 20, 0), (65, 0, 35, 0))
VINYL_L1_LEVELS = ((80, 10, 10, 0), (70, 15, 10, 5), (55, 25, 15, 5), (40, 20, 20, 20))
VINYL_L2_LEVELS = ((95, 0, 0, 5), (90, 0, 0, 10), (80, 0, 0, 20), (65, 0, 0, 35))
FULL_VINYL_GROUPS = (0x16,)
VINYL_L0_GROUPS = (0x17,)
VINYL_L1_GROUPS = tuple(range(0, 14)) + (0x10, 0x11, 0x12, 0x13, 0x14)
VINYL_L2_GROUPS = (0x19,)

# RandomCarPaintJobs (0x8040B154, 5 of 0x28 bytes) and RandomVinylPaintJobs
# (0x8040B21C, 5 of 4), from the static initializer at 0x80254C6C.
RANDOM_CAR_PAINT_JOBS = (
    ("GLOSS_L1_COLOR02", ("GLOSS_L1_COLOR02", "GLOSS_L1_COLOR07", "GLOSS_L2_COLOR02",
                          "GLOSS_L2_COLOR07", "GLOSS_L2_COLOR13")),
    ("GLOSS_L1_COLOR03", ("GLOSS_L1_COLOR03", "GLOSS_L1_COLOR02", "GLOSS_L2_COLOR01",
                          "GLOSS_L2_COLOR06", "GLOSS_L3_TEST07")),
    ("GLOSS_L2_COLOR01", ("GLOSS_L2_COLOR01", "GLOSS_L2_COLOR02", "GLOSS_L2_COLOR07",
                          "GLOSS_L2_COLOR11", "GLOSS_L2_COLOR03")),
    ("GLOSS_L2_COLOR17", ("GLOSS_L2_COLOR17", "GLOSS_L1_COLOR02", "GLOSS_L1_COLOR04",
                          "GLOSS_L2_COLOR01", "GLOSS_L2_COLOR07")),
    ("METAL_L2_COLOR01", ("METAL_L2_COLOR01", "GLOSS_L1_COLOR02", "GLOSS_L1_COLOR07",
                          "METAL_L2_COLOR05", "METAL_L3_TEST04")),
)
RANDOM_VINYL_PAINT_JOBS = (
    ("VINYL_L1_COLOR01", "VINYL_L1_COLOR02", "VINYL_L2_COLOR11", "VINYL_L1_COLOR09"),
    ("VINYL_L1_COLOR01", "VINYL_L1_COLOR02", "VINYL_L1_COLOR03", "VINYL_L1_COLOR04"),
    ("VINYL_L1_COLOR02", "VINYL_L1_COLOR03", "VINYL_L1_COLOR04", "VINYL_L1_COLOR05"),
    ("VINYL_L1_COLOR02", "VINYL_L3_COLOR12", "VINYL_L3_COLOR13", "VINYL_L3_COLOR14"),
    ("VINYL_L1_COLOR02", "VINYL_L3_COLOR18", "VINYL_L3_COLOR19", "VINYL_L3_COLOR20"),
)

PATHS = 10
OUTER_RADIUS = bstring_hash("OUTER_RADIUS")   # 0xCE7D8DB5
INNER_RADIUS = bstring_hash("INNER_RADIUS")   # 0xEB0101E2
CARINFO_OUTER = 0xDC
CARINFO_INNER_MIN = 0xDD
CARINFO_INNER_MAX = 0xDE


class Rng:
    """bRandom(int) on the shared state -- opponent.c's bRandomNext."""

    def __init__(self, state):
        self.state = state & 0xFFFFFFFF

    def __call__(self, n):
        s = self.state
        x = s ^ 0x1D872B41
        y = x ^ (x >> 5)
        x = (x ^ (y << 27)) & 0xFFFFFFFF
        y ^= x
        self.state = y & 0xFFFFFFFF
        return s % n if n else 0


def random_level(rng, chances):
    """GetRandomLevel (0x80247040): 0..3, or 4 when the chances run out."""
    r = rng(100)
    i, acc = 0, chances[0]
    while acc <= r:
        i += 1
        if i >= 4:
            break
        acc += chances[i]
    return i


class Ride:
    def __init__(self, db, data, car):
        self.db, self.car = db, car
        self.name, self.stock_paint = db.cars[car]
        self.hash = bstring_hash(self.name)
        body, _size = sp.chunk(data, sp.CH_CARINFO)
        o = (((body - 8) + 0x17) & ~0xF) + car * sp.CARINFO_STRIDE
        self.outer = data[o + CARINFO_OUTER]
        self.inner = (data[o + CARINFO_INNER_MIN], data[o + CARINFO_INNER_MAX])
        self.part = [None] * sp.NUM_SLOTS

    # CarPartDatabase::NewGetCarPart with the level filter (-1 = any).
    def get(self, slot, part_hash=0, prev=None, level=sp.ANY_LEVEL):
        return self.db.get_part(self.hash, slot, part_hash, prev, level)

    def count(self, slot, level):
        n, p = 0, self.get(slot, level=level)
        while p is not None:
            n += 1
            p = self.get(slot, prev=p, level=level)
        return n

    def nth(self, slot, level, k):
        p = self.get(slot, level=level)
        for _ in range(k):
            p = self.get(slot, prev=p, level=level)
        return p

    # SetPart (0x80244E88) and UpdatePartsEnabled (0x80244EE8), the derived
    # slots only: the hood's underside and the stock body's door pieces.
    def set(self, slot, p):
        self.part[slot] = p
        hood = self.part[HOOD]
        if hood is not None:
            under = self.db.attr(hood, sp.HOODUNDER, 0)
            self.part[sp.SLOT_HOOD_UNDER] = self.get(sp.SLOT_HOOD_UNDER, under)
        body = self.part[sp.SLOT_BODY]
        if body is None or body.level == 0:
            for k in range(6):
                self.part[sp.SLOT_DOOR_FIRST + k] = self.get(sp.SLOT_DOOR_FIRST + k)
        else:
            raise SystemExit("a widebody is not something BuildRandomRide fits")

    def stock_parts(self):
        """SetStockParts(0) (0x80244784)."""
        for i in range(sp.NUM_SLOTS):
            if 0x40 <= i <= 0x43 or 0x61 <= i <= 0x98:
                continue
            p = (self.get(sp.SLOT_PAINT, self.stock_paint) if i == sp.SLOT_PAINT
                 else self.db.find_with_level(self.hash, i, 0))
            if p is not None:
                self.set(i, p)
        hashes = [bstring_hash(n) for n in sp.VINYL_COLOURS]
        for layer in range(4):
            for j in range(4):
                s = VINYL_COLOUR0_0 + layer * 4 + j
                self.set(s, self.get(s, hashes[j]))

    def random_part(self, slot, level, rng):
        """GetRandomPart (0x80244994)."""
        n = self.count(slot, level)
        return self.nth(slot, level, rng(n)) if n else None

    def random_wheel(self, level, rng):
        """GetRandomWheel (0x80246D94)."""
        fits = []
        p = self.get(FRONT_WHEEL, level=level)
        while p is not None and len(fits) < 0x80:
            outer = self.db.attr(p, OUTER_RADIUS, 0) & 0xFF
            inner = self.db.attr(p, INNER_RADIUS, 0) & 0xFF
            if outer == self.outer and self.inner[0] <= inner <= self.inner[1]:
                fits.append(p)
            p = self.get(FRONT_WHEEL, prev=p, level=level)
        return fits[rng(len(fits))] if fits else None

    def random_vinyl(self, level, groups, rng):
        """GetRandomVinyl (0x80246ED4): the group is the kit field."""
        pool = []
        p = self.get(VINYL_LAYER0, level=level)
        while p is not None and len(pool) < 0x200:
            for g in groups:
                if self.db.kit[p.index] == g:
                    pool.append(p)
            p = self.get(VINYL_LAYER0, prev=p, level=level)
        return pool[rng(len(pool))] if pool else None

    def build_random_ride(self, levels, rng):
        """BuildRandomRide (0x802470A8)."""
        mean = sum(levels) / 10.0
        for slot, stock, chances in RANDOM_PART_UPGRADES:
            if slot in (SPOILER, FRONT_BUMPER, ROOF):
                lv = levels[9]
            elif slot == EXHAUST:
                lv = levels[3]
            elif slot == FRONT_BRAKE:
                lv = levels[6]
            else:
                lv = int(mean)
            r = random_level(rng, chances[lv])
            if r == 4:
                self.set(slot, self.get(slot, level=0) if stock else None)
            else:
                self.set(slot, self.random_part(slot, r, rng))
        front = self.part[FRONT_BUMPER]
        if front is None:
            self.set(FRONT_BUMPER, self.get(FRONT_BUMPER, level=0))
            self.set(REAR_BUMPER, self.get(REAR_BUMPER, level=0))
            self.set(SKIRT, self.get(SKIRT, level=0))
        else:
            rear = self.get(REAR_BUMPER, bstring_hash("%s_KIT%.2d_REAR_BUMPER" % (self.name, self.db.kit[front.index])))
            skirt = self.get(SKIRT, bstring_hash("%s_KIT%.2d_SKIRT" % (self.name, self.db.kit[front.index])))
            if rear is not None and skirt is not None:
                self.set(FRONT_BUMPER, front)
                self.set(REAR_BUMPER, rear)
                self.set(SKIRT, skirt)
        w = self.random_wheel(random_level(rng, WHEEL_LEVELS[levels[8]]), rng)
        if w is not None:
            self.set(FRONT_WHEEL, w)
            self.set(REAR_WHEEL, w)
        self.build_random_skin(levels, rng)

    def build_random_skin(self, levels, rng):
        """BuildRandomSkin (0x8024748C)."""
        mean = int(sum(levels) / 10.0)
        if rng(100) < FULL_VINYL_CHANCE[mean]:
            v = self.random_vinyl(sp.ANY_LEVEL, FULL_VINYL_GROUPS, rng)
            if v is not None:
                for k in (1, 2, 3):
                    self.set(VINYL_LAYER0 + k, None)
                self.set(VINYL_LAYER0, v)
        else:
            v0 = self.random_vinyl(random_level(rng, VINYL_L0_LEVELS[mean]), VINYL_L0_GROUPS, rng)
            v1 = self.random_vinyl(random_level(rng, VINYL_L1_LEVELS[mean]), VINYL_L1_GROUPS, rng)
            v2 = self.random_vinyl(random_level(rng, VINYL_L2_LEVELS[mean]), VINYL_L2_GROUPS, rng)
            self.set(VINYL_LAYER0, v0)
            self.set(VINYL_LAYER0 + 1, v1)
            self.set(VINYL_LAYER0 + 2, v2)
            for layer in range(3):
                perm = [0, 1, 2, 3]
                for i in range(4):            # GetRandomPermutation (0x80246FC8)
                    j = rng(4)
                    perm[i], perm[j] = perm[j], perm[i]
                job = RANDOM_VINYL_PAINT_JOBS[rng(len(RANDOM_VINYL_PAINT_JOBS))]
                for k in range(4):
                    s = VINYL_COLOUR0_0 + layer * 4 + k
                    self.set(s, self.get(s, bstring_hash(job[perm[k]])))
        _slot, _stock, chances = RANDOM_PART_UPGRADES[9]
        r = random_level(rng, chances[mean])
        if r == 4:
            self.set(BASE_PAINT, self.get(BASE_PAINT, level=0))
        else:
            self.set(BASE_PAINT, self.random_part(BASE_PAINT, r, rng))
        base = self.part[BASE_PAINT]
        if base is not None:
            for paint, accents in RANDOM_CAR_PAINT_JOBS:
                if bstring_hash(paint) == base.hash:
                    a, b = rng(len(accents)), rng(len(accents))
                    self.set(PAINT_SPOILER, self.get(PAINT_SPOILER, bstring_hash(accents[a])))
                    self.set(PAINT_EXHAUST, self.get(PAINT_EXHAUST, bstring_hash(accents[b])))


def check_names(db):
    """Every name the tables hash has to be a part in the PC database."""
    have = {p.hash for p in db.parts}
    names = [p for p, acc in RANDOM_CAR_PAINT_JOBS] + [a for _p, acc in RANDOM_CAR_PAINT_JOBS for a in acc]
    names += [n for job in RANDOM_VINYL_PAINT_JOBS for n in job]
    missing = sorted({n for n in names if bstring_hash(n) not in have})
    print("table names: %d, missing from the PC parts database: %s"
          % (len(set(names)), ", ".join(missing) or "none"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pc_root")
    ap.add_argument("car", help="CarTypeInfo name, e.g. IMPREZAWRX")
    ap.add_argument("--rng", default="0", help="bRandom state before BuildRandomRide")
    ap.add_argument("--levels", default="0,0,0,0,0,0,0,0,0,0",
                    help="GetUpgradeLevel of paths 0..9")
    ap.add_argument("--skin-only", action="store_true",
                    help="BuildRandomSkin alone (the field's reused ride)")
    a = ap.parse_args()
    data = open(os.path.join(a.pc_root, "GLOBAL", "GLOBALB.BUN"), "rb").read()
    db = sp.Db(data)
    # The kit/group field is byte +5's low five bits; Db keeps only the level.
    roff, _ = sp.pp.find_parts_db(data)[sp.pp.CH_PARTS]
    db.kit = [data[roff + p.index * sp.pp.PART_RECORD_SIZE + 5] & 0x1F for p in db.parts]
    check_names(db)
    cars = [n for n, _ in db.cars]
    if a.car not in cars:
        raise SystemExit("no car %s" % a.car)
    levels = [int(x) for x in a.levels.split(",")]
    if len(levels) != PATHS:
        raise SystemExit("--levels takes ten")
    rng = Rng(int(a.rng, 0))
    ride = Ride(db, data, cars.index(a.car))
    ride.stock_parts()
    if a.skin_only:
        ride.build_random_skin(levels, rng)
    else:
        ride.build_random_ride(levels, rng)
    print("%s rng after 0x%08x" % (a.car, rng.state))
    stock = Ride(db, data, cars.index(a.car))
    stock.stock_parts()
    for s in range(sp.NUM_SLOTS):
        p, q = ride.part[s], stock.part[s]
        if p is not q:
            print("  slot %02x (CarSlotId %02x): %-34s was %s" % (
                s, s + 1, p.name if p else "-", q.name if q else "-"))


if __name__ == "__main__":
    main()
