#!/usr/bin/env python3
"""NFSU2-3DS Mod Packer (TASKS.md #320): turns a PC mod into the 3DS port's
files, in nfsu2data/mods/<name>. Double-click on Windows, or
`python tools/nfsu2_mods.pyw`. Without a window: `python tools/stage.py --mod`."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nfsu2_gui  # noqa: E402

nfsu2_gui.main("mods")
