# Builds race.c and trackroute.c on the PC and prints the AI line a race's
# field drives (trackroute.h, TASKS.md #272): which posted TrackRoute each
# stretch of the race route was matched to, as laterals across the race route.
# Needs a host gcc -- devkitPro's MSYS2 one is used if gcc is not on PATH.
#
# Usage:
#   python tools/ailine_dump.py <track> [--reverse] [--from M] [--to M] [--every N]
#   python tools/ailine_dump.py --all          (one summary line per track)
#   python tools/ailine_dump.py <track> --speed [--class N]
#   python tools/ailine_dump.py --all --speed  (one comparison line per track)
#
# --speed builds aidriver.c's cornering profile (aispeed_build) twice, once
# along the racing line and once along the race route's own spline, and prints
# what the field would be held to either way. That is the measurement behind
# TASKS.md #272's "the limits come off a line no car drives": on the spline a
# corner that is not on the AI's road still slows them, and a corner that is
# does not. Comparing lap times from Azahar cannot answer it -- a driver
# re-rolls its lookahead every 5 s, so two runs of the same race differ by
# tens of seconds.
#
# The data comes from "PC Files/TRACKS/ROUTESL4RA", the files the SD copy is
# made from. The code is the shipping source, compiled as it is; <3ds.h> is
# replaced by a stub that only declares the integer types.
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "3DS Files", "source")
DATA = os.path.join(ROOT, "PC Files", "TRACKS", "ROUTESL4RA")

STUB_3DS_H = """// Host stand-in for libctru's <3ds.h>: the integer types only.
#pragma once
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef s32 Result; typedef u32 Handle;
"""

MAIN_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "race.h"
#include "trackroute.h"
#include "aidriver.h"

static int quiet;
static void lg(const char* m) { if (!quiet) printf("%s\n", m); }

// --speed: the cornering profile along the line against the one along the
// spline, at the same distances. The pair of figures that matters is how much
// of the route each holds below the other, and by how much.
static int speedMode(const RaceRoute* r, const AiLine* t, int cls) {
	const float top = 56.8f, accel = 7.0f;   // the class-3 figures a Bayview
	                                         // career field reports on Azahar
	AiSpeedProfile* onLine = aispeed_build(r, t, cls, top, accel, 0.0f, NULL);
	AiSpeedProfile* onPath = aispeed_build(r, NULL, cls, top, accel, 0.0f, NULL);
	if (!onLine || !onPath) { printf("no profile\n"); return 1; }
	const float len = race_route_length(r);
	int n = 0, slower = 0, faster = 0;
	float sumL = 0.0f, sumP = 0.0f, worstSlow = 0.0f, worstFast = 0.0f;
	float atSlow = 0.0f, atFast = 0.0f;
	for (float d = 0.0f; d < len; d += 5.0f) {
		const float a = aispeed_at(onLine, d), b = aispeed_at(onPath, d);
		sumL += a; sumP += b; n++;
		const float diff = a - b;
		if (diff < -0.5f) {
			slower++;
			if (-diff > worstSlow) { worstSlow = -diff; atSlow = d; }
		} else if (diff > 0.5f) {
			faster++;
			if (diff > worstFast) { worstFast = diff; atFast = d; }
		}
	}
	printf("speed: %d samples, mean %.1f m/s on the line against %.1f on the spline "
		"(%+.1f); the line is slower at %d (worst %.1f m/s at %.0f m) and faster at "
		"%d (worst %.1f m/s at %.0f m); slowest %.1f against %.1f\n",
		n, sumL / (float)n, sumP / (float)n, (sumL - sumP) / (float)n,
		slower, worstSlow, atSlow, faster, worstFast, atFast,
		aispeed_min(onLine), aispeed_min(onPath));
	aispeed_free(onLine);
	aispeed_free(onPath);
	return 0;
}

int main(int argc, char** argv) {
	// paths routes track reverse from to every quiet [class]
	const char* paths = argv[1];
	const char* routes = argv[2];
	int track = atoi(argv[3]);
	int reverse = atoi(argv[4]);
	float from = (float)atof(argv[5]), to = (float)atof(argv[6]);
	float every = (float)atof(argv[7]);
	quiet = atoi(argv[8]);
	RaceRoute* r = race_route_open(paths, track, reverse != 0, lg);
	if (!r) { printf("no route\n"); return 1; }
	TrackRouteSet* s = trackroute_open(routes, lg);
	quiet = 0;
	AiLine* t = ailine_build(s, r, -1000.0f, NULL, 0, NULL, lg);
	if (!t) { printf("no line\n"); return 1; }
	if (argc > 9) {
		int rc = speedMode(r, t, atoi(argv[9]));
		ailine_free(t);
		trackroute_close(s);
		race_route_close(r);
		return rc;
	}
	float len = race_route_length(r);
	if (to > len) to = len;
	for (float d = from; d <= to && every > 0.0f; d += every) {
		AiLineSample a;
		float P[2], D[2], L[2];
		ailine_at(t, d, &a);
		race_route_sample(r, d, 0.0f, P, D);
		race_route_sample(r, d, a.line, L, NULL);
		printf("d %7.1f  route (%8.1f,%8.1f)  line %+6.1f  point %+6.1f  road %+6.1f..%+6.1f  "
			"z %6.1f  dir %+.2f,%+.2f  -> (%8.1f,%8.1f)\n",
			d, P[0], P[1], a.line, a.point, a.left, a.right, a.z, a.dir[0], a.dir[1], L[0], L[1]);
	}
	ailine_free(t);
	trackroute_close(s);
	race_route_close(r);
	return 0;
}
"""


def find_gcc():
    gcc = shutil.which("gcc")
    if gcc:
        return gcc
    for cand in (r"C:\devkitPro\msys2\usr\bin\gcc.exe", "/c/devkitPro/msys2/usr/bin/gcc"):
        if os.path.exists(cand):
            return cand
    sys.exit("no host gcc found (looked on PATH and in devkitPro's MSYS2)")


def build(out):
    os.makedirs(os.path.join(out, "inc"), exist_ok=True)
    with open(os.path.join(out, "inc", "3ds.h"), "w", encoding="utf-8") as f:
        f.write(STUB_3DS_H)
    with open(os.path.join(out, "main.c"), "w", encoding="utf-8") as f:
        f.write(MAIN_C)
    gcc = find_gcc()
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(gcc) + os.pathsep + env.get("PATH", "")
    exe = os.path.join(out, "ailine_dump.exe")
    cmd = [gcc, "-O1", "-std=gnu11", "-Wall", "-Wno-unused-function",
           "-I", os.path.join(out, "inc"), "-I", SRC,
           os.path.join(out, "main.c"), os.path.join(SRC, "race.c"),
           os.path.join(SRC, "trackpath.c"), os.path.join(SRC, "trackroute.c"),
           os.path.join(SRC, "aidriver.c"), os.path.join(SRC, "carsim.c"),
           "-lm", "-o", exe]
    subprocess.run(cmd, check=True, env=env)
    return exe, env


def run(exe, env, track, reverse, frm, to, every, quiet, speed_class=None):
    paths = os.path.join(DATA, "Paths%d.bin" % track)
    routes = os.path.join(DATA, "Routes%d%s.bin" % (track, "B" if reverse else "F"))
    argv = [exe, paths, routes, str(track), "1" if reverse else "0",
            str(frm), str(to), str(every), "1" if quiet else "0"]
    if speed_class is not None:
        argv.append(str(speed_class))
    return subprocess.run(argv, capture_output=True, text=True, env=env).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("track", nargs="?", type=int)
    ap.add_argument("--reverse", action="store_true")
    ap.add_argument("--from", dest="frm", type=float, default=0.0)
    ap.add_argument("--to", type=float, default=1e9)
    ap.add_argument("--every", type=float, default=10.0)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--speed", action="store_true",
                    help="compare the cornering profile on the line against the spline")
    ap.add_argument("--class", dest="cls", type=int, default=3,
                    help="car class for --speed (0-3, default 3)")
    ap.add_argument("--keep", help="build here and keep it, instead of a temp dir")
    args = ap.parse_args()

    out = args.keep or tempfile.mkdtemp(prefix="ailine_dump_")
    exe, env = build(out)
    try:
        if args.all:
            tracks = sorted({int(m.group(1)) for m in
                             (re.match(r"Routes(\d+)([FB])\.bin$", n) for n in os.listdir(DATA)) if m})
            for t in tracks:
                for rev in (False, True):
                    if not os.path.exists(os.path.join(DATA, "Paths%d.bin" % t)):
                        continue
                    txt = run(exe, env, t, rev, 0, 0, 0, True,
                              args.cls if args.speed else None)
                    # The summary, not the first "ai line:" -- a bridged gap
                    # logs under that prefix too, and there can be several.
                    want = "speed:" if args.speed else "ai line"
                    line = next((l for l in txt.splitlines()
                                 if (l.startswith(want) and
                                     (args.speed or " samples " in l))
                                 or l.startswith("no ")), txt.strip()[:120])
                    print("%d%s  %s" % (t, "B" if rev else "F", line))
        else:
            sys.stdout.write(run(exe, env, args.track, args.reverse, args.frm, args.to,
                                 args.every, args.speed,
                                 args.cls if args.speed else None))
    finally:
        if not args.keep:
            shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    main()
