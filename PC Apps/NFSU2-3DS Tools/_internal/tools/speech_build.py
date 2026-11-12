#!/usr/bin/env python3
"""Build the NFSU2 character-speech set for the 3DS port.

Pipeline (host-side; nothing here ships to the console):

    PC Files/speech/UG2_Speech.big  --carve----->  one SCHl stream (.asf)
                                    --vgmstream->  PCM16 WAV (22.05 kHz mono)
                                    --ffmpeg---->  PCM16 WAV (22.05 kHz mono)
                                    --dspenc----->  .dspm  (Nintendo DSPADPCM)

Same shape as tools/eatrax_build.py, and for the same reason: the .dspm goes
straight into an NDSP hardware channel (NDSP_FORMAT_ADPCM), so the ARM11 never
decodes a speech sample.  The lines are mono 22.05 kHz, so a 10 s line is about
110 KB -- the whole Stage 0 prologue is under 400 KB.


THE SPEECH BANK
---------------
`PC Files/speech/` holds four files, and all four were decoded for this port:

  UG2_Speech.big  54.2 MB, 857 EA `SCHl` streams, each starting on a 0x100
                  byte boundary.  Every one is EA MicroTalk, 22050 Hz mono, and
                  every one is a valid standalone EA stream file once carved out
                  at [offset, next offset), which is what vgmstream reads.
  UG2_Speech.idx  the clump index -- SpeechManager::LoadSpeechBank's
                  CLUMP_IDX_FILE.  Decoded below; this is the whole mapping.
  UG2_Speech.evt  SPCH's event dat (`VoxDat`), decoded below.  It carries the
                  per-category validation filter, not the sample choice.
  UG2_Speech.csi  268 bytes, magic 'MOIR', the Csis schema: the ten category
                  names and one u32 each.  The same table is repeated in the
                  tail of the .evt's last record, which is how the two were
                  confirmed against each other.

The structures were read out of `Gamecube Files/Speed.elf`, which ships full
C++ symbols for the EA sound library -- iSPCH_SearchEventDat, iSPCH_*, Csis::*
and UG2_SpeechParser::Play* are all named there.


UG2_Speech.idx -- the clump index
---------------------------------
    0x00  u32 version (1)
    0x04  u32 group count (14)
    0x08  0x48 bytes of zero
    0x54  u32 group count again
    0x58  group[14], 16 bytes each:
              +0 u32 group id (0x01000000 + index)
              +4 u32 sub-table length, in bytes
              +8 u32 sub-table offset, from the start of the .idx
             +12 u32 the group's base offset in the .big

Groups partition the .big exactly: group N owns every SCHl from its own base up
to group N+1's, and the fourteen counts below sum to 857 with nothing left over.

A sub-table is a 12-byte header then `count` records of 2 + nparm bytes:

    header  +0 u16 bank id  +2 u16 sub-bank (0xFFFF for none)
            +4 u8 nparm     (low 7 bits; the parameter count)
            +5 u8 record count
            +6 u8 ?         +7 u8 sector shift: a sector is 0x100 << this
            +8 u32 the group's size in sectors
    record  +0 u16 BE       the sound's offset from the group base, in
                            sectors -- big-endian, in an otherwise
                            little-endian file
            +2 u8[nparm]    the Csis parameters for this sound, as BIT INDICES
                            rather than values (see below)

(Group 7 alone has a shift of 1, 0x200-byte sectors: it is 8.7 MB, past what
a u16 of 0x100s reaches.  Until 2026-11-05 every record was read as five bytes
in 0x100-byte sectors. That is right for the
three-parameter groups -- 0, 6, 7 and 9-13, the Stage 0 lines among them -- and
wrong for groups 1-5, which have four, and group 8, which has two.)

Parameter 0 is the DX_Mode the parser passes first (UG2_SpeechParser::Play*,
SpeedPS.elf 0x8008CB30..): 1 career, 2 frontend, 4 timed, 8 Outrun, 0x10
intro-event, 0x20 intro-area.  The fourteen groups, as bit indices:

    0     Career           (0, CR_Event, CharId)
    1     Career_Dvd       (0, 10, Dvd_Name, Status)      CR_Event 0x400
    2     Career_Magazine  (0, 9, Mag_Name, Status)       CR_Event 0x200
    3     FE_Dynoshop      (1, 5, Dynoshop, CharId)       FE_Mode 0x20
    4     FE_OptionScreen? (1, 4, 7, 5)                   one sound
    5     FE_Screen        (1, 0, FE_Screen, CharId)      FE_Mode 1
    6     Intro_Area       (5, Locations, CharId)
    7     Intro_Events     (4, Locations, CharId)
    8     Time             (2, TimeProgress)
    9-13  Outrun           (3, OR_Parms, CharId), one sub-bank per voice:
                           CharId bits 1, 2, 0, 3 and 9 (sub-banks 1-5)

The record count at +5 was checked against the .big for all fourteen groups:
47, 31, 38, 11, 1, 52, 6, 69, 3, 135, 111, 118, 123, 112 -- each exactly the
number of SCHl headers in that group's range, 857 together.


WHY THE PARAMETERS ARE BIT INDICES
----------------------------------
Csis passes its enums as one-hot bit flags.  CareerSpeech::Stage0Intro
(Speed.elf 0x8018A8B4) builds SpeechCmdCareer(eventId=0x8000, charId=0x20), and
its three siblings pass 0x800, 0x2 and 0x10000000.  The .idx stores a single
byte per parameter, so it cannot be holding 0x10000000 -- it holds 28, the bit
number.

The .evt confirms the widths.  Its event records end in a filter of
`[u8 parm][ff ff 00][u32 mask]` pairs, and the Career event's masks are
0x1FFFFFFF for the event parameter and 0x000003FF for the character -- 29 and
10 bits, so bit indices 0..28 and 0..9.  Group 0's records use exactly those
ranges and no more: param 1 spans 0..28 and param 2 spans 0..9.

Group 0 is the Career category, and the four Stage 0 lines resolve there to one
record each with no alternates -- which is what a scripted prologue has to be.
Events that do have alternates (7, 6, 4, 8 ...) carry five to nine records
apiece, and those are the lines the engine picks between at run time.


WHAT GOES ON THE CARD
---------------------
Every record, as g<group>_<record>.dspm (g07_012.dspm is group 7's thirteenth
sound), and UG2_Speech.idx itself.  The console parses the idx with this same
decode (speech.c) and looks a call's parameters up there, so the choice between
alternates happens at run time as in retail, against retail's own table.
"""
import argparse
import concurrent.futures
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEECH = os.path.join(os.environ.get("NFSU2_PC_ROOT") or os.path.join(ROOT, "PC Files"), "speech")
BIG = os.path.join(SPEECH, "UG2_Speech.big")
IDX = os.path.join(SPEECH, "UG2_Speech.idx")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolbin  # noqa: E402  (dspenc / vgmstream-cli / ffmpeg on any system)
DSPENC = toolbin.find("dspenc")
CYGWIN_BIN = os.path.join(ROOT, "tools", "bin")
VGMSTREAM_CANDIDATES = [
    toolbin.find("vgmstream-cli"),
    os.path.join(ROOT, "tools", "bin", "vgmstream", "vgmstream-cli.exe"),
    os.path.join(ROOT, "tools", "bin", "vgmstream-cli.exe"),
]

# The DX_Mode bit each category's records carry as parameter 0 (see above).
DX_BITS = {0: "Career", 1: "FE", 2: "Time", 3: "Outrun", 4: "Intro_Events",
           5: "Intro_Area"}


def record_name(group, record):
    """The file a record is written to; speech.c builds the same name."""
    return "g%02d_%03d" % (group, record)


def load_index():
    """Returns (groups, sounds).

    groups is a list of dicts, one per group, each with its records in .big
    order; sounds maps (group, *params) -> [record, ...]."""
    idx = open(IDX, "rb").read()
    big_size = os.path.getsize(BIG)
    ngroups = struct.unpack_from("<I", idx, 4)[0]
    groups = []
    for i in range(ngroups):
        gid, length, off, base = struct.unpack_from("<IIII", idx, 0x58 + i * 16)
        bank, subbank = struct.unpack_from("<HH", idx, off)
        groups.append(dict(id=gid, index=i, base=base, sub=off, sublen=length,
                           bank=bank, subbank=subbank, nparm=idx[off + 4] & 0x7F,
                           count=idx[off + 5], sector=0x100 << idx[off + 7],
                           records=[]))
    sounds = {}
    big = open(BIG, "rb")
    for i, g in enumerate(groups):
        # A group's last sound ends where the next group begins.
        limit = groups[i + 1]["base"] if i + 1 < ngroups else big_size
        size = 2 + g["nparm"]
        raw = []
        for r in range(g["count"]):
            o = g["sub"] + 12 + r * size
            sector = struct.unpack_from(">H", idx, o)[0]
            raw.append((g["base"] + sector * g["sector"], tuple(idx[o + 2:o + size])))
        for r, (start, params) in enumerate(raw):
            end = raw[r + 1][0] if r + 1 < len(raw) else limit
            assert end > start, (i, r)
            big.seek(start)
            assert big.read(4) == b"SCHl", ("not a stream", i, r, hex(start))
            rec = dict(group=i, record=r, offset=start, length=end - start,
                       params=params)
            g["records"].append(rec)
            sounds.setdefault((i,) + params, []).append(rec)
    return groups, sounds


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


def carve(src, start, length, dst):
    with open(src, "rb") as f, open(dst, "wb") as out:
        f.seek(start)
        left = length
        while left > 0:
            chunk = f.read(min(1 << 20, left))
            if not chunk:
                break
            out.write(chunk)
            left -= len(chunk)


def cmd_list(args):
    groups, sounds = load_index()
    total = sum(g["count"] for g in groups)
    print("UG2_Speech.idx: %d groups, %d sounds" % (len(groups), total))
    for g in groups:
        dx = DX_BITS.get(g["records"][0]["params"][0], "?") if g["records"] else "?"
        print("\ngroup %-2d  bank %02x sub %-5d  %d parms  base %08x  %3d sounds  (%s)"
              % (g["index"], g["bank"], g["subbank"], g["nparm"], g["base"],
                 g["count"], dx))
        if args.group is None or g["index"] != args.group:
            continue
        for rec in g["records"]:
            print("   %3d  @%08x %7d B  p=%s"
                  % (rec["record"], rec["offset"], rec["length"], rec["params"]))


def build_one(rec, out, vgm, env, rate, block, force):
    """Carves, decodes and encodes one record. Returns (bytes, built)."""
    dst = os.path.join(out, record_name(rec["group"], rec["record"]) + ".dspm")
    if os.path.exists(dst) and not force:
        return os.path.getsize(dst), False
    tmpdir = tempfile.mkdtemp(prefix="speech_")
    try:
        cut = os.path.join(tmpdir, "cut.asf")
        wav = os.path.join(tmpdir, "raw.wav")
        res = os.path.join(tmpdir, "res.wav")
        carve(BIG, rec["offset"], rec["length"], cut)
        run([vgm, "-o", wav, cut])
        run(["ffmpeg", "-y", "-v", "error", "-i", wav,
             "-ar", str(rate), "-ac", "1", "-c:a", "pcm_s16le", res])
        tmp = dst + ".part"
        run([DSPENC, res, "-o", tmp, "--block", str(block)], env=env)
        os.replace(tmp, dst)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    return os.path.getsize(dst), True


def cmd_build(args):
    if not args.out:
        sys.exit("--out is required")
    os.makedirs(args.out, exist_ok=True)
    if not os.path.isfile(DSPENC):
        sys.exit("missing %s -- build it with:\n"
                 "  gcc -O2 -o tools/bin/dspenc.exe tools/src/dspenc.c -lm" % DSPENC)
    vgm = find_vgmstream()
    env = os.environ.copy()
    env["PATH"] = CYGWIN_BIN + os.pathsep + env.get("PATH", "")

    groups, sounds = load_index()
    want = None
    if args.groups:
        want = set(int(x) for x in args.groups.split(",") if x.strip())
    recs = [r for g in groups for r in g["records"]
            if want is None or g["index"] in want]

    total = built = 0
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futs = {pool.submit(build_one, r, args.out, vgm, env, args.rate, args.block,
                            args.force): r for r in recs}
        for f in concurrent.futures.as_completed(futs):
            n, b = f.result()
            total += n
            built += b
            done += 1
            if b or done % 100 == 0:
                r = futs[f]
                print("[%d/%d] %s %s" % (done, len(recs),
                      record_name(r["group"], r["record"]),
                      "built" if b else "exists"), flush=True)

    # The console's lookup table: retail's own idx.
    shutil.copyfile(IDX, os.path.join(args.out, "UG2_Speech.idx"))
    print("\nspeech set: %d sounds (%d built), %.1f MB in %s, plus UG2_Speech.idx"
          % (len(recs), built, total / 1048576.0, args.out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--list", action="store_true", help="dump the clump index and stop")
    ap.add_argument("--group", type=int, default=None,
                    help="with --list, also print this group's records")
    ap.add_argument("--out", help="output directory for the .dspm set")
    ap.add_argument("--groups", help="comma separated group numbers (default: all 14)")
    ap.add_argument("--rate", type=int, default=22050,
                    help="output sample rate; the source is 22050 mono (default: %(default)s)")
    ap.add_argument("--block", type=int, default=4096, help="dspenc block size in frames")
    ap.add_argument("--jobs", type=int, default=6, help="parallel conversions")
    ap.add_argument("--force", action="store_true", help="rebuild files that already exist")
    args = ap.parse_args()
    if args.list:
        cmd_list(args)
    else:
        cmd_build(args)


if __name__ == "__main__":
    main()
