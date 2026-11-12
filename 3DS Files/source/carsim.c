#include "carsim.h"

#include <math.h>
#include <string.h>

// Retail's car model (TASKS.md #222), ported function by function from the
// GameCube Speed.elf. docs/retail_car_physics.md is the write-up; every
// function here names the retail one it comes from, and every number that is
// not per-car data is retail's own constant, named after its symbol where the
// executable has one.
//
// Frame: the rigid body's position is the centre of gravity; `rot` holds the
// car's forward, left and up axes in world space as rows, which is bWare's
// layout. The record frame is x forward, y LEFT, z up. Units are retail's:
// tonnes, kN, kN m, metres, seconds, radians.
//
// What is PORT (no retail counterpart, or a part of retail this port has not
// taken):
//   * The world query. Retail's CheckTerrain asks the track for the surface
//     under each wheel every substep; here the world is probed once a frame
//     (eight points, one batched call) and each answer is kept as a plane the
//     substeps measure against. See probeGround().
//   * Where the body hits the ground and the walls. Retail runs its collision
//     system against the car's collision body; here the four lowest corners of
//     the record's body box stand in for the body against the ground, and the
//     existing footprint resolver (CarSimWorld.collide) for the walls. Both
//     are resolved with retail's Contact::CollisionImpulse numbers.
//   * Orthonormalising the rotation (Gram-Schmidt), the shift-modifier random
//     source (a fixed LCG), and reading CollisionBody+0x11c in
//     TuneBoostAndDampers as "no wheel on the ground".
//   * Taken since: turbo pressure and engine heat, the drag and drift
//     physics modes and the catch-up boost (#255, #268 D/E). Not taken: the
//     first-gear clutch-play graph, the PhysicsMover's JumpStabilizer, road
//     noise, damage wobble, the burnout mode, the post-collision boost
//     graphs, the e-brake timer, IsBlinking and the MellowMover's hydraulics
//     and MaybeForceYaw. Terrain is ROAD everywhere (every factor 1).

// ---------------------------------------------------------------------------
// CarPhysicsInfo offsets (docs/retail_car_physics.md, "Data")

#define CPI_CG              0x000
#define CPI_WHEEL           0x010   // WheelInfo[4], FL FR RR RL
#define CPI_WHEEL_STRIDE    0x30
#define WI_RADIUS           0x10
#define WI_WIDTH            0x14
#define CPI_TIRE            0x0D0   // TireInfo[2], front and rear
#define CPI_TIRE_STRIDE     0x20
#define CPI_MASS            0x110   // RigidBodyInfo
#define CPI_DIMS            0x114   // body length, width, height
#define CPI_IXX             0x120
#define CPI_IYY             0x134
#define CPI_IZZ             0x148
#define CPI_IZZ_SCALE       0x160
#define CPI_SUSP            0x170   // SuspensionInfo[2], front and rear
#define CPI_SUSP_STRIDE     0x20
#define CPI_DTI             0x1B0   // DriveTrainInfo
#define DTI_LAUNCH          0x00
#define DTI_FRONT_FINAL     0x08
#define DTI_REAR_FINAL      0x0C
#define DTI_SPLIT           0x10    // 1 = rear drive, 0 = front drive
#define DTI_CLUTCH_K        0x14
#define DTI_NUM_GEARS       0x18    // int
#define DTI_CLUTCH_SPEED    0x1C
#define DTI_RATIO           0x20    // ratio[gear], gear 0 = R, 1 = N, 2 = 1st
#define CPI_EI              0x1F0   // EngineInfo
#define EI_IDLE             0x00
#define EI_REDLINE          0x04
#define EI_LIMITER          0x08
#define EI_INERTIA          0x0C
#define EI_TORQUE           0x10    // n9 over [idle, limiter]
#define EI_BRAKING          0x58    // n3 over [idle, limiter]
#define CPI_AERO            0x260   // Cd, front, rear, downforce load factor
#define CPI_CTL             0x270   // steer scale, brake k, e-brake k, bias
#define CPI_YFB_P4          0x280
#define CPI_YFB_P5          0x284
#define CPI_YAW_CLAMP_ON    0x288   // control mode 0 (Stability Control On)
#define CPI_YAW_CLAMP_OFF   0x28C   // the player's mode 1
#define CPI_GOVERNOR        0x290   // mph
#define CPI_NOS_CAP         0x2A0   // seconds in the bottle
#define CPI_NOS_BOTTLES     0x2A4   // int
#define CPI_NOS_TORQUE      0x2AC   // kN m at NOSCurve's 1

#define GEAR_R      0
#define GEAR_N      1
#define GEAR_FIRST  2

// ---------------------------------------------------------------------------
// Retail constants

#define MAX_TIME_STEP           0.0044f   // MaximumTimeStep
#define GRAVITY                 9.8f      // Gravity
#define RPM_PER_RADS            9.5493f
#define RADS_PER_RPM            0.10472f
#define MPS_TO_MPH              2.237f

// RigidBody
#define RB_VELOCITY_CLAMP       200.0f    // VelocityClamp, for a body over 1.8 t
#define RB_ANG_VEL_CLAMP        250.0f
#define RB_FORCE_CLAMP          1200.0f   // ApplyForce
#define RB_TORQUE_CLAMP         100.0f
#define RB_IMPULSE_CLAMP        1000.0f   // ApplyImpulse
#define RB_IMPULSE_MOMENT_CLAMP 10.0f
#define RB_GOVERNOR_SLACK       1.9f      // mph over the governor
#define RB_GOVERNOR_BOOST       1.03f
#define RB_GOVERNOR_DRAG        (-0.5f)   // kN, car-frame x, at the governor

// Wheel
#define WHEEL_INERTIA           0.01f     // WheelMomentOfInertia
#define BRAKING_TORQUE          4.0f      // BrakingTorque
#define EBRAKING_TORQUE         10.0f     // EBrakingTorque
#define STATIC_TO_DYNAMIC_BRAKE 1.2f      // StaticToDynamicBrakeForceRatio
#define BRAKE_LOCK_W_FACTOR     0.1f      // BrakeLockAngularVelocityFactor
#define MAX_STATIC_SLIP         0.5f      // MaxStaticSlip
#define PILOT_FACTOR            0.85f     // PilotFactor
#define TIRE_ELLIPSE_RATIO      1.5f      // TireForceEllipseRatio
#define GROUND_PENALTY          0.225f    // ApplyGroundPenaltyImpulse

// Aerodynamics
#define OFF_THROTTLE_DRAG       2.0f      // OffThrottleDragFactor

// YawFrictionBoost
#define YAW_EBRAKE_THRESHOLD    0.5f      // YawEBrakeThreshold
#define YAW_ANGLE_THRESHOLD     20.0f     // YawAngleThreshold, degrees
#define HIGH_SPEED_SPEED        30.0f     // HighSpeedSpeed (LowSpeedSpeed 0)
#define MAX_YAW_BONUS           0.35f     // MaxYawBonus
#define NFS_ADJUST              0.85f     // NFSAdjust, control mode 0's grip

// MovementDousingAndRoofReset / MellowMover::Move, dampening mode 1
#define WILD_COLLISION_GRAVITY  11.0f     // WildCollisionGravity

// MellowMover and Mellovator (Speed.elf 0x801BB200..0x801BE100)
#define MELLOW_MAX_TIME_STEP    0.0142857f   // MaxiMellowTimeStep
#define MELLOW_WHEEL_INERTIA    0.008f    // GetDerivative's divisor
#define MELLOW_WHEEL_W_CLAMP    20.0f     // the spin the brake term sees
#define MELLOW_BRAKE_K          (-0.2f)
#define MELLOW_DRIVE_SHARE      0.5f      // per wheel of an axle
#define MELLOW_TYRE_K           1.25f     // slip speed to force, per load
#define MELLOW_STATIC_SLIP      0.05f     // below this the rating is not used
#define MELLOW_SLIDING_SLIP     6.0f      // m/s: the tyre counts as sliding
#define MELLOW_FC_CLAMP         50.0f     // the stored tyre force's length
#define MELLOW_LOCK_BRAKE       0.95f     // the brake that locks a wheel...
#define MELLOW_LOCK_W           30.0f     // ...turning slower than this
#define MELLOW_UNLOCK_BRAKE     0.2f
#define MELLOW_HOLD_BRAKE       0.4f      // the brake that holds a car...
#define MELLOW_HOLD_SPEED       0.1f      // ...moving slower than this
#define MELLOW_DAMPER_K         0.9f      // ComputeSuspensionForces
#define MELLOW_BOTTOM_OUT       0.2f      // BottomOut's share of the speed
#define MELLOW_STEER_RATE       120.0f    // deg/s, the wheels as drawn

// A hit, as retail resolves a car against a track polygon (TASKS.md #150):
// Contact::CollisionImpulse in Speed.elf (0x801b24f4) and what it calls. Not
// tuning -- every number here is read out of the executable, in the record's
// tonne units, which is why the impulse below works in tonnes too.
//
//   BOUNCE    GetBounciness__16CarCollisionBody: 0.1, halved when the other
//             body IsWallish. Contact::GetBounciness takes the car's value
//             for a car against a track polygon rather than the larger of the
//             two.
//   FRICTION  GetFriction__16CarCollisionBody: 0.3, squared against a wallish
//             polygon; picked the same way.
//   MIN_J     MinimumContactImpulseFactor. The normal impulse is never less
//             than this over the inverse mass, so a car leaning on a wall is
//             always moved off it at 0.3 m/s at least.
//   SCRAPE    MaxFrictionScrapeSpeed, m/s. Below this sliding speed the
//             friction impulse ramps down in proportion, so it never reverses
//             a car that was barely sliding.
//   INV_MASS  GetMassInverse__16CarCollisionBody clamps 1/m to this, per
//             tonne: in a collision no car is lighter than 1.85 t, and
//             CarCollisionBody::ApplyImpulse scales the impulse down to match.
//   SPIN      GetInertiaTensorInverse__16CarCollisionBody (0x801B0168) and
//             CarCollisionBody::ApplyImpulse (0x801B0350) scale the inverse
//             inertia by 0.4 in every dampening mode but 1, and mode 1 is only
//             set by Collision::CheckForWildCollision, for a car-car hit of 30
//             or more (dampenSpin below). Against a wall the car turns at 0.4
//             of what its inertia alone would give.
//   MAX_J/L   RigidBody::ApplyImpulseWithScaledInverseInertiaTensor caps the
//             impulse at 1000 t m/s and its moment at 10 t m^2/s, the moment
//             before the SPIN scale.
#define RETAIL_WALL_BOUNCE      0.05f
#define RETAIL_WALL_FRICTION    0.09f
#define RETAIL_GROUND_BOUNCE    0.10f
#define RETAIL_GROUND_FRICTION  0.30f
#define RETAIL_WALL_MIN_J       0.30f
#define RETAIL_WALL_SCRAPE      1.00f
#define RETAIL_WALL_INV_MASS    0.5405405f
#define RETAIL_DAMPED_SPIN      0.40f
#define RETAIL_WALL_MAX_J       1000.0f
#define RETAIL_WALL_MAX_L       10.0f

// PORT: how far above the centre of gravity the once-a-frame probe looks for
// ground. Enough for a wheel climbing a kerb or a ramp; far less than the
// clearance under any deck carried over a street, so a wheel never stands on
// the underside of one (see carsim_place for the overpass this was measured
// against).
#define PORT_PROBE_CEILING      1.0f

// ---------------------------------------------------------------------------
// Static tables (the initialiser at 0x801d56c0, and PlayerSteering's)

static const float kAngMomDamp[6] = { 0.05f, 0.05f, 0.25f, 0.7f, 2.0f, 5.0f };   // [0,10]
static const float kMotionDamp[8] = { 1.5f, 1.0f, 0.6f, 0.22f, 0.06f, 0.03f, 0.015f, 0.0f };   // [0,1]
// LoadSensitivityTable, Zero..TwelveDegree, each over load [0,10] kN
static const float kLoadSens[7][6] = {
	{ 0.0f, 0.0f,  0.0f, 0.0f, 0.0f,  0.0f },
	{ 0.0f, 1.2f,  2.3f, 3.0f, 3.0f,  2.8f },
	{ 0.0f, 1.7f,  3.2f, 4.3f, 5.1f,  5.2f },
	{ 0.0f, 1.8f,  3.5f, 4.9f, 5.8f,  6.1f },
	{ 0.0f, 1.83f, 3.6f, 5.0f, 5.96f, 6.4f },
	{ 0.0f, 1.86f, 3.7f, 5.1f, 6.13f, 6.7f },
	{ 0.0f, 1.9f,  3.8f, 5.2f, 6.3f,  7.1f }
};
static const float kEBrakeSettings[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
static const float kClutchSpeed[4] = { -3.0f, 0.0f, 1.5f, 0.0f };   // per clutch state

// PlayerSteering (DoUndergroundSteering)
static const float kPsycho[21] = {   // over [-1,1]
	-1.0f, -0.712f, -0.453f, -0.303f, -0.216f, -0.148f, -0.116f, -0.08f, -0.061f, -0.034f,
	0.0f,
	0.034f, 0.061f, 0.08f, 0.116f, 0.148f, 0.216f, 0.303f, 0.453f, 0.712f, 1.0f
};
static const float kSteerRange[10] = { 40.0f, 20.0f, 10.0f, 5.0f, 3.5f, 3.1f, 2.9f, 2.9f, 2.9f, 2.9f };   // deg over [0,160] m/s
static const float kSteerSpeed[10] = { 1.0f, 0.92f, 0.78f, 0.64f, 0.54f, 0.48f, 0.42f, 0.38f, 0.34f, 0.32f };   // over [0,160] m/s
static const float kRangeCoeff[6] = { 1.0f, 1.0f, 1.1f, 1.2f, 1.25f, 1.35f };   // [0,1]
static const float kYawCoeff[5] = { 0.5f, 0.6f, 0.7f, 0.8f, 0.95f };   // [0,1]
static const float kInputSpeedCoeff[6] = { 1.0f, 1.05f, 1.1f, 1.5f, 2.2f, 3.1f };   // [0,10]
static const float kInputCoeff[6] = { 1.0f, 1.05f, 1.1f, 1.2f, 1.3f, 1.4f };   // [0,1]

// The drift (#268 E). The tables below are read out of Speed.elf; the
// stabilizer's graphs are built by the static initialiser at 0x80041BC0 from
// the constants at 0x803334C8 (degrees times 0.0174533).
//
// JoystickInputToSteerRemapDrift, PsychoSteeringCurveTable[4] (0x80352FA4),
// over [-1,1]: DoUndergroundSteering's input curve in a drag or a drift.
static const float kPsychoDrift[21] = {
	-1.0f, -1.0f, -0.688f, -0.492f, -0.319f, -0.228f, -0.16f, -0.123f, -0.085f, -0.05f,
	0.0f,
	0.05f, 0.085f, 0.123f, 0.16f, 0.228f, 0.319f, 0.492f, 0.688f, 1.0f, 1.0f
};
// DriftYawCoeffTable (0x80396030, its data 0x80352E64), over [0,1]:
// CalculateMaxSteering's counter-steer coefficient in a drag or a drift.
static const float kDriftYawCoeff[5] = { 0.2f, 0.3f, 0.5f, 0.7f, 1.0f };
// DriftControlData (0x80352FF8), the DriftControlTable graph: the steering
// lock's factor over |Car+0x120|, the yaw acceleration in rad/s^2.
static const float kDriftControl[6][2] = {
	{ 0.0f, 1.0f }, { 1.0f, 1.1f }, { 2.0f, 1.2f }, { 3.0f, 1.25f }, { 4.0f, 1.3f }, { 5.0f, 1.35f }
};
// PostCollisionSteerReductionData (0x803960BC), the PostCollisionSteerReduction
// graph (r13-0x53D8) the PS2 static constructor builds (0x80318BE4, 0x80318C20,
// 0x80318C00, 0x80318C9C, 0x80318BF0): the share of the steering lock kept
// over the seconds since the last hit, at full speed.
static const float kPostCollisionSteer[4][2] = {
	{ 0.0f, 0.15f }, { 0.15f, 0.5f }, { 0.5f, 0.7f }, { 0.7f, 1.0f }
};
// DriftStabilizerDataAI / DriftStabilizerDataPlayer (0x803DD9B8 / 0x803DD9F0):
// the stabilizer's factor over |yaw| in radians. They differ only at 90 deg.
#define DRIFT_DEG(d) ((d) * 0.0174533f)
static const float kDriftStabAI[7][2] = {
	{ 0.0f, 0.0f }, { DRIFT_DEG(15.0f), 0.1f }, { DRIFT_DEG(30.0f), 0.45f },
	{ DRIFT_DEG(45.0f), 0.85f }, { DRIFT_DEG(60.0f), 0.95f }, { DRIFT_DEG(89.0f), 1.15f },
	{ DRIFT_DEG(90.0f), 1.0f }
};
static const float kDriftStabPlayer[7][2] = {
	{ 0.0f, 0.0f }, { DRIFT_DEG(15.0f), 0.1f }, { DRIFT_DEG(30.0f), 0.45f },
	{ DRIFT_DEG(45.0f), 0.85f }, { DRIFT_DEG(60.0f), 0.95f }, { DRIFT_DEG(89.0f), 1.15f },
	{ DRIFT_DEG(90.0f), 0.0f }
};
// DriftRearFrictionData (0x8035FBC4), DriftRearFrictionTable over [0,1].
static const float kDriftRearFriction[10] = {
	1.1f, 0.93f, 0.86f, 0.82f, 0.8f, 0.792f, 0.78f, 0.77f, 0.765f, 0.76f
};
// TheMiscCarTunings (0x8035FD9C), the stabilizer's fields of its three
// records: +0x28 the strength by the race's AI skill (TheRaceParameters
// +0x78, for every car, the player's included), +0x34 the rear friction's mix
// of slip angle and yaw rate, +0x38 its scale. GetMiscCarTunings (0x801C997C)
// picks the record by the drive split CPI+0x1C0: [0] under 0.2 (front drive),
// [2] over 0.8 (rear drive), [1] between.
typedef struct { float stab[3]; float mix; float rearMul; } DriftMisc;
static const DriftMisc kDriftMisc[3] = {
	{ { -10.0f, -7.0f, -4.0f }, 0.35f, 1.0f },
	{ { -7.5f, -5.0f, -4.0f }, 0.55f, 1.0f },
	{ { -15.0f, -10.0f, -6.0f }, 0.5f, 1.125f },
};
static float graphGet(const float (*p)[2], int n, float x);

// The 350Z's CarPhysicsInfo as shipped (CTI+0x110..+0x3C0 of its GLOBALB
// record), used when a car's own block is missing or unusable so that the
// fallback is a real car and not an invented one.
static const u32 k350zCpi[CARPHYS_CPI_SIZE / 4] = {
	0x3E800001, 0x00000000, 0x3E800001, 0x00000000, 0x3FAED3D9, 0x3F4147AF,
	0x3E2ED13B, 0x00000000, 0x3EA8240D, 0x3E666668, 0x00000000, 0x3F61BB07,
	0x3F722921, 0x00000000, 0x00000000, 0x00000000, 0x3FAED3D9, 0xBF4147AF,
	0x3E2ED13B, 0x00000000, 0x3EA8240D, 0x3E666668, 0x00000001, 0xBF61BB07,
	0xBF722921, 0x00000000, 0x00000000, 0x00000000, 0xBFA5C290, 0xBF4147AF,
	0x3E2ED13B, 0x00000000, 0x3EAAB369, 0x3E70A3D8, 0x00000002, 0xBF61BB07,
	0xBF7305FC, 0x00000000, 0x00000000, 0x00000000, 0xBFA5C290, 0x3F4147AF,
	0x3E2ED13B, 0x00000000, 0x3EAAB369, 0x3E70A3D8, 0x00000003, 0x3F61BB07,
	0x3F7305FC, 0x00000000, 0x00000000, 0x00000000, 0x41400000, 0x3F800000,
	0x3F800000, 0x3FD70A3E, 0x3FCA3D71, 0x00000000, 0x00000000, 0x00000000,
	0x41400000, 0x3F800000, 0x3F800000, 0x3FD9999A, 0x3FCAE148, 0x00000000,
	0x00000000, 0x00000000, 0x3FB9999A, 0x4089999A, 0x3FE4BC6B, 0x3FA51EB8,
	0x3F1641FA, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x401BDBC0,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x4027AF4B, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x3F800000, 0x4002E148, 0x00000000,
	0x00000000, 0x00000000, 0x40A9999A, 0x42480000, 0x40A4CCCD, 0x40C00000,
	0x41A80000, 0x3E19999B, 0xBE19999B, 0x3D3851ED, 0x40A33333, 0x422C0000,
	0x40ACCCCD, 0x40B66666, 0x41B80000, 0x3E19999B, 0xBE19999B, 0x3D4CCCCE,
	0x3F4CCCCE, 0x43FA0000, 0x40628F5C, 0x40628F5C, 0x3F800000, 0x3D23D70C,
	0x00000006, 0x40000000, 0xC0728F5C, 0x00000000, 0x40728F5C, 0x40147AE1,
	0x3FCF5C2A, 0x3FA28F5C, 0x3F866666, 0x3F6B8520, 0x44480000, 0x45DAC000,
	0x45EA6000, 0x39B78036, 0x3E570A3F, 0x3E800001, 0x3E9EB853, 0x3EB33334,
	0x3EB851ED, 0x3EBD70A5, 0x3EB3B647, 0x3E99999B, 0x3E828F5D, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x3E2E147C, 0x3E947AE2, 0x3E4CCCCE, 0x00000000,
	0x00000000, 0x00000000, 0x39DC3374, 0x3E147AE2, 0x3E19999B, 0x3CE147B0,
	0x3F8147AE, 0x3EFD70A5, 0x3ECCCCCE, 0x3EF0A3D8, 0x00000000, 0x40200000,
	0x41880000, 0x00000000, 0x431B0000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

// ---------------------------------------------------------------------------
// Helpers

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// TableBase::GetValue: n samples spread evenly over [x0,x1], held at the ends.
static float tableGet(const float* d, int n, float x0, float x1, float x) {
	float t = (x - x0) * (float)(n - 1) / (x1 - x0);
	if (t < 0.0f) return d[0];
	int i = (int)t;
	if (i >= n - 1) return d[n - 1];
	return d[i] + (d[i + 1] - d[i]) * (t - (float)i);
}

static float cf(const CarSim* s, int off) {
	return s->cpi[off >> 2];
}

static int ci(const CarSim* s, int off) {
	int v;
	memcpy(&v, &s->cpi[off >> 2], sizeof(v));
	return v;
}

static const float* cpiAt(const CarSim* s, int off) {
	return &s->cpi[off >> 2];
}

static float dot3(const float* a, const float* b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float len3(const float* a) {
	return sqrtf(dot3(a, a));
}

static void cross3(const float* a, const float* b, float* o) {
	float x = a[1] * b[2] - a[2] * b[1];
	float y = a[2] * b[0] - a[0] * b[2];
	float z = a[0] * b[1] - a[1] * b[0];
	o[0] = x; o[1] = y; o[2] = z;
}

static void clampLen3(float* v, float m) {
	float l = len3(v);
	if (l > m) {
		float k = m / l;
		v[0] *= k; v[1] *= k; v[2] *= k;
	}
}

// Car frame to world: v.x * fwd + v.y * left + v.z * up.
static void toWorld(const float R[3][3], const float* v, float* o) {
	float x = v[0] * R[0][0] + v[1] * R[1][0] + v[2] * R[2][0];
	float y = v[0] * R[0][1] + v[1] * R[1][1] + v[2] * R[2][1];
	float z = v[0] * R[0][2] + v[1] * R[1][2] + v[2] * R[2][2];
	o[0] = x; o[1] = y; o[2] = z;
}

// World to car frame.
static void toCar(const float R[3][3], const float* v, float* o) {
	float x = dot3(R[0], v), y = dot3(R[1], v), z = dot3(R[2], v);
	o[0] = x; o[1] = y; o[2] = z;
}

static float wrapPi(float a) {
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a <= -3.14159265f) a += 6.2831853f;
	return a;
}

static float dtRatio(const CarSim* s, int gear) {
	return cf(s, CPI_DTI + DTI_RATIO + 4 * gear);
}

static int numGears(const CarSim* s) {
	return ci(s, CPI_DTI + DTI_NUM_GEARS);
}

// World::Random: uniform in [0, range). A fixed LCG rather than retail's
// generator (PORT), so a run is repeatable.
static float randomRange(CarSim* s, float range) {
	s->rng = s->rng * 1664525u + 1013904223u;
	return (float)(s->rng >> 8) * (1.0f / 16777216.0f) * range;
}

// CarState::GetYaw: the angle from the direction of travel to the heading,
// zero below 1 m/s forward.
static float carYaw(const CarSim* s) {
	if (s->vfwd < 1.0f) return 0.0f;
	float heading = atan2f(s->rot[0][1], s->rot[0][0]);
	float travel = atan2f(s->rbVel[1], s->rbVel[0]);
	return wrapPi(heading - travel);
}

// CarState::GetYawAlways (0x801AA360): GetYaw without its 1 m/s floor.
static float carYawAlways(const CarSim* s) {
	float heading = atan2f(s->rot[0][1], s->rot[0][0]);
	float travel = atan2f(s->rbVel[1], s->rbVel[0]);
	return wrapPi(heading - travel);
}

// The "special physics" test DoUndergroundSteering, CalculateMaxSteering and
// TuneWheelParams share: TheRaceParameters +0x40 (drag) or +0x44 (drift).
// Its g_tweak terms are 0 on the disc.
static bool driftPhysics(const CarSim* s) {
	return s->drag || s->drift;
}

// ---------------------------------------------------------------------------
// The world, as planes (PORT; see the header comment)

// Elevation of the plane kept for `slot` under (x, y), and its normal. -10000
// with no plane at all, which is what retail's CheckTerrain reads on a miss.
static float groundElev(const CarSim* s, int slot, float x, float y, float* n) {
	const CarSimPlane* p = &s->plane[slot];
	if (!p->valid) {
		n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f;
		return -10000.0f;
	}
	n[0] = p->n[0]; n[1] = p->n[1]; n[2] = p->n[2];
	return p->p[2] - (p->n[0] * (x - p->p[0]) + p->n[1] * (y - p->p[1])) / p->n[2];
}

// The record's body box, as eight corners from the centre of gravity: the box
// is centred on the record origin in plan, and runs from the wheel centres up
// to the body's height above the ground at rest.
static void boxCorner(const CarSim* s, int k, float* c) {
	const float L = cf(s, CPI_DIMS), W = cf(s, CPI_DIMS + 4), H = cf(s, CPI_DIMS + 8);
	float zLo = 0.0f;
	for (int i = 0; i < CARPHYS_WHEELS; i++) zLo += s->susp[i].attach[2];
	zLo *= 0.25f;
	c[0] = -s->cg[0] + ((k & 1) ? 0.5f : -0.5f) * L;
	c[1] = -s->cg[1] + ((k & 2) ? 0.5f : -0.5f) * W;
	c[2] = (k & 4) ? (H - s->hRest) : zLo;
}

// The body, as points from the centre of gravity. Retail's body is the car's
// collision hull (CarCollisionBody's ctor, Speed.elf 0x801AFAD4: the "<CAR>_CV"
// polytope, MoveCenterOfGravity'd by the CarPhysicsInfo CG), and that is what
// meets the world once the car is off its wheels -- GetGJKPoly (0x801AFF10)
// hands back the polytope's bounding box only for an AI car on all four
// wheels against another such car. So the hull's vertices, less the CG; the
// box's eight corners when there is no hull.
//
// The box is not a stand-in on its side. Its floor is the wheel centres,
// 0.08-0.13 m below the CG on the traffic record every traffic car shares,
// where their hulls go down 0.17-0.26 m: a car on its side had its CG barely
// inside the edge it stood on, and the sideways scrub of a slide tipped it
// back onto its wheels (TASKS.md #316, the user's "moonwalked on its side").
static int bodyPoints(const CarSim* s) {
	return s->hullN > 0 ? s->hullN : 8;
}

static void bodyPoint(const CarSim* s, int k, float* c) {
	if (s->hullN <= 0) {
		boxCorner(s, k, c);
		return;
	}
	const float* v = s->hull + k * 3;
	c[0] = v[0] - s->cg[0];
	c[1] = v[1] - s->cg[1];
	c[2] = v[2] - s->cg[2];
}

// One batched probe a frame: the four suspension attach points and the four
// lowest points of the body, each at where it will be half way through the
// frame. A miss keeps that slot's last plane, because a miss in the middle of
// the city is the pager not having caught up, not a hole.
static void probeGround(CarSim* s, const CarSimWorld* w, float dt) {
	if (!w || !w->groundProbe) return;

	float ahead[2] = { s->rbVel[0] * dt * 0.5f, s->rbVel[1] * dt * 0.5f };
	float xy[CARPHYS_WHEELS * 4];
	float p[3];
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		toWorld(s->rot, s->susp[i].attach, p);
		xy[i * 2]     = s->rbPos[0] + p[0] + ahead[0];
		xy[i * 2 + 1] = s->rbPos[1] + p[1] + ahead[1];
	}

	// The four lowest of the body's points, in world z.
	const int np = bodyPoints(s);
	float cz[CARSIM_HULL_MAX], cxy[CARSIM_HULL_MAX][2];
	for (int k = 0; k < np; k++) {
		float c[3];
		bodyPoint(s, k, c);
		toWorld(s->rot, c, p);
		cxy[k][0] = s->rbPos[0] + p[0] + ahead[0];
		cxy[k][1] = s->rbPos[1] + p[1] + ahead[1];
		cz[k] = p[2];
	}
	bool used[CARSIM_HULL_MAX] = { false };
	for (int j = 0; j < CARPHYS_WHEELS; j++) {
		int best = -1;
		for (int k = 0; k < np; k++)
			if (!used[k] && (best < 0 || cz[k] < cz[best])) best = k;
		used[best] = true;
		s->corner[j] = best;
		xy[(CARPHYS_WHEELS + j) * 2]     = cxy[best][0];
		xy[(CARPHYS_WHEELS + j) * 2 + 1] = cxy[best][1];
	}

	const int n = CARPHYS_WHEELS * 2;
	float z[CARPHYS_WHEELS * 2];
	bool hit[CARPHYS_WHEELS * 2];
	float nrm[CARPHYS_WHEELS * 2 * 3];
	signed char ter[CARPHYS_WHEELS * 2];
	w->groundProbe(w->ctx, xy, n, s->rbPos[2] + PORT_PROBE_CEILING, z, hit, nrm, ter);
	for (int k = 0; k < n; k++) {
		if (!hit[k] || nrm[k * 3 + 2] < 0.001f) continue;
		CarSimPlane* pl = &s->plane[k];
		pl->p[0] = xy[k * 2];
		pl->p[1] = xy[k * 2 + 1];
		pl->p[2] = z[k];
		pl->n[0] = nrm[k * 3];
		pl->n[1] = nrm[k * 3 + 1];
		pl->n[2] = nrm[k * 3 + 2];
		pl->valid = true;
		if (k < CARPHYS_WHEELS) s->wheel[k].terrain = ter[k];
	}
}

// ---------------------------------------------------------------------------
// RigidBody

// RigidBody::ApplyForce: the force is capped, the moment is taken from the
// uncapped force and capped on its own.
static void rbApplyForce(CarSim* s, const float* F, const float* r) {
	float f[3] = { F[0], F[1], F[2] };
	clampLen3(f, RB_FORCE_CLAMP);
	s->F[0] += f[0]; s->F[1] += f[1]; s->F[2] += f[2];
	float t[3];
	cross3(r, F, t);
	clampLen3(t, RB_TORQUE_CLAMP);
	s->T[0] += t[0]; s->T[1] += t[1]; s->T[2] += t[2];
}

// RigidBody::ApplyForceInFrame: both vectors in the car's frame.
static void rbApplyForceInFrame(CarSim* s, const float* Fc, const float* rc) {
	float F[3], r[3];
	toWorld(s->rot, Fc, F);
	toWorld(s->rot, rc, r);
	rbApplyForce(s, F, r);
}

// RigidBody::ApplyImpulse (ApplyImpulseWithScaledInverseInertiaTensor with a
// scale of `spin`).
static void rbApplyImpulse(CarSim* s, const float* J, const float* r, float spin) {
	float j[3] = { J[0], J[1], J[2] };
	clampLen3(j, RB_IMPULSE_CLAMP);
	s->P[0] += j[0]; s->P[1] += j[1]; s->P[2] += j[2];
	float l[3];
	cross3(r, j, l);
	clampLen3(l, RB_IMPULSE_MOMENT_CLAMP);
	s->L[0] += spin * l[0]; s->L[1] += spin * l[1]; s->L[2] += spin * l[2];
}

// The inverse inertia in use: Izz scaled up by the record's factor while
// Z-rotation damping is on, which it is for the player (SetZRotationDamping).
static void rbInvInertiaWorld(const CarSim* s, const float* v, float* o) {
	const float* inv = s->zdamp ? s->IinvZ : s->Iinv;
	float c[3];
	toCar(s->rot, v, c);
	c[0] *= inv[0]; c[1] *= inv[1]; c[2] *= inv[2];
	toWorld(s->rot, c, o);
}

// The velocities half of RigidBody::ComputeAuxiliaries.
static void rbVelocities(CarSim* s) {
	s->rbVel[0] = s->P[0] * s->invMass;
	s->rbVel[1] = s->P[1] * s->invMass;
	s->rbVel[2] = s->P[2] * s->invMass;
	float v = len3(s->rbVel);
	if (v > RB_VELOCITY_CLAMP && s->mass > 1.8f) {
		float k = RB_VELOCITY_CLAMP / v;
		for (int i = 0; i < 3; i++) {
			s->P[i] *= k;
			s->rbVel[i] = s->P[i] * s->invMass;
		}
	}
	rbInvInertiaWorld(s, s->L, s->w);
	clampLen3(s->w, RB_ANG_VEL_CLAMP);
}

// RigidBody::ComputeAuxiliaries.
static void rbAux(CarSim* s) {
	rbVelocities(s);
	if (s->damping < 0.0f) s->damping = 0.0f;
	s->F[0] = s->F[1] = s->F[2] = 0.0f;
	s->T[0] = s->T[1] = s->T[2] = 0.0f;
	if (s->g > 0.0f) {
		float grav[3] = { 0.0f, 0.0f, -s->g * s->mass };
		float zero[3] = { 0.0f, 0.0f, 0.0f };
		rbApplyForce(s, grav, zero);
	}
}

typedef struct {
	float pos[3], rot[3][3], P[3], L[3], damping;
	float angle[CARPHYS_WHEELS], w[CARPHYS_WHEELS];
	float engW, clutch;
} Deriv;

// RigidBody::GetDerivative, speed governor first.
static void rbDeriv(CarSim* s, Deriv* d) {
	if (s->governor != 0.0f) {
		float lim = s->governor * s->govBoost + RB_GOVERNOR_SLACK;
		// RigidBody +0x148: the PhysicsMover sets it from the car record, a
		// MellowMover's freshly built body leaves it 0.
		if (s->boostFlag && !s->mellow) lim *= RB_GOVERNOR_BOOST;
		float fz = s->rot[0][2];
		float f = fabsf(fz) > 0.3f ? 0.3f : fabsf(fz);
		float factor = fz <= 0.0f ? 1.0f + f * 0.1f / 0.3f : 1.0f - f * 0.1f / 0.3f;
		float vxy = sqrtf(s->rbVel[0] * s->rbVel[0] + s->rbVel[1] * s->rbVel[1]);
		if (lim * factor < vxy * 3.6f / 1.61f && dot3(s->rot[0], s->F) > 0.0f) {
			float c[3];
			toCar(s->rot, s->F, c);
			c[0] = -5.0f * f < RB_GOVERNOR_DRAG ? -5.0f * f : RB_GOVERNOR_DRAG;
			toWorld(s->rot, c, s->F);
		}
	}

	for (int i = 0; i < 3; i++) {
		d->pos[i] = s->rbVel[i];
		d->P[i] = s->F[i];
		d->L[i] = s->T[i];
	}
	for (int r = 0; r < 3; r++) cross3(s->w, s->rot[r], d->rot[r]);

	float wm = len3(s->w), vm = len3(s->rbVel);
	if (s->flag140) {
		float k = tableGet(kAngMomDamp, 6, 0.0f, 10.0f, wm);
		for (int i = 0; i < 3; i++) d->L[i] -= k * s->L[i];
	}
	float x = wm * 0.5f > vm * 0.25f ? wm * 0.5f : vm * 0.25f;
	d->damping = s->damping < tableGet(kMotionDamp, 8, 0.0f, 1.0f, x) ? 0.1f : -10.0f;
	if (s->flag13c) {
		for (int i = 0; i < 3; i++) {
			d->L[i] += -2.0f * s->damping * s->L[i];
			d->P[i] += -8.0f * s->damping * s->P[i];
		}
	}
	if (s->yawClamp != 0.0f) {
		if (s->T[2] > s->yawClamp) d->L[2] = s->yawClamp;
		else if (s->T[2] < -s->yawClamp) d->L[2] = -s->yawClamp;
	}
}

// Gram-Schmidt on the rows (PORT: retail's own routine is not ported).
static void orthonormalise(float R[3][3]) {
	float l = len3(R[0]);
	if (l < 1e-6f) return;
	for (int i = 0; i < 3; i++) R[0][i] /= l;
	float d = dot3(R[1], R[0]);
	for (int i = 0; i < 3; i++) R[1][i] -= d * R[0][i];
	l = len3(R[1]);
	if (l < 1e-6f) return;
	for (int i = 0; i < 3; i++) R[1][i] /= l;
	cross3(R[0], R[1], R[2]);
}

// ---------------------------------------------------------------------------
// Wheel and Suspension

// Wheel::ComputeAuxiliaries.
static void wheelAux(CarSimWheel* wh) {
	wh->T = 0.0f;
	wh->s = 0.0f;
	wh->Flong = 0.0f;
	wh->Flat = 0.0f;
	if (wh->angle > 6283.18f || wh->angle < -6283.18f) wh->angle = 0.0f;
	else if (wh->angle > 628.318f) wh->angle -= 628.318f;
	else if (wh->angle < -628.318f) wh->angle += 628.318f;
	if ((wh->dir == 0 && wh->w < 0.0f) || (wh->dir == 2 && wh->w > 0.0f)) wh->w = 0.0f;
	wh->dir = wh->w > 1e-5f ? 0 : (wh->w < -1e-5f ? 2 : 1);
}

// Suspension::ComputeAuxiliaries: where the wheel is, how fast its contact
// patch moves, and the spring / damper / anti-roll force.
static void suspAux(CarSim* s, int i) {
	CarSimSusp* su = &s->susp[i];
	CarSimWheel* wh = &s->wheel[i];
	const float* info = su->info;

	float P[3];
	toWorld(s->rot, su->attach, P);
	P[0] += s->rbPos[0]; P[1] += s->rbPos[1]; P[2] += s->rbPos[2];

	// Wheel::CheckTerrain: the surface's height over the attach point, along
	// the surface normal, over the car's up.
	float n[3];
	float elev = groundElev(s, i, P[0], P[1], n);
	float nu = dot3(n, s->rot[2]);
	float ct = nu < 0.001f ? -100.0f : n[2] * (elev - P[2]) / nu;
	float x = ct + wh->radius;

	float xold = su->x;
	if (x > info[5]) {
		x = info[5];
		su->onGround = 1;
	} else {
		su->onGround = x >= info[6];
		if (x < info[6]) x = info[6];
	}
	su->x = x;
	if (s->ugly > 0.0001f) su->xdot = (x - xold) / s->ugly;

	su->wcpos[0] = su->attach[0];
	su->wcpos[1] = su->attach[1];
	su->wcpos[2] = su->attach[2] + x;

	// The wheel's frame: steered at the front (TwoWheelAckermanSteering).
	if (i < 2) {
		float a = i == 0 ? s->steerFL : s->steerFR;
		float c = cosf(a), sn = sinf(a);
		wh->rowC[0][0] = c;   wh->rowC[0][1] = sn;
		wh->rowC[1][0] = -sn; wh->rowC[1][1] = c;
	} else {
		wh->rowC[0][0] = 1.0f; wh->rowC[0][1] = 0.0f;
		wh->rowC[1][0] = 0.0f; wh->rowC[1][1] = 1.0f;
	}

	float Pw[3], wxp[3];
	toWorld(s->rot, su->wcpos, Pw);
	cross3(s->w, Pw, wxp);
	for (int k = 0; k < 3; k++) {
		wh->cvel[k] = s->rbVel[k] + wxp[k];
		wh->wpos[k] = Pw[k] + s->rbPos[k];
		wh->up[k] = s->rot[2][k];
		wh->n[k] = n[k];
	}
	float f[3] = { wh->rowC[0][0], wh->rowC[0][1], 0.0f };
	float sd[3] = { wh->rowC[1][0], wh->rowC[1][1], 0.0f };
	toWorld(s->rot, f, wh->fwd);
	toWorld(s->rot, sd, wh->side);

	float F = 0.0f;
	if (su->onGround) {
		const CarSimSusp* pa = &s->susp[su->partner];
		F = su->xdot * (su->xdot > 0.0f ? info[2] : info[3]) +
			x * info[1] * ((x > 0.0f ? x : 0.0f) * info[0] + 1.0f) +
			su->preload + info[4] * (x - pa->x);
		if (F < 0.0f) F = 0.0f;
	}
	su->F = F;
}

// Wheel::ExchangeZForces: the load the tyre carries, less as the wheel leans
// onto a surface it is not square to.
static void wheelExchangeZ(CarSimWheel* wh, float F, int onGround) {
	wh->onGround = onGround;
	if (onGround)
		for (int k = 0; k < 3; k++) wh->gpt[k] = wh->wpos[k] - wh->radius * wh->n[k];
	if (F > 0.0f) {
		float sn = dot3(wh->side, wh->n);
		float k = 1.0f - 4.0f * sn * sn;
		wh->Fz = F * (k < 0.3f ? 0.3f : k);
	} else {
		wh->Fz = 0.0f;
	}
}

// Wheel::GetPilotFactor: a rear tyre that is driving has a little less grip
// below 30 mph, full grip from 50. GuessGroundForce and FinalizeGroundForce
// apply it only in control mode 1 (the Wheel's +0x20 is its CarState).
static float pilotFactor(const CarSim* s, const CarSimWheel* wh, int i) {
	if (s->controlMode != 1) return 1.0f;
	if (!wh->locked && wh->w >= 0.0f && i > 1) {
		float f = clampf((s->vfwd - 13.4108f) / 8.94055f, 0.0f, 1.0f);
		return (1.0f - PILOT_FACTOR) * f + PILOT_FACTOR;
	}
	return 1.0f;
}

// Wheel::CheckForBrakeLock.
static void checkBrakeLock(const CarSim* s, CarSimWheel* wh, float Fp) {
	const float* ctl = cpiAt(s, CPI_CTL);
	float tb = (BRAKING_TORQUE * wh->brake * ctl[1] + EBRAKING_TORQUE * wh->ebrake * ctl[2]) *
		STATIC_TO_DYNAMIC_BRAKE;
	if (Fp * wh->radius + fabsf(wh->w) * BRAKE_LOCK_W_FACTOR < tb) {
		wh->locked = tb > 1.0f;
		wh->w = 0.0f;
	} else {
		wh->locked = 0;
	}
}

// Wheel::GetLateralForce: the load-sensitivity tables, read at the slip angle
// in two-degree steps.
static float lateralForce(const CarSimWheel* wh, float Fz, float a) {
	const float* tire = wh->tire;
	float f = a * 57.29578f * (12.0f / tire[0]) * 0.5f;
	float L = Fz * 0.8f / tire[1];
	int i = (int)f;
	float v;
	if (i < 6) {
		float a0 = tableGet(kLoadSens[i], 6, 0.0f, 10.0f, L);
		float a1 = tableGet(kLoadSens[i + 1], 6, 0.0f, 10.0f, L);
		v = a0 + (a1 - a0) * (f - (float)i);
	} else {
		v = tableGet(kLoadSens[6], 6, 0.0f, 10.0f, L);
	}
	return v * tire[2] * 2.5f;
}

// The contact patch's velocity in the wheel's frame, and the slip angle as
// GuessGroundForce and FinalizeGroundForce take it.
static float wheelSlipAngle(const CarSimWheel* wh, float* vx, float* vy) {
	*vx = dot3(wh->fwd, wh->cvel);
	*vy = dot3(wh->side, wh->cvel);
	float a = atan2f(*vy, *vx);
	if (*vx < 0.0f) {
		a += 3.14159265f;
		if (a > 3.14159265f) a -= 6.2831853f;
	}
	return a;
}

// Wheel::GuessGroundForce(0).
static void wheelGuess(CarSim* s, int i) {
	CarSimWheel* wh = &s->wheel[i];
	const float* tire = wh->tire;
	const float* ctl = cpiAt(s, CPI_CTL);

	float te = EBRAKING_TORQUE * wh->ebrake * ctl[2];
	float tb = BRAKING_TORQUE * wh->brake * ctl[1];
	if (!wh->locked) wh->T += wh->w > 0.0f ? -tb : tb;
	if (!wh->locked) wh->T += wh->w > 0.0f ? -te : te;

	float vx, vy;
	float a = wheelSlipAngle(wh, &vx, &vy);
	float r = wh->radius;
	float sl = wh->w * r - vx;
	wh->s = sl;
	wh->slip = sl;
	if (sl < -MAX_STATIC_SLIP && wh->T < 0.0f && te == 0.0f)
		wh->T *= MAX_STATIC_SLIP / -sl;

	float m2 = sl * sl + vy * vy;
	float slipMag = m2 > 5e-11f ? sqrtf(m2) : 0.0f;
	int sliding = 0;
	float k = 0.0f, mu = 0.0f;
	if (slipMag != 0.0f && (vy != 0.0f || vx != 0.0f)) {
		mu = tire[4] * wh->grip1 * wh->grip2 * pilotFactor(s, wh, i);
		k = wh->Fz * mu / slipMag;
		float v2 = vx * vx + vy * vy;
		float vm = v2 > 5e-11f ? sqrtf(v2) : 0.0f;
		checkBrakeLock(s, wh, vm > 0.0f ? fabsf(vx) * wh->Fz * mu / vm : 1e30f);
		sliding = fabsf(sl) > MAX_STATIC_SLIP || tire[0] < fabsf(a) || wh->locked;
	}

	wh->sndSlip[0] = sliding ? sl : 0.0f;
	wh->sndSlip[1] = vy;
	if (sliding) {
		wh->Flong = k * sl * wh->longScale;
		wh->Flat = -k * vy * wh->latScale;
		if (s->vfwd * MPS_TO_MPH < 1.0f && mu > 0.1f) {
			wh->Flong /= mu;
			wh->Flat /= mu;
		}
		wh->sliding = 1;
	} else {
		wh->sliding = 0;
		wh->Flong = wh->longScale * wh->T / r;
		float sg = wh->latScale;
		if (vx <= 0.0f) sg = -sg;
		if (a > 0.0f) sg = -sg;
		wh->Flat = sg * lateralForce(wh, wh->Fz, fabsf(a));
	}
}

// Wheel::ModifyGroundForce: what of the drive goes into spinning the wheel up
// as the car accelerates. Reads column 0 of the wheel's car-frame matrix, as
// retail does.
static void wheelModify(CarSimWheel* wh, float ax, float ay) {
	float r = wh->radius;
	wh->Flong += (((wh->T - wh->Flong * r) / WHEEL_INERTIA) * r -
		(wh->rowC[0][0] * ax + wh->rowC[1][0] * ay)) * WHEEL_INERTIA / r;
}

// Wheel::FinalizeGroundForce: the friction cap, and the force into the world.
static void wheelFinalize(CarSim* s, int i) {
	CarSimWheel* wh = &s->wheel[i];
	int ellipse = wh->T > 0.0f && !wh->locked;
	if (ellipse) wh->Flong *= TIRE_ELLIPSE_RATIO;

	float mag = sqrtf(wh->Flong * wh->Flong + wh->Flat * wh->Flat);
	float cap = wh->Fz * wh->tire[3] * wh->grip1 * wh->grip2 * wh->grip3 * pilotFactor(s, wh, i);
	if (mag <= cap || mag <= 0.001f) {
		if (ellipse) wh->Flong /= TIRE_ELLIPSE_RATIO;
	} else {
		wh->Flong *= cap / mag;
		wh->Flat *= cap / mag;
	}
	if (cap <= 0.001f) wh->gripUse = 1.0f;
	else wh->gripUse = mag / cap > 1.0f ? 1.0f : mag / cap;
	if (wh->sliding || wh->locked) wh->gripUse = 1.0f;
	if (!wh->onGround) wh->gripUse = 0.0f;

	float vx, vy;
	float a = wheelSlipAngle(wh, &vx, &vy);
	if (fabsf(vx) > 1.0f) wh->Flong -= sinf(a) * wh->Flat * 0.15f;

	for (int k = 0; k < 3; k++)
		wh->F[k] = wh->Flong * wh->fwd[k] + wh->Flat * wh->side[k] + wh->Fz * wh->up[k];
}

// Wheel::GetDerivative.
static void wheelDeriv(const CarSim* s, CarSimWheel* wh, float* dAngle, float* dW) {
	*dAngle = wh->w;
	wh->g = 0.5f * (wh->g + (wh->T - wh->Flong * wh->radius));
	// Terrain rolling resistance 1 (ROAD); no damage wobble, so no +0x170 term.
	float d = (wh->g - wh->w * 0.002f) / WHEEL_INERTIA;
	if (wh->locked) d = 0.0f;
	// A gripping wheel is put back on road speed every substep.
	if (!wh->sliding && s->ugly > 0.001f) d -= wh->s / (wh->radius * s->ugly);
	*dW = d;
}

// Suspension::ApplyGroundPenaltyImpulse (0x801D1214): a bottomed-out wheel
// takes 0.225 of the speed it is driving into the ground with off the car.
// Returns the impulse's size, 0 for a wheel leaving the ground.
static float groundPenalty(CarSim* s, int i) {
	const CarSimWheel* wh = &s->wheel[i];
	float Pp[3], wxp[3], v[3];
	toWorld(s->rot, s->susp[i].wcpos, Pp);
	cross3(s->w, Pp, wxp);
	for (int k = 0; k < 3; k++) v[k] = s->rbVel[k] + wxp[k];
	float vn = dot3(wh->n, v);
	if (-vn < 0.0f) return 0.0f;
	const float j = -vn * GROUND_PENALTY * s->mass;
	float J[3];
	for (int k = 0; k < 3; k++) J[k] = wh->n[k] * j;
	rbApplyImpulse(s, J, Pp, 1.0f);
	return j;
}

// ---------------------------------------------------------------------------
// Nitrous (CarState, Speed.elf 0x801AA248..0x801AB1C4)
//
// The bottle is held as WorldTimer ticks: nosLeft is what was in it when it
// was last shut, and while it is open what has burnt since nosOnAt comes off
// that. NitrousContinuous (0x8046E31C) is 1, so a shot is the whole bottle for
// as long as the button is held (RacingJoyHandler stops it on the release).

#define NOS_TICKS 4000.0f   // WorldTimer per second (0.00025 at 0x80331A40)
#define NOS_UNSET 0x7FFFFFFF

// GameBrakerPercentThresholds (0x803335A8) and the two GameBreaker tables
// indexed by the level less one: a bottle that is not overfilled is level 1,
// where both multipliers are 1.
static const float kNosLevelPct[3] = { 0.0f, 1.4f, 1.7f };
static const float kNosTorqueMul[3] = { 1.0f, 1.4f, 1.7f };    // 0x803335B4
static const float kNosFrictionMul[3] = { 1.0f, 1.4f, 1.7f };  // 0x803335C0
// NOSCurve (0x8035FAB8), Engine+0x60: the nitrous's torque over [idle,
// redline], where the engine's own curve runs to the limiter.
static const float kNosCurve[9] = { 0.3f, 0.4f, 0.45f, 0.5f, 0.55f, 0.6f, 0.7f, 1.0f, 0.7f };

static bool nosTickSet(int t) { return t != 0 && t != NOS_UNSET; }
static int  nosNow(const CarSim* s) { return (int)(s->clock * NOS_TICKS); }

// ResetNitrous (0x801AA248): the bottle full and shut.
static void nosReset(CarSim* s) {
	float cap = cf(s, CPI_NOS_CAP);
	int bottles = ci(s, CPI_NOS_BOTTLES);
	s->nosCap = (int)(cap * NOS_TICKS + 0.5f);
	s->nosLeft = (int)(cap * NOS_TICKS + 0.5f);
	s->nosShot = bottles < 1 ? 0 : (int)(cap / (float)bottles * NOS_TICKS + 0.5f);
	s->nosOnAt = 0;
}

// GetRealNitrousRemaining (0x801AAD44): seconds left, less what is burning.
static float nosRemaining(const CarSim* s) {
	float left = nosTickSet(s->nosLeft) ? (float)s->nosLeft / NOS_TICKS : 0.0f;
	float used = 0.0f;
	if (nosTickSet(s->nosOnAt))
		used = (float)nosNow(s) / NOS_TICKS - (float)s->nosOnAt / NOS_TICKS;
	return left - used;
}

// The level IsNitrousOn and IsNitrousAvailable count off the fraction left:
// 3 past 1.7, 2 past 1.4, 1 past 0, else none.
static int nosLevelOf(float f) {
	if (f > kNosLevelPct[2]) return 3;
	for (int i = 2; i > 0; i--)
		if (f > kNosLevelPct[i - 1]) return i;
	return 0;
}

float carsim_nos_fraction(const CarSim* s) {
	float rem = nosRemaining(s);
	float cap = nosTickSet(s->nosCap) ? (float)s->nosCap / NOS_TICKS : 0.0f;
	float f = 0.0f;
	if (cap > 0.0f) {
		if (rem > cap) {
			f = (rem - cap) / cap;
			rem = cap;
		}
		f += rem / cap;
	}
	return f;
}

float carsim_nos_remaining(const CarSim* s) {
	return nosRemaining(s);
}

int carsim_nos_available(const CarSim* s) {
	if (nosTickSet(s->nosOnAt)) return 0;
	return nosLevelOf(carsim_nos_fraction(s));
}

bool carsim_nos_start(CarSim* s) {
	if (!carsim_nos_available(s)) return false;
	s->nosOnAt = nosNow(s);
	s->nosShot = s->nosLeft;   // NitrousContinuous
	return true;
}

void carsim_nos_stop(CarSim* s) {
	if (!nosTickSet(s->nosOnAt)) return;
	float left = (float)s->nosLeft / NOS_TICKS - (float)(nosNow(s) - s->nosOnAt) / NOS_TICKS;
	if (left < 0.0f) left = 0.0f;
	s->nosLeft = (int)(left * NOS_TICKS + 0.5f);
	s->nosShot = 0;            // NitrousContinuous
	s->nosOnAt = 0;
}

int carsim_nos_on(CarSim* s) {
	if (!nosTickSet(s->nosOnAt)) return 0;
	int level = nosLevelOf(carsim_nos_fraction(s));
	if (level == 0) carsim_nos_stop(s);
	return level;
}

// FillNitrous's step: d bottles more (or less) in what was left at the last
// shut, as seconds of the bottle's size.
static void nosAdd(CarSim* s, float cap, float d) {
	float left = nosTickSet(s->nosLeft) ? (float)s->nosLeft / NOS_TICKS : 0.0f;
	s->nosLeft = (int)((cap * d + left) * NOS_TICKS + 0.5f);
}

// FillNitrous (0x801AB1C4): a fill tops the bottle up to full, then what is
// over goes on into the overfill up to two bottles; a drain takes off the
// overfill first, then down to empty.
void carsim_nos_reset(CarSim* s) {
	nosReset(s);
}

void carsim_nos_fill(CarSim* s, float amount) {
	float cap = nosTickSet(s->nosCap) ? (float)s->nosCap / NOS_TICKS : 0.0f;
	float pct = carsim_nos_fraction(s);
	if (amount > 0.0f) {
		if (pct < 1.0f) {
			float d = fminf(1.0f - pct, amount);
			nosAdd(s, cap, d);
			amount -= d;
			pct += d;
		}
		if (amount <= 0.0f) return;
		nosAdd(s, cap, fminf(1.0f - (pct - 1.0f), amount));
	} else {
		if (pct > 1.0f) {
			float d = fmaxf(-(pct - 1.0f), amount);
			nosAdd(s, cap, d);
			amount -= d;
			pct += d;
		}
		if (amount >= 0.0f) return;
		nosAdd(s, cap, fmaxf(-pct, amount));
	}
}

// ---------------------------------------------------------------------------
// Engine

// Engine::ComputeEngineHeat (0x801B3420), at the step's dt: while heat
// builds (Engine::GetAccumulatingHeat, 0 <= heat < 2) it climbs 0.45 a
// second over the redline and sheds 0.1 a second under it, and at 1.2 the
// engine blows (2). It does not build while the launch's bonus is paying out
// in first.
static void engineHeat(CarSim* s, float dt) {
	if (!(s->heat >= 0.0f && s->heat < 2.0f)) return;
	if (s->dtGear == GEAR_FIRST && s->shiftBonus > 0.0f) return;
	float rate = cf(s, CPI_EI + EI_REDLINE) < s->engW * RPM_PER_RADS ? 0.45f : -0.1f;
	s->heat = fmaxf(0.0f, s->heat + rate * dt);
	if (s->heat >= 1.2f) s->heat = 2.0f;
}

// Engine::ComputeTurboPressure (0x801B3154), at the step's dt. Nothing moves
// without a turbo (Engine+0x98, turboMax, 0). The boost runs between a vacuum
// of -1/4 of the full boost (turboMax / 0.19) and that full boost:
//   0 at rest: under 0.3 effective throttle the vacuum, scaled by twice how
//     far the revs are from idle to the limiter (at most 1); more throttle
//     starts the build;
//   1 building: up 2 x (that rev fraction) x throttle a second, capped at the
//     full boost; off the throttle (and not on the limiter) the blow-off;
//   2 the blow-off: a sine wobble from the boost it let go at, decaying with
//     its clock, never under twice the vacuum nor over where it began; back
//     to rest after 0.5 s, or once a swing crosses within 0.15 of the last.
// It only feeds the HUD's TurboMeter: the torque's turbo share is in
// CarPhysicsInfo +0x200 already (BuildTurboPackage adds +0x224 into it).
static void engineTurbo(CarSim* s, float dt) {
	if (!(s->turboMax > 0.0f) || dt == 0.0f) return;
	const float* ei = cpiAt(s, CPI_EI);
	const float full = s->turboMax / 0.19f;
	const float rpm = s->engW * RPM_PER_RADS;
	const float vac = -(full * 0.25f);
	const float eff = s->limiter ? 0.0f : s->engThr * s->engMult;   // GetEffectiveThrottle
	const float span = ei[2] - ei[0];
	const float frac = span != 0.0f ? (rpm - ei[0]) / span : 0.0f;
	if (s->turboState == 1) {
		if (eff < 0.3f) {
			if (!s->limiter) {
				s->turboState = 2;
				s->turboFrom = s->turboBoost;
				s->turboT = dt;
			}
		} else {
			const float thr = 1.0f - eff < 0.0f ? 1.0f : eff;
			float b = 2.0f * frac * thr * dt + s->turboBoost;
			if (full - b < 0.0f) b = full;
			s->turboBoost = b;
		}
	} else if (s->turboState == 0) {
		if (eff < 0.3f) {
			float f = 2.0f * frac;
			if (1.0f - f < 0.0f) f = 1.0f;
			s->turboBoost = vac * f;
		} else {
			s->turboState = 1;
		}
	} else if (s->turboState == 2) {
		float t = s->turboT + 1.1f;
		t *= t;
		const float sn = sinf(t * t * 6.5f + 90.0f * 0.0174533f);
		float v = (s->turboFrom / full) * 0.015f * sn / s->turboT + vac;
		if (v - 2.0f * vac < 0.0f) v = 2.0f * vac;
		if (s->turboFrom - v < 0.0f) v = s->turboFrom;
		bool done = false;
		if (s->turboLast == 0.0f) {
			s->turboLast = v;
		} else {
			const float cur = s->turboBoost;
			if ((cur < v && v < s->turboLast) || (v < cur && s->turboLast < v)) {
				if (fabsf(cur - s->turboLast) >= 0.15f) s->turboLast = cur;
				else done = true;
			}
		}
		s->turboBoost = v;
		s->turboT += dt;
		if (done || s->turboT > 0.5f) {
			s->turboState = 0;
			s->turboLast = 0.0f;
		}
	}
}

// Engine::ComputeAuxiliaries: the rev range and the limiter, then the turbo
// and the heat.
static void engineAux(CarSim* s) {
	const float* ei = cpiAt(s, CPI_EI);
	float lo = ei[0] * RADS_PER_RPM, hi = ei[2] * RADS_PER_RPM;
	if (s->engW < lo) s->engW = lo;
	if (s->engW > hi) s->engW = hi;
	float rpm = s->engW * RPM_PER_RADS;
	s->engExt = 0.0f;
	if (!s->limiter) {
		if (rpm > ei[2] - 25.0f) s->limiter = 1;
	} else {
		if (rpm < ei[2] - ei[2] * 0.0125f) s->limiter = 0;
	}
	engineTurbo(s, s->ugly);
	engineHeat(s, s->ugly);
}

// SetBoost's argument: Car +0x478, never under 1 (TASKS.md #255).
static inline float catchupOf(const CarSim* s) {
	return s->catchup > 1.0f ? s->catchup : 1.0f;
}

// Engine::GetTotalEngineTorque.
static float engineTotal(const CarSim* s) {
	const float* ei = cpiAt(s, CPI_EI);
	float rpm = s->engW * RPM_PER_RADS;
	float eff = s->limiter ? 0.0f : s->engThr * s->engMult;
	// Engine::GetTorque scales the table by Engine +0x90, SetBoost's.
	float tq = tableGet(ei + 4, 9, ei[0], ei[2], rpm) * catchupOf(s);
	if (s->boostFlag) tq *= 1.05f;                        // TorqueHPMultiplier
	float total = tq * eff;
	// The nitrous, at the level DelegateDriverInput handed SetNitroFlag.
	if (s->nosLevel > 0)
		total += kNosTorqueMul[s->nosLevel - 1] * cf(s, CPI_NOS_TORQUE) *
			tableGet(kNosCurve, 9, ei[0], ei[1], rpm) * eff;
	// A hot engine loses up to 3/4 of its drive from 0.6 heat to 1.
	if (s->heat > 0.6f) total *= 1.0f - (fminf(s->heat, 1.0f) - 0.6f) / 0.4f * 0.75f;
	float br = -tableGet(ei + 22, 3, ei[0], ei[2], rpm) * (1.0f - eff);
	if (s->limiter) br += br;
	total += br;
	if (total < 0.0f && !s->limiter) total *= s->dtGear == GEAR_N ? 0.65f : 0.85f;
	return total;
}

// ---------------------------------------------------------------------------
// DriveTrain

// DriveTrain::GetDifferentialAngularVelocity: the driven axles' mean wheel
// speed, through the final drive and the gear.
static float diffAngVel(const CarSim* s) {
	float split = cf(s, CPI_DTI + DTI_SPLIT);
	float d = 0.0f;
	int rear = split >= 0.001f, front = split <= 0.999f;
	if (rear) d += (s->wheel[2].w + s->wheel[3].w) * 0.5f * cf(s, CPI_DTI + DTI_REAR_FINAL);
	if (front) d += (s->wheel[0].w + s->wheel[1].w) * 0.5f * cf(s, CPI_DTI + DTI_FRONT_FINAL);
	if (rear && front) d *= 0.5f;
	return d * dtRatio(s, s->dtGear);
}

// The share of an axle's torque the right-hand wheel of the pair (W1 / W2)
// gets: proportional to its speed inside a band, and 82.5% to the slower
// wheel outside it.
static float diffShare(const CarSimWheel* a, const CarSimWheel* b) {
	float band = (a->onGround && b->onGround) ? 0.35f : 0.5f;
	float sum = a->w + b->w;
	float f = fabsf(sum) <= 1e-5f ? 0.5f : b->w / sum;
	if (f < band) f = 0.825f;
	else if (f > 1.0f - band) f = 0.175f;
	return f;
}

// DriveTrain::ApplyClutchTorque.
static void applyClutchTorque(CarSim* s, float T) {
	if (s->dtGear == GEAR_N) return;
	s->clutchT += T;
	float f = 0.25f * (float)s->wheelsDown;
	s->engExt -= T * (f < 0.25f ? 0.25f : f);
	float tg = T * dtRatio(s, s->dtGear);
	float rear = tg * s->split * cf(s, CPI_DTI + DTI_REAR_FINAL);
	float front = tg * (1.0f - s->split) * cf(s, CPI_DTI + DTI_FRONT_FINAL);
	CarSimWheel* W = s->wheel;

	float ff = diffShare(&W[0], &W[1]);
	if (!W[0].locked) W[0].T += front * (1.0f - ff);
	if (!W[1].locked) W[1].T += front * ff;
	float rf = W[2].w + W[3].w;
	float band = (W[2].onGround && W[3].onGround) ? 0.35f : 0.5f;
	float fr = fabsf(rf) <= 1e-5f ? 0.5f : W[2].w / rf;
	if (fr < band) fr = 0.825f;
	else if (fr > 1.0f - band) fr = 0.175f;
	if (!W[2].locked) W[2].T += rear * fr;
	if (!W[3].locked) W[3].T += rear * (1.0f - fr);
}

// DriveTrain::GuessClutchTorque.
static void guessClutchTorque(CarSim* s) {
	s->clutchT = 0.0f;
	if (s->clutchState == 3) {
		applyClutchTorque(s, engineTotal(s));
	} else if (s->clutchState == 0 || s->clutchState == 2) {
		float slip = clampf(s->engW - s->diffW, -300.0f, 300.0f);
		float k = cf(s, CPI_DTI + DTI_CLUTCH_K);
		float rpmSlip = (s->diffW - s->engW) * RPM_PER_RADS;
		if (rpmSlip * s->rpmSlip < 0.0f && fabsf(rpmSlip) > 2000.0f && fabsf(s->rpmSlip) > 2000.0f)
			k *= 0.5f;
		s->rpmSlip = rpmSlip;
		applyClutchTorque(s, slip * k * s->clutch);
	}
}

// DriveTrain::ModifyClutchTorque: with the clutch in, the torque that makes
// the engine and the driven wheels accelerate together.
static void modifyClutchTorque(CarSim* s) {
	if (s->clutchState != 3) return;
	float inertia = cf(s, CPI_EI + EI_INERTIA);
	float split = cf(s, CPI_DTI + DTI_SPLIT);
	float fF = cf(s, CPI_DTI + DTI_FRONT_FINAL), rF = cf(s, CPI_DTI + DTI_REAR_FINAL);
	float ratio = dtRatio(s, s->dtGear);
	int rear = split >= 0.001f, front = split <= 0.999f;
	const CarSimWheel* W = s->wheel;

	float engAcc = (engineTotal(s) + s->engExt) / inertia;
	float denom = 1.0f / inertia;
	if (front) denom += (1.0f - s->split) / WHEEL_INERTIA * (fF * ratio) * (fF * ratio) * 0.5f;
	if (rear) denom += s->split / WHEEL_INERTIA * (rF * ratio) * (rF * ratio) * 0.5f;
	float diffAcc = 0.0f;
	if (rear)
		diffAcc += ((W[2].T - W[2].Flong * W[2].radius) / WHEEL_INERTIA +
			(W[3].T - W[3].Flong * W[3].radius) / WHEEL_INERTIA) * 0.5f * rF;
	if (front)
		diffAcc += ((W[0].T - W[0].Flong * W[0].radius) / WHEEL_INERTIA +
			(W[1].T - W[1].Flong * W[1].radius) / WHEEL_INERTIA) * 0.5f * fF;
	if (rear && front) diffAcc *= 0.5f;
	diffAcc *= ratio;
	applyClutchTorque(s, (engAcc - diffAcc) / denom);
}

// DriveTrain::SetDifferentialAngularVelocity.
static void setDiffAngVel(CarSim* s, float target) {
	float d = diffAngVel(s);
	float ratio = dtRatio(s, s->dtGear);
	if (ratio == 0.0f) return;
	float split = cf(s, CPI_DTI + DTI_SPLIT);
	float mph = s->speedAbs * MPS_TO_MPH;
	CarSimWheel* W = s->wheel;
	if (split >= 0.001f) {
		int eq = mph < 40.0f || (!W[2].onGround && !W[3].onGround);
		float dw = (target - d) / (ratio * cf(s, CPI_DTI + DTI_REAR_FINAL));
		if (eq) W[2].w = W[3].w = (W[2].w + W[3].w) * 0.5f;
		W[2].w += dw;
		W[3].w += dw;
	}
	if (split <= 0.999f) {
		int eq = mph < 40.0f || (!W[0].onGround && !W[1].onGround);
		float dw = (target - d) / (ratio * cf(s, CPI_DTI + DTI_FRONT_FINAL));
		if (eq) W[0].w = W[1].w = (W[0].w + W[1].w) * 0.5f;
		W[0].w += dw;
		W[1].w += dw;
	}
}

// DriveTrain::GetDriveWheelSlippage.
static float driveSlip(const CarSim* s) {
	float split = cf(s, CPI_DTI + DTI_SPLIT);
	float sum = 0.0f;
	int n = 0;
	if (split >= 0.001f) { sum += s->wheel[2].slip + s->wheel[3].slip; n += 2; }
	if (split <= 0.999f) { sum += s->wheel[0].slip + s->wheel[1].slip; n += 2; }
	return n ? sum / (float)n : 0.0f;
}

// DriveTrain::FinalizeClutchTorque.
static void finalizeClutchTorque(CarSim* s) {
	if (s->dtGear == GEAR_N || s->clutchState != 3 || s->wheelsDown <= 0) return;
	if (s->engThr <= 0.2f || driveSlip(s) <= 0.1f) {
		float tot = fabsf(engineTotal(s));
		s->engExt += clampf((s->diffW - s->engW) * cf(s, CPI_DTI + DTI_CLUTCH_K), -tot, tot);
	} else {
		s->diffW = s->engW;
		setDiffAngVel(s, s->engW);
	}
}

// DriveTrain::ChooseNewShiftModifiers: each somewhere in its range (the
// ctor's -100..0, or an AI drag racer's skill).
static void chooseShiftModifiers(CarSim* s) {
	s->mod78 = s->modLo[0] + randomRange(s, s->modHi[0] - s->modLo[0]);
	s->mod7c = s->modLo[1] + randomRange(s, s->modHi[1] - s->modLo[1]);
	s->mod80 = s->modLo[2] + randomRange(s, s->modHi[2] - s->modLo[2]);
}

// PerfectShiftMaxRPMDiff (0x8035FADC), [car class][gear]: how far under the
// shift point an upshift still earns a bonus.
static const float kPerfectShiftMaxRpmDiff[4][9] = {
	{ 0.0f,  560.0f, 360.0f, 175.0f, 160.0f, 140.0f, 140.0f, 0.0f, 0.0f },
	{ 0.0f,  600.0f, 400.0f, 230.0f, 220.0f, 160.0f, 160.0f, 0.0f, 0.0f },
	{ 0.0f,  850.0f, 550.0f, 300.0f, 250.0f, 200.0f, 180.0f, 0.0f, 0.0f },
	{ 0.0f, 1100.0f, 750.0f, 400.0f, 325.0f, 290.0f, 230.0f, 0.0f, 0.0f }
};

// DriveTrain::GetPotentialShiftBonus (0x801B54AC): an upshift out of `g` at
// `rpm` against the shift point up[g]. Over it by less than 5% of the range
// is perfect (1), further over is -1; under it, the bonus falls off across
// 95% of the range, and a scored shift (`actual`) rounds it to 0.85 or 0.5.
static float shiftBonusAt(const CarSim* s, float rpm, int g, int actual) {
	if (g < 0 || g > 8) return 0.0f;
	float d = s->up[g] - rpm;
	float range = kPerfectShiftMaxRpmDiff[s->carClass & 3][g] * s->shiftRangeMul;
	if (!(range > 0.0f)) return 0.0f;
	if (d < 0.0f) return fabsf(d) < range * (1.0f - 0.95f) ? 1.0f : -1.0f;
	float r = fmaxf(0.0f, 1.0f - d / (range * 0.95f));
	if (r > 0.0f && actual) return r >= 0.5f ? 0.85f : 0.5f;
	return r;
}

// DriveTrain::CheckPerfectShift (0x801B5438): in a drag, an upshift asked for
// at `rpm` scores the bonus the shift then pays out (dtAux, integrate).
static void checkPerfectShift(CarSim* s, float rpm) {
	if (!s->drag || s->dtGear > s->desired) return;
	s->shiftBonus = shiftBonusAt(s, rpm, s->dtGear, 1);
}

// DriveTrain::ComputeAuxiliaries: the clutch's four states.
static void dtAux(CarSim* s) {
	s->diffW = diffAngVel(s);
	float idleW = cf(s, CPI_EI + EI_IDLE) * RADS_PER_RPM;
	int neutral = s->dtGear == GEAR_N;
	int idling = s->engThr < 0.3f && s->diffW <= idleW + 0.0001f;
	switch (s->clutchState) {
	case 0:   // disengaging
		s->engMult = 0.0f;
		if (s->clutch <= 0.0f) {
			s->clutch = 0.0f;
			s->clutchState = 1;
		}
		break;
	case 1:   // disengaged: the gear changes here
		s->engMult = 0.0f;
		// RacingCarStats::Update (0x802267D0) counts each step up into a
		// gear, and a perfect one when Car::GetShiftBonus is over 0.5.
		if (s->desired > s->dtGear && s->desired != 0) {
			s->shifts++;
			if (s->shiftBonus > 0.5f) s->perfectShifts++;
		}
		s->dtGear = s->desired;
		if (s->dtShifting || !idling) {
			s->clutchState = 2;
			chooseShiftModifiers(s);
		}
		break;
	case 2:   // engaging
		s->engMult = 0.7f;
		if (s->clutch >= 1.0f) {
			s->clutch = 1.0f;
			if (fabsf(s->engW - s->diffW) <= 50.0f || neutral) s->clutchState = 3;
		}
		if (idling && !neutral) s->clutchState = 0;
		break;
	default:  // engaged
		if (s->shiftBonus > 0.0f && s->bonusAt == 0.0f) s->bonusAt = s->clock;
		s->engMult = 1.0f;
		if (!neutral && idling) s->clutchState = 0;
		if (s->dtGear == s->desired) {
			s->dtShifting = 0;
		} else {
			s->dtShifting = 1;
			s->clutchState = 0;
		}
		break;
	}
	// A scored shift's payout: from when it was scored, half a second of
	// extra drive that starts at twice the bonus and runs down to nothing.
	if (s->clutchState == 3 && s->bonusAt > 0.0f) {
		float t = s->clock - s->bonusAt;
		float boost = 2.0f * s->shiftBonus * (0.5f - t) / 0.5f;
		if (boost > 0.0f) s->engMult = boost + 1.0f;
		if (t >= 0.5f) s->shiftBonus = s->bonusAt = 0.0f;
	}
}

// DriveTrain::GetShiftUpRPM / GetShiftDownRPM.
// A drag lets the box run to the limiter, anything else to the redline.
static float shiftUp(const CarSim* s, int g, int f) {
	float v = s->up[g] + (f ? s->modLo[0] : (g == 3 ? s->mod80 : s->mod78));
	float cap = cf(s, CPI_EI + (s->drag ? EI_LIMITER : EI_REDLINE));
	return v > cap ? cap : v;
}

static float shiftDown(const CarSim* s, int g, int f) {
	return s->down[g] + (f ? s->modLo[1] : s->mod7c);
}

void carsim_shift_band(const CarSim* s, float* lo, float* hi) {
	const int g = s->dtGear < 0 ? 0 : (s->dtGear > 9 ? 9 : s->dtGear);
	if (s->mellow) {
		// The Mellovator's own tables, +0x19C up and +0x1C0 down.
		*lo = s->down[g];
		*hi = s->up[g];
	} else {
		*lo = shiftDown(s, g, 0);
		*hi = shiftUp(s, g, 0);
	}
}

// DriveTrain::SetDesiredGear: retail's automatic box, or an explicit gear.
static void setDesiredGear(CarSim* s, float rpm, int param) {
	int g = s->dtGear;
	if (param != -1) {
		if (param != g && param != s->desired) {
			s->desired = param;
			if (param > g && g >= GEAR_N) checkPerfectShift(s, rpm);
		}
		return;
	}
	if (g <= GEAR_N) {
		s->desired = GEAR_FIRST;
		if (s->clutchState == 3 && g > GEAR_R) checkPerfectShift(s, rpm);
		return;
	}
	if (s->clutchState != 3) return;
	float up = shiftUp(s, g, 0), down = shiftDown(s, g, 0), prevUp;
	if (g == GEAR_FIRST) {
		down = s->engThr >= 0.2f ? 0.0f : cf(s, CPI_EI + EI_IDLE) + 100.0f;
		prevUp = 0.0f;
	} else {
		prevUp = shiftUp(s, g - 1, 1) * dtRatio(s, g) / dtRatio(s, g - 1) - 200.0f;
	}
	if (rpm >= up && g < numGears(s) + 1) {
		int ok = 1;
		for (int i = 0; i < CARPHYS_WHEELS; i++)
			if (!s->wheel[i].onGround || s->wheel[i].s >= 4.0f) ok = 0;
		if (ok) s->desired = g + 1;
	} else if (rpm <= down && g >= GEAR_FIRST && rpm <= prevUp) {
		s->desired = g - 1;
	}
	if (g < s->desired) checkPerfectShift(s, rpm);
}

// CalculateShiftPoints (GearShiftMethod 0), then the DriveTrain ctor's scale
// on the launch point out of MiscCarTunings.
static void calcShiftPoints(CarSim* s) {
	const float* ei = cpiAt(s, CPI_EI);
	memset(s->up, 0, sizeof(s->up));
	memset(s->down, 0, sizeof(s->down));
	s->up[1] = cf(s, CPI_DTI + DTI_LAUNCH) * (ei[1] - ei[0]) + ei[0];
	int ng = numGears(s);
	int g = GEAR_FIRST;
	for (; g < ng + 1; g++) {
		float r0 = dtRatio(s, g), r1 = dtRatio(s, g + 1);
		s->up[g] = ei[1] - 100.0f;
		s->down[g + 1] = fabsf(r0) <= 1e-5f ? 0.0f : s->up[g] * r1 / r0;
	}
	s->up[g] = ei[2] + 500.0f;
	float split = cf(s, CPI_DTI + DTI_SPLIT);
	s->up[1] *= split < 0.2f ? 1.0f : (split > 0.8f ? 1.1f : 1.2f);
}

// ---------------------------------------------------------------------------
// Aerodynamics::ResolveForces. The object's +8 is SetBoost's catch-up boost
// (TASKS.md #255): the speed both forces are worked from is |v| / boost, so a
// boosted car feels the drag and the downforce of a slower one; +0xC is
// 1 - Car +0x4DC, which nothing sets. [10] and [11] are aeroScale and
// aeroSurf, which only the MellowMover moves off 1.

static void aeroResolve(CarSim* s) {
	const float* ai = cpiAt(s, CPI_AERO);
	float sp = len3(s->rbVel) / catchupOf(s);
	float k = sp * s->aeroSurf * ai[0];
	k = (k * (OFF_THROTTLE_DRAG - 1.0f) * (1.0f - s->aeroThr) + k) * s->aeroScale;
	float F[3] = { -s->rbVel[0] * k, -s->rbVel[1] * k, -s->rbVel[2] * k };
	float zero[3] = { 0.0f, 0.0f, 0.0f };
	rbApplyForce(s, F, zero);

	float up = s->rot[2][2] > 0.0f ? s->rot[2][2] : 0.0f;
	if (s->aeroDown >= 0.5f) up = 1.0f;
	float f = 0.0f;
	if (sp > 0.0001f) {
		f = dot3(s->rbVel, s->rot[0]) / sp;
		if (f < 0.0f) f = 0.0f;
	}
	float m = f * f > 0.4f ? f * f : 0.4f;
	float front, rear;
	if (s->aeroDown == 0.0f) {
		float D = sp * up * m * (ai[1] + ai[2]);
		front = D * 0.4f;
		rear = D * 0.41f;
	} else {
		float D = sp * up * m;
		front = D * ai[1];
		rear = D * ai[2];
	}
	float Ff[3] = { 0.0f, 0.0f, -front * s->aeroScale }, rf[3] = { 1.0f, 0.0f, 0.0f };
	float Fr[3] = { 0.0f, 0.0f, -rear * s->aeroScale }, rr[3] = { -1.0f, 0.0f, 0.0f };
	rbApplyForceInFrame(s, Ff, rf);
	rbApplyForceInFrame(s, Fr, rr);
}

// ---------------------------------------------------------------------------
// PhysicsMover

// PhysicsMover::IterateForces: tyre forces that agree with the acceleration
// they produce, five passes.
static void iterateForces(CarSim* s) {
	guessClutchTorque(s);
	for (int i = 0; i < CARPHYS_WHEELS; i++) wheelGuess(s, i);
	float v[3];
	toCar(s->rot, s->F, v);
	float izzInv = s->zdamp ? s->IinvZ[2] : s->Iinv[2];
	for (int it = 0; it < 5; it++) {
		float fx = v[0], fy = v[1], M = 0.0f;
		for (int i = 0; i < CARPHYS_WHEELS; i++) {
			const CarSimWheel* wh = &s->wheel[i];
			const float* att = s->susp[i].attach;
			float wx = wh->rowC[0][0] * wh->Flong + wh->rowC[1][0] * wh->Flat;
			float wy = wh->rowC[0][1] * wh->Flong + wh->rowC[1][1] * wh->Flat;
			fx += wx;
			fy += wy;
			M += att[0] * wy - att[1] * wx;
		}
		float ax = fx * s->invMass, ay = fy * s->invMass, al = M * izzInv;
		for (int i = 0; i < CARPHYS_WHEELS; i++) {
			CarSimWheel* wh = &s->wheel[i];
			const float* att = s->susp[i].attach;
			if (!wh->sliding && !wh->locked)
				wheelModify(wh, ax - al * att[1], ay + al * att[0]);
		}
		modifyClutchTorque(s);
	}
	for (int i = 0; i < CARPHYS_WHEELS; i++) wheelFinalize(s, i);
	finalizeClutchTorque(s);
}

// PhysicsMover::DriftStabilizer (0x801C22B0). With the car sliding the way it
// is rotating and more than one wheel down (Car+0x434), a moment about the
// car's up axis (Car+0xA8, its matrix's third row) against the yaw rate,
// strongest near 89 degrees of slide; and always, the rear tyres' Wheel+0x184
// set from the slide and the yaw rate, for the next step's lateral cap.
static void driftStabilizer(CarSim* s) {
	const float split = cf(s, CPI_DTI + DTI_SPLIT);
	const DriftMisc* m = &kDriftMisc[split < 0.2f ? 0 : (split > 0.8f ? 2 : 1)];
	const float yaw = carYawAlways(s);
	const float rate = s->w[2];
	const int skill = s->driftSkill < 0 ? 0 : (s->driftSkill > 2 ? 2 : s->driftSkill);
	if (yaw * rate > 0.0f && s->wheelsDown > 1) {
		const float g = graphGet(s->aiDriven ? kDriftStabAI : kDriftStabPlayer, 7, fabsf(yaw));
		const float k = rate * m->stab[skill] * g;
		for (int i = 0; i < 3; i++) s->T[i] += s->rot[2][i] * k;   // ApplyMoment
	}
	const float rear = tableGet(kDriftRearFriction, 10, 0.0f, 1.0f,
		(1.0f - m->mix) * fabsf(yaw) + m->mix * fabsf(rate)) * m->rearMul;
	s->wheel[2].grip3 = rear;
	s->wheel[3].grip3 = rear;
}

// PhysicsMover::ResolveHydraulicForces (0x801C1748, TASKS.md #313). Each
// corner's extension walks toward 0.2 m times the driver's extend at 0.5 m/s
// (HydraulicExtendMaxLength / HydraulicExtendSpeed); a bounce first lifts the
// pair across from it to 0.2 m (HydraulicBounceMinExtend) as anchors, then,
// on every substep in which no bounced corner's spring is compressed past
// 1 cm (HydraulicBounceTrigger), kicks each bounced corner moving off the
// ground by up to 0.9 m/s (HydraulicBounceVel) less 0.3 of that speed
// (HydraulicBounceImpulseCoeff) along its ground normal. Car +0x3C's +0x16,
// the body touching something last frame (`contacts`), turns it off.
static void resolveHydraulics(CarSim* s) {
	if (s->contacts >= 1)
		return;
	const float* b = s->hydBounce;
	int mask;
	if (b[1] != 0.0f && b[0] != 0.0f)
		mask = 0xC;          // the front bounces off the rear pair
	else if (b[2] != 0.0f && b[3] != 0.0f)
		mask = 0x3;          // the rear off the front pair
	else if ((b[2] != 0.0f && b[1] != 0.0f) || (b[3] != 0.0f && b[0] != 0.0f))
		mask = 0x20;         // a side: no anchors
	else
		mask = 0;
	bool ok = mask != 0;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		float tgt = 0.2f * s->hydExtend[i];
		const bool anchor = (mask & (1 << i)) != 0;
		if (anchor && tgt < 0.2f)
			tgt = 0.2f;
		float ext = s->hydExt[i];
		if (tgt > ext) {
			ext += 0.5f * s->ugly;
			if (ext > tgt) ext = tgt;
			s->hydState[i] = 2;
		} else if (tgt < ext) {
			ext -= 0.5f * s->ugly;
			if (ext < 0.0f) ext = 0.0f;
			s->hydState[i] = 3;
		} else {
			s->hydState[i] = tgt > 0.0f ? 1 : 0;
		}
		s->hydExt[i] = ext;
		if (anchor && ext < 0.2f)
			ok = false;
	}
	if (!ok)
		return;
	for (int i = 0; i < CARPHYS_WHEELS; i++)
		if (b[i] > 0.0f && s->susp[i].x < -0.01f)
			return;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		if (b[i] <= 0.0f)
			continue;
		const CarSimWheel* wh = &s->wheel[i];
		float Pp[3], wxp[3], v[3];
		toWorld(s->rot, s->susp[i].wcpos, Pp);
		cross3(s->w, Pp, wxp);
		for (int k = 0; k < 3; k++) v[k] = s->rbVel[k] + wxp[k];
		const float vn = dot3(v, wh->n);
		if (vn < 0.0f)
			continue;
		const float imp = b[i] * (0.9f - 0.3f * vn);
		if (imp <= 0.0f)
			continue;
		float J[3];
		for (int k = 0; k < 3; k++) J[k] = wh->n[k] * imp * s->mass;
		rbApplyImpulse(s, J, Pp, 1.0f);
		s->hydFired[i] = 1;
	}
}

bool carsim_hyd_bouncing(const CarSim* s) {
	for (int i = 0; i < CARPHYS_WHEELS; i++)
		if (s->hydBounce[i] > 0.0f) return true;
	return false;
}

bool carsim_hyd_extended(const CarSim* s) {
	for (int i = 0; i < CARPHYS_WHEELS; i++)
		if (s->hydExt[i] > 0.0f || s->hydExtend[i] > 0.0f) return true;
	return false;
}

void carsim_set_hydraulics(CarSim* s, const float bounce[CARPHYS_WHEELS],
	const float extend[CARPHYS_WHEELS])
{
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		s->hydBounce[i] = bounce ? bounce[i] : 0.0f;
		s->hydExtend[i] = extend ? extend[i] : 0.0f;
	}
}

// PhysicsMover::ResolveForces. PORT: JumpStabilizer and MaybeForceYaw, which
// retail runs between the wheel forces and the drift's stabilizer, are not
// taken (see the header).
static void resolveForces(CarSim* s) {
	aeroResolve(s);
	for (int i = 0; i < CARPHYS_WHEELS; i++)
		wheelExchangeZ(&s->wheel[i], s->susp[i].F, s->susp[i].onGround);
	iterateForces(s);
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		if (!s->susp[i].onGround) continue;
		const CarSimWheel* wh = &s->wheel[i];
		float r[3] = { wh->gpt[0] - s->rbPos[0], wh->gpt[1] - s->rbPos[1], wh->gpt[2] - s->rbPos[2] };
		rbApplyForce(s, wh->F, r);
	}
	// TheRaceParameters +0x44 and not +0x40: a drift that is not a drag.
	if (s->drift && !s->drag) driftStabilizer(s);
	resolveHydraulics(s);
}

// PhysicsSystem::ComputeAuxiliaries, in the system's component order.
static void systemAux(CarSim* s) {
	rbAux(s);
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		wheelAux(&s->wheel[i]);
		suspAux(s, i);
	}
	engineAux(s);
	dtAux(s);
}

// Car::AreDriveWheelsOffGround.
static int driveWheelsOffGround(const CarSim* s) {
	float split = cf(s, CPI_DTI + DTI_SPLIT);
	if (split >= 0.001f && (s->wheel[2].onGround || s->wheel[3].onGround)) return 0;
	if (split <= 0.999f && (s->wheel[0].onGround || s->wheel[1].onGround)) return 0;
	return 1;
}

// CheckDampingSuitability: 0 leave it be, 1 bleed it off, 2 hold it still.
static int checkDampingSuitability(const CarSim* s, int prev) {
	// 0x801C2EDC's first test (#313): a bouncing car, or a corner rising or
	// lowering, is left be.
	if (!s->mellow) {
		if (carsim_hyd_bouncing(s)) return 0;
		for (int i = 0; i < CARPHYS_WHEELS; i++)
			if (s->hydState[i] == 2 || s->hydState[i] == 3) return 0;
	}
	if (s->thr > 0.3f && s->dtGear != GEAR_N && s->wheelsDown > 0) return 0;   // all 4 attached
	float vm = len3(s->rbVel), wm = len3(s->w);
	if (vm > 0.7f || wm > 0.7f) return 0;
	if ((fabsf(s->rot[0][2]) > 0.15f || fabsf(s->rot[1][2]) > 0.15f) && s->wheelsDown > 0) return 0;
	bool touching = s->wheelsDown > 0 || s->touching;
	if (!touching && prev != 2) return 0;
	if (vm > 0.25f || wm > 0.25f) return 1;
	return 2;
}

// MovementDousingAndRoofReset: a car that has all but stopped is stopped, and
// one stuck on its roof (or beached on its driven wheels) for a second asks to
// be put back. `mover` is the PhysicsMover argument: the MellowMover passes
// none, so its wheels are left alone and it never asks for the reset (the
// timer still runs). Gravity is WildCollisionGravity in dampening mode 1.
static int dousing(CarSim* s, int mover) {
	int st = checkDampingSuitability(s, s->dousing);
	s->dousing = st;
	float vm = len3(s->rbVel);
	float g = s->dampenMode == 1 ? WILD_COLLISION_GRAVITY : GRAVITY;
	if (st == 1) {
		for (int i = 0; i < 3; i++) {
			s->P[i] *= 0.95f;
			s->L[i] *= 0.95f;
		}
		if (mover)
			for (int i = 0; i < CARPHYS_WHEELS; i++) s->wheel[i].w *= 0.95f;
		if (vm > 0.8f) s->roofTimer = 0.0f;
		s->g = g;
		return 1;
	}
	if (st == 0) {
		if (vm > 0.8f) s->roofTimer = 0.0f;
		s->g = g;
		return 0;
	}
	for (int i = 0; i < 3; i++) s->P[i] = s->L[i] = 0.0f;
	if (mover && s->wheelsDown > 0)
		for (int i = 0; i < CARPHYS_WHEELS; i++) s->wheel[i].w = 0.0f;
	if (s->rot[2][2] < 0.1f || driveWheelsOffGround(s)) {
		if (s->roofTimer == 0.0f) s->roofTimer = s->clock;
		else if (mover && s->clock - s->roofTimer > 1.0f) s->resetRequest = true;
	} else {
		s->roofTimer = 0.0f;
	}
	s->g = 0.0f;
	return 1;
}

// PhysicsMover::BottomOut (0x801C0240), which keeps each wheel's penalty in
// CarState +0x328 for the sound.
static void bottomOut(CarSim* s) {
	int r = dousing(s, 1);
	if (r == 0) {
		for (int i = 0; i < CARPHYS_WHEELS; i++) {
			if (s->susp[i].x == s->susp[i].info[5]) {
				s->bottomPen[i] += groundPenalty(s, i);
				r = 1;
			}
		}
	}
	if (r) {
		s->ugly = 0.0f;
		systemAux(s);
	}
}

// PhysicsMover::ComputeAuxiliaries.
static void computeAux(CarSim* s) {
	systemAux(s);
	bottomOut(s);
	resolveForces(s);
}

// PhysicsSystem::IntegrateEuler: every derivative from the same state, then
// the step, then the auxiliaries.
static void integrate(CarSim* s, float h) {
	Deriv d;
	rbDeriv(s, &d);
	for (int i = 0; i < CARPHYS_WHEELS; i++) wheelDeriv(s, &s->wheel[i], &d.angle[i], &d.w[i]);
	d.engW = (engineTotal(s) + s->engExt) / cf(s, CPI_EI + EI_INERTIA);
	// A scored shift moves the clutch faster by its bonus; an over-revved one
	// past second slows it to 0.35.
	float cm = s->shiftBonus > 0.0f ? s->shiftBonus + 1.0f :
		(s->shiftBonus < 0.0f && s->desired > GEAR_FIRST ? 0.35f : 1.0f);
	d.clutch = cf(s, CPI_DTI + DTI_CLUTCH_SPEED) * s->clutchMul * cm *
		kClutchSpeed[s->clutchState & 3];

	for (int i = 0; i < 3; i++) {
		s->rbPos[i] += h * d.pos[i];
		s->P[i] += h * d.P[i];
		s->L[i] += h * d.L[i];
		for (int r = 0; r < 3; r++) s->rot[r][i] += h * d.rot[r][i];
	}
	s->damping += h * d.damping;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		s->wheel[i].angle += h * d.angle[i];
		s->wheel[i].w += h * d.w[i];
	}
	s->engW += h * d.engW;
	s->clutch += h * d.clutch;

	computeAux(s);
}

// PhysicsMover::Calibrate: the static load each spring carries. Mellovator::
// Calibrate is the same, from the same GetDownforceAtSpeed (scaled by [10]).
static void calibrate(CarSim* s) {
	const float* ai = cpiAt(s, CPI_AERO);
	float a = s->wheel[0].pos[0] - s->cg[0];
	float b = s->wheel[3].pos[0] - s->cg[0];
	float rear = a / (a - b) * 0.5f;
	float front = 0.5f - rear;
	float v = s->vfwd > 0.0f ? s->vfwd : 0.0f;
	float load = s->mass * GRAVITY + ai[3] * (s->aeroScale * v * (ai[1] + ai[2]));
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimSusp* su = &s->susp[i];
		su->preload = load * (i < 2 ? front : rear);
		for (int k = 0; k < 3; k++) su->attach[k] = s->wheel[i].pos[k] - s->cg[k];
		// Suspension::Calibrate (0x801D0AE0): the mount's z less CarState
		// +0x3BC, so an extended corner stands the body up by it (#313).
		su->attach[2] -= s->hydExt[i];
	}
}

// YawFrictionBoost. In any control mode but 0, p5 is scaled by the post-
// collision yaw boost, which the port does not model (0).
static float yawFrictionBoost(float yaw, float eb, float speed, float p4, float p5,
	int mode, float fz)
{
	float base = fabsf(fz) + 1.0f;
	if (mode != 0) p5 = 0.0f;   // p5 * GetPostCollisionYawControlBoost
	if (eb > YAW_EBRAKE_THRESHOLD && fabsf(yaw) < YAW_ANGLE_THRESHOLD * 0.0174533f) return base;
	float bonus = fabsf(yaw) * (p4 + p5) * (speed / HIGH_SPEED_SPEED);
	if (bonus > MAX_YAW_BONUS) bonus = MAX_YAW_BONUS;
	return base + bonus;
}

// RigidBody::SetZRotationDamping. Turning it off divides the yaw momentum by
// the Izz scale, so the yaw rate carries over; turning it on keeps the
// momentum, so the rate drops by the scale -- that is the damping.
static void setZRotationDamping(CarSim* s, int on) {
	if (s->zdamp == on) return;
	if (!on) {
		float zs = cf(s, CPI_IZZ_SCALE);
		s->L[2] /= zs > 0.0f ? zs : 1.0f;
	}
	s->zdamp = on;
}

// PhysicsMover::TuneBoostAndDampers.
static void tuneBoostAndDampers(CarSim* s, const CarSimInput* in) {
	s->flag13c = 1;
	s->flag140 = 1;
	s->g = GRAVITY;
	// On unless no wheel touches the ground: Car+0x434 is CarState+0x3f4, the
	// count SetWheelTouchingGround keeps (not the control mode, CarState+0x434).
	setZRotationDamping(s, s->wheelsDown != 0);
	if (in->racing) {
		carsim_set_heat(s, s->drag);
		if (s->thr > 0.3f || (s->wheelsDown == 4 && fabsf(s->rot[0][2]) > 0.15f))
			s->flag13c = 0;
	}
	// SetBoost(Car +0x478): RigidBody +0x1F0 and each tyre's +0x180.
	s->govBoost = catchupOf(s);
	for (int i = 0; i < CARPHYS_WHEELS; i++) s->wheel[i].grip2 = s->govBoost;

	float c;
	if (s->controlMode == 0) {
		c = cf(s, CPI_YAW_CLAMP_ON);
	} else {
		c = cf(s, CPI_YAW_CLAMP_OFF);
		if (c == 0.0f) {
			float yaw = carYaw(s);
			if (yaw * s->L[2] < 0.0f && fabsf(yaw) > 0.1f) c = 20.0f;
		}
	}
	s->yawClamp = c;
	s->governor = cf(s, CPI_GOVERNOR);

	float split = cf(s, CPI_DTI + DTI_SPLIT);
	if (split > 0.8f) {
		// PORT: CollisionBody+0x11c is read as "no wheel on the ground".
		if (s->speedAbs * MPS_TO_MPH >= 60.0f || s->wheelsDown < 1) s->split = split;
		else s->split = 0.65f;
	}
}

// PhysicsMover::TuneWheelParams. Control mode 0 takes NFSAdjust off every
// tyre's grip (outside a drag or a drift) and adds 0.5 to any e-brake over
// 0.2; the other modes only near a spin below 80 mph. A drag or a drift also
// leaves the rear tyres without the yaw friction boost.
static void tuneWheelParams(CarSim* s, float dt) {
	float mph = s->vfwd * MPS_TO_MPH;
	const bool special = driftPhysics(s);
	float grip = (s->controlMode == 0 && !special) ? NFS_ADJUST : 1.0f;
	if (s->ebrakeIn < 0.5f) {
		s->ebrakeRecovery += dt;
		if (s->ebrakeRecovery > 1.0f) s->ebrakeRecovery = 1.0f;
	} else {
		s->ebrakeRecovery -= dt * 20.0f;
		if (s->ebrakeRecovery < 0.5f) s->ebrakeRecovery = 0.5f;
	}
	float bias = cf(s, CPI_CTL + 0xC);
	float yaw = carYaw(s);
	const int nos = carsim_nos_on(s);
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* wh = &s->wheel[i];
		if (s->thr <= 0.8f || s->brakeIn <= 0.5f || mph >= 10.0f || i < 2)
			wh->brake = s->brakeIn * (i < 2 ? 2.0f * (1.0f - bias) : 2.0f * bias);
		else
			wh->brake = mph / 20.0f;
		float eb = s->ebrakeIn * kEBrakeSettings[i];
		if (eb > 0.2f && (s->controlMode == 0 || (fabsf(yaw) > 0.3f && mph < 80.0f)))
			eb += 0.5f;
		wh->ebrake = eb;
		if (i < 2 || special) {
			wh->grip1 = grip;
		} else {
			float yfb = yawFrictionBoost(yaw, eb, s->speedAbs, cf(s, CPI_YFB_P4),
				cf(s, CPI_YFB_P5), s->controlMode, s->rot[0][2]);
			wh->grip1 = grip * (s->ebrakeRecovery * (yfb - 1.0f) + 1.0f);
		}
		// Wheel+0x17C, the yaw boost's factor, takes the nitrous's
		// (0x801C12C4).
		if (nos > 0)
			wh->grip1 *= kNosFrictionMul[nos - 1];
	}
}

// RealDriver::SetLowGear (0x80025748): first for a manual box, the
// automatic's choice otherwise, and in a drag the launch timer.
static void setLowGear(CarSim* s) {
	if (s->transmission != 1) {
		s->driverGear = -1;
		return;
	}
	s->driverGear = GEAR_FIRST;
	if (s->drag) s->launchTicks = 1000;
}

// RealDriver::Drive (0x800257A8) and its DoThrottleBrakeReverse (0x80026780):
// the brake is a brake while moving and reverse from a stop. The e-brake
// timer and the burnout map are not ported.
static void driverInput(CarSim* s, const CarSimInput* in, float dt) {
	// PsychoDriver: the pedals as Drive set them, and the gear it asked for.
	if (in->aiDriver) {
		s->thr = clampf(in->throttle, 0.0f, 1.0f);
		s->brakeIn = clampf(in->brake, 0.0f, 1.0f);
		s->ebrakeIn = in->handbrake ? 1.0f : 0.0f;
		s->driverGear = in->gear;
		s->reverseFlag = 0;
		s->drvThr = s->thr;
		return;
	}
	// JoyHandler_ShiftUp / ShiftDown (0x80026D9C / 0x80026ECC) with a manual
	// box: one gear up as far as the top, one down as far as neutral, and
	// neither while in reverse.
	if (s->transmission == 1 && s->driverGear != GEAR_R) {
		if (in->shiftUp && s->driverGear < numGears(s) + 1) s->driverGear++;
		if (in->shiftDown && s->driverGear > GEAR_N) s->driverGear--;
	}
	float rawT = clampf(in->throttle, 0.0f, 1.0f);
	float rawB = clampf(in->brake, 0.0f, 1.0f);
	float v = s->vfwd;
	float fwdThr = rawT, brake, rev;
	if (!(rawB <= 0.6f || rawT >= 0.5f)) s->reverseFlag = 1;
	else if (v > -2.0f) s->reverseFlag = 0;
	if (v > 0.5f || !s->reverseFlag) {
		brake = rawB;
		rev = 0.0f;
	} else if (v < -2.0f) {
		rev = rawB;
		brake = rawT;
		fwdThr = 0.0f;
	} else {
		rev = rawB;
		brake = 0.0f;
	}
	float revThr = 2.0f * rev > 1.0f ? 1.0f : 2.0f * rev;
	// A drag's launch: Timer::CountDown at 4000 ticks a second, and while the
	// timer SetLowGear started runs, the pedal is doubled into 0.7..1.
	if (s->drag && s->launchTicks > 0) {
		s->launchTicks -= (int)(dt * 4000.0f);
		if (s->launchTicks < 0) s->launchTicks = 0;
		if (s->launchTicks > 0) fwdThr = clampf(2.0f * fwdThr, 0.7f, 1.0f);
	}
	if (fwdThr + 0.2f < revThr) {
		s->thr = revThr;
		s->driverGear = GEAR_R;
	} else {
		// In a drag, with the car in neutral (Car+0x444), the throttle follows
		// the pedal at MaxThrottleSpeed, 1.0 a second up and 1.5 down from the
		// last step's; with the pedal past 0.6 and the revs under idle + 1000
		// (Car+0x450; TestRPM is left at its 100) it is at least 0.6.
		if (s->drag && s->dtGear == GEAR_N) {
			fwdThr = clampf(fwdThr, s->drvThr - 1.5f * dt, s->drvThr + 1.0f * dt);
			if (rawT > 0.6f && s->rpmSmooth < cf(s, CPI_EI + EI_IDLE) + 1000.0f && fwdThr < 0.6f)
				fwdThr = 0.6f;
		}
		s->thr = clampf(fwdThr, 0.0f, 1.0f);
		if (s->driverGear == GEAR_R) setLowGear(s);
	}
	s->brakeIn = brake;
	s->ebrakeIn = in->handbrake ? 1.0f : 0.0f;
	s->drvThr = s->thr;
	// Drive: on the start line the driver holds neutral and the brake, and
	// the throttle stays the pedal's.
	if (s->startLine) {
		s->driverGear = GEAR_N;
		s->brakeIn = 1.0f;
	}
}

// AverageWindow::Record.
static void windowRecord(CarSimWindow* a, float v, float t) {
	if (a->value[a->idx] == 0.0f && a->time[a->idx] == 0.0f) a->count++;
	else a->sum -= a->value[a->idx];
	a->sum += v;
	a->value[a->idx] = v;
	a->time[a->idx] = t;
	while (t - a->time[a->oldest] > a->length) {
		if (a->time[a->oldest] > 0.0f) {
			a->sum -= a->value[a->oldest];
			a->value[a->oldest] = 0.0f;
			a->time[a->oldest] = 0.0f;
			a->count--;
		}
		if (++a->oldest >= a->cap) a->oldest = 0;
	}
	a->avg = a->count > 0 ? a->sum / (float)a->count : 0.0f;
	if (++a->idx >= a->cap) a->idx = 0;
}

static void windowInit(CarSimWindow* a, int cap, float length) {
	memset(a, 0, sizeof(*a));
	a->cap = cap;
	a->length = length;
}

// PlayerSteering::CalcMaxSteering: the lock on offer, in degrees.
static float steerCalcMax(CarSim* s, float in) {
	float pedal = ((s->thr + 1.0f) - (s->brakeIn + s->ebrakeIn) * 0.5f) * 0.5f;
	float v = s->vfwd;
	float avgA = fabsf(s->winA.avg);
	float range = tableGet(kSteerRange, 10, 0.0f, 160.0f, v) *
		((1.0f - pedal) * tableGet(kSteerSpeed, 10, 0.0f, 160.0f, v) + 1.0f) *
		tableGet(kRangeCoeff, 6, 0.0f, 1.0f, avgA);
	// For a second after a hit the lock closes in, the more so the faster the
	// car is going against 0.7 of its theoretical top speed (0x803178B4).
	// Skipped in a drift (TheRaceParameters +0x44); the timer is in WorldTimer
	// ticks there, times 0.00025 (0x803178B0), seconds here.
	if (!s->drift) {
		const float t = s->clock - s->yawHitAt;
		if (t < 1.0f) {
			float ratio = s->theoTop > 0.0f ? v / (s->theoTop * 0.7f) : 1.0f;
			if (1.0f - ratio < 0.0f) ratio = 1.0f;
			const float g = graphGet(kPostCollisionSteer, 4, t);
			range *= (1.0f - g) * (1.0f - ratio) + g;
		}
	}
	float yaw = carYaw(s);
	float yawDeg = fabsf(yaw) * 57.2958f;
	int agree = yaw * in >= 0.0f;
	if (s->slideState == 0) {
		if (yawDeg > 15.0f && s->speedAbs >= 13.4108f) s->slideState = 1;
	} else if (yawDeg < 5.0f) {
		s->slideState = 0;
	}
	if (agree || (s->slideState == 0 && yawDeg <= range)) {
		if (avgA >= 0.5f && range < s->steerLastMax) range = s->steerLastMax;
	} else {
		// Counter-steering a slide: as much lock as the slide has angle.
		float c = tableGet(driftPhysics(s) ? kDriftYawCoeff : kYawCoeff, 5, 0.0f, 1.0f, fabsf(in));
		range = yawDeg * c < c * 45.0f ? yawDeg * c : c * 45.0f;
	}
	if (range > 45.0f) range = 45.0f;
	s->steerLastMax = range;
	return range;
}

// PlayerSteering::CalcSteeringSpeed: how fast the wheel turns, degrees/s.
// The base is 110 in a drag or a drift (0x803178CC; SPEED2.EXE 0x4060E0 reads
// 110 at 0x783AE8 under the same test) and 180 otherwise (0x803178D0) -- the
// drift's wheel turns at 0.61 of the street rate (#462). The test's other arm,
// the burnout (TheRaceParameters +0x54), has no race here.
static float steerCalcSpeed(CarSim* s, float in, float dt) {
	float rate = dt > 0.0f ? fabsf((in - s->steerLastIn) / dt) : 0.0f;
	windowRecord(&s->winB, tableGet(kInputSpeedCoeff, 6, 0.0f, 10.0f, rate), s->clock);
	return (driftPhysics(s) ? 110.0f : 180.0f) * tableGet(kSteerSpeed, 10, 0.0f, 160.0f, s->vfwd) * s->winB.avg *
		tableGet(kInputCoeff, 6, 0.0f, 1.0f, fabsf(s->winA.avg));
}

// PlayerSteering::DoUndergroundSteering. In a drag or a drift the input
// takes the drift's curve, and the lock and its rate grow with the yaw
// acceleration while the wheel is turned the way the car is rotating
// (DriftControlTable).
static void steering(CarSim* s, float raw, float dt) {
	const bool special = driftPhysics(s);
	float in = tableGet(special ? kPsychoDrift : kPsycho, 21, -1.0f, 1.0f, clampf(raw, -1.0f, 1.0f));
	float cur = (float)s->steerU16 * 0.00549316f;
	if (cur >= 180.0f) cur -= 360.0f;
	float scale = cf(s, CPI_CTL);
	if (special && s->yawAccel * in > 0.0f)
		scale *= graphGet(kDriftControl, 6, fabsf(s->yawAccel));
	float mx = steerCalcMax(s, in);
	float sp = steerCalcSpeed(s, in, dt);
	float target = clampf(in * mx * scale, -45.0f, 45.0f);
	float rate = sp * scale * dt;
	if (target > cur + rate) target = cur + rate;
	else if (target < cur - rate) target = cur - rate;
	s->steerU16 = (u16)((int)(target * 65536.0f) / 360);
	windowRecord(&s->winA, in, s->clock);
	s->steerLastIn = in;
}

// TwoWheelAckermanSteering::SetSteering: the inside wheel turns further.
static void ackerman(CarSim* s) {
	float th = (float)s->steerU16 * (6.2831853f / 65536.0f);
	if (th > 3.14159265f) th -= 6.2831853f;
	float L = s->wheel[0].pos[0] - s->wheel[3].pos[0];
	float T = s->wheel[0].pos[1] - s->wheel[1].pos[1];
	float a = fabsf(th);
	float a2 = L * a / (T * a + L);
	if (th > 0.0f) {
		s->steerFL = a;
		s->steerFR = a2;
	} else {
		s->steerFL = -a2;
		s->steerFR = -a;
	}
}

// PhysicsMover::DelegateDriverInput.
static void delegateDriverInput(CarSim* s) {
	if (s->dtGear != GEAR_N) {
		if (s->thr <= s->brakeIn || s->thr <= s->ebrakeIn) {
			if (s->thr < 0.5f) s->thr = 0.0f;
		} else {
			if (s->brakeIn < 0.5f) s->brakeIn = 0.0f;
			if (s->ebrakeIn < 0.5f) s->ebrakeIn = 0.0f;
		}
	}
	float t = s->thr <= 0.15f ? 0.0f : s->thr;
	float e = t;
	if (t > 0.0f && s->dtGear > GEAR_N && s->engW * RPM_PER_RADS < 3000.0f)
		e = t > 0.5f ? t : 0.5f;
	ackerman(s);
	s->engThr = e;
	s->nosLevel = carsim_nos_on(s);   // Engine::SetNitroFlag (0x801C0DC0)
	setDesiredGear(s, s->rpmSmooth, s->driverGear);
	s->aeroThr = e;
	s->aeroDown = (float)s->wheelsDown * 0.25f;
}

// The wheels as drawn (CarState +0x3DE) turn toward the steering at 120
// degrees a second: PhysicsMover::UpdateCar (0x801C1AC0) and
// MellowMover::UpdateCar alike, out of the car's own steering (+0x1C4).
static void steerVis(CarSim* s, float dt) {
	float target = (float)s->steerU16 * (360.0f / 65536.0f);
	if (target >= 180.0f) target -= 360.0f;
	float cur = s->mlSteerVis;
	s->mlSteerVis = clampf(target, cur - dt * MELLOW_STEER_RATE, cur + dt * MELLOW_STEER_RATE);
}

// PhysicsMover::UpdateCar: what the rest of the game reads.
static void updateCar(CarSim* s, float dt) {
	steerVis(s, dt);
	s->vfwd = dot3(s->rbVel, s->rot[0]);
	s->speedAbs = len3(s->rbVel);
	float rpmNew = s->engW * RPM_PER_RADS;
	float decel = -((s->dtShifting && s->desired > GEAR_FIRST) ? 15.0f : 2.5f) /
		cf(s, CPI_EI + EI_INERTIA);
	if (dt > 0.0f && (rpmNew - s->rpmSmooth) / dt < decel) {
		float lim = s->rpmSmooth + decel * dt;
		if (lim > rpmNew) rpmNew = lim;
	}
	s->rpmSmooth = 0.55f * rpmNew + 0.45f * s->rpmSmooth;
	int n = 0;
	for (int i = 0; i < CARPHYS_WHEELS; i++) n += s->wheel[i].onGround ? 1 : 0;
	s->wheelsDown = n;
}

// ---------------------------------------------------------------------------
// MellowMover (TASKS.md #280)
//
// Car::SetMovementMode's mode 3, which GetDefaultMovementMode (0x8021D7C8)
// hands every AI racer and RaceCarController::Think (0x80027EDC) switches it
// to once its physics timer runs out. It keeps the PhysicsMover's rigid body
// but replaces the rest with the Mellovator: four sprung wheels whose tyres
// push back in proportion to their slip speed, up to the wheel's load times a
// friction cap (mlMu, 4 on a street course), and an engine that is only a rev
// figure read off the driven wheels. That cap is what lets retail's AI hold
// its racing line: the PhysicsMover's tyres give a racer about a third of it.
//
// The Mellovator's fields, against the CarSim ones they are kept in:
//   +0x20 w          wheel[i].w           +0x90 slip       wheel[i].s / .slip
//   +0x30 x          susp[i].x            +0xB0 sliding    wheel[i].sliding
//   +0x40 xdot       susp[i].xdot         +0xC0 locked     wheel[i].locked
//   +0x50 F (load)   susp[i].F            +0xD0 pen        mlPen[i]
//   +0x60 onGround   susp[i].onGround     +0xE0 normal     wheel[i].n
//   +0x70 Fc         mlFc[i]              +0x120 rpm       mlRpm
//   +0x124 gear      dtGear               +0x128 throttle  thr
//   +0x12C brake     mlBrake              +0x130 steer     steerU16
//   +0x134 gear asked for  driverGear     +0x144 rating    mlRating
//   +0x148 preload   susp[i].preload      +0x19C/0x1C0 shift points up/down
//
// Not taken: the frozen flag (+0x138, never set on a racing AI car), NOS
// (+0x198, an AI car has none), the hydraulic and ride-height offsets (+0x1E8,
// +0x1F8: 0 without hydraulics), the rain term on the friction boost, and the
// catch-up boost (+0x13C and +0x140 are SetBoost's argument, which is 1).

// MellowForceFeedbackGraph and MellowJumpStabilization, (x, y) pairs.
static const float kMellowFeedback[3][2] = { { 0.0f, 0.05f }, { 2.0f, 0.322f }, { 20.0f, 0.5f } };
static const float kMellowJumpStab[3][2] = { { 0.0f, 0.0f }, { 0.1f, 0.0f }, { 2.0f, 20.0f } };

// Graph::GetValue: straight lines between the points, held at both ends.
static float graphGet(const float (*p)[2], int n, float x) {
	if (x <= p[0][0]) return p[0][1];
	for (int i = 1; i < n; i++)
		if (x < p[i][0])
			return p[i - 1][1] + (p[i][1] - p[i - 1][1]) * (x - p[i - 1][0]) / (p[i][0] - p[i - 1][0]);
	return p[n - 1][1];
}

// Mellovator::ComputeSuspensionForces, through GetGroundHeightAndWheelPos:
// each wheel's spring measured from the record's wheel centre to the surface
// under it, along the surface normal over the car's up. Unlike the
// PhysicsMover's, a wheel past its bump stop is held there and the overshoot
// kept for BottomOut.
static void mlSuspension(CarSim* s) {
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* wh = &s->wheel[i];
		CarSimSusp* su = &s->susp[i];
		const float* info = su->info;
		float rc[3] = { wh->pos[0] - s->cg[0], wh->pos[1] - s->cg[1], wh->pos[2] - s->cg[2] };
		float P[3];
		toWorld(s->rot, rc, P);
		for (int k = 0; k < 3; k++) {
			P[k] += s->rbPos[k];
			wh->wpos[k] = P[k];
		}
		float gz = groundElev(s, i, P[0], P[1], wh->n);
		float nu = dot3(wh->n, s->rot[2]);
		float x = (nu >= 0.001f ? (gz - P[2]) / nu : -100.0f) + wh->radius;

		float xold = su->x;
		s->mlPen[i] = 0.0f;
		if (x < info[6]) {
			su->x = info[6];
			su->onGround = 0;
		} else if (x > info[5]) {
			su->x = info[5];
			su->onGround = 1;
			s->mlPen[i] = x - info[5];
		} else {
			su->x = x;
			su->onGround = 1;
		}
		su->xdot = (su->x - xold) / (s->ugly > 0.0001f ? s->ugly : 0.0001f);

		float F = 0.0f;
		if (su->onGround) {
			float xp = su->x > 0.0f ? su->x : 0.0f;
			F = su->xdot * info[su->xdot > 0.0f ? 2 : 3] * MELLOW_DAMPER_K +
				su->x * info[1] * MELLOW_DAMPER_K * (xp * info[0] + 1.0f) + su->preload;
			if (F < 0.0f) F = 0.0f;
		}
		su->F = F;
	}
}

// Mellovator::ComputeAuxiliaries: the brakes lock and unlock, then the revs
// and the automatic box from the driven wheels' speed.
static void mlAux(CarSim* s) {
	mlSuspension(s);
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* wh = &s->wheel[i];
		if (s->mlBrake > MELLOW_LOCK_BRAKE && fabsf(wh->w) < MELLOW_LOCK_W) {
			wh->locked = 1;
			wh->w = 0.0f;
		}
		if (s->mlBrake < MELLOW_UNLOCK_BRAKE) wh->locked = 0;
	}
	const CarSimWheel* W = s->wheel;
	float split = cf(s, CPI_DTI + DTI_SPLIT);
	float fr = cf(s, CPI_DTI + DTI_FRONT_FINAL) * (W[0].w + W[1].w) * 0.5f * RPM_PER_RADS;
	float rr = cf(s, CPI_DTI + DTI_REAR_FINAL) * (W[2].w + W[3].w) * 0.5f * RPM_PER_RADS;
	float base = split < 0.2f ? fr : (split > 0.8f ? rr : (fr + rr) * 0.5f);
	float rpm = base * dtRatio(s, s->dtGear);
	if (s->driverGear == -1) {
		if (s->dtGear < GEAR_FIRST) s->dtGear = GEAR_FIRST;
		while (s->dtGear - 1 < numGears(s) && s->up[s->dtGear] < rpm)
			rpm = base * dtRatio(s, ++s->dtGear);
		while (s->dtGear - 1 > 1 && rpm < s->down[s->dtGear])
			rpm = base * dtRatio(s, --s->dtGear);
	} else if (s->driverGear == GEAR_R) {
		s->dtGear = GEAR_R;
		rpm = base * dtRatio(s, GEAR_R);
	}
	const float* ei = cpiAt(s, CPI_EI);
	s->mlRpm = clampf(rpm, ei[0], ei[2]);
}

// Mellovator::GetDerivative: each wheel's spin from its share of the engine's
// torque at the current revs, the brake, and the tyre's own force pushing
// back. The torque goes through the REAR final drive on the front wheels and
// the front one on the rears (+0x1B8 + 4 for i < 2): retail's own, kept.
static void mlDeriv(const CarSim* s, float* dw) {
	const float* ei = cpiAt(s, CPI_EI);
	const float boost = catchupOf(s);   // +0x13C, SetBoost
	float d24 = boost > 1.0f ? (boost - 1.0f) * 3.0f + 1.0f : 1.0f;
	float d22 = (boost > 1.0f ? (boost - 1.0f) * 2.5f + 1.0f : 1.0f) * MELLOW_TYRE_K;
	float v2 = dot3(s->rbVel, s->rbVel);
	float feedback = graphGet(kMellowFeedback, 3, v2 > 5e-11f ? sqrtf(v2) : 0.0f);
	float split = cf(s, CPI_DTI + DTI_SPLIT);
	float x = s->mlRpm / boost;
	float T = s->thr * tableGet(ei + EI_TORQUE / 4, 9, ei[0], ei[2], x) -
		(1.0f - s->thr) * tableGet(ei + EI_BRAKING / 4, 3, ei[0], ei[2], x);
	if (s->thr < 0.05f && s->mlRpm == ei[0]) T = 0.0f;
	float ratio = dtRatio(s, s->dtGear);
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		const CarSimWheel* wh = &s->wheel[i];
		if (wh->locked) {
			dw[i] = 0.0f;
			continue;
		}
		float share = i < 2 ? 1.0f - split : split;
		float fin = cf(s, CPI_DTI + (i < 2 ? DTI_REAR_FINAL : DTI_FRONT_FINAL));
		float ws = clampf(wh->w, -MELLOW_WHEEL_W_CLAMP, MELLOW_WHEEL_W_CLAMP);
		float k = share > 0.0f ? feedback / (d24 * d22) : feedback;
		dw[i] = (share * MELLOW_DRIVE_SHARE * T * ratio * fin * d24 +
			s->mlBrake * MELLOW_BRAKE_K * 2.5f * ws - s->mlFc[i][0] * k) / MELLOW_WHEEL_INERTIA;
	}
}

// Mellovator::ResolveForces: each tyre's force in proportion to its slip
// speed, capped at the wheel's load times the friction cap, plus the term
// that keeps a car parked across a slope where it is.
//
// ResolveForces also reads +0x1E4 against the ticker, and zeroes every force
// while that grace time is ahead; +0x1E4 is 0 from the constructor on, so the
// grace has always passed and the term is left out.
static void mlTyreForces(CarSim* s) {
	const float boost = catchupOf(s), fboost = boost;   // +0x13C, +0x140: SetBoost
	float heading = atan2f(s->rot[0][1], s->rot[0][0]);
	float steer = (float)s->steerU16 * (6.2831853f / 65536.0f);
	float cs = cosf(steer), sn = sinf(steer);
	float vm = len3(s->rbVel);
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* wh = &s->wheel[i];
		const CarSimSusp* su = &s->susp[i];
		float rc[3] = { wh->pos[0] - s->cg[0], wh->pos[1] - s->cg[1], wh->pos[2] - s->cg[2] };
		float r[3];
		toWorld(s->rot, rc, r);
		wh->sliding = 0;
		float fc[2] = { 0.0f, 0.0f };
		if (su->onGround) {
			// The contact patch's velocity in the plane, turned into the
			// wheel's own frame (the yaw rate's part only, as retail has it).
			float vx = s->rbVel[0] - s->w[2] * r[1];
			float vy = s->w[2] * r[0] + s->rbVel[1];
			float a = heading + (i < 2 ? steer : 0.0f);
			float ca = cosf(a), sa = sinf(a);
			float vlong = vx * ca + vy * sa, vlat = -vx * sa + vy * ca;
			float sl = vlong - wh->w * wh->radius;
			wh->s = sl;
			wh->slip = vlat;
			float m = sqrtf(sl * sl + vlat * vlat);
			float k = m > fboost * MELLOW_STATIC_SLIP ? s->mlRating * fboost : 1.0f;
			if (m > MELLOW_SLIDING_SLIP) wh->sliding = 1;
			float Fl = -sl * MELLOW_TYRE_K * su->F * k;
			float Ft = -vlat * MELLOW_TYRE_K * su->F * k;
			if (i < 2) {
				fc[0] = cs * Fl - sn * Ft;
				fc[1] = sn * Fl + cs * Ft;
			} else {
				fc[0] = Fl;
				fc[1] = Ft;
			}
		}
		s->mlFc[i][0] = fc[0];
		s->mlFc[i][1] = fc[1];
		float Fc[3] = { fc[0], fc[1], su->F }, F[3];
		toWorld(s->rot, Fc, F);
		if (su->onGround) {
			float fh = sqrtf(F[0] * F[0] + F[1] * F[1]);
			float cap = su->F * s->mlMu * fboost;
			if (cap < fh) {
				F[0] *= cap / fh;
				F[1] *= cap / fh;
			}
			float bm = boost - 1.0f > 0.0f ? (boost - 1.0f) * 2.5f + 1.0f : 1.0f;
			F[0] *= bm;
			F[1] *= bm;
			float l = su->F / s->rot[2][2];
			if (wh->locked || (s->mlBrake > MELLOW_HOLD_BRAKE && vm < MELLOW_HOLD_SPEED)) {
				F[0] += -s->rot[2][0] * l;
				F[1] += -s->rot[2][1] * l;
			} else if (fh < cap) {
				F[0] += -s->rot[2][0] * fabsf(s->rot[1][0]) * l;
				F[1] += -s->rot[2][1] * fabsf(s->rot[1][1]) * l;
			}
		}
		rbApplyForce(s, F, r);
		float fl = sqrtf(s->mlFc[i][0] * s->mlFc[i][0] + s->mlFc[i][1] * s->mlFc[i][1]);
		if (fl > MELLOW_FC_CLAMP) {
			s->mlFc[i][0] *= MELLOW_FC_CLAMP / fl;
			s->mlFc[i][1] *= MELLOW_FC_CLAMP / fl;
		}
	}
}

// Mellovator::BottomOut: a wheel past its bump stop takes 0.2 of the speed it
// is driving into the ground with off the car. Returns the impulse given.
static float mlBottomOut(CarSim* s) {
	float sum = 0.0f;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		if (!(s->mlPen[i] > 0.0f)) continue;
		const CarSimWheel* wh = &s->wheel[i];
		float rc[3] = { wh->pos[0] - s->cg[0], wh->pos[1] - s->cg[1],
			wh->pos[2] - s->cg[2] + s->susp[i].x };
		float r[3], wxr[3], pv[3];
		toWorld(s->rot, rc, r);
		cross3(s->w, r, wxr);
		for (int k = 0; k < 3; k++) pv[k] = s->rbVel[k] + wxr[k];
		float vn = -dot3(pv, wh->n);
		if (vn > 0.0f) {
			float j = vn * MELLOW_BOTTOM_OUT * s->mass;
			float J[3] = { wh->n[0] * j, wh->n[1] * j, wh->n[2] * j };
			rbApplyImpulse(s, J, r, 1.0f);
			sum += j;
		}
	}
	return sum;
}

// MellowMover::JumpStabilizer: a car on fewer than two wheels has its spin
// damped by MellowJumpStabilization. The airborne realign that follows (no
// wheel down, up.z over 0.1) only turns the car when mass * 100 * (dot - 1)
// reaches 1e-5, and a dot of two unit vectors never takes that above 0, so
// its moment is always zero and it is left out.
static void mlJumpStabilizer(CarSim* s) {
	if (s->dampenMode == 1 || s->wheelsDown >= 2) return;
	float k = -s->mass * graphGet(kMellowJumpStab, 3, len3(s->w));
	for (int i = 0; i < 3; i++) s->T[i] += s->w[i] * k;   // ApplyMoment
}

// PhysicsSystem::ComputeAuxiliaries over the MellowMover's two components.
static void mlSystemAux(CarSim* s) {
	rbAux(s);
	mlAux(s);
}

// MellowMover::ComputeAuxiliaries, then its ResolveForces (the Mellovator's
// tyres, Aerodynamics, the hydraulics an AI car has none of, JumpStabilizer;
// MaybeForceYaw is not taken). The second system pass after a dousing or a
// bottom-out runs at the same timestep, so it reads the springs as still:
// retail's, kept.
static void mlComputeAux(CarSim* s) {
	mlSystemAux(s);
	int r = dousing(s, 0);
	float j = mlBottomOut(s);
	if (r || j > 0.0f) mlSystemAux(s);
	if (s->dousing == 2) s->mlBrake = 1.0f;
	mlTyreForces(s);
	aeroResolve(s);
	mlJumpStabilizer(s);
}

// PhysicsSystem::IntegrateEuler over the rigid body and the four wheel spins.
static void mlIntegrate(CarSim* s, float h) {
	Deriv d;
	float dw[CARPHYS_WHEELS];
	rbDeriv(s, &d);
	mlDeriv(s, dw);
	for (int i = 0; i < 3; i++) {
		s->rbPos[i] += h * d.pos[i];
		s->P[i] += h * d.P[i];
		s->L[i] += h * d.L[i];
		for (int r = 0; r < 3; r++) s->rot[r][i] += h * d.rot[r][i];
	}
	s->damping += h * d.damping;
	for (int i = 0; i < CARPHYS_WHEELS; i++) s->wheel[i].w += h * dw[i];
	mlComputeAux(s);
}

// MellowMover::Move, after Car::Move's ResetDampenMode and the driver.
static void mlMove(CarSim* s, float dt) {
	// The surface factor is the mean over the touching wheels; ROAD is 1.
	const float surface = 1.0f;
	int zdamp = 0;
	if (s->dampenMode == 1) {
		s->flag13c = s->flag140 = 0;
		s->g = WILD_COLLISION_GRAVITY;
		s->aeroScale = (float)s->wheelsDown * 0.25f;
		s->aeroSurf = 1.0f;
	} else {
		s->flag13c = s->flag140 = 1;
		s->g = GRAVITY;
		s->aeroScale = 1.0f;
		s->aeroSurf = surface;
		zdamp = 1;
	}
	if (s->wheelsDown == 0) zdamp = 0;
	setZRotationDamping(s, zdamp);
	calibrate(s);
	s->mlBrake = s->brakeIn > s->ebrakeIn * 0.5f ? s->brakeIn : s->ebrakeIn * 0.5f;
	s->aeroThr = s->thr;
	s->aeroDown = (float)s->wheelsDown * 0.25f;
	s->govBoost = catchupOf(s);   // SetBoost(Car +0x478): RigidBody +0x1F0
	int n = (int)(dt / MELLOW_MAX_TIME_STEP) + 1;
	float h = dt / (float)n;
	for (int i = 0; i < n; i++) {
		s->ugly = h;
		mlIntegrate(s, h);
		orthonormalise(s->rot);
	}
}

// MellowMover::UpdateCar: the wheels as drawn turn toward the steering at
// 120 degrees a second, and the CarState reads the Mellovator.
static void mlUpdateCar(CarSim* s, float dt) {
	steerVis(s, dt);
	s->steerFL = s->steerFR = s->mlSteerVis * 0.0174533f;

	s->vfwd = dot3(s->rbVel, s->rot[0]);
	s->speedAbs = len3(s->rbVel);
	int n = 0;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* wh = &s->wheel[i];
		wh->angle += dt * wh->w;
		wh->onGround = s->susp[i].onGround;
		n += wh->onGround ? 1 : 0;
	}
	s->wheelsDown = n;
	s->rpmSmooth = s->mlRpm;
}

// ---------------------------------------------------------------------------
// Contacts (PORT geometry, retail impulse)

// The SPIN scale an impulse on the car turns it by: its whole inertia in
// dampening mode 1, 0.4 of it in every other.
static float dampenSpin(const CarSim* s) {
	return s->dampenMode == 1 ? 1.0f : RETAIL_DAMPED_SPIN;
}

// Car::ResetDampenMode (0x80223E84), first thing in Car::Move, on what the
// body touched last frame. Mode 1 holds until the car has had all four wheels
// down and nothing against it for 0.1 s; anything against the body puts
// mode 0 into 2, and 2 drops back to 0 as soon as it is clear on four wheels.
#define RETAIL_WILD_SETTLE 0.1f
static void resetDampenMode(CarSim* s, float dt) {
	const bool quiet = s->contacts == 0 && s->wheelsDown == 4;
	if (s->dampenMode == 1) {
		if (!quiet)
			s->dampenQuiet = -1.0f;
		else if (s->dampenQuiet < 0.0f)
			s->dampenQuiet = 0.0f;
		else if ((s->dampenQuiet += dt) >= RETAIL_WILD_SETTLE)
			s->dampenMode = 0;
	} else if (s->dampenMode == 2) {
		if (quiet)
			s->dampenMode = 0;
	} else if (s->contacts > 0) {
		s->dampenMode = 2;
	}
	s->contacts = 0;
}

#define IMPULSE_HOOKS 4
static CarSimImpulseHook s_impulseHook[IMPULSE_HOOKS];

void carsim_add_impulse_hook(CarSimImpulseHook fn) {
	int spare = -1;
	for (int i = 0; i < IMPULSE_HOOKS; i++) {
		if (s_impulseHook[i] == fn) return;
		if (!s_impulseHook[i] && spare < 0) spare = i;
	}
	if (spare >= 0) s_impulseHook[spare] = fn;
}

void carsim_remove_impulse_hook(CarSimImpulseHook fn) {
	for (int i = 0; i < IMPULSE_HOOKS; i++)
		if (s_impulseHook[i] == fn) s_impulseHook[i] = NULL;
}

// CheckForWildCollision's drag arm (0x801ABE94), which every Collision::
// AddImpulse runs for each car it touches: in a drag (TheRaceParameters
// +0x40) a racing car (Car+0x1C) hit by an impulse of 30 or more, its size
// counted to 40 as the wild test counts it, is totaled -- Car+0x964, which
// Race::Update finishes with reason 9. The arm's other test, a float read
// through the body's +0x58 interface (+0x34, then +0x38 < -0.5), is not
// decoded and not ported. Its SetVisibleDamageEnabled is main.c's
// carDamageUpdate, which reads `totaled` (#324).
static void dragTotal(CarSim* s, const float* J) {
	float jl = len3(J);
	if (jl > 40.0f) jl = 40.0f;
	if (s->drag && jl >= 30.0f) s->totaled = true;
}

// Collision::AddImpulse's call: the impulse negated, at the contact (carsim.h).
static void hookImpulse(const CarSim* s, const CarSim* other, CarSimProp* prop, bool ground,
		const float* r, const float* J, float slip) {
	const float pos[3] = { s->rbPos[0] + r[0], s->rbPos[1] + r[1], s->rbPos[2] + r[2] };
	const float imp[3] = { -J[0], -J[1], -J[2] };
	for (int i = 0; i < IMPULSE_HOOKS; i++)
		if (s_impulseHook[i]) s_impulseHook[i](s, other, prop, ground, pos, imp, slip);
}

void carsim_prop_world_impulse(CarSimProp* p, bool ground, const float pos[3],
		const float impulse[3], float slip) {
	const float imp[3] = { -impulse[0], -impulse[1], -impulse[2] };
	for (int i = 0; i < IMPULSE_HOOKS; i++)
		if (s_impulseHook[i]) s_impulseHook[i](NULL, NULL, p, ground, pos, imp, slip);
}

// Contact::CollisionImpulse against a static surface, normal `n` facing the
// car, `r` the contact point from the centre of gravity. Returns the speed
// into the surface, 0 if the point was already leaving it. `ground` is only
// for the impulse hook: which of the world's surfaces it was.
static float contactImpulse(CarSim* s, const float* n, const float* r, float bounce, float friction,
		bool ground) {
	float rr[3] = { r[0], r[1], r[2] };
	// With all four wheels down the lever arm loses its up component, so the
	// hit turns the car about its up axis only ((+0x124)+0x434 == 4).
	if (s->wheelsDown == 4) {
		float u = dot3(rr, s->rot[2]);
		for (int k = 0; k < 3; k++) rr[k] -= u * s->rot[2][k];
	}
	float vp[3], wxr[3];
	cross3(s->w, rr, wxr);
	for (int k = 0; k < 3; k++) vp[k] = s->rbVel[k] + wxr[k];
	float vn = dot3(vp, n);
	if (vn >= 0.0f) return 0.0f;

	float invM = s->invMass > RETAIL_WALL_INV_MASS ? RETAIL_WALL_INV_MASS : s->invMass;
	float rn[3], t[3], u[3];
	cross3(rr, n, rn);
	rbInvInertiaWorld(s, rn, t);
	for (int k = 0; k < 3; k++) t[k] *= dampenSpin(s);
	cross3(t, rr, u);
	float j = -(1.0f + bounce) * vn / (invM + dot3(n, u));
	float jMin = RETAIL_WALL_MIN_J / invM;
	if (j < jMin) j = jMin;

	float vt[3];
	for (int k = 0; k < 3; k++) vt[k] = vp[k] - n[k] * vn;
	float vtl = len3(vt);
	float jf = j * friction;
	if (vtl < RETAIL_WALL_SCRAPE) jf *= vtl / RETAIL_WALL_SCRAPE;
	float J[3];
	for (int k = 0; k < 3; k++) J[k] = n[k] * j - (vtl > 1e-5f ? vt[k] / vtl * jf : 0.0f);
	hookImpulse(s, NULL, NULL, ground, r, J, vtl);
	dragTotal(s, J);
	if (s->invMass > RETAIL_WALL_INV_MASS)
		for (int k = 0; k < 3; k++) J[k] *= RETAIL_WALL_INV_MASS * s->mass;
	rbApplyImpulse(s, J, rr, dampenSpin(s));
	return -vn;
}

// The body against the ground: the deepest of the four probed body points is
// pushed out along the surface normal and given a ground impulse.
//
// A surface higher than the box reaches over that corner is not one the body
// is in: the body is under it (TASKS.md #251). The walls stop a footprint
// 0.92 of the box (main.c's WORLD_FOOTPRINT), so a car pressed against one has
// its corners just past it, where the section tree may have nothing at street
// level and hand back the road over the top. Taking that as ground stood the
// car on an overpass deck 10.8 m up. Retail never moves the car onto ground
// above it -- Car::LandSharkDetector (0x80223df8) asks for a reset instead.
// The body's reach is its highest point: a car on its side has the box
// corner's own top (`^ 4`) on the ground beside it, which passed every surface
// the corner was in as one over the car, and the car sank through the road.
static void bodyGroundContact(CarSim* s) {
	float deepest = 0.0f, dn[3] = { 0.0f, 0.0f, 1.0f }, dr[3] = { 0.0f, 0.0f, 0.0f };
	int found = 0;
	float reach = -1e9f;
	const int np = bodyPoints(s);
	for (int k = 0; k < np; k++) {
		float c[3], p[3];
		bodyPoint(s, k, c);
		toWorld(s->rot, c, p);
		if (p[2] > reach) reach = p[2];
	}
	for (int j = 0; j < CARPHYS_WHEELS; j++) {
		int slot = CARPHYS_WHEELS + j;
		if (!s->plane[slot].valid) continue;
		float c[3], p[3], n[3];
		bodyPoint(s, s->corner[j], c);
		toWorld(s->rot, c, p);
		float elev = groundElev(s, slot, s->rbPos[0] + p[0], s->rbPos[1] + p[1], n);
		if (elev > s->rbPos[2] + reach) continue;
		float pen = (elev - (s->rbPos[2] + p[2])) * n[2];
		if (pen > deepest) {
			deepest = pen;
			for (int k = 0; k < 3; k++) { dn[k] = n[k]; dr[k] = p[k]; }
			found = 1;
		}
	}
	s->touching = found != 0;
	if (!found) return;
	s->contacts++;
	for (int k = 0; k < 3; k++) s->rbPos[k] += dn[k] * deepest;
	float hit = contactImpulse(s, dn, dr, RETAIL_GROUND_BOUNCE, RETAIL_GROUND_FRICTION, true);
	if (hit > s->lastImpact) s->lastImpact = hit;
}

// The body against the walls, through the footprint resolver: the box's plan
// centre moved from where it was drawn last frame to where it is now.
// GetCollisionClassification (0x8000E564) for a contact at `at` (world XY),
// kept as a run per side: a side touched again within CARSIM_TOUCH_GAP is the
// same Collision, still ageing. The contact is taken into the car's frame from
// its position (WorldToCar 0x801AA6E4 from CarState +0x18, the rigid body's)
// and is on a side at 0.95 (0x80316324) of GetCarSpaceBoundingBox's width or
// further (0x802230F0: the polytope's box, BuildBoundingVolumes' +0xDC /
// +0xEC), each side against its own bound. `with` is CARSIM_TOUCH_WALL or the
// other car's speed. PORT: that speed is the one it had at the touch, where
// Drive reads the car's speed when it steers, a frame later.
static void recordTouch(CarSim* s, const float at[2], float with) {
	const float arm[2] = { at[0] - s->rbPos[0], at[1] - s->rbPos[1] };
	float fwd = arm[0] * s->rot[0][0] + arm[1] * s->rot[0][1];
	float lat = arm[0] * s->rot[1][0] + arm[1] * s->rot[1][1];
	int c;
	if (lat >= s->tpMax[1] * 0.95f || lat <= s->tpMin[1] * 0.95f)
		c = lat > 0.0f ? CARSIM_TOUCH_LEFT : CARSIM_TOUCH_RIGHT;
	else
		c = fwd > 0.0f ? CARSIM_TOUCH_FRONT : CARSIM_TOUCH_BEHIND;
	if (carsim_touch_age(s, c) < 0.0f) s->touchStart[c] = s->clock;
	s->touchLast[c] = s->clock;
	s->touchX[c] = -lat;
	s->touchWith[c] = with;
}

static void wallContact(CarSim* s, const CarSimWorld* w, const float from[3]) {
	if (!w || !w->collide) return;
	float c[3];
	toWorld(s->rot, s->cg, c);
	float to[3] = { s->rbPos[0] - c[0], s->rbPos[1] - c[1], s->rbPos[2] - s->hRest };
	float before[2] = { to[0], to[1] };
	float nx = 0.0f, ny = 0.0f, arm[2] = { 0.0f, 0.0f };
	float yaw = atan2f(s->rot[0][1], s->rot[0][0]);
	if (!w->collide(w->ctx, from, to, yaw, cf(s, CPI_DIMS) * 0.5f, cf(s, CPI_DIMS + 4) * 0.5f,
		&nx, &ny, arm))
		return;
	s->contacts++;
	s->yawHitAt = s->clock;
	const float at[2] = { to[0] + arm[0], to[1] + arm[1] };
	recordTouch(s, at, CARSIM_TOUCH_WALL);
	s->rbPos[0] += to[0] - before[0];
	s->rbPos[1] += to[1] - before[1];
	float len = sqrtf(nx * nx + ny * ny);
	if (len <= 0.0001f) return;
	float n[3] = { nx / len, ny / len, 0.0f };
	float r[3] = { to[0] + arm[0] - s->rbPos[0], to[1] + arm[1] - s->rbPos[1], 0.0f };
	float hit = contactImpulse(s, n, r, RETAIL_WALL_BOUNCE, RETAIL_WALL_FRICTION, false);
	if (hit > s->lastImpact) s->lastImpact = hit;
}

// ---------------------------------------------------------------------------
// Track polygons (TASKS.md #223)
//
// Retail's body against the track. Every polygon the topology hands back round
// the car -- the roads, the walls and the 0x08 surfaces alike -- becomes a
// TrackPolygonCollisionBody: a flat GJKWafer of its three or four vertices
// (the wafer's 1 m extrusion is for Smokeables only), static, at the world's
// origin. A CollisionBodyWitness between it and the car keeps the GJK distance
// from the car's hull, and at a millimetre or less the pair is a Contact:
// backed up to where it first touched, crowbarred out when it never came
// apart, given Contact::CollisionImpulse and moved on again for the rest of
// the step. docs/retail_track_collision.md is the write-up; each function
// here names the retail one it is.
//
// PORT:
//   * Each car's contacts are resolved on their own, 20 to a car, where
//     retail sorts every body's into one list and stops at 20 for the world.
//     Against a static polygon only the car moves, so only the order between
//     cars differs.
//   * The GJK core, SCEA_GJKdistance, is not in the dumps. tpGjk() is van den
//     Bergen's loop over Ericson's closest points on the simplex: an exact
//     distance with its two closest points is the same answer.
//   * It runs in a frame at the car, the polygon clipped to TP_CLIP round it:
//     an unbounded wall reaches 2048 m up and down, and float GJK loses the
//     millimetres the contact is decided on at that scale.
//   * A body retail made for another car gets this car's witness on the box
//     overlap alone (MaybeCreateWitnesses' sweep); here every witness passes
//     TrackPolygonCheck, because each car keeps its own polygons.
//   * Car::Move's ResetDampenMode is not run again for the rest of a step
//     after a contact: the port counts the body's contacts per frame (see
//     resetDampenMode), where retail's Collision count lives for 0.3 s.

#define TP_BOX_PAD          0.1f     // SetInterestBBox's pad (0x80336080)
#define TP_LOOK_AHEAD       0.2f     // its look-ahead, s (0x80336084)
#define TP_LOOK_MAX         3.0f     // clamped to this (0x80336088)
#define TP_SLAB_PAD         0.05f    // BuildBoundingVolumes' (0x80331E08)
#define TP_OUTSIDE_PAD      0.1f     // TrackPolygonCheck's R + 0.1 (0x80331EC8)
#define TP_SAME_VERT        0.001f   // the key table's compare (0x80331EB4), GJKWafer::Init's (0x80332428)
#define TP_MOVED            0.001f   // UpdateWorldMatrices' (0x803323BC)
#define TP_COLLIDING        0.001f   // RecalcDistance's (0x803323CC)
#define TP_DEEP             1e-5f    // (0x803323C8)
#define TP_LIFETIME         0.3f     // CollisionLifetime
#define TP_MAX_RESOLVES     20       // MAX_CONTACT_RESOLUTIONS
#define TP_MAX_PER_WITNESS  2        // MAX_CONTACTS_PER_FRAME
#define TP_PROBES           8        // FindContactTime's halvings
#define TP_CROWBAR_PASSES   20
#define TP_CROWBAR_Z        0.15f    // Crowbar's clamp on the push's height (0x80332104)
#define TP_WALLISH          0.1f     // IsWallish (0x8033200C)
#define TP_GROUNDISH        0.7f     // IsGroundish (0x80332010)
#define TP_GJK_ITERATIONS   32       // PORT
#define TP_CLIP             16.0f    // PORT: the clip round the car, m
#define TP_CLIP_MAX         10       // a quad cut by six planes

// The dyno's hold after each step (carsim_set_dyno): back on the spot, the
// momentum kept only along the car's own level heading and no yaw.
// PORT: retail (ResolveDynoForces 0x801C2CB0) springs a virtual body back to
// the spot and leaves the real body's height and attitude to the suspension;
// here the body keeps the 49 m/s of a pull while it stands still, and the
// tyres and dampers fed by that speed pitched it by up to 15 degrees and
// bounced it 20 cm (the user, 2026-11-08: "THE CAR IS SUPPOSED TO BE STILL
// DURING THESE TESTS"). The whole pose is held as it stood at the start of
// the run: only the wheels and the engine move.
static void dynoHold(CarSim* s, float h) {
	s->rbPos[0] = s->dynoXY[0];
	s->rbPos[1] = s->dynoXY[1];
	s->rbPos[2] = s->dynoZ;
	memcpy(s->rot, s->dynoRot, sizeof(s->rot));
	s->P[2] = 0.0f;
	s->L[0] = s->L[1] = 0.0f;
	float fx = s->rot[0][0], fy = s->rot[0][1];
	const float n = sqrtf(fx * fx + fy * fy);
	if (n > 1e-4f) {
		fx /= n;
		fy /= n;
		float p = s->P[0] * fx + s->P[1] * fy;
		// #494: the coast's roller drag, towards a stop and never past it.
		if (s->dynoDrag > 0.0f) {
			const float dp = s->mass * s->dynoDrag * h;
			p = p > dp ? p - dp : p < -dp ? p + dp : 0.0f;
		}
		s->P[0] = fx * p;
		s->P[1] = fy * p;
	}
	s->L[2] = 0.0f;
	computeAux(s);
}

// The mover's Move over `dt`: the step's own, and the rest of it after a
// contact (CarCollisionBody::Move). The mellow mover's, or PhysicsMover::Move,
// which first clears CarState +0x328..+0x334.
static void moverMove(CarSim* s, const CarSimInput* in, float dt) {
	if (s->mellow) {
		mlMove(s, dt);
		return;
	}
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		s->bottomPen[i] = 0.0f;
		s->hydFired[i] = 0;   // CarState +0x3CC, PhysicsMover::Move
	}
	calibrate(s);
	tuneBoostAndDampers(s, in);
	delegateDriverInput(s);
	tuneWheelParams(s, dt);
	int n = (int)(dt / MAX_TIME_STEP) + 1;
	float h = dt / (float)n;
	for (int i = 0; i < n; i++) {
		s->ugly = h;   // UglyTimestepHack
		integrate(s, h);
		orthonormalise(s->rot);
		if (s->dyno)
			dynoHold(s, h);
	}
}

// The car's GJK polytope from the centre of gravity (MoveCenterOfGravity) is
// bodyPoint()'s: the hull's vertices, or the record's body box. PORT: retail's
// last resort for a car with no polytope, FindGJKPolytope's box
// (0x80331EE0..0x80331EF4, -2.217,-0.971,-0.2 .. 2.165,0.971,0.624 in the
// model), reaches 4 cm under the road at the 350Z's CG and ride height, and
// no shipped car takes it: every car folder on the PC disc but four has a
// hull, and those four (IMPREZA, LANCER, NEON, S2000) hold only vinyls. On
// the port a car goes without one only when CARHULLS.nfsg is missing, and it
// then meets the track with the box it meets the ground with.

// BuildBoundingVolumes (0x801AE39C): the polytope's box with the centre of
// gravity always inside it, the largest radius, the two slab heights
// TrackPolygonCheck reads and the box's middle.
static void tpVolumes(CarSim* s) {
	float r2 = 0.0f;
	for (int i = 0; i < 3; i++) s->tpMin[i] = s->tpMax[i] = 0.0f;
	const int n = bodyPoints(s);
	for (int k = 0; k < n; k++) {
		float c[3];
		bodyPoint(s, k, c);
		for (int i = 0; i < 3; i++) {
			if (c[i] < s->tpMin[i]) s->tpMin[i] = c[i];
			if (c[i] > s->tpMax[i]) s->tpMax[i] = c[i];
		}
		const float d = dot3(c, c);
		if (d > r2) r2 = d;
	}
	s->tpR = sqrtf(r2);
	s->tpBelow = -s->tpMin[2] + TP_SLAB_PAD;
	s->tpAbove = s->tpMax[2] + TP_SLAB_PAD;
	for (int i = 0; i < 3; i++) s->tpCentre[i] = 0.5f * (s->tpMin[i] + s->tpMax[i]);
}

// DynamicCollisionBody::BeginMove (0x801AE8A4): the pose and velocity the step
// starts from, which a contact backs up toward, and UpdatePosition's +0xFC,
// the middle of the car's box where the step starts.
static void beginMove(CarSim* s, float dt) {
	memcpy(s->mvRot, s->rot, sizeof(s->rot));
	for (int k = 0; k < 3; k++) {
		s->mvPos[k] = s->rbPos[k];
		s->mvVel[k] = s->rbVel[k];
	}
	s->mvDt = dt;
	toWorld(s->rot, s->tpCentre, s->mvCentre);
	for (int k = 0; k < 3; k++) s->mvCentre[k] += s->rbPos[k];
}

// DynamicCollisionBody::Interpolate (0x801AE9F8): the pose `t` seconds back
// from the end of the step, every element of the matrix lerped on its own.
static float tpBackFraction(const CarSim* s, float t) {
	return s->mvDt >= 1e-6f ? (s->mvDt - t) / s->mvDt : 1.0f;
}

static void tpInterpolate(const CarSim* s, float t, float R[3][3], float p[3]) {
	const float f = tpBackFraction(s, t);
	for (int k = 0; k < 3; k++) {
		p[k] = s->mvPos[k] + (s->rbPos[k] - s->mvPos[k]) * f;
		for (int j = 0; j < 3; j++)
			R[k][j] = s->mvRot[k][j] + (s->rot[k][j] - s->mvRot[k][j]) * f;
	}
}

// TrackPolygon's edges in GetAmountOutside's order: v0-v1, v1-v2, v3-v0, and
// v2-v3 on a quad.
static const u8 kTpEdge[4][2] = { { 0, 1 }, { 1, 2 }, { 3, 0 }, { 2, 3 } };

// TrackPolygon::GetAmountOutside (0x801F0494), read for its sign: <= 0 is
// inside the polygon seen from above.
static float tpAmountOutside(const TrackPoly* p, float x, float y) {
	float worst = -1.0f;
	const int ne = p->nv == 4 ? 4 : 3;
	for (int e = 0; e < ne; e++) {
		const float* a = p->v[kTpEdge[e][0]];
		const float* b = p->v[kTpEdge[e][1]];
		const float d = (b[1] - a[1]) * (x - a[0]) - (b[0] - a[0]) * (y - a[1]);
		if (d > worst) {
			worst = d;
			if (worst > 0.0f) break;
		}
	}
	return worst;
}

// TrackPolygon::GetDistanceOutside (0x801F058C): 0 inside a polygon that is
// not a wall, otherwise the first edge nearer (x, y) than `maxDist`, otherwise
// maxDist.
static float tpDistanceOutside(const TrackPoly* p, float x, float y, float maxDist) {
	if (!(p->flags & 0x02) && tpAmountOutside(p, x, y) <= 0.0f) return 0.0f;
	const int ne = p->nv == 4 ? 4 : 3;
	for (int e = 0; e < ne; e++) {
		const float* a = p->v[kTpEdge[e][0]];
		const float* b = p->v[kTpEdge[e][1]];
		const float ex = b[0] - a[0], ey = b[1] - a[1];
		const float l2 = ex * ex + ey * ey;
		const float u = l2 > 0.0f ? clampf(((x - a[0]) * ex + (y - a[1]) * ey) / l2, 0.0f, 1.0f)
		                          : 0.0f;
		const float dx = a[0] + ex * u - x, dy = a[1] + ey * u - y;
		const float d = sqrtf(dx * dx + dy * dy);
		if (d < maxDist) return d;
	}
	return maxDist;
}

// TrackPolygonCheck (0x801AF8B4): whether the polygon's plane is within the
// slab the car's polytope can reach at the car's tilt, and its outline within
// the car's radius seen from above.
static bool tpCheck(const CarSim* s, const TrackPoly* p) {
	float c = dot3(s->rot[2], p->n);
	if (c > 1.0f) c = 1.0f;
	const float s2 = 1.0f - c * c;
	const float sn = s2 > 5e-11f ? sqrtf(s2) : 0.0f;
	const float dist = dot3(s->rbPos, p->n) - p->d;
	const float h = c * dist < 0.0f ? s->tpAbove : s->tpBelow;
	if (!(fabsf(dist) < fabsf(c) * h + sn * s->tpR)) return false;
	return tpDistanceOutside(p, s->rbPos[0], s->rbPos[1], s->tpR + TP_OUTSIDE_PAD) <= s->tpR;
}

// bEqual (0x8005D588): every axis within `tol`.
static bool tpSameVert(const float* a, const float* b) {
	return fabsf(a[0] - b[0]) <= TP_SAME_VERT && fabsf(a[1] - b[1]) <= TP_SAME_VERT &&
		fabsf(a[2] - b[2]) <= TP_SAME_VERT;
}

// The car's box seen from above (UpdatePosition: pos.xy +- R) against a
// polygon's, edges included.
static bool tpBoxesMeet(const float a[4], const float b[4]) {
	return !(b[2] < a[0] || b[0] > a[2] || b[3] < a[1] || b[1] > a[3]);
}

static void tpCarBox(const CarSim* s, float bb[4]) {
	bb[0] = s->rbPos[0] - s->tpR;
	bb[1] = s->rbPos[1] - s->tpR;
	bb[2] = s->rbPos[0] + s->tpR;
	bb[3] = s->rbPos[1] + s->tpR;
}

// SetInterestBBox (0x801F1198), then CreateTrackPolygonCollisionBodies
// (0x801AF514) and MaybeCreateWitnesses (0x801AF2BC) for the car: a witness
// for each polygon near enough to matter that has none yet. The polygon list
// is fetched again only once the car's box leaves the one it was fetched for,
// which is padded and stretched ahead along the velocity. False when the
// world has no topology to ask.
static bool tpCreate(CarSim* s, const CarSimInput* in, const CarSimWorld* w) {
	float bb[4];
	tpCarBox(s, bb);
	if (s->tpN < 0 || bb[0] < s->tpBox[0] || bb[1] < s->tpBox[1] ||
	    bb[2] > s->tpBox[2] || bb[3] > s->tpBox[3]) {
		float nb[4] = { bb[0] - TP_BOX_PAD, bb[1] - TP_BOX_PAD, bb[2] + TP_BOX_PAD,
			bb[3] + TP_BOX_PAD };
		for (int k = 0; k < 2; k++) {
			const float a = clampf(s->rbVel[k] * TP_LOOK_AHEAD, -TP_LOOK_MAX, TP_LOOK_MAX);
			if (a > 0.0f) nb[2 + k] += a;
			else nb[k] += a;
		}
		const int n = w->trackPolys(w->ctx, nb, s->tpId, &s->tpBoxes[0][0], CARSIM_TP_MAX);
		if (n < 0) {
			s->tpN = -1;
			return false;
		}
		s->tpN = n;
		memcpy(s->tpBox, nb, sizeof(nb));
	}

	// The polygons under the car's box with no witness yet that pass the
	// check, at most 48, sorted by the polygon.
	u32 cand[CARSIM_WITNESS_MAX];
	int nc = 0;
	for (int i = 0; i < s->tpN && nc < CARSIM_WITNESS_MAX; i++) {
		if (!tpBoxesMeet(bb, s->tpBoxes[i])) continue;
		const u32 id = s->tpId[i];
		bool have = false;
		for (int j = 0; j < s->witN && !have; j++) have = s->wit[j].id == id;
		if (have) continue;
		TrackPoly p;
		if (!w->trackPoly(w->ctx, id, &p) || !tpCheck(s, &p)) continue;
		int at = nc++;
		for (; at > 0 && cand[at - 1] > id; at--) cand[at] = cand[at - 1];
		cand[at] = id;
	}

	for (int i = 0; i < nc && s->witN < CARSIM_WITNESS_MAX; i++) {
		CarSimWitness* wt = &s->wit[s->witN];
		if (!w->trackPoly(w->ctx, cand[i], &wt->poly)) continue;
		// TrackPolygonKeyTable: a polygon with the same vertices as one that
		// has a body already is that body.
		bool dup = false;
		for (int j = 0; j < s->witN && !dup; j++) {
			dup = true;
			for (int k = 0; k < 4 && dup; k++) dup = tpSameVert(s->wit[j].poly.v[k], wt->poly.v[k]);
		}
		if (dup) continue;
		wt->id = cand[i];
		// GJKWafer::Init counts a triangle by its fourth vertex being its
		// third, not by the flag: an unbounded triangle's fourth is not.
		wt->wnv = tpSameVert(wt->poly.v[2], wt->poly.v[3]) ? 3 : 4;
		// CarCollisionBody::GetGJKPoly (0x801AFF10), once, at the witness's
		// making: the polytope's box for an AI car with its four wheels down.
		wt->box = in->aiDriver && s->wheelsDown == 4;
		for (int k = 0; k < 3; k++) {
			wt->lastPos[k] = 1e30f;
			wt->pa[k] = wt->pb[k] = 0.0f;
		}
		wt->dist = 1e30f;
		wt->hitAt = -1.0f;
		wt->colliding = wt->deep = wt->resolves = 0;
		s->witN++;
	}
	return true;
}

// ---- GJK (PORT core, see the section header)

typedef struct {
	float w[3], a[3], b[3];   // a - b, and the two support points it came from
} TpGjkPt;

typedef struct {
	const CarSim* s;
	bool  box;
	float R[3][3];               // the car's matrix
	float poly[TP_CLIP_MAX][3];  // the wafer, clipped, from the car's position
	int   pn;
	// Another car in the wafer's place (carsim_car_contact): its shape, its
	// matrix and its centre of gravity from the first car's.
	const CarSim* sb;
	bool  boxB;
	float Rb[3][3];
	float ob[3];
} TpGjk;

// GetSupportPoint on a car's shape at matrix `R`, world direction `d`: the
// polytope's point furthest along it (the first of equals), or its box's
// corner, from the car's centre of gravity.
static void carSupport(const CarSim* s, bool box, const float R[3][3], const float* d,
		float* out) {
	float dc[3], c[3];
	toCar(R, d, dc);
	if (box) {
		for (int i = 0; i < 3; i++) c[i] = dc[i] > 0.0f ? s->tpMax[i] : s->tpMin[i];
	} else {
		const int n = bodyPoints(s);
		bodyPoint(s, 0, c);
		float bd = dot3(c, dc);
		for (int k = 1; k < n; k++) {
			float p[3];
			bodyPoint(s, k, p);
			const float x = dot3(p, dc);
			if (x > bd) {
				bd = x;
				for (int i = 0; i < 3; i++) c[i] = p[i];
			}
		}
	}
	toWorld(R, c, out);
}

static void tpSupportA(const TpGjk* g, const float* d, float* out) {
	carSupport(g->s, g->box, g->R, d, out);
}

// GJKWafer::GetSupportPoint (0x801BA1D4) over the face, or the other car's.
static void tpSupportB(const TpGjk* g, const float* d, float* out) {
	if (g->sb) {
		carSupport(g->sb, g->boxB, g->Rb, d, out);
		for (int i = 0; i < 3; i++) out[i] += g->ob[i];
		return;
	}
	int best = 0;
	float bd = dot3(g->poly[0], d);
	for (int k = 1; k < g->pn; k++) {
		const float x = dot3(g->poly[k], d);
		if (x > bd) {
			bd = x;
			best = k;
		}
	}
	for (int i = 0; i < 3; i++) out[i] = g->poly[best][i];
}

// PORT: the wafer from `o`, clipped to TP_CLIP round it (Sutherland-Hodgman
// on the six faces of the box). 0 when none of it is that near.
static int tpClip(const TrackPoly* p, int nv, const float* o, float out[TP_CLIP_MAX][3]) {
	float bufA[TP_CLIP_MAX][3], bufB[TP_CLIP_MAX][3];
	float (*src)[3] = bufA, (*dst)[3] = bufB;
	int n = nv;
	for (int k = 0; k < nv; k++)
		for (int i = 0; i < 3; i++) src[k][i] = p->v[k][i] - o[i];
	for (int f = 0; f < 6; f++) {
		const int axis = f >> 1;
		const float sg = (f & 1) ? -1.0f : 1.0f;   // keep sg * x <= TP_CLIP
		int m = 0;
		for (int k = 0; k < n; k++) {
			const float* P = src[k];
			const float* Q = src[k + 1 == n ? 0 : k + 1];
			const float dp = sg * P[axis] - TP_CLIP, dq = sg * Q[axis] - TP_CLIP;
			if (dp <= 0.0f && m < TP_CLIP_MAX) {
				for (int i = 0; i < 3; i++) dst[m][i] = P[i];
				m++;
			}
			if ((dp <= 0.0f) != (dq <= 0.0f) && m < TP_CLIP_MAX) {
				const float t = dp / (dp - dq);
				for (int i = 0; i < 3; i++) dst[m][i] = P[i] + (Q[i] - P[i]) * t;
				m++;
			}
		}
		if (m == 0) return 0;
		float (*tmp)[3] = src;
		src = dst;
		dst = tmp;
		n = m;
	}
	for (int k = 0; k < n; k++)
		for (int i = 0; i < 3; i++) out[k][i] = src[k][i];
	return n;
}

// The closest point to the origin on the segment a-b, as the two weights.
static void tpSegBary(const float* a, const float* b, float* bc) {
	const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	const float l2 = dot3(ab, ab);
	const float t = l2 > 1e-30f ? clampf(-dot3(a, ab) / l2, 0.0f, 1.0f) : 0.0f;
	bc[0] = 1.0f - t;
	bc[1] = t;
}

// Ericson's ClosestPtPointTriangle (Real-Time Collision Detection, 5.1.5) for
// the origin, as the three weights. A triangle with no area takes the nearest
// of its edges.
static void tpTriBary(const float* a, const float* b, const float* c, float* bc) {
	const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	const float ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	bc[0] = bc[1] = bc[2] = 0.0f;
	const float d1 = -dot3(ab, a), d2 = -dot3(ac, a);
	if (d1 <= 0.0f && d2 <= 0.0f) { bc[0] = 1.0f; return; }
	const float d3 = -dot3(ab, b), d4 = -dot3(ac, b);
	if (d3 >= 0.0f && d4 <= d3) { bc[1] = 1.0f; return; }
	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
		const float t = d1 / (d1 - d3);
		bc[0] = 1.0f - t;
		bc[1] = t;
		return;
	}
	const float d5 = -dot3(ab, c), d6 = -dot3(ac, c);
	if (d6 >= 0.0f && d5 <= d6) { bc[2] = 1.0f; return; }
	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
		const float t = d2 / (d2 - d6);
		bc[0] = 1.0f - t;
		bc[2] = t;
		return;
	}
	const float va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) {
		const float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		bc[1] = 1.0f - t;
		bc[2] = t;
		return;
	}
	const float den = va + vb + vc;
	if (den > 1e-30f) {
		bc[0] = va / den;
		bc[1] = vb / den;
		bc[2] = vc / den;
		return;
	}
	const float* e[3][2] = { { a, b }, { a, c }, { b, c } };
	static const u8 ei[3][2] = { { 0, 1 }, { 0, 2 }, { 1, 2 } };
	float best = -1.0f;
	for (int k = 0; k < 3; k++) {
		float sb[2], q[3];
		tpSegBary(e[k][0], e[k][1], sb);
		for (int i = 0; i < 3; i++) q[i] = sb[0] * e[k][0][i] + sb[1] * e[k][1][i];
		const float d = dot3(q, q);
		if (best < 0.0f || d < best) {
			best = d;
			bc[0] = bc[1] = bc[2] = 0.0f;
			bc[ei[k][0]] = sb[0];
			bc[ei[k][1]] = sb[1];
		}
	}
}

// Whether the origin is on the far side of face a-b-c from d (Ericson's
// PointOutsideOfPlane). The face of a flat tetrahedron -- d within 0.01 mm
// of its plane -- counts as outside, so float noise never encloses the
// origin in a sliver.
static bool tpOutsideFace(const float* a, const float* b, const float* c, const float* d) {
	const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	const float ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	const float ad[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
	float n[3];
	cross3(ab, ac, n);
	const float sd = dot3(ad, n);
	if (sd * sd <= 1e-10f * dot3(n, n)) return true;
	return -dot3(a, n) * sd < 0.0f;
}

// The closest point to the origin on the simplex, and the simplex cut down
// to the points that make it, with their weights. True when a tetrahedron
// encloses the origin: then v is 0, the weights are the origin's own and
// nothing is cut.
static bool tpSimplex(TpGjkPt* sx, int* n, float* lam, float* v) {
	float bc[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	if (*n == 2) {
		tpSegBary(sx[0].w, sx[1].w, bc);
	} else if (*n == 3) {
		tpTriBary(sx[0].w, sx[1].w, sx[2].w, bc);
	} else if (*n == 4) {
		static const u8 kFace[4][4] = { { 0, 1, 2, 3 }, { 0, 2, 3, 1 }, { 0, 3, 1, 2 },
			{ 1, 3, 2, 0 } };
		float best = -1.0f;
		for (int f = 0; f < 4; f++) {
			const float* A = sx[kFace[f][0]].w;
			const float* B = sx[kFace[f][1]].w;
			const float* C = sx[kFace[f][2]].w;
			if (!tpOutsideFace(A, B, C, sx[kFace[f][3]].w)) continue;
			float fb[3], q[3];
			tpTriBary(A, B, C, fb);
			for (int i = 0; i < 3; i++) q[i] = fb[0] * A[i] + fb[1] * B[i] + fb[2] * C[i];
			const float d = dot3(q, q);
			if (best < 0.0f || d < best) {
				best = d;
				bc[0] = bc[1] = bc[2] = bc[3] = 0.0f;
				for (int i = 0; i < 3; i++) bc[kFace[f][i]] = fb[i];
			}
		}
		if (best < 0.0f) {
			// Inside: the origin's own weights, by Cramer's rule.
			const float* A = sx[0].w;
			float ab[3], ac[3], ad[3], ao[3], t[3];
			for (int i = 0; i < 3; i++) {
				ab[i] = sx[1].w[i] - A[i];
				ac[i] = sx[2].w[i] - A[i];
				ad[i] = sx[3].w[i] - A[i];
				ao[i] = -A[i];
			}
			cross3(ac, ad, t);
			const float det = dot3(ab, t);
			if (fabsf(det) > 1e-30f) {
				lam[1] = dot3(ao, t) / det;
				cross3(ao, ad, t);
				lam[2] = dot3(ab, t) / det;
				cross3(ac, ao, t);
				lam[3] = dot3(ab, t) / det;
				lam[0] = 1.0f - lam[1] - lam[2] - lam[3];
			} else {
				lam[0] = lam[1] = lam[2] = lam[3] = 0.25f;
			}
			v[0] = v[1] = v[2] = 0.0f;
			return true;
		}
	}
	for (int i = 0; i < 3; i++) {
		v[i] = 0.0f;
		for (int k = 0; k < *n; k++) v[i] += bc[k] * sx[k].w[i];
	}
	int m = 0;
	for (int k = 0; k < *n; k++)
		if (bc[k] > 0.0f) {
			sx[m] = sx[k];
			lam[m] = bc[k];
			m++;
		}
	*n = m;
	return false;
}

// SCEA_GJKdistance's answer (PORT, see the section header): the distance from
// the car's shape to the wafer, and the closest point on each, from the
// frame's origin (the car's centre of gravity).
static float tpGjk(const TpGjk* g, float pa[3], float pb[3]) {
	TpGjkPt sx[4], keep[4];
	float lam[4] = { 1.0f, 0.0f, 0.0f, 0.0f }, keepLam[4];
	int n = 0, keepN = 0;
	// A first point of A - B: the centre of gravity, inside the car, less
	// the wafer's middle (the other car's centre of gravity).
	float mid[3] = { 0.0f, 0.0f, 0.0f };
	if (g->sb) {
		for (int i = 0; i < 3; i++) mid[i] = g->ob[i];
	} else {
		for (int k = 0; k < g->pn; k++)
			for (int i = 0; i < 3; i++) mid[i] += g->poly[k][i];
		for (int i = 0; i < 3; i++) mid[i] /= (float)g->pn;
	}
	float v[3] = { -mid[0], -mid[1], -mid[2] };
	float vv = dot3(v, v);
	bool inside = false;
	for (int it = 0; it < TP_GJK_ITERATIONS && vv > 1e-12f; it++) {
		TpGjkPt q;
		const float d[3] = { -v[0], -v[1], -v[2] };
		tpSupportA(g, d, q.a);
		tpSupportB(g, v, q.b);
		for (int i = 0; i < 3; i++) q.w[i] = q.a[i] - q.b[i];
		if (vv - dot3(v, q.w) <= vv * 1e-6f) break;   // nothing nearer that way
		bool dup = false;
		for (int k = 0; k < n && !dup; k++)
			dup = sx[k].w[0] == q.w[0] && sx[k].w[1] == q.w[1] && sx[k].w[2] == q.w[2];
		if (dup) break;
		memcpy(keep, sx, sizeof(sx));
		memcpy(keepLam, lam, sizeof(lam));
		keepN = n;
		sx[n++] = q;
		float nv[3];
		if (tpSimplex(sx, &n, lam, nv)) {
			inside = true;
			vv = 0.0f;
			break;
		}
		const float nvv = dot3(nv, nv);
		if (keepN > 0 && nvv >= vv) {
			// No nearer than before: float noise. Keep the last answer.
			memcpy(sx, keep, sizeof(sx));
			memcpy(lam, keepLam, sizeof(lam));
			n = keepN;
			break;
		}
		for (int i = 0; i < 3; i++) v[i] = nv[i];
		vv = nvv;
	}
	if (n == 0) {
		for (int i = 0; i < 3; i++) {
			pa[i] = 0.0f;
			pb[i] = mid[i];
		}
		return sqrtf(vv);
	}
	for (int i = 0; i < 3; i++) {
		pa[i] = pb[i] = 0.0f;
		for (int k = 0; k < n; k++) {
			pa[i] += lam[k] * sx[k].a[i];
			pb[i] += lam[k] * sx[k].b[i];
		}
	}
	return inside ? 0.0f : sqrtf(vv);
}

// GJKWitness::UpdateWorldMatrices (0x801B8848) and RecalcDistance
// (0x801B8AB0) with the car at (R, p). Nothing is worked out again unless
// the car has moved more than a millimetre along some axis since the last
// time it was, so a turn on the spot keeps the old answer, as retail's does.
static void witRecalc(const CarSim* s, CarSimWitness* wt, const float R[3][3], const float p[3]) {
	if (fabsf(p[0] - wt->lastPos[0]) <= TP_MOVED && fabsf(p[1] - wt->lastPos[1]) <= TP_MOVED &&
	    fabsf(p[2] - wt->lastPos[2]) <= TP_MOVED)
		return;
	for (int k = 0; k < 3; k++) wt->lastPos[k] = p[k];
	TpGjk g;
	g.s = s;
	g.box = wt->box != 0;
	g.sb = NULL;
	memcpy(g.R, R, sizeof(g.R));
	g.pn = tpClip(&wt->poly, wt->wnv, p, g.poly);
	float pa[3], pb[3];
	if (g.pn == 0) {
		wt->dist = TP_CLIP;
		for (int k = 0; k < 3; k++) pa[k] = pb[k] = 0.0f;
	} else {
		wt->dist = tpGjk(&g, pa, pb);
	}
	for (int k = 0; k < 3; k++) {
		wt->pa[k] = pa[k] + p[k];
		wt->pb[k] = pb[k] + p[k];
	}
	wt->colliding = wt->dist <= TP_COLLIDING;
	wt->deep = wt->dist <= TP_DEEP;
}

// GJKWafer::Init's face centroid (+0x1D0), the weights its constants give:
// 0.33 / 0.33 / 0.34 on a triangle, a quarter each on a quad.
static void tpWaferCentroid(const CarSimWitness* wt, float c[3]) {
	const float (*v)[3] = wt->poly.v;
	for (int i = 0; i < 3; i++)
		c[i] = wt->wnv == 3 ? 0.33f * (v[0][i] + v[1][i]) + 0.34f * v[2][i]
		                    : 0.25f * (v[0][i] + v[1][i] + v[2][i] + v[3][i]);
}

// FindWaferNormal (0x801B1628), the wafer's normal toward the car. A
// single-sided polygon (group 0) has its own face's whichever side the car is
// on; any other's points to the side the car's box started the step on.
static void tpWaferNormal(const CarSim* s, const CarSimWitness* wt, float n[3]) {
	for (int k = 0; k < 3; k++) n[k] = wt->poly.n[k];
	if (wt->poly.group == 0) return;
	float c[3], d[3];
	tpWaferCentroid(wt, c);
	for (int k = 0; k < 3; k++) d[k] = s->mvCentre[k] - c[k];
	if (dot3(d, n) < 0.0f)
		for (int k = 0; k < 3; k++) n[k] = -n[k];
}

// Contact::Crowbar (0x801B18E8): the car as it was `t` back, pushed out along
// the wafer's normal in growing steps -- 5 mm, then 10 mm more a step up to
// 0.1 m a step, 1.55 m by the twentieth -- until the pair is no longer deep,
// the push's height held to 0.15 m either way. A car whose movement is doused
// is not moved (IsMoveable). The push is kept in `off`.
static void tpCrowbar(const CarSim* s, CarSimWitness* wt, float t, float off[3]) {
	float R[3][3], p0[3], n[3];
	tpInterpolate(s, t, R, p0);
	tpWaferNormal(s, wt, n);   // toward the car; the push from car to wafer is -n
	float total = 0.0f, step = 0.005f;
	int i = 0;
	do {
		total += step;
		if (step < 0.1f) step += 0.01f;
		for (int k = 0; k < 3; k++) off[k] = -n[k] * total;
		off[2] = clampf(off[2], -TP_CROWBAR_Z, TP_CROWBAR_Z);
		float p[3];
		for (int k = 0; k < 3; k++) p[k] = s->dousing != 2 ? p0[k] - off[k] : p0[k];
		witRecalc(s, wt, R, p);
	} while (wt->colliding && wt->deep && ++i < TP_CROWBAR_PASSES);
}

// Contact::FindContactTime (0x801B13EC): how far back from the end of the
// step the car first touched, by eight halvings of the step -- only for a
// contact more than a hundredth of a millimetre deep; a shallower one is
// resolved where it is. The answer is the probe that only touched, else the
// nearest one that was clear; a contact that never came apart is crowbarred
// out at the last probe instead, and its push left in `off`.
static float tpContactTime(const CarSim* s, CarSimWitness* wt, float off[3]) {
	off[0] = off[1] = off[2] = 0.0f;
	if (!wt->deep) return 0.0f;
	float lo = 0.0f, hi = s->mvDt, best = -1.0f, t = 0.0f;
	for (int i = 0; i < TP_PROBES; i++) {
		t = lo + (hi - lo) * 0.5f;
		float R[3][3], p[3];
		tpInterpolate(s, t, R, p);
		witRecalc(s, wt, R, p);
		if (wt->deep) {
			lo = t;
		} else if (wt->colliding) {
			return t;
		} else {
			hi = t;
			if (best < 0.0f || t < best) best = t;
		}
	}
	if (best > 0.0f) return best;
	tpCrowbar(s, wt, t, off);
	return t;
}

// Contact::Resolve (0x801B1E10), the car against a wafer: backed up `t`
// (Backup 0x801AEF50: the pose and the linear momentum; the angular momentum
// is left as it is), pushed out by the crowbar's `off`, given Contact::
// CollisionImpulse at the middle of the witness's last closest points -- from
// the last distance worked out, which need not be the pose at `t`, as in
// retail -- and moved on again over the `t` it gave back (CarCollisionBody::
// Move 0x801AFFC4).
static void tpResolve(CarSim* s, const CarSimInput* in, CarSimWitness* wt, float t,
		const float off[3]) {
	float R[3][3], p[3];
	tpInterpolate(s, t, R, p);
	const float f = tpBackFraction(s, t);
	memcpy(s->rot, R, sizeof(R));
	for (int k = 0; k < 3; k++) {
		s->rbPos[k] = p[k];
		s->P[k] = (s->mvVel[k] + (s->rbVel[k] - s->mvVel[k]) * f) * s->mass;
		if (s->dousing != 2) s->rbPos[k] -= off[k];
	}
	rbVelocities(s);

	float n[3], r[3];
	tpWaferNormal(s, wt, n);
	for (int k = 0; k < 3; k++) r[k] = 0.5f * (wt->pa[k] + wt->pb[k]) - s->rbPos[k];
	// Contact::GetBounciness / GetFriction take the car's own against a track
	// polygon (CarCollisionBody 0x801B0690 / 0x801B0590): half the bounce and
	// the friction squared against a wall, the friction growing with the
	// wheels off the ground against the ground.
	const float nz = fabsf(n[2]);
	const bool ground = nz > TP_GROUNDISH;
	float bounce = RETAIL_GROUND_BOUNCE, friction = RETAIL_GROUND_FRICTION;
	if (nz < TP_WALLISH) {
		bounce = RETAIL_WALL_BOUNCE;
		friction = RETAIL_WALL_FRICTION;
	} else if (ground) {
		friction *= (float)(CARPHYS_WHEELS - s->wheelsDown) * 0.17f + 1.0f;
		if (friction > 1.0f) friction = 1.0f;
	}
	s->contacts++;
	if (wt->poly.flags & 0x02) s->yawHitAt = s->clock;   // a wall polygon
	if (!ground) {
		const float at[2] = { s->rbPos[0] + r[0], s->rbPos[1] + r[1] };
		recordTouch(s, at, CARSIM_TOUCH_WALL);
	}
	const float hit = contactImpulse(s, n, r, bounce, friction, ground);
	if (hit > s->lastImpact) s->lastImpact = hit;
	wt->hitAt = s->clock;
	rbVelocities(s);
	if (t >= 1e-6f) {
		beginMove(s, t);
		moverMove(s, in, t);
	}
}

// TestForContact (0x801AD7C0) over the car's witnesses at the end of the
// step: each one closer than a millimetre is a Contact, its time found as it
// is made, kept in AddSorted's order -- latest backup first, which is the
// earliest touch, equals in the order they came. `again` is the re-test
// after a resolution, which skips a witness resolved twice already.
static int tpTestAll(CarSim* s, u8* order, float* tOf, float (*offOf)[3], bool again) {
	int nc = 0;
	for (int i = 0; i < s->witN; i++) {
		CarSimWitness* wt = &s->wit[i];
		if (again && wt->resolves >= TP_MAX_PER_WITNESS) continue;
		witRecalc(s, wt, s->rot, s->rbPos);
		if (!wt->colliding) continue;
		if (fabsf(wt->poly.n[2]) > TP_GROUNDISH) s->touching = true;
		float off[3];
		const float t = tpContactTime(s, wt, off);
		int at = nc;
		for (int j = 0; j < nc; j++)
			if (t > tOf[j]) {
				at = j;
				break;
			}
		for (int j = nc; j > at; j--) {
			order[j] = order[j - 1];
			tOf[j] = tOf[j - 1];
			for (int k = 0; k < 3; k++) offOf[j][k] = offOf[j - 1][k];
		}
		order[at] = (u8)i;
		tOf[at] = t;
		for (int k = 0; k < 3; k++) offOf[at][k] = off[k];
		nc++;
	}
	return nc;
}

// World::DoTimestepCollisions (0x80210838) for one car, after its move: the
// witnesses that lapsed dropped (HasExpired 0x801AD444: one stays while its
// Collision lives, 0.3 s past its last impulse, or while the boxes still
// meet), new ones made, every one tested, and the earliest contact resolved.
// The car is then somewhere new, so the other contacts are thrown away, the
// witnesses brought up to date and tested again, up to 20 resolutions. False
// when the world has no track polygons; the caller then has the planes and
// the footprint resolver.
static bool tpCollide(CarSim* s, const CarSimInput* in, const CarSimWorld* w) {
	if (!w || !w->trackPolys || !w->trackPoly) return false;
	float bb[4];
	tpCarBox(s, bb);
	int m = 0;
	for (int i = 0; i < s->witN; i++) {
		const CarSimWitness* wt = &s->wit[i];
		const bool live = wt->hitAt >= 0.0f && s->clock - wt->hitAt < TP_LIFETIME;
		if (!live && !tpBoxesMeet(bb, wt->poly.box)) continue;
		if (m != i) s->wit[m] = s->wit[i];
		m++;
	}
	s->witN = m;
	if (!tpCreate(s, in, w)) {
		s->witN = 0;
		return false;
	}

	s->touching = false;
	for (int i = 0; i < s->witN; i++) s->wit[i].resolves = 0;
	u8 order[CARSIM_WITNESS_MAX];
	float tOf[CARSIM_WITNESS_MAX], offOf[CARSIM_WITNESS_MAX][3];
	int nc = tpTestAll(s, order, tOf, offOf, false);
	for (int it = 0; nc > 0 && it < TP_MAX_RESOLVES; it++) {
		CarSimWitness* wt = &s->wit[order[0]];
		tpResolve(s, in, wt, tOf[0], offOf[0]);
		wt->resolves++;
		tpCreate(s, in, w);
		nc = tpTestAll(s, order, tOf, offOf, true);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Outputs

static void publish(CarSim* s) {
	float c[3];
	toWorld(s->rot, s->cg, c);
	for (int k = 0; k < 3; k++) s->origin[k] = s->rbPos[k] - c[k];
	s->pos[0] = s->rbPos[0];
	s->pos[1] = s->rbPos[1];
	s->pos[2] = s->rbPos[2] - s->hRest;
	s->yaw = atan2f(s->rot[0][1], s->rot[0][0]);
	s->pitch = asinf(clampf(s->rot[0][2], -1.0f, 1.0f));
	s->roll = atan2f(s->rot[1][2], s->rot[2][2]);
	for (int k = 0; k < 3; k++) s->vel[k] = s->rbVel[k];
	s->speed = sqrtf(s->rbVel[0] * s->rbVel[0] + s->rbVel[1] * s->rbVel[1]);
	s->gear = s->dtGear == GEAR_R ? -1 : s->dtGear - 1;
	s->rpm = s->rpmSmooth;
	s->shifting = s->dtShifting != 0;
	s->onGround = s->wheelsDown > 0;
}

void carsim_publish(CarSim* s) {
	publish(s);
}

// ---------------------------------------------------------------------------
// The HUD's copies, for a record whose own blocks were missing

static void perfFromCpi(const CarSim* s, CarPerf* p) {
	memset(p, 0, sizeof(*p));
	float split = cf(s, CPI_DTI + DTI_SPLIT);
	p->finalDrive = cf(s, CPI_DTI + (split >= 0.5f ? DTI_REAR_FINAL : DTI_FRONT_FINAL));
	p->numGears = numGears(s);
	p->reverseRatio = dtRatio(s, GEAR_R);
	for (int g = 0; g < CARPERF_MAX_GEARS && g < p->numGears; g++)
		p->gearRatio[g] = dtRatio(s, GEAR_FIRST + g);
	p->idleRpm = cf(s, CPI_EI + EI_IDLE);
	p->redlineRpm = cf(s, CPI_EI + EI_REDLINE);
	p->limiterRpm = cf(s, CPI_EI + EI_LIMITER);
	for (int k = 0; k < CARPERF_TORQUE_POINTS; k++) p->torque[k] = cf(s, CPI_EI + EI_TORQUE + 4 * k);
	p->valid = true;
}

static void chassisFromCpi(const CarSim* s, CarChassis* c) {
	memset(c, 0, sizeof(*c));
	c->mass = cf(s, CPI_MASS) * 1000.0f;
	c->length = cf(s, CPI_DIMS);
	c->width = cf(s, CPI_DIMS + 4);
	c->height = cf(s, CPI_DIMS + 8);
	c->inertia[0] = cf(s, CPI_IXX) * 1000.0f;
	c->inertia[1] = cf(s, CPI_IYY) * 1000.0f;
	c->inertia[2] = cf(s, CPI_IZZ) * 1000.0f;
	float r = 0.0f;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		const float* wi = cpiAt(s, CPI_WHEEL + CPI_WHEEL_STRIDE * i);
		for (int k = 0; k < 3; k++) c->wheel[i].pos[k] = wi[k];
		c->wheel[i].radius = wi[WI_RADIUS / 4];
		c->wheel[i].tyreWidth = wi[WI_WIDTH / 4];
		r += wi[WI_RADIUS / 4];
	}
	c->wheelbase = c->wheel[0].pos[0] - c->wheel[3].pos[0];
	c->trackFront = c->wheel[0].pos[1] - c->wheel[1].pos[1];
	c->trackRear = c->wheel[3].pos[1] - c->wheel[2].pos[1];
	c->cgToFront = c->wheel[0].pos[0];
	c->cgToRear = -c->wheel[3].pos[0];
	c->wheelRadius = r * 0.25f;
	c->dragCoef = cf(s, CPI_EI + EI_INERTIA);
	c->valid = true;
}

// A block this simulation can run: a mass, an inertia, wheels and a gearbox.
static bool cpiUsable(const CarSim* s) {
	int ng = numGears(s);
	if (!(cf(s, CPI_MASS) > 0.1f) || ng < 1 || ng > CARPERF_MAX_GEARS) return false;
	if (!(cf(s, CPI_IXX) > 0.0f) || !(cf(s, CPI_IYY) > 0.0f) || !(cf(s, CPI_IZZ) > 0.0f)) return false;
	if (!(cf(s, CPI_EI + EI_INERTIA) > 0.0f) || !(cf(s, CPI_EI + EI_LIMITER) > cf(s, CPI_EI + EI_IDLE)))
		return false;
	for (int i = 0; i < CARPHYS_WHEELS; i++)
		if (!(cf(s, CPI_WHEEL + CPI_WHEEL_STRIDE * i + WI_RADIUS) > 0.05f)) return false;
	for (int t = 0; t < 2; t++)
		if (!(cf(s, CPI_TIRE + CPI_TIRE_STRIDE * t) > 0.0f) || !(cf(s, CPI_TIRE + CPI_TIRE_STRIDE * t + 4) > 0.0f))
			return false;
	return true;
}

// ---------------------------------------------------------------------------
// API

// RidePhysicsInfo::GetTheoreticalTopSpeed over carsim_accel_table (the
// Table at +0x550): up from 0 in 3 mph steps (0x80332C8C) to the first where
// the acceleration is under v^2 * Cd / mass, capped at the governor (mph,
// 0x80332C90 the m/s of one).
static float theoreticalTop(const CarSim* s) {
	float acc[CARSIM_ACCEL_N];
	const float vmax = carsim_accel_table(s, acc);
	const float cd = cf(s, CPI_AERO), mass = cf(s, CPI_MASS);
	float v = 0.0f;
	while (v <= vmax) {
		const float a = vmax > 0.0f ? tableGet(acc, CARSIM_ACCEL_N, 0.0f, vmax, v) : 0.0f;
		if (mass > 0.0f && a < v * v * cd / mass) break;
		v += 1.34108f;
	}
	const float gov = cf(s, CPI_GOVERNOR);
	if (gov > 0.0f && gov * 0.447027f < v) v = gov * 0.447027f;
	return v;
}

void carsim_init(CarSim* s, const CarPerf* perf, const CarChassis* chassis,
	const CarPhysInfo* phys, const float pos[3], float yaw)
{
	memset(s, 0, sizeof(*s));
	s->bfGear = GEAR_N;
	s->bfArmed = 1;
	if (phys && phys->valid) {
		memcpy(s->cpi, phys->raw, CARPHYS_CPI_SIZE);
		s->boostFlag = phys->boostFlag;
	}
	if (!phys || !phys->valid || !cpiUsable(s)) {
		memcpy(s->cpi, k350zCpi, CARPHYS_CPI_SIZE);
		s->boostFlag = 0;
	} else if (phys->hull && phys->hullN >= 4 && phys->hullN <= CARSIM_HULL_MAX) {
		s->hull = phys->hull;
		s->hullN = phys->hullN;
	}
	if (perf && perf->valid) s->perf = *perf;
	else perfFromCpi(s, &s->perf);
	if (chassis && chassis->valid) s->chassis = *chassis;
	else chassisFromCpi(s, &s->chassis);

	for (int k = 0; k < 3; k++) s->cg[k] = cf(s, CPI_CG + 4 * k);
	tpVolumes(s);

	// RigidBody / CalculateInertiaTensor
	s->mass = cf(s, CPI_MASS);
	s->invMass = 1.0f / s->mass;
	s->Iinv[0] = 1.0f / cf(s, CPI_IXX);
	s->Iinv[1] = 1.0f / cf(s, CPI_IYY);
	s->Iinv[2] = 1.0f / cf(s, CPI_IZZ);
	float zs = cf(s, CPI_IZZ_SCALE);
	s->IinvZ[0] = s->Iinv[0];
	s->IinvZ[1] = s->Iinv[1];
	s->IinvZ[2] = 1.0f / (cf(s, CPI_IZZ) * (zs > 0.0f ? zs : 1.0f));
	s->zdamp = 1;
	s->flag13c = s->flag140 = 1;
	s->g = GRAVITY;
	s->govBoost = 1.0f;
	s->aeroScale = s->aeroSurf = 1.0f;   // the Aerodynamics ctor's [10], and [11]
	s->mlMu = carsim_mellow_mu(false, 0);
	s->controlMode = 1;   // Default__14PlayerSettings: Stability Control Off

	// Wheels and suspensions: the front two share info [0], the rear [1].
	float rSum = 0.0f, zSum = 0.0f;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* wh = &s->wheel[i];
		const float* wi = cpiAt(s, CPI_WHEEL + CPI_WHEEL_STRIDE * i);
		for (int k = 0; k < 3; k++) wh->pos[k] = wi[k];
		wh->radius = wi[WI_RADIUS / 4];
		wh->tire = cpiAt(s, CPI_TIRE + (i < 2 ? 0 : CPI_TIRE_STRIDE));
		wh->dir = 1;
		wh->grip1 = wh->grip2 = wh->grip3 = 1.0f;
		wh->longScale = wh->latScale = 1.0f;
		CarSimSusp* su = &s->susp[i];
		su->info = cpiAt(s, CPI_SUSP + (i < 2 ? 0 : CPI_SUSP_STRIDE));
		su->partner = i ^ 1;
		for (int k = 0; k < 3; k++) su->attach[k] = wh->pos[k] - s->cg[k];
		rSum += wh->radius;
		zSum += su->attach[2];
	}
	// The centre of gravity's height over the ground with the springs at
	// their static length.
	s->hRest = (rSum - zSum) * 0.25f;

	s->split = cf(s, CPI_DTI + DTI_SPLIT);
	calcShiftPoints(s);
	carsim_set_shift_tuning(s, 3, 1.0f, 1.0f, NULL, NULL);   // the DriveTrain ctor's
	s->theoTop = theoreticalTop(s);
	s->heat = -1.0f;
	// The Engine ctor (0x801B2DF8): +0x98 the largest of EngineInfo +0x34's
	// nine turbo values, from 0; the pressure at rest.
	{
		const float* ei = cpiAt(s, CPI_EI);
		s->turboMax = 0.0f;
		for (int k = 0; k < 9; k++)
			if (ei[13 + k] > s->turboMax) s->turboMax = ei[13 + k];
		s->turboBoost = s->turboFrom = s->turboT = s->turboLast = 0.0f;
		s->turboState = 0;
	}
	s->rng = 0x2222u;
	s->ebrakeRecovery = 1.0f;
	s->driverGear = -1;
	s->clock = 10.0f;   // the world timer does not start at zero
	s->yawHitAt = s->clock;   // CarCollisionBody's constructor stamps +0x134
	windowInit(&s->winA, 33, 0.55f);
	windowInit(&s->winB, 9, 0.15f);

	// Not used by the model; the HUD and the debug overlay want to know what
	// the gearing would allow, and the governor caps it.
	float fin = cf(s, CPI_DTI + (s->split >= 0.5f ? DTI_REAR_FINAL : DTI_FRONT_FINAL));
	float top = dtRatio(s, numGears(s) + 1) * fin;
	float rr = s->wheel[s->split >= 0.5f ? 2 : 0].radius;
	s->topSpeed = top > 0.0f ? cf(s, CPI_EI + EI_REDLINE) * RADS_PER_RPM * rr / top : 0.0f;
	float gov = cf(s, CPI_GOVERNOR);
	if (gov > 0.0f) {
		float g = (gov + RB_GOVERNOR_SLACK) * 1.61f / 3.6f;
		if (s->topSpeed <= 0.0f || g < s->topSpeed) s->topSpeed = g;
	}

	carsim_place(s, NULL, pos, yaw);
}

void carsim_set_transmission(CarSim* s, int manual) {
	s->transmission = manual ? 1 : 0;
	s->driverGear = manual ? s->dtGear : -1;   // Car+0x444, the gear it is in
}

void carsim_set_drag(CarSim* s, bool on) {
	s->drag = on;
	s->totaled = false;
	s->shiftBonus = s->bonusAt = 0.0f;
	s->shifts = s->perfectShifts = 0;
	if (!on) s->heat = -1.0f;
}

bool carsim_totaled(const CarSim* s) {
	return s->totaled;
}

void carsim_set_drift(CarSim* s, bool on, int skill) {
	s->drift = on;
	s->driftSkill = skill < 0 ? 0 : (skill > 2 ? 2 : skill);
	if (!on) {
		// Wheel+0x184 is DriftStabilizer's alone; a car kept after the race
		// goes back to the constructor's 1.
		for (int i = 0; i < CARPHYS_WHEELS; i++) s->wheel[i].grip3 = 1.0f;
		s->yawAccel = 0.0f;
	}
}

void carsim_set_shift_tuning(CarSim* s, int carClass, float rangeMul, float clutchMul,
	const float lo[3], const float hi[3])
{
	s->carClass = carClass < 0 ? 0 : (carClass > 3 ? 3 : carClass);
	s->shiftRangeMul = rangeMul;
	s->clutchMul = clutchMul;
	for (int k = 0; k < 3; k++) {
		s->modLo[k] = lo ? lo[k] : -100.0f;
		s->modHi[k] = hi ? hi[k] : 0.0f;
	}
}

void carsim_set_heat(CarSim* s, bool accumulating) {
	s->heat = accumulating ? fmaxf(0.0f, s->heat) : -1.0f;
}

void carsim_set_start_line(CarSim* s, bool on) {
	s->startLine = on;
	if (!on) setLowGear(s);
}

void carsim_hold_xy(CarSim* s, const float xy[2]) {
	s->rbPos[0] = xy[0];
	s->rbPos[1] = xy[1];
	s->P[0] = s->P[1] = 0.0f;
	s->rbVel[0] = s->rbVel[1] = 0.0f;
	publish(s);
}

void carsim_net_pull(CarSim* s, const float pos[3], const float fwd[3],
		const float up[3], const float vel[3], const float w[3], float k, float kv) {
	k = clampf(k, 0.0f, 1.0f);
	kv = clampf(kv, 0.0f, 1.0f);
	float f[3], u[3], l[3];
	for (int i = 0; i < 3; i++) {
		s->rbPos[i] += k * (pos[i] - s->rbPos[i]);
		f[i] = s->rot[0][i] + k * (fwd[i] - s->rot[0][i]);
		u[i] = s->rot[2][i] + k * (up[i] - s->rot[2][i]);
	}
	// Rows forward, left, up: left = up x forward, then Gram-Schmidt.
	cross3(u, f, l);
	if (len3(f) > 1e-3f && len3(l) > 1e-3f) {
		for (int i = 0; i < 3; i++) {
			s->rot[0][i] = f[i];
			s->rot[1][i] = l[i];
		}
		orthonormalise(s->rot);
	}
	// P = m v; L = I w, the world inertia the step's own (rbInvInertiaWorld).
	const float* inv = s->zdamp ? s->IinvZ : s->Iinv;
	float wv[3], c[3];
	for (int i = 0; i < 3; i++) {
		const float v = s->rbVel[i] + kv * (vel[i] - s->rbVel[i]);
		s->P[i] = v * s->mass;
		wv[i] = s->w[i] + kv * (w[i] - s->w[i]);
	}
	toCar(s->rot, wv, c);
	for (int i = 0; i < 3; i++)
		c[i] = inv[i] > 1e-9f ? c[i] / inv[i] : 0.0f;
	toWorld(s->rot, c, s->L);
	computeAux(s);
	publish(s);
}

void carsim_set_dyno_drag(CarSim* s, float mps2) {
	s->dynoDrag = mps2 > 0.0f ? mps2 : 0.0f;
}

void carsim_set_dyno(CarSim* s, bool on) {
	s->dyno = on;
	s->dynoDrag = 0.0f;
	s->dynoXY[0] = s->rbPos[0];
	s->dynoXY[1] = s->rbPos[1];
	s->dynoZ = s->rbPos[2];
	memcpy(s->dynoRot, s->rot, sizeof(s->rot));
	s->P[0] = s->P[1] = s->P[2] = 0.0f;
	s->L[0] = s->L[1] = s->L[2] = 0.0f;
	for (int i = 0; i < CARPHYS_WHEELS; i++)
		s->wheel[i].w = 0.0f;
	if (on) {
		// StartDynoRun: car +0x444 = 1 (neutral), the rpm fields cleared.
		s->dtGear = GEAR_N;
		s->desired = GEAR_N;
		s->dtShifting = 0;
	}
	computeAux(s);
	publish(s);
}

float carsim_engine_heat(const CarSim* s) {
	return clampf(s->heat, 0.0f, 1.0f);
}

bool carsim_engine_blown(const CarSim* s) {
	return s->heat == 2.0f;
}

float carsim_turbo_boost(const CarSim* s, bool* hasTurbo) {
	if (hasTurbo) *hasTurbo = s->turboMax > 0.0f;
	return s->turboBoost;
}

float carsim_shift_potential(const CarSim* s, float rpm) {
	return shiftBonusAt(s, rpm, s->dtGear, 0);
}

float carsim_shift_bonus(const CarSim* s) {
	return s->shiftBonus;
}

float carsim_optimal_shift_rpm(const CarSim* s) {
	return carsim_optimal_shift_rpm_gear(s, s->dtGear);
}

float carsim_shift_potential_gear(const CarSim* s, float rpm, int g) {
	return shiftBonusAt(s, rpm, g, 0);
}

float carsim_optimal_shift_rpm_gear(const CarSim* s, int g) {
	return g >= 0 && g < 10 ? s->up[g] : 0.0f;
}

float carsim_perfect_shift_range(const CarSim* s, int g) {
	return g >= 0 && g < 9 ? kPerfectShiftMaxRpmDiff[s->carClass & 3][g] * s->shiftRangeMul : 0.0f;
}

void carsim_retune(CarSim* s) {
	float v[3];
	for (int i = 0; i < 3; i++) v[i] = s->P[i] * s->invMass;
	s->mass = cf(s, CPI_MASS);
	s->invMass = 1.0f / s->mass;
	s->Iinv[0] = 1.0f / cf(s, CPI_IXX);
	s->Iinv[1] = 1.0f / cf(s, CPI_IYY);
	s->Iinv[2] = 1.0f / cf(s, CPI_IZZ);
	float zs = cf(s, CPI_IZZ_SCALE);
	s->IinvZ[0] = s->Iinv[0];
	s->IinvZ[1] = s->Iinv[1];
	s->IinvZ[2] = 1.0f / (cf(s, CPI_IZZ) * (zs > 0.0f ? zs : 1.0f));
	for (int i = 0; i < 3; i++) s->P[i] = v[i] * s->mass;
	s->split = cf(s, CPI_DTI + DTI_SPLIT);
	calcShiftPoints(s);
	s->theoTop = theoreticalTop(s);
}

void carsim_place(CarSim* s, const CarSimWorld* w, const float pos[3], float yaw) {
	float gz = pos[2];
	float n[3] = { 0.0f, 0.0f, 1.0f };

	// Dropped onto the surface under the car's middle: a spawn point is a
	// place to be put, and the first frame re-probes all eight points anyway.
	//
	// The ceiling is 1.5 m and not the 8 m it used to be, because the probe
	// answers with the HIGHEST surface under the ceiling and counts a face as
	// ground on the size of its normal's z without looking at the sign -- so
	// the underside of a deck carried over the street is ground too. At the
	// career respawn point (-992.0, 818.0, 39.2) the street in C32 is at
	// z 38.8 and the underside of the overpass in C12 is at z 42.8: both are
	// under a 47.2 ceiling, the higher one wins, and the car is parked in
	// mid-air under the overpass.
	//
	// 1.5 m is chosen against what a placement point actually is. Both callers
	// hand this function a position that is already on or just above its own
	// surface -- the career path passes the authored respawn z, which sits
	// 0.4 m over its street, and the fallback path has just been clamped to a
	// surface by world_ground_z(). Neither needs to look further up than the
	// car is tall, and the clearance under a road deck is several metres, so
	// this reaches the surface the point belongs to and nothing above it.
	if (w && w->groundProbe) {
		float q[2] = { pos[0], pos[1] };
		float z[1], nn[3];
		bool hit[1];
		if (w->groundProbe(w->ctx, q, 1, pos[2] + 1.5f, z, hit, nn, NULL) > 0 && hit[0]) {
			gz = z[0];
			if (nn[2] > 0.001f) { n[0] = nn[0]; n[1] = nn[1]; n[2] = nn[2]; }
		}
	}

	// Square to the surface (PORT): `yaw` laid onto the plane the probe
	// answered with, so a car put down on a hill stands on all four wheels
	// rather than on two with the other two in the air.
	float c = cosf(yaw), sn = sinf(yaw);
	float f[3] = { c, sn, 0.0f };
	float d = dot3(f, n);
	for (int k = 0; k < 3; k++) f[k] -= d * n[k];
	float fl = len3(f);
	if (fl < 1e-4f) {
		f[0] = c; f[1] = sn; f[2] = 0.0f;
		n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f;
		fl = 1.0f;
	}
	for (int k = 0; k < 3; k++) {
		s->rot[0][k] = f[k] / fl;
		s->rot[2][k] = n[k];
	}
	cross3(s->rot[2], s->rot[0], s->rot[1]);
	s->rbPos[0] = pos[0];
	s->rbPos[1] = pos[1];
	s->rbPos[2] = gz + s->hRest / n[2];
	for (int k = 0; k < 3; k++) s->P[k] = s->L[k] = s->F[k] = s->T[k] = 0.0f;
	s->damping = 0.0f;

	// Every slot starts on the surface the car was put on.
	for (int k = 0; k < CARPHYS_WHEELS * 2; k++) {
		CarSimPlane* pl = &s->plane[k];
		pl->p[0] = pos[0]; pl->p[1] = pos[1]; pl->p[2] = gz;
		pl->n[0] = n[0]; pl->n[1] = n[1]; pl->n[2] = n[2];
		pl->valid = true;
	}
	for (int j = 0; j < CARPHYS_WHEELS; j++) s->corner[j] = j;

	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* wh = &s->wheel[i];
		wh->angle = wh->w = 0.0f;
		wh->g = 0.0f;
		wh->locked = wh->sliding = 0;
		wh->dir = 1;
		wh->onGround = 1;
		s->susp[i].x = s->susp[i].xdot = 0.0f;
	}

	// InitializeStateFromCar: the engine at idle, the box engaged in first.
	s->engW = cf(s, CPI_EI + EI_IDLE) * RADS_PER_RPM;
	s->limiter = 0;
	s->clutchState = 3;
	s->dtGear = s->desired = GEAR_FIRST;
	s->clutch = 1.0f;
	s->dtShifting = 0;
	s->diffW = 0.0f;
	s->rpmSlip = 0.0f;
	s->driverGear = s->transmission == 1 ? GEAR_FIRST : -1;   // a manual box keeps first
	s->reverseFlag = 0;
	s->thr = s->brakeIn = s->ebrakeIn = 0.0f;
	s->drvThr = 0.0f;
	s->launchTicks = 0;
	s->engThr = s->aeroThr = 0.0f;

	s->dousing = 0;
	s->roofTimer = 0.0f;
	s->resetRequest = false;
	s->touching = false;
	s->lastImpact = 0.0f;
	// A car put down somewhere new has no track polygons yet, and no move
	// to back up through.
	s->tpN = -1;
	s->witN = 0;
	s->mvDt = 0.0f;
	for (int k = 0; k < 4; k++) {
		s->touchStart[k] = s->touchLast[k] = -1.0f;
		s->touchX[k] = 0.0f;
		s->touchWith[k] = CARSIM_TOUCH_WALL;
	}
	s->vfwd = s->speedAbs = 0.0f;
	s->rpmSmooth = cf(s, CPI_EI + EI_IDLE);
	s->wheelsDown = 4;
	s->aeroDown = 1.0f;
	// CarState::ResetCar fills the bottle only while it has no size
	// (0x801AA1A8): the first placing after carsim_init(), which is
	// BuildCarPhysicsInfo's ResetNitrous. A reset does not top it up.
	if (!nosTickSet(s->nosCap)) nosReset(s);

	s->steerU16 = 0;
	s->steerLastIn = s->steerLastMax = 0.0f;
	s->slideState = 0;
	s->steerFL = s->steerFR = 0.0f;
	s->mlSteerVis = 0.0f;   // CarState::ResetCar: +0x3DE = 0
	windowInit(&s->winA, 33, 0.55f);
	windowInit(&s->winB, 9, 0.15f);

	// The ctor's Calibrate and ComputeAuxiliaries: the force on the body is
	// gravity alone until the first substep resolves the rest.
	calibrate(s);
	s->ugly = 0.0f;
	if (s->mellow) {
		// The Mellovator as InitializeStateFromCar leaves it (see
		// carsim_set_mellow), at idle in first.
		for (int i = 0; i < CARPHYS_WHEELS; i++) {
			s->mlFc[i][0] = s->mlFc[i][1] = 0.0f;
			s->mlPen[i] = 0.0f;
			s->wheel[i].s = s->wheel[i].slip = 0.0f;
		}
		s->mlRpm = s->rpmSmooth;
		s->mlBrake = 0.0f;
		s->mlSteerVis = 0.0f;
		mlComputeAux(s);
		mlComputeAux(s);
		mlUpdateCar(s, 0.0f);
	} else {
		systemAux(s);
		updateCar(s, 0.0f);
	}
	publish(s);
}

void carsim_set_mellow(CarSim* s, bool on, float rating, float mu) {
	if ((s->mellow != 0) == on) return;

	// Either way a new mover, with a new RigidBody whose SetState takes the
	// CarState's pose and both velocities: the momenta carry over as they
	// are, and the body's own integrator state starts again.
	s->damping = 0.0f;
	s->yawClamp = 0.0f;
	s->aeroScale = s->aeroSurf = 1.0f;
	s->dousing = 0;
	s->roofTimer = 0.0f;
	s->governor = cf(s, CPI_GOVERNOR);   // RigidBody +0x1EC
	s->govBoost = 1.0f;
	s->ugly = 0.0f;   // the ctor's UglyTimestepHack

	if (on) {
		// MellowMover's ctor and InitializeStateFromCar (0x801BB200,
		// 0x801BB648). SetFrictionRating takes the measure over 10 into
		// [0.25, 0.5]. Mellovator::SetState keeps the wheel spins and zeroes
		// each spring and brake lock; the gear and the revs are the
		// CarState's, which dtGear and rpmSmooth are.
		s->mellow = 1;
		s->mlRating = clampf(rating, 0.0f, 10.0f) / 10.0f * 0.25f + 0.25f;
		s->mlMu = mu;
		s->mlRpm = s->rpmSmooth;
		s->mlBrake = s->brakeIn;
		s->mlSteerVis = (float)s->steerU16 * (360.0f / 65536.0f);
		if (s->mlSteerVis >= 180.0f) s->mlSteerVis -= 360.0f;
		for (int i = 0; i < CARPHYS_WHEELS; i++) {
			CarSimWheel* wh = &s->wheel[i];
			s->susp[i].x = s->susp[i].xdot = 0.0f;
			wh->locked = wh->sliding = 0;
			wh->s = wh->slip = 0.0f;
			s->mlFc[i][0] = s->mlFc[i][1] = 0.0f;
			s->mlPen[i] = 0.0f;
		}
		calibrate(s);
		// InitializeStateFromCar runs ComputeAuxiliaries twice: with the
		// timestep at 0 the first pass's spring speeds are nonsense, and
		// the second reads the springs as still.
		mlComputeAux(s);
		mlComputeAux(s);
		return;
	}

	// PhysicsMover::InitializeStateFromCar (0x801C0718): each spring at rest,
	// each wheel at the CarState's angle and spin, the engine at the
	// CarState's revs and the box engaged in the CarState's gear. The rest is
	// the components' constructors.
	s->mellow = 0;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* wh = &s->wheel[i];
		s->susp[i].x = s->susp[i].xdot = 0.0f;
		wh->g = 0.0f;
		wh->locked = wh->sliding = 0;
		wh->dir = wh->w > 1e-5f ? 0 : (wh->w < -1e-5f ? 2 : 1);
	}
	s->engW = s->rpmSmooth * RADS_PER_RPM;
	s->limiter = 0;
	s->clutchState = 3;
	s->desired = s->dtGear;
	s->clutch = 1.0f;
	s->dtShifting = 0;
	s->diffW = 0.0f;
	s->rpmSlip = 0.0f;
	calibrate(s);
	systemAux(s);
}

void carsim_update(CarSim* s, const CarSimInput* in, const CarSimWorld* w, float dt) {
	if (dt <= 0.0f) return;
	if (dt > 0.10f) dt = 0.10f;   // a hitch must not become a teleport
	s->clock += dt;
	resetDampenMode(s, dt);

	// Where the box was drawn last frame, for the wall resolver.
	float from[3] = { s->origin[0], s->origin[1], s->pos[2] };

	probeGround(s, w, dt);

	// RealDriver and PlayerSteering, which run ahead of the physics. An AI
	// driver has already done its own steering: PsychoDriver::Drive writes
	// CarDriver +0x1C4 itself, so the player's lock and rate are skipped and
	// the angle goes straight to the Ackerman (see CarSimInput.steerIsAngle).
	driverInput(s, in, dt);
	if (in->steerIsAngle) s->steerU16 = in->steerAngleU16;
	else steering(s, in->steer, dt);

	// Car::Move's mover, then World::DoTimestepCollisions for the car against
	// the track's polygons: the step backed up and moved on again at each
	// contact (tpCollide). A world with no topology keeps the port's planes
	// and footprint resolver (PORT geometry).
	s->aiDriven = in->aiDriver;
	s->mvIn = *in;
	s->posed = false;
	s->prevWz = s->w[2];   // Car+0x530, the rate the step starts from
	beginMove(s, dt);
	moverMove(s, in, dt);
	s->lastImpact = 0.0f;
	if (!tpCollide(s, in, w)) {
		bodyGroundContact(s);
		wallContact(s, w, from);
	}
	rbVelocities(s);

	if (s->mellow) mlUpdateCar(s, dt);
	else updateCar(s, dt);

	// Car::FinishMovement (0x8021FB80): Car+0x118, the world angular
	// velocity's change over the step; its z is the drift steering's input.
	s->yawAccel = (s->w[2] - s->prevWz) / dt;

	// Car::UpdateSpinners (0x80223DA8), from Car::Move: each spinner's rate
	// closes 0.0035 / 0.0225 of the gap to its wheel's spin a second, and its
	// angle runs on at that rate. PORT: the angle is kept within a turn,
	// which the u16 it is drawn by never sees.
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		s->spinnerRate[i] += (s->wheel[i].w - s->spinnerRate[i]) * (0.0035f / 0.0225f) * dt;
		s->spinnerAngle[i] = fmodf(s->spinnerAngle[i] + s->spinnerRate[i] * dt, 6.2831855f);
	}
	publish(s);
}

void carsim_wheel_state(const CarSim* s, float spin[CARPHYS_WHEELS],
		float susp[CARPHYS_WHEELS], float* steer) {
	// CarState +0x2F8 the spin angle, +0x338 min(x, SuspensionInfo [7]) less
	// +0x3BC (the hydraulics' extension, #313), +0x3DE the steering as drawn.
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		spin[i] = s->wheel[i].angle;
		const float x = s->susp[i].x;
		const float cap = s->susp[i].info ? s->susp[i].info[7] : x;
		susp[i] = (x < cap ? x : cap) - s->hydExt[i];
	}
	*steer = s->mlSteerVis * 0.017453292f;
}

// ---------------------------------------------------------------------------
// Car against car (TASKS.md #223)
//
// Retail resolves this through the CollisionBody stack the track polygons go
// through above: every car owns a CarCollisionBody, MaybeCreateWitnesses
// pairs two whose boxes meet seen from above, the CollisionBodyWitness keeps
// the GJK distance between the two bodies' GetGJKPoly shapes, and at a
// millimetre or less the pair is a Contact -- FindContactTime backs both cars
// up to where they first touched (or Crowbar pushes them apart),
// FindContactPointAndNormal takes the closest points' middle and the line
// between them, and Contact::CollisionImpulse (0x801B24F4) resolves the pair.
// carsim_car_contact() is that, for one pair (docs/retail_track_collision.md
// sections 4-7; the GJK is tpGjk() with the other car in the wafer's place).
//
// PORT:
//   * No witness is kept between frames: the distance is worked out at every
//     call, and GetGJKPoly's shape is chosen at it, where retail chooses it
//     once, at the witness's making, and works the distance out again only
//     when a body has moved a millimetre.
//   * Each pair is its own list: resolved at most twice a call
//     (MAX_CONTACTS_PER_FRAME), in the order the caller offers the pairs,
//     where retail sorts every contact of the step into one list by time.
//   * After the impulse each car runs its mover again for the time it was
//     backed up (CarCollisionBody::Move) with its step's input; a car the
//     traffic mover put down (carsim_traffic_pose) moves on in a straight
//     line instead, that mover being traffic.c's.
//   * Retail then tests a moved car's witnesses again, the track polygons'
//     among them. Here a car pushed into a wall meets it at its next step,
//     crowbarred out if it never comes apart from it.
//
// The numbers, read out of the executable rather than tuned:
//
//   BOUNCE    GetBounciness__16CarCollisionBody (0x801B0690) returns 0.1 and
//             only halves it against a wallish track polygon, so a car-on-car
//             hit keeps the whole 0.1. Contact::GetBounciness picks the larger
//             of the two bodies' values; both are cars here, so it is 0.1.
//   FRICTION  GetFriction__16CarCollisionBody (0x801B0590) returns 0.3 and
//             only changes it against ground; car on car is the flat 0.3.
//   SPIN      The dampening mode's, as against a wall: 0.4 of the car's
//             inertia, and the whole of it only in mode 1. Contact::
//             CollisionImpulse (0x801B24F4) passes the impulse to
//             Collision::AddImpulse, whose CheckForWildCollision (0x801ABE94)
//             puts a car in mode 1 only when the impulse is 30 or more and the
//             hit is car on car (IsCarWorldObjectCollision is a stub that
//             returns 0) -- a crash, not the trading of paint. Until the port
//             read that, every car-car hit turned the cars at full inertia,
//             two and a half times retail, and the testers found the AI cars
//             "very light" and easy to spin out (2026-10-26).
//   MIN_J     MinimumContactImpulseFactor, 0.3, the same floor on the normal
//             impulse: two cars leaning on each other always come apart.
//   SCRAPE    MaxFrictionScrapeSpeed, so a slow nudge is not reversed.
//
// The impulse itself is Contact::CollisionImpulse's two-body form, which the
// static version in contactImpulse() above is the other half of: there the
// second body's inverse mass and inverse inertia are zero because a track
// polygon cannot move, and here both terms are real.
#define RETAIL_CARCAR_BOUNCE    0.10f
#define RETAIL_CARCAR_FRICTION  0.30f

// A car and a prop (carsim_prop_contact): how much of the overlap is taken out
// per frame when the pair was already overlapping as the step began
// (FindContactTime finds no time they were apart, retail's Contact::Crowbar
// case): the depth is split between them by inverse mass over a few frames
// rather than in one, which keeps a scrape from firing them apart. PORT: the
// share is the port's own.
#define CARCAR_SEPARATION 0.35f

// The car's plan rectangle: half length and half width of the body box, and
// the plan centre, which is the rigid body's position less the CG offset.
static void carPlanBox(const CarSim* s, float c[2], float ax[2][2], float half[2]) {
	float cg[3];
	toWorld(s->rot, s->cg, cg);
	c[0] = s->rbPos[0] - cg[0];
	c[1] = s->rbPos[1] - cg[1];
	// The body's forward and left axes, flattened and renormalised: a car on
	// a slope still occupies its full footprint seen from above.
	for (int i = 0; i < 2; i++) {
		float x = s->rot[i][0], y = s->rot[i][1];
		float l = sqrtf(x * x + y * y);
		if (l < 1e-4f) { x = i == 0 ? 1.0f : 0.0f; y = i == 0 ? 0.0f : 1.0f; l = 1.0f; }
		ax[i][0] = x / l;
		ax[i][1] = y / l;
	}
	half[0] = cf(s, CPI_DIMS) * 0.5f;
	half[1] = cf(s, CPI_DIMS + 4) * 0.5f;
}

// The separating-axis test over the four rectangle axes. Returns false when a
// gap is found; otherwise `n` is the axis of least overlap, pointing from `a`
// to `b`, and `depth` is that overlap.
static bool planBoxOverlap(const float ca[2], const float axa[2][2], const float ha[2],
		const float cb[2], const float axb[2][2], const float hb[2],
		float n[2], float* depth) {
	const float* axes[4] = { axa[0], axa[1], axb[0], axb[1] };
	float d[2] = { cb[0] - ca[0], cb[1] - ca[1] };
	float best = 1e30f;
	for (int i = 0; i < 4; i++) {
		const float* ax = axes[i];
		float ra = ha[0] * fabsf(axa[0][0] * ax[0] + axa[0][1] * ax[1]) +
			ha[1] * fabsf(axa[1][0] * ax[0] + axa[1][1] * ax[1]);
		float rb = hb[0] * fabsf(axb[0][0] * ax[0] + axb[0][1] * ax[1]) +
			hb[1] * fabsf(axb[1][0] * ax[0] + axb[1][1] * ax[1]);
		float sep = d[0] * ax[0] + d[1] * ax[1];
		float over = ra + rb - fabsf(sep);
		if (over <= 0.0f)
			return false;
		if (over < best) {
			best = over;
			// Always from a towards b, so the push signs are unambiguous.
			float sgn = sep < 0.0f ? -1.0f : 1.0f;
			n[0] = ax[0] * sgn;
			n[1] = ax[1] * sgn;
		}
	}
	*depth = best;
	return true;
}

// CarCollisionBody::ApplyImpulse (0x801B0350): scaled back down for a car the
// mass clamp made heavier than it is, then turned by the dampening mode's
// share of the inertia.
static void carBodyImpulse(CarSim* s, const float* J, const float* r) {
	float j[3] = { J[0], J[1], J[2] };
	if (s->invMass > RETAIL_WALL_INV_MASS)
		for (int k = 0; k < 3; k++) j[k] *= RETAIL_WALL_INV_MASS * s->mass;
	rbApplyImpulse(s, j, r, dampenSpin(s));
}

// Collision::CheckForWildCollision (0x801ABE94) for one car of a two-car hit:
// `J` the impulse on it, `r` the contact from its centre. The impulse's size,
// counted to 40, has to reach 30 and the car must not be in mode 1 already;
// then it goes into mode 1 (Car::SetDampenMode) and is kicked twice more.
// Hit nearly head-on -- the impulse within 0.97 of straight against its
// velocity -- it is pushed sideways off the line, 0.2 of the impulse, at the
// contact. Then it is lifted, 0.1 x 1.1 of the impulse's part along its
// velocity and at most 4.51, at the contact taken into the car's own plane:
// pushed 1.19 times further out to the side (no further than 0.595 of the
// front track, Car+0x20 being the CarPhysicsInfo) so the car rolls, or,
// head-on, at one end or -- half the time -- 1.1 times as far out at the
// other, nearly on the centre line, so it pitches. The player's crash camera,
// the arm after this one, is main.c's (it watches `wildHits`). The
// visible-damage arm and the 1.2 in place of 1.1 hang off globals
// (0x803FCA80/84 and two r13 flags) the port has no counterpart for.
#define RETAIL_WILD_IMPULSE  30.0f
#define RETAIL_WILD_CAP      40.0f
#define RETAIL_WILD_HEAD_ON  (-0.97f)
#define RETAIL_WILD_SIDE     0.2f
#define RETAIL_WILD_LIFT     (0.1f * 1.1f)
#define RETAIL_WILD_LIFT_MAX (4.1f * 1.1f)
static bool wildCollision(CarSim* s, const float* J, const float* r) {
	float jl = len3(J);
	if (jl > RETAIL_WILD_CAP)
		jl = RETAIL_WILD_CAP;
	if (jl < RETAIL_WILD_IMPULSE || s->dampenMode == 1)
		return false;
	s->dampenMode = 1;
	s->dampenQuiet = -1.0f;
	s->wildHits++;

	// bNormalize leaves a zero vector zero, so a car at rest is never head-on.
	float eJ[3], eV[3];
	float l = len3(J), v = len3(s->rbVel);
	for (int k = 0; k < 3; k++) {
		eJ[k] = l > 1e-6f ? J[k] / l : 0.0f;
		eV[k] = v > 1e-6f ? s->rbVel[k] / v : 0.0f;
	}
	const float d = dot3(eJ, eV);
	const bool headOn = d < RETAIL_WILD_HEAD_ON;
	if (headOn) {
		float p[3];
		for (int k = 0; k < 3; k++) p[k] = eJ[k] - eV[k] * d;
		float pl = len3(p);
		if (pl > 1e-6f) {
			const float m = -d * jl * RETAIL_WILD_SIDE / pl;
			const float K[3] = { p[0] * m, p[1] * m, 0.0f };
			carBodyImpulse(s, K, r);
		}
	}

	float lx = dot3(r, s->rot[0]), ly = dot3(r, s->rot[1]);
	if (!headOn) {
		const float lim = fabsf(cf(s, CPI_WHEEL + 4) - cf(s, CPI_WHEEL + CPI_WHEEL_STRIDE + 4)) * 0.595f;
		ly = clampf(ly * 1.19f, -lim, lim);
	} else {
		if (randomRange(s, 100.0f) >= 50.0f)
			lx *= -1.1f;
		if (fabsf(ly) > 0.13f)
			ly *= 0.2f;
	}
	float p[3];
	for (int k = 0; k < 3; k++) p[k] = lx * s->rot[0][k] + ly * s->rot[1][k];
	const float lift = clampf(fabsf(d) * jl * RETAIL_WILD_LIFT, -RETAIL_WILD_LIFT_MAX, RETAIL_WILD_LIFT_MAX);
	const float K[3] = { 0.0f, 0.0f, lift };
	carBodyImpulse(s, K, p);
	return true;
}

// The CollisionBodyWitness between two cars: each car's pose at the probe and
// the shape GetGJKPoly gave it, the GJK distance between the two shapes and
// the closest point on each (world), from the last time it was worked out.
typedef struct {
	CarSim* s;
	bool  box;
	float R[3][3], p[3];
} CarPairBody;

typedef struct {
	CarPairBody a, b;
	float dist;
	bool  colliding, deep;
	float pa[3], pb[3];
	float maxBack;   // the caller's step: no backup further than it
} CarPair;

// GJKWitness::RecalcDistance (0x801B8AB0) at the two poses: tpGjk() in a
// frame at the first car's centre of gravity, the second car in the wafer's
// place.
static void pairRecalc(CarPair* c) {
	TpGjk g;
	g.s = c->a.s;
	g.box = c->a.box;
	memcpy(g.R, c->a.R, sizeof(g.R));
	g.pn = 0;
	g.sb = c->b.s;
	g.boxB = c->b.box;
	memcpy(g.Rb, c->b.R, sizeof(g.Rb));
	for (int k = 0; k < 3; k++) g.ob[k] = c->b.p[k] - c->a.p[k];
	float pa[3], pb[3];
	c->dist = tpGjk(&g, pa, pb);
	for (int k = 0; k < 3; k++) {
		c->pa[k] = pa[k] + c->a.p[k];
		c->pb[k] = pb[k] + c->a.p[k];
	}
	c->colliding = c->dist <= TP_COLLIDING;
	c->deep = c->dist <= TP_DEEP;
}

// TestForContact's pose: both cars where they are now.
static void pairNow(CarPair* c) {
	memcpy(c->a.R, c->a.s->rot, sizeof(c->a.R));
	memcpy(c->b.R, c->b.s->rot, sizeof(c->b.R));
	for (int k = 0; k < 3; k++) {
		c->a.p[k] = c->a.s->rbPos[k];
		c->b.p[k] = c->b.s->rbPos[k];
	}
	pairRecalc(c);
}

// Contact::Crowbar (0x801B18E8) for two cars: both as they were `t` back,
// pushed apart along the line from the first's centre of gravity to the
// second's, in Crowbar's growing steps with the push's height held to 0.15 m,
// until they are no longer deep. Each car takes the whole push (a car whose
// movement is doused, none). The push is kept in `off`, the first car's to
// take away and the second's to add.
static void pairCrowbar(CarPair* c, float t, float off[3]) {
	float pa0[3], pb0[3];
	tpInterpolate(c->a.s, t, c->a.R, pa0);
	tpInterpolate(c->b.s, t, c->b.R, pb0);
	float n[3] = { pb0[0] - pa0[0], pb0[1] - pa0[1], pb0[2] - pa0[2] };
	// bNormalize leaves a zero vector zero.
	const float nl = len3(n);
	for (int k = 0; k < 3; k++) n[k] = nl > 0.0f ? n[k] / nl : 0.0f;
	float total = 0.0f, step = 0.005f;
	int i = 0;
	do {
		total += step;
		if (step < 0.1f) step += 0.01f;
		for (int k = 0; k < 3; k++) off[k] = n[k] * total;
		off[2] = clampf(off[2], -TP_CROWBAR_Z, TP_CROWBAR_Z);
		for (int k = 0; k < 3; k++) {
			c->a.p[k] = c->a.s->dousing != 2 ? pa0[k] - off[k] : pa0[k];
			c->b.p[k] = c->b.s->dousing != 2 ? pb0[k] + off[k] : pb0[k];
		}
		pairRecalc(c);
	} while (c->colliding && c->deep && ++i < TP_CROWBAR_PASSES);
}

// Contact::FindContactTime (0x801B13EC) for two cars, as tpContactTime() is
// for a car and a wafer: eight halvings of the shorter of the two cars' steps
// (GetMaxBackupTime, each car's BeginMove dt), both cars interpolated at each
// probe, for a contact deeper than a hundredth of a millimetre.
//
// Retail's answer to the car that went through another at nitrous speed (the
// user's hardware run, TASKS.md #316): the pair is resolved where it first
// touched, not where the step left it, driven well into the other.
static float pairContactTime(CarPair* c, float off[3]) {
	off[0] = off[1] = off[2] = 0.0f;
	if (!c->deep) return 0.0f;
	const float da = c->a.s->mvDt, db = c->b.s->mvDt;
	float lo = 0.0f, hi = da < db ? da : db, best = -1.0f, t = 0.0f;
	if (hi > c->maxBack) hi = c->maxBack;
	for (int i = 0; i < TP_PROBES; i++) {
		t = lo + (hi - lo) * 0.5f;
		tpInterpolate(c->a.s, t, c->a.R, c->a.p);
		tpInterpolate(c->b.s, t, c->b.R, c->b.p);
		pairRecalc(c);
		if (c->deep) {
			lo = t;
		} else if (c->colliding) {
			return t;
		} else {
			hi = t;
			if (best < 0.0f || t < best) best = t;
		}
	}
	if (best > 0.0f) return best;
	pairCrowbar(c, t, off);
	return t;
}

// DynamicCollisionBody::Backup (0x801AEF50): the pose `t` back and the linear
// momentum there, the angular momentum left as it is; then Resolve's crowbar
// push, `sign` of `off`, for a car that can be moved.
static void pairBackup(CarSim* s, float t, const float off[3], float sign) {
	float R[3][3], p[3];
	tpInterpolate(s, t, R, p);
	const float f = tpBackFraction(s, t);
	memcpy(s->rot, R, sizeof(R));
	for (int k = 0; k < 3; k++) {
		s->rbPos[k] = p[k];
		s->P[k] = (s->mvVel[k] + (s->rbVel[k] - s->mvVel[k]) * f) * s->mass;
		if (s->dousing != 2) s->rbPos[k] += sign * off[k];
	}
	rbVelocities(s);
}

// CarCollisionBody::Move (0x801AFFC4) over the `t` Resolve backed the car up:
// the mover again from where it now is, with its step's input. A car the
// traffic mover put down has no mover here (PORT, see the section header).
static void pairMove(CarSim* s, float t) {
	if (t >= 1e-6f) {
		if (s->posed) {
			for (int k = 0; k < 3; k++) s->rbPos[k] += s->rbVel[k] * t;
			orthonormalise(s->rot);
		} else {
			beginMove(s, t);
			moverMove(s, &s->mvIn, t);
		}
	}
	rbVelocities(s);
	publish(s);
}

// Contact::Resolve (0x801B1E10) for two cars: both backed up `t` and pushed
// apart by the crowbar's `off`; FindContactPointAndNormal (0x801B2164) -- the
// middle of the last closest points, and the line between them from the first
// car to the second, or between the two centres of gravity when the points
// are one; Contact::CollisionImpulse (0x801B24F4); and both moved on again.
static void pairResolve(CarPair* c, float t, const float off[3]) {
	CarSim* a = c->a.s;
	CarSim* b = c->b.s;
	const float speedA = a->speed, speedB = b->speed;
	pairBackup(a, t, off, -1.0f);
	pairBackup(b, t, off, 1.0f);

	float pt[3], n[3];
	for (int k = 0; k < 3; k++) {
		pt[k] = 0.5f * (c->pa[k] + c->pb[k]);
		n[k] = c->pb[k] - c->pa[k];
	}
	float nl = len3(n);
	if (nl < 1e-6f) {
		for (int k = 0; k < 3; k++) n[k] = b->rbPos[k] - a->rbPos[k];
		nl = len3(n);
	}
	for (int k = 0; k < 3; k++) n[k] = nl > 0.0f ? n[k] / nl : 0.0f;

	float rA[3], rB[3];
	for (int k = 0; k < 3; k++) {
		rA[k] = pt[k] - a->rbPos[k];
		rB[k] = pt[k] - b->rbPos[k];
	}
	a->contacts++;
	b->contacts++;
	a->yawHitAt = a->clock;
	b->yawHitAt = b->clock;
	recordTouch(a, pt, speedB);
	recordTouch(b, pt, speedA);

	// Contact::CollisionImpulse, both bodies live. `n` points from a to b, so
	// the pair is closing while the relative velocity along it is positive for
	// a and negative for b -- written here as b's velocity minus a's, against
	// -n, exactly as the static case uses the surface normal facing the car.
	float vpa[3], vpb[3], wxr[3];
	cross3(a->w, rA, wxr);
	for (int k = 0; k < 3; k++) vpa[k] = a->rbVel[k] + wxr[k];
	cross3(b->w, rB, wxr);
	for (int k = 0; k < 3; k++) vpb[k] = b->rbVel[k] + wxr[k];
	float vrel[3] = { vpa[0] - vpb[0], vpa[1] - vpb[1], vpa[2] - vpb[2] };
	float vn = dot3(vrel, n);

	float imaC = a->invMass > RETAIL_WALL_INV_MASS ? RETAIL_WALL_INV_MASS : a->invMass;
	float imbC = b->invMass > RETAIL_WALL_INV_MASS ? RETAIL_WALL_INV_MASS : b->invMass;
	float rn[3], tt[3], ua[3], ub[3];
	cross3(rA, n, rn);
	rbInvInertiaWorld(a, rn, tt);
	for (int k = 0; k < 3; k++) tt[k] *= dampenSpin(a);
	cross3(tt, rA, ua);
	cross3(rB, n, rn);
	rbInvInertiaWorld(b, rn, tt);
	for (int k = 0; k < 3; k++) tt[k] *= dampenSpin(b);
	cross3(tt, rB, ub);
	float denom = imaC + imbC + dot3(n, ua) + dot3(n, ub);
	// Already coming apart: no impulse, and both still move on.
	if (vn > 0.0f && denom > 1e-6f) {
		float j = (1.0f + RETAIL_CARCAR_BOUNCE) * vn / denom;
		// The floor's divisor is the LARGER of the two inverse masses, not
		// their sum: 0x801B27D4 picks max(imA, imB) into f13 and divides
		// MinimumContactImpulseFactor by that, which is what makes the floor
		// mean "the lighter car leaves at 0.3 m/s" whichever car it is paired
		// with.
		float imMax = imaC > imbC ? imaC : imbC;
		float jMin = RETAIL_WALL_MIN_J / imMax;
		if (j < jMin) j = jMin;

		float vt[3];
		for (int k = 0; k < 3; k++) vt[k] = vrel[k] - n[k] * vn;
		float vtl = len3(vt);
		float jf = j * RETAIL_CARCAR_FRICTION;
		if (vtl < RETAIL_WALL_SCRAPE) jf *= vtl / RETAIL_WALL_SCRAPE;
		float J[3];
		for (int k = 0; k < 3; k++)
			J[k] = -n[k] * j - (vtl > 1e-5f ? vt[k] / vtl * jf : 0.0f);

		// Collision::AddImpulse, before the impulse is applied: a crash puts
		// either car into mode 1 and kicks it (wildCollision). Only such a
		// collision (Collision+0x10, which nothing else sets) then has the
		// lever arm of a car with all four wheels down lose its up component,
		// so the hit turns that car about its up axis alone --
		// CollisionImpulse 0x801B2A4C, after the impulse was worked out on the
		// whole arm.
		float Jb[3] = { -J[0], -J[1], -J[2] };
		hookImpulse(a, b, NULL, false, rA, J, vtl);
		dragTotal(a, J);
		dragTotal(b, Jb);
		const bool wildA = wildCollision(a, J, rA);
		const bool wildB = wildCollision(b, Jb, rB);
		if (wildA || wildB) {
			if (a->wheelsDown == 4) {
				float u = dot3(rA, a->rot[2]);
				for (int k = 0; k < 3; k++) rA[k] -= u * a->rot[2][k];
			}
			if (b->wheelsDown == 4) {
				float u = dot3(rB, b->rot[2]);
				for (int k = 0; k < 3; k++) rB[k] -= u * b->rot[2][k];
			}
		}
		carBodyImpulse(a, J, rA);
		carBodyImpulse(b, Jb, rB);
		rbVelocities(a);
		rbVelocities(b);
		if (vn > a->lastImpact) a->lastImpact = vn;
		if (vn > b->lastImpact) b->lastImpact = vn;
	}
	pairMove(a, t);
	pairMove(b, t);
}

float carsim_car_distance(const CarSim* a, const CarSim* b) {
	CarPair c;
	c.a.s = (CarSim*)a;
	c.b.s = (CarSim*)b;
	c.a.box = c.b.box = false;
	c.maxBack = 0.0f;
	pairNow(&c);
	return c.dist;
}

bool carsim_car_contact(CarSim* a, CarSim* b, float dt) {
	if (!a || !b || a == b)
		return false;
	// MaybeCreateWitnesses' sweep: the two boxes seen from above meet.
	float ba[4], bb[4];
	tpCarBox(a, ba);
	tpCarBox(b, bb);
	if (!tpBoxesMeet(ba, bb))
		return false;

	CarPair c;
	c.a.s = a;
	c.b.s = b;
	// Backed up within each car's own move (its BeginMove), and never further
	// than the step the caller names.
	c.maxBack = dt > 0.0f ? dt : 0.0f;
	// CarCollisionBody::GetGJKPoly (0x801AFF10): the polytope's box for both
	// when both are AI cars with their four wheels down, the polytope else.
	c.a.box = c.b.box = a->aiDriven && a->wheelsDown == CARPHYS_WHEELS &&
		b->aiDriven && b->wheelsDown == CARPHYS_WHEELS;
	bool touched = false;
	for (int pass = 0; pass < TP_MAX_PER_WITNESS; pass++) {
		pairNow(&c);
		if (!c.colliding)
			break;
		float off[3];
		const float t = pairContactTime(&c, off);
		pairResolve(&c, t, off);
		touched = true;
	}
	return touched;
}

// ---------------------------------------------------------------------------
// Car against a smokeable prop (TASKS.md #318)
//
// The crash barrels, signs, bins and bumpers smokeable.c keeps. In retail each
// is a Smokeable, a DynamicCollisionBody of type 1, and meets the car through
// the same Contact::CollisionImpulse (0x801B24F4) as another car does. The
// geometry is the car-on-car pair's -- the car's plan box against the prop's
// box seen from above, backed up to where they first touched -- and the rest
// is retail's, with the Smokeable's half of each call:
//
//   MASS      GetMassInverse__9Smokeable (0x801D6264) is 0 while the prop's
//             anchor (+0x144) is not, and its RigidBody's 1/mass after; its
//             inverse inertia the same (GetInertiaTensorInverse, 0x801D6284).
//   BOUNCE    Contact::GetBounciness (0x801B2D3C) takes the larger of the
//   FRICTION  two bodies' values (only a type-0 body against a type-3 one
//             keeps the first's), and GetFriction (0x801B2C80) the same: the
//             car's 0.1 against the prop's 0.005 at most, and the larger of
//             the car's 0.3 and the prop's 0.3-0.6.
//   ANCHOR    Smokeable::ConsiderImpulse (0x801D47C8), asked first with the
//             impulse the prop is to take. Asleep with no anchor it wakes and
//             takes all of it. Anchored, it stays put unless the impulse or
//             its moment about the base, the larger, reaches the anchor; then
//             it takes the anchor's share s of it, is kicked over by -s J at
//             its base, wakes with no anchor, and the impulse loop goes round
//             again with the prop's mass in it.
//   APPLY     Smokeable::ApplyImpulse (0x801D6228) acts only on an awake one:
//             RigidBody::ApplyImpulse, the impulse clamped to 1000 and its
//             moment to 10, into the momenta.
//
// PORT: a barrel already against a wall (IsCarSmokeableWallContact, which
// then keeps the car's impulse from pointing down) has no counterpart: the
// props do not meet the walls here.

// The prop's half-axes seen from above, and the centre of its box.
static void propPlan(const CarSimProp* p, float c[3], float e[3][2], float h[3]) {
	float mid[3];
	for (int k = 0; k < 3; k++) {
		h[k] = (p->hi[k] - p->lo[k]) * 0.5f;
		mid[k] = (p->hi[k] + p->lo[k]) * 0.5f;
	}
	for (int k = 0; k < 3; k++)
		c[k] = p->pos[k] + mid[0] * p->rot[0][k] + mid[1] * p->rot[1][k] + mid[2] * p->rot[2][k];
	for (int k = 0; k < 3; k++) {
		e[k][0] = p->rot[k][0] * h[k];
		e[k][1] = p->rot[k][1] * h[k];
	}
}

// How far a shape of half-axes `e` reaches along unit `a`.
static float planReach(const float e[][2], int n, const float a[2]) {
	float r = 0.0f;
	for (int k = 0; k < n; k++)
		r += fabsf(e[k][0] * a[0] + e[k][1] * a[1]);
	return r;
}

// The separating axes of the car's rectangle and the prop's outline: the
// normals of every half-axis seen from above, a prop lying on its side
// included. Returns how many.
static int propAxes(const float ea[2][2], const float eb[3][2], float ax[5][2]) {
	int n = 0;
	const float* src[5] = { ea[0], ea[1], eb[0], eb[1], eb[2] };
	for (int i = 0; i < 5; i++) {
		float l = sqrtf(src[i][0] * src[i][0] + src[i][1] * src[i][1]);
		if (l < 1e-4f)
			continue;
		ax[n][0] = -src[i][1] / l;
		ax[n][1] = src[i][0] / l;
		n++;
	}
	return n;
}

int carsim_body_point_count(const CarSim* s) { return bodyPoints(s); }
void carsim_body_point(const CarSim* s, int k, float out[3]) { bodyPoint(s, k, out); }

void carsim_body_extent(const CarSim* s, float c[2], float* reach, float* zLo,
		float* zHi) {
	float ax[2][2], half[2];
	carPlanBox(s, c, ax, half);
	*reach = sqrtf(half[0] * half[0] + half[1] * half[1]);
	float lo = 1e30f, hi = -1e30f;
	for (int k = 0; k < 8; k++) {
		float b[3], w[3];
		boxCorner(s, k, b);
		toWorld(s->rot, b, w);
		const float z = s->rbPos[2] + w[2];
		if (z < lo) lo = z;
		if (z > hi) hi = z;
	}
	*zLo = lo;
	*zHi = hi;
}

void carsim_plan_box(const CarSim* s, float c[2], float ax[2][2], float half[2]) {
	carPlanBox(s, c, ax, half);
}

// The prop's world inverse inertia applied to `v`.
static void propInvInertiaWorld(const CarSimProp* p, const float* v, float* o) {
	float b[3], t[3];
	for (int k = 0; k < 3; k++) b[k] = dot3(p->rot[k], v);
	for (int k = 0; k < 3; k++)
		t[k] = p->invI[k][0] * b[0] + p->invI[k][1] * b[1] + p->invI[k][2] * b[2];
	for (int k = 0; k < 3; k++)
		o[k] = p->rot[0][k] * t[0] + p->rot[1][k] * t[1] + p->rot[2][k] * t[2];
}

// Smokeable::ApplyImpulse, then ComputeAuxiliaries' velocities.
static void propImpulse(CarSimProp* p, const float* J, const float* r) {
	if (!p->awake)
		return;
	float j[3] = { J[0], J[1], J[2] };
	clampLen3(j, RB_IMPULSE_CLAMP);
	float l[3];
	cross3(r, j, l);
	clampLen3(l, RB_IMPULSE_MOMENT_CLAMP);
	for (int k = 0; k < 3; k++) {
		p->P[k] += j[k];
		p->L[k] += l[k];
		p->vel[k] = p->P[k] * p->invMass;
	}
	propInvInertiaWorld(p, p->L, p->w);
}

// Smokeable::ConsiderImpulse: the share of `J`, at `r` from the centre of
// gravity, the prop takes this time round. Retail adds the CG, a body-space
// vector, to the world-space arm as it is; so does this.
static float propConsiderImpulse(CarSimProp* p, const float* J, const float* r) {
	if (p->awake)
		return 1.0f;
	if (p->anchor == 0.0f) {
		p->awake = p->woke = true;
		return 1.0f;
	}
	const float rc[3] = { r[0] + p->cg[0], r[1] + p->cg[1], r[2] + p->cg[2] };
	float m[3];
	cross3(rc, J, m);
	float mag = len3(m);
	const float jl = len3(J);
	if (jl > mag) mag = jl;
	if (mag < p->anchor)
		return 1.0f;
	const float s = p->anchor / mag;
	p->awake = p->woke = p->anchorGave = true;
	p->anchor = 0.0f;
	const float K[3] = { -J[0] * s, -J[1] * s, -J[2] * s };
	const float base[3] = { -p->cg[0], -p->cg[1], -p->cg[2] };
	propImpulse(p, K, base);
	return s;
}

bool carsim_prop_contact(CarSim* a, CarSimProp* p, float dt) {
	if (!a || !p)
		return false;
	float pc[3], eb[3][2], hb3[3];
	propPlan(p, pc, eb, hb3);
	float ca[2], axa[2][2], ha[2];
	carPlanBox(a, ca, axa, ha);
	float ea[2][2];
	for (int k = 0; k < 2; k++) {
		ea[k][0] = axa[k][0] * ha[k];
		ea[k][1] = axa[k][1] * ha[k];
	}
	// In height first: a sign on the bridge is not touching the car below.
	float cxy[2], reach, zLoA, zHiA;
	carsim_body_extent(a, cxy, &reach, &zLoA, &zHiA);
	const float hz = hb3[0] * fabsf(p->rot[0][2]) + hb3[1] * fabsf(p->rot[1][2]) +
		hb3[2] * fabsf(p->rot[2][2]);
	const float zLoB = pc[2] - hz, zHiB = pc[2] + hz;
	if (zHiB <= zLoA || zLoB >= zHiA)
		return false;

	float ax[5][2];
	const int nAx = propAxes(ea, eb, ax);
	float d[2] = { pc[0] - ca[0], pc[1] - ca[1] };
	float n2[2] = { 1.0f, 0.0f }, best = 1e30f;
	for (int i = 0; i < nAx; i++) {
		const float sep = d[0] * ax[i][0] + d[1] * ax[i][1];
		const float over = planReach(ea, 2, ax[i]) + planReach(eb, 3, ax[i]) - fabsf(sep);
		if (over <= 0.0f)
			return false;
		if (over < best) {
			best = over;
			const float sg = sep < 0.0f ? -1.0f : 1.0f;
			n2[0] = ax[i][0] * sg;
			n2[1] = ax[i][1] * sg;
		}
	}

	// FindContactTime, as for two cars: back along the closing speeds to the
	// first axis that comes apart. Overlapping as the step began, the pair is
	// eased apart along the shallowest axis by inverse mass, as two cars are:
	// an anchored prop does not move at all.
	float tBack = 0.0f;
	bool backed = false;
	if (dt > 0.0f) {
		const float vr[2] = { a->rbVel[0] - p->vel[0], a->rbVel[1] - p->vel[1] };
		float bt = dt;
		for (int i = 0; i < nAx; i++) {
			const float sep = d[0] * ax[i][0] + d[1] * ax[i][1];
			const float sg = sep < 0.0f ? -1.0f : 1.0f;
			const float closing = (vr[0] * ax[i][0] + vr[1] * ax[i][1]) * sg;
			if (closing <= 1e-3f)
				continue;
			const float t = (planReach(ea, 2, ax[i]) + planReach(eb, 3, ax[i]) -
				fabsf(sep)) / closing;
			if (t <= bt) {
				bt = t;
				backed = true;
				n2[0] = ax[i][0] * sg;
				n2[1] = ax[i][1] * sg;
			}
		}
		tBack = bt;
	}
	if (backed) {
		for (int k = 0; k < 3; k++) {
			a->rbPos[k] -= a->rbVel[k] * tBack;
			p->pos[k] -= p->vel[k] * tBack;
			pc[k] -= p->vel[k] * tBack;
		}
		carPlanBox(a, ca, axa, ha);
	} else {
		const float ia = a->invMass, ib = p->anchor != 0.0f ? 0.0f : p->invMass;
		const float push = best * CARCAR_SEPARATION;
		if (ia + ib > 1e-6f) {
			for (int k = 0; k < 2; k++) {
				a->rbPos[k] -= n2[k] * push * (ia / (ia + ib));
				p->pos[k] += n2[k] * push * (ib / (ia + ib));
				pc[k] += n2[k] * push * (ib / (ia + ib));
			}
			carPlanBox(a, ca, axa, ha);
		}
	}

	// The contact point: halfway between the two faces along the normal, in
	// the middle of where they overlap across it and in height.
	const float t2[2] = { -n2[1], n2[0] };
	const float sA = ca[0] * n2[0] + ca[1] * n2[1] + planReach(ea, 2, n2);
	const float sB = pc[0] * n2[0] + pc[1] * n2[1] - planReach(eb, 3, n2);
	const float uA = ca[0] * t2[0] + ca[1] * t2[1], rA2 = planReach(ea, 2, t2);
	const float uB = pc[0] * t2[0] + pc[1] * t2[1], rB2 = planReach(eb, 3, t2);
	const float uLo = uA - rA2 > uB - rB2 ? uA - rA2 : uB - rB2;
	const float uHi = uA + rA2 < uB + rB2 ? uA + rA2 : uB + rB2;
	const float s = (sA + sB) * 0.5f, u = (uLo + uHi) * 0.5f;
	const float pt[3] = {
		n2[0] * s + t2[0] * u, n2[1] * s + t2[1] * u,
		((zLoA > zLoB ? zLoA : zLoB) + (zHiA < zHiB ? zHiA : zHiB)) * 0.5f
	};
	float rA[3], rB[3];
	for (int k = 0; k < 3; k++) {
		rA[k] = pt[k] - a->rbPos[k];
		rB[k] = pt[k] - p->pos[k];
	}
	const float n[3] = { n2[0], n2[1], 0.0f };
	const float bounce = RETAIL_CARCAR_BOUNCE > p->bounce ? RETAIL_CARCAR_BOUNCE : p->bounce;
	const float friction = RETAIL_CARCAR_FRICTION > p->friction ? RETAIL_CARCAR_FRICTION
		: p->friction;
	const float ima = a->invMass > RETAIL_WALL_INV_MASS ? RETAIL_WALL_INV_MASS : a->invMass;

	// CollisionImpulse's loop: once, or twice when an anchored prop gave way.
	float vnHit = 0.0f;
	for (int pass = 0; pass < 2; pass++) {
		float vpa[3], vpb[3], wxr[3];
		cross3(a->w, rA, wxr);
		for (int k = 0; k < 3; k++) vpa[k] = a->rbVel[k] + wxr[k];
		cross3(p->w, rB, wxr);
		for (int k = 0; k < 3; k++) vpb[k] = p->vel[k] + wxr[k];
		const float vrel[3] = { vpb[0] - vpa[0], vpb[1] - vpa[1], vpb[2] - vpa[2] };
		const float vn = dot3(vrel, n);
		if (vn >= 0.0f)
			break;
		const bool fixed = p->anchor != 0.0f;
		const float imb = fixed ? 0.0f : p->invMass;
		float rn[3], t[3], ua[3], ub[3] = { 0.0f, 0.0f, 0.0f };
		cross3(rA, n, rn);
		rbInvInertiaWorld(a, rn, t);
		for (int k = 0; k < 3; k++) t[k] *= dampenSpin(a);
		cross3(t, rA, ua);
		if (!fixed) {
			cross3(rB, n, rn);
			propInvInertiaWorld(p, rn, t);
			cross3(t, rB, ub);
		}
		const float denom = ima + imb + dot3(n, ua) + dot3(n, ub);
		if (denom <= 1e-6f)
			break;
		float j = -(bounce + 1.0f) * vn / denom;
		const float imMax = ima > imb ? ima : imb;
		if (imMax > 0.0f && j < RETAIL_WALL_MIN_J / imMax)
			j = RETAIL_WALL_MIN_J / imMax;
		const float JB[3] = { n[0] * j, n[1] * j, n[2] * j };
		const float sh = propConsiderImpulse(p, JB, rB);
		const float jn = sh * j;
		float vt[3];
		for (int k = 0; k < 3; k++) vt[k] = vrel[k] - n[k] * vn;
		const float vtl = len3(vt);
		float jf = jn * friction;
		if (vtl < RETAIL_WALL_SCRAPE) jf *= vtl / RETAIL_WALL_SCRAPE;
		// PORT (#329): no more friction than stops the slip. The plan-box
		// contact's slip is mostly up or down the prop (its spin from the
		// anchor's kick), and a 5 kg sign took the whole 0.5 jn of it: a
		// 2 m/s slip turned into 17-26 m/s down, the sign slammed into the
		// road and vaulted 10-18 m. A car's mass leaves the walls' and the
		// cars' contacts nowhere near this, so they keep retail's term.
		if (vtl > 1e-5f) {
			float td[3], rt[3], tt[3], uta[3], utb[3] = { 0.0f, 0.0f, 0.0f };
			for (int k = 0; k < 3; k++) td[k] = vt[k] / vtl;
			cross3(rA, td, rt);
			rbInvInertiaWorld(a, rt, tt);
			for (int k = 0; k < 3; k++) tt[k] *= dampenSpin(a);
			cross3(tt, rA, uta);
			const bool loose = p->anchor == 0.0f;
			if (loose) {
				cross3(rB, td, rt);
				propInvInertiaWorld(p, rt, tt);
				cross3(tt, rB, utb);
			}
			const float kt = ima + (loose ? p->invMass : 0.0f) + dot3(td, uta) + dot3(td, utb);
			if (kt > 1e-6f && jf * kt > vtl)
				jf = vtl / kt;
		}
		float J[3];
		for (int k = 0; k < 3; k++)
			J[k] = n[k] * jn - (vtl > 1e-5f ? vt[k] / vtl * jf : 0.0f);
		const float Ja[3] = { -J[0], -J[1], -J[2] };
		// Collision::AddImpulse: the collision sounds play it as a
		// stSmokableCollision, and the style points do not count it a wall
		// (IsWallInvolved 0x801AC890 is the track's polygons only).
		hookImpulse(a, NULL, p, false, rA, Ja, vtl);
		dragTotal(a, Ja);
		propImpulse(p, J, rB);
		carBodyImpulse(a, Ja, rA);
		rbVelocities(a);
		if (-vn > vnHit) vnHit = -vn;
		if (sh == 1.0f)
			break;
	}
	if (backed) {
		for (int k = 0; k < 3; k++) {
			a->rbPos[k] += a->rbVel[k] * tBack;
			p->pos[k] += p->vel[k] * tBack;
		}
	}
	publish(a);
	a->contacts++;
	a->yawHitAt = a->clock;
	if (vnHit > a->lastImpact) a->lastImpact = vnHit;
	return true;
}

float carsim_speed_kmh(const CarSim* s) {
	return s->speed * 3.6f;
}

float carsim_yaw(const CarSim* s) {
	return carYaw(s);
}

float carsim_yaw_always(const CarSim* s) {
	return carYawAlways(s);
}

float carsim_tyre_grip(const CarSim* s) {
	const float f = cf(s, CPI_TIRE + 0xC), r = cf(s, CPI_TIRE + CPI_TIRE_STRIDE + 0xC);
	const float mu = f < r ? f : r;
	return (mu > 0.0f ? mu : 1.0f) * (s->controlMode == 0 ? NFS_ADJUST : 1.0f);
}

// CalculateMaxDeceleration (0x80017F9C): the car's own weight and half its
// speed's worth of the front and rear downforce factors (+0x264, +0x268), on
// the mean of the two tyres' static mu, over the mass. The speed is taken
// unsigned.
float carsim_max_decel(const CarSim* s, float speed) {
	const float m = cf(s, CPI_MASS);
	if (m <= 0.0f) return 0.0f;
	const float load = fabsf(speed) * 0.5f * (cf(s, CPI_AERO + 4) + cf(s, CPI_AERO + 8)) +
		GRAVITY * m;
	const float mu = (cf(s, CPI_TIRE + 0xC) + cf(s, CPI_TIRE + CPI_TIRE_STRIDE + 0xC)) * 0.5f;
	return -(load * mu / m);
}

// Car::AverageAir's pull toward the ground (0x80223478): the car's speed
// (Car +0x440) x 0.3 (0x8033A790) of the front and rear downforce factors
// (+0x264, +0x268), over the mass, in m/s^2. Gravity is the caller's.
float carsim_air_downforce(const CarSim* s) {
	const float m = cf(s, CPI_MASS);
	if (m <= 0.0f) return 0.0f;
	return s->speedAbs * 0.3f * (cf(s, CPI_AERO + 4) + cf(s, CPI_AERO + 8)) / m;
}

// MakeAccelerationTable (0x801C940C): up through the gears from rest in
// 3 mph steps (0x80332CAC), each step's acceleration the engine's torque at
// the step's revs through the ratio, over the wheel's radius and the mass --
// no drag, no grip. The radius and final drive are the front's and rear's,
// mixed by the drive split. A shift takes the next gear's ratio with the
// revs the last gear reached (retail's order), the last step is set to 0,
// and the steps are resampled onto the 64-sample table RidePhysicsInfo
// keeps at +0x550 (count set in its constructor, 0x801C7DC4).
float carsim_accel_table(const CarSim* s, float out[CARSIM_ACCEL_N]) {
	enum { STEPS = 92 };
	const float step = 1.34108f, sp = cf(s, CPI_DTI + DTI_SPLIT);
	const float radius = (1.0f - sp) * cf(s, CPI_WHEEL + WI_RADIUS) +
		sp * cf(s, CPI_WHEEL + 2 * CPI_WHEEL_STRIDE + WI_RADIUS);
	const float final = (1.0f - sp) * cf(s, CPI_DTI + DTI_FRONT_FINAL) +
		sp * cf(s, CPI_DTI + DTI_REAR_FINAL);
	const float mass = cf(s, CPI_MASS);
	const float* ei = cpiAt(s, CPI_EI);
	float a[STEPS];
	float v = 0.0f;
	int gear = GEAR_FIRST, n = 0;
	if (!(radius > 0.0f) || !(mass > 0.0f)) {
		for (int k = 0; k < CARSIM_ACCEL_N; k++) out[k] = 0.0f;
		return 0.0f;
	}
	while (n < STEPS) {
		const float rpm = v / radius * final * dtRatio(s, gear) * RPM_PER_RADS;
		if (rpm >= s->up[gear] && ++gear >= numGears(s) + 2) break;
		v += step;
		a[n++] = tableGet(ei + EI_TORQUE / 4, 9, ei[0], ei[2], rpm) * final *
			dtRatio(s, gear) / radius / mass;
	}
	for (int i = n > 0 ? n - 1 : 0; i < STEPS; i++) a[i] = 0.0f;
	for (int k = 0; k < CARSIM_ACCEL_N; k++)
		out[k] = n > 1 ? tableGet(a, n, 0.0f, v, (float)k * v / (float)(CARSIM_ACCEL_N - 1))
			: 0.0f;
	return v;
}

bool carsim_drive_wheel_down(const CarSim* s) {
	return !driveWheelsOffGround(s);
}

bool carsim_front_wheel_down(const CarSim* s) {
	return s->wheel[0].onGround || s->wheel[1].onGround;
}

// TerrainTypeInfoTable (0x8036FE2C, records of 0x98) +0x78 / +0x7C: the
// summed penalty impulse at which a terrain starts to bottom out and where
// it is at its loudest. SPEED2.EXE's table (file 0x400F58) is the same.
static const float kBottomOutRange[29][2] = {
	{ 50.0f, 100.0f },                                                     // 0 NONE
	{ 1.0f, 2.0f }, { 1.0f, 2.0f }, { 1.0f, 2.0f }, { 1.0f, 2.0f },        // 1 ROAD, 2 ROAD_WET, 4 ROAD_SMOKE_1
	{ 1.0f, 2.0f }, { 1.0f, 2.0f }, { 1.0f, 2.0f }, { 1.0f, 2.0f },        // 8 DIRT
	{ 1.0f, 2.0f }, { 1.0f, 2.0f }, { 1.0f, 2.0f }, { 1.0f, 2.0f },        // 11 COBBLESTONE
	{ 0.6f, 3.0f }, { 0.6f, 3.0f },                                        // 13 PUDDLE, 14 DEEP_WATER
	{ 1.0f, 2.0f }, { 1.0f, 2.0f },                                        // 15, 16
	{ 0.6f, 3.0f }, { 0.6f, 3.0f }, { 0.6f, 3.0f }, { 0.6f, 3.0f },        // 20 SOLID_WALL
	{ 0.6f, 3.0f }, { 0.6f, 3.0f }, { 0.6f, 3.0f }, { 0.6f, 3.0f },        // 21..24
	{ 0.6f, 3.0f }, { 0.6f, 3.0f }, { 0.6f, 3.0f }, { 0.6f, 3.0f },        // 25..28
};

float carsim_bottom_out(const CarSim* s, int terrain) {
	int down = 0;
	float pen = 0.0f;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		down += s->wheel[i].onGround != 0;
		pen += s->bottomPen[i];
	}
	if (down < 3) return 0.0f;
	// PORT guard: a type past the table reads as ROAD's.
	const float* r = kBottomOutRange[(unsigned)terrain < 29 ? terrain : 1];
	float x = (pen - r[0]) / (r[1] - r[0]);
	return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

bool carsim_backfire(CarSim* s, float level) {
	// CarState +0x404 and +0x418 as UpdateCar leaves them: the box's gear, and
	// its clutch in state 3 (PhysicsMover 0x801C1EBC). The MellowMover's
	// (0x801BBA0C) is always engaged.
	const int gear = s->dtGear;
	const int engaged = s->mellow ? 1 : s->clutchState == 3;
	bool fire = false;
	// ChangeGear. The PhysicsMover (movement mode 1) fires on any change, and
	// past first only on the clutch's bite; the MellowMover on a change at
	// least half a second after the one before.
	if (gear != s->bfGear) {
		s->bfGear = gear;
		fire = !s->mellow || s->clock - s->bfAt > 0.5f;
		s->bfAt = s->clock;
	}
	if (!s->mellow && gear > GEAR_FIRST) fire = !s->bfEngaged && engaged;
	s->bfEngaged = engaged;
	if (level < 2.0f) return false;
	// TriggerCarTerrainEffects: the revs (CarState +0x410) against the span
	// from EngineInfo's idle (+0x1F0) to its limiter (+0x1F8).
	if (gear == GEAR_N) {
		const float span = cf(s, CPI_EI + EI_LIMITER) - cf(s, CPI_EI + EI_IDLE);
		if (!s->bfArmed) {
			if (s->rpmSmooth <= span * 0.5f) s->bfArmed = 1;
		} else if (span * 0.75f <= s->rpmSmooth) {
			s->bfArmed = 0;
			fire = true;
		}
	}
	return fire;
}

// CarState::IsDriveWheel (0x801AB500): DriveTrainInfo's split under 0.2 is
// the front pair, over 0.8 the rear, and between the two all four.
bool carsim_is_drive_wheel(const CarSim* s, int i) {
	float split = cf(s, CPI_DTI + DTI_SPLIT);
	if (split < 0.2f) return i == 0 || i == 1;
	if (split <= 0.8f) return true;
	return i == 2 || i == 3;
}

// The nearest two points of the plan rectangles can be told apart by
// checking each corner of one against each edge of the other: for two convex
// shapes that do not overlap, one of those pairs is the closest.
static float segPointDist2(const float p[2], const float a[2], const float b[2]) {
	float ab[2] = { b[0] - a[0], b[1] - a[1] }, ap[2] = { p[0] - a[0], p[1] - a[1] };
	float l = ab[0] * ab[0] + ab[1] * ab[1];
	float t = l > 1e-9f ? (ap[0] * ab[0] + ap[1] * ab[1]) / l : 0.0f;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	float dx = ap[0] - ab[0] * t, dy = ap[1] - ab[1] * t;
	return dx * dx + dy * dy;
}

static void planCorners(const float c[2], const float ax[2][2], const float h[2], float out[4][2]) {
	static const float sg[4][2] = { { 1, 1 }, { 1, -1 }, { -1, -1 }, { -1, 1 } };
	for (int k = 0; k < 4; k++)
		for (int j = 0; j < 2; j++)
			out[k][j] = c[j] + ax[0][j] * h[0] * sg[k][0] + ax[1][j] * h[1] * sg[k][1];
}

float carsim_plan_gap(const CarSim* a, const CarSim* b) {
	float ca[2], cb[2], axa[2][2], axb[2][2], ha[2], hb[2], n[2], depth;
	carPlanBox(a, ca, axa, ha);
	carPlanBox(b, cb, axb, hb);
	if (planBoxOverlap(ca, axa, ha, cb, axb, hb, n, &depth))
		return 0.0f;
	float pa[4][2], pb[4][2];
	planCorners(ca, axa, ha, pa);
	planCorners(cb, axb, hb, pb);
	float best = 1e30f;
	for (int i = 0; i < 4; i++)
		for (int k = 0; k < 4; k++) {
			float d = segPointDist2(pa[i], pb[k], pb[(k + 1) & 3]);
			if (d < best) best = d;
			d = segPointDist2(pb[i], pa[k], pa[(k + 1) & 3]);
			if (d < best) best = d;
		}
	return sqrtf(best);
}

void carsim_window_init(CarSimWindow* a, int cap, float length) {
	windowInit(a, cap > CARSIM_WINDOW_MAX ? CARSIM_WINDOW_MAX : cap, length);
}

void carsim_window_record(CarSimWindow* a, float v, float t) {
	windowRecord(a, v, t);
}

bool carsim_cars_overlap(const CarSim* a, const CarSim* b) {
	if (!a || !b || a == b)
		return false;
	float dx = a->rbPos[0] - b->rbPos[0], dy = a->rbPos[1] - b->rbPos[1];
	float reach = (cf(a, CPI_DIMS) + cf(a, CPI_DIMS + 4) +
		cf(b, CPI_DIMS) + cf(b, CPI_DIMS + 4)) * 0.5f;
	if (dx * dx + dy * dy > reach * reach)
		return false;
	float hz = (cf(a, CPI_DIMS + 8) + cf(b, CPI_DIMS + 8)) * 0.5f;
	if (fabsf(a->rbPos[2] - b->rbPos[2]) > hz)
		return false;
	float ca[2], cb[2], axa[2][2], axb[2][2], ha[2], hb[2];
	carPlanBox(a, ca, axa, ha);
	carPlanBox(b, cb, axb, hb);
	float n2[2] = { 1.0f, 0.0f }, depth = 0.0f;
	return planBoxOverlap(ca, axa, ha, cb, axb, hb, n2, &depth);
}

void carsim_set_rolling(CarSim* s, float speed) {
	if (speed <= 0.0f)
		return;
	for (int k = 0; k < 3; k++) s->P[k] = s->mass * speed * s->rot[0][k];
	rbVelocities(s);
	// ResetCore: each wheel's spin is the reset speed over its own radius.
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* wh = &s->wheel[i];
		wh->w = wh->radius > 0.0f ? speed / wh->radius : 0.0f;
		wh->dir = 0;
	}
	if (s->mellow) mlUpdateCar(s, 0.0f);
	else updateCar(s, 0.0f);
	publish(s);
}

// A debug launch (main.c's DbgJumpKick, #504, a test aid): the car rolling
// at `speed` as carsim_set_rolling sends it, `up` m/s straight up on top.
void carsim_dbg_launch(CarSim* s, float speed, float up) {
	carsim_set_rolling(s, speed);
	s->P[2] += s->mass * up;
	rbVelocities(s);
	publish(s);
}

void carsim_traffic_pose(CarSim* s, const CarSimWorld* w, float x, float y, float roadZ,
		float heading, float speed, float dt) {
	const float old[3] = { s->rbPos[0], s->rbPos[1], s->rbPos[2] };
	const float oldYaw = s->yaw;
	// Put down, not moved: no track polygons kept from before. Still a body
	// that moved over the step (BeginMove), which a hit by another car backs
	// up through (carsim_car_contact).
	s->tpN = -1;
	s->witN = 0;
	s->aiDriven = true;
	s->posed = true;
	beginMove(s, dt);

	// FindFlatOnTrackPosition (0x8022073C): the four wheels' WheelInfo
	// positions turned to `heading` about the CG's point, each dropped onto
	// the ground under it. With all four found the car takes the plane they
	// make -- the two sides' front-to-back vectors summed for its nose, the
	// two axles' left-to-right for its left -- and its CG stands hRest over
	// their mean height along that plane's normal; with any missing it stays
	// level at the height it had.
	const float c = cosf(heading), sn = sinf(heading);
	float xy[CARPHYS_WHEELS * 2];
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		const float* p = s->wheel[i].pos;
		xy[i * 2 + 0] = x + c * p[0] - sn * p[1];
		xy[i * 2 + 1] = y + sn * p[0] + c * p[1];
	}
	float z[CARPHYS_WHEELS], nn[CARPHYS_WHEELS * 3];
	bool hit[CARPHYS_WHEELS] = { false, false, false, false };
	bool all = false;
	if (w && w->groundProbe) {
		// PORT: retail's TopologyCoordinate keeps the surface the wheel was
		// on; the port's probe takes the highest one under a ceiling, put
		// as carsim_place puts it, 1.5 m over the road the car is on.
		all = w->groundProbe(w->ctx, xy, CARPHYS_WHEELS, roadZ + 1.5f, z, hit, nn,
			NULL) == CARPHYS_WHEELS;
		for (int i = 0; i < CARPHYS_WHEELS && all; i++) all = hit[i];
	}
	// PORT: with a wheel off the loaded world retail keeps the height the
	// car had; the port's world is only paged in round the player, so a car
	// out of it would hold one height over every hill. It takes the road's.
	float rz = roadZ + s->hRest;
	if (all) {
		float wp[CARPHYS_WHEELS][3];
		float zSum = 0.0f;
		for (int i = 0; i < CARPHYS_WHEELS; i++) {
			wp[i][0] = xy[i * 2]; wp[i][1] = xy[i * 2 + 1]; wp[i][2] = z[i];
			zSum += z[i];
		}
		float f[3], l[3], u[3];
		for (int k = 0; k < 3; k++) {
			f[k] = (wp[0][k] - wp[3][k]) + (wp[1][k] - wp[2][k]);
			l[k] = (wp[0][k] - wp[1][k]) + (wp[3][k] - wp[2][k]);
		}
		const float fl = len3(f), ll = len3(l);
		if (fl > 1e-4f && ll > 1e-4f) {
			for (int k = 0; k < 3; k++) { f[k] /= fl; l[k] /= ll; }
			cross3(f, l, u);
			const float ul = len3(u);
			if (ul > 1e-4f && fabsf(u[2]) > 1e-3f) {
				for (int k = 0; k < 3; k++) u[k] /= ul;
				// PORT: the summed sides need not be square to each other;
				// retail keeps the matrix as it comes, the port squares the
				// left up so the body and the renderer get a rotation.
				cross3(u, f, l);
				for (int k = 0; k < 3; k++) {
					s->rot[0][k] = f[k]; s->rot[1][k] = l[k]; s->rot[2][k] = u[k];
				}
				rz = zSum * 0.25f + s->hRest / fabsf(u[2]);
			} else {
				all = false;
			}
		} else {
			all = false;
		}
	}
	if (!all) {
		const float flat[3][3] = { { c, sn, 0.0f }, { -sn, c, 0.0f }, { 0.0f, 0.0f, 1.0f } };
		memcpy(s->rot, flat, sizeof(flat));
	}
	s->rbPos[0] = x;
	s->rbPos[1] = y;
	s->rbPos[2] = rz;

	// The velocity is the step's own displacement, at the mover's speed;
	// none below 1e-5 m/s (0x80317D08).
	float v[3] = { s->rbPos[0] - old[0], s->rbPos[1] - old[1], s->rbPos[2] - old[2] };
	const float vl = len3(v);
	if (fabsf(speed) <= 1e-5f || vl < 1e-6f) {
		v[0] = v[1] = v[2] = 0.0f;
	} else {
		for (int k = 0; k < 3; k++) v[k] *= speed / vl;
	}
	for (int k = 0; k < 3; k++) {
		s->P[k] = s->mass * v[k];
		s->F[k] = s->T[k] = 0.0f;
	}
	// +0x110, the yaw rate: the s16 heading's change over the step.
	const s16 dh = (s16)((s16)(int)(heading * 10430.378f) - (s16)(int)(oldYaw * 10430.378f));
	const float wz = dt > 0.0f ? (float)dh * (6.2831855f / 65536.0f) / dt : 0.0f;
	const float wv[3] = { 0.0f, 0.0f, wz };
	const float* inv = s->zdamp ? s->IinvZ : s->Iinv;
	float wc[3];
	toCar(s->rot, wv, wc);
	for (int k = 0; k < 3; k++) wc[k] = inv[k] > 0.0f ? wc[k] / inv[k] : 0.0f;
	toWorld(s->rot, wc, s->L);
	rbVelocities(s);

	// Each wheel turning at the speed over its radius, on the ground, on
	// ROAD (SetWheelTouchingGround / SetWheelTerrain 1).
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* wh = &s->wheel[i];
		wh->w = wh->radius > 0.0f ? speed / wh->radius : 0.0f;
		wh->angle += wh->w * dt;
		wh->onGround = 1;
		wh->terrain = 1;
		wh->s = wh->slip = 0.0f;
		wh->sndSlip[0] = wh->sndSlip[1] = 0.0f;
		wh->Flong = wh->Flat = 0.0f;
	}
	s->wheelsDown = CARPHYS_WHEELS;
	s->vfwd = speed;
	s->speedAbs = fabsf(speed);
	s->clock += dt;
	publish(s);
}

float carsim_rev_fraction(const CarSim* s) {
	float span = s->perf.limiterRpm - s->perf.idleRpm;
	if (span <= 0.0f) return 0.0f;
	return clampf((s->rpm - s->perf.idleRpm) / span, 0.0f, 1.0f);
}
