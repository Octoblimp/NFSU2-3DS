#pragma once
#include <3ds.h>
#include <stdbool.h>

// DifficultyInfo -- the table every AI driver's skill is read out of.
//
// Retail keeps one of these for the whole session, at the global the engine
// calls pTheDifficultyInfo (Speed.elf r13-0x53B8 == 0x8046FA28), filled by
// LoaderDifficultyInfo(bChunk*) (0x800270A0) from GLOBAL/GLOBALB.BUN's
// top-level 0x00034B00 chunk. Twenty-five functions read it; this module is
// the port of the chunk and of the handful of those reads the port's own AI
// makes. The pointer is a global in retail and a file-static here for the
// same reason: there is exactly one, loaded once, and every driver shares it.
//
// Layout, all verified against the shipped PC GLOBALB.BUN (the chunk header
// sits at file offset 7670144, size 0xAE0) and against the Speed.elf code that
// indexes it. Offsets below are from the chunk PAYLOAD, i.e. the byte after
// the 8-byte bChunk header, which is what pTheDifficultyInfo points at:
//
//   +0x000  a run of 11-point Graphs, each 0x58 bytes of (x, y) float pairs:
//           CalculateDistanceAndPositionBoost's distance curves (+0x000 ..
//           +0x318), then its place tables (+0x420 bytes, +0x424 / +0x464),
//           DecideBoostFactor's speed-ratio Tables (+0x49C / +0x4CC) and its
//           race-fraction Graph (+0x504). See difficulty_decide_boost.
//   +0x524  ten DIFFICULTY_FIELDS-float records, stride 0x68, indexed by the
//           driver's skill 0..9. See the field constants below.
//   +0x934  read by AIActionBlock::CalculateScore and the two ChooseNewAction
//           goals -- the action scoring weights; all 0 in the PC data, so
//           nothing to port (#255).
//   +0x94C  float[skill * 4 + class], read by RaceStarter::SetupCircuit /
//           SetupShortTrack / SetupURL into TheRaceParameters +0x80 (the
//           class handicap); 1.0 in every entry of the PC data (#255).
//   +0x97C  float[2][3][10], the cumulative probability table
//           RaceStarter::DecideAISkillLevel (0x8011AC00) draws a skill from.
//   +0xA6C  Car::CalculateNewCatchupMultipliers.
//   +0xA8C  TrafficTeleporter's min/max car spacing.
//   +0xAD4  the seconds between desired-speed lookahead re-rolls (5.0).
//   +0xADC  100000.0, CalculateDistanceAndPositionBoost's "no car there".
//
// A record is 26 floats. The ones this port reads are named; the rest are
// listed with the retail function that reads them so the next pass knows
// where to look rather than having to find them again:
//
//   [0]  always 0.                [2]  always 1.
//   [1]  0.00 .. 0.90   AIGoal::DecideSignpostAsRacer (0x8000A914): with
//                       [0] and [2] a cumulative roll at a racer's optional
//                       signpost -- r < [0] takes a type-1 sign, r < [1] a
//                       type-0 sign, else neither. How often the driver takes
//                       a shortcut -- but no shipped race file has a type-0
//                       or type-1 racer sign (60 Routes4*F.bin: types 2, 3,
//                       4, 5, 7, 10 only), so the roll never fires on PC data.
//                       The racers' real forks are type 4 (TASKS.md #409).
//   [3]  always 1       RaceCarController::ShouldWeAvoidThisAvoidable.
//   [4]  0.80 .. 2.10   copied into AvoidAvoidable +0x210 by its ctor and
//                       read by nothing after: dead in retail (#255).
//   [6]  0.76 .. 0.92   RaceCarController::DecideBoostFactor, with [8].
//   [8]  0.61 .. 0.79   ditto.
//   [11] always 0.      [12] always 0.01.
//   [15] [16]           copies of [13] [14].
//   [17] 10 .. 5        RaceCarController::CanUseNitrousRightNow, with [20].
//   [18] 5 .. 2         ditto.
//   [19] 0.5 .. 2.0     CanDisengageNitrousRightNow, ShortTermCanUseNitrous.
//   [20] 1.5 .. 4.0     CanUseNitrousRightNow.
//   [21] 0.025 .. 0.95  unread by anything that takes pTheDifficultyInfo
//   [22] 0.10 .. 1.10   directly; reached through a copy.
//   [23] 3 .. 19        RaceCarController::DecideBoostFactor.
//   [25] always 0.
//
// Every one of those columns ascends (or descends) monotonically with the
// skill index, which is what makes the table readable at all: skill 0 is the
// slowest, clumsiest driver the game ships and skill 9 the quickest.

#define DIFFICULTY_SKILLS 10
#define DIFFICULTY_FIELDS 26
#define DIFFICULTY_STRENGTHS 3   // eOpponentStrength: weak, medium, strong

// The record fields this port reads, each confirmed by reading the retail
// code that indexes it rather than by guessing from the numbers.
//
// RACER_SPEED_SCALE: AIGoal::SetDesiredSpeedAsRacer (0x8000B7E0) multiplies
// FuturePoints::InterpolateDrivingLineSpeed's answer by record[skill][5] at
// 0x8000BA14, then by AICarController +0x778 (DecideBoostFactor's catch-up
// boost, difficulty_decide_boost). 0.74 at skill 0 up to 1.00 at skill 9 --
// so even the best AI only just reaches the driving line's own limit.
// SetDesiredSpeedAsCop (0x8000BB28) reads the same column (+0x538).
#define DIFF_RACER_SPEED_SCALE 5

// STREETX_SPEED_SCALE: SetDesiredSpeedAsRacer's Street X branch (0x8000B8E8,
// +0x540), which also leaves out the lookahead (+0x70C). 0.75 .. 0.93.
#define DIFF_STREETX_SPEED_SCALE 7

// WAKE_LO / WAKE_HI: RaceCarController::Think (0x80027F88) seeds its timer
// with World::Random(hi - lo) + lo + 0.001, giving a negative number between
// -5.0 and -0.8 depending on skill, and then starts driving when the seconds
// left on the countdown fall below its magnitude. A skill-0 driver begins
// reacting 4.3 to 5.0 s before GO, a skill-9 driver 0.8 to 1.5 s before it.
#define DIFF_WAKE_LO 9
#define DIFF_WAKE_HI 10

// LAUNCH_RPM_LO / LAUNCH_RPM_HI: PsychoDriver::Drive (0x80020B80) hands
// DoStartLineThrottlePlay a band of 2 * (hi - lo) RPM around the car's optimal
// shift-up RPM -- 2400 RPM of slop at skill 0, 250 at skill 9, which is why a
// weak AI's revs wander all over the countdown and a strong one's sit still.
#define DIFF_LAUNCH_RPM_LO 13
#define DIFF_LAUNCH_RPM_HI 14

// LOOKAHEAD_JITTER: AIGoal::SetDesiredSpeedAsRacer again (0x8000B7F0). Every
// DIFFICULTY_LOOKAHEAD_SWITCH_SEC seconds the driver re-rolls how far up the
// road it reads its desired speed from, uniformly in +/- record[skill][24]
// metres. 40 m at skill 0 down to 20 m at skill 9: the weak driver is reading
// the road at the wrong place far more often, so it brakes late for one corner
// and crawls through the next.
#define DIFF_LOOKAHEAD_JITTER 24

// The payload +0xAD4 global above, in seconds.
#define DIFFICULTY_LOOKAHEAD_SWITCH_SEC 5.0f

// The +0xA8C table: nine (min, max) pairs of metres between traffic cars.
// TrafficTeleporter::GetAbsoluteMin/MaxDistanceBetweenCars (0x8002FD00 /
// 0x8002FD24) index it with the TPZ_TRAFFIC_TYPE zone's +0x30 + (density - 1)
// * 4 (TASKS.md #316).
#define DIFFICULTY_TRAFFIC_SPACINGS 9

typedef struct {
	bool  loaded;
	float rec[DIFFICULTY_SKILLS][DIFFICULTY_FIELDS];
	// DecideAISkillLevel's table: [unused][strength][skill]. Retail indexes
	// the first axis with a hardcoded 1 (0x8011AC20); block 0 is read by
	// nothing in the image, and is kept only so the extents are honest.
	float pick[2][DIFFICULTY_STRENGTHS][DIFFICULTY_SKILLS];
	float lookaheadSwitchSec;
	float trafficSpacing[DIFFICULTY_TRAFFIC_SPACINGS][2];
} DifficultyInfo;

// LoaderDifficultyInfo: scans `bunPath` (GLOBAL/GLOBALB.BUN) for the 0x34B00
// chunk and decodes it into the module's single instance. Returns false and
// leaves the instance unloaded if the file or the chunk is missing, which is
// not fatal -- every accessor below falls back to the shipped skill-9 figures
// so a card without the chunk still races, just without the spread.
bool difficulty_load(const char* bunPath, void (*log)(const char* msg));

// The one instance, never NULL. `loaded` says whether it holds real data.
const DifficultyInfo* difficulty_get(void);

// record[clamp(skill, 0, 9)][field], or a sane default when nothing is loaded.
float difficulty_field(int skill, int field);

// RaceStarter::DecideAISkillLevel (0x8011AC00): draws a skill 0..9 for an
// opponent of the given eOpponentStrength (0 weak, 1 medium, 2 strong) by
// walking the cumulative probability row for that strength against a uniform
// draw. `rng` is opponent.h's bRandom state, advanced in place. Used only when
// an event gives no skill byte of its own; the career roster always does.
int difficulty_pick_skill(int strength, u32* rng);

// The +0xA8C pair `idx` (clamped), min and max metres; the shipped figures
// when nothing is loaded.
void difficulty_traffic_spacing(int idx, float* minDist, float* maxDist);

// bRandom(float) (0x80059184) on an arbitrary state: uniform in [0, f).
float difficulty_random(u32* rng, float f);

// ---------------------------------------------------------------------------
// The AI catch-up boost (TASKS.md #255): RaceCarController::DecideBoostFactor
// (SpeedPS 0x800292B0), which AICarController::ExecuteCurrentAction
// (0x80008714) runs through the vtable (+0x10C) every time a car executes its
// action, and CalculateDistanceAndPositionBoost (0x80029954) under it.
//
// The answer goes to two places. AICarController +0x778 multiplies the speed
// SetDesiredSpeedAsRacer (0x8000B7E0) and SetDesiredSpeedAsCop (0x8000BB28)
// want; Car +0x478 takes max(boost, 1), which PhysicsMover /
// MellowMover::TuneBoostAndDampers hand SetBoost: the engine's torque, the
// aerodynamics' speed, the governor and the tyres' grip2 (carsim.h
// `catchup`).

// What one car's decision reads. Retail reads all of it off globals and the
// cars; the port's callers (opponent.c for a race, encounter.c for free roam)
// fill it in.
typedef struct {
	int   gameMode;      // TheRaceParameters +0x14: 0 free roam, 7 the getaway
	bool  racer;         // the car has a RacingCar (Car +0x1C)
	bool  dragOrDrift;   // +0x40 / +0x44
	bool  streetX;       // +0x58 (DAT_803FCA98)
	bool  pointToPoint;  // +0x18
	int   laps;          // +0x20
	int   trafficLevel;  // +0x08
	bool  catchupOn;     // +0x7C, IsCatchUpOn (1 in every career race; Quick
	                     // Race's setting in a circuit or sprint)
	float handicap;      // +0x80[player], 1.0
	int   players;       // +0x24, the humans
	float lapLength;     // DAT_803F4480, metres
	float raceSec;       // Race +0x14, the race clock
	bool  actionAllows;  // the running AIAction's +0x1EC: 0 only in GetUnstuck
	int   skill;         // the driver's, 0..9
	// The car's own RacingCar and speed.
	float laps_done;     // RacingCar +0x14
	int   place;         // RacingCar +0x0A, 1 leading
	float gap;           // RacingCar +0x24 (the getaway's gap to its leader)
	float speed;         // Car +0x440, m/s
	// Player 0's (the target), and its Player::GetSpeedRatio.
	float tLaps;
	int   tPlace;
	float tGap;
	float tSpeed;
	float tRatio;
} DiffBoostIn;

// The seconds between recomputes (DifficultyInfo +0xAD8, 0.3).
float difficulty_boost_interval(void);

// One DecideBoostFactor. `since` is the seconds since the car's last
// recompute (ctrl +0x734), which the caller advances and this zeroes on a
// recompute; `prev` is the car's +0x778. Returns the new +0x778: the caller
// multiplies its desired speed by it and hands carsim max(it, 1).
float difficulty_decide_boost(const DiffBoostIn* in, float* since, float prev);

// Graph::GetValue (0x801A5214) over `n` (x, y) pairs at payload offset `off`.
// 0 when nothing is loaded.
float difficulty_graph(int off, int n, float x);
