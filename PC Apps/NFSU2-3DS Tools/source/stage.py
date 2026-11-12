#!/usr/bin/env python3
"""
Repack and stage every file the port reads into an nfsu2data folder, from a
PC install of NFSU2 (TASKS.md #107, #320, #472). Runs on any system with
Python 3. The same with a window: tools/nfsu2_setup.pyw (the game data) and
tools/nfsu2_mods.pyw (mods); tools/stage_sd.ps1 is a PowerShell front end.

Two jobs:

  The game data (default)
      One step per output: a retail file copied as-is, or a packer run on
      the PC install. A step runs only when it is stale -- an output missing,
      fewer outputs than it should make, or an output older than its inputs
      (the PC files it reads, the packer, every tools/*.py the packer
      imports). Nothing outside the plan is touched: SAVE/, profiles/, mods/
      and anything else in the folder stay as they are.

        python tools/stage.py --pc "<PC install>" --gamedir <SD>/nfsu2data
        python tools/stage.py --list
        python tools/stage.py --only cars,tracks --tracks L4RG

  A mod (--mod)
      A PC mod is a folder laid out like the PC game's own (a mod for the
      Audi TT's textures is <mod>/CARS/TT/TEXTURES.BIN). It is laid over the
      PC install (tools/mod_overlay.py: nothing in the install is written),
      every step that reads one of its files is run into a scratch folder,
      and the packed files that differ from the stock ones go to
      <gamedir>/mods/<name>/ -- the folder the game lays over its data at
      boot (3DS Files/source/modfs.h). Files no step reads are listed: the
      .asi plugins and SPEED2.EXE patches are x86 code, which the 3DS cannot
      run, and a car the game has no slot for needs code too.

        python tools/stage.py --pc "<PC install>" --gamedir <SD>/nfsu2data \\
            --mod "<the mod's folder>" [--mod-name <name>]

The built apps (tools/build_apps.py) carry all of it: no Python needed, and
`nfsu2-3ds setup ...` / `nfsu2-3ds mods ...` take the options below. From
Python it needs Python 3 with numpy, Pillow, lz4, pefile, unicorn and
capstone; ffmpeg with libx264 (the .wav tracks, the .mvd movies, the audio
builders' resampling); and vgmstream-cli and dspenc (tools/bin has the
Windows builds; elsewhere, the system's, on PATH -- see tools/toolbin.py).
"""
import argparse
import copy
import fnmatch
import queue
import threading
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
FROZEN = bool(getattr(sys, "frozen", False))
# The repo's own PC copy; an exe (tools/build_apps.py) has none to default to.
DEFAULT_PC = None if FROZEN else os.path.join(ROOT, "PC Files")
# What runs a packer: this Python, or in the built apps the console program
# beside the exe, which takes `-u script.py args` as python.exe does.
PYTHON = sys.executable
if FROZEN:
    PYTHON = os.path.join(os.path.dirname(sys.executable), "nfsu2-3ds" + (".exe" if os.name == "nt" else ""))


def default_gamedir():
    if os.name == "nt" and os.environ.get("APPDATA"):
        return os.path.join(os.environ["APPDATA"], "Azahar", "sdmc", "nfsu2data")
    return os.path.join(os.path.expanduser("~"), ".local", "share", "azahar-emu", "sdmc", "nfsu2data")


# ---------------------------------------------------------------- the lists
# The cars the game loads: kit_pack.py's DEFAULT_CARS (the player's) and the
# traffic models traffic.c spawns.
PLAYER_CARS = ["240SX", "3000GT", "350Z", "A3", "CELICA", "CIVIC", "COROLLA", "CORSA",
               "ECLIPSE", "ESCALADE", "FOCUS", "G35", "GOLF", "GTO", "HUMMER", "IMPREZAWRX", "IS300",
               "LANCEREVO8", "MIATA", "MUSTANGGT", "NAVIGATOR", "PEUGOT", "PEUGOT106", "RSX", "RX7",
               "RX8", "SENTRA", "SKYLINE", "SUPRA", "TIBURON", "TT"]
TRAFFIC_CARS = ["4DR_SEDAN", "4DR_SEDAN02", "COUPE", "HATCHBACK", "HATCHBACK02", "MINIVAN",
                "PANELVAN", "PARCELVAN", "PICKUP", "SUV", "TAXI", "TAXI02"]
# The frontend rooms (MegaloShop01 is never entered).
ROOMS = ["Showroom", "Crib01", "CarLot01", "PerfShop01", "PartsShop01", "PaintShop01", "AudioShop01"]
# The NIS crowds: a number is EndingNis0N. These 20 IntroNis are the ones the
# career plays (TASKS.md #244).
CROWDS = ["01", "02", "03", "04", "ShopEnter1", "ShopExit1", "DynoEnter",
          "IntroNis01", "IntroNis02", "IntroNis03", "IntroNis05", "IntroNis06", "IntroNis07",
          "IntroNis08", "IntroNis10", "IntroNis12", "IntroNis13", "IntroNis14", "IntroNis15",
          "IntroNis19", "IntroNis20", "IntroNis21", "IntroNis22", "IntroNis24", "IntroNis25",
          "IntroNis26", "IntroNis27"]
ALL_TRACKS = ["L4RA", "L4RB", "L4RC", "L4RD", "L4RF", "L4RG"]
TRACK_SCRIPTS = ["pack_world.py", "track_pack.py", "sky_pack.py", "shadow_pack.py", "afx_pack.py",
                 "topo_pack.py", "world_archive.py", "lightdb_pack.py", "emevents_pack.py",
                 "smokeable_pack.py", "flare_pack.py"]
GROUPS = ["cars", "kits", "carextras", "frontend", "global", "text", "movies", "video",
          "audio", "music", "speech", "nis", "tracks"]


def movie_name(base):
    # main.c opens psa / thx_logo in lower case, the rest as retail names them.
    return {"PSA": "psa", "THX_logo": "thx_logo"}.get(base, base)


def find_ci(d, name):
    """`name` in folder `d`, matched case-free (the PC install may be on a
    case-sensitive disk); the plain join when it is not there."""
    try:
        for e in os.listdir(d):
            if e.upper() == name.upper():
                return os.path.join(d, e)
    except OSError:
        pass
    return os.path.join(d, name)


def pcp(pc, *parts):
    """A path under the PC install, each part matched case-free."""
    p = pc
    for part in parts:
        p = find_ci(p, part)
    return p


def list_ci(d, pattern):
    try:
        return sorted(os.path.join(d, e) for e in os.listdir(d)
                      if fnmatch.fnmatch(e.upper(), pattern.upper()) and os.path.isfile(os.path.join(d, e)))
    except OSError:
        return []


# ---------------------------------------------------------------- the steps
class Step:
    def __init__(self, group, name, src=None, dst=None, scripts=(), inputs=(), outputs=(),
                 expect=1, run=None, optional_inputs=False, reads=None, narrow=None):
        self.group, self.name = group, name
        self.src, self.dst = src, dst
        self.scripts, self.inputs, self.outputs = list(scripts), list(inputs), list(outputs)
        self.expect, self.run = expect, run
        self.optional_inputs = optional_inputs
        # For a mod (pack_mod): the PC files the packer really opens, as
        # upper-case patterns relative to the install ("CARS/*/TEXTURES.BIN"),
        # where `inputs` names a whole folder. None: any file under `inputs`.
        self.reads = reads
        # narrow(mod rels) -> the commands for just those files, or None for
        # the whole step (kit_pack: one car's kit rather than all of them).
        self.narrow = narrow

    @property
    def is_copy(self):
        return self.src is not None


def py(*args):
    # -u: unbuffered, so each packer's lines reach the log as they are printed.
    return [PYTHON, "-u", os.path.join(TOOLS, args[0])] + [str(a) for a in args[1:]]


def child_env():
    """The packers' environment: the bundled dspenc / vgmstream / ffmpeg first
    on PATH (toolbin.py), and Python's output unbuffered."""
    import toolbin
    env = dict(os.environ)
    env["PATH"] = toolbin.path_with_bins(env.get("PATH", ""))
    env["PYTHONUNBUFFERED"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    return env


def build_plan(pc, gd, tracks=ALL_TRACKS):
    steps = []
    J = os.path.join

    def copy(group, src, dst):
        steps.append(Step(group, "copy " + os.path.relpath(dst, gd).replace(os.sep, "/"), src=src, dst=dst))

    def pack(group, name, scripts, inputs, outputs, expect, cmds, optional_inputs=False,
             reads=None, narrow=None):
        steps.append(Step(group, name, scripts=scripts, inputs=inputs, outputs=outputs,
                          expect=expect, run=cmds, optional_inputs=optional_inputs,
                          reads=reads, narrow=narrow))

    def car_folders(rels):
        # The car folders a mod's files are in (CARS/<car>/...), or None when
        # one is anywhere else (CARS/TEXTURES.BIN, GLOBAL): the whole step.
        names = set()
        for r in rels:
            parts = r.split("/")
            if len(parts) < 3 or parts[0] != "CARS":
                return None
            names.add(parts[1])
        return sorted(names)

    def per_car(build, keep=lambda n: True):
        # narrow= for a packer that takes a list of cars: just the mod's.
        def narrow(rels):
            names = car_folders(rels)
            if names is None:
                return None
            names = [n for n in names if keep(n)]
            return build(names) if names else []
        return narrow

    # A car's kit (and PARTMODELS and the decal and glass kits, which
    # kit_pack always writes).
    kits_for = per_car(lambda ns: [py("kit_pack.py", pc, gd, "--cars", *ns, "--libs")],
                       lambda n: os.path.isfile(pcp(pc, "CARS", n, "GEOMETRY.BIN")))

    exe = pcp(pc, "SPEED2.EXE")
    cars, glob_ = pcp(pc, "CARS"), pcp(pc, "GLOBAL")
    for c in PLAYER_CARS + TRAFFIC_CARS:
        pack("cars", "car_pack " + c, ["car_pack.py"], [pcp(pc, "CARS", c), exe],
             [J(gd, "CARS", c + ".nfsc")], 1,
             [py("car_pack.py", pcp(pc, "CARS", c), J(gd, "CARS", c + ".nfsc"))])
    pack("kits", "kit_pack (car kits, part libraries, PARTMODELS)", ["kit_pack.py"], [cars, glob_, exe],
         [J(gd, "CARS", "*.nfsk"), J(gd, "GLOBAL", "PARTMODELS.nfsm")], 70, [py("kit_pack.py", pc, gd)],
         reads=["CARS/*", "GLOBAL/*", "SPEED2.EXE"], narrow=kits_for)
    pack("carextras", "carpartanim_pack", ["carpartanim_pack.py"], [cars], [J(gd, "CARS", "*.nfsa")], 31,
         [py("carpartanim_pack.py", pc, gd)], reads=["CARS/*/PARTS_ANIMATIONS.BIN"])
    pack("carextras", "carshadow_pack", ["carshadow_pack.py"], [cars, exe], [J(gd, "CARS", "*.nfss")], 31,
         [py("carshadow_pack.py", pc, "--out", J(gd, "CARS"))], reads=["CARS/*/TEXTURES.BIN", "SPEED2.EXE"],
         narrow=per_car(lambda ns: [py("carshadow_pack.py", pc, "--out", J(gd, "CARS"),
                                       *[a for n in ns for a in ("--car", n)])],
                        lambda n: n in PLAYER_CARS + TRAFFIC_CARS))
    pack("carextras", "vinyl_pack", ["vinyl_pack.py"], [cars, glob_], [J(gd, "CARS", "*.nfsv")], 31,
         [py("vinyl_pack.py", pc, "--out", J(gd, "CARS"), "-j", "4")],
         reads=["CARS/TEXTURES.BIN", "CARS/*/TEXTURES.BIN", "CARS/*/VINYLS.BIN", "GLOBAL/*"],
         narrow=per_car(lambda ns: [py("vinyl_pack.py", pc, *ns, "--out", J(gd, "CARS"), "-j", "4")],
                        lambda n: n in PLAYER_CARS))
    pack("carextras", "rim_pack", ["rim_pack.py"], [cars, glob_], [J(gd, "CARS", "WHEELS.nfsr")], 1,
         [py("rim_pack.py", pc, "--out", J(gd, "CARS"))],
         reads=["CARS/TEXTURES.BIN", "CARS/WHEELS/*", "GLOBAL/*"])

    for r in ROOMS:
        b = pcp(pc, "FRONTEND", "PLATFORMS", r + ".BIN")
        pack("frontend", "scene_pack " + r, ["scene_pack.py"], [b], [J(gd, "FRONTEND", r + ".nfsc")], 1,
             [py("scene_pack.py", b, J(gd, "FRONTEND", r + ".nfsc"))])
    for f in ["FrontB.lzc", "MAGAZINES_FRONTEND.BIN", "MAGAZINES_SHOWCASE.BIN",
              "MAGAZINES_STREAMING_HIGH.bin", "MAGAZINES_STREAMING_LOW.bin"]:
        copy("frontend", pcp(pc, "FRONTEND", f), J(gd, "FRONTEND", f))

    for f in ["GLOBALA.BUN", "GLOBALB.BUN", "InGameCommon.bun", "InGameDrag.bun", "InGameDrift.bun",
              "InGameFreeRoam.bun", "InGamePerformanceTuning.bun", "InGameRace.bun"]:
        copy("global", pcp(pc, "GLOBAL", f), J(gd, "GLOBAL", f))
    # None of these opens a file under CARS (2026-11-09: they had `cars` too,
    # so a car mod reran all nine for nothing).
    gin = [glob_, exe]
    gout = J(gd, "GLOBAL")
    for name, out, cmd in [
            ("hud_pack", "HUD.nfsh", py("hud_pack.py", pc, "--out", gout)),
            ("paint_pack", "PAINT.nfsp", py("paint_pack.py", pc, "--out", gout)),
            ("stockparts_pack", "STOCKPARTS.nfss", py("stockparts_pack.py", pc, "--out", gout)),
            ("neon_pack", "NEON.nfsn", py("neon_pack.py", pc, "--out", gout)),
            ("carlights_pack", "CARLIGHTS.nfsl", py("carlights_pack.py", pc, "--out", gout)),
            ("lightmat_pack", "LIGHTMATS.nfsl", py("lightmat_pack.py", pc, gd)),
            ("carfx_pack", "CARFX.nfsa", py("carfx_pack.py", pc, "--out", gout)),
            ("gps_arrow_pack", "GPSARROW.nfsc", py("gps_arrow_pack.py", pc, "--out", gout))]:
        pack("global", name, [name + ".py"], gin, [J(gout, out)], 1, [cmd])
    pack("global", "hull_pack", ["hull_pack.py"], [cars], [J(gout, "CARHULLS.nfsg")], 1,
         [py("hull_pack.py", cars, J(gout, "CARHULLS.nfsg"))], reads=["CARS/*/GEOMETRY.BIN"])

    copy("text", pcp(pc, "LANGUAGES", "English.bin"), J(gd, "LANGUAGES", "English.bin"))
    copy("text", pcp(pc, "CREDITS", "NA_ENGLISH.TXT"), J(gd, "CREDITS", "NA_ENGLISH.TXT"))
    copy("text", pcp(pc, "CREDITS", "UK_ENGLISH.TXT"), J(gd, "CREDITS", "UK_ENGLISH.TXT"))

    movies = [m for m in list_ci(pcp(pc, "MOVIES"), "*.vp6")
              if os.path.splitext(os.path.basename(m))[0].lower() != "blank"]
    for m in movies:
        n = movie_name(os.path.splitext(os.path.basename(m))[0])
        # The software decode path (Azahar, and any clip without an .mvd).
        copy("movies", m, J(gd, "MOVIES", n + ".vp6"))
        # The soundtrack main.c streams beside it: a plain ffmpeg remux.
        pack("audio", "wav " + n, [], [m], [J(gd, "audio", n + ".wav")], 1,
             [["ffmpeg", "-v", "error", "-y", "-i", m, "-vn", "-acodec", "pcm_s16le",
               J(gd, "audio", n + ".wav")]])
    pack("video", "fmv_mvd_pack --all (New 3DS MVD clips; slow)", ["fmv_mvd_pack.py"], [pcp(pc, "MOVIES")],
         [J(gd, "video", "*.mvd")], len(movies),
         [py("fmv_mvd_pack.py", "--src", pcp(pc, "MOVIES"), "--out", J(gd, "video"), "--all", "--verify")])

    fec = pcp(pc, "SOUND", "GLOBAL", "FE_COMMON_MB.abk")
    pack("audio", "fesfx_build FESFX.BIN", ["fesfx_build.py"], [fec], [J(gd, "audio", "FESFX.BIN")], 1,
         [py("fesfx_build.py", "--abk", fec, "--out", J(gd, "audio", "FESFX.BIN"))])
    femb = pcp(pc, "SOUND", "FE", "FE_MB.abk")
    pack("audio", "fesfx_build FEMB.BIN", ["fesfx_build.py"], [femb], [J(gd, "audio", "FEMB.BIN")], 1,
         [py("fesfx_build.py", "--abk", femb, "--out", J(gd, "audio", "FEMB.BIN"))])
    # The banks the runtime opens (audio.c): the SPU engines and their
    # sweeteners, eight in-game globals, and every NOS / SHIFTING / SKIDS /
    # TURBO bank.
    banks = []
    for i in range(41):
        banks += [("ENGINE", "CAR_%02d_ENG_MB_SPU" % i), ("ENGINE", "SWTN_CAR_%02d_MB" % i)]
    banks += [("IG_GLOBAL", b) for b in ["ENV_COMMON_MB", "FX_Misc_MB", "FX_Rain_MB", "MB_Stich_Collision",
                                        "MB_Stitch_Whoosh", "ROADNOISE_00_MB", "TRAFFIC_MB", "WIND_00_MB"]]
    for d in ["NOS", "SHIFTING", "SKIDS", "TURBO"]:
        banks += [(d, os.path.splitext(os.path.basename(f))[0]) for f in list_ci(pcp(pc, "SOUND", d), "*.abk")]
    for d, b in banks:
        src = pcp(pc, "SOUND", d, b + ".abk")
        out = J(gd, "audio", d, b + ".aem")
        pack("audio", "abk3ds %s/%s" % (d, b), ["abk3ds.py"], [src], [out], 1, [py("abk3ds.py", src, "-o", out)])
    for g in list_ci(pcp(pc, "SOUND", "ENGINE"), "*.gin"):
        copy("audio", g, J(gd, "audio", "ENGINE", os.path.basename(g)))
    for x in list_ci(pcp(pc, "SOUND", "MIXMAPS"), "*.mxb"):
        copy("audio", x, J(gd, "audio", "MIXMAPS", os.path.basename(x)))

    # The audio builders keep what they find unless --force; a stale step
    # means the source or the builder changed, so it rebuilds everything.
    pack("music", "eatrax_build (EA Trax)", ["eatrax_build.py"], [pcp(pc, "pfdata", "MusicSFx.mus")],
         [J(gd, "eatrax", "*.dspm"), J(gd, "eatrax", "tracks.txt")], 28,
         [py("eatrax_build.py", "--out", J(gd, "eatrax"), "--force")])
    pack("speech", "speech_build (career speech)", ["speech_build.py"],
         [pcp(pc, "speech", "UG2_Speech.big"), pcp(pc, "speech", "UG2_Speech.idx")],
         [J(gd, "speech", "g*.dspm"), J(gd, "speech", "UG2_Speech.idx")], 858,
         [py("speech_build.py", "--out", J(gd, "speech"), "--force")])
    pack("speech", "nisaudio_build (NIS streams)", ["nisaudio_build.py"], [pcp(pc, "speech", "nisaudio.big")],
         [J(gd, "speech", "colstream*.dspm"), J(gd, "speech", "thunder*.dspm")], 15,
         [py("nisaudio_build.py", "--out", J(gd, "speech"), "--force")])

    crowd_out = [J(gd, "NIS", ("EndingNis%s" % c if c.isdigit() else c) + ".crowd") for c in CROWDS]
    pack("nis", "crowd_pack (%d scenes)" % len(CROWDS), ["crowd_pack.py"], [pcp(pc, "NIS")], crowd_out,
         len(CROWDS), [py("crowd_pack.py", pc, J(gd, "NIS"), *CROWDS)])
    for f in ["Scene_CareerNis1_BundleB.bun", "Scene_ShopEnter1_EvtTrk.txt", "Scene_ShopExit1_EvtTrk.txt"]:
        copy("nis", pcp(pc, "NIS", f), J(gd, "NIS", f))

    # A track set: pack_world.py's archive, collision and index, the per-set
    # packs, then the set's .BUN and ROUTES copied (what pack_track_set.ps1
    # did; its packers are the step's scripts, so editing one repacks).
    tdir = pcp(pc, "TRACKS")
    for t in tracks:
        tin = sorted(os.path.join(tdir, e) for e in os.listdir(tdir) if t.upper() in e.upper()) \
            if os.path.isdir(tdir) else []
        out = J(gd, "TRACKS", t)
        cmds = [py("pack_world.py", pc, "--out", out, "--track", t, "--topology"),
                py("lightdb_pack.py", pc, gd, "--track", t),
                py("emevents_pack.py", pc, gd, "--track", t),
                ("optional", py("smokeable_pack.py", pc, gd, "--track", t)),
                py("flare_pack.py", pc, gd, "--track", t),
                ("copy", pcp(pc, "TRACKS", t + ".BUN"), J(gd, "TRACKS", t + ".BUN")),
                ("copydir", pcp(pc, "TRACKS", "ROUTES" + t), J(gd, "TRACKS", "ROUTES" + t))]
        pack("tracks", "pack_track_set " + t, TRACK_SCRIPTS, tin,
             [J(out, "WORLD.NFSZ"), J(out, "WORLD.NFSX"), J(out, "WORLD.NFST"), J(out, "WORLD.NFSL"),
              J(out, "WORLD.NFSE"), J(out, "WORLD.NFSF"), J(gd, "TRACKS", t + ".BUN")], 7, cmds)
    # The track maps trackmap.c draws on the world map and race HUD.
    for m in list_ci(tdir, "TRACKMAP*.BIN"):
        copy("tracks", m, J(gd, "TRACKS", os.path.basename(m)))
    return steps


# ---------------------------------------------------------------- staleness
_dep_cache = {}


def script_deps(name):
    """A packer and every tools/*.py it imports, recursively."""
    if name in _dep_cache:
        return _dep_cache[name]
    seen, todo = set(), [name]
    while todo:
        n = todo.pop()
        p = os.path.join(TOOLS, n)
        if p in seen or not os.path.isfile(p):
            continue
        seen.add(p)
        if not n.endswith(".py"):
            continue
        for line in open(p, encoding="utf-8", errors="replace"):
            m = re.match(r"\s*from\s+([A-Za-z_]\w*)\s+import\b", line)
            if m:
                todo.append(m.group(1) + ".py")
                continue
            m = re.match(r"\s*import\s+(.+)$", line)
            if m:
                for mod in m.group(1).split(","):
                    mod = mod.strip().split()[0] if mod.strip() else ""
                    if re.match(r"^[A-Za-z_]\w*$", mod):
                        todo.append(mod + ".py")
    _dep_cache[name] = sorted(seen)
    return _dep_cache[name]


_mtime_cache = {}


def newest(paths):
    t = 0.0
    for p in paths:
        if p not in _mtime_cache:
            if os.path.isdir(p):
                x = 0.0
                for dp, _ds, fs in os.walk(p, followlinks=True):
                    for f in fs:
                        x = max(x, os.path.getmtime(os.path.join(dp, f)))
                _mtime_cache[p] = x
            elif os.path.isfile(p):
                _mtime_cache[p] = os.path.getmtime(p)
            else:
                raise FileNotFoundError("input missing: " + p)
        t = max(t, _mtime_cache[p])
    return t


def state(s):
    """"ok", or why the step has to run."""
    if s.is_copy:
        if not os.path.isfile(s.src):
            raise FileNotFoundError("retail file missing: " + s.src)
        if not os.path.isfile(s.dst):
            return "missing"
        if os.path.getsize(s.src) != os.path.getsize(s.dst):
            return "size differs"
        if os.path.getmtime(s.dst) < os.path.getmtime(s.src):
            return "older than retail"
        return "ok"
    deps = []
    for sc in s.scripts:
        deps += script_deps(sc)
    t_in = newest(list(s.inputs) + deps)
    n, oldest = 0, float("inf")
    for o in s.outputs:
        fs = [f for f in glob.glob(o) if os.path.isfile(f)]
        if not fs:
            return "missing " + os.path.basename(o)
        n += len(fs)
        oldest = min([oldest] + [os.path.getmtime(f) for f in fs])
    if n < s.expect:
        return "%d of %d outputs" % (n, s.expect)
    if oldest < t_in:
        return "older than its inputs"
    return "ok"


# ---------------------------------------------------------------- running
class Stopped(Exception):
    pass


def run_cmd(cmd, log, stop):
    """One command, its output passed to `log` line by line. A packer that
    prints nothing for a while (kit_pack, a track set) gets a "still working"
    line every 15 s, so a long step never looks stuck, and Stop is heard
    within a second even then."""
    if not os.path.dirname(cmd[0]):
        # Windows looks a bare name up on this process's PATH, not the one
        # handed to the child: name the bundled ffmpeg outright.
        import toolbin
        cmd = [toolbin.find(cmd[0])] + list(cmd[1:])
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding="utf-8", errors="replace", env=child_env(),
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    q = queue.Queue()

    def pump():
        for line in p.stdout:
            q.put(line)
        q.put(None)

    threading.Thread(target=pump, daemon=True).start()
    t0 = last = time.time()
    while True:
        try:
            line = q.get(timeout=1.0)
        except queue.Empty:
            line = ""
        if line is None:
            break
        if line:
            log("    " + line.rstrip())
            last = time.time()
        elif time.time() - last >= 15:
            log("    ... still working (%d s)" % (time.time() - t0))
            last = time.time()
        if stop and stop():
            p.kill()
            raise Stopped()
    return p.wait()


def run_step(s, log, stop=None):
    if s.is_copy:
        os.makedirs(os.path.dirname(s.dst), exist_ok=True)
        shutil.copyfile(s.src, s.dst)
        return
    for o in s.outputs:
        os.makedirs(os.path.dirname(o), exist_ok=True)
    for c in s.run:
        if isinstance(c, tuple) and c[0] == "copy":
            os.makedirs(os.path.dirname(c[2]), exist_ok=True)
            shutil.copyfile(c[1], c[2])     # a fresh time, so the set reads as up to date
        elif isinstance(c, tuple) and c[0] == "copydir":
            os.makedirs(c[2], exist_ok=True)
            for f in os.listdir(c[1]):
                if os.path.isfile(os.path.join(c[1], f)):
                    shutil.copyfile(os.path.join(c[1], f), os.path.join(c[2], f))
        elif isinstance(c, tuple) and c[0] == "optional":
            if run_cmd(c[1], log, stop) != 0:
                log("    (optional: %s wrote nothing)" % os.path.basename(next((x for x in c[1] if x.endswith(".py")), c[1][-1])))
        else:
            rc = run_cmd(c, log, stop)
            if rc != 0:
                raise RuntimeError("%s failed (exit %d)" % (
                    os.path.basename(next((x for x in c if x.endswith(".py")), c[0])), rc))
    # A narrowed step (a mod's one car) writes only its share of the outputs.
    if getattr(s, "narrowed", False):
        return
    after = state(s)
    if after != "ok":
        raise RuntimeError("%s ran but its outputs are still stale: %s" % (s.name, after))


def check_install(pc):
    """The PC install's must-haves, as a list of what is missing."""
    need = [("SPEED2.EXE",), ("CARS",), ("GLOBAL", "GLOBALB.BUN"), ("TRACKS",), ("LANGUAGES", "English.bin"),
            ("FRONTEND", "FrontB.lzc"), ("SOUND",), ("MOVIES",)]
    return ["/".join(n) for n in need if not os.path.exists(pcp(pc, *n))]


def stage(pc, gd, only=(), tracks=ALL_TRACKS, force=False, list_only=False, log=print, stop=None,
          progress=None):
    missing = check_install(pc)
    if missing:
        raise SystemExit("not an NFSU2 PC install (%s missing): %s" % (", ".join(missing), pc))
    # The packers that open the install by a fixed path (eatrax_build,
    # speech_build ...) read this one.
    os.environ["NFSU2_PC_ROOT"] = os.path.abspath(pc)
    steps = build_plan(pc, gd, tracks)
    for g in only:
        if g not in GROUPS:
            raise SystemExit("unknown group '%s' (groups: %s)" % (g, ", ".join(GROUPS)))
    sel = [s for s in steps if not only or s.group in only]
    t0, ran, fresh = time.time(), 0, 0
    for i, s in enumerate(sel):
        st = state(s)
        if progress:
            progress(i, len(sel), s.name)
        if list_only:
            log("%-10s %-60s %s" % (s.group, s.name, st))
            continue
        if st == "ok" and not force:
            fresh += 1
            continue
        log("[%s] %s (%s)" % (s.group, s.name, "forced" if st == "ok" else st))
        ts = time.time()
        run_step(s, log, stop)
        if not s.is_copy:
            log("    %.0f s" % (time.time() - ts))
        ran += 1
        if stop and stop():
            raise Stopped()
    if progress:
        progress(len(sel), len(sel), "done")
    if not list_only:
        log("stage: %d step(s) run, %d already up to date, %.0f s, into %s" % (ran, fresh, time.time() - t0, gd))
    return steps


def report_extra(steps, gd, tracks, log=print):
    """Files under GameDir that no step makes (the deprecated-file rule)."""
    made = set()
    owned = [os.path.join(gd, d) + os.sep for d in ("SAVE", "profiles", "speech", "eatrax", "mods")]
    for t in tracks:
        owned += [os.path.join(gd, "TRACKS", t) + os.sep, os.path.join(gd, "TRACKS", "ROUTES" + t) + os.sep]
    for s in steps:
        if s.is_copy:
            made.add(os.path.normcase(s.dst))
            continue
        for o in s.outputs:
            made.update(os.path.normcase(f) for f in glob.glob(o))
    extra = []
    for dp, _ds, fs in os.walk(gd):
        for f in fs:
            p = os.path.join(dp, f)
            if os.path.normcase(p) in made or any(os.path.normcase(p).startswith(os.path.normcase(o)) for o in owned):
                continue
            extra.append(p)
    log("stage: %d file(s) under GameDir that no step makes:" % len(extra))
    for p in extra:
        log("  %s  (%d bytes)" % (os.path.relpath(p, gd), os.path.getsize(p)))


# ---------------------------------------------------------------- mods
def car_folders_of(rels):
    return sorted(set(r.split("/")[1] for r in rels if r.count("/") >= 2))


def _touches(path, mod_files):
    p = os.path.normcase(os.path.normpath(path))
    return [m for m in mod_files if m == p or m.startswith(p + os.sep)]


def pack_mod(pc, gd, mod_dir, name=None, out=None, log=print, stop=None, progress=None, keep_work=False):
    """Packs a PC mod into GameDir/mods/<name> (see the module's help).
    Returns (files written, the mod files no step reads)."""
    import mod_overlay
    mod_dir = os.path.abspath(mod_dir)
    if not os.path.isdir(mod_dir):
        raise SystemExit("no mod folder: " + mod_dir)
    name = name or os.path.basename(mod_dir.rstrip("\\/"))
    if not re.match(r"^[A-Za-z0-9 _.-]{1,63}$", name):
        raise SystemExit("a mod's name is letters, digits, space, '_', '.' and '-', at most 63: " + name)
    out = out or os.path.join(gd, "mods", name)
    missing = check_install(pc)
    if missing:
        raise SystemExit("not an NFSU2 PC install (%s missing): %s" % (", ".join(missing), pc))
    work = tempfile.mkdtemp(prefix="nfsu2_mod_")
    ov, staged, lst = os.path.join(work, "pc"), os.path.join(work, "out"), os.path.join(work, "modfiles.txt")
    old_env = os.environ.get("NFSU2_PC_ROOT")
    try:
        log("mod: %s -> %s" % (mod_dir, out))
        mod_overlay.create(pc, mod_dir, ov, lst)
        mods = {}
        for line in open(lst, encoding="utf-8"):
            if "\t" in line:
                p, rel = line.rstrip("\n").split("\t", 1)
                mods[os.path.normcase(os.path.normpath(p))] = rel
        # The packers that open the install by a fixed path read the overlay.
        os.environ["NFSU2_PC_ROOT"] = ov
        _mtime_cache.clear()
        steps = build_plan(ov, staged)
        used, sel = set(), []
        for s in steps:
            hit = _touches(s.src, mods) if s.is_copy else \
                [m for i in s.inputs for m in _touches(i, mods)]
            if hit and s.reads is not None:
                hit = [m for m in hit
                       if any(fnmatch.fnmatchcase(mods[m].replace("\\", "/").upper(), r) for r in s.reads)]
            if not hit:
                continue
            used.update(hit)
            if s.narrow:
                cmds = s.narrow(sorted(set(mods[m].replace("\\", "/").upper() for m in hit)))
                if cmds == []:
                    continue
                if cmds is not None:
                    s = copy.copy(s)
                    s.run = cmds
                    s.narrowed = True
                    s.name += " (only " + ", ".join(car_folders_of(
                        mods[m].replace("\\", "/").upper() for m in hit)) + ")"
            sel.append(s)
        unused = sorted(rel for p, rel in mods.items() if p not in used)
        log("mod: %d file(s); %d step(s) read them" % (len(mods), len(sel)))
        for i, s in enumerate(sel):
            if progress:
                progress(i, len(sel), s.name)
            log("[%s] %s" % (s.group, s.name))
            ts = time.time()
            run_step(s, log, stop)
            if not s.is_copy:
                log("    %.0f s" % (time.time() - ts))
            if stop and stop():
                raise Stopped()
        if progress:
            progress(len(sel), len(sel), "collecting")
        if os.path.isdir(out):
            if not os.path.isfile(os.path.join(out, "MODPACK.TXT")):
                raise SystemExit("%s is there and was not made by this tool; pick another name" % out)
            shutil.rmtree(out)     # a folder this tool made: repacked whole
        n = 0
        if sel:
            n = mod_overlay.collect(staged, gd if os.path.isdir(gd) else None, out, mod_dir)
        for rel in unused:
            log("mod: not read by the port: %s" % rel)
        if unused:
            log("mod: .asi plugins and SPEED2.EXE patches are PC code the 3DS cannot run; a new car "
                "folder needs a car slot the game does not have.")
        if progress:
            progress(len(sel), len(sel), "done")
        log("mod: %d file(s) in %s" % (n, out) if n else "mod: nothing to install -- no file of this mod is read by the port")
        return n, unused
    finally:
        if old_env is None:
            os.environ.pop("NFSU2_PC_ROOT", None)
        else:
            os.environ["NFSU2_PC_ROOT"] = old_env
        if not keep_work:
            mod_overlay.remove(ov)
            shutil.rmtree(work, ignore_errors=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    if DEFAULT_PC and os.path.isdir(DEFAULT_PC):
        ap.add_argument("--pc", default=DEFAULT_PC, help="the NFSU2 PC install (default: the repo's PC Files)")
    else:
        ap.add_argument("--pc", required=True, help="the NFSU2 PC install (the folder SPEED2.EXE is in)")
    ap.add_argument("--gamedir", default=default_gamedir(), help="the nfsu2data folder (default: Azahar's)")
    ap.add_argument("--only", default="", help="step groups: " + ", ".join(GROUPS))
    ap.add_argument("--tracks", default=",".join(ALL_TRACKS))
    ap.add_argument("--force", action="store_true", help="run every selected step")
    ap.add_argument("--list", action="store_true", help="print each step's state and run nothing")
    ap.add_argument("--report-extra", action="store_true", help="list files no step makes")
    ap.add_argument("--mod", help="pack this PC mod folder instead of the game data")
    ap.add_argument("--mod-name", help="the mod's folder name under mods/ (default: the mod folder's)")
    ap.add_argument("--mod-out", help="where to write it (default: <gamedir>/mods/<name>)")
    ap.add_argument("--keep-work", action="store_true", help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    if sys.stdout.encoding and sys.stdout.encoding.lower() != "utf-8":
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    tracks = [t for t in a.tracks.split(",") if t]
    if a.mod:
        pack_mod(a.pc, a.gamedir, a.mod, a.mod_name, a.mod_out, keep_work=a.keep_work)
        return 0
    steps = stage(a.pc, a.gamedir, [g for g in a.only.split(",") if g], tracks, a.force, a.list)
    if a.report_extra:
        report_extra(steps, a.gamedir, tracks)
    return 0


if __name__ == "__main__":
    sys.path.insert(0, TOOLS)
    sys.exit(main())
