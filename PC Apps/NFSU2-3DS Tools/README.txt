NFSU2-3DS PC apps
=================

Two programs that make the files the 3DS port of Need for Speed Underground 2
reads, from your own PC copy of the game. Nothing else needs installing.

  NFSU2-3DS Data Setup   Makes the game data folder (nfsu2data) from the PC
                         game's folder (the one SPEED2.EXE is in). Copy the
                         folder it makes to the 3DS's SD card. The first run
                         takes a long while; later runs only redo what changed.
                         The Peugeot 106 and the Corsa (CARS/PEUGOT106 and
                         CARS/CORSA, only in the UK/EU PC release) are packed
                         when the PC game's folder has them, skipped if not.

  NFSU2-3DS Mod Packer   Turns a PC mod (a folder laid out like the game's,
                         e.g. <mod>/CARS/TT/TEXTURES.BIN) into a 3DS mod in
                         nfsu2data/mods/<name>.

Command line (Windows, Linux, macOS) -- the same work without a window:

  nfsu2-3ds setup --pc "<PC game folder>" --gamedir "<SD>/nfsu2data"
  nfsu2-3ds setup --pc "<PC game folder>" --gamedir "<SD>/nfsu2data" --list
  nfsu2-3ds mods --mod "<mod folder>" --pc "<PC game folder>" --gamedir "<SD>/nfsu2data"
  nfsu2-3ds gui setup        (or: gui mods) opens a window from the command line
  nfsu2-3ds setup --help     every option

bin/ holds the outside programs the packers use: dspenc (the 3DS's ADPCM),
vgmstream-cli (EA audio) and ffmpeg (sound and video), each with its licence.
Without these programs the same tools run from Python: see tools/stage.py and
MODDING.md in the NFSU2-3DS source.
