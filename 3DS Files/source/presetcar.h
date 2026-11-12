#pragma once
#include <3ds.h>
#include <stdbool.h>

// NFSU2's preset rides -- the fixed, fully built cars the game ships for its
// own use: the frontend's DDAY_PLAYER_CAR, the demo and AI presets, and the
// crew and celebrity cars (CALEB_GTO, SNOOP_DOGG, ...). TASKS.md D1 / #206.
//
// Where the data lives
// --------------------
// A top-level bChunk of type 0x00030220 in GLOBAL/GLOBALB.BUN (file offset
// 0x76e980 in the shipped PC data, 0x6700 bytes: 32 records). Its loader is
// SPEED2.EXE:0x61C3C0. The records start right after the 8-byte chunk header,
// the count is size / 0x338, and each is linked onto the list at [0x8A31E4],
// which is what its first two words are for at runtime.
//
// FUN_0061C460 finds one by name: it walks that list comparing FUN_00505450
// (the uppercase-folding string hash) of the record's +0x28 against the hash it
// was given. FUN_00637270 then puts the preset on a ride (layout below).
//
// Record layout (0x338 bytes):
//   +0x000 u32[2]    list links (runtime only)
//   +0x008 char[32]  car type name, "350Z" -- FUN_00505450 of it is the car
//                    record's nameHash (cardb.h +0x0D0)
//   +0x028 char[32]  preset name, "DDAY_PLAYER_CAR"
//   +0x048 s32       performance package, or -1 for the per-upgrade bytes at
//                    +0x2F4 -- what FillWithPreset installs
//   +0x04C u32[0xAA] one part-name hash per *GC* slot -- CarSlotId - 1, the
//                    numbering Render uses -- 0 = the slot is emptied, 1 = the
//                    slot keeps whatever part the ride had
//   +0x2F4 s8[0x44]  per-upgrade bytes (see +0x048): 1 in, 0 out, -1 left
//                    as the ride has it. No shipped preset carries -1 at
//                    +0x048, so these are never read in the shipped game.
//
// The GC slot numbering is confirmed four ways against the record: BASE_PAINT
// (CarSlotId 0x40) lands on a METAL_* colour part, VINYL_LAYER0 (0x41) on a
// vinyl, the four *_CARBON slots (0x5E..0x61) on CARBON FIBRE NONE, and the
// front/rear wheel pair (0x1E/0x1F) on two identical rims.

#define PRESETCAR_RECORD_SIZE 0x338
#define PRESETCAR_GC_SLOTS    0xAA
#define PRESETCAR_PERF_PARTS  0x44   // PERFDB_NUM_PARTS

// One more than the GC slots: the parts array below is indexed by CarSlotId,
// like a save's PresetCarSlot array, with [0] the car type as a save has it.
#define PRESETCAR_SLOTS (PRESETCAR_GC_SLOTS + 1)

// CarSlotId BASE_PAINT (profile_save.c's PROFILE_SLOT_BASE_PAINT): the body
// colour part, looked up in PAINT.nfsp like a save's.
#define PRESETCAR_SLOT_BASE_PAINT 0x40

typedef struct {
	char car[32];   // +0x008
	char name[32];  // +0x028
	u32 carHash;    // FUN_00505450(car): the car record's nameHash
	// By CarSlotId: parts[s] is the record's +0x4C + (s - 1) * 4. A 1 ("keep
	// the ride's part") is carried as-is: no shipped preset uses it, and the
	// part it would keep depends on the ride the preset is put on. Unsigned
	// int, as a save's careerParts are, because that is what carkit takes.
	unsigned int parts[PRESETCAR_SLOTS];
	s32 perfPackage;                      // +0x048
	s8 perfParts[PRESETCAR_PERF_PARTS];   // +0x2F4
} PresetCar;

// The uppercase-folding string hash FUN_00505450 (seed 0xFFFFFFFF, h * 33 + c).
u32 presetcar_hash(const char* s);

// Finds the preset whose name hashes to `nameHash` in `bunPath` (GLOBALB.BUN).
// False when the file, the chunk or the preset is missing.
bool presetcar_load(const char* bunPath, u32 nameHash, PresetCar* out);
