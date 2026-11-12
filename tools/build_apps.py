#!/usr/bin/env python3
"""
Builds the NFSU2-3DS PC apps (TASKS.md #320, #472) into one folder that needs
no Python: PyInstaller (pip install pyinstaller) over tools/nfsu2_app.py.

    python tools/build_apps.py [--out "<folder>"] [--zip]

The folder (default: <repo>/PC Apps/NFSU2-3DS Tools):

    NFSU2-3DS Data Setup(.exe)   the Data Setup window
    NFSU2-3DS Mod Packer(.exe)   the Mod Packer window
    nfsu2-3ds(.exe)              the command line (`nfsu2-3ds --help`); the
                                 window apps run the packers through it too
    bin/                         dspenc, vgmstream-cli and ffmpeg (Windows),
                                 each with its licence
    _internal/                   Python, its libraries, and _internal/tools:
                                 the packers' own sources, run from there
    source/                      what it is all built from: the apps' and
                                 packers' .py files, this script, the icons
                                 (make_app_icons.py) and dspenc.c; it runs
                                 from Python and rebuilds the exes as it is
    README.txt

The packers stay real .py files under _internal/tools rather than inside the
archive: stage.py dates its outputs against them, and they open one another
by path (pack_world.py). Only what they import is frozen in.

On Linux and macOS the same command makes the same three programs (no .exe);
bin/ is left out there and the system's ffmpeg / vgmstream-cli / dspenc are
used from PATH (tools/toolbin.py says where each is looked for).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import stage  # noqa: E402
import toolbin  # noqa: E402

NAMES = {"cli": "nfsu2-3ds", "setup": "NFSU2-3DS Data Setup", "mods": "NFSU2-3DS Mod Packer"}
ICONS = os.path.join(TOOLS, "app_icons")       # tools/make_app_icons.py draws them
# Run from the repo: <repo>/PC Apps/NFSU2-3DS Tools. Run from a built folder's
# source/ (the copy this script puts there): a new folder beside that one,
# since a folder cannot be rebuilt over while this runs from inside it.
IN_APP = os.path.isdir(os.path.join(ROOT, "_internal"))
DEFAULT_OUT = (os.path.join(os.path.dirname(ROOT), os.path.basename(ROOT) + " (rebuilt)") if IN_APP
               else os.path.join(ROOT, "PC Apps", "NFSU2-3DS Tools"))
SOURCE_EXTRA = ["build_apps.py", "make_app_icons.py", "nfsu2_mods.pyw", "nfsu2_setup.pyw",
                os.path.join("src", "dspenc.c")]
NIS = os.path.join(TOOLS, "re", "nis")
THIRD_PARTY = ["numpy", "PIL", "PIL.Image", "lz4", "lz4.block", "lz4.frame", "pefile", "unicorn",
               "capstone", "tkinter", "concurrent.futures"]


def tool_files():
    """Every tools/*.py a step of the plan runs or imports, and the apps'."""
    pc = stage.DEFAULT_PC if stage.DEFAULT_PC and os.path.isdir(stage.DEFAULT_PC) else None
    scripts = {"nfsu2_app.py", "nfsu2_gui.py", "stage.py", "mod_overlay.py", "toolbin.py", "pack_world.py"}
    scripts.update(stage.TRACK_SCRIPTS)
    if pc:
        for s in stage.build_plan(pc, tempfile.gettempdir()):
            scripts.update(s.scripts)
            for c in s.run or []:
                if isinstance(c, tuple) and c[0] == "optional":
                    c = c[1]
                if isinstance(c, list):
                    scripts.update(os.path.basename(x) for x in c if str(x).endswith(".py"))
    else:
        # No PC copy here to plan against: every packer in the folder.
        scripts.update(f for f in os.listdir(TOOLS) if f.endswith((".py",)) and
                       re.search(r"_(pack|build)\.py$", f))
        scripts.update(["abk3ds.py", "kit_pack.py", "fmv_mvd_pack.py"])
    files = set()
    for s in scripts:
        files.update(stage.script_deps(s))
    files.update(os.path.join(NIS, f) for f in os.listdir(NIS) if f.endswith(".py"))
    return sorted(files)


def spec_text(work, icons, mods):
    app = os.path.join(TOOLS, "nfsu2_app.py")
    exes = []
    for key, console in (("cli", True), ("setup", False), ("mods", False)):
        exes.append("""EXE(pyz, a.scripts, [], exclude_binaries=True, name=%r, console=%s,
    icon=%r, upx=False, debug=False, strip=False)""" % (NAMES[key], console, icons.get(key) or "NONE"))
    return """# Written by tools/build_apps.py; not kept.
from PyInstaller.utils.hooks import collect_all
datas, binaries, hidden = [], [], %r
for pkg in ("unicorn", "capstone"):
    d, b, h = collect_all(pkg)
    datas += d; binaries += b; hidden += h
LOCAL = set(%r)
a = Analysis([%r], pathex=[%r, %r], binaries=binaries, datas=datas,
             hiddenimports=hidden + sorted(LOCAL), excludes=["matplotlib", "scipy", "pytest", "IPython"],
             noarchive=False)
# The tools' own modules load from _internal/tools (copied there after the
# build), never from the archive: drop them from it.
a.pure = [t for t in a.pure if t[0] not in LOCAL]
pyz = PYZ(a.pure)
exes = [%s]
coll = COLLECT(*exes, a.binaries, a.datas, strip=False, upx=False, name="app")
""" % (THIRD_PARTY, sorted(mods), app, TOOLS, NIS, ",\n    ".join(exes))


def no_cfg(exe):
    """Clears Control Flow Guard on a built exe. PyInstaller's launcher has
    it on and python.exe does not; with it on, Windows checks every longjmp
    target, and unicorn's (QEMU's, in a MinGW DLL that registers none) fail
    that check: huff.py, so car_pack and everything that reads compressed
    car data, died with 0xC0000409 the moment SPEED2.EXE's decoder ran."""
    import pefile
    pe = pefile.PE(exe, fast_load=True)
    off = pe.OPTIONAL_HEADER.get_field_absolute_offset("DllCharacteristics")
    v = pe.OPTIONAL_HEADER.DllCharacteristics
    pe.close()
    with open(exe, "r+b") as fh:
        fh.seek(off)
        fh.write((v & ~0x4000).to_bytes(2, "little"))   # IMAGE_DLLCHARACTERISTICS_GUARD_CF


def ffmpeg_exe():
    p = toolbin.find("ffmpeg")
    if not os.path.isfile(p):
        raise SystemExit("ffmpeg (with libx264) is needed to bundle; put it on PATH")
    p = os.path.realpath(p)
    conf = subprocess.run([p, "-hide_banner", "-buildconf"], capture_output=True, text=True).stdout
    if "--enable-libx264" not in conf:
        raise SystemExit("%s has no libx264: the .mvd movies need it" % p)
    return p


def copy_bins(dst):
    """bin/: dspenc, vgmstream-cli (with its DLLs) and ffmpeg, as toolbin.py
    looks for them beside a frozen app."""
    b = os.path.join(dst, "bin")
    os.makedirs(b, exist_ok=True)
    dsp = toolbin.find("dspenc")
    if not os.path.isfile(dsp):
        raise SystemExit("dspenc.exe not found (tools/bin, or the app's bin/)")
    shutil.copy2(dsp, os.path.join(b, "dspenc.exe"))
    shutil.copy2(os.path.join(TOOLS, "src", "dspenc.c"), os.path.join(b, "dspenc.c"))
    vg = toolbin.find("vgmstream-cli")
    if not os.path.isfile(vg):
        raise SystemExit("vgmstream-cli.exe not found (tools/bin/vgmstream, or the app's bin/vgmstream)")
    shutil.copytree(os.path.dirname(vg), os.path.join(b, "vgmstream"), dirs_exist_ok=True)
    ff = ffmpeg_exe()
    fd = os.path.join(b, "ffmpeg")
    os.makedirs(fd, exist_ok=True)
    shutil.copy2(ff, os.path.join(fd, "ffmpeg.exe"))
    for f in ("LICENSE", "README.txt"):
        # A winget / gyan.dev build keeps them above bin/; an app's bin/ffmpeg beside the exe.
        for d in (os.path.dirname(ff), os.path.dirname(os.path.dirname(ff))):
            if os.path.isfile(os.path.join(d, f)) and os.path.normcase(d) != os.path.normcase(fd):
                shutil.copy2(os.path.join(d, f), os.path.join(fd, f))
                break
    ver = subprocess.run([ff, "-hide_banner", "-version"], capture_output=True, text=True).stdout.splitlines()
    with open(os.path.join(fd, "SOURCE.txt"), "w", encoding="utf-8") as fh:
        fh.write("This ffmpeg.exe is an unmodified GPL build (with libx264), used by the apps to\n"
                 "make the .wav soundtracks and the New 3DS .mvd movies.\n\n%s\n\n"
                 "Source and build scripts: https://www.gyan.dev/ffmpeg/builds/ and\n"
                 "https://ffmpeg.org/download.html (its licence is LICENSE beside this file).\n"
                 % (ver[0] if ver else "ffmpeg"))


SOURCE_README = """NFSU2-3DS PC apps -- source
===========================

Everything the three programs in the folder above are made from: the apps
(nfsu2_app.py, nfsu2_gui.py, stage.py), every packer they run, the build
script and the icons. The same files are the project's tools/ folder.

Run without the exes (any system with Python 3 and: pip install numpy pillow
lz4 pefile unicorn capstone; plus ffmpeg with libx264, vgmstream-cli and
dspenc on PATH -- build dspenc with `cc -O2 -o dspenc src/dspenc.c -lm`):

  python3 nfsu2_app.py setup --pc "<PC game folder>" --gamedir "<SD>/nfsu2"
  python3 nfsu2_app.py mods --mod "<mod folder>" --pc "<PC game folder>" --gamedir "<SD>/nfsu2"
  python3 nfsu2_app.py gui setup        (or double-click nfsu2_setup.pyw / nfsu2_mods.pyw)

Build the exes again (pip install pyinstaller, plus the above):

  python make_app_icons.py              (only to redraw app_icons/)
  python build_apps.py [--out "<folder>"] [--zip]

From here it builds into a new folder beside this one, "<this folder> (rebuilt)",
taking bin/'s dspenc, vgmstream and ffmpeg from the folder above.
"""


def copy_source(files, dst):
    """source/: the apps' and packers' .py files (tools/ as laid out), the
    build script, the icons and dspenc.c, so the folder carries what it is
    built from."""
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    extra = [os.path.join(TOOLS, f) for f in SOURCE_EXTRA]
    for f in list(files) + [f for f in extra if os.path.isfile(f)]:
        rel = os.path.relpath(f, TOOLS)
        os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
        shutil.copy2(f, os.path.join(dst, rel))
    if os.path.isdir(ICONS):
        shutil.copytree(ICONS, os.path.join(dst, "app_icons"))
    with open(os.path.join(dst, "README.txt"), "w", encoding="utf-8") as fh:
        fh.write(SOURCE_README)


README = """NFSU2-3DS PC apps
=================

Two programs that make the files the 3DS port of Need for Speed Underground 2
reads, from your own PC copy of the game. Nothing else needs installing.

  NFSU2-3DS Data Setup   Makes the game data folder (nfsu2data) from the PC
                         game's folder (the one SPEED2.EXE is in). Copy the
                         folder it makes to the 3DS's SD card. The first run
                         takes a long while; later runs only redo what changed.

  NFSU2-3DS Mod Packer   Turns a PC mod (a folder laid out like the game's,
                         e.g. <mod>/CARS/TT/TEXTURES.BIN) into a 3DS mod in
                         nfsu2data/mods/<name>.

Command line (Windows, Linux, macOS) -- the same work without a window:

  nfsu2-3ds setup --pc "<PC game folder>" --gamedir "<SD>/nfsu2data"
  nfsu2-3ds setup --pc "<PC game folder>" --gamedir "<SD>/nfsu2data" --list
  nfsu2-3ds mods --mod "<mod folder>" --pc "<PC game folder>" --gamedir "<SD>/nfsu2data"
  nfsu2-3ds gui setup        (or: gui mods) opens a window from the command line
  nfsu2-3ds setup --help     every option

bin/ holds the outside programs the packers use: dspenc (the 3DS's ADPCM),
vgmstream-cli (EA audio) and ffmpeg (sound and video), each with its licence.
Without these programs the same tools run from Python: see tools/stage.py and
MODDING.md in the NFSU2-3DS source.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--zip", action="store_true", help="also zip the folder beside it")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    files = tool_files()
    mods = sorted(set(os.path.splitext(os.path.basename(f))[0] for f in files))
    work = tempfile.mkdtemp(prefix="nfsu2_build_")
    icons = {k: os.path.join(ICONS, k + ".ico") for k in NAMES}
    icons = {k: p for k, p in icons.items() if os.path.isfile(p)}
    spec = os.path.join(work, "nfsu2_apps.spec")
    with open(spec, "w", encoding="utf-8") as fh:
        fh.write(spec_text(work, icons, mods))
    print("build_apps: %d tool file(s); PyInstaller in %s" % (len(files), work), flush=True)
    r = subprocess.run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean",
                        "--distpath", os.path.join(work, "dist"), "--workpath", os.path.join(work, "build"),
                        spec], cwd=work)
    if r.returncode != 0:
        raise SystemExit("PyInstaller failed (%d)" % r.returncode)
    built = os.path.join(work, "dist", "app")
    old = out + ".old"
    shutil.rmtree(old, ignore_errors=True)
    if os.path.isdir(out):
        # A rename fails whole when anything holds the folder, where a delete
        # would stop halfway and leave a broken app.
        try:
            os.rename(out, old)
        except OSError as e:
            raise SystemExit("cannot replace %s (%s): close the apps, and any console sitting "
                             "in that folder, then build again" % (out, e))
    shutil.copytree(built, out)
    shutil.rmtree(old, ignore_errors=True)
    # Build-time leftovers of the emulator packages: headers, static libs.
    for pkg in ("unicorn", "capstone"):
        base = os.path.join(out, "_internal", pkg)
        shutil.rmtree(os.path.join(base, "include"), ignore_errors=True)
        for dp, _d, fs in os.walk(base):
            for f in fs:
                if f.endswith((".lib", ".a")):
                    os.remove(os.path.join(dp, f))
    if os.name == "nt":
        for key in NAMES:
            no_cfg(os.path.join(out, NAMES[key] + ".exe"))
    td =os.path.join(out, "_internal", "tools")
    for f in files:
        rel = os.path.relpath(f, TOOLS)
        os.makedirs(os.path.dirname(os.path.join(td, rel)), exist_ok=True)
        shutil.copy2(f, os.path.join(td, rel))
    if os.name == "nt":
        copy_bins(out)
    copy_source(files, os.path.join(out, "source"))
    with open(os.path.join(out, "README.txt"), "w", encoding="utf-8") as fh:
        fh.write(README)
    shutil.rmtree(work, ignore_errors=True)
    size = sum(os.path.getsize(os.path.join(dp, f)) for dp, _d, fs in os.walk(out) for f in fs)
    print("build_apps: %s (%.0f MB)" % (out, size / 1e6))
    if a.zip:
        z = shutil.make_archive(out + ("-win64" if os.name == "nt" else "-" + sys.platform), "zip",
                                os.path.dirname(out), os.path.basename(out))
        print("build_apps: %s (%.0f MB)" % (z, os.path.getsize(z) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
