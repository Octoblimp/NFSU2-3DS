"""Skin one End NIS crowd model on host with an anim frame and plot it (front + side), to check
the palette, the skeleton chain and the anim decode together. TASKS.md #354.
usage: posefig.py <bundleB.bun> <PART> <anim name> <frames comma list> <out.png>"""
import sys, os, struct
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import car_pack, niscrowd, skel, nisanim, skincheck  # noqa: E402

BIP = {'CROWD_FEMALE_01': 'Bip03', 'CROWD_FEMALE_02': 'Bip03'}


def load_part(b, name):
    for off, sub, parts in niscrowd.solids(b):
        for p in parts:
            if p['name'] == name:
                m = p['mesh']
                vs = car_pack.leaf_start(sub, m['vb'][0], m['vb'][1], 60)
                V = [struct.unpack_from('<6fI8f', sub, vs + i * 60) for i in range(m['numVerts'])]
                ss = car_pack.leaf_start(sub, m['subsets'][0], m['subsets'][1], 60)
                ist = car_pack.leaf_start(sub, m['ib'][0], m['ib'][1], 2)
                I = struct.unpack_from('<%dH' % (m['numTris'] * 3), sub, ist)
                subs = []
                for s in range(m['numSubsets']):
                    r = sub[ss + s * 60:ss + s * 60 + 60]
                    cnt = struct.unpack_from('<I', r, 0xC)[0]
                    first = struct.unpack_from('<I', r, 0x34)[0]
                    subs.append((first, cnt, r[0x20], list(r[0x21:0x21 + 19])))
                return V, I, subs
    raise KeyError(name)


def skin(V, I, subs, bones, g):
    out = {}
    for first, cnt, npal, pal in subs:
        for vi in set(I[first:first + cnt]):
            v = V[vi]
            p = np.array([v[0], v[1], v[2], 1.0])
            acc = np.zeros(4)
            for k in range(3):
                w = v[9 + k]
                if w <= 0:
                    continue
                bi = pal[int(v[12 + k])]
                acc += w * (p @ bones[bi]['ib'] @ g[bi])
            out[vi] = acc[:3]
    return out


if __name__ == '__main__':
    path, part, anim, frames, outp = sys.argv[1:6]
    b = open(path, 'rb').read()
    sks = {nm: bones for off, nm, h, bones in skel.skeletons(b)}
    bones = sks[BIP.get(part, 'Bip02')]
    V, I, subs = load_part(b, part)
    for s in subs:
        print('subset first %d cnt %d byte20 %d pal %s' % s)
    an = nisanim.banks(path)
    d, m = an[anim]
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fl = [float(x) for x in frames.split(',')]
    fig, ax = plt.subplots(2, len(fl) + 1, figsize=(3 * (len(fl) + 1), 6))
    cols = ['r', 'g', 'b']
    for c, t in enumerate([None] + fl):
        if t is None:
            g = skincheck.globals_(bones)
        else:
            pose = [0.0] * (12 * len(bones))
            for i, bn in enumerate(bones):
                pose[i * 12:i * 12 + 12] = list(bn['scale']) + [1.0] + list(bn['q']) + list(bn['t'][:3]) + [0.0]
            nisanim.eval_map(d, m, t, pose)
            g = skincheck.globals_(bones, pose)
        P = skin(V, I, subs, bones, g)
        for si, (first, cnt, npal, pal) in enumerate(subs):
            for tri in range(first // 3, (first + cnt) // 3):
                pts = np.array([P[I[tri * 3 + k]] for k in range(3)] + [P[I[tri * 3]]])
                ax[0, c].plot(pts[:, 0], pts[:, 2], cols[si % 3], lw=0.3)
                ax[1, c].plot(pts[:, 1], pts[:, 2], cols[si % 3], lw=0.3)
        for r in range(2):
            ax[r, c].set_aspect('equal')
        ax[0, c].set_title('still' if t is None else 'f%g' % t)
    fig.tight_layout()
    fig.savefig(outp, dpi=90)
