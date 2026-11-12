#!/usr/bin/env python3
"""NFSU2-3DS Data Setup (TASKS.md #472): makes the port's nfsu2data folder
from an NFSU2 PC install. Double-click on Windows, or
`python tools/nfsu2_setup.pyw`. Without a window: `python tools/stage.py`."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nfsu2_gui  # noqa: E402

nfsu2_gui.main("setup")
