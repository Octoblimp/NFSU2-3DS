# Draws one spot of an AI race from above (TASKS.md #280): the topology walls
# there, the race route, the AI line and its edges, and each car's path.
#
# Usage:
#   python tools/airace_plot.py trace.csv walls.txt out.png FROM TO [--lap M] [driver ...]
#
# trace.csv is `airace_sim.py <track> --trace trace.csv` (or the exe's 13th
# argument); walls.txt is the harness's `wallat` lines, printed with
# AIRACE_WALLS_AT="x,y,z,r" (one run per point -- each prints at most 96
# walls, so cover a big spot with several points and `sort -u`). FROM / TO are
# route metres; --lap folds a car's later laps onto the first. Naming drivers
# draws only those.
import collections
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

a = sys.argv[1:]
lap = 0.0
if "--lap" in a:
    i = a.index("--lap")
    lap = float(a[i + 1])
    del a[i:i + 2]
tr, wf, out, d0, d1 = a[0], a[1], a[2], float(a[3]), float(a[4])
cars = a[5:]

def inSpot(d):
    if lap > 0.0:
        d %= lap
    return d0 <= d <= d1

R, L, EL, ER = [], [], [], []
C = collections.defaultdict(list)
for l in open(tr):
    p = l.strip().split(",")
    if p[0] in ("R", "L", "EL", "ER"):
        d = float(p[1])
        if d0 - 40 <= d <= d1 + 40:
            {"R": R, "L": L, "EL": EL, "ER": ER}[p[0]].append((float(p[2]), float(p[3]), d))
    elif p[0] == "C" and inSpot(float(p[6])):
        # C,name,sec,x,y,z,dist,lateral,speed,wall,unstuck
        C[p[1]].append((float(p[3]), float(p[4]), float(p[2])))

fig, ax = plt.subplots(figsize=(14, 14))
for l in open(wf):
    m = re.search(r"\(([-\d.]+),([-\d.]+)\)-\(([-\d.]+),([-\d.]+)\)", l)
    if m:
        x0, y0, x1, y1 = map(float, m.groups())
        ax.plot([x0, x1], [y0, y1], "k-", lw=2)
for pts, sty in ((R, "c:"), (L, "b-"), (EL, "g--"), (ER, "g--")):
    if pts:
        ax.plot([p[0] for p in pts], [p[1] for p in pts], sty, lw=1.2)
for p in L:
    if int(p[2]) % 10 == 0:
        ax.annotate("%d" % p[2], (p[0], p[1]), fontsize=7, color="b")
cols = ["r", "m", "orange", "brown", "y", "k", "g"]
for n, (name, pts) in enumerate(C.items()):
    if cars and name not in cars:
        continue
    col = cols[n % len(cols)]
    seg = []
    for q in pts + [None]:
        if q is None or (seg and q[2] - seg[-1][2] > 1.0):   # a new pass
            if seg:
                ax.plot([s[0] for s in seg], [s[1] for s in seg], "-", color=col, lw=0.8,
                        alpha=0.7, label=name if q is None else None)
            seg = []
        if q is not None:
            seg.append(q)
ax.set_aspect("equal")
ax.legend()
xs = [p[0] for p in L if d0 <= p[2] <= d1]
ys = [p[1] for p in L if d0 <= p[2] <= d1]
if xs:
    ax.set_xlim(min(xs) - 30, max(xs) + 30)
    ax.set_ylim(min(ys) - 30, max(ys) + 30)
plt.savefig(out, dpi=80)
print("wrote", out)
