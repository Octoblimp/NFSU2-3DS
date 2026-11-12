// modfs.h -- data mods laid over the game data (TASKS.md #320).
//
// A mod is a folder under GameDir/mods/, laid out like GameDir itself:
// GameDir/mods/<name>/CARS/350Z/... replaces GameDir/CARS/350Z/... . The stock
// files on the card are never touched; taking a mod's folder away takes the
// mod away.
//
// At boot modfs_init() walks every mod's folder once and keeps an index of
// the files it holds (paths folded to upper case with forward slashes, as
// the SD card's FAT matches them). The linker wraps fopen() and stat()
// (Makefile --wrap=fopen,--wrap=stat): a read-only open or a stat of a path
// under GameDir that a mod holds goes to the mod's copy instead. Writes, and
// anything outside GameDir (the saves, the log, the config), go through as
// they are. With no mods folder the index is empty and the wraps cost a
// pointer test.
//
// Which mods, and in what order, is the config's `Mods=` line:
//   (no line)        every folder in mods/, in name order;
//   Mods=a,b         only a and b; a's file wins where both have one;
//   Mods=off  (or 0) none.
// When two mods hold the same file, the first in that order wins.
//
// The port reads many PC files in forms of its own (CARS/*.nfsc, the
// TRACKS/L4R?/WORLD.* banks, the packed audio): a mod that changes one of
// those needs the same packing (the tools/ packers), done on the PC, with the
// packed result put in the mod's folder at the packed file's path.
#ifndef MODFS_H
#define MODFS_H

#include <stdbool.h>

// `gameDir` is the config's GameDir ("/nfsu2data"), `modsCfg` its Mods= value
// or NULL for no line. Logs through `log` what it found. Call once, before
// the first asset is read.
void modfs_init(const char* gameDir, const char* modsCfg, void (*log)(const char*));

// The mod files read since the last call, one log line each ("mods: CARS/...
// read from <mod>"). Main thread only; cheap when there are none.
void modfs_report(void (*log)(const char*));

// How many files the active mods hold (0 with none).
int modfs_file_count(void);

#endif
