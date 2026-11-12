"""Dump Speed.elf data: floats/words at addresses, and the symbol at/before an address.
usage: gc_data.py f 0xADDR count | w 0xADDR count | s 0xADDR | find <substr>"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gc_disasm as g  # noqa: E402

data, secs = g.load()
syms = g.symbols(data, secs)
byaddr = sorted((v, n) for n, (v, _s) in syms.items())


def sym_at(va):
    best = None
    for v, n in byaddr:
        if v <= va:
            best = (v, n)
        else:
            break
    return best


mode = sys.argv[1]
if mode in ("f", "w"):
    va = int(sys.argv[2], 16)
    n = int(sys.argv[3])
    for i in range(n):
        a = va + i * 4
        off = g.va_to_off(secs, a)
        if off is None:
            print("%08x  (not in file)" % a)
            continue
        w, = struct.unpack_from(">I", data, off)
        f, = struct.unpack_from(">f", data, off)
        s = sym_at(a)
        print("%08x  %08x  %-14g  %s+0x%x" % (a, w, f, s[1] if s else "?", a - s[0] if s else 0))
elif mode == "s":
    va = int(sys.argv[2], 16)
    s = sym_at(va)
    print("%08x -> %s+0x%x" % (va, s[1], va - s[0]))
elif mode == "find":
    for v, n in byaddr:
        if sys.argv[2].lower() in n.lower():
            print("%08x %s" % (v, n))
