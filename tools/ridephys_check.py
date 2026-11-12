# Builds and runs tools/ridephys_check.c on the PC: the port's own cardb.c,
# perfdb.c and ridephys.c over GLOBALB.BUN, printing each player car's
# performance measure against the one the game ships (stock and fully
# upgraded) and its ACCELERATION / TOP SPEED / HANDLING ratings stock and with
# every level-1/2/3 package fitted (TASKS.md #116). Needs a host gcc --
# devkitPro's MSYS2 one is used if gcc is not on PATH.
#
# Usage: python tools/ridephys_check.py [--keep DIR]
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
    ap.add_argument("--keep", help="build here and keep it, instead of a temp dir")
    args = ap.parse_args()

    out = args.keep or tempfile.mkdtemp(prefix="ridephys_check_")
    os.makedirs(os.path.join(out, "inc"), exist_ok=True)
    with open(os.path.join(out, "inc", "3ds.h"), "w", encoding="utf-8") as f:
        # libctru's <3ds.h> also brings in <stdbool.h>, which perfdb.h leans on.
        f.write(STUB_3DS_H + "#include <stdbool.h>\n#include <stddef.h>\n")

    # perfdb.c asks the cheat table (#446); no codes are entered here.
    with open(os.path.join(out, "cheat_stubs.c"), "w", encoding="utf-8") as f:
        f.write("#include <stdbool.h>\n"
                "bool cheats_perf_level(int level) { (void)level; return false; }\n"
                "bool cheats_contest_vinyls(void) { return false; }\n")

    gcc = find_gcc()
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(gcc) + os.pathsep + env.get("PATH", "")
    exe = os.path.join(out, "ridephys_check.exe")
    cmd = [gcc, "-O1", "-std=gnu11", "-Wall", "-Wno-unused-function",
           "-I", os.path.join(out, "inc"), "-I", SRC,
           os.path.join(HERE, "ridephys_check.c"), os.path.join(out, "cheat_stubs.c")] + \
          [os.path.join(SRC, s) for s in ("cardb.c", "bunidx.c", "carhull.c", "perfdb.c", "ridephys.c",
                                                 "custtune.c")] + \
          ["-lm", "-o", exe]
    subprocess.run(cmd, check=True, env=env)
    subprocess.run([exe, BUN], check=True, env=env)
    if not args.keep:
        shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    main()
