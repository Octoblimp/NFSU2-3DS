"""Host-side EAGL anim evaluator for the NIS banks (chunk 0x00E34010), checked against the
SpeedPS.elf decompiles (tools/re/nis notes in TASKS #343):

  0x00 RawPoseChannel   raw per-frame quat/translation (eval_rawpose; L4RB's drag trams)
  0x02 RawLinearChannel raw per-frame floats (eval_rawlinear)
  0x0F CompoundChannel  +2 fps byte, +8 u16 children, +0xA u16 frames, +0xC child maps
  0x0A DeltaLerpChan    +4 DeltaCompressedData, +8 u16 frames, +0xA u16 out float index[n]
  0x0B DeltaQuatChan    as 0x0A, groups of 4 floats (stride 4)
  0x11 DeltaQ           +4 u16 frames, +6 u8 bones, +7 u8 const bones, +8 u16 key times
                        (0 = one key per frame), +0xC bone index bytes, +0x10 u8 block bits,
                        +0x12 bones x {u16 min x,y,z; u16 range x,y,z}, then the blocks
  DeltaCompressedData: +0 u16 n, +2 u8 bits (4/8/16), +4 n x {f32 base, scale, initial},
                        then per-frame deltas: v += q*scale + base
Output: an SQT pose, 0x30 bytes (12 floats) per bone; the quaternion at +0x10.
"""
import struct, sys, os, math
sys.path.insert(0, os.path.dirname(__file__))
from niself import chunks
from nisbank import load, u32, cstr

F40 = 2.0 / 32767.0   # DAT_80345f40 6.1037e-05
F44 = 2.0 / 65535.0   # DAT_80345f44 3.0518e-05
F58 = 1.0 / 127.0
F5C = 1.0 / 255.0


def u16(d, o): return struct.unpack_from("<H", d, o)[0]
def f32(d, o): return struct.unpack_from("<f", d, o)[0]


def dcd_values(d, p, frame):
    """DecompressValues: the n values after `frame` deltas."""
    n = u16(d, p)
    bits = d[p + 2]
    tab = [struct.unpack_from("<3f", d, p + 4 + 12 * i) for i in range(n)]
    v = [t[2] for t in tab]
    s = p + 4 + 12 * n
    for f in range(frame):
        v = step(d, s, f, n, bits, tab, v, 1.0)
    return v, s, n, bits, tab


def deltas(d, s, f, n, bits):
    if bits == 8:
        return list(d[s + f * n: s + f * n + n])
    if bits == 16:
        return list(struct.unpack_from("<%dH" % n, d, s + f * n * 2))
    if bits == 4:
        per = (n + 1) >> 1
        out = []
        for i in range(n):
            b = d[s + f * per + (i >> 1)]
            out.append(b & 0xF if i & 1 == 0 else b >> 4)
        return out
    raise ValueError("bits %d" % bits)


def step(d, s, f, n, bits, tab, v, frac):
    q = deltas(d, s, f, n, bits)
    return [v[i] + (q[i] * tab[i][1] + tab[i][0]) * frac for i in range(n)]


def eval_deltachan(d, m, t, out, stride):
    frames = u16(d, m + 8)
    p = u32(d, m + 4)
    fr = int(t) if t >= 0 else 0
    fr = min(fr, frames - 1)
    v, s, n, bits, tab = dcd_values(d, p, fr)
    idx = [u16(d, m + 0xA + 2 * i) for i in range(n // stride)]
    if t != int(t) and int(t) + 1 < frames:
        v = step(d, s, fr, n, bits, tab, v, t - int(t))
    for g, ix in enumerate(idx):
        for k in range(stride):
            out[ix + k] = v[g * stride + k]


def quat_from(x, y, sign_src):
    pass


def eval_deltaq(d, m, t, pose):
    frames = u16(d, m + 4)
    nb = d[m + 6]
    nconst = d[m + 7]
    times = u32(d, m + 8)
    bones = u32(d, m + 0xC)
    bb = d[m + 0x10]
    bs = 1 << bb
    tabo = m + 0x12
    blocks = tabo + nb * 12
    bsize = (nb * ((bs - 1) * 3 + 6) + 1) & ~1
    ti = int(t)
    if times == 0:
        key = 0 if t < 0 else min(ti, frames - 1)
    else:
        tl = [u16(d, times + 2 * i) for i in range(frames - 1)]
        if ti < tl[0]:
            key = 0
        else:
            k = 0
            while k + 1 < len(tl) and tl[k + 1] <= ti:
                k += 1
            key = k + 1

    def decode_abs(o):
        x0, y0, z0 = struct.unpack_from("<3H", d, o)
        x = (x0 >> 1) * F40 - 1.0
        y = y0 * F44 - 1.0
        z = z0 * F44 - 1.0
        return [x, y, z], x0 & 1

    def fin(q3, sgn):
        l2 = q3[0] ** 2 + q3[1] ** 2 + q3[2] ** 2
        if l2 <= 1.0:
            w = math.sqrt(1.0 - l2)
            if sgn:
                w = -w
            return [q3[0], q3[1], q3[2], w]
        l = math.sqrt(l2)
        return [q3[0] / l, q3[1] / l, q3[2] / l, 0.0]

    def add_delta(q3, bone, o, sgn_scale=1.0):
        mn = struct.unpack_from("<6H", d, tabo + bone * 12)
        b0, b1, b2 = d[o], d[o + 1], d[o + 2]
        q3[0] += mn[3] * F44 * F58 * (b0 >> 1) + (mn[0] * F44 - 1.0)
        q3[1] += mn[4] * F44 * F5C * b1 + (mn[1] * F44 - 1.0)
        q3[2] += mn[5] * F44 * F5C * b2 + (mn[2] * F44 - 1.0)
        return b0 & 1

    def key_quats(k):
        blk = k >> bb
        inb = k & (bs - 1)
        bo = blocks + blk * bsize
        qs = []
        for b in range(nb):
            q3, sg = decode_abs(bo + 6 * b)
            qs.append([q3, sg])
        for j in range(inb):
            for b in range(nb):
                sg = add_delta(qs[b][0], b, bo + nb * 6 + j * nb * 3 + 3 * b)
                qs[b][1] = sg
        return [fin(q3, sg) for q3, sg in qs]

    cur = key_quats(key)
    frac = None
    if times == 0:
        if t != ti:
            frac = t - ti
    else:
        tl = [u16(d, times + 2 * i) for i in range(frames - 1)]
        if key == 0:
            if t != 0.0:
                frac = t / tl[0]
        else:
            t0 = tl[key - 1]
            if t != t0 and key < len(tl):
                frac = (t - t0) / (tl[key] - t0)
    if frac is not None and key < frames - 1:
        nxt = key_quats(key + 1)
        res = []
        for a, b in zip(cur, nxt):
            q = [a[i] + frac * (b[i] - a[i]) for i in range(4)]
            l = math.sqrt(sum(x * x for x in q))
            res.append([x / l for x in q])
        cur = res
    for b in range(nb):
        bi = d[bones + b]
        pose[bi * 12 + 4: bi * 12 + 8] = cur[b]
    # constant bones
    if nconst:
        cb = const_bone_idx(d, m)
        cp = (cb + nconst + 1) & ~1
        for i in range(nconst):
            q3, sg = decode_abs(cp + 6 * i)
            bi = d[cb + i]
            pose[bi * 12 + 4: bi * 12 + 8] = fin(q3, sg)


def key_and_frac(d, times, frames, t):
    """The shared EAGL key search: times == 0 is one key a frame, else u16 times[frames-1]
    where key k >= 1 starts at times[k-1]."""
    ti = int(t)
    if times == 0:
        key = 0 if t < 0 else min(ti, frames - 1)
        frac = (t - ti) if (t != ti and key < frames - 1) else None
        return key, frac
    tl = [u16(d, times + 2 * i) for i in range(frames - 1)]
    if ti < tl[0]:
        key = 0
    else:
        k = 0
        while k + 1 < len(tl) and tl[k + 1] <= ti:
            k += 1
        key = k + 1
    frac = None
    if key == 0:
        if t != 0.0:
            frac = t / tl[0]
    else:
        t0 = tl[key - 1]
        if t != t0 and key < len(tl):
            frac = (t - t0) / (tl[key] - t0)
    if key >= frames - 1:
        frac = None
    return key, frac


def eval_deltaf(d, m, t, pose, dim):
    """FnDeltaF3 (0x14, dim 3) / FnDeltaF1 (0x15, dim 1), EvalSQT 0x802AD160 / 0x802B1EA0."""
    outidx = u32(d, m + 4)
    times = u32(d, m + 8)
    frames = u16(d, m + 0xC)
    n = u16(d, m + 0xE)
    bb = d[m + 0x10]
    nconst = d[m + 0x11]
    bs = 1 << bb
    tw = 0x24 if dim == 3 else 0xC
    tab = []
    for i in range(n):
        o = m + 0x14 + i * tw
        base = struct.unpack_from("<%df" % dim, d, o)
        scale = struct.unpack_from("<%df" % dim, d, o + 4 * dim)
        dmin = struct.unpack_from("<%dH" % dim, d, o + 8 * dim)
        drng = struct.unpack_from("<%dH" % dim, d, o + 10 * dim)
        tab.append((base, scale,
                    [2 * dmin[k] / 65535.0 * scale[k] - scale[k] for k in range(dim)],
                    [2 * drng[k] / 65535.0 * scale[k] / 255.0 for k in range(dim)]))
    blocks = m + 0x14 + n * tw
    bsize = (n * 2 * dim + (bs - 1) * n * dim + 1) & ~1

    def keyvals(k):
        bo = blocks + (k >> bb) * bsize
        v = []
        for i in range(n):
            a = struct.unpack_from("<%dH" % dim, d, bo + i * 2 * dim)
            v.append([a[c] * tab[i][1][c] / 65535.0 + tab[i][0][c] for c in range(dim)])
        for j in range(k & (bs - 1)):
            o = bo + n * 2 * dim + j * n * dim
            for i in range(n):
                for c in range(dim):
                    v[i][c] += d[o + i * dim + c] * tab[i][3][c] + tab[i][2][c]
        return v

    if n:
        key, frac = key_and_frac(d, times, frames, t)
        cur = keyvals(key)
        if frac is not None:
            nx = keyvals(key + 1)
            cur = [[a[c] + (b[c] - a[c]) * frac for c in range(dim)] for a, b in zip(cur, nx)]
        for i in range(n):
            ix = u16(d, outidx + 2 * i)
            pose[ix:ix + dim] = cur[i]
    if nconst:
        rem = frames & (bs - 1)
        o = blocks + bsize * (frames >> bb)
        if rem:
            o = (o + (rem - 1) * n * dim + n * 2 * dim + 1) & ~1
        if n == 0:
            o = (o + 1) & ~1
        fo = (o + nconst * 2 + 3) & ~3
        for i in range(nconst):
            ix = u16(d, o + 2 * i)
            pose[ix:ix + dim] = struct.unpack_from("<%df" % dim, d, fo + 4 * dim * i)


def const_bone_idx(d, m):
    bs = 1 << d[m + 0x10]
    nb = d[m + 6]
    frames = u16(d, m + 4)
    nblk = frames // bs
    rem = frames - nblk * bs
    o = m + 0x12 + nb * 12 + ((nb * ((bs - 1) * 3 + 6) + 1) & ~1) * nblk
    if rem >= 1:
        o += nb * ((rem - 1) * 3 + 6)
    return o


def _frame_pair(t, frames):
    """RawPoseChannel::Eval / FnRawLinearChannel::Eval's frame choice: before 0
    frame 0, past the last but one the last, else frame k lerped to k + 1 by
    the fraction (None when it is 0)."""
    k = int(t)
    if t < 0:
        return 0, None
    if k < frames - 1:
        f = t - k
        return k, (f if f != 0.0 else None)
    return frames - 1, None


def eval_rawpose(d, m, t, pose):
    """FnRawPoseChannel (0x00): RawPoseChannel::Eval 0x802BA25C / EvalFrame
    0x802BA4DC. +4 u32 op words, +8 u32 floats a frame, +0xC u32 frames; at
    +0x10 the op list (per bone a count and that many op ids: 0 QuatF4, 1
    EulF3, 2 TranF3), again at +0x10 + 4n (InitAnimMemoryMap 0x802BA0D4 turns
    the second into the Interp functions), then the frames. Bone b's quaternion
    goes to pose +0x30b + 0x10, its translation to +0x20. QuatF4Interp
    (0x80314E8C) lerps to the nearer sign of the next key (dot <= 0 flips it)
    and normalises; TranF3Interp (0x80315070) is a plain lerp."""
    n, fpf, frames = u32(d, m + 4), u32(d, m + 8), u32(d, m + 0xC)
    ops = [u32(d, m + 0x10 + 4 * i) for i in range(n)]
    data = m + 0x10 + 8 * n
    k, frac = _frame_pair(t, frames)
    a = data + 4 * k * fpf
    b = a + 4 * fpf
    i = bone = 0
    while i < n:
        cnt = ops[i]
        i += 1
        base = 12 * bone
        for _ in range(cnt):
            op = ops[i]
            i += 1
            if op == 0:
                q0 = struct.unpack_from("<4f", d, a)
                q1 = struct.unpack_from("<4f", d, b) if frac is not None else q0
                if frac is not None:
                    if sum(x * y for x, y in zip(q0, q1)) <= 0.0:
                        q = [x - frac * (y + x) for x, y in zip(q0, q1)]
                    else:
                        q = [x + frac * (y - x) for x, y in zip(q0, q1)]
                    r = 1.0 / math.sqrt(sum(x * x for x in q))
                    q = [x * r for x in q]
                else:
                    q = list(q0)
                pose[base + 4:base + 8] = q
                a += 16
                b += 16
            elif op == 2:
                v0 = struct.unpack_from("<3f", d, a)
                v1 = struct.unpack_from("<3f", d, b) if frac is not None else v0
                f = frac or 0.0
                pose[base + 8:base + 11] = [x + f * (y - x) for x, y in zip(v0, v1)]
                a += 12
                b += 12
            else:
                raise ValueError("raw pose op %d (EulF3) not ported" % op)
        bone += 1


def eval_rawlinear(d, m, t, pose):
    """FnRawLinearChannel (0x02), Eval 0x8031394C: +4 u16 floats a frame, +6
    u16 frames, +8 u16 pose float index each, padded to 4, then the frames;
    a plain lerp (the Fn's +0x10 lerp flag is 1 from NewFnAnim)."""
    n, frames = u16(d, m + 4), u16(d, m + 6)
    idx = [u16(d, m + 8 + 2 * i) for i in range(n)]
    data = m + 8 + (((n + 1) * 2) & ~3)
    k, frac = _frame_pair(t, frames)
    v0 = struct.unpack_from("<%df" % n, d, data + 4 * k * n)
    v1 = struct.unpack_from("<%df" % n, d, data + 4 * (k + 1) * n) if frac is not None else v0
    f = frac or 0.0
    for j, ix in enumerate(idx):
        pose[ix] = v0[j] + f * (v1[j] - v0[j])


def eval_map(d, m, t, pose, depth=0):
    ty = u16(d, m)
    if ty == 0xF:
        n = u16(d, m + 8)
        for i in reversed(range(n)):
            eval_map(d, u32(d, m + 0xC + 4 * i), t, pose, depth + 1)
    elif ty == 0x00:
        eval_rawpose(d, m, t, pose)
    elif ty == 0x02:
        eval_rawlinear(d, m, t, pose)
    elif ty == 0x0A:
        eval_deltachan(d, m, t, pose, 1)
    elif ty == 0x0B:
        eval_deltachan(d, m, t, pose, 4)
    elif ty == 0x11:
        eval_deltaq(d, m, t, pose)
    elif ty == 0x14:
        eval_deltaf(d, m, t, pose, 3)
    elif ty == 0x15:
        eval_deltaf(d, m, t, pose, 1)
    else:
        raise ValueError("type 0x%X" % ty)


def describe(d, m, ind="  "):
    ty = u16(d, m)
    if ty == 0xF:
        print("%scompound fps %d children %d frames %d" % (ind, d[m + 2], u16(d, m + 8), u16(d, m + 0xA)))
        for i in range(u16(d, m + 8)):
            describe(d, u32(d, m + 0xC + 4 * i), ind + "  ")
    elif ty in (0xA, 0xB):
        p = u32(d, m + 4)
        n = u16(d, p)
        st = 1 if ty == 0xA else 4
        print("%s%s frames %d n %d bits %d idx %s" % (ind, "lerp" if ty == 0xA else "quat", u16(d, m + 8), n,
              d[p + 2], [u16(d, m + 0xA + 2 * i) for i in range(n // st)]))
    elif ty == 0x11:
        print("%sdeltaQ frames %d bones %d const %d times %s bonesidx %s bits %d" % (
            ind, u16(d, m + 4), d[m + 6], d[m + 7], hex(u32(d, m + 8)),
            list(d[u32(d, m + 0xC):u32(d, m + 0xC) + d[m + 6]]), d[m + 0x10]))
    elif ty in (0x14, 0x15):
        n = u16(d, m + 0xE)
        print("%sdeltaF%d frames %d n %d const %d bits %d times %s idx %s" % (ind, 3 if ty == 0x14 else 1,
              u16(d, m + 0xC), n, d[m + 0x11], d[m + 0x10], hex(u32(d, m + 8)),
              [u16(d, u32(d, m + 4) + 2 * i) for i in range(n)]))
    else:
        print("%stype 0x%X" % (ind, ty))


def banks(path):
    b = open(path, "rb").read()
    out = []
    chunks(b, 0, len(b), 0, out)
    res = {}
    for dp, cid, off, size in out:
        if cid == 0x00E34010:
            d, syms = load(b, off + 8, size)
            bk = syms['__AnimationBank:::bank']
            for i in range(u32(d, bk + 4)):
                res[cstr(d, u32(d, u32(d, bk + 0x14) + 4 * i))] = (d, u32(d, u32(d, bk + 0x10) + 4 * i))
    return res


if __name__ == "__main__":
    an = banks(sys.argv[1])
    step_ = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0
    for name, (d, m) in an.items():
        if len(sys.argv) > 2 and sys.argv[2] not in name:
            continue
        print(name)
        describe(d, m)
        frames = u16(d, m + 0xA)
        t = 0.0
        while t <= frames:
            pose = [0.0] * (12 * 32)
            eval_map(d, m, t, pose)
            print("   t=%6.1f  b0 s/q %s  t %s" % (t, ["%.3f" % v for v in pose[16:20]], ["%.2f" % v for v in pose[20:24]]))
            t += step_
