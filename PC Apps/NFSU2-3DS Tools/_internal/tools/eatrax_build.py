#!/usr/bin/env python3
"""Build the NFSU2 EA Trax music set for the 3DS port.

Pipeline (all host-side; nothing here ships to the console):

    PC Files/pfdata/MusicSFx.mus  --carve----->  one SCHl stream (.asf)
                                  --vgmstream->  PCM16 WAV (32 kHz stereo)
                                  --ffmpeg---->  PCM16 WAV (32 kHz stereo)
                                  --dspenc----->  .dspm  (Nintendo DSPADPCM)

The .dspm files are streamed straight into NDSP hardware channels on the 3DS
(NDSP_FORMAT_ADPCM), so the ARM11 never decodes or mixes a sample.  DSPADPCM is
4 bits/sample, a 4x saving over PCM16 on both SD footprint and read bandwidth.

Track metadata is the real table reverse-engineered out of PC Files/SPEED2.EXE:

  * VA 0x7ECD80 -- 27 x 16-byte records, four char* fields:
        +0 title   +4 artist   +8 album   +12 bank ("FE" frontend / "IG" in-game)
  * The sequential-play routine at 0x0046B740 bounds the rotation at 0x1B = 27
    (`cmp edi, 0x1b`) and indexes this table via `mov eax, [edx + 0x7ecd80]`
    with edx = index << 4, pushing +0 as the "Title = %s" argument.
  * 0x4AC9F0 pushes +0/+4/+8 into the now-playing display call at 0x4AC950,
    which is why the popup carries title, artist and album.
  * 0x00515B46..0x00515BBD builds the playlist array at [this+0x2D8] (8 bytes
    per entry: u32 index, u8 bank class) by strcmp'ing the +12 field against
    "FE" (0x789BD8) -> 1, "IG" (0x789AF4) -> 2, "AL" (0x79B0F4) -> 3.  This
    build ships 6 FE and 21 IG records; no record uses AL.

STREAM MAPPING
--------------
MusicSFx.mus is a flat concatenation of 73 EA SCHl streams (walk the file for
the 'SCHl' magic; stream N occupies [offs[N], offs[N+1]) and each such range is
a valid standalone EA stream file that vgmstream reads).  They fall into two
codec groups:

    raw  0..42  EALayer3 (0x80=2, 0xA0=23), 32 kHz stereo   -- 43 streams
    raw 43..72  EA-XA 4-bit ADPCM (0x80=1), 44.1 kHz stereo -- 30 streams

MusicSFx.mpf indexes only 43 of the 73 -- 16 of the EALayer3 group (raw 0,
11-16, 23-28, 40-42) plus all 30 EA-XA.  Those are the adaptive/interactive
cues the MPF sequencer drives, and the durations say so plainly: every
MPF-indexed EALayer3 stream is 1:04-2:27 and clusters near 2:00, and the EA-XA
group runs to 9:33 and contains exact duplicate pairs (raw 53/55 and 54/56 have
byte-for-byte equal lengths) -- intensity layers, not songs.

The 27 EALayer3 streams the MPF does NOT index are the EA Trax songs.  That is
why a record carries no stream id: the position in this list is the selector,
because EA Trax plays standalone rather than through the MPF sequencer.

They are NOT laid out in record order, and the order is not derivable: it is an
arbitrary permutation.  Nothing in the shipped data spells it out -- the record
table carries no stream id, the SCHl headers are a bare 40 bytes with no name
(PT, then 0x80/0x82/0x84/0x85/0xA0, 0xFF), MusicSFx.mpf holds only beat/section
data for the adaptive cues, and scanning the whole executable finds no 27-entry
table whose values are this stream set.  Two successive guesses at a structure
(file order, then FE/IG run grouping) each shipped the wrong songs.

The mapping below was therefore recovered from the audio itself, matching each
of the 27 game streams against the corresponding retail track:

  * decode both sides to mono 11.025 kHz; take a 24-band log-spaced
    log-magnitude spectrogram (1024-pt FFT, 512 hop); z-normalise per band
  * FFT cross-correlate three 30 s windows of the game edit (at 18/42/68% in,
    so intros, outros and loop seams cannot dominate) against the full retail
    track, scoring by the best lag
  * take the argmax per stream

Every stream argmax landed on a distinct record -- a complete bijection, no
conflicts and nothing unclaimed -- which makes it provably the maximum-sum
assignment rather than a greedy approximation.  It independently reproduces the
two tracks that had been identified by ear (raw 1 = "Switch/Twitch", raw 17 =
"Black Betty"), and agrees with every stream whose duration matches a retail
track to within 0.15 s.  Runner-up margins ranged from +0.05 to +0.60.

The IG entries frequently run longer than their album versions; those are the
looped/extended game edits, which is exactly what in-race music needs.

Usage:
    python tools/eatrax_build.py --out <dir> [--tracks 0,1,2] [--rate 32000]
    python tools/eatrax_build.py --manifest-only --out <dir>
    python tools/eatrax_build.py --probe            # list the 73 raw streams
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MUS = os.path.join(os.environ.get("NFSU2_PC_ROOT") or os.path.join(ROOT, "PC Files"), "pfdata", "MusicSFx.mus")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolbin  # noqa: E402  (dspenc / vgmstream-cli / ffmpeg on any system)
DSPENC = toolbin.find("dspenc")
# dspenc.exe is built by devkitPro's cygwin gcc and needs cygwin1.dll at runtime.
CYGWIN_BIN = r"C:\devkitPro\msys2\usr\bin"

# vgmstream is a host-side build dependency (like ffmpeg); it is never shipped.
VGMSTREAM_CANDIDATES = [
    toolbin.find("vgmstream-cli"),
    os.path.join(ROOT, "tools", "bin", "vgmstream", "vgmstream-cli.exe"),
    os.path.join(ROOT, "tools", "bin", "vgmstream-cli.exe"),
]

# Indexed by record number, so EATRAX_STREAMS[record] == raw SCHl stream.
# Recovered by spectral cross-correlation against the retail tracks -- see the
# STREAM MAPPING section above.  This is an arbitrary permutation; do not try to
# "simplify" it into ranges, and do not reorder it without redoing the match.
EATRAX_STREAMS = [
    38,   # rec  0  FE  Riders on the Storm (Fredwreck Remix)   6:13.6
     3,   # rec  1  FE  I Need Speed                            3:27.5
    34,   # rec  2  FE  I Do                                    3:57.1
     7,   # rec  3  FE  That'z My Name                          3:29.6
    39,   # rec  4  FE  LAX                                     3:18.2
    22,   # rec  5  FE  Lean Back                               4:13.4
     1,   # rec  6  IG  Switch/Twitch                           4:13.7
    18,   # rec  7  IG  Rush Hour                               4:04.4
    19,   # rec  8  IG  Rocket Ride (Soulwax Remix)             3:02.9
    37,   # rec  9  IG  Hard EBM                                3:40.1
    32,   # rec 10  IG  Mind Killer (Jagz Kooner Remix)         5:45.0
    33,   # rec 11  IG  Nothing But You (Cirrus Remix)          4:01.8
    29,   # rec 12  IG  E-Ville                                 4:30.2
     8,   # rec 13  IG  The Death & Resurrection Show           5:01.2
     4,   # rec 14  IG  Give it All                             2:48.8
     5,   # rec 15  IG  Scavenger                               2:37.7
     2,   # rec 16  IG  Notice of Eviction                      2:16.0
    30,   # rec 17  IG  No W                                    2:51.0
    36,   # rec 18  IG  In My Head                              4:01.3
    35,   # rec 19  IG  Determined                              2:37.8
     6,   # rec 20  IG  I Am Weightless                         2:25.6
     9,   # rec 21  IG  Crashing Foreign Cars                   2:30.8
    10,   # rec 22  IG  Back on a Mission                       3:34.7
    17,   # rec 23  IG  Black Betty                             3:26.0
    21,   # rec 24  IG  Nobody                                  3:16.1
    20,   # rec 25  IG  Skeptic                                 2:23.0
    31,   # rec 26  IG  The Celebration Song                    3:42.3
]

# Verbatim from SPEED2.EXE VA 0x7ECD80 (title, artist, album, bank).
# Titles keep the quote marks the game itself stores and displays.
TRACKS = [
    ('"Riders on the Storm (Fredwreck Remix)"', "Snoop Dogg feat. The Doors", "Need for Speed Underground 2 Exclusive", "FE"),
    ('"I Need Speed"',                          "Capone",                     "Need for Speed Underground 2 Exclusive", "FE"),
    ('"I Do"',                                  "Chingy",                     "PowerBallin'",                          "FE"),
    ('"That\'z My Name"',                       "Sly Boogy",                  "Mistaken Identity",                     "FE"),
    ('"LAX"',                                   "Xzibit",                     "Weapons of Mass Destruction",           "FE"),
    ('"Lean Back"',                             "Terror Squad",               "True Story",                            "FE"),
    ('"Switch/Twitch"',                         "Fluke",                      "Puppy",                                 "IG"),
    ('"Rush Hour"',                             "Christopher Lawrence",       "All or Nothing",                        "IG"),
    ('"Rocket Ride (Soulwax Remix)"',           "Felix Da Housecat",          "Devin Dazzle & the Neon Fever",         "IG"),
    ('"Hard EBM"',                              "Sin",                        "Errare Digital EST",                    "IG"),
    ('"Mind Killer (Jagz Kooner Remix)"',       "FREELAND",                   "Now & Them",                            "IG"),
    ('"Nothing But You (Cirrus Remix)"',        "Paul Van Dyk",               "Reflections",                           "IG"),
    ('"E-Ville"',                               "Sonic Animation",            "Reality By Deception",                  "IG"),
    ('"The Death & Resurrection Show"',         "Killing Joke",               "Killing Joke",                          "IG"),
    ('"Give it All"',                           "Rise Against",               "Siren Song of the Counter Culture",     "IG"),
    ('"Scavenger"',                             "Killradio",                  "Raised on Whipped Cream",               "IG"),
    ('"Notice of Eviction"',                    "The Bronx",                  "The Bronx",                             "IG"),
    ('"No W"',                                  "Ministry",                   "Houses of the Mol\u00e9",               "IG"),
    ('"In My Head"',                            "Queens of the Stone Age",    "Lullabies to Paralyze",                 "IG"),
    ('"Determined"',                            "Mudvayne",                   "",                                      "IG"),
    ('"I Am Weightless"',                       "Septembre",                  "Rule 3: Conceal Your Intentions",       "IG"),
    ('"Crashing Foreign Cars"',                 "Helmet",                     "Size Matters",                          "IG"),
    ('"Back on a Mission"',                     "Cirrus",                     "Back on a Mission",                     "IG"),
    ('"Black Betty"',                           "SpiderBait",                 "Tonight Alright",                       "IG"),
    ('"Nobody"',                                "Skindred",                   "Babylon",                               "IG"),
    ('"Skeptic"',                               "Snapcase",                   "Bright Flashes",                        "IG"),
    ('"The Celebration Song"',                  "Unwritten Law",              "The Way",                               "IG"),
]

assert len(TRACKS) == 27, "track table must match the 0x1b bound in the game"
assert len(EATRAX_STREAMS) == 27, "one raw SCHl stream per track record"


def schl_offsets(path):
    """Byte offset of every SCHl stream header in the .mus, in file order."""
    with open(path, "rb") as f:
        data = f.read()
    offs, i = [], data.find(b"SCHl")
    while i != -1:
        offs.append(i)
        i = data.find(b"SCHl", i + 4)
    return offs, len(data)


def schl_info(path, start):
    """Parse the SCHl patch stream: (codec1, codec2, channels, rate, samples)."""
    with open(path, "rb") as f:
        f.seek(start)
        d = f.read(0x400)
    q = 12  # 'SCHl' + size + platform tag
    codec1 = codec2 = chans = rate = nsamp = None
    while q < len(d):
        b = d[q]; q += 1
        if b == 0xFF:
            break
        if b in (0x00, 0xFC, 0xFD, 0xFE):
            continue
        ln = d[q]; q += 1
        val = int.from_bytes(d[q:q + ln], "big"); q += ln
        if   b == 0x80: codec1 = val
        elif b == 0x82: chans = val
        elif b == 0x84: rate = val
        elif b == 0x85: nsamp = val
        elif b == 0xA0: codec2 = val
    return codec1, codec2, chans, rate, nsamp


def carve(path, start, end, dst):
    """Copy [start,end) out to dst. A lone SCHl..SCEl range is a valid EA stream."""
    with open(path, "rb") as src, open(dst, "wb") as out:
        src.seek(start)
        left = end - start
        while left:
            chunk = src.read(min(1 << 20, left))
            if not chunk:
                break
            out.write(chunk)
            left -= len(chunk)


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


def write_manifest(outdir, rate):
    """Line format: index|bank|file|title|artist|album  (UTF-8, '|' never occurs
    in the source strings). Parsed by eatraxLoadManifest() in source/eatrax.c."""
    path = os.path.join(outdir, "tracks.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# NFSU2 EA Trax manifest\n")
        f.write("# generated by tools/eatrax_build.py from SPEED2.EXE VA 0x7ECD80\n")
        f.write("# fields: index|bank|file|title|artist|album\n")
        f.write("count %d\n" % len(TRACKS))
        f.write("rate %d\n" % rate)
        for i, (title, artist, album, bank) in enumerate(TRACKS):
            f.write("%d|%s|track%02d.dspm|%s|%s|%s\n" % (i, bank, i, title, artist, album))
    print("manifest -> %s (%d tracks)" % (path, len(TRACKS)))


def probe():
    offs, size = schl_offsets(MUS)
    print("%s: %d SCHl streams, %d bytes" % (os.path.basename(MUS), len(offs), size))
    rev = {s: i for i, s in enumerate(EATRAX_STREAMS)}
    for n, o in enumerate(offs):
        end = offs[n + 1] if n + 1 < len(offs) else size
        c1, c2, ch, rate, ns = schl_info(MUS, o)
        dur = (ns / rate) if (ns and rate) else 0.0
        codec = "EALayer3" if c2 == 23 else ("EA-XA" if c1 == 1 else "?")
        tag = ("  <- track %2d" % rev[n]) if n in rev else ""
        print("raw %2d  %-8s %5s Hz ch%s  %2d:%05.2f  %9d bytes%s"
              % (n, codec, rate, ch, int(dur // 60), dur % 60, end - o, tag))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="output dir, e.g. <sdmc>/nfsu2data/eatrax")
    ap.add_argument("--tracks", default="", help="comma-separated track indices (default: all 27)")
    ap.add_argument("--rate", type=int, default=32000, help="target sample rate (default 32000)")
    ap.add_argument("--block", type=int, default=512, help="ADPCM frames per stream block")
    ap.add_argument("--manifest-only", action="store_true")
    ap.add_argument("--probe", action="store_true", help="list the raw SCHl streams and exit")
    ap.add_argument("--force", action="store_true", help="re-encode tracks that already exist")
    args = ap.parse_args()

    if not os.path.isfile(MUS):
        sys.exit("missing %s" % MUS)
    if args.probe:
        probe()
        return
    if not args.out:
        sys.exit("--out is required")

    os.makedirs(args.out, exist_ok=True)
    write_manifest(args.out, args.rate)
    if args.manifest_only:
        return

    if not os.path.isfile(DSPENC):
        sys.exit("missing %s -- build it with:\n"
                 "  gcc -O2 -o tools/bin/dspenc.exe tools/src/dspenc.c -lm" % DSPENC)

    vgm = find_vgmstream()
    env = os.environ.copy()
    env["PATH"] = CYGWIN_BIN + os.pathsep + env.get("PATH", "")

    offs, size = schl_offsets(MUS)
    if len(offs) != 73:
        print("warning: expected 73 SCHl streams, found %d" % len(offs), file=sys.stderr)

    want = (sorted(int(x) for x in args.tracks.split(",") if x.strip() != "")
            if args.tracks else list(range(len(TRACKS))))

    tmpdir = tempfile.mkdtemp(prefix="eatrax_")
    total = 0
    try:
        for i in want:
            title, artist, album, bank = TRACKS[i]
            dst = os.path.join(args.out, "track%02d.dspm" % i)
            if os.path.exists(dst) and not args.force:
                total += os.path.getsize(dst)
                print("[%2d/%2d] skip (exists) %s" % (i + 1, len(TRACKS), os.path.basename(dst)))
                continue

            raw_idx = EATRAX_STREAMS[i]
            start = offs[raw_idx]
            end = offs[raw_idx + 1] if raw_idx + 1 < len(offs) else size
            c1, c2, ch, rate, ns = schl_info(MUS, start)
            dur = (ns / rate) if (ns and rate) else 0.0

            # .asf so vgmstream picks the Electronic Arts SCHl reader.
            cut = os.path.join(tmpdir, "cut.asf")
            wav = os.path.join(tmpdir, "raw.wav")
            res = os.path.join(tmpdir, "res.wav")

            print("[%2d/%2d] %s - %s  (raw %d, %d:%05.2f)"
                  % (i + 1, len(TRACKS), artist, title, raw_idx, int(dur // 60), dur % 60))
            carve(MUS, start, end, cut)
            run([vgm, "-o", wav, cut])
            run(["ffmpeg", "-y", "-v", "error", "-i", wav,
                 "-ar", str(args.rate), "-ac", "2", "-c:a", "pcm_s16le", res])
            run([DSPENC, res, "-o", dst, "--block", str(args.block)], env=env)

            total += os.path.getsize(dst)
            for f in (cut, wav, res):
                if os.path.exists(f):
                    os.remove(f)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\nEA Trax set: %d tracks, %.1f MB in %s" % (len(want), total / 1048576.0, args.out))


if __name__ == "__main__":
    main()
