#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "trackpath.h" // the TrackPathZones the wrong-way test asks about

// Retail's race layer: the route a race runs on, where a car is along it, and
// the lap/split/finish rules on top (CONTINUE.MD E4, TASKS.md #218).
//
// Everything here is a port of Speed.elf, function for function, and the
// numbers are the engine's own constants read out of its .sdata2 -- nothing
// is tuned by eye. The functions, with their GameCube addresses:
//
//   TrackPathManager  BuildConnectionTree        0x80200C74
//                     BuildPathConnectionsRecursive 0x80200878
//                     CreateSplines / CreateSpline 0x802013C4 / 0x80200358
//                     AssignDistances            0x8020068C
//                     FindClosestPoint           0x80201FC4
//                     FindClosestAdjacentPoint   0x80202728
//                     UpdatePosition             0x8020281C
//                     CompareDistances / ComparePercentDistances
//   QuickSpline       Overhauser basis (InitBasisMatricies 0x801923C0),
//                     endpoint type 2, CalibrateLength 0x80193418,
//                     FindClosestParam 0x80192FB4
//   Race              __4Race 0x80228928, FigureOutBoundaries 0x80228EFC,
//                     AddBoundaryPosition 0x80228D6C, GetLapNumberExact
//                     0x802292AC, SetRaceStage 0x802294AC,
//                     GetCountdownNumberToDisplay 0x80229620, Update 0x8022A70C
//   RacingCar         Init 0x80226F1C, FakeStartlineCrossing 0x80227580,
//                     CheckBoundaryCrossing 0x80227600, UpdateLapCounts
//                     0x80227FCC
//   RaceStarter       CalculateRacerPositions 0x8021BB94 (the grid)
//
// Where the data lives
// --------------------
// TRACKS/ROUTESL4RA/Paths<track>.bin and TRACKS/ROUTESL4RA/TrackPosMarkersAll.bin,
// copied as-is from the PC install next to the PathsFreeRoam.bin trackpath.c
// already reads. A Paths file is one 0x80034147 container; this reads four of
// its chunks (all little-endian on PC):
//
//   0x34148  points, 0x18 bytes: +0 x,y; +8 u16 path; +0xA u8 near and +0xB
//            u8 far thresholds (0xFF = none); +0xC s16[3] neighbour points;
//            +0x14 float distance (runtime)
//   0x34149  paths, 0xDC bytes: +0 name; +0x1C/+0x1E/+0x20 s16 first point,
//            end point (exclusive), count; +0x22/+0x23 connections per side,
//            {u16 path, u8 side} at +0x24 + side*0x20 + i*8; +0x64 float
//            length; +0x88/+0x90 endpoints; +0x98/+0xA0 box; +0xAC/+0xAD
//            allow/deny track counts over the u16 list at +0xAE; +0xBC s16
//            zone count over the u16 0x3414D indices at +0xBE
//   0x3414C  route groups, 0x110 bytes, one per track: +0 u16 track; +2 s8
//            waypoints; +3 loop; +4 build branches; +5 u8; bVector2 waypoints
//            from +8
//   0x3414A  track path zones, one variable-length record each -- trackpath.h
//            decodes them, and the wrong-way block further down uses them
//   0x3414D  0x24-byte records, a gate across the road: +0x00 two bVector2
//            ends; +0x10 the "blocked" byte BuildPathConnectionsRecursive
//            (0x80200878) refuses to route through; +0x11 how many of the
//            four u32 ids at +0x14 are used; +0x12 a flag, 1 in all but 45 of
//            the 1788. The ids are bStringHash of barrier names --
//            BARRIERS_<track>, PLAYER_BARRIERS_<track>, BARRIERS_CAREER<n>.
//            The shipped +0x10 is 0 everywhere; TrackPathManager::
//            EnableBarriers (0x8020029C) sets it at load for the race's two
//            names, so the route keeps off the roads the race closes
//
// The markers file is one 0x34146 chunk of 0x30-byte records from offset 0x10
// (LoaderTrackPositionMarkers 0x801F3C00): +8 u32 name hash, +0xC slot, +0x10
// xyz, +0x20 u16 angle (0 = +X, counter-clockwise), +0x24 track.

typedef struct RaceRoute RaceRoute;

// TrackPathCoordinate: where one point is along the route.
typedef struct {
	float last[2];   // +0x08 the position it was last updated with
	float onPath[2]; // +0x10 the nearest point on the route spline
	int   point;     // +0x18 the TrackPathPoint it is tracking, -1 for none
	float param;     // +0x1C that path's spline parameter
	float fraction;  // +0x20 dist / route length
	float dist;      // +0x24 metres along the route
	float dir[2];    // +0x28 the route's forward direction there, unit
	float threshold; // +0x38 the |dx|+|dy| an update has to move first
} RaceCoord;

// Opens Paths<track>.bin and builds `trackId`'s route, reversed when asked.
// NULL when the file is absent or has no group for the track, or when the
// connection tree cannot be built -- the caller reports it and stays in free
// roam. `log` may be NULL.
RaceRoute* race_route_open(const char* path, int trackId, bool reverse,
	void (*log)(const char* msg));
void  race_route_close(RaceRoute* r);
float race_route_length(const RaceRoute* r);
bool  race_route_loops(const RaceRoute* r);
int   race_route_path_count(const RaceRoute* r);
int   race_route_branch_count(const RaceRoute* r);

// TrackPathCoordinate(const bVector2*): a fresh coordinate updated once at pos.
void race_coord_init(const RaceRoute* r, RaceCoord* c, const float pos[2]);
// TrackPathCoordinate::UpdatePosition(const bVector2*).
void race_coord_update(const RaceRoute* r, RaceCoord* c, const float pos[2]);
// TrackPathManager::CompareDistances (0x8020309C): a - b in metres, wrapped
// by the route's length on a loop when it is more than half of it either way.
float race_route_compare(const RaceRoute* r, float a, float b);
// TrackPathManager::ComparePercentDistances, a - b in percent, wrapped on a
// loop.
float race_route_compare_percent(const RaceRoute* r, float a, float b);

// TrackPathManager::GetPosition (0x80202F94): the route sampled `dist` metres
// along it, pushed `lateral` metres to the right of travel, with the forward
// direction there. `dist` wraps on a looped route. False when the route has no
// splined path for that distance. Either output may be NULL.
//
// This is what an AI driver steers at: the race's own line, the same spline
// RaceCoord measures the player against.
bool race_route_sample(const RaceRoute* r, float dist, float lateral,
	float outPos[2], float outDir[2]);

// ---------------------------------------------------------------------------
// Going off course: the wrong-way cue and the zone resets (TASKS.md #247).
//
// This is retail's own handling, not a cue this port invented. Player::
// UpdateGameState (0x8021845C) runs it for every player, every frame of
// World::DoTimestep -- in free roam as much as in a race -- as
//
//     player+0x234 = DetectWrongWay(player, player+0x234)
//
// so the last frame's answer feeds the next one, which is the hysteresis.
// Retail's whole off-course story is that flag: HUD WrongWIndi (0x8010BFA8)
// fades "WrongWayImage" in while it is set and out when it clears, and that is
// all. It does not put the car back on the course and it does not stop the
// clock -- RaceCarController::AllowedToBeOutOfBounds (0x80028C50) is
// `li r3,0; blr` and DealWithBeingOutOfBounds (0x80028C58) a bare `blr`.
//
// The test is on heading, not on position: the route's direction where the car
// is, against the car's own heading, over a 110-degree cone. Only the zones
// below can put the car back, and Bayview has exactly one of those.
//
// The zones themselves are trackpath.h's -- the 0x3414A chunk, already loaded
// for the free-roam triggers; the four types this uses are the first four in
// its TrackPathZoneType, and L4RA carries one zone between them (the southern
// bay's RESET_TO_POINT). So in Bayview the cue is the pure angle test, plus
// that one reset for driving into the water.

// TrackPathManager::FindWrongWayOrResetZone (0x802022C4): trackpath_find for
// NO_WRONG_WAY, WRONG_WAY, RESET and RESET_TO_POINT, in that order. NULL when
// the point is in none of them.
const TrackPathZone* race_find_wrong_way_zone(const TrackPaths* tp,
	const float pos[2]);

// What Player::DetectWrongWay reads off the car. The names are the port's,
// the offsets retail's.
typedef struct {
	float pos[3];     // +0x58
	float vel[2];     // +0x68
	u16   heading;    // +0x41C, a bAngle: 65536 to the turn, 0 = +X, CCW
	float speed;      // +0x43C, m/s
	int   wheelsDown; // +0x434
	bool  topology;   // +0x5BC != 0
} RaceCarPose;

// What Car::MaybeResetToTrackPathZone (0x8022120C) decided to do.
typedef enum {
	RACE_RESET_NONE = 0,
	RACE_RESET_LATER,     // PleaseResetMeLater: upright where it lies
	RACE_RESET_TO_POINT,  // ResetToPosition at the zone, 8 m/s
} RaceResetKind;

// Car::ResetToPosition's speed (Speed.elf r13-0x60DC, 8.0): the reset leaves
// the car rolling, it does not stop it.
#define RACE_RESET_SPEED 8.0f

typedef struct {
	bool inZone;            // Player +0x108, set by a WRONG_WAY zone
	RaceResetKind reset;
	float resetPos[2];      // RESET_TO_POINT only
	u16   resetAngle;
} RaceWrongWay;

// Player::DetectWrongWay (0x802190E4). `already` is Player +0x234, the last
// frame's answer, which widens the cone by 10 degrees so the cue does not
// flicker on its own edge; the caller stores the result back there, as
// Player::UpdateGameState (0x8021845C) does. `tp` may be NULL, which is the
// city with no zones loaded -- the angle test still runs.
//
// `out` reports the zone work DetectWrongWay does on the way -- it is the
// function that performs a reset, not a separate pass -- and the caller
// applies it to its car. It may not be NULL.
bool race_detect_wrong_way(const RaceRoute* r, const TrackPaths* tp,
	const RaceCoord* c, const RaceCarPose* car, bool already,
	RaceWrongWay* out);

// bATan (0x8005961C): the bAngle of (x, y).
u16 race_bangle(float x, float y);

// TrackPosMarkersAll.bin, kept whole: a few hundred 0x30-byte records.
typedef struct RaceMarkers RaceMarkers;
typedef struct {
	u32 nameHash;  // +0x08 bStringHash of e.g. "ForwardStartingPosition"
	s32 slot;      // +0x0C
	float pos[3];  // +0x10
	u16 angle;     // +0x20, 65536 to the turn, 0 = +X, counter-clockwise
	s32 track;     // +0x24
} RaceMarker;

RaceMarkers* race_markers_open(const char* path, void (*log)(const char* msg));
void race_markers_close(RaceMarkers* m);
// GetNumTrackPositionMarkers / GetTrackPositionMarker(track, hash, index):
// the index-th marker with that name on that track, in file order.
int race_markers_count(const RaceMarkers* m, int track, u32 nameHash);
const RaceMarker* race_markers_get(const RaceMarkers* m, int track, u32 nameHash,
	int index);
// bStringHash (Speed.elf 0x8005DBE0): h = h*33 + c from 0xFFFFFFFF.
u32 race_string_hash(const char* s);

// The race. eRaceStage values are retail's: SetRaceStage(3) starts the
// countdown and Race::Update moves to 4 when the countdown shows GO.
enum {
	RACE_STAGE_NONE = 0,
	// The pre-race while the race-start NIS plays (#244): RCStatePreRace's
	// stage 2, held until the scene's StartCountdown marker.
	RACE_STAGE_PRE = 2,
	RACE_STAGE_COUNTDOWN = 3,
	RACE_STAGE_RACING = 4,
};

// TheRaceParameters gameMode 7: an outrun, a race with no route (#488).
#define RACE_MODE_OUTRUN 7

// TheRaceParameters, the fields this race reads.
typedef struct {
	int track;         // +0x00
	bool reverse;      // +0x04
	int gameMode;      // +0x14, 1 = race, 7 an outrun (RACE_MODE_OUTRUN)
	bool pointToPoint; // +0x18, TrackInfo +0x85
	int laps;          // +0x20
	int finishSlot;    // +0x3C, the ForwardFinishLine marker slot
	bool drift;        // +0x44, a drift race (SetupDriftRace, StartCareerEvent)
	// +0x48: the Hurry! timer's length, s -- InitWithDefaults' 30 (0x8033A980)
	// for every race; nothing the port starts changes it (#464).
	float hurryLimit;
} RaceParams;
#define RACE_HURRY_SEC 30.0f

#define RACE_MAX_BOUNDARIES 5
// The world timer's reading when a race is set up, in seconds (see race_init).
#define RACE_CLOCK_START 10.0f
#define RACE_MAX_LAPS 21
// FinishedRaceStatsEntry +0x1EC: ten laps' drift places (Construct zeroes
// 0x28 bytes), indexed from 1 at +0x1E8.
#define RACE_DRIFT_LAP_POS 11

// How many AI cars a race carries a RacingCar for. Retail's grid is six, so
// five opponents plus the player -- CalculateRacerPositions(6, ...).
#define RACE_MAX_AI 5

// RacingCar: one car's progress round the route. The player has one and so
// does every AI opponent, which is what the standings are read off.
typedef struct {
	s16  progress;        // +0x04 lap*5 + boundary
	s16  maxProgress;     // +0x06
	s8   lap;             // +0x08, -1 before the start line
	s8   maxLap;          // +0x09
	u8   finishRank;      // +0x0C
	bool newLap;          // +0x0D
	bool newProgress;     // +0x0E
	s16  finishReason;    // +0x10, 0 while racing
	float lapExact;       // +0x14
	float prevPct;        // +0x1C
	s32  crossTime[RACE_MAX_BOUNDARIES];  // +0x30
	s32  split[RACE_MAX_BOUNDARIES];      // +0x44
	s32  prevSplit[RACE_MAX_BOUNDARIES];  // +0x58
	s32  bestSplit[RACE_MAX_BOUNDARIES];  // +0x6C
	s32  prevBest[RACE_MAX_BOUNDARIES];   // +0x80
	s32  lapTime[RACE_MAX_LAPS];          // +0x94
	s32  lapTotal[RACE_MAX_LAPS];         // +0xE8
	s32  raceTime;        // +0x198
	bool extrapolated;    // +0x19C, the time is ExtrapolateRaceTime's estimate
	// RacingCarStats (+0x1A0, Update 0x802267D0), the two of its fields the
	// post-race stats screen reads, and the Car's distance driven (+0x424)
	// the average is taken from. All three in metres and m/s.
	float topSpeed;       // +0x1A0
	float avgSpeed;       // +0x1A4
	float odometer;
	// RacingCarStats +0x18 / +0x1C: every step up into a gear, and those
	// Car::GetShiftBonus scored over 0.5 (race_car_shifts()).
	int shifts;           // +0x1B8
	int perfectShifts;    // +0x1BC
	bool finished;
	RaceCoord coord;      // the Car's own TrackPathCoordinate (+0x8C8)
} RacingCar;

// FinishedRaceStatsEntry (Construct 0x80132A2C, 0x224 bytes): one car's
// record once the race is over, which UI_PostRaceStats reads. The fields
// here are the ones it reads, at retail's offsets.
typedef struct {
	int position;         // +0x00, the place it finished in
	int car;              // +0x04, the race_car() index, 0 the player
	int finishReason;     // +0x08
	s32 raceTime;         // +0x0C, 0 for reason 3
	s32 bestLap;          // +0x10
	s32 lapTime[RACE_MAX_LAPS];  // +0x1C, by lap number from 1
	s32 lapTotal[RACE_MAX_LAPS]; // +0x70, the same, summed
	int maxLap;           // +0xC4
	int lapPos[RACE_MAX_LAPS];   // +0xC8, the place it held at the end of each lap
	int lapsLed;          // +0xF4, how many of those were first
	float topSpeed;       // +0x10C, m/s
	float avgSpeed;       // +0x110, m/s
	int shifts;           // +0x124, RacingCarStats +0x18
	int perfectShifts;    // +0x128, RacingCarStats +0x1C
	// The drift's (#268 E). +0x140 is GetBestLapScore, the most any one lap
	// scored. +0x144 is each lap's points, rounded, and +0x198 those summed
	// -- the score Results prints and sorts a drift by. +0x1EC (driftLapPos,
	// from lap 1) is the place each lap's running total held, which in a
	// drift is also what lapsLed counts. +0x214..+0x21C are the accumulator's
	// DRIFT moment: the most points one drift scored, the longest drift and
	// the banked score.
	int bestLapScore;     // +0x140
	int driftLap[RACE_MAX_LAPS];   // +0x144
	int driftTotal[RACE_MAX_LAPS]; // +0x198
	int driftLapPos[RACE_DRIFT_LAP_POS]; // +0x1E8, [0] unused
	int bestDrift;        // +0x214
	float longestDrift;   // +0x218, s
	float driftScore;     // +0x21C
} RaceStatsEntry;

// One car's drift figures for race_finished_stats(): RacingCar +0x13C (each
// lap's points) and its accumulator's DRIFT moment, with its place on
// DriftManager's leaderboard. `has` is false for a car with no accumulator.
typedef struct {
	float lapScore[RACE_MAX_LAPS];
	float banked;    // the moment's +0x2C
	float maxPts;    // +0x44
	float maxDur;    // +0x40, s
	int boardPos;    // GetLeaderBoardPos, 0 off the board
	bool has;
} RaceDriftCar;

typedef struct {
	RaceParams params;
	RaceRoute* route;     // owned
	int stage;            // +0x00
	s32 startTime;        // +0x04
	s32 countdownTime;    // +0x08
	float countdownStep;  // +0x0C, 0.9 s
	s32 timerBase;        // +0x10, the race clock's zero
	s32 raceTime;         // +0x14
	s32 bestLap;          // +0x18
	// The drift's Hurry! timer (#464): FinishCar starts it when an AI car
	// finishes first -- +0x1C its finish on the world timer, +0x20 the
	// parameters' +0x48 -- and Race::Update finishes every car still racing
	// once it runs out. 0 / 0 until then.
	s32 hurryStart;       // +0x1C
	float hurrySec;       // +0x20
	int laps;             // +0xBE8
	s16 numBoundaries;    // +0xBF4
	s8  startIdx;         // +0xBF6
	s8  finishIdx;        // +0xBF7
	float boundary[RACE_MAX_BOUNDARIES]; // +0xBF8, route percent
	float startLine[3];   // +0xC0C
	float finishLine[3];  // +0xC1C
	s32 now;              // the world timer, 4000 ticks a second
	float nowSec;         // the same, unrounded, so ticks do not drift
	int nextRank;
	RacingCar car;
	// The AI field's, index-matched to OpponentField.car[]. race_add_ai puts
	// one on the grid; race_update_ai steps it.
	RacingCar ai[RACE_MAX_AI];
	int aiCount;
} Race;

// __4Race + Init(eRaceStage 0) for one car at `carPos`. `route` passes to
// the race. Returns false when the route is NULL.
bool race_init(Race* race, const RaceParams* p, RaceRoute* route,
	const RaceMarkers* markers, const float carPos[2]);
void race_free(Race* race);
// SetRaceStage.
void race_set_stage(Race* race, int stage);
// One frame: the world timer, then Race::Update (countdown -> GO, the
// player's lap counts, the finish). `pos` and `speed` (m/s) are the car's.
void race_update(Race* race, float dt, const float pos[2], float speed);

// Puts one AI car on the grid at `pos` and returns its index, or -1 when the
// race is full. The index matches OpponentField.car[].
int race_add_ai(Race* race, const float pos[2]);

// Race::Update for AI car `i`: its coordinate, its boundary crossings and,
// when it completes the race, its finish rank off the same counter the
// player's comes from. `dt` is this frame's, for its stats.
void race_update_ai(Race* race, int i, const float pos[2], float speed, float dt);

// 0 is the player, 1..aiCount the AI cars. race_car returns NULL past the end.
const RacingCar* race_car(const Race* race, int idx);
// RacingCarStats::Update's shift count for car `idx` (race_car()'s), off its
// CarSim's own (carsim.c dtAux counts them as Update does). Held from the
// finish on.
void race_car_shifts(Race* race, int idx, int shifts, int perfect);
int race_car_count(const Race* race);

// SimulateRaceFinish (0x8022B1F4), which TheRaceHasFinished runs once the
// player is over the line: every car still racing is finished in the order
// it is running, ranked after the cars already home, and given a race time
// ExtrapolateRaceTime estimates from how far round it got.
void race_simulate_finish(Race* race);

// FinishCar(car, GetLastAvailableRank, false, `reason`) (0x80229C88): car
// `idx` out of the race in the lowest place no car holds yet, with no race
// time -- Race::Update's drag retirements, 8 a blown engine and 9 a totaled
// car (RACE_FINISH_BLOWN / _TOTALED).
#define RACE_FINISH_BLOWN   8
#define RACE_FINISH_TOTALED 9
// Race::Update's Hurry! finish (0x8022A70C): every car still racing when the
// drift's timer runs out (#464).
#define RACE_FINISH_HURRY   11
void race_retire(Race* race, int idx, int reason);
// An outrun's two (RaceOverMessage's reasons 6 and 7): won it, or out of it.
#define RACE_FINISH_OUTRUN_WON  6
#define RACE_FINISH_OUTRUN_LOST 7
// race_retire() with the race time the car is out at (ticks from GO).
void race_retire_at(Race* race, int idx, int reason, s32 raceTime);
// PORT (#321c, a LAN race): car `idx` home at `raceTime` (ticks from the
// start) by its own console's clock -- finished there if it is not yet, and
// the lap finishers' ranks re-dealt in time order among the places they
// hold, so every console agrees on the order. Not for a drift race.
void race_set_finish_time(Race* race, int idx, s32 raceTime);

// RacePosition::Update's drift arm (0x801083F8): the Hurry! timer's seconds
// left, which Hurry_Counter shows; below 0 while it has not started.
float race_hurry_left(const Race* race);

// cFinishedRaceStats::Construct (0x8013303C) once every car is home: one
// entry per car, in finishing order, with CalculatePositionsForEachLap
// (0x8013327C)'s places and laps led. Returns how many it wrote, at most
// `cap`.
//
// In a drift (`drift` non-NULL, one per race_car() index) each entry also
// gets its drift figures, its position becomes its leaderboard place, the
// lap places are worked out a second time on each lap's running score
// (laps led counting those), and the table is sorted by the total score,
// the highest first (SortResultsByDriftPoints) -- the order Results lists.
int race_finished_stats(const Race* race, const RaceDriftCar* drift, RaceStatsEntry* out,
	int cap);

// That car's position right now, 1 = leading. A finished car keeps the place
// it finished in; the rest are ordered on progress round the route.
int race_position(const Race* race, int idx);
// GetCountdownNumberToDisplay: 3, 2, 1, 4 for GO, 0 for nothing.
int race_countdown_number(const Race* race);
// GetCountdownNumberRatio (0x80229760): how far through the number on show,
// 0..1, which the Countdown widget fades and grows it by.
float race_countdown_ratio(const Race* race);
// The race clock now, in ticks.
s32 race_clock(const Race* race);

// Timer::PrintToString (0x801A6AC8) for English: "M:SS.CC" style, "Hh:MM:SS"
// past an hour. `flags` is retail's: 4 prints zeros rather than dashes in an
// empty field, 1/2 prefix '-'/'+'. An unset time (0) prints as all zero
// fields. `out` needs 16 bytes.
void race_print_time(s32 ticks, u32 flags, char* out);

// RacingCar::GetLastStartLineCrossingTime / GetLastLapTime / GetBestLapTime
// (0x8022873C / 0x80228774 / 0x80228790): the car's split slots at the start
// and finish boundaries.
static inline s32 race_last_start_crossing(const Race* r) { return r->car.crossTime[r->startIdx]; }
static inline s32 race_last_lap_time(const Race* r) { return r->car.split[r->finishIdx]; }
static inline s32 race_best_lap_time(const Race* r) { return r->car.bestSplit[r->finishIdx]; }

// Timer::RoundTime(step) on a tick count.
s32 race_round_time(s32 ticks, float step);

// The grid: CalculateRacerPositions' marker read for start position `pos`
// (1-based), with the marker's own Z. Returns false when the track has no
// such marker.
bool race_grid_slot(const RaceMarkers* m, int track, bool reverse, int pos,
	float outPos[3], u16* outAngle);
