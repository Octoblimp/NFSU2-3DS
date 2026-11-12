#!/usr/bin/env python3
"""Convert a retail EA AEMS module bank (PC `.abk`, "ABKC") into the 3DS port's
`.aem` bank, which `3DS Files/source/aems.c` loads and runs (TASKS.md #298).

What an .abk is (HANDOFF.md, "#298 full audio: retail's AEMS/Csis traced"):
    * a module image: module headers, instance templates, tables, sample tables,
      and each module's x86 update function -- the compiled sound graph;
    * pointer relocations, Csis interface fixups (which class each module
      answers to, by name), and the x86 call fixups (which of the 40 AEMS node
      functions each `call` is);
    * the samples, an embedded EA SND bank ("BNKl" v5, EA-XA 4-bit ADPCM).

What the port keeps:
    * the module image byte for byte (it is little-endian x86 data, so it is
      valid as-is on the 3DS's little-endian ARM11);
    * the relocation and Csis tables, re-emitted in a flat form;
    * each module's update function LIFTED into a tiny bytecode (below), since
      the ARM11 cannot run x86. The PC compiler emitted the graphs as straight
      runs of `push esi; call node; mov [esi+N], eax; add esi, N`, plus the
      inlined add / clamp forms, so the lift is exact: every instruction form in
      every PC .abk is in `LIFT` below (tools/re/audio/survey.txt is the census),
      and anything else is refused, not guessed;
    * every sample, decoded by vgmstream and re-encoded as Nintendo DSPADPCM at
      its own rate, so NDSP plays it in hardware.

Bytecode (u32 words; op in the low byte, operands in the following words):
    CALL  fn            eax = node[fn](esi)             push esi; call fn
    LD    off           eax = [esi+off]                 mov eax, [esi+off]
    LDC   off           ecx = [esi+off]                 mov ecx, [esi+off]
    ST    off           [esi+off] = eax                 mov [esi+off], eax
    STB   off imm       byte [esi+off] = imm            mov byte [esi+off], imm
    ADD / SUB / IMUL off   eax op= [esi+off]
    MOVAC               eax = ecx                       mov eax, ecx
    CMP                 flags = eax ? ecx (signed)      cmp eax, ecx
    JG / JL  target     word index                      jg / jl
    ESI   n             esi += n                        add esi, n
    END                                                 ret

.aem layout (little-endian; source/aems.c AemFileHeader is the reader):
    0x00 'AEM3', u32 version 1
    0x08 u32 imageOff, imageSize        the ABK's bytes [0, first bank)
    0x10 u32 relocOff, relocCount       u32 image offsets, each += image base
    0x18 u32 csisOff,  csisCount        {u32 imageOff; u32 kind; char name[24]}
    0x20 u32 modOff,   modCount         {u32 hdrOff; u32 codeOff; u32 words; u32 0}
    0x28 u32 sampleOff, sampleCount     0x40-byte SampleRec per BNK entry
    0x30 char name[16]
    SampleRec: u32 dataOff, bytes, nsamples, rate, loop, 0;
               u16 coefs[16]; u16 ps; s16 hist0, hist1; u16 0
    then the bytecode, then each sample's DSPADPCM frames (32-byte aligned).

Usage:
    python tools/abk3ds.py <in.abk> -o <out.aem>
    python tools/abk3ds.py <in.abk> --dump        print the lift, write nothing
    python tools/abk3ds.py --survey               lift every PC .abk, write nothing
"""
import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOUND = os.path.join(os.environ.get("NFSU2_PC_ROOT") or os.path.join(ROOT, "PC Files"), "SOUND")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolbin  # noqa: E402  (dspenc / vgmstream-cli / ffmpeg on any system)
DSPENC = toolbin.find("dspenc")
VGMSTREAM = toolbin.find("vgmstream-cli")
CYGWIN_BIN = r"C:\devkitPro\msys2\usr\bin"
FRAME_SAMPLES = 14

# sndaemsfuncs, in order (Speed.elf; the PC's function fixups index the same table).
FUNCS = ["ClassDestructor", "ClassData", "GlobalVariable", "create", "destroy",
         "CallFunction", "counter", "random", "randomshuffle", "randomweighted",
         "rangetrig", "delaytrig", "stategen", "merge", "envelope", "table",
         "delayline", "mux", "demux", "min", "max", "scale", "add", "subtract",
         "multiply", "divide", "modulo", "player", "oscillator", "ramp", "addmax",
         "subtractmin", "multiplymax", "min2", "max2", "scale2", "add2", "Function",
         "ControlClass", "SetGlobalVariable"]

OP_END, OP_CALL, OP_LD, OP_LDC, OP_ST, OP_STB, OP_ADD, OP_SUB, OP_IMUL, \
    OP_MOVAC, OP_CMP, OP_JG, OP_JL, OP_ESI = range(14)
OPNAMES = ["END", "CALL", "LD", "LDC", "ST", "STB", "ADD", "SUB", "IMUL",
           "MOVAC", "CMP", "JG", "JL", "ESI"]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def s32(d, o):
    return struct.unpack_from("<i", d, o)[0]


class Abk:
    """The ABKC header, modules and tables, as SNDAEMSI_resolvemodulebank reads them."""

    def __init__(self, path):
        self.path = path
        d = self.d = open(path, "rb").read()
        if d[:4] != b"ABKC":
            raise ValueError("%s: not an ABKC module bank" % path)
        self.nmod = struct.unpack_from("<H", d, 0x0a)[0]
        self.size, self.codesz, self.mod0 = u32(d, 0x14), u32(d, 0x18), u32(d, 0x1c)
        self.banks = [(u32(d, 0x20), u32(d, 0x24)), (u32(d, 0x28), u32(d, 0x2c))]
        fix = u32(d, 0x30)
        self.fixups = {}
        for i in range(u32(d, fix)):
            o = u32(d, fix + 4 + 4 * i)
            self.fixups[o] = u32(d, o)
        rel = u32(d, 0x34)
        self.relocs = [u32(d, rel + 4 + 4 * i) for i in range(u32(d, rel))]
        cs = u32(d, 0x38)
        self.csis = []
        for i in range(u32(d, cs)):
            off, ida, kind = struct.unpack_from("<IIB", d, cs + 4 + 12 * i)
            name = d[ida + 4:d.index(b"\0", ida + 4)].decode("ascii")
            self.csis.append((off, kind, name))
        self.mods = []
        o = self.mod0
        for _ in range(self.nmod):
            npl, dtor, cdata, nctl = struct.unpack_from("<BBBB", d, o + 0x24)
            fn, tmpl, isz, link = struct.unpack_from("<IIII", d, o + 0x28)
            self.mods.append(dict(hdr=o, fn=fn, tmpl=tmpl, isz=isz, players=npl,
                                  ctls=nctl, link=link))
            o += 0x3c + (npl + nctl) * 4
        # The image the port keeps is everything ahead of the first sample bank.
        firsts = [b for b, _ in self.banks if b] + [self.size]
        self.image_end = min(firsts)
        for r in self.relocs:
            if not (r + 4 <= self.image_end and u32(d, r) < self.image_end):
                raise ValueError("%s: relocation at %#x points outside the image" % (path, r))

    def code_range(self, m):
        """A module's update function runs from its entry to its `ret`."""
        return self.mods[m]["fn"], self.image_end


def lift(abk, m):
    """Lift module m's x86 update function into bytecode words. Exact or refused."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    from capstone import x86 as X
    d = abk.d
    start, end = abk.code_range(m)
    insns = []
    for ins in md.disasm(d[start:end], start):
        insns.append(ins)
        if ins.mnemonic == "ret":
            break
    else:
        raise ValueError("module %d: no ret" % m)

    def mem(op):
        if op.type != X.X86_OP_MEM:
            return None
        if op.mem.base != X.X86_REG_ESI or op.mem.index != 0:
            return None
        return op.mem.disp

    words, addr2word, branches = [], {}, []
    pending_push = False
    for k, ins in enumerate(insns):
        addr2word[ins.address] = len(words)
        mn, ops = ins.mnemonic, ins.operands
        fix = [abk.fixups[a] for a in range(ins.address, ins.address + ins.size) if a in abk.fixups]
        # prologue / epilogue / stack bookkeeping carry no meaning here
        if k == 0 and mn == "push" and ops[0].reg == X.X86_REG_ESI:
            continue
        if k == 1 and mn == "mov" and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_ESI:
            continue
        if mn == "push" and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_ESI:
            pending_push = True
            continue
        if mn == "call":
            if not pending_push or len(fix) != 1:
                raise ValueError("module %d @%#x: call without push esi / fixup" % (m, ins.address))
            if fix[0] >= len(FUNCS):
                raise ValueError("module %d @%#x: node function %d" % (m, ins.address, fix[0]))
            words += [OP_CALL | (fix[0] << 8)]
            pending_push = False
            continue
        if pending_push:
            raise ValueError("module %d @%#x: push esi not followed by call" % (m, ins.address))
        if mn == "add" and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_ESP:
            continue
        if mn == "pop" and ops[0].reg == X.X86_REG_ESI:
            continue
        if mn == "ret":
            words += [OP_END]
            continue
        if mn == "add" and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_ESI \
                and ops[1].type == X.X86_OP_IMM:
            words += [OP_ESI, ops[1].imm & 0xffffffff]
            continue
        if mn == "mov" and ops[0].type == X.X86_OP_REG and mem(ops[1]) is not None:
            if ops[0].reg == X.X86_REG_EAX:
                words += [OP_LD, mem(ops[1]) & 0xffffffff]
                continue
            if ops[0].reg == X.X86_REG_ECX:
                words += [OP_LDC, mem(ops[1]) & 0xffffffff]
                continue
        if mn == "mov" and mem(ops[0]) is not None and ops[1].type == X.X86_OP_REG \
                and ops[1].reg == X.X86_REG_EAX and ops[0].size == 4:
            words += [OP_ST, mem(ops[0]) & 0xffffffff]
            continue
        if mn == "mov" and mem(ops[0]) is not None and ops[1].type == X.X86_OP_IMM \
                and ops[0].size == 1:
            words += [OP_STB, mem(ops[0]) & 0xffffffff, ops[1].imm & 0xff]
            continue
        if mn in ("add", "sub", "imul") and len(ops) == 2 and ops[0].type == X.X86_OP_REG \
                and ops[0].reg == X.X86_REG_EAX and mem(ops[1]) is not None:
            words += [{"add": OP_ADD, "sub": OP_SUB, "imul": OP_IMUL}[mn], mem(ops[1]) & 0xffffffff]
            continue
        if mn == "mov" and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_EAX \
                and ops[1].type == X.X86_OP_REG and ops[1].reg == X.X86_REG_ECX:
            words += [OP_MOVAC]
            continue
        if mn == "cmp" and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_EAX \
                and ops[1].type == X.X86_OP_REG and ops[1].reg == X.X86_REG_ECX:
            words += [OP_CMP]
            continue
        if mn in ("jg", "jl"):
            branches.append((len(words) + 1, ops[0].imm))
            words += [OP_JG if mn == "jg" else OP_JL, 0]
            continue
        raise ValueError("module %d @%#x: unlifted instruction %s %s"
                         % (m, ins.address, mn, ins.op_str))
    for at, target in branches:
        if target not in addr2word:
            raise ValueError("module %d: branch to %#x is not an instruction" % (m, target))
        words[at] = addr2word[target]
    return words


def disasm_words(words):
    out, i = [], 0
    while i < len(words):
        op = words[i] & 0xff
        if op == OP_CALL:
            out.append("%4d CALL %s" % (i, FUNCS[words[i] >> 8])); i += 1
        elif op in (OP_LD, OP_LDC, OP_ST, OP_ADD, OP_SUB, OP_IMUL, OP_ESI, OP_JG, OP_JL):
            v = words[i + 1]
            v = v - (1 << 32) if v & 0x80000000 else v
            out.append("%4d %s %#x" % (i, OPNAMES[op], v)); i += 2
        elif op == OP_STB:
            out.append("%4d STB %#x, %d" % (i, words[i + 1], words[i + 2])); i += 3
        else:
            out.append("%4d %s" % (i, OPNAMES[op])); i += 1
    return out


def bnk_entries(d, base):
    """{index: tags} for an embedded EA SND bank. The offset table is at +0x14, each
    entry relative to its own address; 0 is an empty slot. Patch tags are the EA
    "PT" set: 0x84 rate, 0x85 samples, 0x86 / 0x87 loop start / end (inclusive)."""
    if d[base:base + 4] != b"BNKl":
        raise ValueError("bank at %#x is not BNKl" % base)
    ver, cnt = struct.unpack_from("<HH", d, base + 4)
    out = {}
    for i in range(cnt):
        e = base + 0x14 + 4 * i
        r = u32(d, e)
        if not r:
            continue
        o = e + r
        if d[o:o + 2] != b"PT":
            raise ValueError("bank entry %d: no PT header" % i)
        o += 4
        tags = {}
        while True:
            t = d[o]; o += 1
            if t == 0xFF:
                break
            if t in (0xFC, 0xFD, 0xFE):
                continue
            n = d[o]; o += 1
            tags.setdefault(t, int.from_bytes(d[o:o + n], "big")); o += n
        out[i] = tags
    return cnt, out


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise RuntimeError("%s failed:\n%s%s" % (os.path.basename(cmd[0]), r.stdout, r.stderr))
    return r.stdout


# vgmstream refuses a stream under 300 Hz (VGMSTREAM_MIN_SAMPLE_RATE).
VGMSTREAM_MIN_RATE = 300


def rate_patched_copy(abk, base, idx, path):
    """Write the ABK to `path` with bank entry `idx`'s PT rate (0x84) read as 32000,
    for vgmstream to decode a sample it would refuse for its rate: SWTN_CAR_00_MB and
    SWTN_CAR_10_MB each hold a 440-sample entry at 100 Hz. The samples decode the
    same at any rate; the caller keeps the real one. The tag list is rebuilt in its
    own bytes -- the 2-byte rate in place of the 1-byte one, the tags vgmstream does
    not decode by (0x06 priority, 0x13) dropped to make room, 0xFC padding after."""
    d = bytearray(abk.d)
    e = base + 0x14 + 4 * idx
    o = e + u32(d, e) + 4
    start, tags = o, []
    while d[o] != 0xFF:
        t = d[o]; o += 1
        if t in (0xFC, 0xFD, 0xFE):
            continue
        n = d[o]
        tags.append((t, bytes(d[o + 1:o + 1 + n]))); o += 1 + n
    room = o - start
    body = b"".join(bytes((t, 2)) + b"\x7d\x00" if t == 0x84 else bytes((t, len(v))) + v
                    for t, v in tags if t not in (0x06, 0x13))
    if len(body) > room:
        raise ValueError("entry %d: no room to rewrite its rate" % idx)
    d[start:o] = body + b"\xFC" * (room - len(body))
    open(path, "wb").write(d)


def encode_samples(abk, tmpdir):
    """{bnk index: (rec fields, frames)} for the module image's first bank."""
    base, _ = abk.banks[0]
    if not base:
        return 0, {}
    cnt, ents = bnk_entries(abk.d, base)
    env = os.environ.copy()
    env["PATH"] = CYGWIN_BIN + os.pathsep + env.get("PATH", "")
    out = {}
    # vgmstream numbers the bank's non-empty entries 1..n, mostly in table order --
    # but not always (ROADNOISE_00_MB lists entries 12..14 first), so each entry
    # takes the first free subsong with its sample count, table order breaking ties.
    totals = {}
    for sub in range(1, len(ents) + 1):
        try:  # a stream under VGMSTREAM_MIN_RATE is refused: it keeps its place
            meta = run([VGMSTREAM, "-m", "-s", str(sub), abk.path])
            totals[sub] = int(meta.split("stream total samples:")[1].split()[0])
        except RuntimeError:
            totals[sub] = None
    free = sorted(totals)
    subs = {}
    for pos, idx in enumerate(sorted(ents), 1):
        want = ents[idx][0x85]
        hit = [s for s in free if totals[s] == want]
        sub = pos if pos in hit else (hit[0] if hit else pos)
        if sub in free:
            free.remove(sub)
        subs[idx] = sub
    for idx in sorted(ents):
        sub = subs[idx]
        tags = ents[idx]
        wav = os.path.join(tmpdir, "s.wav")
        dsp = os.path.join(tmpdir, "s.dspm")
        src = abk.path
        if tags.get(0x84, VGMSTREAM_MIN_RATE) < VGMSTREAM_MIN_RATE:
            src = os.path.join(tmpdir, "lowrate.abk")
            rate_patched_copy(abk, base, idx, src)
        meta = run([VGMSTREAM, "-m", "-s", str(sub), src])
        rate = int(meta.split("sample rate:")[1].split("Hz")[0])
        if src != abk.path:
            rate = tags[0x84]
        total = int(meta.split("stream total samples:")[1].split()[0])
        if total != tags[0x85]:
            raise ValueError("entry %d: vgmstream subsong %d has %d samples, PT says %d"
                             % (idx, sub, total, tags[0x85]))
        run([VGMSTREAM, "-i", "-o", wav, "-s", str(sub), src])
        loops = 0x86 in tags
        frames = max(1, (total + FRAME_SAMPLES - 1) // FRAME_SAMPLES)
        run([DSPENC, wav, "-o", dsp, "--block", str(frames)], env=env)
        img = open(dsp, "rb").read()
        if img[:4] != b"DSPM" or u32(img, 0x0c) != 1 or u32(img, 0x18) != 1:
            raise ValueError("entry %d: dspenc did not write one mono block" % idx)
        nsamp = u32(img, 0x10)
        doff = u32(img, 0x1c)
        coefs = img[0x28:0x48]
        ps, h0, h1 = struct.unpack_from("<Hhh", img, 0x48)
        data = img[doff:doff + frames * 8]
        # A loop retail starts a few samples in (2 banks of 176, by 2-3 samples)
        # is played from 0: the 0.1 ms before it is not worth a second waveBuf.
        out[idx] = dict(rate=rate, nsamples=nsamp, loop=1 if loops else 0,
                        coefs=coefs, ps=ps, h0=h0, h1=h1, data=data)
    return cnt, out


def build(abk, out_path, dump=False):
    lifted = [lift(abk, m) for m in range(abk.nmod)]
    if dump:
        for m, w in enumerate(lifted):
            md = abk.mods[m]
            print("module %d: hdr %#x tmpl %#x isz %#x players %d ctls %d -> %d words"
                  % (m, md["hdr"], md["tmpl"], md["isz"], md["players"], md["ctls"], len(w)))
            for line in disasm_words(w):
                print("   ", line)
        for off, kind, name in abk.csis:
            print("csis %#x kind %d %s" % (off, kind, name))
        return
    if abk.banks[1][0]:
        raise ValueError("%s: a second sample bank is not handled yet" % abk.path)
    tmpdir = tempfile.mkdtemp(prefix="abk3ds_")
    try:
        nsamp, samples = encode_samples(abk, tmpdir)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    def align(n, a=32):
        return (n + a - 1) & ~(a - 1)

    hdr_size = 0x40
    reloc_off = hdr_size
    csis_off = reloc_off + 4 * len(abk.relocs)
    mod_off = csis_off + 32 * len(abk.csis)
    samp_off = mod_off + 16 * abk.nmod
    code_off = samp_off + 0x40 * nsamp
    code_blobs, mod_recs, cur = [], [], code_off
    for w in lifted:
        mod_recs.append((cur, len(w)))
        code_blobs.append(struct.pack("<%dI" % len(w), *w))
        cur += 4 * len(w)
    image_off = align(cur)
    data_off = align(image_off + abk.image_end)
    blob = bytearray()
    srecs = []
    for i in range(nsamp):
        s = samples.get(i)
        if not s:
            srecs.append(b"\0" * 0x40)
            continue
        o = data_off + len(blob)
        blob += s["data"]
        blob += b"\0" * ((-len(blob)) & 31)
        srecs.append(struct.pack("<6I", o, len(s["data"]), s["nsamples"], s["rate"], s["loop"], 0)
                     + s["coefs"] + struct.pack("<HhhH", s["ps"], s["h0"], s["h1"], 0))
    name = os.path.splitext(os.path.basename(abk.path))[0].encode("ascii")[:15]
    f = bytearray()
    f += b"AEM3" + struct.pack("<I", 1)
    f += struct.pack("<10I", image_off, abk.image_end, reloc_off, len(abk.relocs),
                     csis_off, len(abk.csis), mod_off, abk.nmod, samp_off, nsamp)
    f += name + b"\0" * (16 - len(name))
    assert len(f) == hdr_size
    f += struct.pack("<%dI" % len(abk.relocs), *abk.relocs)
    for off, kind, nm in abk.csis:
        nb = nm.encode("ascii")
        if len(nb) > 23:
            raise ValueError("Csis name too long: %s" % nm)
        f += struct.pack("<II", off, kind) + nb + b"\0" * (24 - len(nb))
    for (co, nw), md in zip(mod_recs, abk.mods):
        f += struct.pack("<4I", md["hdr"], co, nw, 0)
    for r in srecs:
        f += r
    for b in code_blobs:
        f += b
    f += b"\0" * (image_off - len(f))
    f += abk.d[:abk.image_end]
    f += b"\0" * (data_off - len(f))
    f += blob
    tmp = out_path + ".tmp"
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(tmp, "wb") as fp:
        fp.write(f)
    os.replace(tmp, out_path)
    print("%s: %d module(s), %d bytecode words, %d sample(s), %.1f KB -> %s"
          % (os.path.basename(abk.path), abk.nmod, sum(len(w) for w in lifted),
             len(samples), len(f) / 1024.0, out_path))


def survey():
    import collections
    calls = collections.Counter()
    bad = 0
    for path in sorted(glob.glob(os.path.join(SOUND, "*", "*.abk"))):
        try:
            abk = Abk(path)
            for m in range(abk.nmod):
                for i, w in enumerate(lift(abk, m)):
                    pass
                w = lift(abk, m)
                i = 0
                while i < len(w):
                    op = w[i] & 0xff
                    if op == OP_CALL:
                        calls[FUNCS[w[i] >> 8]] += 1
                    i += {OP_LD: 2, OP_LDC: 2, OP_ST: 2, OP_ADD: 2, OP_SUB: 2, OP_IMUL: 2,
                          OP_ESI: 2, OP_JG: 2, OP_JL: 2, OP_STB: 3}.get(op, 1)
            if abk.banks[1][0]:
                print("second bank:", os.path.basename(path))
        except Exception as e:  # noqa: BLE001 -- the survey reports every failure
            bad += 1
            print("FAIL %s: %s" % (os.path.basename(path), e))
    print("node calls:", ", ".join("%s %d" % kv for kv in calls.most_common()))
    print("failures:", bad)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("abk", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--dump", action="store_true")
    ap.add_argument("--survey", action="store_true")
    a = ap.parse_args()
    if a.survey:
        survey()
        return
    if not a.abk:
        ap.error("an .abk is required")
    abk = Abk(a.abk)
    if a.dump:
        build(abk, None, dump=True)
        return
    if not a.out:
        ap.error("-o is required")
    build(abk, a.out)


if __name__ == "__main__":
    main()
