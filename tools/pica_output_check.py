#!/usr/bin/env python3
"""Checks that every vertex shader writes each of its outputs once (TASKS.md #327).

The PICA200 freezes on a component of an output register it was told about
that a vertex leaves unwritten or writes twice (3dbrew's GPU/Pitfalls). Azahar
does not: scenehl.v.pica drew there and hung the 3DS's first world frame with
outtc0.w and outtc2.zw never written. So every component of every `.out`
register, including the ones its semantics leave unused, must be written
exactly once on every path through main.

The walk follows picasso's structured flow: `call` takes the procedure's
writes, an `ifu`/`ifc` block's branches must agree, and a write inside a `for`
block or a conditional call is an error. A jump or break stops the check of
that file with a warning, since the path can't be followed from the source.

usage:
    python tools/pica_output_check.py                 # every 3DS Files/source/*.v.pica
    python tools/pica_output_check.py a.v.pica ...    # just these

Exit status 1 when any shader breaks the rule; tools/build_cia.ps1 runs it
before make.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "3DS Files", "source")
COMPS = "xyzw"
MASK_LETTERS = {"x": "x", "y": "y", "z": "z", "w": "w",
                "r": "x", "g": "y", "b": "z", "a": "w",
                "s": "x", "t": "y", "p": "z", "q": "w"}
OPENERS = ("ifu", "ifc", "for")
JUMPS = ("jmpc", "jmpu", "breakc", "break")


class Unfollowable(Exception):
    pass


class ShaderError(Exception):
    pass


def parse(path):
    """The shader's output registers, its procedures' statement lists and aliases."""
    outs, aliases, procs = [], {}, {}
    cur = None      # the procedure being read: its block stack
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, raw in enumerate(f, 1):
            line = raw.split(";", 1)[0].strip()
            if not line:
                continue
            parts = line.replace(",", " ").split()
            op = parts[0].lower()
            if cur is None:
                if op == ".out" and len(parts) >= 3 and parts[1] != "-":
                    outs.append(parts[1])
                elif op == ".alias" and len(parts) >= 3:
                    aliases[parts[1]] = parts[2]
                elif op == ".proc" and len(parts) >= 2:
                    cur = [("proc", parts[1], [])]
                elif op == ".entry" and len(parts) >= 2:
                    procs[".entry"] = parts[1]   # #352: the shared blob's mains
                continue
            body = cur[-1][2]
            if op in OPENERS:
                block = (op, [], [])     # then-branch, else-branch
                body.append((n, "block", block))
                cur.append((op, block, block[1]))
            elif op == ".else":
                if len(cur) < 2 or cur[-1][0] not in ("ifu", "ifc"):
                    raise SystemExit(f"{path}:{n}: .else outside an if")
                cur[-1] = (cur[-1][0], cur[-1][1], cur[-1][1][2])
            elif op == ".end":
                kind = cur.pop()
                if kind[0] == "proc":
                    procs[kind[1]] = kind[2]
                    cur = None
            else:
                body.append((n, "op", parts))
    return outs, aliases, procs


def dest_writes(parts, outs, aliases):
    """The output components an instruction writes, as ('outtc0', 'x') pairs."""
    if len(parts) < 2:
        return []
    dst = parts[1]
    m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)(?:\.([a-z]+))?$", dst)
    if not m:
        return []
    name, mask = m.group(1), m.group(2)
    if name in aliases and name not in outs:
        t = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)(?:\.([a-z]+))?$", aliases[name])
        if not t:
            return []
        name, mask = t.group(1), mask or t.group(2)
    if name not in outs:
        return []
    comps = [MASK_LETTERS.get(c) for c in (mask or COMPS)]
    return [(name, c) for c in comps if c]


def walk(path, stmts, outs, aliases, procs, depth=0):
    """The writes along this statement list, each component counted."""
    counts = {}

    def add(more):
        for k, v in more.items():
            counts[k] = counts.get(k, 0) + v

    for n, kind, item in stmts:
        if kind == "op":
            op = item[0].lower()
            if op in JUMPS:
                raise Unfollowable(f"{path}:{n}: '{op}' -- the paths can't be followed")
            if op == "end":
                return counts, True
            if op in ("call", "callc", "callu"):
                target = item[-1]
                if target not in procs:
                    raise Unfollowable(f"{path}:{n}: call of unknown '{target}'")
                if depth > 8:
                    raise Unfollowable(f"{path}:{n}: calls nest too deep")
                sub, _ = walk(path, procs[target], outs, aliases, procs, depth + 1)
                if op != "call" and sub:
                    raise ShaderError(f"{path}:{n}: '{op} {target}' writes outputs on one "
                                      "path only")
                add(sub)
                continue
            for w in dest_writes(item, outs, aliases):
                counts[w] = counts.get(w, 0) + 1
        else:
            op, then, other = item
            a, a_end = walk(path, then, outs, aliases, procs, depth)
            if op == "for":
                if a:
                    raise ShaderError(f"{path}:{n}: writes an output inside a loop")
                continue
            b, b_end = walk(path, other, outs, aliases, procs, depth)
            if a_end != b_end or a != b:
                diff = sorted(set(a) ^ set(b) | {k for k in a if a.get(k) != b.get(k)})
                raise ShaderError(f"{path}:{n}: the if's branches write outputs differently: "
                              + " ".join(f"{r}.{c}" for r, c in diff))
            add(a)
            if a_end:
                return counts, True
    return counts, False


def check(path):
    """None if the shader keeps the rule, else a list of what it breaks."""
    outs, aliases, procs = parse(path)
    entry = procs.pop(".entry", "main")
    if entry not in procs:
        return [f"{path}: no .proc {entry}"]
    try:
        counts, _ = walk(path, procs[entry], outs, aliases, procs)
    except ShaderError as e:
        return [str(e)]
    bad = []
    for r in outs:
        for c in COMPS:
            k = counts.get((r, c), 0)
            if k != 1:
                bad.append(f"{path}: {r}.{c} written {k} times (once is the rule)")
    return bad


def main(argv):
    files = argv or sorted(glob.glob(os.path.join(SOURCE, "*.v.pica")))
    failed = 0
    for path in files:
        name = os.path.basename(path)
        try:
            bad = check(path)
        except Unfollowable as e:
            print(f"warning: {e}; not checked")
            continue
        if bad:
            failed += 1
            for b in bad:
                print(b.replace(path, name))
    if failed:
        print(f"{failed} shader(s) leave an output component unwritten or written twice: "
              "the 3DS's GPU freezes on them (TASKS.md #327)")
        return 1
    print(f"pica_output_check: {len(files)} shader(s), every output component written once")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
