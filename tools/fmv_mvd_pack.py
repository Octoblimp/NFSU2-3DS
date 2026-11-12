#!/usr/bin/env python3
"""Re-encode NFSU2's MOVIES/*.vp6 for the New 3DS hardware video decoder (MVD).

Pipeline (all host-side; nothing here ships to the console):

    PC Files/MOVIES/X.vp6  --ffmpeg/libx264-->  baseline H.264 Annex-B (temp)
                           --this script----->  <GameDir>/video/X.mvd

The New 3DS has a dedicated H.264 decode block behind the `mvd:STD` service
(see 3DS Files/source/mvd_player.c). Feeding it means the ARM11 never touches
a coefficient or a pixel: the block decodes AND colour-converts straight into
a linear RGB565 buffer, and the GPU's transfer engine tiles that into a
texture. The software VP6 path (source/vp6/) stays as the fallback for Old 3DS
and for emulators that do not implement MVD, so the original .vp6 files are
still required on the SD card -- this only ADDS video/*.mvd next to them.
Audio is untouched: the stage still plays audio/X.wav exactly as before.

ENCODE PARAMETERS, AND WHY
--------------------------
* Picture: the real 768x512 (3:2) frame scaled with lanczos to the exact box
  the stage draws it in on the 400x240 top screen, so the 3DS shows it 1:1
  with no GPU filtering. That box is the clip's .fng Movie widget with the
  640x480 canvas spread across the whole panel (TASKS.md #333): 400x240 for a
  whole-canvas Movie (UG_LS_IntroFMV; LS_EAlogo and LS_THXMovie, which have
  no package, take the same), 400x216 for a 640x432 one (LS_PSAMovie and
  IG_PlayMovie, which plays every other movie). See main.c's movieH.
* Coded frame: 512x256, picture at the top-left, black elsewhere. 512x256 is
  a legal power-of-two GPU texture, so the decoder's linear output and the
  tiled texture have IDENTICAL dimensions and the GX transfer is the plain
  same-size linear->tiled copy -- no reliance on MVD's undocumented
  output-stride override or on the transfer engine's padding behaviour. The
  black margin costs next to nothing (skip macroblocks) and the decode block
  has headroom far beyond 512x256@30 (the N3DS browser plays 854x480).
* Baseline profile, level 2.1 (512 MBs/frame x 30fps = 15360 MB/s, under 2.1's
  19800), one slice per picture, no B-frames: decode order == display order,
  so every access unit yields exactly one output frame. 3dbrew's MVD page lists
  levels 1-3.2 of Baseline/Main/High as working.
* Frame count is preserved 1:1 (asserted below) because the runtime paces
  video against the WAV with `frame = elapsed * fps`, same as the VP6 path.

CONTAINER (.mvd, little-endian)
-------------------------------
    0x00  char[4] 'MVD1'
    0x04  u16 version (1)          0x06 u16 header size (64)
    0x08  u16 coded w, coded h     0x0C u16 pic x, pic y, pic w, pic h
    0x14  u32 fps num, fps den     0x1C u32 frame count
    0x20  u32 index offset         0x24 u32 largest access unit (bytes)
    0x28  u8  profile_idc, level_idc, constraint flags, 0
    0x2C  u32 SPS offset, SPS size 0x34 u32 PPS offset, PPS size
    0x3C  u32 first access-unit offset
    SPS, PPS                       each one NAL with a 00 00 01 start code
    index                          frame count x {u32 offset, u32 size}
    access units                   each = its slice NAL(s), each with 00 00 01

SPS and PPS are stored once and fed to the decoder once at open (in separate
calls, which is what 3dbrew documents for SKATER, the N3DS browser). SEI and
AUD NAL units are dropped; repeated SPS/PPS before later IDRs are dropped after
checking they are byte-identical to the first.

Usage:
    python tools/fmv_mvd_pack.py --out <GameDir>/video              # boot movies
    python tools/fmv_mvd_pack.py --out <GameDir>/video --all        # every movie
    python tools/fmv_mvd_pack.py --out ... --names ealogo PSA --verify
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from fractions import Fraction

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SRC = os.path.join(os.environ.get("NFSU2_PC_ROOT") or os.path.join(ROOT, "PC Files"), "MOVIES")

# The movies the engine currently plays (main.c kVideoStages[] / kIntroMovieStage),
# by the lowercase name the runtime asks for.
BOOT_MOVIES = ["ealogo", "thx_logo", "psa", "FMVOpening"]

CODED_W, CODED_H = 512, 256
PIC_W = 400
# The movies whose Movie widget is the whole 640x480 canvas; the rest are 640x432.
FULL_CANVAS = {"ealogo", "thx_logo", "fmvopening"}


def pic_h(name):
    return 240 if name in FULL_CANVAS else 216
MAGIC = b"MVD1"
HEADER_SIZE = 64

NAL_SLICE, NAL_IDR, NAL_SEI, NAL_SPS, NAL_PPS, NAL_AUD = 1, 5, 6, 7, 8, 9


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode("utf-8", "replace"))
        raise SystemExit("command failed: " + " ".join(cmd))
    return r.stdout


def probe(path):
    """The movie's EA MVhd header: after the chunk's id and size, a 4-byte
    codec tag, u16 width, u16 height, u32 frames, u32 largest chunk, then the
    rate as two u32s (fps = first / second), as ffmpeg's electronicarts.c
    reads it. Read here rather than through ffprobe, so the setup app ships
    one ffmpeg.exe; it matched ffprobe on all 29 retail movies (2026-11-09)."""
    with open(path, "rb") as f:
        d = f.read(65536)
    i = d.find(b"MVhd")
    if i < 0:
        raise SystemExit("%s: no MVhd header (not an EA VP6 movie)" % path)
    _tag, w, h, frames, _big, num, den = struct.unpack_from("<4sHHIIII", d, i + 8)
    return "vp6", w, h, num, den, frames


def encode(src, dst, crf, preset, ph):
    vf = ("scale=%d:%d:flags=lanczos+accurate_rnd,pad=%d:%d:0:0:black,format=yuv420p"
          % (PIC_W, ph, CODED_W, CODED_H))
    run(["ffmpeg", "-y", "-v", "error", "-i", src, "-map", "0:v:0", "-an",
         "-vf", vf, "-fps_mode", "passthrough",
         "-c:v", "libx264", "-profile:v", "baseline", "-level:v", "2.1",
         "-preset", preset, "-crf", str(crf),
         "-x264-params", "ref=3:keyint=60:min-keyint=15:slices=1:aud=0:sliced-threads=0",
         "-f", "h264", dst])


def split_nals(data):
    """Annex-B -> list of raw NAL payloads (no start codes, no trailing zeros)."""
    starts = []
    i, n = 0, len(data)
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        starts.append(j + 3)
        i = j + 3
    nals = []
    for k, s in enumerate(starts):
        e = (starts[k + 1] - 3) if k + 1 < len(starts) else n
        nal = data[s:e].rstrip(b"\x00")  # drops the 4-byte form's leading zero too
        if nal:
            nals.append(nal)
    return nals


def first_mb_in_slice_is_zero(nal):
    # ue(v) first_mb_in_slice right after the 1-byte NAL header: value 0 is
    # coded as the single bit '1', so the MSB of byte 1 tells us.
    return len(nal) > 1 and (nal[1] & 0x80) != 0


def build_container(h264, fps_num, fps_den, ph):
    nals = split_nals(h264)
    sps = pps = None
    aus = []
    for nal in nals:
        t = nal[0] & 0x1F
        if t == NAL_SPS:
            if sps is None:
                sps = nal
            elif nal != sps:
                raise SystemExit("encoder emitted a second, different SPS")
        elif t == NAL_PPS:
            if pps is None:
                pps = nal
            elif nal != pps:
                raise SystemExit("encoder emitted a second, different PPS")
        elif t in (NAL_SEI, NAL_AUD):
            continue
        elif t in (NAL_SLICE, NAL_IDR):
            if first_mb_in_slice_is_zero(nal) or not aus:
                aus.append([])
            aus[-1].append(nal)
        else:
            raise SystemExit("unexpected NAL type %d" % t)
    if sps is None or pps is None:
        raise SystemExit("no SPS/PPS in encoder output")

    sc = b"\x00\x00\x01"
    sps_b, pps_b = sc + sps, sc + pps
    au_blobs = [b"".join(sc + n for n in au) for au in aus]

    sps_off = HEADER_SIZE
    pps_off = sps_off + len(sps_b)
    index_off = pps_off + len(pps_b)
    index_off = (index_off + 3) & ~3
    data_off = index_off + 8 * len(au_blobs)

    index = bytearray()
    body = bytearray()
    for blob in au_blobs:
        index += struct.pack("<II", data_off + len(body), len(blob))
        body += blob

    header = struct.pack("<4sHHHHHHHHIIIIIBBBBIIIII",
                         MAGIC, 1, HEADER_SIZE, CODED_W, CODED_H,
                         0, 0, PIC_W, ph,
                         fps_num, fps_den, len(au_blobs),
                         index_off, max(len(b) for b in au_blobs),
                         sps[1], sps[3], sps[2], 0,
                         sps_off, len(sps_b), pps_off, len(pps_b), data_off)
    assert len(header) == HEADER_SIZE
    out = bytearray(header) + sps_b + pps_b
    out += b"\x00" * (index_off - len(out))
    out += index + body
    return bytes(out), len(au_blobs), sps


def verify(container_path, expect_frames, preview_dir=None):
    """Reassemble the stream exactly as the 3DS will feed it, decode on the host."""
    d = open(container_path, "rb").read()
    (magic, ver, hsz, cw, ch, px, py, pw, ph, fn, fd, nframes, ioff, maxau,
     prof, lvl, cons, _r, so, ss, po, ps, doff) = struct.unpack_from("<4sHHHHHHHHIIIIIBBBBIIIII", d)
    assert magic == MAGIC and nframes == expect_frames
    stream = bytearray(d[so:so + ss] + d[po:po + ps])
    for i in range(nframes):
        off, size = struct.unpack_from("<II", d, ioff + 8 * i)
        assert d[off:off + 3] == b"\x00\x00\x01"
        stream += d[off:off + size]
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "re.h264")
        open(p, "wb").write(stream)
        # ffmpeg alone (no ffprobe): one framecrc line per decoded frame, and
        # the stream's line on stderr ("Video: h264 (Constrained Baseline),
        # yuv420p(progressive), 384x256") for its size and profile.
        r = subprocess.run(["ffmpeg", "-hide_banner", "-i", p, "-map", "0:v:0",
                            "-f", "framecrc", "-"], capture_output=True)
        if r.returncode != 0:
            sys.stderr.write(r.stderr.decode("utf-8", "replace"))
            raise SystemExit("verify: ffmpeg could not decode " + p)
        frames = sum(1 for ln in r.stdout.decode("ascii", "replace").splitlines()
                     if ln.strip() and not ln.startswith("#"))
        m = re.search(r"Video: h264 \(([^)]*)\).*?(\d{2,5})x(\d{2,5})",
                      r.stderr.decode("utf-8", "replace"))
        info = {"nb_read_frames": str(frames),
                "width": m.group(2) if m else "?", "height": m.group(3) if m else "?",
                "has_b_frames": "0" if m and "Baseline" in m.group(1) else "?"}
        if int(info["nb_read_frames"]) != nframes:
            raise SystemExit("verify: host decode got %s frames, container says %d"
                             % (info["nb_read_frames"], nframes))
        if preview_dir:
            os.makedirs(preview_dir, exist_ok=True)
            base = os.path.splitext(os.path.basename(container_path))[0]
            pick = nframes // 2
            run(["ffmpeg", "-y", "-v", "error", "-i", p, "-vf", "select=eq(n\\,%d)" % pick,
                 "-frames:v", "1", os.path.join(preview_dir, base + "_mid.png")])
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT_SRC, help="folder holding the real *.vp6")
    ap.add_argument("--out", required=True, help="<GameDir>/video")
    ap.add_argument("--names", nargs="*", help="movie names (no extension); default: boot movies")
    ap.add_argument("--all", action="store_true", help="every .vp6 in --src")
    ap.add_argument("--crf", type=int, default=18)
    ap.add_argument("--preset", default="veryslow")
    ap.add_argument("--verify", action="store_true", help="host-decode the packed stream")
    ap.add_argument("--preview", help="with --verify: write a mid-clip PNG per movie here")
    a = ap.parse_args()

    files = {os.path.splitext(f)[0].lower(): f for f in os.listdir(a.src) if f.lower().endswith(".vp6")}
    if a.all:
        names = sorted(k for k in files if k != "blank")
    else:
        names = [n.lower() for n in (a.names or BOOT_MOVIES)]
    os.makedirs(a.out, exist_ok=True)

    for name in names:
        if name not in files:
            raise SystemExit("no %s.vp6 in %s" % (name, a.src))
        src = os.path.join(a.src, files[name])
        codec, w, h, fnum, fden, nframes = probe(src)
        # The runtime's output name follows main.c's lowercase-or-real casing
        # for the .vp6 it pairs with; FMVOpening keeps its real capitalisation.
        out_name = "FMVOpening" if name == "fmvopening" else name
        dst = os.path.join(a.out, out_name + ".mvd")
        with tempfile.TemporaryDirectory() as td:
            tmp = os.path.join(td, "enc.h264")
            encode(src, tmp, a.crf, a.preset, pic_h(name))
            h264 = open(tmp, "rb").read()
        blob, count, sps = build_container(h264, fnum, fden, pic_h(name))
        if count != nframes:
            raise SystemExit("%s: %d access units but the .vp6 has %d frames" % (name, count, nframes))
        tmpdst = dst + ".new"
        with open(tmpdst, "wb") as f:
            f.write(blob)
        os.replace(tmpdst, dst)
        fps = Fraction(fnum, fden)
        line = ("%-12s %s %dx%d %d frames @ %.3f fps -> %s  %.2f MB (vp6 %.2f MB)  profile %d level %d"
                % (name, codec, w, h, nframes, float(fps), os.path.relpath(dst, ROOT),
                   len(blob) / 1e6, os.path.getsize(src) / 1e6, sps[1], sps[3]))
        if a.verify:
            info = verify(dst, nframes, a.preview)
            line += "  [verified: %s frames %sx%s b=%s]" % (
                info["nb_read_frames"], info["width"], info["height"], info.get("has_b_frames"))
        print(line)


if __name__ == "__main__":
    main()
