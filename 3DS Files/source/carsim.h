// Vehicle dynamics for the drivable world (TASKS.md #142, #222).
//
// This is retail's car model, ported from the GameCube Speed.elf: the same
// components, the same order, the same constants and the same per-car data.
// docs/retail_car_physics.md is the write-up it follows, function by function;
// carsim.c names the retail function each piece comes from.
//
//   * PhysicsMover::Move -- Euler substeps of at most 0.0044 s over a rigid
//     body, four wheel + suspension pairs, the engine and the drivetrain.
//   * The rigid body integrates position, orientation, linear and angular
//     momentum in 3D, so the car pitches, rolls, leaves the ground and lands.
//   * Each suspension is a spring and damper standing on the surface under its
//     own wheel; each tyre is retail's slip / load-sensitivity model with the
//     friction cap and brake lock; the engine has its torque and braking
//     curves, the clutch its four states, the gearbox retail's shift points.
//   * The steering is PlayerSteering::DoUndergroundSteering's speed-sensitive
//     lock and rate, then TwoWheelAckermanSteering.
//   * Wall and body-on-ground hits use Contact::CollisionImpulse's numbers.
//   * MellowMover::Move, the second mover an AI racer switches to away from
//     the player: the same rigid body on four sprung wheels, but a plain
//     slip-force tyre under a load x friction cap and an engine read off the
//     wheel speed (carsim_set_mellow).
//
// The per-car data is the CarPhysicsInfo block out of the shipped car record
// (cardb.h, CarPhysInfo). The few PORT parts -- how the world is queried and
// where a body hits the ground -- are marked PORT in carsim.c.
#pragma once

#include <stdbool.h>

#include "cardb.h"
#include "trackpoly.h"

// How the simulation asks about the world. Kept as function pointers so the
// physics does not include world.h and can be exercised against a flat plane in
// isolation; main.c passes the real pager's implementations.
typedef struct {
	// Highest drivable surface under each of `n` (x,y) pairs -- `xy` is n
	// interleaved pairs -- with nothing above `zMax` counting. outZ and outHit
	// are n long and are written by the callee; outHit[i] is false where there
	// is nothing under that point at all. `outN` (3n long) gets the unit,
	// up-facing normal of the face each hit was taken from: the plane the
	// suspension measures against. `outTerrain` (n long, may be NULL) gets
	// each hit's eTerrainType, the byte TopologyCoordinate::GetElevation
	// hands Wheel::CheckTerrain (1 ROAD). The return is how many were found.
	//
	// Batched rather than one point at a time because the world answers this by
	// scanning a section's triangles, and the car asks about four wheels and
	// four body corners every frame.
	int (*groundProbe)(void* ctx, const float* xy, int n, float zMax,
		float* outZ, bool* outHit, float* outN, signed char* outTerrain);

	// Moves a car-shaped footprint from `from` to `to`, correcting `to`'s XY in
	// place. `yaw` orients the footprint and `halfLength`/`halfWidth` size it
	// from the car's real body box. When it returns true it also writes the unit
	// XY normal of the surface that pushed hardest into (`outNx`,`outNy`), and
	// where on the footprint that push touched it as an offset from its centre
	// (`outArm`), which is what lets the hit turn the car (#150).
	bool (*collide)(void* ctx, const float from[3], float to[3], float yaw,
		float halfLength, float halfWidth, float* outNx, float* outNy,
		float outArm[2]);

	void* ctx;

	// What the body collides with (TASKS.md #223): retail's TopologyTree::
	// BuildTrackPolygons over `box` (min x, min y, max x, max y) -- the roads,
	// the walls and every other surface, at most `max` -- as indices for
	// `trackPoly` and, 4 floats each, their boxes. With these the body meets
	// the world as retail's does, its hull against each polygon (carsim.c,
	// "Track polygons"). Left NULL, or with `trackPolys` answering < 0 (the
	// world has no topology), it meets the ground planes and `collide`
	// instead (PORT).
	int  (*trackPolys)(void* ctx, const float box[4], u32* ids, float* boxes, int max);
	bool (*trackPoly)(void* ctx, u32 id, TrackPoly* out);
} CarSimWorld;

// One frame of driver input. Throttle and brake are 0..1 rather than booleans so
// a circle pad can be analogue; `steer` is -1..1, positive turning left to match
// the world's +Z-up, counter-clockwise-positive yaw.
typedef struct {
	float steer;
	float throttle;
	float brake;       // also reverse, from a standstill (RealDriver)
	bool  handbrake;
	bool  shiftUp;     // pressed this frame; read with a manual box only
	bool  shiftDown;
	bool  racing;      // the current Race is at stage 4 (TuneBoostAndDampers):
	                   // a race past GO, and free roam

	// An AI driver sets the steering angle itself. Retail's PsychoDriver::Drive
	// (0x8001FBEC) writes CarDriver +0x1C4 -- the same field PlayerSteering::
	// DoUndergroundSteering fills for the player -- and so skips the player's
	// lock and rate entirely, because its own PsychoSteeringRangeTable and
	// steering-speed tables have already done that job. With `steerIsAngle`
	// set, `steer` is ignored and `steerAngleU16` is that field, 65536 to the
	// turn.
	bool  steerIsAngle;
	u16   steerAngleU16;

	// The rest of an AI driver's CarDriver. PsychoDriver sets the pedals and
	// the gear itself: Drive writes +0x1C8, the gear it wants, every frame --
	// -1 (SetLowGear) leaves the choice to the automatic box, 0 is reverse.
	// With `aiDriver` set, `throttle` and `brake` are those pedals as they
	// are, not RealDriver's brake that becomes reverse, and `gear` is +0x1C8.
	bool  aiDriver;
	int   gear;
} CarSimInput;

// Where on the body a wall or another car touched it -- AIAction::
// GetCollisionClassification (0x8000E564): a side when the contact is past
// 0.95 of the half-width, otherwise in front or behind by which half of the
// car it is on. The numbering is retail's (CollisionClassificationString).
#define CARSIM_TOUCH_FRONT  0
#define CARSIM_TOUCH_BEHIND 1
#define CARSIM_TOUCH_LEFT   2
#define CARSIM_TOUCH_RIGHT  3
// PORT: retail keeps a Collision while the two bodies' boxes stay within a
// margin of each other (CollisionBodyWitness::HasExpired 0x801AD444); here a
// side stays in contact this long after it was last touched, the freshness
// AIActionGetUnstuck::CalculateScore asks of one (0x80316694).
#define CARSIM_TOUCH_GAP 0.25f
// CarSim::touchWith for a wall (Collision::IsWallInvolved).
#define CARSIM_TOUCH_WALL (-1.0f)

// ---- retail's components (internal; carsim.c owns every field)

#define CARSIM_WINDOW_MAX 33

// The most vertices a collision hull may have (carhull.h's CARHULL_MAX_VERTS).
#define CARSIM_HULL_MAX 64

typedef struct {
	float value[CARSIM_WINDOW_MAX];
	float time[CARSIM_WINDOW_MAX];
	int   cap, idx, oldest, count;
	float sum, avg, length;
} CarSimWindow;           // AverageWindow

typedef struct {
	// state
	float angle, w;           // +0x24 / +0x28
	// geometry and data
	float pos[3];             // WheelInfo position, record frame
	float radius;             // WheelInfo +0x10
	const float* tire;        // TireInfo, 5 floats
	// auxiliaries
	float rowC[2][2];         // car-frame fwd / side (the +0xb0 matrix, xy)
	float fwd[3], side[3], up[3], wpos[3];   // the +0x70 world matrix
	float cvel[3];            // +0xf0 contact velocity
	float gpt[3], n[3];       // +0x13c ground point, +0x14c ground normal
	float T;                  // +0x108 drive + brake torque
	float g;                  // +0x120 smoothed ground torque
	float s;                  // +0x118 longitudinal slip
	float slip;               // +0x168
	float Flong, Flat, Fz;    // +0x124 / +0x128 / +0x160
	float F[3];               // +0x12c world force
	float gripUse;            // +0x16c
	// +0x100 / +0x104, which GuessGroundForce keeps for the skid sound
	// (SFXCTL_Wheel): the forward slip while the tyre slides, else 0, and the
	// contact's sideways speed.
	float sndSlip[2];
	float brake, ebrake;      // +0x174 / +0x178
	float grip1, grip2, grip3;    // +0x17c / +0x180 / +0x184
	float longScale, latScale;    // +0x18c / +0x190
	int   locked, dir, sliding, onGround;
	// Car +0x38C [i] (Wheel::CheckTerrain 0x801CF40C): the eTerrainType under
	// the wheel, off the probe that gives its plane, and kept through a miss.
	// The sound reads it (carsfx.h). Retail's drag-race ROAD override
	// (TheRaceParameters +0, g_tweakIsDragRace) has no counterpart: the port
	// has no drag races.
	int   terrain;
} CarSimWheel;

typedef struct {
	const float* info;        // SuspensionInfo, 8 floats
	int   partner;
	float x, xdot, F;         // +0x1c / +0x20 / +0x24
	float preload;            // +0x80
	float attach[3];          // +0x84, from the CG
	float wcpos[3];           // +0x28, wheel centre from the CG
	int   onGround;           // +0x78
} CarSimSusp;

typedef struct {
	float p[3], n[3];         // a point on the surface and its normal
	bool  valid;
} CarSimPlane;

// SetInterestBBox's polygon list (BuildTrackPolygons' 160) and how many
// collision bodies one CreateTrackPolygonCollisionBodies makes (48).
#define CARSIM_TP_MAX       160
#define CARSIM_WITNESS_MAX  48

// A CollisionBodyWitness between the car and one TrackPolygonCollisionBody:
// the polygon, which of the car's two GJK shapes it was made with, and its
// GJKWitness's cached answer.
typedef struct {
	TrackPoly poly;
	u32   id;
	int   wnv;            // the GJKWafer's vertex count: 3, or 4
	float lastPos[3];     // GJKWitness +0xc: where the car was for `dist`
	float dist;           // +0
	float pa[3], pb[3];   // +0xac / +0xbc: the closest points, world
	float hitAt;          // its Collision's last impulse, on `clock`; < 0 none
	u8    box;            // GetGJKPoly gave the hull's box, not the hull
	u8    colliding, deep, resolves;
} CarSimWitness;

typedef struct {
	// ---- pose, the part the renderer and the camera read
	float pos[3];      // ground contact under the CG at ride height, world m
	float yaw;         // rad, 0 = facing world +X (the model's own front)
	float pitch;       // rad, nose-up positive
	float roll;        // rad, right-side-down positive
	float rot[3][3];   // rows: the car's forward, left and up axes in world
	float origin[3];   // the record origin: where the model is drawn from

	// ---- readouts for the HUD, the camera and the debug overlay
	float vel[3];      // world m/s at the CG
	float speed;       // horizontal speed, m/s
	int   gear;        // -1 reverse, 0 neutral, 1..numGears
	float rpm;         // smoothed, as retail's tachometer reads it
	bool  shifting;
	bool  onGround;    // any wheel down
	float lastImpact;  // speed into the last wall or ground hit, m/s
	bool  resetRequest;    // PleaseResetMeLater: on its roof and stopped for 1 s

	// ---- the car this is configured for
	CarPerf    perf;
	CarChassis chassis;
	float      topSpeed;   // m/s: the gearing at redline, capped by the governor

	// ---- retail state (carsim.c)
	float cpi[CARPHYS_CPI_SIZE / 4];   // CarPhysicsInfo, as floats
	s8    boostFlag;
	float cg[3];
	float hRest;       // CG height above the ground at rest
	// The car's collision hull (CarPhysInfo.hull, model space; at most
	// CARSIM_HULL_MAX vertices), what the body meets the ground with. NULL
	// and 0 without one, and the record's body box stands in.
	const float* hull;
	int   hullN;

	// RigidBody
	float rbPos[3], P[3], L[3], damping;
	float rbVel[3], w[3], F[3], T[3];
	float mass, invMass, Iinv[3], IinvZ[3];
	int   zdamp, flag13c, flag140;
	float g, yawClamp, governor, govBoost;
	// Car +0x478, the catch-up boost (TASKS.md #255): RaceCarController::
	// DecideBoostFactor's max(boost, 1), which TuneBoostAndDampers hands
	// SetBoost every step -- the engine's torque, the aerodynamics' speed,
	// govBoost, each tyre's grip2 and the Mellovator's +0x13C / +0x140. The
	// owner sets it; 0 (never set) reads as 1, as does anything under 1.
	float catchup;

	CarSimWheel wheel[CARPHYS_WHEELS];
	CarSimSusp  susp[CARPHYS_WHEELS];
	CarSimPlane plane[CARPHYS_WHEELS * 2];   // 4 wheels, 4 body corners
	int   corner[CARPHYS_WHEELS];            // which body point each slot probes
	// CarState +0x328 [i]: the ground-penalty impulse wheel i took this step,
	// summed over the substeps (Suspension::ApplyGroundPenaltyImpulse's
	// return, through PhysicsMover::BottomOut; PhysicsMover::Move 0x801C0458
	// clears it first). What the bottom-out sound reads (carsim_bottom_out).
	float bottomPen[CARPHYS_WHEELS];
	// The hydraulics (TASKS.md #313). hydBounce / hydExtend are the driver's
	// RealDriver +0x1D0 / +0x1E0, which the owner hands in each frame
	// (carsim_set_hydraulics): each corner's bounce strength and how far up
	// it is asked to be, 0..1. hydExt is CarState +0x3BC, each corner's
	// extension in metres (at most 0.2), hydState PhysicsMover +0x74 (0 down,
	// 1 held up, 2 rising, 3 lowering) and hydFired CarState +0x3CC, the
	// corners a bounce kicked this frame.
	float hydBounce[CARPHYS_WHEELS], hydExtend[CARPHYS_WHEELS];
	float hydExt[CARPHYS_WHEELS];
	int   hydState[CARPHYS_WHEELS];
	int   hydFired[CARPHYS_WHEELS];
	// Car's backfire state (carsim_backfire): +0x4D0 the gear ChangeGear saw
	// last, +0x4D4 whether the clutch was engaged then, +0x9DC when the gear
	// last changed (WorldTimer; here on `clock`), +0x914 the neutral rev's
	// arming. Car's ctor (0x8021DE78) starts them neutral, disengaged, armed.
	int   bfGear, bfEngaged, bfArmed;
	float bfAt;

	// Engine
	float engW, engThr, engMult, engExt;
	int   limiter;
	// Engine+0x8C, a drag's engine heat (carsim_set_heat): -1 while it does
	// not build, 0..1.2 while it does, 2 once the engine has blown.
	float heat;
	// Engine+0x98..+0xAC, ComputeTurboPressure's: the largest of EngineInfo's
	// nine turbo values (0 without a turbo), the boost (+0x9C, what
	// Car::GetTurboBoost hands the TurboMeter), the state (0 at rest, 1
	// building, 2 the blow-off's wobble), and the wobble's start, clock and
	// last turn.
	float turboMax, turboBoost;
	int   turboState;
	float turboFrom, turboT, turboLast;

	// DriveTrain
	float clutch, diffW, clutchT, rpmSlip, split;
	int   dtGear, desired, clutchState, dtShifting, driverGear;
	float mod78, mod7c, mod80;
	float up[10], down[10];
	u32   rng;
	// The drag's side of the DriveTrain (carsim_set_drag / _shift_tuning):
	// TheRaceParameters+0x40, which CheckPerfectShift and GetShiftUpRPM test;
	// +0xCC..+0xE0, the ranges ChooseNewShiftModifiers draws mod78, mod7c and
	// mod80 from ([0] up, [1] down, [2] third's up); +0xE4, the perfect-shift
	// range's scale; +0xE8, the clutch speed's; +0xF0, the car class
	// (CalculateCarClass, 0 the fastest). +0x68 is the bonus the last shift
	// earned (GetPotentialShiftBonus), +0x64 when it began to pay (on
	// `clock`, 0 while it does not). shifts / perfectShifts count the
	// player's upshifts and the ones that earned the full bonus, for the
	// results screen's PRPerfectShifts.
	bool  drag;
	float modLo[3], modHi[3];
	float shiftRangeMul, clutchMul;
	int   carClass;
	float shiftBonus, bonusAt;
	int   shifts, perfectShifts;
	// RealDriver+0x294, the start line (carsim_set_start_line): the box held
	// in neutral and the brake on, the throttle free to rev. +0x2AC, the
	// launch timer SetLowGear starts in a drag, in WorldTimer ticks of
	// 1/4000 s. drvThr is CarDriver+0x1F0, the throttle the driver set last
	// step, which a drag's neutral rev is rate-limited from.
	bool  startLine;
	int   launchTicks;
	// The dyno (carsim_set_dyno): the body held at dynoXY while its momentum
	// stays, so the car's speed is the rollers' (#417 E). PORT: its height
	// and attitude are held too (dynoZ, dynoRot), as it stood at the start.
	bool  dyno;
	float dynoXY[2];
	float dynoZ;
	float dynoRot[3][3];
	// #494 PORT: the rollers' own drag on the held momentum, m/s^2, while a
	// pull coasts down (carsim_set_dyno_drag); 0 otherwise.
	float dynoDrag;
	float drvThr;
	// Car+0x964: a drag's car totaled by a crash (CheckForWildCollision's
	// drag arm, carsim.c dragTotal); carsim_set_drag clears it.
	bool  totaled;
	// The drift (carsim_set_drift): TheRaceParameters+0x44 and the race's
	// AI skill +0x78, which picks every car's DriftStabilizer strength
	// (MiscCarTunings +0x28). yawAccel is Car+0x120, the z of Car+0x118 that
	// Car::FinishMovement (0x8021FB80) sets each frame to the world angular
	// velocity's change over the frame; prevWz is Car+0x530, the rate it was
	// taken from.
	bool  drift;
	int   driftSkill;
	float yawAccel, prevWz;

	// Aerodynamics
	float aeroThr, aeroDown;

	// PhysicsMover / CarState
	float thr, brakeIn, ebrakeIn, ebrakeRecovery;
	float vfwd, speedAbs, rpmSmooth;
	int   wheelsDown, reverseFlag;
	// CarState+0x434, the handling mode: 0 with Stability Control on, 1 with
	// it off -- PlayerSettings' byte, copied to the car by Player::InitForRace
	// (0x80219520). carsim_init sets 1, Default__14PlayerSettings' value; the
	// caller sets the player's, and in a race gives every AI racer the same
	// (SetAIRaceCarHandlingModesFromPlayers 0x80018228).
	int   controlMode;
	// RealDriver+0x2a0, the box: 0 automatic, 1 manual (carsim_set_transmission).
	int   transmission;
	int   dousing;
	float roofTimer;
	bool  touching;
	// Car+0x38, the dampening mode (Car::SetDampenMode 0x80223FF4): 1 after a
	// wild hit (Collision::CheckForWildCollision), the one mode in which an
	// impulse turns the car at its full inertia; 2 while something touches the
	// body; 0 otherwise. dampenQuiet is +0x544, how long mode 1 has been quiet
	// on four wheels (< 0 while it is not counting). contacts is Car+0x3c's
	// +0x16, what the body touched last frame: walls and ground in
	// carsim_update, other cars in carsim_car_contact.
	int   dampenMode;
	float dampenQuiet;
	int   contacts;
	// How many wild hits have put the car into mode 1 (carsim.c wildCollision,
	// two cars only, as IsTwoCarCollision asks). The player's crash camera
	// (main.c, TASKS.md #335) is CheckForWildCollision's last arm: the caller
	// watches this for a change and stamps it, Car+0x540's WorldTimer.
	unsigned wildHits;
	// Which side of the body is being touched, for the AI (CARSIM_TOUCH_*):
	// touchStart is when the current run of contact there began (Collision
	// +0x08), touchLast when it last touched (+0x0C), both on `clock`.
	// touchLast < 0 for no contact since the car was placed. For the last
	// touch there, touchX is where it was across the car, as CarState::
	// WorldToCar (0x801AA6E4) gives it (+ to the right), and touchWith what it
	// was against: CARSIM_TOUCH_WALL, or the other car's speed (+0x440) --
	// what Drive's steer-away asks of a Collision (0x80020474..0x800204F4).
	float touchStart[4], touchLast[4];
	float touchX[4], touchWith[4];
	float ugly;
	float clock;
	// CarCollisionBody +0x134, on `clock`: when the body last touched something
	// IsYawControlInducingCollisionBody (0x801AD56C) passes -- another car, a
	// prop, or a track polygon flagged a wall -- as CollisionBodyWitness::
	// TestForContact (0x801AD7C0) stamps it. The constructor stamps it too.
	// CalculateMaxSteering's post-collision block reads it (TASKS.md #337).
	float yawHitAt;
	// RidePhysicsInfo::GetTheoreticalTopSpeed (0x801C8FE0), m/s: the first
	// 3 mph step where the gears' acceleration falls under the drag, capped at
	// the governor. carsim_init and carsim_retune set it.
	float theoTop;

	// CarState's nitrous bottle, +0x424..+0x430, in WorldTimer ticks of
	// 1/4000 s as retail keeps them (carsim.c, "Nitrous"): the bottle's size,
	// one shot of it, what is left, and when it was opened (on `clock`), 0
	// while it is shut. nosLevel is Engine+0x84, the step's IsNitrousOn.
	int   nosCap, nosShot, nosLeft, nosOnAt;
	int   nosLevel;

	// Car +0x4E8 / +0x4F8: each rim spinner's angle (rad) and spin rate,
	// lagging its wheel's (Car::UpdateSpinners, in carsim_update).
	float spinnerAngle[CARPHYS_WHEELS], spinnerRate[CARPHYS_WHEELS];

	// PlayerSteering
	u16   steerU16;
	float steerLastIn, steerLastMax;
	int   slideState;
	CarSimWindow winA, winB;
	float steerFL, steerFR;

	// MellowMover (TASKS.md #280): the cheaper mover retail runs an AI racer
	// on away from the player (carsim_set_mellow). While `mellow` is set the
	// wheel, suspension and gear fields above hold the Mellovator's state;
	// the fields here are the rest of it. carsim.c, "MellowMover", has each.
	int   mellow;
	float mlRating;           // Mellovator +0x144, the friction rating
	float mlRpm;              // +0x120
	float mlBrake;            // +0x12C
	float mlMu;               // ResolveForces' friction cap for this race
	float mlFc[CARPHYS_WHEELS][2];   // +0x70, each tyre's force, wheel frame
	float mlPen[CARPHYS_WHEELS];     // +0xD0, how far past the bump stop
	// CarState +0x3DE, degrees: the wheels as drawn, which both movers turn
	// toward steerU16 at 120 degrees a second.
	float mlSteerVis;
	// Aerodynamics [10] / [11]: what MellowMover::Move scales drag and
	// downforce by. Both stay 1 for the PhysicsMover.
	float aeroScale, aeroSurf;

	// Retail's track-polygon collision (carsim.c, "Track polygons", #223).
	// BuildBoundingVolumes (0x801AE39C) over the hull from the CG: its box
	// (from 0 out), the radius, the two slab heights TrackPolygonCheck reads
	// (+0xD0 below, +0xD4 above) and the box's middle (+0x10C).
	float tpMin[3], tpMax[3], tpR, tpBelow, tpAbove, tpCentre[3];
	// DynamicCollisionBody::BeginMove: the pose, velocity and length of the
	// move the collisions back up through, and the world box's middle at its
	// start (+0xFC), which tells a two-sided polygon's side.
	float mvRot[3][3], mvPos[3], mvVel[3], mvDt, mvCentre[3];
	// SetInterestBBox's cached box and the polygons in it; tpN < 0 until the
	// first fill.
	float tpBox[4];
	int   tpN;
	u32   tpId[CARSIM_TP_MAX];
	float tpBoxes[CARSIM_TP_MAX][4];
	CarSimWitness wit[CARSIM_WITNESS_MAX];
	int   witN;
	// The driver is not the player's (CarSimInput.aiDriver, kept from the last
	// step; traffic on its mover too): CarCollisionBody::GetGJKPoly's driver
	// type test, read when a car meets another car.
	bool  aiDriven;
	// The last step's input, which CarCollisionBody::Move runs the mover with
	// again after another car backs this one up (carsim_car_contact); and
	// whether that step was carsim_traffic_pose's instead, a pose put down
	// from outside with no mover here to run.
	CarSimInput mvIn;
	bool  posed;
} CarSim;

// Points the simulation at one car and parks it at (`pos`,`yaw`). `phys` is the
// car's retail physics block; when it is NULL or invalid the 350Z's block is
// used, so a missing table yields a stand-in car and not a dead one. `perf` and
// `chassis` are kept for the HUD and the camera, with the same fallback.
void carsim_init(CarSim* s, const CarPerf* perf, const CarChassis* chassis,
	const CarPhysInfo* phys, const float pos[3], float yaw);

// Advances one frame, as PhysicsMover::Move: Euler substeps of at most 0.0044 s.
void carsim_update(CarSim* s, const CarSimInput* in, const CarSimWorld* w, float dt);

// Puts the car upright on the surface under `pos` (a ground point) facing `yaw`
// and stops it -- for a spawn, a reset or a teleport.
void carsim_place(CarSim* s, const CarSimWorld* w, const float pos[3], float yaw);

// RealDriver::SetTransmission (0x800256D0) for PlayerSettings' Transmission,
// which Player::InitForRace hands the driver: 0 automatic, 1 manual. A manual
// box starts in the gear the car is in, and CarSimInput.shiftUp / shiftDown
// then move it a gear at a time. carsim_init leaves the box automatic.
void carsim_set_transmission(CarSim* s, int manual);

// The drag (#268 D). carsim_set_drag is TheRaceParameters+0x40 for this car:
// on, the box may shift up to the limiter and not the redline
// (GetShiftUpRPM) and an upshift is scored (CheckPerfectShift); off clears
// the score and the heat. carsim_set_shift_tuning is what the PhysicsMover
// ctor (0x801BF960) hands the DriveTrain: the car class (CalculateCarClass,
// 0..3), the perfect-shift range's scale (+0xE4, the player's
// PerfectShiftRangeMultipliers by difficulty), the clutch speed's (+0xE8, an
// AI racer's ShiftSpeedMultipliers), and the ranges its shift modifiers are
// drawn from (lo/hi[0] up, [1] down, [2] third's up; NULL for the ctor's
// -100..0). carsim_init sets class 3, both scales 1 and the ctor's ranges.
void carsim_set_drag(CarSim* s, bool on);
void carsim_set_shift_tuning(CarSim* s, int carClass, float rangeMul, float clutchMul,
	const float lo[3], const float hi[3]);
// The drift (#268 E). carsim_set_drift is TheRaceParameters+0x44 for this
// car, with the race's AI skill (+0x78, 0..2). In a drift or a drag the
// steering takes the drift's input curve, counter-steer table and yaw-led lock
// (DoUndergroundSteering, CalculateMaxSteering) and TuneWheelParams leaves
// NFSAdjust and the rear yaw boost off the tyres; in a drift that is not a
// drag, PhysicsMover::ResolveForces runs DriftStabilizer (0x801C22B0) too.
void carsim_set_drift(CarSim* s, bool on, int skill);
// The driver's hydraulics for the next steps (TASKS.md #313): RealDriver's
// bounce[4] and extend[4], FL FR RR RL. NULL for either is all zero.
void carsim_set_hydraulics(CarSim* s, const float bounce[CARPHYS_WHEELS],
	const float extend[CARPHYS_WHEELS]);
// Car::AreHydraulicsBouncing (0x80224B3C) and AreHydraulicsExtended
// (0x80224B7C).
bool carsim_hyd_bouncing(const CarSim* s);
bool carsim_hyd_extended(const CarSim* s);
// Engine::SetAccumulatingHeat: TuneBoostAndDampers calls it every step at
// race stage 4 with the drag flag, so a drag's heat builds past GO.
void carsim_set_heat(CarSim* s, bool accumulating);
// RealDriver::SetStartLineState (0x80044B08): on, the player's car waits on
// the line in neutral with the brake on; off is SetLowGear (0x80025748), first
// on a manual box, and in a drag the 0.25 s launch timer.
void carsim_set_start_line(CarSim* s, bool on);
// PORT: a car stepped on the start line, put back on its spot (`xy`, the
// grid's) after the step with no way on it. Retail's brake holds it there;
// here a car stepped before GO creeps off the line however hard it is held,
// as the sim settles it onto its springs (opponent.c, main.c's `held`).
void carsim_hold_xy(CarSim* s, const float xy[2]);
// PORT (TASKS #321c): a LAN racer's car on another console, pulled toward
// the state its owner last sent -- the body's centre `pos` (rbPos), its
// forward and up rows, its velocity and spin, world. `k` 0..1 of the way
// for the pose, `kv` for the velocities; 1 and 1 put it there outright.
void carsim_net_pull(CarSim* s, const float pos[3], const float fwd[3],
	const float up[3], const float vel[3], const float w[3], float k, float kv);
// PhysicsMover::SetDynoMode (0x801C2B78) for DynoManager::StartDynoRun
// (0x80191BFC): on the rollers, the car's position is held where it stands
// after every step while its momentum carries on along its own heading -- the
// speed the wheels and the box see is the rollers', and the car goes nowhere.
// On also clears the motion and puts the box in neutral, as StartDynoRun's
// resets do; off stops it where it is. PORT: retail springs the body to the
// rollers' anchor and spins the undriven wheels from the virtual speed
// (ResolveDynoForces); here the tyres keep their contact with the ground under
// a body whose movement is put back each step, so every wheel turns at the
// virtual speed through its own contact.
void carsim_set_dyno(CarSim* s, bool on);
void carsim_set_dyno_drag(CarSim* s, float mps2);
// Car::GetEngineHeat (0..1, for the EngineTempGauge) and Car::IsEngineBlown.
float carsim_engine_heat(const CarSim* s);
bool  carsim_engine_blown(const CarSim* s);
// Car::GetTurboBoost (Speed.elf 0x802243F0): the engine's boost, and in
// `*hasTurbo` (may be NULL) whether the car has a turbo at all.
float carsim_turbo_boost(const CarSim* s, bool* hasTurbo);
// Car+0x964, the drag's totaled car (Race::Update's finish reason 9).
bool  carsim_totaled(const CarSim* s);
// DriveTrain::GetPotentialShiftBonus for an upshift out of the current gear
// at `rpm` (the DragTachometer's shift light, not the scored kind), and the
// bonus the last shift earned (> 0 a good one, 1 perfect, -1 over-revved).
float carsim_shift_potential(const CarSim* s, float rpm);
float carsim_shift_bonus(const CarSim* s);
// DriveTrain::GetOptimalShiftUpRPM for the gear the car is in (the up table).
float carsim_optimal_shift_rpm(const CarSim* s);
// The same two for gear `g`, the DriveTrain's own numbering (dtGear: 0 R,
// 1 N, 2 first), and GetPerfectShiftRange (0x801B5410), PerfectShiftMaxRPMDiff
// [class][g] x the range's scale. The DragTachometer reads all three for the
// gear it showed the frame before.
float carsim_shift_potential_gear(const CarSim* s, float rpm, int g);
float carsim_optimal_shift_rpm_gear(const CarSim* s, int g);
// The RPM band of the gear the box is in, as AICarController::
// IsCloseToShiftingGears (0x80008E28) reads it: on the PhysicsMover the
// DriveTrain's GetShiftDownRPM / GetShiftUpRPM (gear, false), modifiers in;
// on the MellowMover its own tables (+0x1C0 down, +0x19C up).
void  carsim_shift_band(const CarSim* s, float* lo, float* hi);
float carsim_perfect_shift_range(const CarSim* s, int g);
// Re-reads mass and inertia out of s->cpi after a caller changed it there
// (CompleatCarPhysicsInfo::TuneForDragRace's tail, BuildInertiaTensor), and
// remakes the shift points, which hang off the engine and gearing.
void carsim_retune(CarSim* s);

// Car::SetMovementMode (0x8021ED7C) between the two movers an AI racer runs
// on: the PhysicsMover above (`on` false, movement mode 1) and the MellowMover
// (`on` true, mode 3), which RaceCarController::Think gives a racer once its
// physics timer runs out. Retail builds a new mover from the car's state at
// every switch (InitializeStateFromCar), and so does this: pose, velocity,
// wheel spin, gear and revs carry over. `rating` is the car's
// GetPerformanceMeasure on the 0..10 axis, which sets the MellowMover's tyre
// friction (SetFrictionRating); `mu` is carsim_mellow_mu's cap. Both are
// ignored when `on` is false. Does nothing when the mover is already the one
// asked for.
void carsim_set_mellow(CarSim* s, bool on, float rating, float mu);

// Mellovator::ResolveForces' friction cap: 4.0, 2.0 in a Street X race
// (TheRaceParameters +0x58, SetupShortTrack), and 3.2 on a track whose
// TrackInfo +0x94 race-type mask has bit 0x2000, the URL courses.
static inline float carsim_mellow_mu(bool streetX, u32 trackRaceTypeMask) {
	if (trackRaceTypeMask & 0x2000u) return 3.2f;
	return streetX ? 2.0f : 4.0f;
}

// Car against car (TASKS.md #223). Resolves one pair that has been stepped
// this frame (`dt` is that step; each car's own move is what is backed up):
// with the two hulls a millimetre apart or less (GJK), both are backed up to
// where they first touched within their steps (FindContactTime), or
// crowbarred apart when they never came apart, given retail's two-body
// collision impulse at the closest points, moved on again over the time they
// were backed up, and republished; twice at most. Returns true when they
// were touching. Call it after both cars' carsim_update (or
// carsim_traffic_pose) for the frame, once per pair. See carsim.c for what of
// this is retail's and what is the port's.
bool carsim_car_contact(CarSim* a, CarSim* b, float dt);

// The GJK distance between the two cars' hulls as they stand, 0 when they
// meet (what carsim_car_contact tests, for checks and tools).
float carsim_car_distance(const CarSim* a, const CarSim* b);

// A Smokeable a car pushes (smokeable.c, TASKS.md #318): a crash barrel, sign
// or bin as Contact::CollisionImpulse sees it. smokeable.c fills it from its
// body, carsim_prop_contact() resolves the pair and writes back the momenta,
// the velocities, the pose (backed up and forward again with the car) and the
// anchor state.
typedef struct {
	float pos[3];        // the centre of gravity, world
	float rot[3][3];     // rows: the body's own x, y and z axes, world
	float lo[3], hi[3];  // its box, from the centre of gravity along rot
	float cg[3];         // the centre of gravity in the model's own space
	float P[3], L[3];    // momentum, angular momentum
	float vel[3], w[3];  // what they come to
	float invMass;       // the RigidBody's 1/mass
	float invI[3][3];    // and inverse inertia, body space
	float anchor;        // Smokeable +0x144; GetMassInverse is 0 until it is 0
	float bounce, friction;
	bool  awake;         // Smokeable +0x4EE: ApplyImpulse acts only when set
	bool  woke;          // set when this contact woke it (Awaken)
	bool  anchorGave;    // and when it broke the anchor (ActivateAnchorEmitters)
	// What the collision sounds read and write (colsfx.c, stSmokableCollision):
	u32   serial;        // which prop this is, never 0 (smokeable.c reuses the slot)
	u8    category;      // SmokeableInfo +0xD8, MatchSmokeableCategory's
	bool  emitted;       // Smokeable +0x4ED: Awaken has started its emitters
	bool  splashed;      // +0x4F4: the barrel's splash has played
} CarSimProp;

// Car against one prop, after the car's carsim_update for the frame: their
// boxes seen from above and in height, backed up to where they first touched
// within `dt` as carsim_car_contact() does, then the impulse with the prop's
// ConsiderImpulse (0x801D47C8). True when they touched.
bool carsim_prop_contact(CarSim* car, CarSimProp* p, float dt);
// The car's body box from above, for the props' broad test: its plan centre,
// the half-length and half-width the box reaches round it, and its height
// range (world).
void carsim_body_extent(const CarSim* s, float c[2], float* reach, float* zLo,
	float* zHi);
// And that box exactly: its plan centre, its unit forward and left axes
// flattened, and its half-length and half-width (the car-car pair's).
void carsim_plan_box(const CarSim* s, float c[2], float ax[2][2], float half[2]);
// The polytope GetGJKPoly (0x801AFF10) hands back with no other body: the
// hull's vertices less the CG, the box's eight corners without a hull. Body
// frame, metres; the count, then point k.
int carsim_body_point_count(const CarSim* s);
void carsim_body_point(const CarSim* s, int k, float out[3]);

// Collision::AddImpulse (0x801AB990), which Contact::CollisionImpulse
// (0x801B24F4) hands every contact impulse it works out: the collision sounds
// listen there (colsfx.h). `other` is the other car, NULL for the world or a
// prop; `prop` the prop (Collision::GetSmokeable), NULL for the world, where
// `ground` tells the body's ground contact from a wall. `car` is NULL for a
// prop against the world. `pos` is the contact point (world), `impulse` what
// retail passes, the negative of the impulse `car` (or the prop) takes before
// CarCollisionBody::ApplyImpulse scales it for a light car, and `slip` the
// contact's sliding speed, m/s. Up to four listen at once (the collision
// sounds and the style points); adding one twice is a no-op.
typedef void (*CarSimImpulseHook)(const CarSim* car, const CarSim* other, CarSimProp* prop,
	bool ground, const float pos[3], const float impulse[3], float slip);
void carsim_add_impulse_hook(CarSimImpulseHook fn);
void carsim_remove_impulse_hook(CarSimImpulseHook fn);
// The same for a prop against the world (smokeable.c's ground and walls):
// `impulse` the one the prop takes.
void carsim_prop_world_impulse(CarSimProp* p, bool ground, const float pos[3],
	const float impulse[3], float slip);

// Whether a wheel the engine drives is on the ground (the per-wheel test at
// the top of PsychoDriver::Drive, 0x8001FCE8: CarState +0x388 and
// IsDriveWheel), as of the last carsim_update.
bool carsim_drive_wheel_down(const CarSim* s);

// Whether a front wheel is on the ground (CarState +0x388 [0] or [1]), the
// test PsychoDriver::Drive (0x8001FCE8) steers by, as of the last update.
bool carsim_front_wheel_down(const CarSim* s);

// The bottom-out half of Car::TriggerCarTerrainEffects (0x80222300): with
// three or more wheels down, this step's summed penalty impulse (bottomPen)
// placed in the range of `terrain`, the eTerrainType under the car's middle
// (TerrainTypeInfoTable +0x78 / +0x7C), 0..1. Above 0, retail sends the car's
// sound SFXMessage 5 at (int)(x * 32767) and fires the sparks (CarEffect 5).
float carsim_bottom_out(const CarSim* s, int terrain);

// Whether the exhaust backfires this step: Car::ChangeGear (0x80220B64),
// which Car::Move runs every frame -- a change into first, neutral or
// reverse, or the clutch biting again after a shift into second or above --
// and the neutral-rev half of TriggerCarTerrainEffects (0x80222300) -- in
// neutral, revs that fall under half the idle-to-limiter span arm it and
// climbing past three quarters fires it. `level` is the turbo upgrade level
// (GetUpgradeLevel(4), + 0.5 with the junkman turbo); under 2 nothing
// fires. Retail then shows the flame (CarEffect 10, at level / 3.5) and sends
// the car's sound SFXMessage 9 (CARSFX_SparkChatter). Once a frame, after
// carsim_update.
bool carsim_backfire(CarSim* s, float level);

// CarState::IsDriveWheel (0x801AB500): whether the engine drives wheel `i`.
bool carsim_is_drive_wheel(const CarSim* s, int i);

// How far apart the two cars' plan rectangles are, m; 0 when they overlap.
// What the port has for the GJK distance a CollisionBodyWitness keeps
// between two bodies (+0x08), seen from above.
float carsim_plan_gap(const CarSim* a, const CarSim* b);

// Retail's AverageWindow (0x801A58A8, Record 0x801A5A80): an Average whose
// samples drop out once older than `length` seconds of `t`. The steering
// windows here and the AI's adaptive steering controller (aidriver.h) use it.
void carsim_window_init(CarSimWindow* a, int cap, float length);
void carsim_window_record(CarSimWindow* a, float v, float t);

// How long side `side` (CARSIM_TOUCH_*) has been in contact, in seconds of
// the car's clock: the age of the Collision retail would have on that side.
// -1 when nothing touches it.
static inline float carsim_touch_age(const CarSim* s, int side) {
	if (s->touchLast[side] < 0.0f || s->clock - s->touchLast[side] > CARSIM_TOUCH_GAP)
		return -1.0f;
	return s->clock - s->touchStart[side];
}

// Whether the two cars' plan boxes overlap, without pushing them apart: what
// a car that is blinking after a reset (and so passes through the others)
// asks before it becomes solid again.
bool carsim_cars_overlap(const CarSim* a, const CarSim* b);

// Sets a car that has just been placed rolling forward at `speed` m/s, the
// way Car::ResetCore (0x80220F60) sends a reset car on at its reset speed:
// the body's velocity along its nose and every wheel turning to match.
void carsim_set_rolling(CarSim* s, float speed);
// A debug launch: rolling at `speed` with `up` m/s upward (DbgJumpKick, #504).
void carsim_dbg_launch(CarSim* s, float speed, float up);

// Speed in km/h, which is what the tachometer plate is marked in.
float carsim_speed_kmh(const CarSim* s);

// What TrafficMover::Move (0x8002A080) writes to its car once it has moved it
// (traffic.c owns the mover itself): the car put at (`x`,`y`) facing
// `heading` by Car::PutFlatOnTrack (0x80220A90) -- the four wheels dropped on
// the ground and the body laid on the plane they make -- the velocity along
// the step's displacement at `speed`, the yaw rate off the heading's change,
// and each wheel on the ground turning at the speed over its radius. No
// physics runs: the body goes where it is put. `roadZ` is the height of the
// road under (`x`,`y`) off the route: the probe looks down from 1.5 m over
// it, and a car with a wheel off the loaded world stands that high (PORT).
void carsim_traffic_pose(CarSim* s, const CarSimWorld* w, float x, float y, float roadZ,
	float heading, float speed, float dt);

// What CarState::CalculateWheelMatrices (0x801AA974) draws each wheel by, as
// the car's mover last left it: the spin angle (rad, positive rolls the top
// forward), the suspension's lift (m, min(x, SuspensionInfo [7]), positive
// up) and the front wheels' steering as drawn (rad, positive to the left).
// A traffic car takes only the spin: TrafficMover::Move (0x8002A080) leaves
// the suspension at 0 and steers the wheels straight off its driver's +0x1C4.
// The replay (#498): pos, yaw / pitch / roll, origin, vel, speed, gear, rpm,
// shifting and onGround again from rbPos, rot, rbVel, dtGear, rpmSmooth,
// dtShifting and wheelsDown, as the end of carsim_update leaves them.
void carsim_publish(CarSim* s);
void carsim_wheel_state(const CarSim* s, float spin[CARPHYS_WHEELS],
	float susp[CARPHYS_WHEELS], float* steer);

// CarState::GetYaw (0x801AA320): the angle from the direction of travel to
// the heading, rad, +-pi; 0 below 1 m/s forward.
float carsim_yaw(const CarSim* s);
// CarState::GetYawAlways (0x801AA360): the same with no floor.
float carsim_yaw_always(const CarSim* s);

// Nitrous, CarState's (Speed.elf 0x801AA248..0x801AB1C4). The bottle is a
// length of time, CarPhysicsInfo +0x2A0 seconds of it: 0 on a stock car,
// filled in by the nitrous upgrade (BuildNitrousPackage). carsim_place()
// fills it again, as Car::ResetCore does through CarState::ResetCar.
//   carsim_nos_start      StartNitrous: opens the bottle if anything is in
//                         it; false if not
//   carsim_nos_stop       StopNitrous: shuts it, what burnt taken off
//   carsim_nos_on         IsNitrousOn: the GameBreaker level while it burns,
//                         0 shut -- and an empty bottle shuts itself here
//   carsim_nos_available  IsNitrousAvailable: shut, and the level it would
//                         burn at
//   carsim_nos_fraction   GetNosPercentageLeft: what is left over the
//                         bottle's size, past 1 by an overfill
//   carsim_nos_fill       FillNitrous: amount bottles in (or out, below 0),
//                         topping up to full and then overfilling to 2;
//                         style points call it (style.c)
// While it burns the engine adds CarPhysicsInfo +0x2AC times NOSCurve to its
// torque (Engine::GetTotalEngineTorque).
bool  carsim_nos_start(CarSim* s);
void  carsim_nos_stop(CarSim* s);
int   carsim_nos_on(CarSim* s);
int   carsim_nos_available(const CarSim* s);
float carsim_nos_fraction(const CarSim* s);
void  carsim_nos_fill(CarSim* s, float amount);
// GetRealNitrousRemaining (0x801AAD44): the bottle's seconds left, less what
// is burning now. The AI's nitrous reads it (opponent.c).
float carsim_nos_remaining(const CarSim* s);
// ResetNitrous (0x801AA248) on demand: the bottle refilled to CarPhysicsInfo
// +0x2A0 and shut. Retail builds a new CarState for each race, whose
// ResetNitrous reads the race's tune (a drift's +0x2A0 is 0); the port keeps
// its car and re-tunes it, so a tune that changes the bottle calls this.
void  carsim_nos_reset(CarSim* s);

// The most sideways force the car's tyres take, per unit of load: the lesser
// of the front and rear TireInfo +0x0C, less NFSAdjust in control mode 0
// (TuneWheelParams). Downforce adds load at speed; this leaves it out.
float carsim_tyre_grip(const CarSim* s);

// CalculateMaxDeceleration: the hardest the car can slow at `speed`, m/s^2,
// negative.
float carsim_max_decel(const CarSim* s, float speed);
// Car::AverageAir's downforce term, m/s^2 (main.c's jump camera, #504).
float carsim_air_downforce(const CarSim* s);

// RidePhysicsInfo's acceleration table (MakeAccelerationTable): what the
// engine alone gives through the gears, m/s^2, CARSIM_ACCEL_N samples over
// [0, top]. Returns `top`, the speed the gearing runs out at, m/s.
#define CARSIM_ACCEL_N 64
float carsim_accel_table(const CarSim* s, float out[CARSIM_ACCEL_N]);

// 0..1 sweep of the rev counter, idle at 0 and the limiter at 1.
float carsim_rev_fraction(const CarSim* s);

// The car's own steering scale, CarPhysicsInfo's control block +0x270 -- what
// PlayerSteering::DoUndergroundSteering multiplies its lock and rate by. An AI
// driver steering by angle has to apply it itself, and only in the drift and
// drag styles: PsychoDriver::Drive skips it for a normal racer (0x80020aa4).
static inline float carsim_steer_scale(const CarSim* s) {
	float v = s->cpi[0x270 / 4];
	return v > 0.0f ? v : 1.0f;
}
