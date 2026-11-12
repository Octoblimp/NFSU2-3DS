#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "perfdb.h"
#include "custtune.h"

// RidePhysicsInfo (Speed.elf): one car's physics as the game builds it out of
// the shipped data and the car's fitted performance parts, and the three
// ratings the ACCELERATION / TOP SPEED / HANDLING bars of every car screen
// show (TASKS.md #116).
//
// The pieces, all ported from the GameCube Speed.elf decompile:
//
//   RebuildPhysicsInfo 0x801C89A8      BuildCarPhysicsInfo, then
//                                      ApplyJunkmanUpgrades, then
//                                      RebuildCustomTunings (custtune.c),
//                                      then MakeAccelerationTable and the
//                                      performance-measure integral
//   BuildCarPhysicsInfo 0x801C4D44     the stock CarPhysicsInfo, every
//                                      package builder over it, the aero
//                                      block and BuildInertiaTensor
//   MakeAccelerationTable 0x801C940C   acceleration against speed through
//                                      the gears, 64 samples
//   GetTheoreticalTopSpeed 0x801C8FE0
//   ComputeAccelMeasure 0x801C8278
//   GetAccelRating 0x801C8138, GetTopSpeedRating 0x801C8074,
//   GetHandlingRating 0x801C8348
//
// tools/carrating_check.py runs the same arithmetic over GLOBALB.BUN and
// compares the measure with the one the game ships per car (the 0x34A31
// tables): every one of the 31 player cars agrees to 1e-6, which is what
// pins the acceleration table, the shift points and the integral.
// tools/ridephys_check.py builds this file itself on the PC against the same
// tables, stock and with every package of levels 1..N fitted: stock agrees for
// all 31; fully upgraded, 29 agree and the HUMMER and ESCALADE come out 1.4%
// and 0.9% above the shipped last row -- a difference in how that row was
// generated, not located.
//
// The top-speed rating sits on its 0.03 floor for every stock car governed at
// 125 mph (the 206, Focus, Corolla, Miata and the three SUVs): Extrema [0] is
// exactly 125 mph, the fleet's slowest governor, and GetTheoreticalTopSpeed
// caps at the governor in both executables (SPEED2.EXE 0x5B5930 / 0x5B6A20
// agree with the GameCube's; SPEED2.EXE loads Extrema from the same chunk,
// 0x599484). That 0.03 of fill lies under the slider's handle, so retail draws
// those bars empty too: a retail PC Quick Race frame of the stock Focus puts
// its three handles at 0.082 / 0.024 / 0.323 of the 164-unit travel, where
// this file rates it 0.086 / 0.030 / 0.330 -- within a pixel each.
//
// RebuildPhysicsInfo's two other inputs. Custom tunings (CustomPhysicsSpec,
// the tuning screen's sliders) run between ApplyJunkmanUpgrades and
// MakeAccelerationTable, when GetNumActiveTunings is above 0; custtune.c is
// the spec and ridephys_build_tuned() / ridephys_build_spec() the builds that
// take a car's 0x29-byte CustomPhysicsTunings block. The race-type retunes
// are TuneForDragRace, TuneForDrift and TuneForShortTrack, which
// RebuildPhysicsInfo (0x801C8880) picks from TheRaceParameters. A drift (+0x44,
// unless a drag) is type 3, Street X (+0x58) type 10, a drag (+0x40) type 2.
// They are ridephys_tune_for_drag / _drift / _short_track. None applies on the
// frontend's car screens, which have no race set up.

// CompleatCarPhysicsInfo: CarTypeInfo +0x110 up to the CameraInfo at +0x720.
// Its first 0x2B0 bytes are the stock CarPhysicsInfo; the rest are what the
// package builders lerp towards (the upgrade targets) and the junkman parts'
// adjustments.
#define RIDEPHYS_CPI_SIZE      0x2B0
#define RIDEPHYS_COMPLEAT_SIZE 0x610
// Extrema (0x803DDA3C): the 30 floats of GLOBALB's 0x34A32 chunk, fleet
// ranges the ratings normalise by.
#define RIDEPHYS_EXTREMA       30
// RidePhysicsInfo +0x550: a 64-entry Table.
#define RIDEPHYS_ACCEL_SAMPLES 64
#define RIDEPHYS_PATHS         10

enum {
	RIDEPHYS_RATING_ACCEL = 0,
	RIDEPHYS_RATING_TOP_SPEED = 1,
	RIDEPHYS_RATING_HANDLING = 2,
	RIDEPHYS_RATINGS = 3
};

typedef struct {
	u8 cpi[RIDEPHYS_CPI_SIZE];    // +0x24, the built CarPhysicsInfo
	float level[RIDEPHYS_PATHS];  // the spec's +0x00 once built: clamp01(level) * 3
	float accel[RIDEPHYS_ACCEL_SAMPLES]; // +0x450, the Table's data
	float accelMin, accelMax;     // +0x554 / +0x558
	float measure;                // +0x08
} RidePhys;

// RebuildPhysicsInfo for a car whose CompleatCarPhysicsInfo is `compleat`
// (RIDEPHYS_COMPLEAT_SIZE bytes), with `installed` its 68 fitted-part bytes
// (the spec's +0x50) and `junkman` its ten junkman flags (+0x28). A NULL
// `pdb` or `installed` is a stock car; a NULL `junkman` is no junkman parts.
void ridephys_build(RidePhys* out, const u8* compleat, const PerfDb* pdb,
	const u8* installed, const int* junkman);

// The same build with the car's custom tunings: `tunings` is one of its five
// CUSTTUNE_SIZE-byte CustomPhysicsTunings blocks (the eTuningSetting the race
// builds with, Profile careerTuning / tunedTuning). After the junkman step,
// if any type is active (a fitted part enables it) RebuildCustomTunings runs
// with those bytes -- with all-100 bytes too, as retail's does, which still
// clamps the car into the tuneable ranges and re-spaces its gears. A NULL
// `tunings` is ridephys_build(), no tuning step at all.
void ridephys_build_tuned(RidePhys* out, const u8* compleat, const PerfDb* pdb,
	const u8* installed, const int* junkman, const u8* tunings);

// ridephys_build_tuned() keeping the CustomPhysicsSpec, for a tuning screen:
// `spec` is left bound to out->cpi (and to `compleat`, `installed` and
// `junkman`, which must outlive it) with its defaults taken, so that
// custtune_get_value / _set_value / _tuneable_range work on the built car.
// After a custtune_set_value, ridephys_build_tuned() again with spec->tunings
// gives the rebuilt car its acceleration table and measure.
void ridephys_build_spec(RidePhys* out, CustTune* spec, const u8* compleat, const PerfDb* pdb,
	const u8* installed, const int* junkman, const u8* tunings);

// ApplyJunkmanUpgrades (0x801C4F30) and its inverse RemoveJunkmanUpgrades
// (0x801C50CC), which CustomPhysicsSpec wraps every read and write of the
// car's own values in. `jm` is the ten junkman flags. Remove leaves the drive
// split ApplyJunkman's flag 1 sets, as retail's does.
void ridephys_apply_junkman(const u8* compleat, u8* cpi, const int* jm);
void ridephys_remove_junkman(const u8* compleat, u8* cpi, const int* jm);

// Table::GetValue (0x801A4EC8) over `n` samples spread evenly across
// [lo, hi]: linear between samples, the end samples beyond them.
float ridephys_table_get(const float* data, int n, float lo, float hi, float x);

// GetTheoreticalTopSpeed (m/s) and ComputeAccelMeasure.
float ridephys_top_speed(const RidePhys* r);
float ridephys_accel_measure(const RidePhys* r);
// ComputeMiscStats (0x801C8BF8): the 0-60 and 0-100 mph times (seconds, the
// car type's TimingStatsKludgeFactors applied) and the quarter mile's time and
// speed (m/s); 0 for one never reached.
void ridephys_misc_stats(const RidePhys* r, int carType, float* t60, float* t100,
	float* qmTime, float* qmSpeed);

// One RIDEPHYS_RATING_* on retail's 0.03..1 scale, or all three in that
// order. `extrema` is the RIDEPHYS_EXTREMA floats.
float ridephys_rating(const RidePhys* r, const float* extrema, int which);
void ridephys_ratings(const RidePhys* r, const float* extrema, float out[RIDEPHYS_RATINGS]);

// GetMaxTorque (0x801C874C; SPEED2.EXE 0x5B5810) and GetMaxHP (0x801C85EC;
// 0x5B56E0), what the performance parts list's sliders and gains show
// (TASKS.md #289): the engine's peak over its torque curve. Without `extrema`
// it is the curve's own figure -- torque in kN m, power in kW; with it, the
// peak clamped to Extrema [4]/[5] (torque) or [6]/[7] (power) and taken as a
// share of that range. `rpm`, when not NULL, gets the engine speed of the peak.
float ridephys_max_torque(const RidePhys* r, float* rpm, const float* extrema);
float ridephys_max_hp(const RidePhys* r, float* rpm, const float* extrema);

// PhysicsUpgradeSpecification::UnpackUpgradeInfo (0x801C417C): every part out
// (UninstallAllPerfParts(-1)), then each of the 68 whose bit `row` carries put
// straight in -- not through InstallPerfPart, so nothing it requires or
// replaces moves. `row` is one 0x10-byte row of a car's performance-config
// table (cardb_read_perf_rows), a PhysicsUpgradePackedSpec: the bit of part
// `id` is bit 7 - (id & 7) of byte 5 + (id >> 3) (IsPerfPartInstalled,
// 0x801C4098).
void ridephys_unpack_row(const u8* row, u8* installed);

// RidePhysicsInfo::MatchPerformance(float, false, float) (0x801C91C0) for the
// car whose CompleatCarPhysicsInfo is `compleat` and whose table is `rows`
// (`nrows` of them). It finds the two rows the raw measure `measure` falls
// between -- the first row above it and the one before, or the last two --
// and takes the closer, the upper one on a tie (a one-row table takes row 0).
// The car gets that row's parts (`installed`) and is rebuilt into `out`. With
// `handling` >= 0 it then moves the measure, once, by (handling - the rebuilt
// car's handling rating) * 150 and searches again, which needs `extrema`
// (NULL skips it). A car with no rows, or a NULL `pdb`, is left as `out` and
// `installed` came in.
void ridephys_match_performance(RidePhys* out, u8* installed, const u8* compleat,
	const PerfDb* pdb, const u8* rows, int nrows, float measure, float handling,
	const float* extrema);

// CalculateCarClass(measure, false) (0x801C9780) over PerformanceClassRange
// (0x80333638: 0, 273, 401, 500): 0 for a measure over 500, 1 over 401, 2
// over 273, else 3. RebuildPhysicsInfo stores it before any race-type retune;
// the DriveTrain takes it as its class (the perfect-shift ranges). PORT: a
// CarTypeInfo with +0x844 set is class 3 whatever its measure; that flag lies
// outside the CompleatCarPhysicsInfo this file is given and is not read.
int ridephys_car_class(const RidePhys* r);

// RebuildPhysicsInfo's type-2 tail for a drag: CompleatCarPhysicsInfo::
// TuneForDragRace (0x801C5ECC), which is ApplySpecialTunings (0x801C5C34)
// with MiscCarTunings +0xEC, the drag's SpecialModeTunings (the same 22
// floats in all three of TheMiscCarTunings 0x8035FD9C), over r->cpi; then
// MakeAccelerationTable again. Each tuning is lerped by its path's
// GetNormalizedLevel (level / 3): downforce x1.2 stock to x1.4 at the top
// junkman level, the Izz scale x0.9 and the mass x0.95; the rest are
// identities. The measure (and so the class) stays the untuned car's.
void ridephys_tune_for_drag(RidePhys* r);

// RebuildPhysicsInfo's type-3 tail for a drift: CompleatCarPhysicsInfo::
// TuneForDrift (0x801C5E2C), ApplySpecialTunings with the drift's
// SpecialModeTunings (MiscCarTunings +0x3C, picked by the drive split), then
// the NOS bottle +0x2A0 emptied (DAT_80332BA4, 0): a drift has no nitrous.
// Then MakeAccelerationTable again.
void ridephys_tune_for_drift(RidePhys* r);

// RebuildPhysicsInfo's type-10 tail for Street X: CompleatCarPhysicsInfo::
// TuneForShortTrack (0x801C5E7C) is ApplySpecialTunings with Street X's
// SpecialModeTunings (MiscCarTunings +0x94, picked by the drive split), then
// the NOS bottle +0x2A0 emptied (DAT_80332BA8, 0): Street X has no nitrous.
// Then MakeAccelerationTable again.
void ridephys_tune_for_short_track(RidePhys* r);
