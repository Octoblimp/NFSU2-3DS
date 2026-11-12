#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "carsim.h"
#include "langbin.h"
#include "trackpath.h"

// Style points, retail's (Speed.elf 0x8018D960..0x80191398): the moments a
// player's driving earns (power slides, air, 360s, clean laps...), the
// accumulator that banks them, MiscStyleManager's detectors, and what they
// buy -- RacingCar::Update (0x80227770) puts the points into the nitrous
// bottle through FillNitrous (carsim_nos_fill).
//
// The moments' tuning is the game's own: StyleMomentsInfo, chunk 0x34A07 in
// GLOBAL/GLOBALB.BUN, 39 records of 0x30 bytes. Each record says which race
// kinds it runs in; the accumulator only makes the moments the race's kind
// has (InitStyle / StylePointsAccumulator's constructor).
//
// The moments that are up each frame go to the HUD (FEUpdateStylePoints,
// style_take_up and hud.c's CunningStuntSpread); this module also logs them
// ("style:").

// eStyleMomentType, the record index.
enum {
	STYLE_DRIFT = 0,
	STYLE_CHAINED_DRIFT,
	STYLE_BONUS_ZONE,
	STYLE_WALL_IMPACT,
	STYLE_SPRAYSHOW = 15,
	STYLE_TRAFFIC_HIT,
	STYLE_LEADLAP,
	STYLE_CLEANSECTION,
	STYLE_CLEANLAP,
	STYLE_SHORTCUT,
	STYLE_NEARMISS,
	STYLE_360,
	STYLE_BESTLAP,
	STYLE_HEADSTART,
	STYLE_JTURN,
	STYLE_BURNOUT,
	STYLE_LEAPFROG,
	STYLE_GAINED_5TH,
	STYLE_GAINED_4TH,
	STYLE_GAINED_3RD,
	STYLE_GAINED_2ND,
	STYLE_GAINED_1ST,
	STYLE_HYDRAULIC_BOUNCE,
	STYLE_POWERSLIDE,
	STYLE_HANGTIME,
	STYLE_DRAFTING,
	STYLE_REVERSE,
	STYLE_HYDRAULIC_EXTEND,
	STYLE_TYPE_COUNT
};

// LoaderStyleMomentsInfo: reads the records out of GLOBALB.BUN. `lang` (may
// be NULL) names them for the log. False when the chunk is not there, and
// then nothing earns points.
bool style_load(const char* bunPath, const LangTable* lang);

// InitStyle's race kinds, the mask a moment's record is read against.
// Retail's 0x20 kind (DAT_803FCAA0) is not one the port can tell.
enum {
	STYLE_KIND_CIRCUIT = 0x01,   // and free roam
	STYLE_KIND_DRAG    = 0x02,
	STYLE_KIND_DRIFT   = 0x04,
	STYLE_KIND_SPRINT  = 0x08,   // a point-to-point track
	STYLE_KIND_STREETX = 0x10,
	STYLE_KIND_URL     = 0x40,
};

// RacingCar::Init for the player: a fresh accumulator and MiscStyleManager,
// with the moments race kind `kind` runs. Listens to `player`'s collisions
// until style_end.
void style_begin(const CarSim* player, u8 kind);
void style_end(void);

// What one frame of RacingCar::Update reads off the race and the car.
typedef struct {
	float dt;                  // WorldTimer's frame time
	int   stage;               // *pCurrentRace; free roam runs at 4
	bool  finished;            // RacingCar::IsFinishedRacing
	int   gameMode;            // TheRaceParameters +0x14: 0 free roam
	int   totalCars;           // +0x24.. summed: the player and the field
	float raceSec;             // Race +0x14, seconds; below 0 in free roam
	int   position;            // RacingCar +0x0A, 1 = leading; 0 none
	// A drift (TheRaceParameters +0x44) and the car's place on DriftManager's
	// leaderboard (GetLeaderBoardPos, 0 off it), which DetectCleanRun's
	// LEADLAP reads in place of `position`.
	bool  drift;
	int   boardPos;
	bool  newLap, newProgress; // RacingCar +0x0D / +0x0E
	s32   lastLap, bestLap;    // the lap just ended, the best: LapCounter's
	bool  blinking;            // Car::IsBlinking, the player's
	// The other cars, for the near misses (CollisionBodyWitness).
	const CarSim* const* others;
	const bool* othersBlinking;
	int   otherCount;
	// PORT (#459): the traffic (driver type 3), witnesses for the near
	// misses as well, and what RegisterCollisionImpulse's TRAFFIC_HIT knows
	// a traffic car by.
	const CarSim* const* traffic;
	int   trafficCount;
	const CarSimWorld* world;  // GetElevation under the car
	const TrackPaths* paths;   // the SHORTCUT zones
} StyleFrame;

// One frame: RacingCarStats::Update's burnout, MiscStyleManager::Update,
// StylePointsAccumulator::Update, then the nitrous refill. Run it after the
// car has moved and the race has been updated.
void style_update(CarSim* player, const StyleFrame* f);

// The nitrous press (RacingJoyHandler 0x8021B038): once StartNitrous has
// opened the bottle, Car::IsBlowOff -- the throttle shut or under 5 mph --
// gives SPRAYSHOW.
void style_nitrous_pressed(const CarSim* player, float throttle);

// GetRaceStylePoints(false): what has been banked.
float style_points(void);

// eStyleMomentState.
enum {
	STYLE_STATE_ACTIVE = 0,
	STYLE_STATE_ACTIVATING = 1,
	STYLE_STATE_INACTIVE = 2,
	STYLE_STATE_DONE = 3,
	STYLE_STATE_ABORTED = 4,
};

// One moment StylePointsAccumulator::Update (0x8018E634) puts on the list it
// hands FEUpdateStylePoints: made, ACTIVE, DONE or ABORTED, and not flagged
// (a moment Cancel flags goes on the frame after).
typedef struct {
	int   type;        // StyleMoment +0x0C, eStyleMomentType
	int   state;       // +0x14
	float mult;        // +0x1C
	float points;      // +0x24
	u32   nameHash;    // the record's +0x00, its English.bin key
	const char* name;  // that string (style_load's lookup)
	bool  continuous;  // the record's +0x06
} StyleUp;

// The last style_update's list, in moment order, copied into `out` (at most
// `cap`), and emptied: what the HUD takes once a frame. 0 when nothing was up
// or the list has been taken.
int style_take_up(StyleUp* out, int cap);

// ---- The AI racers' own accumulators (#268 E step 5). In a drift retail
// gives every AI RacingCar a StylePointsAccumulator and MiscStyleManager of
// its own (UpdateDriftScore under g_tweakAIDriftOpponents, 1 on the disc), so
// each scores its own drift. style_ai_add makes one for `car` with the
// moments style_begin's race kind runs, listening to its collisions; it
// returns the car's slot (1..7), -1 when there is no room or no style_begin.
// The slots last until style_end. style_ai_update steps that accumulator
// (call it once a frame, before its DriftManager); style_select makes `slot`
// (0 the player) the one the calls below read and drive, and returns the one
// that was.
int  style_ai_add(const CarSim* car);
void style_ai_update(int slot, float dt);
int  style_select(int slot);

// ---- What DriftManager (drift.c) reads and drives on the selected
// accumulator, the player's unless style_select picked another. Run it after
// style_update, as RacingCar::Update runs UpdateDriftScore after the
// accumulator's Update; the times are that frame's.

// One moment's fields, by eStyleMomentType. False when the race kind has
// not made it (StylePointsAccumulator's slot is empty).
typedef struct {
	int   state;     // StyleMoment +0x14, STYLE_STATE_*
	float mult;      // +0x1C
	float points;    // +0x24, what the one running has built up
	float banked;    // +0x2C, every finished one's points, whole
	float maxDur;    // +0x40, the longest, in seconds
	float maxPts;    // +0x44, the most points
} StyleMomentView;
bool style_moment(int type, StyleMomentView* out);

// StylePointsAccumulator::TriggerMoment, StyleMoment::SetMultiplier,
// AbortMoment, ClearMultiplier and ForceFinishAll (0x8018E914: every ACTIVE
// moment banked, every ACTIVATING one dropped).
void style_moment_trigger(int type, float intensity, float value);
void style_moment_set_mult(int type, float mult);
void style_moment_abort(int type);
void style_moment_clear_mult(int type);
void style_force_finish_all(void);

// A StyleMomentsInfo record's +0x00 (the name's English.bin hash) and +0x08,
// which the King drift levels (#8..#14) use for their points.
u32   style_record_name(int type);
// The name style_load read for the record keyed `hash` in English.bin (the
// HUD's King drift line), NULL when no record has it.
const char* style_name_for_hash(u32 hash);
float style_record_value(int type);

// MiscStyleManager's last collision: the seconds since it (99999.9 with none,
// GetTimeSinceLastCollision), its impulse (+0x40), its type (+0x44: 0 two
// cars, 3 anything else), whether it was traffic (+0x48), and DirtyCleanRun.
float style_time_since_collision(void);
float style_last_impulse(void);
int   style_last_collision_type(void);
bool  style_traffic_hit(void);
void  style_dirty_clean_run(void);
