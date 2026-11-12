# Need for Speed: Underground 2 for the New 3DS

An unofficial homebrew port of the PC version of **Need for Speed:
Underground 2** to the New Nintendo 3DS. It is the PC game itself, not a
remake: its code was reverse-engineered from the PC's `SPEED2.EXE` and
reimplemented in C, with the GameCube release as the reference where a
console does things its own way (the pad, the controls screen, the
renderer). It plays from your own PC copy's files, which a PC app converts
and copies to the SD card. No game data comes with it.

What you get is the PC game: the career in Bayview's free roam, with its
shops, sponsors, magazine covers and DVD covers, the SMS messages and the
world map; every race type (circuit, sprint, drift, drag, Street X, URL,
outrun); Quick Race; the car customization, the dyno and the tuning; the
movies and scenes; EA TRAX; the saves; and the PC's cheat codes. The top
screen shows the race and the menus' car, and the bottom touch screen the
minimap, the bank, the standings and the menus' navigation. On top of that,
Local Wireless races up to six New 3DS consoles against each other.

## What you need

- A **New 3DS, New 3DS XL or New 2DS XL** with custom firmware (Luma3DS)
  and FBI. An Old 3DS or 2DS can't run it: it has too little memory and too
  little speed.
- The 3DS's **DSP firmware** dumped to `sdmc:/3ds/dspfirm.cdc` (or `sdmc:/dspfirm.cdc` if
  using CIA version), for the sound. From the Rosalina menu (L + D-Pad Down + SELECT): Miscellaneous
  options > Dump DSP firmware.
- **Your own PC copy** of Need for Speed: Underground 2, installed (the
  folder `SPEED2.EXE` is in).
- About **2.6 GB** free on the SD card.
- A Windows PC for the **NFSU2-3DS Tools** apps (nothing to install). Or
  any computer with Python 3, for the same tools as scripts (see
  [Without the apps](#without-the-apps)).

## Installing

1. Open **NFSU2-3DS Data Setup** from
   [`PC Apps/NFSU2-3DS Tools`](PC%20Apps/NFSU2-3DS%20Tools). Pick your
   NFSU2 PC install, and as the game data folder a folder named `nfsu2` at
   the root of the SD card (with the card in your computer as `E:`,
   `E:\nfsu2`). Press **Set up / update**. The first run takes a long
   while; running it again later redoes only what is missing or out of
   date.
2. Install `NFSU2-3DS.cia`, from this repository's Releases page or built
   as below, with FBI, and start it from the HOME Menu.

On the first start the game writes `sdmc:/nfsu2_config.txt`, which points
it at `/nfsu2`. The `.3dsx` runs from the Homebrew Launcher as well, with
its data in `/3ds/nfsu2` instead, but the CIA is what has been tested on
hardware.

Your profiles and career saves are kept in the game data folder (`SAVE/`
and `profiles/`). Data Setup never touches them, or anything else in the
folder it didn't make.

If something is missing, the game shows a "can't start yet" screen naming
the path it looked at, instead of booting to black. It checks for no config
file (or a `nfsu2_config.txt.txt`: Windows hides known extensions, so
typing the name in Explorer adds a second `.txt`), a `GameDir` that isn't a
folder on the SD card, and no DSP firmware. Press HOME, fix the SD card and
start the game again.

### From a command line

The Tools folder's `nfsu2-3ds.exe` does the same without a window:

```
nfsu2-3ds setup --pc "<PC game folder>" --gamedir E:\nfsu2
nfsu2-3ds setup --pc "<PC game folder>" --gamedir E:\nfsu2 --list
```

`--list` shows what is out of date without doing it, and `nfsu2-3ds setup
--help` lists every option.

### Without the apps

The apps are built from the Python in `tools/` (`python tools/build_apps.py`
rebuilds them). On any system, the same runs from Python 3 with numpy,
Pillow, lz4, pefile, unicorn and capstone installed, and ffmpeg (with
libx264), vgmstream-cli and dspenc on the PATH:

```
python3 tools/stage.py --pc "<PC game folder>" --gamedir <SD>/nfsu2
```

or, with a window, `tools/nfsu2_setup.pyw`.

## Controls

### Driving

| Button | Action |
| --- | --- |
| Circle Pad | steer |
| A | accelerate |
| B | brake and reverse |
| R | nitrous |
| L | handbrake |
| X | shift up |
| Y | shift down |
| ZR | change the camera |
| ZL | reset the car |
| SELECT | look back |
| D-Pad Up | enter a shop or challenge an opponent |
| D-Pad Down | world map |
| D-Pad Left | career statistics |
| D-Pad Right | SMS messages |
| START | pause |

Every action but steering and pause can be moved to another button in
Options > Controls, per profile. **Default Settings** puts the table above
back and **Delete All Controls** clears every button.

### In the menus

The D-Pad or the Circle Pad moves, A selects and B goes back. Every menu on
the bottom screen can be used by touch as well.

## Local Wireless

**LAN** on the main menu races two to six New 3DS consoles, in every race
type. Each console needs the game and its own game data folder, made from
the same version of the tools. One console hosts and picks the race; the
others join it.

## Cheat codes

The PC game's cheat codes are entered on the title screen with the console
button sequences. [CHEAT_CODES.MD](CHEAT_CODES.MD) has every code.

## Mods

Most PC data mods work (car textures and models, vinyls, HUD and menu art,
text, sounds, music). **NFSU2-3DS Mod Packer**, in the Tools folder, packs
one into the game data folder's `mods/`, and the game lays it over its data
at boot. Code mods (`.asi` plugins, `SPEED2.EXE` patches) can't run on the
3DS. [MODDING.md](MODDING.md) has the details.

## The config file

`sdmc:/nfsu2_config.txt` (beside the `.3dsx` for the `.3dsx`) holds
`Key=Value` lines; `#` starts a comment. The first start writes one, and
the game never changes a config that is already there.

| Line | What it does |
| --- | --- |
| `GameDir=/nfsu2` | the game data folder, from the SD card's root (required) |
| `Region=US` | which region's version of the game to be: its cars, badges and cheat codes (see [Regions](#regions)) |
| `Mods=CarPack,HudSkin` | which mods in `mods/` load, and in what order (`Mods=off` turns them all off); without the line, all of them load |
| `HwVideo=0` | plays the movies on the CPU instead of the New 3DS's video hardware |

### Regions

The PC release is the US version, which is the default. The game's code has
every other region's differences in it too, and `Region=` picks one:

| `Region=` | Version |
| --- | --- |
| `US` (or `NA`) | North America, as the PC release |
| `EU` (or `EUROPE`, `UK`) | Europe |
| `KR` (or `KOREA`) | Korea |
| `JP` (or `JAPAN`) | Japan |
| `TW` (or `TAIWAN`) | Taiwan |
| `CN` (or `CHINA`) | China |
| `TH` (or `THAILAND`) | Thailand |
| `ALL` | every region's cars at once (not a real release) |

What changes between them:

- **Cars.** The US version has the Honda Civic and the Acura RSX. Every
  other region has the Peugeot 106 and the Corsa instead. With the Civic
  gone, the Corsa takes its place among the starter cars, and the
  opponents drive the same region's cars.
- **Badges and logos** (the car's name badge and maker's logo art).
  - US: Mazda Miata, Hyundai Tiburon.
  - Europe: Mazda MX-5, Hyundai Coupe, Vauxhall Corsa.
  - Korea: Mazda MX-5, Hyundai Tuscani, Opel Corsa.
  - Japan: Mazda Miata Roadster, Toyota Sprinter Trueno (the Corolla),
    Toyota Altezza (the Lexus IS300), Hyundai Coupe, Nissan Fairlady Z (the
    350Z), Nissan Skyline (the Infiniti G35), Opel Corsa.
  - Taiwan, China and Thailand: Mazda MX-5, Hyundai Coupe, Opel Corsa.
- **Best Buy.** Only the US version has the Best Buy vinyl; every other
  region leaves it out.

`ALL` has all 31 cars: the US badges and logos, the 106 and the Corsa with
their European ones, the US starter cars, the Best Buy vinyl, and
opponents that drive any of them.

The game writes `sdmc:/nfsu2_debug.txt` as it runs. Send it with a bug
report, with the Luma3DS crash dump if it crashed.

## Building from source

Requires devkitPro's 3DS tools (devkitARM, libctru, citro2d and citro3d).
In PowerShell, from the repository's top folder:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_cia.ps1
```

It works around spaces in the path and produces
`3DS Files/NFSU2-3DS.3dsx` and `.cia`. With Python on the PATH it first
checks the shaders and remakes the HOME Menu banner; without it, the
committed banner is used.

- `3DS Files/` -- the devkitARM project: `source/` (the port), `data/` (the
  embedded fonts and button art), `meta/` (the CIA's banner, icon and
  `app.rsf`).
- `tools/` -- the Python data and reverse-engineering tools, the Ghidra
  scripts, and the vendored `makerom` and `bannertool` (`tools/3ds-bin`).
- `PC Apps/NFSU2-3DS Tools/` -- the built Data Setup and Mod Packer, with
  their source and the programs they carry (ffmpeg, vgmstream, dspenc),
  each with its licence.
- `docs/` -- write-ups of the game's file formats and retail behaviour.

## About

A fan project, not made, endorsed or supported by Electronic Arts. Need for
Speed and Need for Speed Underground 2 are Electronic Arts' trademarks. The
repository holds no game data: the port runs only with files from a copy
you own. The one exception is the CIA's HOME Menu banner, its sound and
the icon: `tools/make_banner.py` makes them from the PC game's files, and
they are kept in `3DS Files/meta/nfsu2` so the CIA builds without the game.

## License

The port's own code is under the GNU General Public License, version 3
(`LICENSE`). It covers the code alone: the game, its data and its art stay
Electronic Arts'. The vendored `makerom` and `bannertool`, and the
programs in `PC Apps/NFSU2-3DS Tools/bin`, keep their own licenses.

## AI Disclosure

Claude Opus 5.5 and Claude Fable 5.2 were used to help make this project possible. Specifically, they decompiled the game in full and ported it to the 3DS, and helped me debug in Azahar, then I tested it by myself on real hardware (on a New Nintendo 3DS XL).
