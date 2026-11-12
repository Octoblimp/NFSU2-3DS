# Modding NFSU2-3DS

The port runs the PC game's own data, so most PC **data mods** work: new car textures and
models, vinyls, HUD and menu art, text, sounds, music. They need two things:

1. **Packing.** Some PC files are converted for the 3DS (cars, the city, the audio). The
   **NFSU2-3DS Mod Packer** does that for you.
2. **A mods folder.** Packed mods go in `nfsu2data/mods/<name>/` on the SD card. The game
   lays them over its data at boot, so the stock files on the card are never touched.
   Deleting a mod's folder removes the mod.

**Code mods do not work.** `.asi` plugins (Extra Options, unlimiters and the like) and
patches to `SPEED2.EXE` are PC (x86) programs, which the 3DS cannot run. The packer lists
these files and leaves them out. Cars added in a new slot need such code too, so a mod
that adds a car beside the stock ones is not supported; one that replaces a stock car is.

## Packing a mod

You need the **NFSU2-3DS Tools** folder (nothing to install: it carries everything it
uses), your own NFSU2 PC install, and a copy of the mod. Without the Tools folder, the
same packer runs from Python 3 and this project's `tools` folder on any system (see the end
of this section).

1. Lay the mod out like the PC game's folder. Most mods already come that way: a mod for
   the Audi TT's textures is `<mod>/CARS/TT/TEXTURES.BIN`. If a mod's readme says "copy
   these into your NFSU2 folder", that folder is the layout to use.
2. Open **NFSU2-3DS Mod Packer** in the Tools folder (from Python: double-click
   `tools/nfsu2_mods.pyw`, or `python3 tools/nfsu2_mods.pyw`).
3. Pick your **NFSU2 PC install** (the folder `SPEED2.EXE` is in), the **game data folder**
   (`nfsu2data` on the SD card), and the **mod folder**. The name on the card defaults to
   the mod folder's name.
4. Press **Pack mod**. The log shows each step and, at the end, every file written to
   `nfsu2data/mods/<name>/` and any file the port does not read.

The PC install is only read, never changed. Only the packed files that differ from the
stock ones are kept, so a mod for one car carries that car's files only.

Without a window (any system, or for scripts), with the Tools folder's command line:

```bash
nfsu2-3ds mods --mod "<the mod's folder>" --pc "<NFSU2 PC install>" --gamedir "<SD card>/nfsu2data"
```

or the same from Python 3 (it then needs numpy, Pillow, lz4, pefile, unicorn and capstone,
and ffmpeg, vgmstream-cli and dspenc on the PATH; `tools/toolbin.py` says where each is
looked for):

```bash
python3 tools/nfsu2_app.py mods --mod "<the mod's folder>" --pc "<NFSU2 PC install>" --gamedir "<SD card>/nfsu2data"
```

`nfsu2-3ds gui mods` opens the window from a command line.

`--mod-name <name>` sets the folder name under `mods/`. On Windows,
`tools\stage_sd.ps1 -ModDir "<mod>"` does the same from PowerShell.

## Several mods

Every folder in `mods/` loads, in name order. Where two mods change the same file, the
first one wins. A line in `nfsu2_config.txt` picks which mods load, and in what order:

```
Mods=CarPack,HudSkin
```

`CarPack` wins over `HudSkin` where both have a file. `Mods=off` turns every mod off without
deleting anything. The Mod Packer's "Installed mods" list shows and removes them.

## What the game does at boot

`nfsu2_debug.txt` on the SD card lists the mods it found and each file they hold, then a
`mods: <file> read from <mod>` line the first time the game reads a modded file. With no
`mods` folder it says `mods: none`.

## Which PC files the port reads

The packer works this out by itself; this list is for mod authors.

| PC file | On the 3DS |
|---|---|
| `CARS/<car>/*` (the 31 player cars, the 12 traffic cars) | repacked: `CARS/<car>.nfsc`, part kits, vinyls, rims, shadows, hulls |
| `GLOBAL/*.BUN`, `GLOBAL/*.bun` | copied, and repacked into the HUD, paint, neon, lights and parts tables |
| `FRONTEND/FrontB.lzc`, `FRONTEND/MAGAZINES_*` | copied |
| `FRONTEND/PLATFORMS/<room>.BIN` | repacked: the showroom and shops |
| `LANGUAGES/English.bin`, `CREDITS/*.TXT` | copied |
| `TRACKS/*` (a set's `.BUN` files, `ROUTES*`) | repacked: the whole track set (slow) |
| `TRACKS/TRACKMAP*.BIN` | copied |
| `MOVIES/*.vp6` | copied, with the soundtrack taken out (and the New 3DS `.mvd` copy when that is set up) |
| `SOUND/**/*.abk`, `.gin`, `.mxb` | repacked or copied: engines, turbos, shifts, skids, nitrous, the city's sounds, menu sounds |
| `pfdata/MusicSFx.mus` | repacked: EA Trax |
| `speech/*` | repacked: the career speech and scene audio |
| `NIS/*` | repacked: the scenes' crowds |

Other languages than English are not read yet.

## Limits

- **Memory.** A New 3DS has far less memory than a PC. A car with a very high polygon count
  or large textures may draw slowly or fail to load; the log says so.
- **Packing time.** A car or a sound takes seconds. A city or track mod repacks the whole
  set, which takes minutes. Movies for the New 3DS's hardware decoder take a long while.
