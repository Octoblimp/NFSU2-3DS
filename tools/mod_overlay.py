#!/usr/bin/env python3
"""
The PC side of data mods (TASKS.md #320): lays a PC mod over the retail PC
install so the port's packers can repack what the mod changes, then keeps
only the packed files that differ from the stock data. stage_sd.ps1 -ModDir
drives it; it is not meant to be run by hand, but each command works alone.

  create  <pc_root> <mod_dir> <overlay> --list <file>
      Builds <overlay>: a tree that reads as <pc_root> with <mod_dir>'s files
      in place of (or beside) the retail ones. Only the folders on a mod
      file's path are real; every other folder is a junction to the retail
      one, and the retail files beside a mod file are copied (hard-linked when
      large). Nothing under <pc_root> is ever written. Mod paths match the
      retail names case-free, as Windows and the 3DS's FAT do. --list gets one
      line per mod file: its overlay path, a tab, its path relative to the
      game root as the retail tree spells it.

  remove  <overlay>
      Deletes <overlay> without following its junctions into the retail tree.

  collect <staged> <stock_gamedir> <out> [--source <text>]
      Copies every file under <staged> that is new, or differs from the same
      path under <stock_gamedir>, to <out> (the mod's folder on the card,
      GameDir/mods/<name>). Byte-identical files are left out, so a mod that
      touches one car carries that car's packs only. Writes <out>/MODPACK.TXT
      saying what was packed from where.

A mod is a folder laid out like the PC game's own folder: a mod for the
Audi TT's textures is <mod>/CARS/TT/TEXTURES.BIN. Files the port does not read
(the .asi plugins, SPEED2.EXE patches, readmes) are reported, not packed.
"""
import argparse
import filecmp
import os
import shutil
import sys
import time

LINK_OVER = 64 << 20   # retail files this big are hard-linked, not copied


def _entries(d):
    try:
        return {e.upper(): e for e in os.listdir(d)}
    except FileNotFoundError:
        return {}


def _is_junction(p):
    return os.path.isjunction(p) or os.path.islink(p)


def _make_junction(target, link):
    # A junction needs no rights on Windows; elsewhere a plain symlink.
    if os.name == "nt":
        import _winapi
        _winapi.CreateJunction(os.path.abspath(target), os.path.abspath(link))
    else:
        os.symlink(os.path.abspath(target), link, target_is_directory=True)


def _place_retail_file(src, dst):
    if os.path.getsize(src) >= LINK_OVER:
        try:
            os.link(src, dst)
            return
        except OSError:
            pass
    shutil.copy2(src, dst)


def create(pc_root, mod_dir, overlay, list_path):
    if os.path.exists(overlay):
        remove(overlay)
    pc_root = os.path.abspath(pc_root)
    mod_dir = os.path.abspath(mod_dir)
    # Every mod file, as (path parts in retail spelling, the mod's file).
    mod_files = []
    for dirpath, dirs, files in os.walk(mod_dir):
        dirs.sort()
        for f in sorted(files):
            src = os.path.join(dirpath, f)
            rel = os.path.relpath(src, mod_dir).split(os.sep)
            parts = []
            cur = pc_root
            for i, p in enumerate(rel):
                names = _entries(cur) if cur else {}
                name = names.get(p.upper(), p)
                parts.append(name)
                cur = os.path.join(cur, name) if cur and name.upper() in names else None
            mod_files.append((parts, src))
    if not mod_files:
        raise SystemExit("mod_overlay: %s holds no files" % mod_dir)

    # The folders on a mod file's path are real; the rest junction or copy.
    real_dirs = {()}
    for parts, _ in mod_files:
        for i in range(1, len(parts)):
            real_dirs.add(tuple(p.upper() for p in parts[:i]))
    mod_by_key = {tuple(p.upper() for p in parts): (parts, src) for parts, src in mod_files}

    def fill(rel_parts, out_dir):
        os.makedirs(out_dir, exist_ok=True)
        retail_dir = os.path.join(pc_root, *rel_parts)
        names = _entries(retail_dir)
        for up, name in sorted(names.items()):
            key = tuple(p.upper() for p in rel_parts) + (up,)
            src = os.path.join(retail_dir, name)
            dst = os.path.join(out_dir, name)
            if os.path.isdir(src):
                if key in real_dirs:
                    fill(list(rel_parts) + [name], dst)
                else:
                    _make_junction(src, dst)
            elif key not in mod_by_key:
                _place_retail_file(src, dst)
        # Folders the mod adds that retail does not have.
        for key in sorted(real_dirs):
            if len(key) == len(rel_parts) + 1 and list(key[:-1]) == [p.upper() for p in rel_parts] \
                    and key[-1] not in names:
                parts = next(pp for pp, _ in mod_files if tuple(x.upper() for x in pp[:len(key)]) == key)
                fill(list(rel_parts) + [parts[len(rel_parts)]], os.path.join(out_dir, parts[len(rel_parts)]))

    fill([], overlay)
    lines = []
    for parts, src in mod_files:
        dst = os.path.join(overlay, *parts)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        # A fresh time, so a step's "older than its inputs" check sees it.
        os.utime(dst, None)
        lines.append("%s\t%s" % (dst, "/".join(parts)))
    with open(list_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    print("mod_overlay: %d mod file(s) over %s" % (len(mod_files), pc_root))


def _unlink_dir_link(p):
    # A junction (Windows) or a symlink to a folder: the link goes, never
    # what it points at.
    if os.name == "nt":
        os.rmdir(p)
    else:
        os.unlink(p)


def remove(overlay):
    """Deletes the overlay. Links are removed as links and never entered, so
    the retail tree behind them cannot be touched."""
    if not os.path.lexists(overlay):
        return
    if _is_junction(overlay):
        _unlink_dir_link(overlay)
        return
    with os.scandir(overlay) as it:
        entries = list(it)
    for e in entries:
        if e.is_junction() or e.is_symlink():
            if e.is_dir():
                _unlink_dir_link(e.path)
            else:
                os.unlink(e.path)
        elif e.is_dir(follow_symlinks=False):
            remove(e.path)
        else:
            os.remove(e.path)
    os.rmdir(overlay)


def collect(staged, stock, out, source):
    os.makedirs(out, exist_ok=True)
    kept, same = [], 0
    for dirpath, dirs, files in os.walk(staged):
        dirs.sort()
        for f in sorted(files):
            src = os.path.join(dirpath, f)
            rel = os.path.relpath(src, staged)
            ref = os.path.join(stock, rel) if stock else None
            if ref and os.path.isfile(ref) and os.path.getsize(ref) == os.path.getsize(src) \
                    and filecmp.cmp(ref, src, shallow=False):
                same += 1
                continue
            dst = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            kept.append((rel.replace(os.sep, "/"), os.path.getsize(src)))
    with open(os.path.join(out, "MODPACK.TXT"), "w", encoding="utf-8") as fh:
        fh.write("Packed by the NFSU2-3DS Mod Packer (tools/stage.py) on %s\n" % time.strftime("%Y-%m-%d %H:%M"))
        if source:
            # The mod folder's name alone: a full path would carry the
            # packer's own user and folders onto every card the mod is shared to.
            fh.write("From: %s\n" % os.path.basename(os.path.normpath(source)))
        fh.write("%d file(s); %d packed file(s) matched the stock data and were left out.\n"
                 % (len(kept), same))
        for rel, n in kept:
            fh.write("  %s (%d bytes)\n" % (rel, n))
    for rel, n in kept:
        print("  %s (%s KB)" % (rel, format(n // 1024, ",")))
    print("mod_overlay: %d file(s) into %s; %d matched the stock data" % (len(kept), out, same))
    return len(kept)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("create")
    c.add_argument("pc_root"); c.add_argument("mod_dir"); c.add_argument("overlay")
    c.add_argument("--list", required=True)
    r = sub.add_parser("remove")
    r.add_argument("overlay")
    k = sub.add_parser("collect")
    k.add_argument("staged"); k.add_argument("stock"); k.add_argument("out")
    k.add_argument("--source", default="")
    a = ap.parse_args()
    if a.cmd == "create":
        create(a.pc_root, a.mod_dir, a.overlay, a.list)
    elif a.cmd == "remove":
        remove(a.overlay)
    else:
        collect(a.staged, a.stock, a.out, a.source)


if __name__ == "__main__":
    sys.exit(main())
