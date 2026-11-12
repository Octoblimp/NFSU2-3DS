#!/usr/bin/env python3
"""Build the frontend sound-effect bank the port plays for menu clicks.

    PC Files/SOUND/GLOBAL/FE_COMMON_MB.abk   (EA ABK, EA-XA 4-bit ADPCM, 44.1k mono)
          --vgmstream-->  .wav  --ffmpeg-->  32 kHz mono PCM16
          --dspenc----->  .dspm (Nintendo DSPADPCM)  --> packed into FESFX.BIN

Same pipeline as tools/eatrax_build.py, and for the same reason: NDSP expands
DSPADPCM in hardware, so a menu click costs the ARM11 nothing but the waveBuf
submit.  Each clip is encoded as a SINGLE block (--block = its own frame count)
so the whole thing is one ndspWaveBuf with one ndspAdpcmData -- dspenc pads the
last block out to framesPerBlock, which would otherwise waste a few hundred KB
on eighteen clips that average four tenths of a second.

Which bank: retail's menu navigation sounds all come from EAXCommon
(EAXSound::PlayUISoundFX, GC 0x8007AC9C, routes eMenuSoundTriggers 0..0x16 to
EAXCommon and 0x17..0x5D to EAXFrontEnd).  Every trigger MenuScreen's own
message path can produce is <= 6, so FE_COMMON_MB.abk covers the lot;
FE/FE_MB.abk is the EAXFrontEnd bank and is only reached by screens that call
MakeFrontendSound themselves (the garage, the customiser, the keyboard).
Built with --abk "PC Files/SOUND/FE/FE_MB.abk" it becomes audio/FEMB.BIN
(TASKS.md #298): EAXFrontEnd::Play (0x8007DBA4) hands PlayFrontEndSample the
trigger less 0x17, so that bank's table is indexed from 0 = trigger 0x17.

Which clip a trigger plays: EAXCommon::Play hands the trigger straight to
the bank's PlayCommonSample event (Csis.PlayCommonSampleId), and the bank's
own table answers it -- nineteen entries at 0x384, one per trigger 0..0x12,
each the 1-based sample (stream) it plays: 1..12, then 0 (none) for trigger
0x0C, then 13..18.  So the clips are packed in stream order and the port maps
the trigger (main.c feSoundClip): 0..0x0B are the clip itself, 0x0C is
silent, 0x0D..0x12 are one below.  --probe prints the table as read.
(TASKS.md #259 first took clip == trigger; #291 found the table.)

Container (little-endian, matches source/audio.c SfxBankHeader):
    0x00 char magic[4] = 'FSFX'
    0x04 u32  version  = 1
    0x08 u32  count
    0x0c u32  rate              (informational; each .dspm carries its own)
    0x10 count * { u32 offset, u32 size }    offsets from file start
    ...  the .dspm images, each 32-byte aligned

Usage:
    python tools/fesfx_build.py --out "<sdmc>/nfsu2data/audio/FESFX.BIN"
    python tools/fesfx_build.py --abk "PC Files/SOUND/FE/FE_MB.abk" \
        --out "<sdmc>/nfsu2data/audio/FEMB.BIN"
    python tools/fesfx_build.py --probe [--abk ...]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ABK = os.path.join(os.environ.get("NFSU2_PC_ROOT") or os.path.join(ROOT, "PC Files"), "SOUND", "GLOBAL", "FE_COMMON_MB.abk")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolbin  # noqa: E402  (dspenc / vgmstream-cli / ffmpeg on any system)
DSPENC = toolbin.find("dspenc")
# dspenc.exe is built by devkitPro's cygwin gcc and needs cygwin1.dll at runtime.
CYGWIN_BIN = r"C:\devkitPro\msys2\usr\bin"

VGMSTREAM_CANDIDATES = [
    toolbin.find("vgmstream-cli"),
    os.path.join(ROOT, "tools", "bin", "vgmstream", "vgmstream-cli.exe"),
    os.path.join(ROOT, "tools", "bin", "vgmstream-cli.exe"),
]

FRAME_SAMPLES = 14

# What each clip is, from the FrontendSoundMap rows InitFrontendSounds builds
# (Speed.elf 0x801521CC).  Only for the build log -- nothing reads these.
TRIGGER_NAMES = {
    0: "scroll up", 1: "scroll down", 2: "scroll left", 3: "scroll right",
    4: "accept", 5: "back", 6: "select", 7: "options right", 8: "left (alt)",
    9: "right (alt)", 10: "help / popup", 0x0B: "options (MakeFrontendSound)",
    0x0F: "GPS dialog closed / SMS (MakeFrontendSound)",
    0x11: "bank picked up (FEngHud::ShowPickedUpBank)",
    0x12: "GPS route found (MinimapWrapper::UpdateGPS)",
}
# FE_MB.abk's, by table index (trigger - 0x17): the ones the port plays.
FRONTEND_NAMES = {
    0x32 - 0x17: "package installed (engine and the rest) / part installed",
    0x33 - 0x17: "turbo package installed",
    0x34 - 0x17: "nitrous package installed",
    0x35 - 0x17: "transmission package installed",
    0x36 - 0x17: "tires package installed",
}


def names_for(abk):
    return FRONTEND_NAMES if os.path.basename(abk).upper() == "FE_MB.ABK" else TRIGGER_NAMES


def trigger_table(abk):
    """The PlayCommonSample table: a u32 count, then that many
    {u32 0xFF00, u32 sample (1-based, 0 none), u32 0}.  {trigger: stream}."""
    with open(abk, "rb") as f:
        d = f.read()
    for pos in range(0, len(d) - 16, 4):
        n = struct.unpack_from("<I", d, pos)[0]
        if not 8 <= n <= 0x5D or pos + 4 + n * 12 > len(d):
            continue
        rows = [struct.unpack_from("<III", d, pos + 4 + i * 12) for i in range(n)]
        if all(a == 0xFF00 and c == 0 and b <= n for a, b, c in rows):
            return pos, {t: b for t, (_, b, _) in enumerate(rows)}
    return None, {}


def find_vgmstream():
    for c in VGMSTREAM_CANDIDATES:
        if os.path.isfile(c):
            return c
    found = shutil.which("vgmstream-cli")
    if found:
        return found
    sys.exit(
        "vgmstream-cli not found.\n"
        "Install it to tools/bin/vgmstream/ (host-side build dependency, not shipped):\n"
        "  https://github.com/vgmstream/vgmstream/releases/latest -> vgmstream-win64.zip"
    )


def run(cmd, **kw):
    env = kw.pop("env", None) or os.environ.copy()
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, **kw)
    if p.returncode != 0:
        sys.stderr.write(" ".join(str(c) for c in cmd) + "\n")
        sys.stderr.write((p.stdout or "")[-2000:] + "\n" + (p.stderr or "")[-2000:] + "\n")
        raise SystemExit("command failed (%d)" % p.returncode)
    return p


def stream_count(vgm, path):
    out = run([vgm, "-m", path]).stdout
    for line in out.splitlines():
        if line.startswith("stream count:"):
            return int(line.split(":", 1)[1])
    return 1


def stream_samples(vgm, path, index):
    out = run([vgm, "-m", "-s", str(index), path]).stdout
    for line in out.splitlines():
        if line.startswith("stream total samples:"):
            return int(line.split(":", 1)[1].split()[0])
    return 0


def wav_frames(path, rate):
    """Sample count of a PCM16 mono RIFF/WAVE, from its data chunk size."""
    with open(path, "rb") as f:
        d = f.read()
    pos = 12
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4]
        size = struct.unpack_from("<I", d, pos + 4)[0]
        if cid == b"data":
            return size // 2
        pos += 8 + size + (size & 1)
    sys.exit("no data chunk in %s" % path)


def probe(vgm, abk):
    n = stream_count(vgm, abk)
    at, table = trigger_table(abk)
    print("%s: %d stream(s), trigger table %s" % (os.path.basename(abk), n,
          "at 0x%X" % at if at is not None else "not found"))
    names = names_for(abk)
    for t in sorted(table):
        i = table[t]
        if i == 0:
            print("  trigger 0x%02X  (no sample)" % t)
            continue
        ns = stream_samples(vgm, abk, i)
        print("  trigger 0x%02X  stream %2d = clip %2d  %6d samples  %.3f s  %s"
              % (t, i, i - 1, ns, ns / 44100.0, names.get(t, "")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="output file, e.g. <sdmc>/nfsu2data/SOUND/FESFX.BIN")
    ap.add_argument("--abk", default=ABK, help="source bank (default FE_COMMON_MB.abk)")
    ap.add_argument("--rate", type=int, default=32000, help="target sample rate (default 32000)")
    ap.add_argument("--probe", action="store_true", help="list the bank's clips and exit")
    args = ap.parse_args()

    if not os.path.isfile(args.abk):
        sys.exit("missing %s" % args.abk)
    vgm = find_vgmstream()
    if args.probe:
        probe(vgm, args.abk)
        return
    if not args.out:
        sys.exit("--out is required")
    if not os.path.isfile(DSPENC):
        sys.exit("missing %s -- build it with:\n"
                 "  gcc -O2 -o tools/bin/dspenc.exe tools/src/dspenc.c -lm" % DSPENC)

    env = os.environ.copy()
    env["PATH"] = CYGWIN_BIN + os.pathsep + env.get("PATH", "")

    n = stream_count(vgm, args.abk)
    _, table = trigger_table(args.abk)
    tmpdir = tempfile.mkdtemp(prefix="fesfx_")
    images = []
    try:
        for i in range(1, n + 1):
            wav = os.path.join(tmpdir, "raw.wav")
            res = os.path.join(tmpdir, "res.wav")
            dsp = os.path.join(tmpdir, "clip%02d.dspm" % i)
            # -i: one pass of a looped stream. vgmstream otherwise renders two
            # loops and a 10 s fade -- FE_MB's index 0x32, the one sample
            # EAXFrontEnd::Play loops itself, came out at 15 s.
            run([vgm, "-i", "-o", wav, "-s", str(i), args.abk])
            run(["ffmpeg", "-y", "-v", "error", "-i", wav,
                 "-ar", str(args.rate), "-ac", "1", "-c:a", "pcm_s16le", res])
            frames = (wav_frames(res, args.rate) + FRAME_SAMPLES - 1) // FRAME_SAMPLES
            if frames < 1:
                frames = 1
            run([DSPENC, res, "-o", dsp, "--block", str(frames)], env=env)
            with open(dsp, "rb") as f:
                images.append(f.read())
            print("  clip %2d  %5d frames  %5.1f KB  %s"
                  % (i - 1, frames, len(images[-1]) / 1024.0,
                     ", ".join("trigger 0x%02X" % t for t, v in sorted(table.items())
                               if v == i)))
            for f in (wav, res, dsp):
                if os.path.exists(f):
                    os.remove(f)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    count = len(images)
    head = 0x10 + count * 8
    off = (head + 31) & ~31
    table, blob = [], bytearray()
    for img in images:
        table.append((off + len(blob), len(img)))
        blob += img
        pad = (-len(blob)) & 31
        blob += b"\0" * pad

    outdir = os.path.dirname(os.path.abspath(args.out))
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    tmp = args.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(b"FSFX")
        f.write(struct.pack("<III", 1, count, args.rate))
        for o, s in table:
            f.write(struct.pack("<II", o, s))
        f.write(b"\0" * (off - head))
        f.write(blob)
    os.replace(tmp, args.out)
    print("\nFE sound bank: %d clip(s), %.1f KB -> %s"
          % (count, os.path.getsize(args.out) / 1024.0, args.out))


if __name__ == "__main__":
    main()
