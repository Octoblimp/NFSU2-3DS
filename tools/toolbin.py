"""Where the packers' outside programs are (dspenc, vgmstream-cli, ffmpeg,
ffprobe), on any system.

Looked for, in order:
  - a `bin` folder beside the NFSU2-3DS app's exe (tools/build_apps.py puts
    them there, so a Windows PC needs nothing installed);
  - tools/bin and tools/bin/vgmstream in this folder (the repo's own copies);
  - the PATH (Linux and macOS: the system's ffmpeg and vgmstream-cli, and
    dspenc built with `cc -O2 -o tools/bin/dspenc tools/src/dspenc.c -lm`).

stage.py also puts the bin folders on the PATH it hands each packer, so a
packer that runs plain "ffmpeg" finds the bundled one.
"""
import os
import shutil
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))


def exe_name(name):
    return name + ".exe" if os.name == "nt" else name


def bin_dirs():
    dirs = []
    if getattr(sys, "frozen", False):
        app = os.path.dirname(os.path.abspath(sys.executable))
        dirs += [os.path.join(app, "bin"), os.path.join(app, "bin", "vgmstream"),
                 os.path.join(app, "bin", "ffmpeg")]
    dirs += [os.path.join(TOOLS, "bin"), os.path.join(TOOLS, "bin", "vgmstream")]
    # Run from a built app's source/ folder: the app's own bin/ above it.
    app = os.path.dirname(TOOLS)
    if os.path.isdir(os.path.join(app, "_internal")):
        dirs += [os.path.join(app, "bin"), os.path.join(app, "bin", "vgmstream"),
                 os.path.join(app, "bin", "ffmpeg")]
    return [d for d in dirs if os.path.isdir(d)]


def find(name):
    """The program's path, or its bare name (so the error names it) when it
    is nowhere."""
    for d in bin_dirs():
        p = os.path.join(d, exe_name(name))
        if os.path.isfile(p):
            return p
    return shutil.which(name) or name


def have(name):
    return find(name) != name or shutil.which(name) is not None


def path_with_bins(path=None):
    """PATH with the bin folders in front, for a child process."""
    path = os.environ.get("PATH", "") if path is None else path
    return os.pathsep.join(bin_dirs() + ([path] if path else []))

