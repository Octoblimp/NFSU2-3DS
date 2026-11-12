#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "aidriver.h"
#include "cardb.h"
#include "draglane.h"
#include "carsim.h"
#include "eventdb.h"
#include "model3d.h"
#include "perfdb.h"
#include "race.h"
#include "rideinfo.h"
#include "ridephys.h"
#include "trackroute.h"

// The AI opponents of a career race (TASKS.md #221, CONTINUE.MD E4/E5).
//
// Retail builds this roster in RaceStarter::AddCareerEventAIOpponents
// (Speed.elf 0x8011A22C), which this module is the port of. For each of the
// event record's `opponentCount` CareerEventOpponentData entries it:
//
//   1. decides the performance measure the opponent is built to, from the
//      entry's own +0x02 mode (EventOppMatch in eventdb.h);
//   2. picks the car -- PickRandomCarsOfSomeSort (0x80119F24) when the entry
//      says RANDOM, RideInfo::FillWithPreset otherwise;
//   3. names the driver, takes its skill byte for the current difficulty, and
//      hands the lot to DriverInfo(id = 100 + i, type 2 = AI, ride, name,
//      skill, flag), which RaceParameters::AddDriverInfo appends.
//
// RaceCoordinator::InitCars (0x8021BE84) is what turns that roster into a
// grid. It calls CalculateRacerPositions(6, locations) (0x8021BB94), which
// fills six RacerStartLocations in marker-slot order 0..5, then hands
// location[p - 1] to the driver whose DriverInfo +0x03 start position is p.
// The player is position 1 and opponent i is position i + 2, so opponent i
// lands on the ForwardStartingPosition marker with slot i + 1 -- which is
// race_grid_slot(..., i + 2, ...), whose `pos` is 1-based. (The i + 2 that
// SetCompositeNameHash is called with is the skin and name index, not the
// grid slot; the two agree by construction.)
//
// What is ported and what is not
// ------------------------------
// The roster read, the car list, the shuffle, the widening performance
// tolerance and the measure every comparison is made in are retail's, line for
// line. The measure comes out of the shipped performance-config container that
// LoaderPerformanceConfigChunk reads -- see cardb_perf_measure() in cardb.h --
// so the figures here are the game's own numbers and not a stand-in.
//
// The seven shipped opponents that name a preset instead of RANDOM (Nikki in
// S5_URL_4, the five crew in S5_URL_9, Caleb in S5_CIRCUIT_36) take it through
// RideInfo::FillWithPreset (rideinfo_fill_with_preset()): the car and every
// part out of presetcar.h's table, with no pick and no bRandom draw. The
// upgrade pass and SyncVisualPartsWithPhysics(true, false) follow as for any
// other entry. A card without the preset (or without GLOBALB.BUN) falls back
// to the random pick, and the log says so.
//
// The upgrade pass that follows the pick is ported (TASKS.md #288), given an
// OpponentMatch. RidePhysicsInfo::MatchPerformance (0x801C91C0) fits the car
// with the parts of the row of its own shipped table (cardb_read_perf_rows)
// whose measure is closest to the target, and rebuilds it --
// ridephys_match_performance(). What the target is depends on the entry's
// mode:
//
//   PLAYER  MatchPerformance(the player's ride, true, !drag) (0x801C9108): the
//           player's own built measure, then a second search moved by the
//           player's handling rating, then the nitrous re-fitted to the
//           player's nitrous level. The class is the player's (GetCarClass).
//           616 of the 679 shipped opponents.
//   STAGE   GetAIPerformanceMatchingParameter (the other 63), or for FIXED the
//           entry's own byte: clamped to 0..10 and taken as a share of the
//           PLAYER's car's measure range (GetMinMaxPerformanceMeasure of the
//           player's car type, not the opponent's), the handling target the
//           same share of Extrema [28]..[29]. The class is
//           CalculateCarClass(that raw measure, true), which clamps a raw
//           measure to 10 and so always comes out 0, the quickest. Retail's.
//   NONE    nothing: the car as picked, stock.
//
// The handling half is skipped in a drag race (TheRaceParameters +0x40, which
// StartCareerEvent sets for a drag event that is not a URL one). A STAGE or
// FIXED car then has its nitrous brought to the player's, MatchUpgradeLevel
// (0x801C48A8) of path 2.
//
// Every car is then dressed (TASKS.md #296, rideinfo.h):
// SyncVisualPartsWithPhysics fits the aero parts that match its tyre, weight
// and suspension levels and, for a car picked at random, BuildRandomRide
// gives it a body kit, hood, spoiler, rims, lights, paint and vinyls by those
// levels; the reused second-to-last keeps the ride before it with a fresh
// BuildRandomSkin. The sim is built on every part the car ended up with, as
// AddRacingCar's RebuildPhysicsInfo does, and the model is assembled out of
// the parts (opponent_load_models). Not ported: the race-type retunes
// RebuildPhysicsInfo makes in a drag, drift or Street X race.
//
// Everything else -- which cars are eligible, the order they are tried in, the
// 0.03 tolerance and its doubling -- is the engine's own, including the detail
// that the career path passes the picker's `allowPlayerCar` true, so an
// opponent may well turn up in the same car as the player.

#define OPPONENT_MAX EVENTDB_MAX_OPPONENTS

// The very-stuck Average's size (RaceCarController ctor, +0x744).
#define OPPONENT_PROGRESS_N 10

// A Collision on a race car's CollisionBody with another race car, as the
// AI's GetMostImportantNearbyCollidingAvoidable (0x8000E61C) reads it: +0x48,
// the size of every impulse Collision::AddImpulse has added, summed; done
// OPPONENT_COLL_LIFE past its last impulse (Collision::Update, as style.c and
// colsfx.c keep theirs). `who` is the other car: an opponent's index, or
// OPPONENT_COLL_PLAYER. Fed by carsim's impulse hook (opponent.c).
#define OPPONENT_COLL_MAX    (OPPONENT_MAX + 1)

// Why an AI racer last shut its nitrous (Opponent nosWhy): Think's two
// stop arms -- ShortTermCanUseNitrousRightNow said no, or
// CanUseNitrousRightNow did -- or the bottle ran dry under it.
#define OPP_NOS_STOP_NONE  0
#define OPP_NOS_STOP_SHORT 1
#define OPP_NOS_STOP_CANT  2
#define OPP_NOS_STOP_EMPTY 3
#define OPPONENT_COLL_PLAYER (-1)
#define OPPONENT_COLL_LIFE   0.3f
typedef struct {
	bool  used;
	s8    who;
	float mag;
	float age;    // seconds since its last impulse
} OpponentColl;

typedef struct {
	char  name[20];     // the driver, out of the career string blob
	int   carType;      // the car table record it drives
	char  carDir[32];   // that record's geometry directory, e.g. "350Z"
	s8    skill;        // the entry's skill byte for the current difficulty
	int   slot;         // grid slot, 1-based -- retail's i + 2 in career
	float perf;         // its car's measure once built, 0..10
	int   parts;        // the performance parts the upgrade pass fitted
	// What EAXCar::CarPreSetup reads for its engine sound (carsfx.h): CSTATE
	// +0x90, 3 / 2 / 1 with engine part 0x18 / 0x13 / 0xF fitted, else 0; and
	// +0xA0, its transmission upgrade level, 0..3. Both 0 for a stock car.
	int   sndEngLevel, sndTransLevel;
	// CARSFX_Turbo +0xAC, its DriverInfo's GetUpgradeLevel(TURBO), 0..3.
	int   sndTurboLevel;
	// RidePhysicsInfo::GetCamberPercent of its build (0x801c9394): 0.5 per
	// installed Pro (0x24) and Extreme (0x23) suspension part, what Render
	// tilts its front wheels by (TASKS.md #296).
	float camber;
	// carClass was decided by the upgrade pass (the player's, or STAGE's 0),
	// which opponent_start_driving keeps rather than classing the measure.
	bool  classSet;

	// Its visual parts (TASKS.md #296): the RideInfo it was dressed in, as the
	// PresetCarSlot array carkit_assemble() takes -- [0] the car's name hash,
	// [slot + 1] each part's. All 0 when it was not dressed, and it is then
	// drawn as the stock car.
	unsigned int ride[CARPARTDB_SLOTS + 1];

	// The model it is drawn with: its own, assembled from `ride`, else the
	// stock car's, which opponents in the same car share; one with neither
	// borrows another's. `ownsModel` says which of them frees it.
	Model3D* model;
	bool     ownsModel;

	// Where it is. The sim is initialised with the car's own drivetrain and
	// rigid body so that the moment the driver is wired up it is a real car
	// and not a marker, and so that rot/origin come out of the same code the
	// player's car is drawn from.
	CarSim    sim;
	bool      simReady;
	// The world it asks for ground and walls, through its own topology
	// coordinate (TASKS.md #280). Left zeroed, the caller's world is used.
	CarSimWorld world;
	RaceCoord coord;    // where it is along the race's route
	// And where it is along its own line (ailine_track, TASKS.md #280), as a
	// distance along the race route: what it steers, reads its speed and
	// measures itself off the line from. `coord` can stand on a branch path
	// down the other carriageway, which the line never does. coord.dist when
	// the field has no line.
	float     lineDist;
	int       raceCar;  // its RacingCar index in the Race, or -1
	float     desired;  // the speed its goal wants, m/s

	// Its DrivingChoice (TASKS.md #280): the offset off the racing line it is
	// making for, metres right of the line (`goalL`, the choice's L at +0x04);
	// the offset its drive target has reached (`curL`), which ChangeCurrentL
	// walks toward the goal at `latRate` metres across per metre along; and
	// the seconds since the choice was made (+0x0C).
	float     curL, goalL, latRate, choiceSec;
	// In a drag (TASKS.md #268 D step 6), the choice is a traffic lane: `lane`,
	// 2..5 (draglane.h), whose lateral `goalL` follows down the road; -1 for
	// none yet, and in any other race.
	int       lane;
	// Its AIActionAvoidAvoidable, while it has one: the car it is avoiding
	// (`avoidWho`, an index into the field, -1 for the player, or
	// OPPONENT_AVOID_TRAFFIC + k for traffic car k) and the time to the hit
	// when the action began (+0x208, CalculateScore's).
	bool      avoiding;
	int       avoidWho;
	float     avoidT;
	// The action's age (+0x214) and the age HasBeenAccomplished waits for
	// (+0x218: avoidT and OPPONENT_AVOID_DONE_PAD_SEC when it began).
	float     avoidSec, avoidDone;
	// Its AIActionAccidentRecovery, while it has one (opponent_accident_*):
	// the action's age and whether it has asked for PleaseResetMeLater (+0x200),
	// and that ask, which the reset after the car's update takes.
	bool      accident, accidentAsked, accidentReset;
	float     accidentSec;
	// Car::GetForwardAcceleration (0x80223448): the body's acceleration along
	// its nose over the last step, m/s^2.
	float     fwdAccel;
	// The race cars its body touched in the last step, which its CollisionBody
	// lists for the next think: bit k for opponent k, OPPONENT_HIT_PLAYER for
	// the player's car (opponent_car_contact, opponent_player_contact).
	u32       hitBy;
	// Its body's collisions with the other race cars (OpponentColl), and
	// AIActionIJustGotHitDragRace (type 0xC) while it runs: `dragHitWho`
	// its target, an OpponentColl `who`.
	OpponentColl coll[OPPONENT_COLL_MAX];
	bool      dragHit;
	s8        dragHitWho;
	// Where it is this frame as the cars round it see it: metres right of the
	// line, its speed down the line, and the half extents of its footprint
	// along and across the line.
	float     lineLat, alongV, halfAlong, halfAcross;

	// The driver. aidriver.h is retail's PsychoDriver; `skillScale` is what
	// the opponent's difficulty entry is worth as a fraction of the route's
	// own speed limit, and `topSpeed` the gearing's ceiling.
	AiDriver  drv;
	float     skillScale;
	float     topSpeed;
	// In a drift (OpponentField `drift`), the DriftDriver RaceCarController's
	// constructor gives an AI racer instead of a PsychoDriver (#268 E step 5).
	AiDriftDriver drift;
	// Its ride's RidePhysicsInfo, as DriftManager::Initialize reads it:
	// GetCarClass (0..3) and GetTheoreticalTopSpeed (m/s), the drift's tune
	// over the ride when it has one; with none (a stock car), its carClass and
	// topSpeed below, as opponent_start_driving sets them.
	int       rideClass;
	float     rideTop;

	// AICarController's desired-speed lookahead (+0x70C) and the timer behind
	// GetNumSecondsSinceLastDesiredSpeedLookaheadSwitch (0x800091A4). Every
	// DifficultyInfo.lookaheadSwitchSec the driver re-rolls how far up the
	// road it reads its speed limit from, uniformly within the metres its
	// skill is allowed -- see DIFF_LOOKAHEAD_JITTER. `rng` is this driver's
	// own bRandom state so one car's draws do not shuffle another's.
	float     lookahead;
	float     lookaheadSec;
	u32       rng;
	// RaceCarController's catch-up boost (TASKS.md #255): +0x778, which
	// multiplies the speed the driver wants, and the seconds since the last
	// DecideBoostFactor recompute (+0x734). sim.catchup gets max(boost, 1).
	float     boost;
	float     boostSec;
	bool      driving;
	int       carClass;           // CalculateCarClass, 0 (quickest) .. 3
	const AiSpeedProfile* prof;   // the field's profile for this car

	// What the driver asked for last frame, kept only for the telemetry line
	// -- there is no other way to see an AI car's pedals from outside.
	float     lastThr, lastBrk, lastSteerDeg;
	// A diagnostic for the telemetry (not retail): the frames since the last
	// tel line on which the wheel went from past STEER_FLIP_DEG one way to past
	// it the other -- the MellowMover's chatter (#268 D) -- and the most the
	// car was on the MellowMover meanwhile.
	u16       steerFlips;
	bool      flipMellow;

	// AIActionGetUnstuck (0x80010D58..0x80011878): backing out of whatever
	// the car has run its nose into. `sinceUnstuck` is RaceCarController
	// +0x710, which JustCalledGetUnstuckAction stamps when the action starts;
	// `sinceReset` is Car::GetNumSecondsSinceLastReset.
	bool      unstuck;
	float     unstuckSec;
	float     sinceUnstuck;
	float     sinceReset;

	// RaceCarController::Think's very-stuck test (0x80027EDC): every 1.5 s
	// the car's speed along the route goes into a ten-sample Average
	// (+0x744), and a full one under 0.2 m/s resets the car, onto the route
	// point at its own z. PORT: the sample is the ground made along the line
	// since the last one (from `progressAt`), not the speed at that instant,
	// so a car rocking back and forth against a wall averages what it is
	// really getting -- nothing (TASKS.md #280). `roadZ` is the height the car last stood on all
	// four wheels near the route point, which is what the reset drops it from
	// when the field has no TrackRoute line (PORT). `blinkSec` is what is left
	// of Car::StartBlinking's 2 s.
	float     progress[OPPONENT_PROGRESS_N];
	int       progressN, progressIdx;
	float     progressSec, progressAt;
	float     roadZ, roadDist;   // and how far along the route it was taken
	float     blinkSec;

	// Which mover the car runs on (TASKS.md #280). RaceCarController::Think
	// (0x80027EDC) keeps a racer on the PhysicsMover while its timer at +0x73C
	// is running -- 20 s from GO, and 20 s again from any knock off the
	// player's car (AICarController::SetToPhysicsMode, 0x80008930) -- and
	// then puts it on its default mover, the MellowMover (carsim.h), as soon
	// as all four wheels are down, re-arming 4 s while they are not. A racer
	// that has finished is put back on the PhysicsMover. `physSec` is that
	// timer in seconds, 0 once it has run out; `finished` is
	// RacingCar::IsFinishedRacing, which the caller owns.
	float     physSec;
	bool      finished;

	// Its nitrous (TASKS.md #281, opponent.c "AI nitrous"): RaceCarController
	// +0x7B4 / +0x7B8 / +0x7BC, when it last opened the bottle, last shut it,
	// and since when the line speeds ahead have not fallen -- on the field's
	// race clock (raceSec), 0 for never, as the ctor leaves them. `nosAction`
	// is the frame's action's +0x1F0 side: true when it is one of the four
	// that call MaybeUseNitrous (JustDrive, Pass, DriveTrafficLane,
	// AvoidAvoidable). `nosLine` / `nosMono` are the route point's line speed
	// (+0x2C) and whether the speeds ahead rise, sampled every
	// OPP_NOS_ROUTE_SEC (`nosRouteSec`). For the log: `nosOn`, the bottle as
	// main.c saw it last; `nosWhy`, why the AI last shut it (OPP_NOS_STOP_*).
	float     nosOnAt, nosOffAt, nosMonoAt;
	bool      nosAction, nosMono, nosOn;
	u8        nosWhy;
	float     nosLine, nosRouteSec;
	// RacingCar +0x28 / +0x2C, its distance refill (RacingCar::Update's AI
	// arm): when it last topped the bottle up (raceSec, 0 before GO's
	// FakeStartlineCrossing stamps it) and how far round it was then, percent.
	float     nosFillAt, nosFillPct;

	// The race's forks (TASKS.md #409, trackroute.h): the one this racer is
	// taking, an index into the field's table, -1 on the line; the branch's
	// lateral where the car was last stepped (`forkLat`), by which its offsets
	// are carried along the branch; and which forks it has decided on this
	// time round, by bit (DecideSignpostAsRacer once a sign).
	int       fork;
	float     forkLat;
	u64       forkSeen;

	// Car::Deactivate (Speed.elf): a free-roam racer (encounter.c) the
	// RandomEncounterManager has taken off the road -- not drawn, not
	// collided with, not moved, parked at DeactiveCarLocation. Activate
	// clears it. Race code never sets it. (A LAN racer who left the race is
	// put there too, #321c.)
	bool      inactive;

	// PORT (TASKS #321c): a LAN racer's car, driven on its own console. No
	// AI: opponent_update steps it on `netIn`, its owner's inputs as last
	// sent, and main.c pulls it onto each state that comes in
	// (carsim_net_pull). `console` is its owner's LAN id.
	bool      remote;
	int       console;
	CarSimInput netIn;
} Opponent;

// The race's traffic as the field's avoidables (TASKS.md #316). Build
// (NearbyAvoidableList, Speed.elf 0x8022F998) walks every Avoidable, traffic
// among them, and keeps the visible ones (IsVisible 0x8022F4EC: on the road
// and not poofed). The caller hands each traffic car's body and
// TrackPathCoordinate every frame (opponent_set_traffic); the rest is the
// field's, the car found on the line as the player's car is.
#define OPPONENT_TRAFFIC_MAX 16    // traffic.h's TRAFFIC_MAX
#define OPPONENT_AVOID_TRAFFIC 100 // avoidWho's first traffic car
typedef struct {
	const CarSim*    sim;
	const RaceCoord* coord;
	bool  on;
	// Where it is on the line, as playerLineDist .. playerHalfAcross; and the
	// race distance it was found at last frame, which a teleport jumps.
	bool  tracked;
	float lineDist, lat, alongV, halfAlong, halfAcross, coordDist;
} OpponentTraffic;

// DecideBoostFactor's race inputs (difficulty.h DiffBoostIn), which the
// caller fills every frame from TheRaceParameters, the Race and the player.
// `valid` false (the default) keeps every boost at 1.
typedef struct {
	bool  valid;
	bool  catchupOn;     // TheRaceParameters +0x7C
	float handicap;      // +0x80[player]
	int   players;       // +0x24
	bool  pointToPoint;  // +0x18
	int   laps;          // +0x20
	int   trafficLevel;  // +0x08
	float lapLength;     // DAT_803F4480, metres
	float raceSec;       // the race clock
	float lapsDone[OPPONENT_MAX];  // each car's RacingCar +0x14
	int   place[OPPONENT_MAX];     // and +0x0A
	float pLaps;         // the player's, the target's
	int   pPlace;
	float pSpeed;
	float pRatio;        // Player::GetSpeedRatio
} OpponentCatchup;

typedef struct {
	Opponent car[OPPONENT_MAX];
	int count;
	OpponentCatchup catchup;
	int difficulty;     // 0..2, the index into an entry's skill triple
	// The shared generator's state as the field was built, which each driver's
	// own state is derived from -- so a race is reproducible but the cars do
	// not all draw the same numbers.
	u32 rngSeed;

	// The route's speed limits, one profile per car (by index into `car`) --
	// what retail bakes into the Routes file, which the shipped Bayview files
	// do not carry (aidriver.h), scrubbed with that car's own acceleration.
	// Built by opponent_start_driving and freed with the field.
	AiSpeedProfile* prof[OPPONENT_MAX];

	// The TrackRoutes matched along the race (trackroute.h): the racing line
	// each car drives and the route points a reset puts it on. Set by the
	// caller after the field is built and before opponent_start_driving; owned
	// by the field and freed with it. NULL drives the race route's own spline.
	AiLine* line;
	// The Routes file `line` was matched from, kept for the player's reset
	// (Player::HandleReset searches the race's TrackRoutes, TASKS.md #279).
	// Owned by the field and freed with it; NULL when it was not loaded.
	TrackRouteSet* routes;

	// Seconds since GO, for Race::GracePeriodIsActive.
	float raceSec;

	// The player's car as one of the field's avoidables (TASKS.md #280): where
	// it is on the line, found the way a car of the field finds itself, and
	// the same figures as an Opponent's lineLat .. halfAcross.
	RaceCoord playerCoord;
	float playerLineDist, playerLat, playerAlongV, playerHalfAlong, playerHalfAcross;
	bool playerTracked;
	// The player's car's collisions with the field's, and its DragController's
	// IJustGotHitDragRace (opponent_player_drag_hit), as an Opponent's.
	OpponentColl playerColl[OPPONENT_COLL_MAX];
	bool playerDragHit;
	s8 playerDragHitWho;

	// The race's traffic this frame, by traffic car index; `trafficCount` 0
	// when the race has none.
	OpponentTraffic traffic[OPPONENT_TRAFFIC_MAX];
	int trafficCount;

	// The MellowMover's friction cap for this race, carsim_mellow_mu(): set by
	// the caller before opponent_start_driving. 0 takes the plain race's 4.
	float mellowMu;
	// g_tweakIsShortTrackRace, a Street X track: the racers' steering limits
	// (aidriver.h). Set with mellowMu.
	bool shortTrack;
	// TheRaceParameters+0x40, a drag: the grid revs on the line before GO
	// (opponent_update). Set with mellowMu.
	bool drag;
	// #511: the race is at its countdown (RACE_STAGE_COUNTDOWN), set by the
	// caller each frame. PsychoDriver::Drive's stage 3 revs every race's grid,
	// not only a drag's.
	bool countdown;
	// GetDefaultMovementMode (0x8021D7C8): in a drag or a drift (DAT_803FCA80,
	// DAT_803FCA84, the race types 4 and 5) a racer's default mover is the
	// PhysicsMover, so it never goes onto the MellowMover. Set with mellowMu.
	// PORT: the two other flags it reads (r13-0x6098, r13-0x608C) are not
	// decoded and taken as 0.
	bool physicsMover;
	// TheRaceParameters +0x44, a drift: every racer drives a DriftDriver
	// (aidriver.h's AiDriftDriver) and slides down the line at no more than
	// 70 mph rather than racing it. Set with mellowMu.
	bool drift;
	// A drag's traffic lanes (TASKS.md #268 D step 6): the racers drive
	// AIGoalWinDragRace's lanes instead of the racing line. Set by the caller
	// after `line` and before opponent_start_driving, from trackroute_lanes()
	// on `line`; the table is owned by the field and freed with it. `lanes.lanes`
	// NULL in any other race.
	DragLanes lanes;
	// The race's forks (TASKS.md #409): trackroute_forks() on `line`, set by
	// the caller with `lanes` and owned by the field; NULL in a drag (retail's
	// DecideSignpostAsRacer takes none there) or where the race has none.
	// `forkAfterSec` is the track's TrackInfo +0xE4 (trackdb.h).
	TrackForks* forks;
	float forkAfterSec;
	// opponent_start_driving's log, for what the racers decide on the way.
	void (*log)(const char* msg);
} OpponentField;

// bRandom's global state (Speed.elf r13-0x7D38 == 0x8046D0A8), which the image
// ships holding this and every draw in the game advances. A caller keeps one
// of these for the whole session and hands it to every build, the way retail
// shares the one global -- a fresh seed per race would put up the same field
// every time.
#define OPPONENT_RNG_SEED 0x12345678u

// What the upgrade pass needs of the player and the race (TASKS.md #288).
// A NULL OpponentMatch, or one without `bunPath`, `pdb` or `player`, leaves
// every opponent stock.
typedef struct {
	const char* bunPath;     // GLOBAL/GLOBALB.BUN: each car's record and table
	const PerfDb* pdb;       // the parts the tables' rows name
	const RidePhys* player;  // driver 0's ride as built, its own parts fitted
	// TheRaceParameters +0x40 or g_tweakIsDragRace: no handling match.
	bool dragRace;
	// +0x44: TuneForDrift over each racer's ride and the drift's physics.
	bool driftRace;
	// +0x58, Street X: TuneForShortTrack over each racer's ride.
	bool shortTrack;
	// +0x78, the race's AI skill 0..2: the DriftStabilizer's strength.
	int aiSkill;
	// Not a career session (FEDatabase +0x125F0 != 1): a drag's AI racers
	// take ShiftSpeedMultipliers[aiSkill] as their clutch speed (#419).
	bool quickRace;
	// GetAIPerformanceMatchingParameter (0x80124598) for the career's current
	// stage, 0..10; < 0 when it is not known, which sends a STAGE entry's car
	// pick down the "match the player" path and leaves its car stock.
	float stageParam;
	// Debug (Config DbgFieldCar, #413): a random pick is this car folder
	// instead, "" = the picker's own.
	char forceCarDir[16];
} OpponentMatch;

// AddCareerEventAIOpponents: fills `f` from the event's roster. Returns the
// number of opponents built, which is 0 for an event with none and for a NULL
// event or car table. `rng` is the generator state above, advanced in place.
// `match` may be NULL (stock opponents), and so may `log`.
int opponent_build(OpponentField* f, const EventRec* ev, const CarDb* db,
	int playerCarType, int difficulty, u32* rng, const OpponentMatch* match,
	void (*log)(const char* msg));

// The Quick Race field: RaceStarter::AddAIOpponentCars (0x8011A664) as
// SetupCircuit (0x8011B230) runs it, then its DetermineStartingPositions(3).
// `count` opponents, each named from QuickRaceCharacterNames, in a random car
// matched to the player's measure (the player's own car allowed, never an
// SUV), with a skill drawn by DecideAISkillLevel for `strength` (the Quick
// Race difficulty, 0..2); MatchAICarPerformanceToPlayer then upgrades each to
// the player's measure (the PLAYER mode above, given `match`), and with two or
// more the last takes the car of the one before it. The player starts first and
// RandomizePlayerStartPositions then swaps it with a random driver:
// `*playerSlot` is the grid position (1-based) the player ends up on, and each
// opponent's `slot` is set to match. Returns the number built.
// GetRandomCharacterNames (0x80119BD0) asked for one name, as
// EventEngageDialog::FillFromWithRandomName (0x800E74BC) asks: a single
// bRandom(53) draw off `rng`, nothing being taken yet.
const char* opponent_random_character_name(u32* rng);
// GetRandomCharacterNames itself: `n` names from QuickRaceCharacterNames, no
// two the same, drawn off `rng` (BuildLeaderBoard asks for four).
void opponent_random_character_names(const char** out, int n, u32* rng);

// The career's grid: DetermineStartingPositions (0x8011ADBC) as
// StartCareerEvent (0x8011D02C) picks its kind by the event's type (+0xF):
//   1, a D-Day event: the player first, the AI behind in roster order;
//   2, Street X or a drift: the AI first in roster order, the player last;
//   3, a circuit, sprint or drag: as 1, then RandomizePlayerStartPositions
//      (0x8011B07C). With one player and any opponents (TheRaceParameters
//      +0x28, the event's +0x7C) it swaps the player's position with driver
//      bRandom(1 + opponents)'s, driver 0 being the player.
// Then, on each track of a multi-track career event but its first,
// ForcePlayerToBeInThisStartPosition (0x8011ACB0): the player takes the place
// it finished the track before in (GetFinishPositionForDriver), swapping with
// whoever held it. `prevPlace` is that place, 0 for none. Sets every
// opponent's `slot`; returns the player's.
int opponent_career_grid(OpponentField* f, int kind, int prevPlace, u32* rng);

// `startKind` is the Setup's DetermineStartingPositions kind: 3 (SetupCircuit,
// SetupDragRace) as above; 2 (SetupDriftRace, SetupShortTrack, SetupURL) the
// AI first in roster order and the player last, with no draw (#419).
int opponent_build_quickrace(OpponentField* f, int count, const CarDb* db,
	int playerCarType, int strength, u32* rng, int* playerSlot,
	const OpponentMatch* match, int startKind, void (*log)(const char* msg));

// PORT (TASKS #321c): a LAN race's field -- the other racers' cars, each as
// its owner built and dressed it (lanrace.h LanRaceCar), on the grid place
// the server gave it. No AI, no upgrade pass: each car's physics is its
// record's, which only carries it between its owner's states. Returns the
// number built; a car whose record is not in `db` is left out.
typedef struct {
	int carType;
	const unsigned int* ride;   // CARPARTDB_SLOTS + 1 hashes, or NULL
	float camber;
	int sndEng, sndTrans, sndTurbo;
	const char* name;
	int slot;                   // its grid position, 1-based
	int console;
} OpponentRemote;
int opponent_build_remote(OpponentField* f, const OpponentRemote* r, int n,
	const CarDb* db, void (*log)(const char* msg));

// AddRandomEncounterCars (0x8011A7D8), what StartCareerFreeRoam and
// StartCareerFreeRoamFromInGame run with 2: GetRandomCharacterNames(8), then
// per car PickRandomCarsOfSomeSort(ride, 100 + i, i + 2, true, NULL, NULL,
// -1) -- the player's measure, the SUV list when the player drives an SUV --
// SetRandomParts, SetRandomPaint, SetRandomPart(0x9A, 2) and
// DecideAISkillLevel(career difficulty); then MatchAICarPerformanceToPlayer(0),
// every car matched to the player and dressed by BuildRandomRide, none
// sharing a ride. Every car starts inactive (encounter.c places them).
int opponent_build_encounter(OpponentField* f, int count, const CarDb* db,
	int playerCarType, int strength, u32* rng, const OpponentMatch* match,
	void (*log)(const char* msg));

// Loads the stock car, CARS/<dir>.nfsc out of `dataRoot` (the game data
// directory as a full path, e.g. "sdmc:/nfsu2data"), for each opponent with
// no model yet -- the caller assembles the ones it can from their rides
// first, each in its own paint and skin (main.c), and every one left here
// has its ride cleared. Opponents in the same stock car share it, ones whose
// car will not load share the first one that did, and an opponent left with
// none is simply not drawn. Returns how many models it loaded.
int opponent_load_models(OpponentField* f, const char* dataRoot,
	void (*log)(const char* msg));

// Puts opponent `i` on the ground at `pos` facing `yaw`, the way the player's
// car is placed. `w` may be NULL, in which case the position is taken as given
// instead of being dropped onto the surface.
void opponent_place(OpponentField* f, int i, const CarSimWorld* w,
	const float pos[3], float yaw);

// Arms the drivers once the field is on the grid: each one takes its car's
// class (the upgrade pass's, else CalculateCarClass over its measure) and a lateral
// offset from the route centre so the field does not start the race stacked
// in one line, and the route's speed profile is built for every class the
// field uses. `db` supplies the fleet's measure range; it may be NULL, in
// which case every car is classed at its stock measure's face value.
//
// How much of the route's limit a given skill is allowed to use comes out of
// DifficultyInfo (difficulty.h) -- the shipped 0x34B00 chunk's own column, not
// a stand-in. difficulty_load() must have run first; without it every driver
// falls back to the skill-9 record and the field races flat out together.
void opponent_start_driving(OpponentField* f, const CarDb* db,
	const RaceRoute* route, void (*log)(const char* msg));

// One frame of the field: each driver picks its point and speed off the route
// and its car is advanced through the same carsim the player's car uses.
// `racing` false holds the grid still through the countdown. `player` is the
// player's car, which the very-stuck reset keeps its distance from and a
// blinking car is checked against; it may be NULL.
void opponent_update(OpponentField* f, const RaceRoute* route,
	const CarSimWorld* w, const CarSim* player, bool racing, float dt);

// Traffic car `k` for this frame's opponent_update: its body, its
// TrackPathCoordinate and whether it is on the road. Set every car up to the
// traffic's count and `trafficCount` with it; a car that is off the road is
// found on the line afresh when it comes back.
void opponent_set_traffic(OpponentField* f, int k, const CarSim* sim,
	const RaceCoord* coord, bool on);

// The player's car has hit opponent `i` (carsim_car_contact between the two):
// AICarController::SetToPhysicsMode with the player's car as the other body
// puts it on the PhysicsMover and runs its 20 s timer again. A knock from
// another AI car leaves it on the mover it is on.
void opponent_player_contact(OpponentField* f, int i);

// Opponents `a` and `b` have hit each other (carsim_car_contact between the
// two): each lists the other as a race car its body touched, for its next
// think's AIActionIJustGotHit.
#define OPPONENT_HIT_PLAYER (1u << 31)
void opponent_car_contact(OpponentField* f, int a, int b);

// AIActionIJustGotHitDragRace for the player's car under its DragController
// (control mode 2, main.c's drag pilot), once a frame after the controller's
// Think. False when the action is not running. True while it runs: the
// caller clears the controller's exclusion and its lane change
// (SetLanesToNotLockOnto(0xff, 0xff), +0x7CC = 0), and `*lane` is the lane
// CalculatePointsToDrive chose, -1 to leave the choice as it is.
bool opponent_player_drag_hit(OpponentField* f, const RaceRoute* route, const CarSim* player,
	int* lane);

// The player's car once it has finished (TASKS.md #268 E): Race::FinishCar
// (0x80229C88) puts it in control mode 3 and engages AIGoalPark, whose
// AIActionStopDriving brakes it to a stop on its DrivingChoice, moving that
// choice only for a car it would meet (opponent.c's stopChoice). `begin` is
// the new controller, the frame the car finishes. Then once a frame after
// opponent_update: `pt` is the steer-to point `ahead` metres on, `*lateral`
// the car's metres right of its drive target, `*choice` (may be NULL) the
// choice's offset off the line. False when the player's car is not tracked.
void opponent_park_player_begin(OpponentField* f, const CarSim* player);
bool opponent_park_player(OpponentField* f, const RaceRoute* route, const CarSim* player,
	float ahead, float dt, float pt[2], float* lateral, float* choice);

// Car::IsBlinking: the car is fresh off a reset, so it is drawn every other
// frame and nothing collides with it.
static inline bool opponent_blinking(const Opponent* o) {
	return o->blinkSec > 0.0f;
}

// How far along the route opponent `i` is, in metres -- what the standings
// are ordered on. 0 when it is not driving.
float opponent_distance(const OpponentField* f, int i);

void opponent_free(OpponentField* f);

// AIActionGetUnstuck and Think's very-stuck test, shared with encounter.c's
// free-roam racers (RaceCarControllers too, TASKS.md #517). The figures are
// the GameCube image's own.
#define OPPONENT_UNSTUCK_LIFETIME   5.0f    // the action's lifetime, 0x80317BCC
#define OPPONENT_UNSTUCK_DESIRED  (-15.0f)  // a racer's speed backing out, 0x80316744
#define OPPONENT_UNSTUCK_GOAL_M    75.0f    // GetGoalFuturePoint's reach (opponent.c)
#define OPPONENT_STUCK_SAMPLE_SEC   1.5f    // the 6000-tick timer at +0x740
#define OPPONENT_STUCK_AVG_SPEED    0.2f    // 0x80317BB8
#define OPPONENT_STUCK_PLAYER_DIST 50.0f    // 0x80317BB4
#define OPPONENT_BLINK_SEC          2.0f    // Car::StartBlinking, 0x8033A558
// AIAction::GetHeadingClassification: `p` / `d` the line's point and way
// beside the car, `eL` / `eR` the road's edges as metres right of the line.
int opponent_heading_class(const CarSim* c, const float steerTo[2], const float p[2],
	const float d[2], float eL, float eR, bool checkSteer);
// AIActionGetUnstuck::CalculateScore past the grace period (`outside`: metres
// outside the road, negative inside), HasBeenAccomplished and HasFailed.
bool opponent_unstuck_score(const CarSim* s, float outside, int headCls, float sinceReset,
	float sinceUnstuck);
bool opponent_unstuck_done(const CarSim* s, const float goal[2], float sinceReset,
	float unstuckSec);
bool opponent_unstuck_failed(const CarSim* s, int headCls, float unstuckSec);


// AIActionAvoidAvoidable's core, shared with encounter.c's free-roam racers
// (RaceCarControllers as well, TASKS.md #517). ChooseNewAction asks for it
// after GetUnstuck and IJustGotHit, through RaceCarController::
// ShouldWeAvoidThisAvoidable (0x800289B4), which passes a car over for a
// second when World::Random(1) comes to difficulty record [3] -- 1 at every
// skill in the shipped data, so never. opponent.c says how a drive target is
// modelled here (AVOID_SAMPLES). The figures are the GameCube image's own.
#define OPPONENT_AVOID_HORIZON     3.0f     // MakeNewChoice / IsDriveable, 0x8031637C
#define OPPONENT_AVOID_HOLD_SEC    0.5f     // a new choice is kept this long, 0x80317374
#define OPPONENT_AVOID_STEPS       10.0f    // the road's width in this many steps, 0x80317378
#define OPPONENT_AVOID_STEP_MIN    1.0f     // but never under a metre, 0x8031737C
#define OPPONENT_AVOID_RATE_MIN    0.05f    // DrivingChoiceMinLateralSpeed, 0x8046CE44
#define OPPONENT_AVOID_RATE_MAX    0.151f   // 0x80317380
#define OPPONENT_AVOID_RATE_STEP   0.05f    // 0x80317384
#define OPPONENT_AVOID_BACK_SEC    0.75f    // TryAndDriveBestLine, 0x80316378
#define OPPONENT_AVOID_BACK_RELV   0.1f     // 0x80316374
#define OPPONENT_AVOID_PLAYER_PAD  -0.5f    // DoWeCareAboutThisAvoidable, 0x80317000
#define OPPONENT_AVOID_TRAFFIC_PAST -10.0f   // 0x803168E0
#define OPPONENT_AVOID_LIST_MAX    15
#define OPPONENT_AVOID_SAMPLES     20
#define OPPONENT_AVOID_PLAN_SPEED  10.0f
#define OPPONENT_AVOID_SIDE_PAD    1.0f
#define OPPONENT_AVOID_NEAR_M      100.0f
#define OPPONENT_AVOID_PACE_EPS    0.001f   // 0x80316934
#define OPPONENT_AVOID_PACE_GAP    3.0f     // 0x80316930
#define OPPONENT_AVOID_PACE_CLOSE  0.25f    // -0.25 at 0x8031693C
#define OPPONENT_AVOID_PACE_MIN    20.0f    // 0x80316944
#define OPPONENT_AVOID_DONE_PAD_SEC 2.0f     // 0x803168A4

// The cars round one racer as its CollisionPredictor sees them (PORT, see
// opponent.c's AVOID_SAMPLES): each one's path at the sample times, metres
// down the line from where the racer is now and metres right of the line.
typedef struct {
	float d[OPPONENT_AVOID_SAMPLES], l[OPPONENT_AVOID_SAMPLES];
	float halfAlong, halfAcross;
	int   id;           // as Opponent.avoidWho
	float relD, relL;   // NearbyAvoidableList's relative D and L
	float speed;        // its speed down the line
	// Already within the padded box of the racer, and how far apart the two
	// paths start across the line (PORT, see AVOID_SIDE_PAD).
	bool  close;
	float sep;
} OpponentAvoidCar;

typedef struct {
	OpponentAvoidCar car[OPPONENT_AVOID_LIST_MAX];
	int n;
	// The racer's own run down the line at each sample, the offsets the road
	// leaves it there, and its footprint following its drive target.
	float d[OPPONENT_AVOID_SAMPLES], lo[OPPONENT_AVOID_SAMPLES], hi[OPPONENT_AVOID_SAMPLES];
	// And how far its fork's branch carries its offsets there (TASKS.md #409).
	float sh[OPPONENT_AVOID_SAMPLES];
	bool road;
	float halfAlong, halfAcross;
	// TryAndDriveBestLine's soonest time for the racer and a nearby car to meet.
	float minT;
} OpponentAvoidScene;

float opponent_avoid_sample_sec(int k);
// WhoWillThisDriveTargetHit: the first sample time the drive target from
// `from` walking to `goal` at `rate` meets one of the scene's cars (`who`);
// past the horizon when none does.
float opponent_avoid_hit_time(const OpponentAvoidScene* sc, float from, float goal, float rate,
	int* who);
// ChooseOffsetFromDrivingLine past its IsDriveable: `minL` .. `maxL` the road
// either side of the line, `curL` the drive target's offset now; the choice
// (`goalL`, `latRate`, `choiceSec`) is changed when an offset is clear for
// `horizon` seconds. False is no offset.
bool opponent_avoid_offset(const OpponentAvoidScene* sc, float minL, float maxL, float curL,
	float* goalL, float* latRate, float* choiceSec, bool preferLeft, float horizon);
// AvoidAvoidable's speed when no offset round `c` is clear; past any speed
// when it asks for none (a traffic car: id OPPONENT_AVOID_TRAFFIC on).
float opponent_avoid_pace(float avoidT, float alongV, float fwdAccel, float speed,
	const OpponentAvoidCar* c);

// AIActionAccidentRecovery (ctor 0x80011FE8, AIAction type 0xD), which a
// racer's ChooseNewAction (ChooseNewActionAsRacer 0x8000BE48, AIGoalOffset-
// Pursuit's 0x8000D320) asks for after AvoidAvoidable. Its scores: GetUnstuck
// 500, AccidentRecovery 500, IJustGotHit 400, AvoidAvoidable 300 (0x804708A4,
// 0x804708A8, 0x8047089C, 0x80470894), so once it runs only GetUnstuck takes
// over. CalculateScore (0x800120B8) takes it on for a car with under two
// wheels down (Car +0x434) that is not SortOfParallelToTrack (0x8001210C: its
// up axis, Car +0xA8, more than 45 degrees off world up); CalculatePointsToDrive
// (0x8001226C) asks every speed 0 and, OPPONENT_ACCIDENT_RESET_SEC into the
// action, PleaseResetMeLater once; HasBeenAccomplished (0x80012198) once it has
// asked, or with all four wheels down and upright again. Its lifetime is -1.
#define OPPONENT_ACCIDENT_UPRIGHT   0.707f  // 0x803167D4 / 0x803167E0
#define OPPONENT_ACCIDENT_RESET_SEC 5.0f    // 0x8031683C
// One step of the action for `o` (its accident* fields): true while it runs,
// and then every speed it asks is 0.
bool opponent_accident_step(Opponent* o, float dt);

// AIActionIJustGotHit's CalculatePointsToDrive (0x80011B20) once
// GetMostImportantNearbyCollidingAvoidable (0x8000E61C) has picked the race car
// it touched (opponent.c's gotHit): for a racer at `myL` on its line, its
// choice `*goalL` / `*latRate` set to clear that car at `theirL`, `relD`
// metres ahead of it; the speed it asks is returned (1e9 for none).
// `headCls` is its GetHeadingClassification (checkSteer false), `player` the
// player's car when that is the one it touched, and `floorOn` whether the
// HIT_FLOOR under the player's speed applies (no race running, or the race
// past its first 10 s).
float opponent_hit_step(const CarSim* s, float myL, float myHalfAcross, float myHalfAlong,
	float* goalL, float* latRate, float theirL, float theirHalfAcross, float theirHalfAlong,
	float relD, int headCls, const CarSim* player, bool floorOn);
#define OPPONENT_HIT_FLOOR_SEC 10.0f   // 0x803167D0
