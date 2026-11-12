"""EAGL skeleton chunk 0x00E34009 (an ELF like the anim banks): `__Skeleton:::<name>` points at
+8 u32 bone count, then from +0x10 one 0x70-byte bone each: scale[3], parent (s32, -1 root),
quat[4], translation[4], inverse bind matrix[16] (row-major, translation in the last row).
`__Bone:::<skel>.<bone>` symbols name each bone index. Scratch tool for TASKS.md #354."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from niself import chunks
from nisbank import load, u32
import numpy as np


def skeletons(b):
    out = []
    chunks(b, 0, len(b), 0, out)
    res = []
    for dp, cid, off, size in out:
        if cid != 0x00E34009:
            continue
        d, syms = load(b, off + 8, size)
        sk = [v for k, v in syms.items() if k.startswith('__Skeleton:::')][0]
        skname = [k for k in syms if k.startswith('__Skeleton:::')][0][13:]
        names = {}
        for k, v in syms.items():
            if k.startswith('__Bone:::'):
                names[u32(d, v)] = k.split('.', 1)[1]
        n = u32(d, sk + 8)
        bones = []
        for i in range(n):
            o = sk + 0x10 + i * 0x70
            sc = struct.unpack_from('<3f', d, o)
            par = struct.unpack_from('<i', d, o + 0xC)[0]
            q = struct.unpack_from('<4f', d, o + 0x10)
            t = struct.unpack_from('<4f', d, o + 0x20)
            ib = np.array(struct.unpack_from('<16f', d, o + 0x30)).reshape(4, 4)
            bones.append(dict(name=names.get(i, '?'), scale=sc, parent=par, q=q, t=t, ib=ib))
        res.append((off, skname, struct.unpack_from('<I', d, sk)[0], bones))
    return res


if __name__ == '__main__':
    b = open(sys.argv[1], 'rb').read()
    for off, nm, h, bones in skeletons(b):
        print('skeleton %s @%d hash/hdr %08X bones %d' % (nm, off, h, len(bones)))
        for i, bn in enumerate(bones):
            w = np.linalg.inv(bn['ib'])
            print('  %2d %-18s par %2d q %s t %s bindpos %s' % (i, bn['name'], bn['parent'],
                  ['%.3f' % x for x in bn['q']], ['%.3f' % x for x in bn['t'][:3]], ['%.3f' % x for x in w[3, :3]]))
