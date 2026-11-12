#!/usr/bin/env python3
"""Build the NIS audio streams the 3DS port plays: the crash camera's CollisionStream and the rain's thunder.

Pipeline (host-side; nothing here ships to the console):

    PC Files/speech/nisaudio.big  --carve----->  one SCHl stream (.asf)
                                  --vgmstream->  PCM16 WAV (32 kHz stereo)
                                  --ffmpeg---->  PCM16 WAV (32 kHz stereo)
                                  --dspenc----->  .dspm  (Nintendo DSPADPCM)

The same shape as tools/speech_build.py, whose UG2_Speech decode this bank
shares; only the record width differs (below).


THE NIS BANK
------------
`PC Files/speech/nisaudio.*` is the second bank SpeechManager loads: the
NIS_AudioParser's (EAXSound +0x28), which plays the in-game movies' streams and
the one stream the world asks for, EAXCollision::QueueCollisionStream
(Speed.elf 0x80080264):

    QueStream(nisParser, 5, bStringHash("CollisionStream"), 4,
              callback 0x80080230 (PlayStream(module +8, 5)), false)

NIS_AudioParser::QueStream (0x8008B0A0) maps the "CollisionStream" hash to the
Csis SoundFX_Select parameters {2, 8} -- one-hot flags, so bit indices 1 and 3
in the index, as UG2_Speech.idx stores them.

nisaudio.idx is UG2_Speech.idx's layout with two parameters per record, not
three (the header's +4 byte says 2): four groups, each a 12-byte header and
then `count` four-byte records -- a big-endian u16 sector offset from the
group's base, then p0 and p1. Group 3 (id 0x01000003, base 0x42D7100) holds
23 sounds, and parameters (1, 3) select records 3..8: six alternates, each an
EA-XA v2 stereo 32 kHz stream of 2.35 to 4.71 s. The SPCH engine picks one at
run time; the port picks with bRandom (main.c / colsfx.c say so).

Output: colstream0.dspm .. colstream5.dspm, records 3..8 in order, stereo at
the source's 32 kHz -- about 860 KB together.

The rain's thunder (TASKS.md #315): CARSFX_Rain::QueueWeatherStream
(0x800A1948) asks QueStream(4, bStringHash("THUNDERSTRM"), variant), variant
1 under intensity 0.5 and 2 over; QueStream maps it to SoundFX_Select
{4, variant}. Group 3's records 14..22 carry (2, 0) -- variant 1, nine
alternates -- and nothing carries (2, 1), so variant 2 finds no sound.
Output: thunder0.dspm .. thunder8.dspm.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEECH = os.path.join(os.environ.get("NFSU2_PC_ROOT") or os.path.join(ROOT, "PC Files"), "speech")
BIG = os.path.join(SPEECH, "nisaudio.big")
IDX = os.path.join(SPEECH, "nisaudio.idx")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolbin  # noqa: E402  (dspenc / vgmstream-cli / ffmpeg on any system)
DSPENC = toolbin.find("dspenc")
CYGWIN_BIN = os.path.join(ROOT, "tools", "bin")
VGMSTREAM_CANDIDATES = [
    toolbin.find("vgmstream-cli"),
    os.path.join(ROOT, "tools", "bin", "vgmstream", "vgmstream-cli.exe"),
    os.path.join(ROOT, "tools", "bin", "vgmstream-cli.exe"),
]

# The streams the port asks for: (file stem, group, p0, p1). A stem ending in
# '#' takes every alternate, numbered from 0.
STREAMS = [
    ("colstream#", 3, 1, 3),   # CollisionStream, SoundFX_Select {2, 8}
    ("thunder#", 3, 2, 0),     # THUNDERSTRM variant 1, SoundFX_Select {4, 1} (#315)
]


def load_index():
    """Returns a list of groups, each a dict with its records in .big order."""
    idx = open(IDX, "rb").read()
    big_size = os.path.getsize(BIG)
    ngroups = struct.unpack_from("<I", idx, 4)[0]
    groups = []
    for i in range(ngroups):
        gid, length, off, base = struct.unpack_from("<IIII", idx, 0x58 + i * 16)
        nparam = idx[off + 4]
        groups.append(dict(id=gid, base=base, sub=off, count=idx[off + 5],
                           width=2 + nparam, records=[]))
    for i, g in enumerate(groups):
        limit = groups[i + 1]["base"] if i + 1 < ngroups else big_size
        raw = []
        for r in range(g["count"]):
            o = g["sub"] + 12 + r * g["width"]
            sector = struct.unpack_from(">H", idx, o)[0]
            raw.append((g["base"] + sector * 0x100, tuple(idx[o + 2:o + g["width"]])))
        for r, (start, params) in enumerate(raw):
            end = raw[r + 1][0] if r + 1 < len(raw) else limit
            g["records"].append(dict(index=r, offset=start, length=end - start, params=params))
    return groups


def find_vgmstream():
    for c in VGMSTREAM_CANDIDATES:
        if os.path.isfile(c):
            return c
    found = shutil.which("vgmstream-cli")
    if found:
        return found
    sys.exit("vgmstream-cli not found -- see tools/speech_build.py")


def run(cmd, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, env=env or os.environ.copy())
    if p.returncode != 0:
        sys.stderr.write(" ".join(str(c) for c in cmd) + "\n")
        sys.stderr.write((p.stdout or "")[-2000:] + "\n" + (p.stderr or "")[-2000:] + "\n")
        raise SystemExit("command failed (%d)" % p.returncode)


def carve(start, length, dst):
    with open(BIG, "rb") as f, open(dst, "wb") as out:
        f.seek(start)
        out.write(f.read(length))


def cmd_list(groups):
    for i, g in enumerate(groups):
        print("group %d  id %08x  base %08x  %d sounds, %d params"
              % (i, g["id"], g["base"], g["count"], g["width"] - 2))
        for rec in g["records"]:
            print("   %3d  @%08x %8d B  p=%s" % (rec["index"], rec["offset"], rec["length"],
                                              rec["params"]))


def cmd_build(groups, args):
    os.makedirs(args.out, exist_ok=True)
    if not os.path.isfile(DSPENC):
        sys.exit("missing %s -- see tools/speech_build.py" % DSPENC)
    vgm = find_vgmstream()
    env = os.environ.copy()
    env["PATH"] = CYGWIN_BIN + os.pathsep + env.get("PATH", "")
    tmpdir = tempfile.mkdtemp(prefix="nisaudio_")
    total = 0
    try:
        for stem, gi, p0, p1 in STREAMS:
            hits = [r for r in groups[gi]["records"] if r["params"] == (p0, p1)]
            if not hits:
                sys.exit("%s: no sound with parameters (%d, %d) in group %d" % (stem, p0, p1, gi))
            for n, rec in enumerate(hits):
                name = stem.replace("#", str(n)) if stem.endswith("#") else stem
                dst = os.path.join(args.out, name + ".dspm")
                if os.path.exists(dst) and not args.force:
                    total += os.path.getsize(dst)
                    print("skip (exists) %s" % name)
                    continue
                cut = os.path.join(tmpdir, "cut.asf")
                wav = os.path.join(tmpdir, "raw.wav")
                res = os.path.join(tmpdir, "res.wav")
                print("%-12s record %2d  @%08x %7d B" % (name, rec["index"], rec["offset"],
                                                        rec["length"]))
                carve(rec["offset"], rec["length"], cut)
                run([vgm, "-o", wav, cut])
                run(["ffmpeg", "-y", "-v", "error", "-i", wav, "-ar", str(args.rate),
                     "-ac", "2", "-c:a", "pcm_s16le", res])
                run([DSPENC, res, "-o", dst, "--block", str(args.block)], env=env)
                total += os.path.getsize(dst)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    print("\nNIS streams: %.0f KB in %s" % (total / 1024.0, args.out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--list", action="store_true", help="dump the clump index and stop")
    ap.add_argument("--out", help="output directory (the card's speech directory)")
    ap.add_argument("--rate", type=int, default=32000,
                    help="output sample rate; the source is 32000 stereo (default: %(default)s)")
    ap.add_argument("--block", type=int, default=512,
                    help="dspenc block size in frames: EA Trax's, the stereo streams' (default: %(default)s)")
    ap.add_argument("--force", action="store_true", help="rebuild files that already exist")
    args = ap.parse_args()
    groups = load_index()
    if args.list:
        cmd_list(groups)
    elif not args.out:
        sys.exit("--out is required")
    else:
        cmd_build(groups, args)


if __name__ == "__main__":
    main()
