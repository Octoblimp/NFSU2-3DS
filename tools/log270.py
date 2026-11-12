"""Summarise world loading in an nfsu2_debug.txt (TASKS.md #270).

    python tools/log270.py [path-to-nfsu2_debug.txt]

Default path: Azahar's sdmc copy under %APPDATA%. Prints, in log order:

  * each timed load (world entry, race start, return to free roam): its
    `load: +N ms (step) name` marks and the total;
  * the section reads: count, KB, MB/s over the I/O time, idle, and the
    read-ahead's share (#270 P6: bytes read between jobs, used or not);
  * each load's wall time: median, p90, worst;
  * the streamer's lines: retarget, "does not fit", budget changes, drops;
  * every load that failed -- "MISSING or invalid" (written off) and "did not
    load" (retried) -- and every texture that did not allocate;
  * the holes: every `world: hole` that opened, how long it lasted, and the
    total -- the number #270 has to get to zero;
  * the lowest the two heaps went (`world: mem` lines);
  * the arena's compaction: each load that waited on moved blocks, the idle
    tidy runs, and every wanted section whose pools still did not fit.

The hole count alone is not the verdict. A section written off as missing
used to be left out of it (2026-10-25: eight A sections, files present, never
loaded, "0 holes"), so the failures are totalled on their own line, and a
hole still open at the end of the log is counted as one.
"""
import os
import re
import sys

RE_CLOCK = re.compile(r"^load: (.*) -- clock started")
RE_MARK = re.compile(r"^load: \+(\d+) ms \((\d+)\) (.*)")
RE_IO = re.compile(r"^world: io (\S+) hdr (\d+) ms (\d+) KB, geo (\d+) ms (\d+) KB, "
                   r"tex (\d+) ms (\d+) reads (\d+) KB, ([\d.]+) MB/s, idle (\d+) ms"
                   r"(?:, decode (\d+) ms (\d+) KB)?(?:, ahead (\d+) KB, ra (\d+) KB (\d+) ms)?")
RE_WALL = re.compile(r"^world: load (\S+) .*?\) (\d+) ms wall")
RE_HOLE = re.compile(r"^world: hole -- (\S+) visible (\d+) m away .*?, (\d+) km/h, loader (\w+) ?(\S*)"
                     r"(?:, it is (\w+))?")
RE_CLOSED = re.compile(r"^world: hole closed after (\d+) ms")
RE_DROP = re.compile(r"^world: drop (\S+)")
RE_MISSING = re.compile(r"^world: (\S+) MISSING or invalid")
RE_RETRY = re.compile(r"^world: (\S+) did not load \(([^,]+)")
RE_NOTEX = re.compile(r"^world: (\S+) has (\d+) texture\(s\) that did not allocate")
RE_MEM = re.compile(r"^world: mem\s+\d+ linear (\d+) KB free, (\d+) KB largest, heap (\d+) KB free, "
                    r"world (\d+) of (\d+) KB")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.environ.get("APPDATA", ""), "Azahar", "sdmc", "nfsu2_debug.txt")
    with open(path, "rb") as f:
        text = f.read().replace(b"\x00", b"").decode("utf-8", "replace")
    lines = text.splitlines()

    reads = []      # (name, kb, io_ms, idle_ms, dec_ms, out_kb, tex_reads, ahead_kb)
    walls = []
    ra_kb = ra_ms = 0       # the read-ahead's running totals, summed over world opens
    ra_last = (0, 0)
    holes = []      # (name, metres, kmh, loader, ms or None, state)
    missing = []
    retries = []
    notex = []
    mem_low = None  # (linear free, largest, heap free, world, budget) at the lowest linear free
    heap_low = None
    drops = 0
    compacts = []   # "compact for X" lines: a load that waited on moved blocks
    tidies = 0
    nofit = []
    for ln in lines:
        m = RE_CLOCK.match(ln)
        if m:
            print("\n== %s" % m.group(1))
            continue
        m = RE_MARK.match(ln)
        if m:
            print("  %6s ms  (+%5s)  %s" % (m.group(1), m.group(2), m.group(3)))
            continue
        m = RE_IO.match(ln)
        if m:
            g = m.groups()
            kb = int(g[2]) + int(g[4]) + int(g[7])
            io = int(g[1]) + int(g[3]) + int(g[5])
            dec = int(g[10]) if g[10] else 0
            out = int(g[11]) if g[11] else kb
            ahead = int(g[12]) if g[12] else 0
            reads.append((g[0], kb, io, int(g[9]), dec, out, int(g[6]), ahead))
            if g[13]:
                cur = (int(g[13]), int(g[14]))
                if cur[0] < ra_last[0]:     # a new world: its own running total
                    ra_kb += ra_last[0]
                    ra_ms += ra_last[1]
                ra_last = cur
            continue
        m = RE_WALL.match(ln)
        if m:
            walls.append(int(m.group(2)))
            continue
        m = RE_HOLE.match(ln)
        if m:
            holes.append([m.group(1), int(m.group(2)), int(m.group(3)),
                          m.group(4) + " " + m.group(5), None, m.group(6) or "?"])
            print("  HOLE  %s %s m away at %s km/h, loader %s %s, it is %s"
                  % (m.groups()[:5] + (m.group(6) or "?",)))
            continue
        m = RE_CLOSED.match(ln)
        if m and holes and holes[-1][4] is None:
            holes[-1][4] = int(m.group(1))
            print("  hole closed after %s ms" % m.group(1))
            continue
        if RE_DROP.match(ln):
            drops += 1
            continue
        m = RE_MISSING.match(ln)
        if m:
            missing.append(m.group(1))
            print("  FAILED  " + ln[:200])
            continue
        m = RE_RETRY.match(ln)
        if m:
            retries.append((m.group(1), m.group(2)))
            print("  RETRY  " + ln[:230])
            continue
        m = RE_NOTEX.match(ln)
        if m:
            notex.append(m.group(1))
            print("  NOTEX  " + ln[:200])
            continue
        m = RE_MEM.match(ln)
        if m:
            v = tuple(int(x) for x in m.groups())
            if mem_low is None or v[0] < mem_low[0]:
                mem_low = v
            if heap_low is None or v[2] < heap_low[2]:
                heap_low = v
            continue
        if ln.startswith("world: compact for"):
            compacts.append(ln)
            print("  " + ln[:200])
            continue
        if ln.startswith("world: tidy"):
            tidies += 1
            if tidies <= 12:
                print("  " + ln[:160])
            continue
        if "do not fit" in ln:
            nofit.append(ln.split()[1])
        if ln.startswith("world: retarget") or "does not fit" in ln or \
           "do not fit" in ln or ln.startswith("world: compaction") or \
           ln.startswith("world: arena") or \
           ln.startswith("world: budget") or "has all its textures" in ln or \
           ln.startswith("world: retail visibility") or "split-section part" in ln or \
           ln.startswith("world: in drivable section"):
            print("  " + ln[:160])

    print("\n== totals")
    ra_kb += ra_last[0]
    ra_ms += ra_last[1]
    if reads and ra_kb:
        used = sum(r[7] for r in reads)
        card = sum(r[1] - r[7] for r in reads) + ra_kb
        print("  read-ahead: %.1f MB in %.1f s outside the jobs, %.1f MB of it used; "
              "%.1f MB off the card in all, %.1f s with the jobs' I/O"
              % (ra_kb / 1024.0, ra_ms / 1000.0, used / 1024.0, card / 1024.0,
                 (sum(r[2] for r in reads) + ra_ms) / 1000.0))
    if walls:
        w = sorted(walls)
        print("  load wall time: median %d ms, p90 %d ms, worst %d ms (%d loads)"
              % (w[len(w) // 2], w[int(len(w) * 0.9)], w[-1], len(w)))
    if reads:
        kb = sum(r[1] for r in reads)
        io = sum(r[2] for r in reads)
        idle = sum(r[3] for r in reads)
        print("  %d section reads, %.1f MB, %.1f s of I/O (%.2f MB/s), %.1f s idle"
              % (len(reads), kb / 1024.0, io / 1000.0,
                 (kb / 1024.0) / (io / 1000.0) if io else 0.0, idle / 1000.0))
        dec = sum(r[4] for r in reads)
        out = sum(r[5] for r in reads)
        print("  %.1f MB made of them (%.2f MB/s of it), %.1f s of the I/O decoding, "
              "%d texture reads"
              % (out / 1024.0, (out / 1024.0) / (io / 1000.0) if io else 0.0,
                 dec / 1000.0, sum(r[6] for r in reads)))
        names = {}
        for r in reads:
            names[r[0]] = names.get(r[0], 0) + 1
        again = sorted(n for n, c in names.items() if c > 1)
        print("  read more than once: %s" % (", ".join(again) if again else "none"))
    print("  %d drops" % drops)
    print("  %d load(s) written off: %s" % (len(missing), ", ".join(missing) or "none"))
    print("  %d load(s) retried: %s" % (len(retries), ", ".join(
        "%s (%s)" % r for r in retries) or "none"))
    print("  %d section(s) short of textures: %s" % (len(notex), ", ".join(notex) or "none"))
    print("  %d wanted section(s) whose pools did not fit: %s" % (len(nofit), ", ".join(nofit) or "none"))
    print("  %d compaction(s) for a load, %d idle tidy run(s)" % (len(compacts), tidies))
    if mem_low:
        print("  lowest linear free %d KB (largest %d KB), heap %d KB, world %d of %d KB"
              % mem_low)
        print("  lowest heap free %d KB (linear %d KB free)" % (heap_low[2], heap_low[0]))
    closed = [h[4] for h in holes if h[4] is not None]
    still = [h[0] for h in holes if h[4] is None]
    print("  %d hole(s), %.1f s in all, worst %.1f s%s"
          % (len(holes), sum(closed) / 1000.0, max(closed) / 1000.0 if closed else 0.0,
             ("; STILL OPEN at the end: " + ", ".join(still)) if still else ""))


if __name__ == "__main__":
    main()
