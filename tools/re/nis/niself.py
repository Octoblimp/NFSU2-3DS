import struct, sys

def chunks(b, off, end, depth, out):
    while off + 8 <= end:
        cid, size = struct.unpack_from("<II", b, off)
        out.append((depth, cid, off, size))
        if cid & 0x80000000 and depth < 4:
            chunks(b, off + 8, off + 8 + size, depth + 1, out)
        off += 8 + size

def elf(b, base, size):
    # skip 0x11 padding
    p = base
    while b[p] == 0x11 and p < base + size:
        p += 1
    e = b[p:base + size]
    if e[:4] != b'\x7fELF':
        print("  no ELF at", hex(p)); return
    shoff, = struct.unpack_from("<I", e, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", e, 0x2E)
    secs = []
    for i in range(shnum):
        nm, ty, fl, addr, off, sz, link, info, al, es = struct.unpack_from("<10I", e, shoff + i * 40)
        secs.append((nm, ty, off, sz, link, info, es))
    shs = secs[shstrndx]
    def sname(o):
        s = e[shs[2] + o:]
        return s[:s.index(0)].decode()
    print("  ELF at 0x%X, %d bytes" % (p, len(e)))
    for i, s in enumerate(secs):
        print("   sec %d %-10s type %d off 0x%X size %d link %d info %d" % (i, sname(s[0]), s[1], s[2], s[3], s[4], s[5]))
    symtab = [s for s in secs if s[1] == 2]
    if symtab:
        st = symtab[0]
        strs = secs[st[4]]
        syms = []
        for k in range(st[3] // 16):
            nm, val, sz, inf, oth, shn = struct.unpack_from("<IIIBBH", e, st[2] + k * 16)
            s = e[strs[2] + nm:]
            syms.append((s[:s.index(0)].decode(errors='replace'), val, sz, inf, shn))
        for s in syms:
            print("   sym %-50s val 0x%X size %d info %d shn %d" % s)
    for s in secs:
        if s[1] == 9:
            n = s[3] // 8
            rel = [struct.unpack_from("<II", e, s[2] + k * 8) for k in range(n)]
            print("   rel %d entries for sec %d:" % (n, s[5]), " ".join("%X:%d/%d" % (o, i >> 8, i & 255) for o, i in rel[:40]))
    return e, secs

if __name__ == "__main__":
    b = open(sys.argv[1], "rb").read()
    out = []
    chunks(b, 0, len(b), 0, out)
    for d, cid, off, size in out:
        if cid in (0x00E34010, 0x00E34009):
            print("%08X at 0x%X size %d" % (cid, off, size))
            elf(b, off + 8, size)
