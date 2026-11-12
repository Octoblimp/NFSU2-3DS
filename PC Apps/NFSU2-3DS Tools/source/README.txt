NFSU2-3DS PC apps -- source
===========================

Everything the three programs in the folder above are made from: the apps
(nfsu2_app.py, nfsu2_gui.py, stage.py), every packer they run, the build
script and the icons. The same files are the project's tools/ folder.

Run without the exes (any system with Python 3 and: pip install numpy pillow
lz4 pefile unicorn capstone; plus ffmpeg with libx264, vgmstream-cli and
dspenc on PATH -- build dspenc with `cc -O2 -o dspenc src/dspenc.c -lm`):

  python3 nfsu2_app.py setup --pc "<PC game folder>" --gamedir "<SD>/nfsu2"
  python3 nfsu2_app.py mods --mod "<mod folder>" --pc "<PC game folder>" --gamedir "<SD>/nfsu2"
  python3 nfsu2_app.py gui setup        (or double-click nfsu2_setup.pyw / nfsu2_mods.pyw)

Build the exes again (pip install pyinstaller, plus the above):

  python make_app_icons.py              (only to redraw app_icons/)
  python build_apps.py [--out "<folder>"] [--zip]

From here it builds into a new folder beside this one, "<this folder> (rebuilt)",
taking bin/'s dspenc, vgmstream and ffmpeg from the folder above.
