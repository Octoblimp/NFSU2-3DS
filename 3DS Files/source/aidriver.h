#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "carsim.h"
#include "race.h"
#include "trackroute.h"

// The AI driver (TASKS.md #221): retail's PsychoDriver, the thing that turns a
// point to steer at and a speed to hold into a frame of car input.
//
// This is a port of Speed.elf's own control loop, not a steering routine this
// project invented. docs/retail_ai_driver.md is the write-up it follows; every
// table, graph and weight below is the engine's own, read out of the GameCube
// image by tools/re/physics/tabinit.py and tools/re/graphinit.py. The retail
// functions, with their addresses:
//
//   PsychoDriver  Drive                  0x8001FBEC
//                 CalcSteerToPoint       0x80020F0C
//                 GetSteeringError       0x8001F7DC
//                 CalcThrottleAndBrake   0x80021114
//                 GetMovementIndex       0x8001FA8C
//                 RecordWheelSlip        0x8001FAF0
//   Average       Record / Reset         0x801A571C / 0x801A57C0
//   PidError      Record                 0x801A5C18
//   FuturePoints  ScrubSpeeds            0x80036CD8
//   (file scope)  GetSpeedLimitForCurvature 0x801FA2E4
//                 Curvature              0x801FA46C
//                 GetCarBraking          0x801F9650
//                 CalculateCarClass      0x801C9780
//
// What is ported and what is not
// ------------------------------
// Faithful: the whole of PsychoDriver -- the steer-to-point look-ahead, the
// three steering errors and their squared-error blend, the PID memory, the
// self-tuning steering controller a circuit or sprint racer steers through
// (AdaptivePIDControllerComplicated, ported but off: see below), the
// fixed-weight steering of the drift and drag styles, the airborne
// steering, the fixed-weight throttle gains and the self-tuning throttle above
// 30 m/s (AdaptivePIDControllerSimple, ported but off: see below),
// the throttle rate limit, traction control, the steering range and rate
// tables, and the speed profile's curvature limit and its braking and
// acceleration scrub (ScrubSpeeds, over each car's own MakeAccelerationTable).
//
// PORT, and marked PORT where it happens:
//
//   * The drive target. Retail reaches PsychoDriver through FuturePoints ->
//     DrivingChoice::BuildDriveTarget -> DriveTarget, a graph walk over the
//     TrackRoute topology with spline interpolation, corner smoothing,
//     NearbyAvoidableList and action scoring -- several hundred functions over
//     a world representation this port does not have. Instead the target is
//     the race's own route spline (race_route_sample), which is the same pair
//     of inputs -- a point ahead and a speed -- that retail's pipeline ends up
//     producing, from the same retail route data. The racer's DrivingChoice
//     -- AIActionJustDrive's TryAndDriveBestLine, AIActionAvoidAvoidable and
//     ChooseOffsetFromDrivingLine -- is ported in opponent.c over that line,
//     with a port-side stand-in for CollisionPredictor (TASKS.md #280).
//
//   * The steering's self-tuning controller is ported but left off
//     (aidriver.c's kSteerAdaptive): it is tuned for retail's drive target,
//     and the port's needs more proportional gain than it allows. Every racer
//     steers on retail's own fixed weights, the drift and drag styles'
//     branch.
//
//   * So is the throttle's (aidriver.c's kThrottleAdaptive): over the port's
//     speed profile its rate term brakes the car under the limit into every
//     corner, and the field bunches and touches. Every racer is on the fixed
//     throttle weights at any speed.

// --------------------------------------------------------------------------
// Retail's Average (0x28 bytes): a ring buffer of at most five samples that
// keeps its own running sum. Drive only ever asks for five, so the heap case
// in Average::Init (0x801A54F8) is not needed.
#define AI_AVG_MAX 5

typedef struct {
	float d[AI_AVG_MAX];
	u8    cap, count, idx;
	float sum, avg;
} AiAverage;

// Retail's PidError: three Averages, then K, the last error and the one
// before it. The first Average holds the frame times, not an error -- that is
// what makes the integral a true time integral over the window.
typedef struct {
	AiAverage dt;     // +0x00
	AiAverage integ;  // +0x28, dt * trapezoid of the error
	AiAverage deriv;  // +0x50, d(error)/dt
	float k;          // +0x78
	float last;       // +0x7C
	float prev;       // +0x80
} AiPidError;

// Retail's AdaptivePIDControllerBase + Complicated (0x8003014C / 0x80030B74),
// the controller at PsychoDriver +0x264 a circuit or sprint racer steers
// through: an output of sum(coef[i] * term[i]) over the P, I and D terms,
// whose coefficients tune themselves (UpdateBase, 0x80030444) -- one term
// every `window` seconds, in turn, by how the error moves against how that
// coefficient has been moving. Offsets are the controller's own.
#define AI_APID_TERMS 3
typedef struct {
	float termMin[AI_APID_TERMS], termMax[AI_APID_TERMS];   // +0x00, a pair each
	float window;                                         // +0x1C, seconds
	float coefMin[AI_APID_TERMS], coefMax[AI_APID_TERMS];   // +0x20, a pair each
	float threshold[AI_APID_TERMS];   // +0x38: a term this small is not tuned
	float rate[AI_APID_TERMS];        // +0x44, the learning rate
	float coef[AI_APID_TERMS];        // +0x54
	CarSimWindow coefRate[AI_APID_TERMS];   // +0x60, each coefficient's rate
	float lastErr;                    // +0x6C
	CarSimWindow errRate;             // +0x70, the error's rate of change
	float term[AI_APID_TERMS];        // +0xA8, the caller's P, I and D
} AiAdaptivePid;

// --------------------------------------------------------------------------
// One AI driver. The caller sets the target (steerTo, lateral, desired) and
// calls aidriver_drive once a frame, the way RaceCarController::Think sets a
// PsychoDriver's target before Car::Update runs it.
typedef struct {
	// PsychoDriver +0x248 / +0x24C / +0x250 / +0x254 / +0x258 / +0x25C.
	AiPidError heading, lateral, blended, throttle, slipSigned, slipAbs;

	float steerTo[2];   // +0x204, the world XY point being steered at
	float lateral_m;    // DriveTarget::GetLateralOffset, metres right of line
	float desired;      // +0x210, the speed the goal wants, m/s; < 0 backs up
	float desiredPrev;  // +0x244

	u16   steerU16;     // CarDriver +0x1C4: the steering angle, 65536/turn
	float throttleMax;  // +0x20C
	float prevThrottle; // +0x1F0
	float prevBrake;    // +0x1F8

	// +0x224, which way the car is being driven: 1 under way the way
	// `desired` asks (forward for a positive speed, reverse for a negative
	// one), 0 still rolling the other way and braking first. Drive flips it
	// with 5 m/s of hysteresis; the constructor starts it at 1.
	int   dirState;

	int   carClass;     // 0 (quickest) .. 3, picks the grip and braking figures
	bool  racing;       // false holds the car still on the grid

	AiAdaptivePid steer;  // +0x264, the circuit and sprint steering
	// +0x318, AdaptivePIDControllerSimple: the throttle above 30 m/s, and its
	// own PidError of the speed error (+0x3C0).
	AiAdaptivePid throttleAdaptive;
	AiPidError    throttleSpeed;
	float clock;          // WorldTimer, seconds: what `steer` tunes by
	float airSince;       // +0x23C: when the front wheels left the ground, < 0 on it
	// Set by the caller after aidriver_init. Drive's steering limits differ
	// for an SUV (Car +0xC's FECarRecord +0x88A) on the PhysicsMover, and on
	// a Street X track (g_tweakIsShortTrackRace) on any other mover.
	bool  suv;
	bool  shortTrack;
	// A traffic car's movement and control modes, as GetMovementIndex
	// (0x8001FA8C) reads them off Car +0x28 / +0x24 (TASKS.md #316):
	// `trafficMover` while it is on the TrafficMover (movement mode 2, the
	// index 3 column), and `traffic` for its control mode 4, which puts it in
	// the index 4 column on the MellowMover where a racer is in 0. Both false
	// for a racer; set after aidriver_init.
	bool  trafficMover;
	bool  traffic;
} AiDriver;

// CalculateCarClass (0x801C9780) with PerformanceClassRange {0, 273, 401, 500}:
// the 0..10 measure is mapped back onto the fleet's own points range and
// classed against those thresholds. `lo`/`hi` are cardb's Extrema pair.
int aidriver_car_class(float measure, float lo, float hi);
// The same with its bool false: a raw measure, classed as it is -- what
// RebuildPhysicsInfo stores in RidePhysicsInfo +0x04 and GetCarClass returns.
int aidriver_car_class_raw(float measure);

// fSmoothCurvatureCarFriction / fSmoothCurvatureCarBraking for a class -- the
// grip (in g) the speed profile corners on and the deceleration (m/s^2) its
// braking pass uses.
float aidriver_class_grip(int carClass);
float aidriver_class_braking(int carClass);

void aidriver_init(AiDriver* d, int carClass);
// ZeroSteeringMemory + ZeroThrottleMemory (0x8001F8E8 / 0x8001FA28).
void aidriver_zero_memory(AiDriver* d);
// ClearSteeringMemory (0x8001F9CC): the heading, lateral and blended errors
// restart from what they are right now, at `steerTo`, so a change of target
// does not arrive as a jump in the derivative. GetUnstuck's CleanUp.
void aidriver_clear_steering(AiDriver* d, const CarSim* s);

// CalcSteerToPoint's look-ahead, in metres: how far down the line the driver
// aims. `speed` is the car's body-axis speed, m/s.
float aidriver_lookahead(const AiDriver* d, const CarSim* s, float speed);

// PsychoDriver::Drive. Fills `out` with this frame's steering, throttle and
// brake. `steerU16` is written here, which is what retail's Drive does to
// CarDriver +0x1C4, so the caller hands `out->steerAngleU16` straight to
// carsim with `out->steerIsAngle` set.
void aidriver_drive(AiDriver* d, const CarSim* s, float dt, CarSimInput* out);

// --------------------------------------------------------------------------
// DriftDriver (#268 E step 5): the driver RaceCarController's constructor
// (0x80027908) gives an AI racer (car type 3) in a drift (TheRaceParameters
// +0x44) in place of a PsychoDriver. It does not race: it holds the car in a
// slide down the line, its speed capped at 70 mph, and scores on its own
// DriftManager (drift.c). Drive is 0x80021E78, SetSteeringClamp 0x80021D60;
// the fields are at retail's offsets.
//
//   * Each frame the drive target gives two points down the line,
//     aidrift_ahead's distances from the car (DriveTarget::InterpolatePoint):
//     the first is steered at; with none (`p1` NULL) the car runs 10 m down
//     its own heading (SteerStraightVector3D).
//   * Under 10 mph, or with the point more than 120 degrees off the nose (90
//     in reverse) or the speed asked below 0, it drives straight at the point
//     on the speed error, backing round when the point is behind.
//   * Otherwise it picks a slip angle -- the point's bearing off the
//     direction of travel, more of it the faster, at most 45 to 70 degrees
//     -- and steers the slip (GetYawAlways) onto it, damped by its rate; the
//     e-brake comes on as the slip falls short of it at speed, and is the
//     handbrake on the PhysicsMover once past 0.8; the throttle holds the
//     speed asked, eased off the more the slip runs past 20 degrees.
//   * On the start line (SetStartLineState, `mode` not 0) both pedals are
//     down in neutral and the wheel straight.
typedef struct {
	u16   steerU16;     // CarDriver +0x1C4, 65536 to the turn
	int   gear;         // +0x1C8, GetDesiredGear: -1 the box's, 0 reverse, 1 neutral
	float throttle;     // +0x1F0
	float ebrake;       // +0x1F4
	float brake;        // +0x1F8
	float steerTo[2];   // +0x204
	float desired;      // +0x20C, m/s
	float lastYaw;      // +0x210, last frame's GetYawAlways
	float targetYaw;    // +0x218, the slip angle aimed for, rad
	int   mode;         // +0x21C, the start-line state; 0 once racing
} AiDriftDriver;

void aidrift_init(AiDriftDriver* d);
// SetDesiredSpeed (vtable +0x7C): at most 31.2919 m/s.
void aidrift_set_desired(AiDriftDriver* d, float speed);
// Drive's two InterpolatePoint distances, metres up the line from the car:
// 15 to 30 m by the speed and how far (`lateral`, m) the car is off the line,
// then 8 to 12 m.
void aidrift_ahead(const CarSim* s, float lateral, float* d1, float* d2);
// One frame: `p1` / `p2` are the line's points at aidrift_ahead's distances
// (world XY; NULL for no drive target), `physicsMover` the car is on the
// PhysicsMover (Car +0x28 == 1), not the MellowMover. Fills `out` as
// aidriver_drive does (steerIsAngle, aiDriver, the gear).
void aidrift_drive(AiDriftDriver* d, const CarSim* s, float dt, const float* p1,
	const float* p2, bool physicsMover, CarSimInput* out);

// --------------------------------------------------------------------------
// The speed profile: what a corner is worth, every few metres along a route.
//
// Retail bakes this into the Routes files with CurvaturePoint::
// CalculateSpeedLimits (0x801FB694) -- GetSpeedLimitForCurvature over the
// line's curvature, then ScrubSpeedLimitsBraking / Accelerating. The shipped
// Bayview files carry 100 m/s on both racing lines, i.e. no limit, so the
// cornering speed has to be computed the same way retail's tool computed it.
// That is what this does.
//
// It is measured along the racing line, which is what retail measures: a
// CurvaturePoint is a point of the TrackRoute chain, taken at that point's own
// driving-line offset, so the corner the limit describes is the corner the car
// will actually take. Over the race route's spline instead -- which is what
// this did before #272 -- the limits come off a line no car drives: on route
// 4164 the spline cuts the walled median between two carriageways, so the AI
// braked for a kink that is not on their road and ran wide where the road
// really bends. Without a line (`line` NULL) it falls back to the spline.
//
// PORT: a sample is still indexed by how far along the *race route* it is, as
// aispeed_at() is asked, rather than by arc length up the line itself -- the
// two differ by the few per cent a lateral offset adds through a corner, and
// every other part of the race (progress, standings, the pager) is measured
// along the race route.
typedef struct AiSpeedProfile AiSpeedProfile;

// Builds the profile for `r`, along `line` where there is one. `carClass`
// picks fSmoothCurvatureCarFriction and fSmoothCurvatureCarBraking;
// `topSpeed` caps it (TrackRouteManager::GetTopSpeed's role); `accel` is the
// car's acceleration table, CARSIM_ACCEL_N samples over [0, `accelTop`] m/s
// (carsim_accel_table), which the forward pass reads as retail's ScrubSpeeds
// reads RidePhysicsInfo's (NULL for a fixed fallback). `grip` is the
// slowest-cornering car's carsim_tyre_grip(), and caps the class's friction
// where it is lower (0 for no cap). NULL when the route is NULL or has no
// length. `log` may be NULL.
//
// PORT (TASKS.md #280, #398): retail's class friction is 2.8 to 3.7 g, and
// carsim's PhysicsMover tyres hold 1.4 to 1.7. The cap was the tyres' while
// the AI drove on the PhysicsMover; since it races on the MellowMover (cap
// 4 g) the caller passes that cap, so a normal race plans on the class's
// figure as retail does. Drag and street drift fields stay on the
// PhysicsMover and pass carsim_tyre_grip().
AiSpeedProfile* aispeed_build(const RaceRoute* r, const AiLine* line, int carClass,
	float topSpeed, const float* accel, float accelTop, float grip,
	void (*log)(const char* msg));
void  aispeed_free(AiSpeedProfile* p);

// The speed limit `dist` metres along the route, m/s, interpolated between
// samples. Wraps on a looped route.
float aispeed_at(const AiSpeedProfile* p, float dist);

// What a corner of curvature `curv` (1/m) is worth on the friction the
// profile was planned on, and the braking it was scrubbed with (m/s^2): for a
// path the profile does not cover, a fork's branch (TASKS.md #409).
float aispeed_corner(const AiSpeedProfile* p, float curv);
float aispeed_braking(const AiSpeedProfile* p);

// The slowest sample in the profile, for the load log.
float aispeed_min(const AiSpeedProfile* p);
int   aispeed_count(const AiSpeedProfile* p);
