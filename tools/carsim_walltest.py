# Builds and runs tools/carsim_walltest.c on the PC: the port's car sim on a
# flat plane and against one straight wall, for checking the handling and the
# wall response (TASKS.md #150, #222) without an emulator run. Needs a host gcc -- devkitPro's MSYS2 one is used if gcc is not
# on PATH.
#
# Usage: python tools/carsim_walltest.py [--keep DIR] [case]
#
# world.c's resolver and cardb.c's carperf functions are extracted by marker
# rather than copied, so the test always exercises the code that ships. carsim.c
# compiles as it is; <3ds.h> is replaced by a stub that only declares the
# integer types cardb.h uses.
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), "3DS Files", "source")

STUB_3DS_H = """// Host stand-in for libctru's <3ds.h>: the integer types only.
#pragma once
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef s32 Result; typedef u32 Handle;
"""


def extract(path, start, stop):
    """Lines of `path` from the one starting with `start` up to, not including,
    the one starting with `stop`."""
    lines = open(path, encoding="utf-8").read().split("\n")
    a = next(i for i, l in enumerate(lines) if l.startswith(start))
    b = next(i for i, l in enumerate(lines) if i > a and l.startswith(stop))
    return "\n".join(lines[a:b]) + "\n"


def find_gcc():
    gcc = shutil.which("gcc")
    if gcc:
        return gcc
    for cand in (r"C:\devkitPro\msys2\usr\bin\gcc.exe", "/c/devkitPro/msys2/usr/bin/gcc"):
        if os.path.exists(cand):
            return cand
    sys.exit("no host gcc found (looked on PATH and in devkitPro's MSYS2)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", help="build here and keep it, instead of a temp dir")
    ap.add_argument("case", nargs="*", help="run only this case (rest, launch, brake, corner, ...)")
    args = ap.parse_args()

    out = args.keep or tempfile.mkdtemp(prefix="carsim_walltest_")
    os.makedirs(os.path.join(out, "inc"), exist_ok=True)
    with open(os.path.join(out, "inc", "3ds.h"), "w", encoding="utf-8") as f:
        f.write(STUB_3DS_H)
    with open(os.path.join(out, "world_resolve.inc"), "w", encoding="utf-8") as f:
        f.write(extract(os.path.join(SRC, "world.c"),
                        "#define COLLIDE_MAX_WALLS", "bool world_collide("))
    with open(os.path.join(out, "carperf.inc"), "w", encoding="utf-8") as f:
        f.write(extract(os.path.join(SRC, "cardb.c"),
                        "float carperf_torque_at(", "// SPEED2.EXE:0x505450"))

    # The cars' collision hulls, packed as the 3DS gets them, for a CARSIM_CAR
    # case. carhull_load() takes the GLOBALB.BUN path and looks beside it.
    hulls = os.path.join(out, "CARHULLS.nfsg")
    subprocess.run([sys.executable, os.path.join(HERE, "hull_pack.py"),
                    os.path.join(os.path.dirname(HERE), "PC Files", "CARS"), hulls],
                   check=True, stdout=subprocess.DEVNULL)

    gcc = find_gcc()
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(gcc) + os.pathsep + env.get("PATH", "")
    env.setdefault("CARSIM_HULLS", os.path.join(out, "GLOBALB.BUN").replace("\\", "/"))
    exe = os.path.join(out, "carsim_walltest.exe")
    # world.c's s_walls comes along with the extract and is unused here.
    cmd = [gcc, "-O1", "-std=gnu11", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
           "-I", os.path.join(out, "inc"), "-I", out, "-I", SRC,
           os.path.join(HERE, "carsim_walltest.c"), os.path.join(SRC, "carsim.c"),
           os.path.join(SRC, "carhull.c"),
           os.path.join(SRC, "aidriver.c"), os.path.join(SRC, "trackroute.c"),
           "-lm", "-o", exe]
    subprocess.run(cmd, check=True, env=env)
    subprocess.run([exe] + args.case, check=True, env=env)
    if not args.keep:
        shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    main()
