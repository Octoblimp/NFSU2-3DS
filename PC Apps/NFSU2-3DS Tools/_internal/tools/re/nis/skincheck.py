"""Check the EAGL skin chain on host: still pose -> PoseSQTToGlobal -> x inverse bind ~ identity.
Row-vector matrices (translation in row 3); global = local @ parentGlobal. TASKS.md #354."""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import skel


def quat_mat(q, transpose=False):
    x, y, z, w = q
    m = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
                  [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
                  [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]])
    return m.T if transpose else m


def sqt(s, q, t, transpose=False):
    m = np.eye(4)
    m[:3, :3] = np.diag(s) @ quat_mat(q, transpose)
    m[3, :3] = t[:3]
    return m


def globals_(bones, pose=None, transpose=False):
    g = []
    for i, b in enumerate(bones):
        if pose is None:
            s, q, t = b['scale'], b['q'], b['t']
        else:
            p = pose[i * 12:i * 12 + 12]
            s, q, t = p[0:3], p[4:8], p[8:11]
        m = sqt(s, q, t, transpose)
        if b['parent'] >= 0:
            m = m @ g[b['parent']]
        g.append(m)
    return g


if __name__ == '__main__':
    b = open(sys.argv[1], 'rb').read()
    for off, nm, h, bones in skel.skeletons(b):
        for tr in (False, True):
            g = globals_(bones, None, tr)
            err = max(np.abs(gi @ bn['ib'] - np.eye(4)).max() for gi, bn in zip(g, bones))
            err2 = max(np.abs(bn['ib'] @ gi - np.eye(4)).max() for gi, bn in zip(g, bones))
            print(nm, 'transpose', tr, 'g@ib err %.5f  ib@g err %.5f' % (err, err2))
