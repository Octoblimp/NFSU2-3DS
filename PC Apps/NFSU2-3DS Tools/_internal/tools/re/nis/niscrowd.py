"""Probe the End NIS crowd: the solids (0x80134000), skeletons (0x00E34009) and the scene's
entities (0x00037040) of a Scene_EndingNis0N_BundleB.bun. Scratch tool for TASKS.md #354."""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import car_pack  # noqa: E402


def top_chunks(b):
    out = []
    o = 0
    while o + 8 <= len(b):
        t, s = struct.unpack_from("<II", b, o)
        out.append((t, o + 8, s))
        o += 8 + s
    return out


def solids(b):
    res = []
    for t, p, s in top_chunks(b):
        if t != 0x80134000:
            continue
        sub = b[p - 8:p + s]
        parts = car_pack.parse_geometry(sub)
        res.append((p - 8, sub, parts))
    return res


if __name__ == "__main__":
    b = open(sys.argv[1], "rb").read()
    for t, p, s in top_chunks(b):
        if t:
            print("%08X @%d %d" % (t, p, s))
    for off, sub, parts in solids(b):
        print("solid @%d" % off)
        for pt in parts:
            m = pt["mesh"]
            vb = m["vb"][1]
            print("  %-28s hash %08X verts %d tris %d subsets %d vb %d stride %.2f tex %s" % (
                pt["name"], pt["hash"], m["numVerts"], m["numTris"], m["numSubsets"], vb,
                vb / max(1, m["numVerts"]), ["%08X" % x for x in pt["textures"]]))
        for tt, pp, ss in car_pack.walk_chunks(sub):
            if tt not in (0x80134000, 0x80134010, 0x134011, 0x134012, 0x134013, 0x80134100,
                          0x134900, 0x134b01, 0x134b02, 0x134b03, 0x134001, 0x134002, 0x134003,
                          0x134004, 0):
                print("    other chunk %08X size %d" % (tt, ss))
