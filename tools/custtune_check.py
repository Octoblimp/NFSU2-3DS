# Builds and runs tools/custtune_check.c on the PC: the port's own cardb.c,
# perfdb.c, ridephys.c and custtune.c over GLOBALB.BUN, building each career
# car of a profile save with its saved CustomPhysicsTunings the way
# RebuildPhysicsInfo (Speed.elf 0x801C89A8) does, and printing the
# CarPhysicsInfo floats each non-neutral setting changes (TASKS.md #417). The
# save is opened read-only. Needs a host gcc -- devkitPro's MSYS2 one is used
# if gcc is not on PATH. Run it from PowerShell (Git Bash env vars do not
# reach MSYS2 exes).
#
# Usage: python tools/custtune_check.py SAVE [--keep DIR]
#   SAVE  the profile save file, e.g. the Azahar SD's
#         nfsu2data/SAVE/NFS Underground 2/<name>/<name>
import argparse
import os
import shutil
import subprocess
import tempfile

from carsim_walltest import STUB_3DS_H, find_gcc

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "3DS Files", "source")
BUN = os.path.join(ROOT, "PC Files", "GLOBAL", "GLOBALB.BUN")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("save", help="the profile save file (read only)")
    ap.add_argument("--keep", help="build here and keep it, instead of a temp dir")
    args = ap.parse_args()

    out = args.keep or tempfile.mkdtemp(prefix="custtune_check_")
    os.makedirs(os.path.join(out, "inc"), exist_ok=True)
    with open(os.path.join(out, "inc", "3ds.h"), "w", encoding="utf-8") as f:
        # libctru's <3ds.h> also brings in <stdbool.h>, which perfdb.h leans on.
        f.write(STUB_3DS_H + "#include <stdbool.h>\n#include <stddef.h>\n")

    gcc = find_gcc()
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(gcc) + os.pathsep + env.get("PATH", "")
    exe = os.path.join(out, "custtune_check.exe")
    cmd = [gcc, "-O1", "-std=gnu11", "-Wall", "-Wno-unused-function",
           "-I", os.path.join(out, "inc"), "-I", SRC,
           os.path.join(HERE, "custtune_check.c")] + \
          [os.path.join(SRC, s) for s in ("cardb.c", "bunidx.c", "carhull.c", "perfdb.c",
                                          "ridephys.c", "custtune.c")] + \
          ["-lm", "-o", exe]
    subprocess.run(cmd, check=True, env=env)
    subprocess.run([exe, BUN, os.path.abspath(args.save)], check=True, env=env)
    if not args.keep:
        shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    main()
