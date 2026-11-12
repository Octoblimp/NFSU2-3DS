import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from niself import chunks

def load(b, base, size):
    p = base
    while b[p] == 0x11:
        p += 1
    e = b[p:base + size]
    shoff, = struct.unpack_from("<I", e, 0x20)
    shnum, = struct.unpack_from("<H", e, 0x30)
    secs = [struct.unpack_from("<10I", e, shoff + i * 40) for i in range(shnum)]
    data = [s for s in secs if s[1] == 1][0]
    d = bytearray(e[data[4]:data[4] + data[5]])
    for s in secs:
        if s[1] == 9:
            for k in range(s[5] // 8):
                o, inf = struct.unpack_from("<II", e, s[4] + k * 8)
                # R_MIPS_32 against the section symbol: value is already a .data offset
                assert inf & 255 == 2
    sym = [s for s in secs if s[1] == 2][0]
    strs = secs[sym[6]]
    syms = {}
    for k in range(sym[5] // 16):
        nm, val, sz, inf, oth, shn = struct.unpack_from("<IIIBBH", e, sym[4] + k * 16)
        s = e[strs[4] + nm:]
        syms[s[:s.index(0)].decode()] = val
    return bytes(d), syms

def u32(d, o): return struct.unpack_from("<I", d, o)[0]
def cstr(d, o): return d[o:d.index(0, o)].decode(errors='replace')

def hd(d, o, n):
    for i in range(o, min(len(d), o + n), 16):
        r = d[i:i + 16]
        print("    %04X " % i + " ".join("%02X" % x for x in r))

def dump(d, syms):
    bk = syms['__AnimationBank:::bank']
    bank = bk
    print("  bank ptr 0x%X (symbol at 0x%X)" % (bank, bk))
    hd(d, bank, 0x20)
    n = u32(d, bank + 4)
    maps = u32(d, bank + 0x10)
    names = u32(d, bank + 0x14)
    for i in range(n):
        m = u32(d, maps + 4 * i)
        nm = cstr(d, u32(d, names + 4 * i))
        ty = struct.unpack_from("<H", d, m)[0]
        print("  anim %d %-40s map 0x%X type 0x%X" % (i, nm, m, ty))
        hd(d, m, 0x40)

if __name__ == "__main__":
    b = open(sys.argv[1], "rb").read()
    out = []
    chunks(b, 0, len(b), 0, out)
    for dp, cid, off, size in out:
        if cid == 0x00E34010:
            print("bank chunk at 0x%X size %d" % (off, size))
            d, syms = load(b, off + 8, size)
            dump(d, syms)
            if len(sys.argv) > 2: break
