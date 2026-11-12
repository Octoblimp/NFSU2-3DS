"""TASKS.md #184: is retail's collision (WORLD.NFST) solid where the XS_ signs stand?

For every placed XS_ instance in L4RA, the topology walls the runtime's
gather would find inside the instance's own footprint, in the car's body band
over the instance's floor. A control set (XW_ barriers / walls) runs the same.

    python tools/xs_topo_check.py   (reads PC Files and the Azahar SD's WORLD.NFST)
"""
import collections
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import track_pack as tp  # noqa: E402
import topo_probe as tpr  # noqa: E402

tdir = os.path.join(ROOT, "PC Files", "TRACKS")
secs = tp.read_section_table(os.path.join(tdir, "L4RA.BUN"))
stream = open(os.path.join(tdir, "STREAML4RA.BUN"), "rb")

topo = tpr.Topo(os.path.join(os.environ["APPDATA"], "Azahar", "sdmc", "nfsu2data", "TRACKS",
                             "L4RA", "WORLD.NFST"))
topo.set_groups(4000, None)

seen = set()
stats = collections.defaultdict(lambda: [0, 0])
examples = collections.defaultdict(list)
for sec in secs:
    data = tp.read_section(stream, sec)
    for infos, insts in tp.read_scenery_groups(data):
        for inst in insts:
            name = infos[inst["info"]]["name"]
            kind = "XS" if name.startswith("XS_") else "XW" if name.startswith("XW_") else None
            if not kind:
                continue
            b = inst["box"]
            key = (name, round(inst["pos"][0], 1), round(inst["pos"][1], 1))
            if key in seen:
                continue
            seen.add(key)
            cx, cy = (b[0] + b[3]) * 0.5, (b[1] + b[4]) * 0.5
            r = max(b[3] - b[0], b[4] - b[1]) * 0.5
            if r > 6.0 or r < 0.05:
                continue
            hits, ground = tpr.probe(topo, cx, cy, b[2], r, verbose=False)
            # Ground-standing only: the floor of the box within 1 m of the ground there.
            if not ground or abs(ground[0][0] - b[2]) > 1.0:
                continue
            st = stats[kind]
            st[0] += 1
            if hits:
                st[1] += 1
            if len(examples[kind]) < 6 and (hits or kind == "XS"):
                examples[kind].append((name, cx, cy, b[2], r, len(hits)))
for kind, (n, h) in stats.items():
    print("%s: %d ground-standing instances, %d with a topology wall in their footprint (%.0f%%)"
          % (kind, n, h, 100.0 * h / max(n, 1)))
for kind, ex in examples.items():
    for e in ex:
        print("  %s %-32s at %.1f,%.1f z %.1f r %.2f  walls %d" % ((kind,) + e))
