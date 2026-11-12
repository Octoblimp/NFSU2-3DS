#ifndef CHEATS_H
#define CHEATS_H

// The PC's cheat codes (#446), entered with 3DS buttons on the title screen.
//
// SPEED2.EXE's cheat manager (object 0x865930, built by 0x575770): twenty
// 0x34-byte records at 0x7FB960 -- the typed code, a category, a parameter,
// a group, an active byte, a persistent byte and an available byte. The
// title screen (UG_LS_Splash, 0x4A90F0) turns entry on and its destructor
// turns it off; the matcher (0x57B8B0) compares the typed characters against
// every record and activation (0x579CB0) sets the record's active byte.
// Everything else asks the table through 0x576000, which answers for the
// FIRST record matching (category, parameter).
//
// PORT: the keyboard codes are typed with the 3DS buttons as the console
// builds enter them -- the GameCube EasterEggs table's 8-press sequences
// (Speed.elf 0x801A3C54, JoyHandler 0x801A46E8), L1 -> L, R1 -> R,
// Square -> Y, the D-Pad as is. CHEAT_CODES.MD has the table.

#include <3ds.h>
#include <stdbool.h>

#define CHEAT_CAT_VISUAL  1   // car parts of a level (QuickRaceUnlocker)
#define CHEAT_CAT_PERF    2   // performance packages of a level
#define CHEAT_CAT_SPECIAL 5   // 2 = $200, 3 = RX-8/Skyline + contest vinyls, 4 = $1000
#define CHEAT_CAT_SPONSOR 6   // a sponsor car, by its preset name hash
#define CHEAT_CAT_VINYL   7   // a manufacturer vinyl, by its part name hash

#define CHEAT_COUNT 20

// The table, built once (0x575770). `region` is the build region: the Best
// Buy record is unavailable outside region 0 (0x575770's 0x864F24 test).
void cheats_init(int region);

// Entry on (the title screen is up) or off (it went away), 0x579C90/0x579CA0.
void cheats_set_entry(bool on);

// What one frame's input did.
typedef struct {
	int  record;      // the record activated, -1 for none
	bool wasActive;   // it already was (0x579CB0 plays no sound then)
	unsigned deposit; // 0x5760A0's immediate deposit, to add to the wallet
} CheatFeed;

// One frame of input while entry is on: every button pressed this frame
// (START and A excepted -- they leave the screen) goes into the 8-press
// window. `careerStarted` and `cash` are what 0x5760A0 reads (0x86347C and
// the wallet at 0x861E74).
CheatFeed cheats_feed(u32 kDown, bool careerStarted, unsigned cash);

// FUN_00576000: the active byte of the first record matching.
bool cheats_lookup(int category, u32 param);

// The wrappers SPEED2.EXE calls it through.
bool cheats_visual_level(int level);           // 0x579D80
bool cheats_perf_level(int level);             // 0x579D90
bool cheats_sponsor_car(u32 presetHash);       // 0x579D70
bool cheats_manufacturer_vinyl(u32 partHash);  // 0x579DA0
bool cheats_contest_vinyls(void);              // 0x579D60
bool cheats_stock_car_type(int carType);       // 0x579D30: RX-8 0x15, Skyline 0x1B

// 0x576090: the cash a new career starts with (PlayerCareerState::Default).
unsigned cheats_start_cash(void);

// Record `i`'s PC code, for log lines.
const char* cheats_pc_code(int i);

#endif
