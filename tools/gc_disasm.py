#!/usr/bin/env python3
"""Disassemble a range of `Gamecube Files/Speed.elf` by address or by symbol name.

Why this exists: Ghidra's PowerPC decoder gives up on Gekko's paired-single
opcodes (`psq_l`, `ps_merge*`, ...), and when one lands inside a function the
whole decompile comes back as `halt_baddata()`. Several of the functions this
port needs to read -- `Minimap::Tick`, `MinimapElement::GetWorldTelemetry`,
`Minimap::GetScreenPositionForWorldCoordinate` -- are exactly those. Capstone
decodes the generic PowerPC around them, and the paired-single words are
decoded here by hand from the Gekko supplement's encodings.

Usage:
    python tools/gc_disasm.py <symbol-or-0xADDR> [count]
    python tools/gc_disasm.py --sym <substring>        # list matching symbols
"""
import struct
import sys
import os

ELF = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'Gamecube Files', 'Speed.elf')


def load():
    data = open(ELF, 'rb').read()
    # Big-endian ELF32.
    assert data[:4] == b'\x7fELF' and data[5] == 2, 'not a BE ELF32'
    e_shoff, = struct.unpack_from('>I', data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('>HHH', data, 0x2e)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        name, typ, flags, addr, off, size, link, info, align, entsize = \
            struct.unpack_from('>IIIIIIIIII', data, o)
        secs.append(dict(name=name, type=typ, addr=addr, off=off, size=size,
                         link=link, entsize=entsize))
    shstr = secs[e_shstrndx]
    for s in secs:
        end = data.find(b'\0', shstr['off'] + s['name'])
        s['sname'] = data[shstr['off'] + s['name']:end].decode()
    return data, secs


def symbols(data, secs):
    out = {}
    for s in secs:
        if s['type'] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s['link']]
        n = s['size'] // s['entsize']
        for i in range(n):
            o = s['off'] + i * s['entsize']
            nm, val, size, info, other, shndx = struct.unpack_from('>IIIBBH', data, o)
            end = data.find(b'\0', strtab['off'] + nm)
            name = data[strtab['off'] + nm:end].decode('ascii', 'replace')
            if name:
                out[name] = (val, size)
    return out


def va_to_off(secs, va):
    for s in secs:
        if s['addr'] and s['addr'] <= va < s['addr'] + s['size'] and s['type'] != 8:
            return s['off'] + (va - s['addr'])
    return None


# Gekko paired-single primary opcodes Capstone does not know.
PSQ = {56: 'psq_l', 57: 'psq_lu', 60: 'psq_st', 61: 'psq_stu'}


def gekko(word):
    op = word >> 26
    if op in PSQ:
        d = (word >> 21) & 31
        a = (word >> 16) & 31
        w = (word >> 15) & 1
        i = (word >> 12) & 7
        disp = word & 0xfff
        if disp & 0x800:
            disp -= 0x1000
        return '%s f%d, %d(r%d), %d, qr%d' % (PSQ[op], d, disp, a, w, i)
    if op == 4:  # paired-single arithmetic
        d = (word >> 21) & 31
        a = (word >> 16) & 31
        b = (word >> 11) & 31
        c = (word >> 6) & 31
        sub = (word >> 1) & 0x3ff
        sub5 = (word >> 1) & 0x1f
        names5 = {18: 'ps_div', 20: 'ps_sub', 21: 'ps_add', 23: 'ps_sel',
                  24: 'ps_res', 25: 'ps_mul', 26: 'ps_rsqrte', 28: 'ps_msub',
                  29: 'ps_madd', 30: 'ps_nmsub', 31: 'ps_nmadd'}
        names10 = {40: 'ps_neg', 72: 'ps_mr', 136: 'ps_nabs', 264: 'ps_abs',
                   0: 'ps_cmpu0', 32: 'ps_cmpo0', 64: 'ps_cmpu1', 96: 'ps_cmpo1',
                   528: 'ps_merge00', 560: 'ps_merge01', 592: 'ps_merge10',
                   624: 'ps_merge11', 1014: 'dcbz_l'}
        if sub5 in names5:
            return '%s f%d, f%d, f%d, f%d' % (names5[sub5], d, a, c, b)
        if sub in names10:
            return '%s f%d, f%d, f%d' % (names10[sub], d, a, b)
        return 'ps_?? 0x%08x' % word
    return None


def disasm(va, count=80):
    import capstone
    data, secs = load()
    off = va_to_off(secs, va)
    if off is None:
        raise SystemExit('address 0x%08x is not in a loaded section' % va)
    blob = data[off:off + count * 4]
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    md.detail = False
    syms = symbols(data, secs)
    byaddr = {}
    for n, (v, _s) in syms.items():
        byaddr.setdefault(v, n)
    out = []
    for i in range(count):
        w = struct.unpack_from('>I', blob, i * 4)[0]
        a = va + i * 4
        g = gekko(w)
        if g is not None:
            out.append('%08x  %08x  %s' % (a, w, g))
            continue
        txt = None
        for ins in md.disasm(blob[i * 4:i * 4 + 4], a):
            txt = '%s %s' % (ins.mnemonic, ins.op_str)
        if txt is None:
            txt = '.long 0x%08x' % w
        # Annotate branch targets with their symbol when there is one.
        if txt.startswith('bl ') or txt.startswith('b '):
            try:
                tgt = int(txt.split()[-1], 0)
                if tgt in byaddr:
                    txt += '   ; ' + byaddr[tgt]
            except ValueError:
                pass
        out.append('%08x  %08x  %s' % (a, w, txt))
    return '\n'.join(out)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--sym':
        data, secs = load()
        pat = sys.argv[2]
        for n, (v, s) in sorted(symbols(data, secs).items(), key=lambda kv: kv[1][0]):
            if pat in n:
                print('%08x %5d %s' % (v, s, n))
        raise SystemExit
    arg = sys.argv[1]
    cnt = int(sys.argv[2]) if len(sys.argv) > 2 else 80
    if arg.startswith('0x'):
        va = int(arg, 16)
    else:
        data, secs = load()
        syms = symbols(data, secs)
        if arg not in syms:
            cands = [n for n in syms if arg in n]
            raise SystemExit('no exact symbol %r; candidates: %s' % (arg, cands[:20]))
        va, size = syms[arg]
        if size and len(sys.argv) <= 2:
            cnt = size // 4
    print(disasm(va, cnt))
