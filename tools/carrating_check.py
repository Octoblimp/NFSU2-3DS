#!/usr/bin/env python3
"""Offline check of the port's car ratings (TASKS.md #116) against shipped data.

ridephys.c ports RidePhysicsInfo from Speed.elf: BuildCarPhysicsInfo with its
package builders, MakeAccelerationTable, ComputeAccelMeasure,
GetTheoreticalTopSpeed and the three ratings the ACCELERATION / TOP SPEED /
HANDLING bars show. RebuildPhysicsInfo also integrates the same acceleration
table into the car's performance measure, and the game ships that measure per
car and upgrade level (GLOBALB.BUN's 0x34A31 tables). So this script runs the
same arithmetic in Python and compares its measure with the shipped one: if
the table, the shift points and the integral are right, the two agree.

usage:
    python tools/carrating_check.py            # every car, stock
    python tools/carrating_check.py --rows     # also dump each car's table rows
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUN = os.path.join(ROOT, "PC Files", "GLOBAL", "GLOBALB.BUN")

CAR_CHUNK = 0x00034600
PERF_CONTAINER = 0x80034A30
PERF_TABLE = 0x00034A31
PERF_EXTREMA = 0x00034A32
REC = 0x890
NREC = 0x189E0 // REC
CPI = 0x110          # CarPhysicsInfo inside CarTypeInfo
COMPLEAT = 0x610     # CompleatCarPhysicsInfo, CTI+0x110 .. CTI+0x720


def f32(b, o):
    return struct.unpack_from("<f", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def top_chunks(data):
    o = 0
    while o + 8 <= len(data):
        t, s = struct.unpack_from("<II", data, o)
        yield t, o, s
        o += 8 + s


def load():
    data = open(BUN, "rb").read()
    cars = None
    tables = []
    extrema = None
    for t, o, s in top_chunks(data):
        if t == CAR_CHUNK:
            base = (o + 0x17) & ~0xF
            cars = [data[base + i * REC: base + (i + 1) * REC] for i in range(NREC)]
        elif t == PERF_CONTAINER:
            co = o + 8
            end = o + 8 + s
            while co + 8 <= end:
                ct, cs = struct.unpack_from("<II", data, co)
                pay = co + 8
                if ct == PERF_TABLE:
                    n = u32(data, pay)
                    rows = [struct.unpack_from("<4f", data, pay + 4 + r * 16) for r in range(n)]
                    tables.append(rows)
                elif ct == PERF_EXTREMA:
                    extrema = struct.unpack_from("<30f", data, pay)
                if ct == 0 and cs == 0:
                    break
                co = pay + cs
    return cars, tables, extrema


class Table:
    """Table / TableBase (Speed.elf GetValue 0x801A4EC8, CalcIndexMultiplier
    0x80302950): n samples spread evenly over [lo, hi], linear between them,
    clamped to the end samples."""

    def __init__(self, data, lo, hi):
        self.data = data
        self.n = len(data)
        self.set_min_max(lo, hi)

    def set_min_max(self, lo, hi):
        self.lo = lo
        self.hi = hi
        self.mult = (self.n - 1) / (hi - lo) if hi != lo else 0.0

    def get(self, x):
        f = self.mult * (x - self.lo)
        i = int(f)  # fctiwz truncates toward zero
        if i < 0 or f < 0.0:
            return self.data[0]
        if i >= self.n - 1:
            return self.data[self.n - 1]
        fi = float(i)
        if fi > f:
            fi -= 1.0
        t = f - fi
        return (1.0 - t) * self.data[i] + t * self.data[i + 1]


STEP = 1.34108   # 0x80332CAC: 3 mph in m/s
RAD_TO_RPM = 9.5493  # 0x80332CB8
NACCEL = 0x5C


def shift_points(cpi):
    """CalculateShiftPoints (0x801C99C8) with GearShiftMethod == 0, which is
    what the shipped global holds: every upshift at redline - 100, the top gear
    at limiter + 500."""
    ngears = struct.unpack_from("<i", cpi, 0x1C8)[0]
    idle, redline, limiter = f32(cpi, 0x1F0), f32(cpi, 0x1F4), f32(cpi, 0x1F8)
    up = [0.0] * 10
    up[1] = f32(cpi, 0x1B0) * (redline - idle) + idle
    g = 2
    while g < ngears + 1:
        up[g] = redline - 100.0
        g += 1
    up[g] = limiter + 500.0
    return up


def accel_table(cpi):
    """MakeAccelerationTable (0x801C940C), resampled to the 64 entries
    RidePhysicsInfo keeps at +0x550."""
    split = f32(cpi, 0x1C0)
    mass = f32(cpi, 0x110)
    radius = (1.0 - split) * f32(cpi, 0x20) + split * f32(cpi, 0x80)
    fd = (1.0 - split) * f32(cpi, 0x1B8) + split * f32(cpi, 0x1BC)
    ngears = struct.unpack_from("<i", cpi, 0x1C8)[0]
    up = shift_points(cpi)
    torque = Table([f32(cpi, 0x200 + 4 * k) for k in range(9)], f32(cpi, 0x1F0), f32(cpi, 0x1F8))
    ratio = lambda g: f32(cpi, 0x1D0 + 4 * g)
    acc = [0.0] * NACCEL
    gear = 2
    v = 0.0
    i = 0
    while i <= 0x5B:
        rpm = (v / radius) * fd * ratio(gear) * RAD_TO_RPM
        if rpm >= up[gear]:
            gear += 1
            if gear >= ngears + 2:
                break
        v += STEP
        acc[i] = torque.get(rpm) * fd * ratio(gear) / radius / mass
        i += 1
    if i - 1 <= 0x5B:
        for k in range(i - 1, NACCEL):
            acc[k] = 0.0
    raw = Table(acc[:i], 0.0, v)
    out = [raw.get(0.0 + (k * (v - 0.0)) / 63.0) for k in range(64)]
    return Table(out, 0.0, v), v


def perf_measure(cpi, table):
    """RebuildPhysicsInfo's own integral (0x801C89A8): sum of (accel - drag)
    every 3 mph up to where drag wins."""
    cd = f32(cpi, 0x260)
    mass = f32(cpi, 0x110)
    m = 0.0
    v = table.lo
    while v <= table.hi:
        a = table.get(v)
        d = v * v * cd / mass
        if a < d:
            break
        v += STEP
        m += (a - d) * STEP
    return m


def build_stock(compleat):
    """BuildCarPhysicsInfo (0x801C4D44) for a car with nothing installed: every
    package builder is guarded by its upgrade level being non-zero except
    BuildTurboPackage (0x801C55CC), which always lerps the boost curve toward
    the turbo kit's and folds it into the torque -- at level 0 that is the
    stock boost, so a turbo car's torque table is base + boost."""
    cpi = bytearray(compleat[:0x2B0])
    for k in range(9):
        boost = f32(cpi, 0x224 + 4 * k)  # lerp(stock, kit, 0) == stock
        struct.pack_into("<f", cpi, 0x224 + 4 * k, boost)
        struct.pack_into("<f", cpi, 0x200 + 4 * k, f32(cpi, 0x200 + 4 * k) + boost)
    return bytes(cpi)


def main():
    cars, tables, extrema = load()
    print("extrema:", " ".join("%.4g" % x for x in extrema))
    worst = 0.0
    for t, rec in enumerate(cars):
        if t >= len(tables):
            break
        name = rec[:32].split(b"\0")[0].decode()
        kind = u32(rec, 0x844)
        rows = tables[t]
        if kind != 0 or len(rows) < 2:
            continue
        cpi = build_stock(rec[CPI:CPI + COMPLEAT])
        table, vmax = accel_table(cpi)
        m = perf_measure(cpi, table)
        ship = rows[0][0]
        err = abs(m - ship) / ship
        worst = max(worst, err)
        print("%2d %-12s measure %8.3f shipped %8.3f  err %.2e  rows %d  vmax %.1f" %
              (t, name, m, ship, err, len(rows), vmax))
        if "--rows" in sys.argv:
            for r in rows:
                print("      " + " ".join("%10.4f" % x for x in r))
    print("worst relative error %.3e" % worst)


if __name__ == "__main__":
    main()
