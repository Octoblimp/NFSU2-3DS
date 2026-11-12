"""ETC1 / ETC1A4 encoding for the world texture bank, via devkitPro's tex3ds.

Why (TASKS.md #427): the world bank is the one texture set big enough to
matter for GPU bandwidth. At RGB565/RGBA4444 it is 16 bits a texel; ETC1 is 4
and ETC1A4 is 8, the PICA200's two block formats and the same class as the
DXT1/DXT3 the PC ships. L4RA's bank goes from ~115 MB to ~33 MB, which is
fewer bytes read off the SD per section, less linear heap, and above all less
memory traffic per textured pixel -- the New 3DS look sweep (J29.1) put the
world's texture fetch at 20-28 ms of GPU time a frame.

The encoder is tex3ds's (rg_etc1 at -q high). Its output is tiled exactly like
tools/car_pack.py's encode_texture() -- same 8x8 tile order, image top row
first, no flip -- checked by encoding one image both ways at RGB565 and
comparing bytes. It writes a 4-byte header in front of the pixels even with
-r -z none; that is dropped here.

Mips: `-m box` makes the whole chain down to 8x8, the same stopping rule as
car_pack.mip_max_level(). The chain length is checked against what the
runtime will compute (world.c chainBytes) and truncated to the caller's
maxLevel, so a disagreement is an assertion here, never a GPU read past an
allocation on the console.

tex3ds takes ~0.2 s a texture at -q high, so results are cached on disk keyed
by a hash of the pixels, and encode_many() runs them on a thread pool (the
work is in the child processes, so threads are enough).
"""

import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor

FMT_ETC1 = 4     # opaque art
FMT_ETC1A4 = 5   # art with any alpha: ETC1 colour + 4-bit alpha, exact vs RGBA4

_CACHE_TAG = b"nfsu2-etc1-v1"
# ETC1A4 entries made before the colour fix (#438) are not reused.
_A4_TAG = b"-a4-keep-colour"


def tex3ds_path():
    # DEVKITPRO is often an MSYS path (/opt/devkitpro), which Git Bash hands
    # Python rewritten into its own tree; try it, then the usual install.
    for root in (os.environ.get("DEVKITPRO"), r"C:\devkitPro"):
        if root:
            exe = os.path.join(root, "tools", "bin", "tex3ds.exe")
            if os.path.isfile(exe):
                return exe
    exe = shutil.which("tex3ds")
    if not exe:
        raise RuntimeError("tex3ds not found: install devkitPro's 3ds-tools")
    return exe


def cache_dir():
    base = os.environ.get("LOCALAPPDATA") or tempfile.gettempdir()
    d = os.path.join(base, "nfsu2_3ds", "etc1cache")
    os.makedirs(d, exist_ok=True)
    return d


def is_etc(fmt):
    return fmt in (FMT_ETC1, FMT_ETC1A4)


def level0_bytes(w, h, fmt):
    return w * h // 2 if fmt == FMT_ETC1 else w * h


def chain_bytes(level0, maxlvl):
    total = sz = level0
    for _ in range(maxlvl):
        sz >>= 2
        total += sz
    return total


def pick_fmt(rgba):
    opaque = all(rgba[i] == 255 for i in range(3, len(rgba), 4))
    return FMT_ETC1 if opaque else FMT_ETC1A4


def _tex3ds(rgba, w, h, fmtname, maxlvl):
    """tex3ds's raw chain (header dropped) for `rgba` as `fmtname`."""
    from PIL import Image
    tmp = tempfile.mkdtemp(prefix="etc1_")
    try:
        src = os.path.join(tmp, "in.png")
        out = os.path.join(tmp, "out.bin")
        Image.frombytes("RGBA", (w, h), rgba).save(src)
        cmd = [tex3ds_path(), "-f", fmtname, "-q", "high", "-r", "-z", "none"]
        if maxlvl:
            cmd += ["-m", "box"]
        cmd += ["-o", out, src]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE)
        return open(out, "rb").read()[4:]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _etc1a4_keep_colour(rgba, w, h, maxlvl):
    """ETC1A4 whose colour is the art's own RGB under every texel.

    tex3ds writes black colour wherever alpha is 0 (an 8x8 grey at alpha 0
    encodes as colour 000000; at alpha 8 it keeps 888888). The 16-bit bank kept
    the RGB, and the world draws some of this art without its alpha -- the road
    at the career spawn, whose alpha is a mask, came out holed with black
    (TASKS #438) -- while filtering pulls the black into cut-out edges. So the
    colour half of each block comes from an encode of the art made opaque, and
    the alpha half from the real ETC1A4 encode. Both chains have the same tile
    and block order (ETC1A4 = per 4x4 block, 8 bytes of alpha then the 8-byte
    ETC1 block)."""
    opaque = bytearray(rgba)
    opaque[3::4] = bytes([255]) * (len(opaque) // 4)
    col = _tex3ds(bytes(opaque), w, h, "etc1", maxlvl)
    alp = _tex3ds(rgba, w, h, "etc1a4", maxlvl)
    nblk = chain_bytes(level0_bytes(w, h, FMT_ETC1), maxlvl) // 8
    assert len(col) >= nblk * 8 and len(alp) >= nblk * 16, (w, h, maxlvl)
    out = bytearray(nblk * 16)
    for i in range(nblk):
        out[16 * i:16 * i + 8] = alp[16 * i:16 * i + 8]
        out[16 * i + 8:16 * i + 16] = col[8 * i:8 * i + 8]
    return bytes(out)


def encode_chain(rgba, w, h, maxlvl, fmt=None, cache=True):
    """(fmt, bytes): the level 0..maxlvl chain of `rgba` (w x h RGBA8888, row 0
    the top) as ETC1, or ETC1A4 if any texel is not fully opaque."""
    assert w >= 8 and h >= 8 and not (w & (w - 1)) and not (h & (h - 1)), (w, h)
    rgba = bytes(rgba)
    if fmt is None:
        fmt = pick_fmt(rgba)
    need = chain_bytes(level0_bytes(w, h, fmt), maxlvl)
    tag = _CACHE_TAG + (_A4_TAG if fmt == FMT_ETC1A4 else b"")
    key = hashlib.sha1(tag + struct.pack("<HHBB", w, h, fmt, maxlvl)
                       + rgba).hexdigest()
    cpath = os.path.join(cache_dir(), key[:2], key + ".bin") if cache else None
    if cpath and os.path.isfile(cpath):
        data = open(cpath, "rb").read()
        if len(data) == need:
            return fmt, data
    if fmt == FMT_ETC1A4:
        data = _etc1a4_keep_colour(rgba, w, h, maxlvl)
    else:
        data = _tex3ds(rgba, w, h, "etc1", maxlvl)
    assert len(data) >= need, ("tex3ds chain too short", w, h, maxlvl,
                               len(data), need)
    data = data[:need]
    if cpath:
        os.makedirs(os.path.dirname(cpath), exist_ok=True)
        part = cpath + ".%d.tmp" % os.getpid()
        with open(part, "wb") as fh:
            fh.write(data)
        os.replace(part, cpath)
    return fmt, data


def encode_many(jobs, workers=None):
    """jobs: [(rgba, w, h, maxlvl, fmt)] -> [(fmt, bytes)] in the same order."""
    workers = workers or max(1, (os.cpu_count() or 4))
    with ThreadPoolExecutor(max_workers=workers) as ex:
        return list(ex.map(lambda j: encode_chain(*j), jobs))
