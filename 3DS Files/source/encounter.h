#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "carsim.h"
#include "opponent.h"
#include "traffic.h"

// The free-roam racers (TASKS.md #268 H step 1): the two AI cars career free
// roam always carries, which cruise the city's racing lines near the player
// and are the cars an Outrun is raced against (step 2).
//
//   AddRandomEncounterCars    RaceStarter (0x8011A7D8), opponent.c's
//                             opponent_build_encounter: two random cars
//                             matched to the player, both inactive
//   RandomEncounterState      the global at 0x803B82EC: a timer (Init 30 s,
//                             Reset 0) that Tick (0x80127998) runs down in
//                             career free roam, and once it is out and
//                             CheckForRightConditions (0x80127890) holds --
//                             the stage, its outruns won against StageData
//                             +0x40, no world challenge -- state 1, race type
//                             7: an encounter may happen
//   RandomEncounterManager    Race +0xC34 (Update 0x8003CF54): state 0 resets
//                             the state, 1 waits for CanTrigger, 3 drives the
//                             cars -- TryToPlaceInactiveCars (0x8003D4B4) puts
//                             an inactive car on the racing line 250..500 m up
//                             the road from the player at most every 5 s,
//                             MakeOpponentsDriveNearPlayer keeps each one's
//                             AIGoalOffsetPursuit 10 m ahead of the player and
//                             5 m to the side it is on, and
//                             TryAndDeactivateOldOpponents (0x8003D7F8) takes
//                             one off the road 30 s after it was placed once
//                             it is 250 m from the player
//   AIGoalOffsetPursuit       type 9: the racing line, signposts taken only
//                             when a racer must (sign type 2), the speed off
//                             the line held to GetSpeedLimit's graph of the
//                             player's speed by distance (60 mph floor within
//                             100 m, 40 beyond), and AIActionDriveAtOffset
//                             alongside the player within 30 m of the goal
//
// PORT: the world is only paged in round the player (traffic.c's
// TR_PHYS_RANGE), so a racer further than ENCOUNTER_PHYS_M from the player
// rolls along its line without physics, as a far traffic car does, and drops
// onto the ground when the player comes within range. The free-roam lines'
// speed bytes are all 100 (a placeholder), so the speed along the line is
// planned off its curvature with the class's grip and braking, as a race's
// profile is (aidriver.c).

// The Outrun (#268 H step 2): race type 7, a 1 v 1 against the racer the
// player engaged.
//   GetCarThatCanBeEngaged    (0x8003DAB0), every frame of manager state 3:
//                             the first racer on the road ahead of the player
//                             within 30 m (|dz| <= 5 m), at 30 mph or more,
//                             within 10 mph of the player and under 10
//                             degrees off its heading. NotifyPlayerCanTrigger
//                             (0x80127B28): state 2 with one -- engage event
//                             9, ICON_RANDOM_ENCOUNTER, up -- 3 without
//   RequestGetAway            (0x8022A26C) on D-pad Up while it is up, and
//   StartGetAway              (0x8022A278) the next frame: manager state 4,
//                             the engaged racer is racer 0 and the player
//                             racer 1, and the other racer blinks for 2 s
//                             and poofs (FinishCar reason 7)
//   AIGoalGetAwayFromCar      the leader when it is the AI: the racing line,
//                             DecideSignpostAsGetAwayCar's signposts, no
//                             speed limit; AIGoalChaseCar the follower when
//                             it is: DoPathfinding to the leader's route point
//   UpdateGetAway             (0x80227C4C) each follower's gap; Race::Update
//                             (type 7) swaps the lead, finishes a follower
//                             300 m (1000 ft) back -- reason 7 -- and the one
//                             left, reason 6; an AI that finishes blinks and
//                             poofs whoever won
//   EndGetAway                (0x8022A594), the player's finish: main.c's
//                             NotifyRandomEncounterFinished, and manager state
//                             5 clears the road before state 0
#define ENC_REASON_WON   6
#define ENC_REASON_LOST  7
#define ENC_PATH_MAX     96

#define ENCOUNTER_CARS   2     // AddRandomEncounterCars(2)
#define ENCOUNTER_NODES  48    // the racing line walked ahead of a car
#define ENCOUNTER_PHYS_M 200.0f

// What CheckForRightConditions and CanTrigger read of the career.
typedef struct {
	bool  career;          // DAT_803CE800 == 1, a career game
	bool  freeRoam;        // game state 6 and no race on (DAT_803FCA54 == 0)
	int   stage;           // DAT_803BB288
	int   outrunWins;      // the stage's RuntimeCareerStageInfo +0xDC
	int   outrunCap;       // the stage's StageData +0x40, -1 for none
	bool  hasCareerCar;    // DAT_803BBED0, PlayerCareerState +0x7E2C
	bool  stage0Timeout;   // DAT_803BBED4, +0x7E30
	bool  worldChallenge;  // WorldChallengeState::IsActive
} EncounterCareer;

// One point of a racer's line down the road.
typedef struct {
	int   point;
	bool  fwd;
	float pos[3];
	float dir[2];      // the way it is driven
	float s;           // metres along the walk
	float lo, hi;      // the road's edges, metres right of the line
	float v;           // the cornering limit there, m/s (< 0 until known)
} EncounterNode;

typedef struct {
	EncounterNode node[ENCOUNTER_NODES];
	int   nNodes;
	int   cur;           // the node the car is past
	bool  ended;         // a dead end or a route's end: no more nodes
	int   walkPoint;     // where the walk has got to
	bool  walkFwd;
	float progress;      // metres along the walk
	float lat;           // the car's metres right of the line
	float driveL;        // the drive target's L, eased toward `goalL`
	float goalL;
	float speed;         // the off-page mover's speed, m/s
	bool  phys;          // on the ground under physics (near the player)
	int   action;        // AIActionDriveAtOffset +0x208: 1 alongside
	float offsetL;       // SetOffsetPursuit's lateral offset
	float physSec;       // SetToPhysicsMode's 5 s
	float playerDist;
	float playerD, playerL;   // GetRelativePositionTo: the car ahead / right of the player
	// RaceCarController::Think's very-stuck test (opponent.c): the ground
	// made along the line every OPPONENT_STUCK_SAMPLE_SEC, the last ten kept.
	float stuckSec;      // the sample's clock
	float stuckRef;      // `progress` when it opened
	float stuckV[10];
	int   stuckN, stuckIdx;
	int   resets;
	// AIActionGetUnstuck (opponent.c's, TASKS.md #517).
	bool  unstuck;
	float unstuckSec;    // the action's age
	float sinceUnstuck;  // JustCalledGetUnstuckAction's clock
	float sinceReset;    // since placed on the ground or reset
	int   hitWho;        // AIActionIJustGotHit's car while it has one, else -2
} EncounterRacer;

// The getaway: Race's two RacingCars while DAT_803FCA54 is 7. Index 0 is the
// engaged AI, 1 the player.
typedef struct {
	bool  on;
	int   ai;            // the engaged racer
	bool  playerLeads;   // SortRacingCarsByLeadership's racer 0 is the player
	float gap[2];        // RacingCar +0x24: metres behind the leader
	int   place[2];      // +0x0C, 0 while racing
	int   reason[2];     // +0x10: ENC_REASON_WON / LOST
	float time[2];       // +0x198, the race time at the finish
	float clock;         // the race clock since StartGetAway
	float dist;          // GetGetAwayDistance
	float logSec;
} EncounterGetaway;

typedef struct {
	bool  on;
	OpponentField* f;
	// RandomEncounterState
	float timer;         // +0x11B8
	int   state;         // +0x11BC
	int   raceType;      // +0x11B4
	// RandomEncounterManager
	int   mgr;
	float clock;         // the WorldTimer, s
	float placedAt[ENCOUNTER_CARS];   // +0x11C4 stride 0x10
	int   mode[ENCOUNTER_CARS];       // +0x11C8: 1 behind / ahead, 2 left, 3 right
	EncounterRacer r[ENCOUNTER_CARS];
	float carTop[ENCOUNTER_CARS];
	float grip[ENCOUNTER_CARS], brake[ENCOUNTER_CARS];
	u32   rng;           // GuessTargetDestinations' junction coin (PORT: its own)
	float dbgPlaceM;     // DbgEncounterPlaceM: the placing distance in place of 250 m, 0 = retail
	EncounterRacer pilot; // DbgFreeRoamPilot's walk (encounter_debug_pilot)
	bool  pilotOn;
	float pilotLogSec;
	// The Outrun.
	int   engaged;                     // Player +0x244: the offer's racer, -1 for none
	bool  engageFlag[ENCOUNTER_CARS];  // +0x11CC
	bool  requested;                   // Race +0xC2C, RequestGetAway
	bool  poofed[ENCOUNTER_CARS];      // Car +0x998
	int   goal[ENCOUNTER_CARS];        // the AIGoal type: 9 OffsetPursuit, 6 GetAwayFromCar, 5 ChaseCar
	EncounterGetaway ga;
	float playerXY[2];                 // this frame's, for the signpost decisions
	// ChaseCar's path (DoPathfinding): route points from the car's next one.
	int   pathPt[ENC_PATH_MAX];
	bool  pathFwd[ENC_PATH_MAX];
	int   pathN, pathAt;
	float chaseSec;
	int   pathFails;
	// EndGetAway's result, for main.c to take once (encounter_take_result).
	bool  resultReady;
	bool  resultWon;
	float resultTime;
	int   offerLogged;                 // the racer last logged as on offer, -1 none
	// For the log.
	int   placed, deactivated, placeFails;
	float logSec, telSec, traceAt;
	bool  condLogged;
} Encounter;

// What a frame needs. `world[i]` is racer i's CarSimWorld (its own topology
// coordinate); `traffic` may be NULL.
typedef struct {
	CarSim* player;
	const CarSimWorld* world[ENCOUNTER_CARS];
	Traffic* traffic;
	EncounterCareer career;
	bool  metric;        // DAT_803BC268 == 1: GetGetAwayDistance is 300 m, else 1000 ft
	float playerRatio;   // Player::GetSpeedRatio, for DecideBoostFactor (#255)
	float dt;
	void (*log)(const char* msg);
} EncounterFrame;

// The field opponent_build_encounter made, every car inactive:
// RandomEncounterState::Init (the timer at 30 s, or `firstSec` when >= 0) and
// the manager at state 0. Each car's driver is armed here.
void encounter_open(Encounter* e, OpponentField* f, const CarDb* db, float firstSec,
	void (*log)(const char* msg));
void encounter_close(Encounter* e);
void encounter_update(Encounter* e, const EncounterFrame* fr);

// Whether racer `i` is on the road (drawn, collided with).
static inline bool encounter_active(const Encounter* e, int i) {
	return e && e->on && e->f && i >= 0 && i < e->f->count && i < ENCOUNTER_CARS &&
		e->f->car[i].simReady && !e->f->car[i].inactive;
}

// The player's car has hit racer `i`: SetToPhysicsMode, the PhysicsMover for
// 5 s.
void encounter_player_contact(Encounter* e, int i);
// Free-roam racers `a` and `b` have hit each other (carsim_car_contact
// between the two): each one's IJustGotHit (TASKS.md #517).
void encounter_car_contact(Encounter* e, int a, int b);

// DbgFreeRoamPilot (PORT, a test aid, not retail): the player's car carried
// along the racing line from where it is at `speed` m/s (slower for the
// corners), 2 m right of it, on the walk a racer's line takes -- for watching
// AIActionDriveAtOffset with no driver. False while there is no line to put
// it on, when the caller drives the car as usual.
bool encounter_debug_pilot(Encounter* e, CarSim* player, const CarSimWorld* w, float speed,
	float dt, void (*log)(const char* msg));

// TrafficRace's racers for traffic_update in free roam: TooCloseToAnotherCar
// walks every car in the world, these two among them.
void encounter_traffic_racers(const Encounter* e, TrafficRace* rc);

// IsAvailable (state 2) with a car engaged: the Outrun is on offer, engage
// event 9 up and D-pad Up starts it.
static inline bool encounter_offer_up(const Encounter* e) {
	return e && e->on && e->mgr == 3 && e->state == 2 && e->engaged >= 0 && !e->requested;
}
// RequestGetAway: the race starts on the next encounter_update. False while
// nothing is on offer.
bool encounter_request_getaway(Encounter* e, void (*log)(const char* msg));
// Race type 7: from StartGetAway to EndGetAway.
static inline bool encounter_racing(const Encounter* e) {
	return e && e->on && (e->ga.on || e->requested);
}
// GetDistanceFromLeader for the player: <= 0 ahead by that much, > 0 behind;
// never under -GetGetAwayDistance.
float encounter_player_gap(const Encounter* e);
// RenderGetAwayArrow's target: the leader's car while the AI leads.
bool encounter_leader_pos(const Encounter* e, float pos[3]);
// EndGetAway's result once: whether the player won (reason 6) and its race
// time. False until a getaway has ended, and after it has been taken.
bool encounter_take_result(Encounter* e, bool* won, float* raceSec);
