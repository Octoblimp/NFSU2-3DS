"""The End NIS scene's character entities, chunk 0x00037040 (CBasicCharacterAnimEntity, type 1).
After 8 bytes of 0x11 filler: +0 type, +4 entity hash, +8 ?, +0xC ?, +0x10 placement matrix
(row-major, translation in row 3), +0x50 f32, +0x54..+0x5C three model hashes, +0x60..+0x6C 0,
+0x70 anim hash, +0x74 skeleton hash, +0x78 f32, +0x84 start frame?. Scratch tool, TASKS #354."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from niself import chunks
import nisanim


def h(s):
    x = 0xFFFFFFFF
    for c in s.encode():
        x = (x * 33 + c) & 0xFFFFFFFF
    return x


def entities(b):
    out = []
    chunks(b, 0, len(b), 0, out)
    res = []
    for dp, cid, off, size in out:
        if cid != 0x37040:
            continue
        p = off + 8
        while b[p] == 0x11:
            p += 1
        res.append(b[p:off + 8 + size])
    return res


if __name__ == '__main__':
    path = sys.argv[1]
    b = open(path, 'rb').read()
    names = {}
    for n in ['CROWD_FEMALE_01', 'CROWD_FEMALE_02', 'CROWD_MALE_01', 'CROWD_MALE_02', 'LISHA', 'NICK',
              'Bip01', 'Bip02', 'Bip03']:
        names[h(n)] = n
    for a in nisanim.banks(path):
        names[h(a)] = a
        if a.endswith('_s'):
            names[h(a[:-2])] = a[:-2] + '(-s)'
    for e in entities(b):
        w = struct.unpack_from('<4I', e, 0)
        m = struct.unpack_from('<16f', e, 0x10)
        rest = struct.unpack_from('<%dI' % ((len(e) - 0x50) // 4), e, 0x50)
        print('len %d type %d hash %08X %08X %08X' % (len(e), w[0], w[1], w[2], w[3]))
        print('   rows', [['%.3f' % x for x in m[r * 4:r * 4 + 4]] for r in range(4)])
        print('   ', ' '.join(('%08X' % x) + ('(' + names[x] + ')' if x in names else '') for x in rest))
