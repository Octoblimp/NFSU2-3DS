// EA Trax -- the NFSU2 soundtrack player.
//
// This is the music half of SPEED2.EXE's audio object (the one whose methods
// sit around 0x46B6F0), driven by the profile's song table
// (options.h, OptTraxSong). The exe's track table is at VA 0x7ECD80, 27
// 16-byte records (+0 title, +4 artist, +8 album, +0xC bank "FE" / "IG" /
// "AL"); this port reads the same records from the manifest
// tools/eatrax_build.py writes, and a song's number is its index there.
//
// The player keeps two rotations, one per context, each a pair of 27-bit song
// masks and a table position:
//
//   FE (the frontend)  all +0x84, left +0x88, position +0xA8
//   IG (in game)       all +0x8C, left +0x90, position +0xAC
//   play order         +0x9C, AudioSettings +0x30 (0 Sequential, 1 Random)
//
// InitEATRAX (0x479220; GC 0x8007BC70) -> InitializeEATrax (0x46BAA0) fills
// both from the table: FE gets every song whose playability has bit 0 (Menu,
// All), IG every one with bit 1 (Racing, All); `left` starts as `all` and both
// positions as -1. The next pick (0x46B6F0) refills `left` from `all` once it
// runs dry, then takes, sequentially, the first table position past `position`
// whose song is still left, or, at random, the r-th song left counting by song
// number, r = bRandom(songs left). Either way the song leaves `left`, so a
// rotation plays every song once before any song repeats.
//
// The audio itself is a .dspm stream per track (tools/eatrax_build.py),
// played through the NDSP hardware mixer in audio.h -- the ARM11 never decodes
// or mixes any of it.
#pragma once

#include <stdbool.h>
#include "audio.h"
#include "options.h"

#define EATRAX_MAX_TRACKS OPTIONS_TRAX_SONGS

// The track record's +0xC bank, which only Default__11UserProfile reads
// (0x515B46): it picks a fresh profile's playability for the song.
typedef enum {
	EATRAX_BANK_FE = 0,  // frontend / menu music
	EATRAX_BANK_IG,      // in-race music
	EATRAX_BANK_AL       // both; no song in this build's table has it
} EatraxBank;

typedef enum {
	EATRAX_CTX_FE = 0,
	EATRAX_CTX_IG,
} EatraxContext;

typedef struct {
	int        index;       // the song number
	EatraxBank bank;
	char       file[64];    // relative to the eatrax directory
	char       title[96];   // carries the game's own quote marks
	char       artist[64];
	char       album[80];
} EatraxTrack;

// Loads <dataDir>/eatrax/tracks.txt. Returns false if the manifest is missing
// or empty; every other entry point then no-ops safely.
bool eatraxInit(const char* dataDir);
void eatraxExit(void);

// How many songs the manifest has, and one by its song number (NULL for a
// song the manifest does not carry).
int  eatraxTrackCount(void);
const EatraxTrack* eatraxTrack(int song);

// InitEATRAX: rebuilds both rotations from the profile's table and play order.
// It is what retail calls after loading a profile and after every change the
// jukebox makes. The song playing carries on.
void eatraxSetSongs(const OptTraxSong table[OPTIONS_TRAX_SONGS], u32 playOrder);

// The title screen's opener (0x47AEC4, the FE music start with -2): when the
// table's first entry plays in the frontend, the FE rotation's `left` becomes
// song 0 alone, so the first FE pick is song 0 whatever the order.
void eatraxTitleOpener(void);

// The next pick in `ctx` (0x46B6F0), played on that context's bus. Returns
// false, with nothing playing, when the rotation has nothing to play.
bool eatraxPlayNext(EatraxContext ctx);

// PlayEATraxSong: the jukebox's preview. Plays `song` now, on the frontend
// bus, and leaves both rotations as they are.
bool eatraxPreview(int song);

void eatraxStop(void);
bool eatraxIsPlaying(void);
// Holds the current track where it is (true) or lets it carry on (false): the
// pause menu's PATH_pause, see audioMusicPause(). Playing another track
// releases the hold.
void eatraxPause(bool paused);
int  eatraxCurrentSong(void);
const EatraxTrack* eatraxCurrentTrack(void);
EatraxContext eatraxCurrentContext(void);

// Call once a frame with the frame delta. When the current song drains, the
// next pick in the same context follows it; it also drives the now-playing
// popup timer.
void eatraxUpdate(float dt);

// ------------------------------------------------- now-playing popup
// 0x4AC9F0 pushes the record's +0/+4/+8 into the display call at 0x4AC950,
// which is why the popup carries title, artist and album.

bool eatraxPopupActive(void);
float eatraxPopupAlpha(void);              // 0..1, includes fade in/out
const EatraxTrack* eatraxPopupTrack(void);
void eatraxDismissPopup(void);

// Keeps a raised popup in its hold window for as long as it is called (once a
// frame). For screens that wait on the player rather than a timer -- see the
// implementation's note and main.c's BOOT_TITLE leg.
void eatraxHoldPopup(void);

// The popup is the real game's "chyron" widget (Chyron_FE.fng in
// FRONTEND/FrontB.lzc, Chyron_IG.fng in GLOBAL/InGameCommon.bun -- FE/IG
// matching the two EA Trax bank names). Its entry and exit are authored FEng
// animation scripts, and their durations are shipped data: every "in" script in
// Chyron_FE.fng's Scrp chunks is 800ms and every "out" script is 600ms, so the
// popup's own fade timings are those, not a made-up pair. main.c's chyron
// renderer plays the decoded keyframes across each window, which is why it
// needs the phase and the position inside it rather than just an alpha.
#define EATRAX_POPUP_IN_MS   800.0f
#define EATRAX_POPUP_OUT_MS  600.0f

typedef enum {
	EATRAX_POPUP_PHASE_IN,
	EATRAX_POPUP_PHASE_HOLD,
	EATRAX_POPUP_PHASE_OUT,
} EatraxPopupPhase;

// Which of the three windows the popup is in, and how far into it, in ms.
// `outMs` may be NULL. Meaningless unless eatraxPopupActive().
EatraxPopupPhase eatraxPopupPhase(float* outMs);

// Total ms since the popup was raised -- drives the ring's idle pulse, which
// is a free-running 1800ms loop in the authored data rather than a per-phase
// track.
float eatraxPopupElapsedMs(void);
