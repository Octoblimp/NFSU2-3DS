# Races the AI field offline over the real collision (TASKS.md #280) and
# reports what a tester would see go wrong: walls, cars off their line, stalls,
# wrong-way spells, resets, GetUnstuck, unfinished races. Needs a host gcc --
# devkitPro's MSYS2 one is used if gcc is not on PATH.
#
# Usage:
#   python tools/airace_sim.py 4001 [--reverse] [-v]          one race, verbose
#   python tools/airace_sim.py --all [--jobs N]                every track both ways
#   options: --opponents N (5) --seed S --laps N (2) --fps F (30) --car T (15)
#            --mode M (0) --max-sec S --keep DIR --world DIR
#
# The code is the shipping source compiled as it is: opponent.c, aidriver.c,
# carsim.c, race.c, trackroute.c, trackpath.c, topology.c and the loaders. Of
# world.c only the collision queries are taken, by marker, along with main.c's
# body band; model3d.h is replaced by a stub that keeps its wall record, since
# the drawn geometry is never asked once a topology is loaded. <3ds.h> is the
# integer types and bool only.
#
# The data is what the game reads: "PC Files" for GLOBALB.BUN and the
# ROUTESL4RA routes, and the WORLD.NFST copied to the Azahar SD card (read,
# never written).
import argparse
import concurrent.futures
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "3DS Files", "source")
PC = os.path.join(ROOT, "PC Files")
WORLD = os.path.join(os.environ.get("APPDATA", ""), "Azahar", "sdmc", "nfsu2data",
                     "TRACKS", "L4RA")

SOURCES = ["opponent.c", "aidriver.c", "carsim.c", "race.c", "trackroute.c",
           "trackpath.c", "topology.c", "difficulty.c", "cardb.c", "bunidx.c",
           "trackdb.c", "eventdb.c", "ridephys.c", "perfdb.c", "rideinfo.c",
           "carpartdb.c", "carhull.c", "draglane.c", "custtune.c", "presetcar.c",
           "lz4dec.c"]

STUB_3DS_H = """// Host stand-in for libctru's <3ds.h>: the integer types, and the bool and
// size_t it brings in with them, and the linear heap as the ordinary one.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef s32 Result; typedef u32 Handle;
#include <stdlib.h>
static inline void* linearAlloc(size_t n) { return malloc(n); }
static inline void linearFree(void* p) { free(p); }
"""

STUB_MODEL3D_H = """// Host stand-in for model3d.h: the wall record topology.h hands back, and
// nothing that draws.
#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#define MODEL3D_MAX_PROBES %(probes)s
%(wall)s
typedef struct Model3D Model3D;
static inline Model3D* model3d_load(const char* p) { (void)p; return NULL; }
static inline void model3d_free(Model3D* m) { (void)m; }
static inline void model3d_ground_z_multi(const Model3D* m, const float* xy, int n,
	float zMax, float* outZ, bool* outHit, float* outN)
{ (void)m; (void)xy; (void)n; (void)zMax; (void)outZ; (void)outHit; (void)outN; }
static inline int model3d_walls_near(const Model3D* m, float x, float y, float r,
	float zLo, float zHi, Model3DWall* out, int count, int max)
{ (void)m; (void)x; (void)y; (void)r; (void)zLo; (void)zHi; (void)out; (void)max; return count; }
"""


def read(path):
    return open(path, encoding="utf-8").read()


def extract(path, start, stop):
    """Lines of `path` from the one starting with `start` up to, not including,
    the one starting with `stop`."""
    lines = read(path).split("\n")
    a = next(i for i, l in enumerate(lines) if l.startswith(start))
    b = next(i for i, l in enumerate(lines) if i > a and l.startswith(stop))
    return "\n".join(lines[a:b]) + "\n"


def typedef_block(text, name):
    """The `typedef struct { ... } name;` block in `text`."""
    end = text.index("} %s;" % name)
    start = text.rindex("typedef struct {", 0, end)
    return text[start:end + len(name) + 3] + "\n"


def find_gcc():
    gcc = shutil.which("gcc")
    if gcc:
        return gcc
    for cand in (r"C:\devkitPro\msys2\usr\bin\gcc.exe", "/c/devkitPro/msys2/usr/bin/gcc"):
        if os.path.exists(cand):
            return cand
    sys.exit("no host gcc found (looked on PATH and in devkitPro's MSYS2)")


def build(out):
    src = os.path.join(out, "src")
    inc = os.path.join(out, "inc")
    os.makedirs(src, exist_ok=True)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, "3ds.h"), "w", encoding="utf-8") as f:
        f.write(STUB_3DS_H)
    for h in glob.glob(os.path.join(SRC, "*.h")):
        if os.path.basename(h) != "model3d.h":
            shutil.copy(h, src)
    for c in SOURCES:
        shutil.copy(os.path.join(SRC, c), src)
    m3d = read(os.path.join(SRC, "model3d.h"))
    probes = re.search(r"#define MODEL3D_MAX_PROBES\s+(\d+)", m3d).group(1)
    with open(os.path.join(src, "model3d.h"), "w", encoding="utf-8") as f:
        f.write(STUB_MODEL3D_H % {"probes": probes, "wall": typedef_block(m3d, "Model3DWall")})
    world_c = os.path.join(SRC, "world.c")
    with open(os.path.join(src, "world_collide_info.inc"), "w", encoding="utf-8") as f:
        f.write(typedef_block(read(os.path.join(SRC, "world.h")), "WorldCollideInfo"))
    with open(os.path.join(src, "world_ground.inc"), "w", encoding="utf-8") as f:
        f.write(extract(world_c, "int world_ground_probe(const World* w",
                        "// ------------------------------------------------------------- wall collision"))
    with open(os.path.join(src, "world_resolve.inc"), "w", encoding="utf-8") as f:
        f.write(extract(world_c, "#define COLLIDE_MAX_WALLS",
                        "// ------------------------------------------------------------ camera wall gap"))
    main_c = read(os.path.join(SRC, "main.c"))
    body = [m.group(0) for m in re.finditer(r"^#define WORLD_(FOOTPRINT|BODY_LO|BODY_HI)\b.*$",
                                            main_c, re.M)]
    if len(body) != 3:
        sys.exit("main.c's WORLD_FOOTPRINT / WORLD_BODY_LO / WORLD_BODY_HI not found")
    with open(os.path.join(src, "world_body.inc"), "w", encoding="utf-8") as f:
        f.write("\n".join(body) + "\n")
    shutil.copy(os.path.join(HERE, "airace_sim.c"), src)

    gcc = find_gcc()
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(gcc) + os.pathsep + env.get("PATH", "")
    exe = os.path.join(out, "airace_sim.exe")
    cmd = [gcc, "-O2", "-std=gnu11", "-Wall"] + [ "-Wno-unused-function", "-Wno-unused-variable",
           "-Wno-stringop-overflow", "-I", src, "-I", inc,
           os.path.join(src, "airace_sim.c")] + [os.path.join(src, c) for c in SOURCES] + \
          ["-lm", "-o", exe]
    subprocess.run(cmd, check=True, env=env)
    return exe, env


def one_way():
    """The track ids the track DB (GLOBALB.BUN chunk 0x34201, 296-byte records)
    marks not reversible at +0x86: their B routes are never raced."""
    import struct
    d = open(os.path.join(PC, "GLOBAL", "GLOBALB.BUN"), "rb").read()
    o = 0
    while o + 8 <= len(d):
        t, s = struct.unpack_from("<II", d, o)
        if t == 0x34201:
            recs = d[o + 8:o + 8 + s]
            return {struct.unpack_from("<H", recs, i + 0x8A)[0]
                    for i in range(0, len(recs) - 295, 296) if not recs[i + 0x86]}
        o += 8 + s
    return set()


def tracks():
    """Every track with a Paths file and a Routes file for the direction, B
    only where the track runs both ways."""
    out = []
    skip = one_way()
    for p in sorted(glob.glob(os.path.join(PC, "TRACKS", "ROUTESL4RA", "Routes*.bin"))):
        m = re.match(r"Routes(\d+)([FB])\.bin$", os.path.basename(p))
        if m and os.path.exists(os.path.join(PC, "TRACKS", "ROUTESL4RA",
                                             "Paths%s.bin" % m.group(1))):
            if m.group(2) == "B" and int(m.group(1)) in skip:
                continue
            out.append((int(m.group(1)), m.group(2) == "B"))
    return out


def run_one(exe, env, track, reverse, args, verbose):
    cmd = [exe, PC, args.world, str(track), "1" if reverse else "0", str(args.opponents),
           str(args.seed), str(args.laps), str(args.fps), str(args.car), str(args.mode),
           "1" if verbose else "0"]
    if args.max_sec or args.trace:
        cmd.append(str(args.max_sec))
    if args.trace:
        cmd.append(args.trace)
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("track", nargs="?", type=int)
    ap.add_argument("--reverse", action="store_true")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--opponents", type=int, default=5)
    ap.add_argument("--seed", default="0x12345678")
    ap.add_argument("--laps", type=int, default=2)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--car", type=int, default=15)
    ap.add_argument("--mode", type=int, default=0)
    ap.add_argument("--max-sec", type=float, default=0.0)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--keep", help="build here and keep it, instead of a temp dir")
    ap.add_argument("--world", default=WORLD, help="the directory holding WORLD.NFST")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--trace", help="one race: write the route, the line and every car's "
                    "path here as CSV")
    ap.add_argument("--quiet", action="store_true", help="--all: RESULT lines and the tally only")
    args = ap.parse_args()

    out = args.keep or tempfile.mkdtemp(prefix="airace_sim_")
    try:
        exe, env = build(out)
        if not args.all:
            if args.track is None:
                ap.error("a track, or --all")
            rc, text = run_one(exe, env, args.track, args.reverse, args, args.verbose)
            print(text, end="")
            return 0 if rc in (0, 3) else rc
        races = tracks()
        results = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futs = {pool.submit(run_one, exe, env, t, rv, args, False): (t, rv) for t, rv in races}
            for fut in concurrent.futures.as_completed(futs):
                results[futs[fut]] = fut.result()
        tally = {}
        clean = 0
        for key in races:
            rc, text = results[key]
            lines = text.splitlines()
            if args.quiet:
                lines = [l for l in lines if l.startswith(("RESULT", "FAIL", "RACE"))]
            print("\n".join(lines))
            if rc == 0:
                clean += 1
            for l in text.splitlines():
                if l.startswith("  INC "):
                    kind = l.split()[1]
                    tally[kind] = tally.get(kind, 0) + 1
                elif l.startswith("RESULT") and " DNF " in l:
                    tally["DNF"] = tally.get("DNF", 0) + 1
                elif l.startswith("FAIL"):
                    tally["FAIL"] = tally.get("FAIL", 0) + 1
        print("\n%d of %d races clean; incidents: %s" % (
            clean, len(races), ", ".join("%s %d" % kv for kv in sorted(tally.items())) or "none"))
    finally:
        if not args.keep:
            shutil.rmtree(out, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
