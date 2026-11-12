"""Find every `bl` to an address (or symbol) in Speed.elf's executable sections.
usage: gc_callers.py <symbol-or-0xADDR>"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gc_disasm as g  # noqa: E402

data, secs = g.load()
syms = g.symbols(data, secs)
byaddr = sorted((v, n) for n, (v, _s) in syms.items())
arg = sys.argv[1]
target = int(arg, 16) if arg.startswith("0x") else syms[arg][0]


def sym_at(va):
    import bisect
    i = bisect.bisect_right([v for v, _ in byaddr], va) - 1
    return byaddr[i] if i >= 0 else (0, "?")


for s in secs:
    if not (s["type"] == 1 and s["size"] and s.get("sname", "").startswith(".text")):
        continue
    base, off, size = s["addr"], s["off"], s["size"]
    for i in range(0, size, 4):
        w, = struct.unpack_from(">I", data, off + i)
        if (w >> 26) != 18 or (w & 3) != 1:
            continue
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        if base + i + li == target:
            v, n = sym_at(base + i)
            print("%08x  %s+0x%x" % (base + i, n, base + i - v))
