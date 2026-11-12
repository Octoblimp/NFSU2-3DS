"""Disassemble SPEED2.EXE (the PC build) by virtual address.

    python tools/pc_disasm.py <hexva> [count]      instructions from <hexva>
    python tools/pc_disasm.py --str <hexva>...     C strings at each VA
    python tools/pc_disasm.py --refs <hexva>       every imm32 that equals <hexva>

The PC exe has no symbols, so this is the x86 twin of gc_disasm.py without the
symbol column. Call targets that land on a known string or float are annotated.
Needs capstone (user site-packages, so do not run it under `python -I`).
"""
import os
import struct
import sys

import capstone

EXE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'PC Files', 'SPEED2.EXE')


def load():
    d = open(EXE, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        vsz, va, rsz, roff = struct.unpack_from('<IIII', d, o + 8)
        secs.append((base + va, max(vsz, rsz), roff, rsz))
    return d, secs


def va_to_off(secs, va):
    for sva, vsz, roff, rsz in secs:
        if sva <= va < sva + rsz:
            return roff + (va - sva)
    return None


def off_to_va(secs, off):
    for sva, vsz, roff, rsz in secs:
        if roff <= off < roff + rsz:
            return sva + (off - roff)
    return None


def cstr(d, secs, va, n=120):
    o = va_to_off(secs, va)
    if o is None:
        return None
    s = d[o:o + n]
    z = s.find(b'\0')
    s = s[:z if z >= 0 else n]
    if len(s) < 2 or any(c < 0x20 or c > 0x7e for c in s):
        return None
    return s.decode('ascii')


def main():
    d, secs = load()
    a = sys.argv[1:]
    if a and a[0] == '--str':
        for x in a[1:]:
            print('%08x  %r' % (int(x, 16), cstr(d, secs, int(x, 16), 200)))
        return
    if a and a[0] == '--refs':
        needle = struct.pack('<I', int(a[1], 16))
        j = d.find(needle)
        while j >= 0:
            print('%08x' % off_to_va(secs, j))
            j = d.find(needle, j + 1)
        return
    va = int(a[0], 16)
    count = int(a[1]) if len(a) > 1 else 60
    o = va_to_off(secs, va)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    n = 0
    for ins in md.disasm(d[o:o + count * 16], va):
        note = ''
        for tok in ins.op_str.replace('[', ' ').replace(']', ' ').replace(',', ' ').split():
            if tok.startswith('0x') and len(tok) >= 8:
                s = cstr(d, secs, int(tok, 16))
                if s:
                    note = '   ; "%s"' % s
                elif ins.mnemonic.startswith('fld') or ins.mnemonic.startswith('fmul') \
                        or ins.mnemonic.startswith('fdiv'):
                    fo = va_to_off(secs, int(tok, 16))
                    if fo is not None:
                        note = '   ; %r' % struct.unpack_from('<f', d, fo)[0]
        print('%08x  %-8s %s%s' % (ins.address, ins.mnemonic, ins.op_str, note))
        n += 1
        if n >= count:
            break


if __name__ == '__main__':
    main()
