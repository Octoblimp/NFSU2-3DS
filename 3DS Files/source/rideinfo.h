#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "cardb.h"
#include "carpartdb.h"
#include "perfdb.h"
#include "presetcar.h"

// RideInfo (Speed.elf), the part of it an AI car needs: the visual parts it
// wears and the PhysicsUpgradeSpecification that decides them (TASKS.md
// #296). RaceStarter dresses every RANDOM opponent in three steps:
//
//   PickRandomCarsOfSomeSort    Init(car) + SetStockParts(0)
//   SyncVisualPartsWithPhysics  (0x80244A38, ride, true, random):
//                               MatchUpgradeLevel the aero path (9) to the
//                               mean of the tyre, weight and suspension
//                               levels, BuildRandomRide (0x802470A8) when
//                               random, then the ENGINE / FRONT_BRAKE parts
//                               by the engine and brake packages fitted
//   BuildRandomRide             ends in BuildRandomSkin (0x8024748C)
//
// and the second-to-last of a field takes the one before it's RideInfo with
// a fresh BuildRandomSkin. Everything random draws on bRandom, the same state
// the car picker shuffles with, so the order of the calls is retail's too.
//
// tools/randomride_check.py is the offline twin of BuildRandomRide and
// BuildRandomSkin: given the levels and the bRandom state opponent.c logs, it
// prints the parts this file should have chosen.

#define RIDEINFO_PATHS 10   // ePhysicsUpgradePath

// GC CAR_SLOT_IDs this file names.
#define RIDE_SLOT_FRONT_BUMPER  0x01
#define RIDE_SLOT_REAR_BUMPER   0x02
#define RIDE_SLOT_WIDE_BODY     0x06
#define RIDE_SLOT_ROOF          0x07
#define RIDE_SLOT_TOP           0x08
#define RIDE_SLOT_HOOD          0x09
#define RIDE_SLOT_SKIRT         0x0B
#define RIDE_SLOT_SPOILER       0x0C
#define RIDE_SLOT_ENGINE        0x0D
#define RIDE_SLOT_EXHAUST       0x10
#define RIDE_SLOT_DOOR_FIRST    0x11   // ..0x16
#define RIDE_SLOT_HOOD_UNDER    0x19
#define RIDE_SLOT_FRONT_BRAKE   0x1B
#define RIDE_SLOT_FRONT_WHEEL   0x1D
#define RIDE_SLOT_REAR_WHEEL    0x1E
#define RIDE_SLOT_WIDE_DECAL    0x3B   // ..0x3E
#define RIDE_SLOT_BASE_PAINT    0x3F
#define RIDE_SLOT_VINYL_LAYER0  0x40   // ..0x43
#define RIDE_SLOT_PAINT_SPOILER 0x45
#define RIDE_SLOT_PAINT_EXHAUST 0x47
#define RIDE_SLOT_VINYL_COLOUR  0x4D   // four per layer, ..0x5C

// PhysicsUpgradeSpecification: +0x00 the per-path level SetNormalizedLevel
// stores (0..3), +0x50 the 68 installed bytes. The junkman flags between
// them are the player's alone.
typedef struct {
	float norm[RIDEINFO_PATHS];
	u8 inst[PERFDB_NUM_PARTS];
} RideSpec;

typedef struct {
	const CarPartDb* db;
	const CarRec* car;
	CarPart part[CARPARTDB_SLOTS];
	RideSpec spec;
} RideInfo;

// bRandom(n) on `state` (Speed.elf 0x800590C4): returns the state it came in
// with, modulo n, and then advances it.
u32 rideinfo_random(u32* state, u32 n);

// Init (0x802443B0): no parts, nothing installed.
void rideinfo_init(RideInfo* r, const CarPartDb* db, const CarRec* car);

// SetPart (0x80244E88) with UpdatePartsEnabled's derived slots: the hood's
// underside and the door pieces, which follow the WIDE_BODY part. Slots
// 0x19 and 0x61..0x68 are not set this way, as in retail.
void rideinfo_set_part(RideInfo* r, int slot, CarPart p);

// SetStockParts(0) (0x80244784).
void rideinfo_set_stock_parts(RideInfo* r);

// GetUpgradeLevel (0x801C41DC) and GetNormalizedLevel (0x801C4208).
int rideinfo_upgrade_level(const RideSpec* s, int path);
float rideinfo_normalized_level(const RideSpec* s, int path);

// After RebuildPhysicsInfo: every path's level as ComputeUpgradeLevel stores
// it, out of the installed parts.
void rideinfo_compute_levels(RideSpec* s, const PerfDb* pdb);

// MatchUpgradeLevel (0x801C48A8): per path in `paths` (all ten when NULL),
// the installed parts that bring its level closest to `level` (0..1), found
// the way retail searches for them. Returns the mean level reached.
float rideinfo_match_upgrade_level(RideSpec* s, const PerfDb* pdb, float level,
	const int* paths, int numPaths);

// SetRandomPaint (0x80244DBC): one of the car's BASE_PAINT parts at random
// (bRandom over every level), and the four vinyl layers emptied. Nothing
// when the car has no paint.
void rideinfo_set_random_paint(RideInfo* r, u32* rng);

// SetRandomPart (0x80244954): GetRandomPart(slot, level) set into `slot`
// (emptied when the car has no part of that level).
void rideinfo_set_random_part(RideInfo* r, int slot, int level, u32* rng);

// BuildRandomRide (0x802470A8), BuildRandomSkin (0x8024748C).
void rideinfo_build_random_ride(RideInfo* r, u32* rng);
void rideinfo_build_random_skin(RideInfo* r, u32* rng);

// FillWithPreset (0x80245BF8), past FindFEPresetCar and GetCarTypeInfoFromHash:
// `car` is the record whose nameHash is FUN_00505450 of the preset's car name.
// Init + SetStockParts(0) only when the ride holds another car, then every
// slot from the preset (0 empties it, 1 keeps it, anything else is
// NewGetCarPart of that name), TOP put back to the car's first part, the
// preset's performance parts in, and RebuildPhysicsInfo's levels. `pdb` NULL
// leaves the performance side alone. Returns how many of the parts it names
// the database has no part for (retail empties those slots without a word).
int rideinfo_fill_with_preset(RideInfo* r, const CarPartDb* db, const CarRec* car,
	const PresetCar* p, const PerfDb* pdb);

// SyncVisualPartsWithPhysics(true, random) (0x80244A38).
void rideinfo_sync_visual(RideInfo* r, const PerfDb* pdb, bool random, u32* rng);

// The parts as a save's PresetCarSlot array, what carkit_assemble() takes:
// [0] the car's name hash, [slot + 1] each part's hash, 0 for none.
void rideinfo_slots(const RideInfo* r, unsigned int out[CARPARTDB_SLOTS + 1]);
