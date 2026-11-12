#pragma once
#include <3ds.h>
#include <stdbool.h>

// CustomPhysicsSpec (Speed.elf): a car's custom tunings -- the tuning screen's
// sliders -- applied over its built CarPhysicsInfo. RebuildPhysicsInfo
// (0x801C89A8) runs it between ApplyJunkmanUpgrades and MakeAccelerationTable:
//
//     if (GetNumActiveTunings(spec) > 0) RebuildCustomTunings(spec);
//
// A tuning type is active when one of the car's fitted performance parts has
// a row for it in the params table (GetInstalledParams 0x801C9E20), so a
// stock car has none and the step does nothing. ridephys_build_tuned() is the
// hook; this file is the spec itself, ported function by function from the
// GameCube decompile (the addresses below), so that a later tuning screen can
// drive the same Get/SetValue and ranges retail's does.
//
// The three pieces retail keeps:
//
//   CustomPhysicsTunings (0x29 bytes)  what the save keeps per car and per
//       eTuningSetting (PresetCarSlot +0x348, five blocks): one byte per
//       type, 0x17 singles, then 9 for the torque curve (type 23, +0x17) and
//       9 for the boost curve (type 24, +0x20). 100 is the car's own value;
//       0 and 200 the two ends of the tuneable range around it.
//   DefaultTuningsStructure            each active type's value before the
//       tuning, taken by RebuildCustomTunings: 0x17 floats, 9 at +0x5C, 9 at
//       +0x80.
//   the params table                   GLOBALB.BUN's 0x34A1A chunk
//       (CreateCustomTuningParamsTable 0x801C9D14, loaded by
//       LoaderMostWantedCareerChunk 0x80123520), 62 rows, baked into
//       custtune.c by tools/custtune_params.py.

// ePhysicsTuningType. The field each one moves is GetRealValuePtr's
// (0x801CBFD0), CarPhysicsInfo offsets (docs/retail_car_physics.md).
enum {
	CUSTTUNE_FRONT_SPRING = 0,      // +0x174, front SuspensionInfo [1]
	CUSTTUNE_FRONT_DAMPING = 1,     // +0x178 / +0x17C, front bump / rebound
	CUSTTUNE_FRONT_SWAY = 2,        // +0x180, front anti-roll
	CUSTTUNE_FRONT_RIDE_HEIGHT = 3, // +0x18 / +0x48, the front wheels' z, as a drop
	CUSTTUNE_REAR_SPRING = 4,       // +0x194
	CUSTTUNE_REAR_DAMPING = 5,      // +0x198 / +0x19C
	CUSTTUNE_REAR_SWAY = 6,         // +0x1A0
	CUSTTUNE_REAR_RIDE_HEIGHT = 7,  // +0x78 / +0xA8, the rear wheels' z, as a drop
	CUSTTUNE_STEERING = 8,          // +0x270, ControlInfo steer scale
	CUSTTUNE_FINAL_DRIVE = 9,       // +0x1B8 / +0x1BC, both final drives
	CUSTTUNE_GEAR1 = 10,            // +0x1D8 .. +0x1EC, DriveTrainInfo ratios [2..7]
	CUSTTUNE_GEAR6 = 15,
	CUSTTUNE_NOS_2A0 = 16,          // +0x2A0, nitrous (moves type 17 against it)
	CUSTTUNE_NOS_2AC = 17,          // +0x2AC, nitrous (moves type 16 against it)
	CUSTTUNE_BRAKE_BIAS = 18,       // +0x27C
	CUSTTUNE_FRONT_GRIP = 19,       // +0xDC / +0xE0, front TireInfo static / dynamic mu
	CUSTTUNE_REAR_GRIP = 20,        // +0xFC / +0x100
	CUSTTUNE_FRONT_DOWNFORCE = 21,  // +0x264, AeroInfo [1]
	CUSTTUNE_REAR_DOWNFORCE = 22,   // +0x268, AeroInfo [2]
	CUSTTUNE_TORQUE = 23,           // +0x200 [9], the torque curve less the boost
	CUSTTUNE_BOOST = 24,            // +0x224 [9], the boost curve
	CUSTTUNE_NUM_TYPES = 25
};

// The torque and boost curves' points (types 23 and 24 take an index 0..8).
#define CUSTTUNE_CURVE_POINTS 9
#define CUSTTUNE_SIZE         0x29
#define CUSTTUNE_DEFAULT_BYTE 100

// eTuningSetting: which of a car's five tuning blocks a race builds with.
// StartCarTuningRace (0x8011C904) and the RaceStarter setups give the order;
// StartCareerEvent (0x8011D02C) and SetupPlayerCarsAndStuff (0x80119C88)
// pass it to BuildRideForPlayer.
enum {
	CUSTTUNE_SETTING_CIRCUIT = 0,
	CUSTTUNE_SETTING_DRAG = 1,
	CUSTTUNE_SETTING_DRIFT = 2,
	CUSTTUNE_SETTING_STREETX = 3,
	CUSTTUNE_SETTING_URL = 4,
	CUSTTUNE_NUM_SETTINGS = 5
};

// CustomPhysicsSpec (RidePhysicsInfo +0x368). The car type and the
// PhysicsUpgradeSpecification retail points at are given here as the pieces
// this port keeps: the CompleatCarPhysicsInfo, the 68 fitted-part bytes (the
// spec's +0x50) and the ten junkman flags (+0x28).
typedef struct {
	const u8* compleat;   // +0x00's CarTypeInfo +0x110 (RIDEPHYS_COMPLEAT_SIZE)
	int units;            // +0x04, 1 = metric (the ctor's); SetCarTuningSpec
	                      // sets it from IsHudKPH
	const u8* installed;  // +0x08's +0x50, NULL = nothing fitted
	const int* junkman;   // +0x08's +0x28, NULL = no junkman parts
	u8* cpi;              // +0x0C, the CarPhysicsInfo it tunes
	int numActive;        // +0x10, GetNumActiveTunings' count
	u8 tunings[CUSTTUNE_SIZE];           // +0x14 CustomPhysicsTunings
	float defaults[0x17 + 2 * CUSTTUNE_CURVE_POINTS]; // +0x40 DefaultTuningsStructure
	int uniqueDepth;      // +0xE4, Remove/ApplyUniqueUpgrades' count
} CustTune;

// CustomPhysicsSpec ctor (0x801CA2C8) and Init (0x801CA350): the tunings at
// 100 unless `tunings` (CUSTTUNE_SIZE bytes) is given, the defaults zeroed,
// metric units. `cpi` must already be the built car with its junkman parts
// on, as RebuildPhysicsInfo hands it over.
void custtune_init(CustTune* s, const u8* compleat, u8* cpi, const u8* installed,
	const int* junkman, const u8* tunings);

// IsTuningActive (0x801CA5D0) and GetNumActiveTunings (0x801CA65C, counts
// types 0..24 and stores the count in numActive).
bool custtune_is_active(const CustTune* s, int type);
int  custtune_num_active(CustTune* s);

// RebuildCustomTunings (0x801CA6E4): every active type's untuned value into
// the defaults, then the stored tunings applied over s->cpi. Run once on a
// freshly built car; the stored bytes may be requantised on the way
// (UpdateStoredTuning), as retail's own copy is.
void custtune_rebuild(CustTune* s);

// CustomPhysicsSpec::GetValue (0x801CA360) / SetValue (0x801CA3FC): a type's
// tuned value, in real units, or in the screen's display units with `remap`
// (RemapValue 0x801CA934: km/h or mph for the gears, cm or inches for the
// ride heights, N m or ft lb for the curves, the params' 0..10 or -1..1
// scales where the table gives one). `idx` is the curve point for types 23
// and 24, else 0. SetValue moves s->cpi and the stored byte (and, for the
// gears, the nitrous pair and the curves, the types it drags along) at once.
float custtune_get_value(CustTune* s, int type, int idx, bool remap);
void  custtune_set_value(CustTune* s, float v, int type, int idx, bool remap);

// GetTuneableRangeMinMax (0x801CAC58): the slider's ends. False, with the
// outputs untouched, for a type no fitted part enables.
bool custtune_tuneable_range(CustTune* s, int type, int idx, bool remap, float* mn, float* mx);
// GetTotalRangeMinMax (0x801CAAC4): the scale the slider is drawn against.
bool custtune_total_range(CustTune* s, int type, int idx, bool remap, float* lo, float* hi);

// GetTuningTrend (0x801CCAEC): -1 for the ride heights and the final drive
// (a bigger number is a lower car or a shorter gear), else 1.
float custtune_trend(int type);

// CustomPhysicsSpec::Reset (0x801CA6C0) = CustomPhysicsTunings::Reset
// (0x801CA218): one type back to 100, or every byte with `type` -1.
void custtune_reset(CustTune* s, int type);

// The 0x29-byte block alone, as PresetCarSlot keeps it.
// CustomPhysicsTunings::Reset (0x801CA218), GetValue (0x801CA100: the byte
// as -1..1), SetValue (0x801CA194) and operator== (0x801CA054).
void  custtune_tunings_reset(u8* tunings, int type);
float custtune_tunings_get(const u8* tunings, int type, int idx);
void  custtune_tunings_set(u8* tunings, float v, int type, int idx);
bool  custtune_tunings_equal(const u8* a, const u8* b);
