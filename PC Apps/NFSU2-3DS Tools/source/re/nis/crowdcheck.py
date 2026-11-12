"""Read a .crowd (tools/crowd_pack.py) back the way crowd.c does and skin the whole
scene at one time: every entity placed by its matrix, every car by its track.
Plots a top view and a side view. TASKS.md #354.
usage: crowdcheck.py <file.crowd> <scene seconds> <out.png>"""
import struct, sys
import numpy as np


class R:
    def __init__(self, b):
        self.b, self.o = b, 0

    def u(self, fmt):
        v = struct.unpack_from("<" + fmt, self.b, self.o)
        self.o += struct.calcsize("<" + fmt)
        return v

    def align(self):
        self.o = (self.o + 3) & ~3


def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
                     [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
                     [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]])


def qt(q, t):
    m = np.eye(4)
    m[:3, :3] = qmat(q)
    m[3, :3] = t
    return m


def load(path):
    r = R(open(path, "rb").read())
    magic, ver, nm, ns, na, ne, nc = r.u("4sIIIIII")
    assert magic == b"CRWD" and ver in (1, 2)
    models = []
    for _ in range(nm):
        h, sk, nv, nb = r.u("IIII")
        nf = r.b[r.o:r.o + nb]
        r.o += nb
        r.align()
        skin = [r.u("BBBB3f") for _ in range(nv)]
        hdr = struct.unpack_from("<4sIIIII", nf, 0)
        ntex, nbat, nvert, nidx = hdr[2:6]
        vo = 0x40 + ntex * 16 + nbat * 16
        V = [struct.unpack_from("<8f", nf, vo + i * 32) for i in range(nvert)]
        io = vo + nvert * 32
        I = struct.unpack_from("<%dH" % nidx, nf, io)
        models.append(dict(hash=h, skel=sk, V=V, I=I, skin=skin))
    skels = []
    for _ in range(ns):
        h, n = r.u("II")
        bones = []
        for _ in range(n):
            par, = r.u("i")
            ib = np.array(r.u("16f")).reshape(4, 4)
            q = r.u("4f")
            t = r.u("3f")
            bones.append(dict(parent=par, ib=ib, q=q, t=t))
        skels.append(bones)
    anims = []
    for _ in range(na):
        h, nb, nk = r.u("III")
        keys = [[r.u("7f") for _ in range(nb)] for _ in range(nk)]
        anims.append(keys)
    ents = []
    for _ in range(ne):
        h, mi, si, ai, fl = r.u("IiiiI")
        m = r.u("12f")
        sp, dl = r.u("ff")
        ls = r.u("I")[0] if ver >= 2 else 0
        le, = r.u("I")
        M = np.eye(4)
        for row in range(4):
            M[row, :3] = m[row * 3:row * 3 + 3]
        ents.append(dict(model=mi, skel=si, anim=ai, flags=fl, M=M, speed=sp, delay=dl, loopStart=ls, loopEnd=le))
    cars = []
    for _ in range(nc):
        k, = r.u("I")
        cars.append([r.u("7f") for _ in range(k)])
    n, = r.u("I")
    r.o += n
    r.align()
    assert r.o == len(r.b), (r.o, len(r.b))
    return models, skels, anims, ents, cars


def sample(keys, f):
    """Key f (fractional): lerp t, nlerp q (shortest arc)."""
    n = len(keys)
    f = max(0.0, min(f, n - 1))
    k0 = int(f)
    k1 = min(k0 + 1, n - 1)
    a = f - k0
    out = []
    for b0, b1 in zip(keys[k0], keys[k1]):
        q0, q1 = np.array(b0[:4]), np.array(b1[:4])
        if q0 @ q1 < 0:
            q1 = -q1
        q = q0 * (1 - a) + q1 * a
        q /= np.linalg.norm(q)
        t = np.array(b0[4:]) * (1 - a) + np.array(b1[4:]) * a
        out.append((q, t))
    return out


def anim_frame(e, keys, t):
    """CAnimCtrl (AdvanceAnimTime): t x 30 x speed x 0.5 anim frames, once to
    the range's end, then looped in the range (flags & 0x20) or held. The range
    is the keys, or with flags & 0x40 SetLoopRange's (crowd.c, #422)."""
    f = max(0.0, t - e["delay"]) * 30.0 * e["speed"] * 0.5
    last = len(keys) - 1
    lo, hi = 0.0, float(last)
    if e["flags"] & 0x40:
        hi = min(e["loopEnd"] * e["speed"] * 0.5, last)
        lo = min(e["loopStart"] * e["speed"] * 0.5, hi)
    if f > hi:
        f = lo + (f - lo) % (hi - lo) if (e["flags"] & 0x20) and hi > lo else hi
    return min(f, last)


def skin_entity(models, skels, anims, e, t):
    m = models[e["model"]]
    bones = skels[e["skel"]]
    if e["anim"] >= 0:
        keys = anims[e["anim"]]
        loc = sample(keys, anim_frame(e, keys, t))
    else:
        loc = [(np.array(b["q"]), np.array(b["t"])) for b in bones]
    g = []
    for i, b in enumerate(bones):
        L = qt(*loc[i])
        if b["parent"] >= 0:
            L = L @ g[b["parent"]]
        g.append(L)
    S = [b["ib"] @ gi for b, gi in zip(bones, g)]
    P = []
    for v, sk in zip(m["V"], m["skin"]):
        p = np.array([v[0], v[1], v[2], 1.0])
        acc = np.zeros(4)
        for k in range(3):
            if sk[4 + k] > 0:
                acc += sk[4 + k] * (p @ S[sk[k]])
        P.append((acc @ e["M"])[:3])
    return np.array(P), m["I"]


if __name__ == "__main__":
    path, t, outp = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    models, skels, anims, ents, cars = load(path)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(1, 2, figsize=(14, 7))
    for ei, e in enumerate(ents):
        if e["model"] < 0:
            continue
        P, I = skin_entity(models, skels, anims, e, t)
        for tri in range(len(I) // 3):
            pts = P[[I[tri * 3], I[tri * 3 + 1], I[tri * 3 + 2], I[tri * 3]]]
            ax[0].plot(pts[:, 0], pts[:, 1], lw=0.2, color="C%d" % (ei % 10))
            ax[1].plot(pts[:, 0], pts[:, 2], lw=0.2, color="C%d" % (ei % 10))
    for ci, tr in enumerate(cars):
        if not tr:
            continue
        f = min(t * 30.0 * 0.5, len(tr) - 1)
        k = tr[int(f)]
        R3 = qmat(k[:4])
        c = np.array(k[4:])
        box = np.array([[2.2, 0.9], [2.2, -0.9], [-2.2, -0.9], [-2.2, 0.9], [2.2, 0.9]])
        pts = np.array([c + np.array([x, y, 0]) @ R3 for x, y in box])
        ax[0].plot(pts[:, 0], pts[:, 1], "k-", lw=1.2)
        ax[0].text(c[0], c[1], "car%d" % (ci + 1))
        ax[0].plot([c[0], c[0] + 2.5 * R3[0, 0]], [c[1], c[1] + 2.5 * R3[0, 1]], "r-")
    for a in ax:
        a.set_aspect("equal")
    ax[0].set_title("top, t=%.1f s" % t)
    ax[1].set_title("side (x, z)")
    fig.tight_layout()
    fig.savefig(outp, dpi=80)
    print("models %d skels %d anims %d ents %d cars %s" % (
        len(models), len(skels), len(anims), len(ents), [len(c) for c in cars]))
