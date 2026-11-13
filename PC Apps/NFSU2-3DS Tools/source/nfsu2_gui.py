#!/usr/bin/env python3
"""
The windows of the port's two PC apps (TASKS.md #320, #472), over the same
engine (tools/stage.py). Each app is its own program:

  nfsu2_mods.pyw     NFSU2-3DS Mod Packer: a PC mod's folder in, its 3DS
                     files out, into nfsu2data/mods/<name> where the game
                     finds them at boot.
  nfsu2_setup.pyw    NFSU2-3DS Data Setup: every file the port reads, made
                     from the PC install (only what is missing or out of
                     date is redone).

tools/build_apps.py freezes both (and the nfsu2-3ds command line) into
exes that need no Python; from Python, double-click either .pyw or run
`python tools/nfsu2_mods.pyw`. The same work runs without a window from
tools/nfsu2_app.py (setup / mods), the way on a system with no Tk.
"""
import json
import os
import queue
import shutil
import sys
import threading
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stage  # noqa: E402

import tkinter as tk  # noqa: E402
from tkinter import filedialog, messagebox, ttk  # noqa: E402

SETTINGS = os.path.join(os.path.expanduser("~"), ".nfsu2_3ds_tool.json")
TITLES = {"mods": "NFSU2-3DS Mod Packer", "setup": "NFSU2-3DS Data Setup"}
TITLE = "NFSU2-3DS"


def load_settings():
    try:
        with open(SETTINGS, encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return {}


def save_settings(d):
    try:
        with open(SETTINGS, "w", encoding="utf-8") as fh:
            json.dump(d, fh, indent=1)
    except Exception:
        pass


class App:
    def __init__(self, root, mode):
        global TITLE
        TITLE = TITLES[mode]
        self.root = root
        root.title(TITLE)
        root.minsize(640, 520)
        self.cfg = load_settings()
        self.q = queue.Queue()
        self.worker = None
        self.stop_flag = False

        pad = {"padx": 8, "pady": 4}
        top = ttk.Frame(root)
        top.pack(fill="x", **pad)
        top.columnconfigure(1, weight=1)
        self.pc = tk.StringVar(value=self.cfg.get("pc", stage.DEFAULT_PC if stage.DEFAULT_PC and os.path.isdir(stage.DEFAULT_PC) else ""))
        self.gd = tk.StringVar(value=self.cfg.get("gamedir", stage.default_gamedir()))
        self._folder_row(top, 0, "NFSU2 PC install:", self.pc,
                         "The folder SPEED2.EXE is in (your PC copy of the game).")
        self._folder_row(top, 2, "Game data folder:", self.gd,
                         "The folder the game reads: nfsu2 on the SD card (CIA), 3ds/nfsu2 (.3dsx), or Azahar's.")
        self.pc_state = ttk.Label(top, foreground="#a33")
        self.pc_state.grid(row=4, column=0, columnspan=3, sticky="w")
        self.pc.trace_add("write", lambda *_: self._check_pc())
        self._check_pc()

        self.mod_btn = self.setup_btn = self.list_btn = None
        if mode == "mods":
            self._mods_frame(root, pad)
        else:
            self._setup_frame(root, pad)
        self._log_frame(root, pad)
        if mode == "mods":
            self.refresh_mods()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(100, self.pump)

    def _mods_frame(self, root, pad):
        m = ttk.LabelFrame(root, text="Pack a mod", padding=8)
        m.pack(fill="x", **pad)
        m.columnconfigure(1, weight=1)
        self.mod = tk.StringVar(value=self.cfg.get("mod", ""))
        self.mod_name = tk.StringVar(value=self.cfg.get("mod_name", ""))
        self._folder_row(m, 0, "Mod folder:", self.mod,
                         "Laid out like the PC game's folder, e.g. <mod>/CARS/TT/TEXTURES.BIN.",
                         on_pick=self._mod_picked)
        ttk.Label(m, text="Name on the card:").grid(row=2, column=0, sticky="w")
        ttk.Entry(m, textvariable=self.mod_name).grid(row=2, column=1, sticky="ew")
        self.mod_btn = ttk.Button(m, text="Pack mod", command=self.pack_mod)
        self.mod_btn.grid(row=2, column=2, sticky="e", padx=(8, 0))
        ttk.Label(m, foreground="#555", wraplength=600, justify="left", text=(
            "Every mod in mods/ loads, in name order; a Mods=a,b line in nfsu2_config.txt picks which and "
            "in what order (Mods=off turns them off). Code mods (.asi plugins, SPEED2.EXE patches) "
            "cannot run on the 3DS and are listed, not packed.")).grid(
            row=3, column=0, columnspan=3, sticky="w", pady=(6, 0))
        inst = ttk.Frame(m)
        inst.grid(row=4, column=0, columnspan=3, sticky="ew", pady=(6, 0))
        inst.columnconfigure(0, weight=1)
        ttk.Label(inst, text="Installed mods:").grid(row=0, column=0, sticky="w")
        self.mods_list = tk.Listbox(inst, height=4)
        self.mods_list.grid(row=1, column=0, sticky="ew")
        side = ttk.Frame(inst)
        side.grid(row=1, column=1, sticky="n", padx=(8, 0))
        ttk.Button(side, text="Refresh", command=self.refresh_mods).pack(fill="x")
        ttk.Button(side, text="Remove", command=self.remove_mod).pack(fill="x", pady=(4, 0))

    def _setup_frame(self, root, pad):
        s = ttk.LabelFrame(root, text="Set up game data", padding=8)
        s.pack(fill="x", **pad)
        ttk.Label(s, wraplength=600, justify="left", text=(
            "Makes every file the port reads from the PC install, into the game data folder, the "
            "New 3DS hardware movies (video/*.mvd) included. Only what is missing or out of date is "
            "redone, so running it again is quick. The first run takes a long while (the city, the "
            "audio and the movies are large). The Peugeot 106 and the Corsa come only with the "
            "UK/EU PC release: they are packed when the install's CARS folder has them and "
            "skipped when it doesn't.")).pack(anchor="w")
        row = ttk.Frame(s)
        row.pack(fill="x", pady=(6, 0))
        self.list_btn = ttk.Button(row, text="Check what is out of date", command=lambda: self.setup(True))
        self.list_btn.pack(side="left")
        self.setup_btn = ttk.Button(row, text="Set up / update", command=lambda: self.setup(False))
        self.setup_btn.pack(side="right")

    def _log_frame(self, root, pad):
        bar = ttk.Frame(root)
        bar.pack(fill="x", **pad)
        self.prog = ttk.Progressbar(bar, mode="determinate")
        self.prog.pack(side="left", fill="x", expand=True)
        self.stop_btn = ttk.Button(bar, text="Stop", command=self.stop, state="disabled")
        self.stop_btn.pack(side="left", padx=(8, 0))
        self.status = ttk.Label(root, text="Ready.")
        self.status.pack(fill="x", padx=8)
        logf = ttk.Frame(root)
        logf.pack(fill="both", expand=True, **pad)
        self.log = tk.Text(logf, height=14, wrap="none", font=("Consolas", 9) if os.name == "nt" else ("Courier", 10))
        sb = ttk.Scrollbar(logf, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set, state="disabled")
        self.log.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

    # ------------------------------------------------------------ widgets
    def _folder_row(self, parent, r, label, var, hint, on_pick=None):
        ttk.Label(parent, text=label).grid(row=r, column=0, sticky="w")
        ttk.Entry(parent, textvariable=var).grid(row=r, column=1, sticky="ew")

        def pick():
            d = filedialog.askdirectory(initialdir=var.get() or os.path.expanduser("~"), title=label.rstrip(":"))
            if d:
                var.set(os.path.normpath(d))
                if on_pick:
                    on_pick()
        ttk.Button(parent, text="Browse...", command=pick).grid(row=r, column=2, padx=(8, 0))
        ttk.Label(parent, text=hint, foreground="#555").grid(row=r + 1, column=1, sticky="w")

    def _mod_picked(self):
        if self.mod.get():
            self.mod_name.set(os.path.basename(self.mod.get().rstrip("\\/")))

    def _check_pc(self):
        p = self.pc.get()
        if not p:
            self.pc_state.config(text="Pick the PC install.")
            return False
        missing = stage.check_install(p) if os.path.isdir(p) else ["the folder"]
        self.pc_state.config(text="" if not missing else
                             "Not an NFSU2 PC install here (%s missing)." % ", ".join(missing))
        return not missing

    # ------------------------------------------------------------ mods list
    def refresh_mods(self):
        if not hasattr(self, "mods_list"):
            return
        self.mods_list.delete(0, "end")
        d = os.path.join(self.gd.get(), "mods")
        if os.path.isdir(d):
            for n in sorted(os.listdir(d), key=str.lower):
                if os.path.isdir(os.path.join(d, n)):
                    self.mods_list.insert("end", n)

    def remove_mod(self):
        sel = self.mods_list.curselection()
        if not sel:
            return
        n = self.mods_list.get(sel[0])
        p = os.path.join(self.gd.get(), "mods", n)
        if messagebox.askyesno(TITLE, "Delete the mod \"%s\" from\n%s?\n\nThis cannot be undone." % (n, p)):
            shutil.rmtree(p, ignore_errors=True)
            self.write("removed mod %s" % n)
            self.refresh_mods()

    # ------------------------------------------------------------ running
    def write(self, line):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def pump(self):
        try:
            while True:
                kind, a = self.q.get_nowait()
                if kind == "log":
                    self.write(a)
                elif kind == "prog":
                    i, n, name = a
                    self.prog.configure(maximum=max(n, 1), value=i)
                    self.status.config(text="%d / %d  %s" % (i, n, name))
                elif kind == "done":
                    self._busy(False)
                    self.status.config(text=a)
                    self.refresh_mods()
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def _busy(self, on):
        st = "disabled" if on else "normal"
        for b in (self.mod_btn, self.setup_btn, self.list_btn):
            if b is not None:
                b.configure(state=st)
        self.stop_btn.configure(state="normal" if on else "disabled")

    def _start(self, fn, label):
        if self.worker and self.worker.is_alive():
            return
        if not self._check_pc():
            messagebox.showerror(TITLE, self.pc_state.cget("text") or "Pick the PC install.")
            return
        self.cfg.update(pc=self.pc.get(), gamedir=self.gd.get())
        if hasattr(self, "mod"):
            self.cfg.update(mod=self.mod.get(), mod_name=self.mod_name.get())
        self.cfg.pop("video", None)     # the movies are always made now
        save_settings(self.cfg)
        self.stop_flag = False
        self._busy(True)
        self.write("=== " + label)

        def body():
            log = lambda s: self.q.put(("log", s))  # noqa: E731
            prog = lambda i, n, name: self.q.put(("prog", (i, n, name)))  # noqa: E731
            try:
                msg = fn(log, prog)
            except stage.Stopped:
                msg = "Stopped."
                log(msg)
            except SystemExit as e:
                msg = str(e)
                log("error: " + msg)
            except Exception as e:
                msg = "Failed: %s" % e
                log(traceback.format_exc().rstrip())
            self.q.put(("done", msg))
        self.worker = threading.Thread(target=body, daemon=True)
        self.worker.start()

    def pack_mod(self):
        mod, name = self.mod.get(), self.mod_name.get().strip()
        if not mod or not os.path.isdir(mod):
            messagebox.showerror(TITLE, "Pick the mod's folder.")
            return
        pc, gd = self.pc.get(), self.gd.get()

        def fn(log, prog):
            n, unused = stage.pack_mod(pc, gd, mod, name or None, log=log, progress=prog,
                                       stop=lambda: self.stop_flag)
            if n:
                return "Done: %d file(s) in mods/%s.%s" % (n, name, " Some files are not used (see the log)." if unused else "")
            return "Nothing to install: the port reads none of this mod's files."
        self._start(fn, "pack mod " + mod)

    def setup(self, list_only):
        pc, gd = self.pc.get(), self.gd.get()
        def fn(log, prog):
            stage.stage(pc, gd, list_only=list_only, log=log, progress=prog,
                        stop=lambda: self.stop_flag)
            return "Checked." if list_only else "Game data up to date in " + gd
        self._start(fn, ("check " if list_only else "set up ") + gd)

    def stop(self):
        self.stop_flag = True
        self.status.config(text="Stopping after the current step...")

    def close(self):
        if self.worker and self.worker.is_alive():
            if not messagebox.askyesno(TITLE, "A job is running. Stop it and quit?"):
                return
            self.stop_flag = True
        self.root.destroy()


def main(mode):
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista" if os.name == "nt" else "clam")
    except tk.TclError:
        pass
    App(root, mode)
    root.mainloop()
