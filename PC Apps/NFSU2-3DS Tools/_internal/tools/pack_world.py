#!/usr/bin/env python3
"""Convert a retail track to the port's world directory, with the settings the
shipped build actually uses.

Why this exists: the pack settings used to live only in TASKS.md prose, and
track_pack.py's own defaults are not them. `--maxdim` defaults to 128 while the
shipped world is packed at 256 (TASKS.md #192/#193 raised it, and the bank went
30.1 -> 109.2 MB), so running track_pack.py with its defaults quietly halves
every texture in the city -- which is exactly what happened once, and the only
symptom is that the world looks soft. Keeping the numbers in a script instead
of a sentence is the fix.

It also runs sky_pack.py, shadow_pack.py and afx_pack.py, which track_pack.py
does not emit: SKYDOME.nfsc, SHADOW.nfsc and ACIDFX.nfsa (the event, shop and
smoke-stack effects, TASKS.md #174) live in the same directory, are not in
WORLD.NFSX's index, and are therefore destroyed by anything that clears the
output directory before a repack. Running them all together means a full
rebuild is one command and cannot leave the sky behind.

Note that this writes over the output directory in place rather than clearing
it, which is what keeps an interrupted run from leaving a half-world behind.

    python tools/pack_world.py "PC Files" --out <sdmc>/nfsu2data/TRACKS/L4RA

Add --topology to rebuild WORLD.NFST (the retail collision set) as well; it is
left alone by default because it is independent of the art and takes a while.

Last, it rewrites WORLD.NFSZ, the compressed archive of the sections and the
bank that the runtime reads in their place (world_archive.py, TASKS.md #270).
That step is not optional: the runtime can only tell an archive is stale when
WORLD.NFSX changed, so an archive left over from before a repack that kept the
index the same -- a bank-only change, say -- would go on serving the old
textures. The runtime reads nothing else, so once the archive is verified the
loose sections and WORLD.NFSB are deleted (--prune): the user zips Azahar's
sdmc for testers and wants no unread files in it (2026-10-29).
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# The shipped settings. Anything not named here is the tool's own default.
WORLD_MAXDIM = 256      # TASKS.md #192/#193; track_pack.py's own default is 128
SKY_TIME = "night"
SKY_MAXDIM = 512


def run(script, args):
    cmd = [sys.executable, "-u", os.path.join(HERE, script)] + args
    print("\n=== %s ===" % " ".join(cmd[2:]), flush=True)
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit("%s failed (%d)" % (script, r.returncode))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pc_files", help='the "PC Files" directory')
    ap.add_argument("--out", required=True, metavar="DIR",
                    help="the world directory, e.g. "
                         "<sdmc>/nfsu2data/TRACKS/L4RA")
    ap.add_argument("--track", default="L4RA")
    ap.add_argument("--topology", action="store_true",
                    help="also rebuild WORLD.NFST from STREAM<track>.BUN")
    ap.add_argument("--skip-sky", action="store_true")
    ap.add_argument("--skip-shadow", action="store_true")
    ap.add_argument("--skip-afx", action="store_true")
    # ETC1 is the default since TASKS.md #427: the 16-bit bank (~112 MB on
    # L4RA) is too big for the world budget and drew at its 128px mip; the
    # ETC1 one (~31 MB) fits at 256px. --no-etc1 packs the old RGB565/4444.
    ap.add_argument("--no-etc1", dest="etc1", action="store_false",
                    help="pack the world texture bank as RGB565 / RGBA4444 "
                         "instead of ETC1 / ETC1A4 (track_pack.py --etc1)")
    args = ap.parse_args()

    tracks = os.path.join(args.pc_files, "TRACKS")
    if not os.path.isdir(tracks):
        sys.exit("no TRACKS under %s" % args.pc_files)
    os.makedirs(args.out, exist_ok=True)

    run("track_pack.py", [tracks, args.track,
                          "--maxdim", str(WORLD_MAXDIM),
                          "--out", args.out] + (["--etc1"] if args.etc1 else []))
    if not args.skip_sky:
        run("sky_pack.py", [tracks, args.track,
                            "--time", SKY_TIME,
                            "--maxdim", str(SKY_MAXDIM),
                            "--out", args.out])
    if not args.skip_shadow:
        run("shadow_pack.py",
            [os.path.join(args.pc_files, "GLOBAL", "GLOBALB.BUN"),
             "--out", args.out])
    if not args.skip_afx:
        run("afx_pack.py", [args.pc_files, "--out", args.out,
                            "--track", args.track])
    if args.topology:
        run("topo_pack.py", [tracks, args.out, "--track", args.track])
    run("world_archive.py", [args.out, "--verify", "--prune"])

    print("\nworld written to %s" % args.out)


if __name__ == "__main__":
    main()
