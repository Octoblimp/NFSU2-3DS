#!/usr/bin/env python3
"""
The NFSU2-3DS PC apps' one entry point (TASKS.md #320, #472), as Python or as
the exes tools/build_apps.py makes (no Python needed for those).

  nfsu2-3ds setup [stage.py's options]      the game data, in the console
  nfsu2-3ds mods --mod <folder> [options]   pack a PC mod, in the console
  nfsu2-3ds gui setup                       the Data Setup window
  nfsu2-3ds gui mods                        the Mod Packer window

The built folder has three programs over one shared _internal folder:
"NFSU2-3DS Data Setup" and "NFSU2-3DS Mod Packer" open their window (no
console), and nfsu2-3ds is the command line above. Without the exes, the same
from Python on any system: `python tools/nfsu2_app.py setup --pc ...`, or the
windows through tools/nfsu2_setup.pyw and tools/nfsu2_mods.pyw.

`nfsu2-3ds [-u] <script>.py args` runs one of the bundled packers the way
python.exe would; stage.py and pack_world.py start their packers like that,
so a frozen app needs no Python beside it.
"""
import multiprocessing
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WINDOWS = {"mods": "mods", "mod": "mods", "setup": "setup"}


def tools_dir():
    if getattr(sys, "frozen", False):
        return os.path.join(sys._MEIPASS, "tools")
    return HERE


def run_script(path, args):
    """A packer run as its own __main__, the way `python path args` would.
    run_module (not run_path) gives __main__ a real module name, so a packer
    whose worker processes re-import __main__ (vinyl_pack's process pool)
    works frozen too."""
    import runpy
    d = os.path.dirname(os.path.abspath(path))
    stem = os.path.splitext(os.path.basename(path))[0]
    sys.path[:0] = [d, os.path.join(tools_dir(), "re", "nis")]
    sys.argv = [path] + list(args)
    try:
        sys.stdout.reconfigure(line_buffering=True)
        sys.stderr.reconfigure(line_buffering=True)
    except Exception:
        pass
    runpy.run_module(stem, run_name="__main__", alter_sys=True)
    return 0


def gui(mode):
    sys.path.insert(0, tools_dir())
    import nfsu2_gui
    nfsu2_gui.main(mode)
    return 0


def usage(code):
    prog = os.path.splitext(os.path.basename(sys.argv[0]))[0]
    print(__doc__.replace("nfsu2-3ds ", prog + " ", 4).strip())
    return code


def main(argv):
    # The bundled dspenc / vgmstream-cli / ffmpeg ahead of anything installed,
    # for this process and every packer it starts (tools/toolbin.py).
    sys.path.insert(0, tools_dir())
    import toolbin
    os.environ["PATH"] = toolbin.path_with_bins(os.environ.get("PATH", ""))
    # Line by line even into a file or a pipe (`nfsu2-3ds setup > log.txt`).
    for f in (sys.stdout, sys.stderr):
        try:
            f.reconfigure(line_buffering=True)
        except Exception:
            pass
    exe = os.path.splitext(os.path.basename(sys.executable if getattr(sys, "frozen", False)
                                            else sys.argv[0]))[0].lower()
    # The two window programs: their name says which window.
    if getattr(sys, "frozen", False) and not argv:
        if "mod" in exe:
            return gui("mods")
        if "setup" in exe:
            return gui("setup")
    a = list(argv)
    if a and a[0] == "-u":                      # as python.exe takes it
        a = a[1:]
    if a and a[0].lower().endswith(".py"):
        return run_script(a[0], a[1:])
    if not a or a[0] in ("-h", "--help", "help"):
        return usage(0 if a else 2)
    cmd, rest = a[0].lower(), a[1:]
    if cmd == "gui":
        return gui(WINDOWS.get((rest or ["setup"])[0].lower(), "setup"))
    if cmd in ("setup", "mods", "mod"):
        sys.path.insert(0, tools_dir())
        import stage
        if cmd != "setup" and "--mod" not in rest and rest and not rest[0].startswith("-"):
            rest = ["--mod"] + rest             # `nfsu2-3ds mods <folder>`
        if cmd != "setup" and "--mod" not in rest:
            print("mods: name the mod's folder: %s mods --mod <folder> --pc <PC install>" % exe)
            return 2
        return stage.main(rest)
    print("unknown command: %s" % a[0])
    return usage(2)


if __name__ == "__main__":
    # A process-pool worker of a frozen packer starts here: let it run.
    multiprocessing.freeze_support()
    sys.exit(main(sys.argv[1:]))
