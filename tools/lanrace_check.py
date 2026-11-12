#!/usr/bin/env python3
"""Checks a LAN race's setup and states (`3DS Files/source/lanrace.c`).

TASKS #321c: after lanlobby's GO each racer sends the server its car, the
server sends every racer the table, each loads, the server starts every
racer at once, and from then on each car's state goes to the server and on
to the other racers. tools/src/lanrace_test.c runs up to six consoles of it
in one process over a fake star whose reliable links have a window and
whose unreliable channel loses; its header lists what it checks.

    python tools/lanrace_check.py [-v]
"""
import argparse
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostbuild                        # noqa: E402
import mutpar                           # noqa: E402

SOURCES = ["tools/src/lanrace_test.c", "3DS Files/source/lanrace.c"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", action="store_true", help="the races' log lines")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        exe = hostbuild.build(tmp, SOURCES, name="lanrace_test")
        out = Path(tmp) / "out.txt"
        rc = mutpar.run_exe([exe] + (["-v"] if args.v else []), out, 600)
        lines = out.read_text(encoding="latin-1").splitlines()
    for ln in lines:
        print(ln)
    sys.exit(1 if rc or "faults 0" not in lines else 0)


if __name__ == "__main__":
    main()
