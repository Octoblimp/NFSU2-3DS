#!/usr/bin/env python3
"""Who calls / who reads: cross-references in `Gamecube Files/Speed.elf`.

gc_disasm.py answers "what does this function do"; this answers the other
question a port keeps asking -- "and who does it to whom". Two modes:

  calls <addr-or-symbol>   every `bl` landing on that address, named by the
                           function the call site is inside
  reads <addr-or-symbol>   every instruction with that address as its r13 or
                           r2 small-data displacement, i.e. every read or
                           write of a global. r13/r2 are taken from the ELF's
                           own _SDA_BASE_/_SDA2_BASE_ symbols.

Usage:
    python tools/gc_xref.py calls DetectWrongWay__6Playeri
    python tools/gc_xref.py reads 0x8046D0A8
"""
import os
import struct
import sys

ELF = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'Gamecube Files', 'Speed.elf')


def load():
    data = open(ELF, 'rb').read()
    e_shoff, = struct.unpack_from('>I', data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('>HHH', data, 0x2e)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        nm, typ, flags, addr, off, size, link, info, align, entsize = \
            struct.unpack_from('>IIIIIIIIII', data, o)
        secs.append(dict(nm=nm, typ=typ, addr=addr, off=off, size=size,
                         link=link, entsize=entsize))
    shstr = secs[e_shstrndx]
    for s in secs:
        end = data.find(b'\0', shstr['off'] + s['nm'])
        s['name'] = data[shstr['off'] + s['nm']:end].decode()

    syms = []
    for s in secs:
        if s['typ'] != 2:           # SHT_SYMTAB
            continue
        strtab = secs[s['link']]
        for i in range(s['size'] // s['entsize']):
            o = s['off'] + i * s['entsize']
            nm, val, size, info, other, shndx = \
                struct.unpack_from('>IIIBBH', data, o)
            end = data.find(b'\0', strtab['off'] + nm)
            name = data[strtab['off'] + nm:end].decode('ascii', 'replace')
            if name:
                syms.append((val, size, name))
    syms.sort()
    return data, secs, syms


def resolve(syms, what):
    if what.lower().startswith('0x'):
        return int(what, 16)
    hit = [s for s in syms if s[2] == what]
    if not hit:
        hit = [s for s in syms if what in s[2]]
    if not hit:
        raise SystemExit('no symbol matching %r' % what)
    if len(hit) > 1:
        print('note: %d symbols match, using %s' % (len(hit), hit[0][2]))
    return hit[0][0]


def owner(syms, addr):
    lo, hi = 0, len(syms)
    best = None
    for val, size, name in syms:
        if val <= addr and (size == 0 or addr < val + size):
            if best is None or val > best[0]:
                best = (val, name)
    return best[1] if best else '?'


def text_words(data, secs):
    for s in secs:
        if s['typ'] == 8 or not (s['name'].startswith('.text') or
                                 s['name'] == '.init'):
            continue
        for o in range(0, s['size'] - 3, 4):
            (w,) = struct.unpack_from('>I', data, s['off'] + o)
            yield s['addr'] + o, w


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    mode, what = sys.argv[1], sys.argv[2]
    data, secs, syms = load()
    target = resolve(syms, what)
    print('%s 0x%08x (%s)' % (mode, target, owner(syms, target)))

    if mode == 'calls':
        n = 0
        for addr, w in text_words(data, secs):
            if (w >> 26) != 18 or not (w & 1):      # bl only
                continue
            off = w & 0x03FFFFFC
            if off & 0x02000000:
                off -= 0x04000000
            dst = addr + off if not (w & 2) else off
            if dst == target:
                print('  %08x  in %s' % (addr, owner(syms, addr)))
                n += 1
        print('  %d call site(s)' % n)
        return

    if mode == 'reads':
        # r13/r2 relative: the ELF names its own small-data bases.
        bases = {}
        for val, size, name in syms:
            if name == '_SDA_BASE_':
                bases[13] = val
            elif name == '_SDA2_BASE_':
                bases[2] = val
        if not bases:
            raise SystemExit('no _SDA_BASE_/_SDA2_BASE_ in this ELF')
        print('  r13=0x%08x r2=0x%08x' % (bases.get(13, 0), bases.get(2, 0)))
        n = 0
        for addr, w in text_words(data, secs):
            op = w >> 26
            if op not in (32, 36, 34, 38, 40, 44, 48, 52, 50, 54, 14, 15):
                continue                      # lwz/stw/lbz/stb/lhz/sth/lfs/stfs/lfd/stfd/addi/addis
            ra = (w >> 16) & 31
            if ra not in bases:
                continue
            d = w & 0xFFFF
            if d & 0x8000:
                d -= 0x10000
            if bases[ra] + d == target:
                print('  %08x  in %s' % (addr, owner(syms, addr)))
                n += 1
        print('  %d reference(s)' % n)
        return

    raise SystemExit('mode is "calls" or "reads"')


if __name__ == '__main__':
    main()
