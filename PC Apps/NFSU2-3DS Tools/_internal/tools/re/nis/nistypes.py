import struct, sys, os, glob, collections
sys.path.insert(0, os.path.dirname(__file__))
from niself import chunks
from nisbank import load, u32, cstr

tally = collections.Counter()
where = collections.defaultdict(set)

def walk(d, m, path, depth, fname):
    ty = struct.unpack_from("<H", d, m)[0]
    tally[ty] += 1
    where[ty].add(os.path.basename(fname).replace('_BundleB.bun', ''))
    if ty == 0xF and depth < 6:
        n = struct.unpack_from("<H", d, m + 8)[0]
        for i in range(n):
            walk(d, u32(d, m + 0xC + 4 * i), path, depth + 1, fname)

for f in sorted(glob.glob(sys.argv[1] + "/*.bun")):
    b = open(f, "rb").read()
    out = []
    chunks(b, 0, len(b), 0, out)
    for dp, cid, off, size in out:
        if cid != 0x00E34010:
            continue
        try:
            d, syms = load(b, off + 8, size)
        except Exception as ex:
            print(f, hex(off), "load fail", ex); continue
        bk = syms.get('__AnimationBank:::bank')
        if bk is None:
            print(f, hex(off), "no bank symbol", list(syms)[:4]); continue
        n = u32(d, bk + 4)
        for i in range(n):
            walk(d, u32(d, u32(d, bk + 0x10) + 4 * i), cstr(d, u32(d, u32(d, bk + 0x14) + 4 * i)), 0, f)
for ty, c in sorted(tally.items()):
    print("type 0x%02X: %d  (%s)" % (ty, c, ", ".join(sorted(where[ty]))[:150]))
