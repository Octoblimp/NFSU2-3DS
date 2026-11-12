#include "ridephys.h"
#include <math.h>
#include <string.h>

// See ridephys.h for what this is and where each piece comes from. Offsets are
// the decompile's own, relative to the CarPhysicsInfo (P) and to the
// CompleatCarPhysicsInfo (C); docs/retail_car_physics.md names the CPI ones.

static float rd(const u8* b, int off) {
	float v;
	memcpy(&v, b + off, sizeof(v));
	return v;
}

static void wr(u8* b, int off, float v) {
	memcpy(b + off, &v, sizeof(v));
}

static int rdi(const u8* b, int off) {
	int v;
	memcpy(&v, b + off, sizeof(v));
	return v;
}

// (target - v) * t + v, the lerp every builder writes out longhand.
static float lerp(float v, float target, float t) {
	return (target - v) * t + v;
}

// Table::GetValue (0x801A4EC8) over `n` samples spread evenly across
// [lo, hi] (CalcIndexMultiplier 0x80302950): linear between samples, the end
// samples beyond them.
float ridephys_table_get(const float* data, int n, float lo, float hi, float x) {
	if (n <= 0)
		return 0.0f;
	const float mult = hi != lo ? (float)(n - 1) / (hi - lo) : 0.0f;
	const float f = mult * (x - lo);
	if (f < 0.0f)
		return data[0];
	if (f >= (float)(n - 1))
		return data[n - 1];
	const int i = (int)f;
	float fi = (float)i;
	if (f < fi)
		fi -= 1.0f;
	const float t = f - fi;
	return (1.0f - t) * data[i] + t * data[i + 1];
}

// ---------------------------------------------------------------------------
// BuildCarPhysicsInfo's package builders (0x801C5254 .. 0x801C5B6C). Each
// takes ComputeUpgradeLevel's average `lvl` (0..1) and its components, and
// does nothing at 0 -- except the turbo, which always runs.
// ---------------------------------------------------------------------------

static void build_weight(const u8* C, u8* P, float lvl) {
	if (lvl == 0.0f)
		return;
	wr(P, 0x000, rd(P, 0x000) + ((rd(C, 0x344) - 0.0f) * lvl + 0.0f));
	wr(P, 0x160, lerp(rd(P, 0x160), rd(C, 0x348), lvl));
	wr(P, 0x110, rd(P, 0x110) * ((rd(C, 0x340) - 1.0f) * lvl + 1.0f));
}

// The third component picks one of three whole drivetrain blocks (C +0x350,
// +0x390, +0x3D0): final drives, split, shift point, the eight ratios and the
// gear count, the non-zero ratios from the first forward one (at most six).
static void build_drivetrain(const u8* C, u8* P, float lvl, const float* comp) {
	if (lvl == 0.0f)
		return;
	const int idx = (int)(comp[2] * 3.0f + 0.5f);
	if (idx >= 1 && idx <= 3) {
		const int b = 0x310 + idx * 0x40;
		wr(P, 0x1B8, rd(C, b + 0x08));
		wr(P, 0x1BC, rd(C, b + 0x0C));
		wr(P, 0x1C0, rd(C, b + 0x10));
		wr(P, 0x1B0, rd(C, b + 0x00));
		wr(P, 0x1B4, rd(C, b + 0x04));
		memcpy(P + 0x1D0, C + b + 0x20, 0x20);
		int gears = 0;
		for (int k = 0; k < 6 && rd(C, b + 0x28 + 4 * k) != 0.0f; k++)
			gears++;
		memcpy(P + 0x1C8, &gears, sizeof(gears));
	}
	wr(P, 0x1CC, lerp(rd(P, 0x1CC), rd(C, 0x3EC), comp[0]));
	wr(P, 0x1C4, lerp(rd(P, 0x1C4), rd(C, 0x3E4), comp[0]));
	wr(P, 0x1FC, rd(P, 0x1FC) + ((rd(C, 0x41C) - 0.0f) * comp[1] + 0.0f));
}

static void build_nitrous(const u8* C, u8* P, float lvl, const float* comp) {
	if (lvl == 0.0f)
		return;
	wr(P, 0x2A0, lerp(rd(P, 0x2A0), rd(C, 0x580), comp[0]));
	wr(P, 0x2AC, rd(P, 0x2AC) + (rd(C, 0x58C) - rd(P, 0x2AC)));
}

static void build_engine(const u8* C, u8* P, float lvl) {
	if (lvl == 0.0f)
		return;
	for (int k = 0; k < 9; k++)
		wr(P, 0x200 + 4 * k, rd(P, 0x200 + 4 * k) + ((rd(C, 0x420 + 4 * k) - 0.0f) * lvl + 0.0f));
	for (int k = 0; k < 3; k++)
		wr(P, 0x248 + 4 * k, rd(P, 0x248 + 4 * k) + ((rd(C, 0x444 + 4 * k) - 0.0f) * lvl + 0.0f));
}

// No level gate: the boost curve is lerped towards the turbo kit's and folded
// into the torque curve every build, so a stock turbo car's torque is its base
// curve plus its stock boost.
static void build_turbo(const u8* C, u8* P, float lvl) {
	for (int k = 0; k < 9; k++) {
		const float boost = lerp(rd(P, 0x224 + 4 * k), rd(C, 0x510 + 4 * k), lvl);
		wr(P, 0x224 + 4 * k, boost);
		wr(P, 0x200 + 4 * k, rd(P, 0x200 + 4 * k) + boost);
	}
}

// Front then rear SuspensionInfo (P +0x170 / +0x190) towards C +0x5C0 /
// +0x5E0, then the ride height: the wheels' z (front pair, rear pair) up and
// the CG down.
static void build_suspension(const u8* C, u8* P, float lvl, const float* comp) {
	if (lvl == 0.0f)
		return;
	for (int a = 0; a < 2; a++) {
		const int s = a ? 0x5E0 : 0x5C0;
		const int p = 0x170 + a * 0x20;
		wr(P, p + 0x1C, lerp(rd(P, p + 0x1C), rd(C, s + 0x1C), comp[0]));
		wr(P, p + 0x00, lerp(rd(P, p + 0x00), rd(C, s + 0x00), comp[0]));
		wr(P, p + 0x04, lerp(rd(P, p + 0x04), rd(C, s + 0x04), comp[0]));
		for (int j = 0; j < 2; j++) {
			wr(P, p + 0x14 + 4 * j, lerp(rd(P, p + 0x14 + 4 * j), rd(C, s + 0x14 + 4 * j), comp[0]));
			wr(P, p + 0x08 + 4 * j, lerp(rd(P, p + 0x08 + 4 * j), rd(C, s + 0x08 + 4 * j), comp[0]));
		}
		wr(P, p + 0x10, lerp(rd(P, p + 0x10), rd(C, s + 0x10), comp[1]));
	}
	for (int w = 0; w < 2; w++)
		wr(P, 0x18 + w * 0x30, rd(P, 0x18 + w * 0x30) + ((rd(C, 0x604) - 0.0f) * comp[0] + 0.0f));
	for (int w = 2; w < 4; w++)
		wr(P, 0x18 + w * 0x30, rd(P, 0x18 + w * 0x30) + ((rd(C, 0x608) - 0.0f) * comp[0] + 0.0f));
	wr(P, 0x008, rd(P, 0x008) - ((rd(C, 0x600) - 0.0f) * comp[0] + 0.0f));
	wr(P, 0x270, lerp(rd(P, 0x270), rd(C, 0x5BC), comp[2]));
}

static void build_brakes(const u8* C, u8* P, float lvl) {
	if (lvl == 0.0f)
		return;
	wr(P, 0x27C, lerp(rd(P, 0x27C), rd(C, 0x5B8), lvl));
	wr(P, 0x278, lerp(rd(P, 0x278), rd(C, 0x5B4), lvl));
	wr(P, 0x274, lerp(rd(P, 0x274), rd(C, 0x5B0), lvl));
}

// Parts 0x2D, 0x2E and 0x33 take the speed governor off whatever the level.
// The rounded level then picks one of three idle / redline / limiter offsets
// (C +0x450, +0x490, +0x4D0).
static void build_ecu(const u8* C, u8* P, float lvl, float level7, const u8* installed) {
	if (installed && (installed[0x2D] || installed[0x2E] || installed[0x33]))
		wr(P, 0x290, 0.0f);
	if (lvl == 0.0f)
		return;
	const int l = (int)(level7 + 0.5f);
	if (l >= 1 && l <= 3) {
		const int b = 0x410 + l * 0x40;
		wr(P, 0x1F0, rd(P, 0x1F0) + rd(C, b + 0x0));
		wr(P, 0x1F4, rd(P, 0x1F4) + rd(C, b + 0x4));
		wr(P, 0x1F8, rd(P, 0x1F8) + rd(C, b + 0x8));
	}
	for (int k = 0; k < 9; k++)
		wr(P, 0x200 + 4 * k, rd(P, 0x200 + 4 * k) + ((rd(C, 0x4E0 + 4 * k) - 0.0f) * lvl + 0.0f));
	for (int k = 0; k < 3; k++)
		wr(P, 0x248 + 4 * k, rd(P, 0x248 + 4 * k) + ((rd(C, 0x504 + 4 * k) - 0.0f) * lvl + 0.0f));
}

// Front then rear TireInfo (P +0xD0 / +0xF0) towards C +0x540 / +0x560.
static void build_tires(const u8* C, u8* P, float lvl) {
	if (lvl == 0.0f)
		return;
	for (int a = 0; a < 2; a++) {
		const int t = a ? 0x560 : 0x540;
		const int p = 0xD0 + a * 0x20;
		wr(P, p + 0x08, lerp(rd(P, p + 0x08), rd(C, t + 0x08), lvl));
		wr(P, p + 0x04, lerp(rd(P, p + 0x04), rd(C, t + 0x04), lvl));
		wr(P, p + 0x00, lerp(rd(P, p + 0x00), rd(C, t + 0x00), lvl));
		wr(P, p + 0x10, lerp(rd(P, p + 0x10), rd(C, t + 0x10), lvl));
		wr(P, p + 0x0C, lerp(rd(P, p + 0x0C), rd(C, t + 0x0C), lvl));
	}
}

// BuildAeroInfo: the aero path's two components, each adding the two aero
// kits' sum.
static void build_aero(const u8* C, u8* P, float lvl, const float* comp) {
	if (lvl == 0.0f)
		return;
	wr(P, 0x264, rd(P, 0x264) + (((rd(C, 0x5A0) + rd(C, 0x590)) - 0.0f) * comp[0] + 0.0f));
	wr(P, 0x268, rd(P, 0x268) + (((rd(C, 0x5A4) + rd(C, 0x594)) - 0.0f) * comp[1] + 0.0f));
	wr(P, 0x26C, rd(P, 0x26C) + (((rd(C, 0x5A8) + rd(C, 0x598)) - 0.0f) * lvl + 0.0f));
}

// RigidBodyInfo::BuildInertiaTensor (0x801C98F8): a box of the body's length,
// width and height (RB +4/+8/+0xC) and mass, on an identity matrix at +0x10.
static void build_inertia(u8* P) {
	const int rb = 0x110;
	const float m = rd(P, rb), l = rd(P, rb + 4), w = rd(P, rb + 8), h = rd(P, rb + 0xC);
	for (int k = 0; k < 16; k++)
		wr(P, rb + 0x10 + 4 * k, (k % 5) == 0 ? 1.0f : 0.0f);
	wr(P, rb + 0x10, ((w * w + h * h) * m) / 12.0f);
	wr(P, rb + 0x24, ((l * l + h * h) * m) / 12.0f);
	wr(P, rb + 0x38, ((l * l + w * w) * m) / 12.0f);
}

// ApplyJunkmanUpgrades (0x801C4F30): one adjustment per junkman flag, out of
// C +0x2B0..+0x33C.
void ridephys_apply_junkman(const u8* C, u8* P, const int* jm) {
	if (jm[0])
		wr(P, 0x110, rd(P, 0x110) * (1.0f - rd(C, 0x2B0)));
	if (jm[1])
		memcpy(P + 0x1C0, C + 0x2B4, 4);
	if (jm[2])
		wr(P, 0x2AC, rd(P, 0x2AC) + rd(C, 0x2B8));
	if (jm[6])
		wr(P, 0x274, rd(P, 0x274) + rd(C, 0x2BC));
	if (jm[8]) {
		for (int a = 0; a < 2; a++) {
			wr(P, 0xDC + a * 0x20, rd(P, 0xDC + a * 0x20) + rd(C, 0x2C0 + 4 * a));
			wr(P, 0xE0 + a * 0x20, rd(P, 0xE0 + a * 0x20) + rd(C, 0x2C8 + 4 * a));
		}
	}
	// The engine (+0x2D0), ECU (+0x2F4) and turbo (+0x318) torque curves; the
	// turbo's goes into the boost curve as well.
	for (int k = 0; k < 9; k++) {
		const int t = 0x200 + 4 * k;
		wr(P, t, rd(P, t) + (jm[3] ? rd(C, 0x2D0 + 4 * k) : 0.0f));
		wr(P, t, rd(P, t) + (jm[7] ? rd(C, 0x2F4 + 4 * k) : 0.0f));
		wr(P, t, rd(P, t) + (jm[4] ? rd(C, 0x318 + 4 * k) : 0.0f));
		wr(P, 0x224 + 4 * k, rd(P, 0x224 + 4 * k) + (jm[4] ? rd(C, 0x318 + 4 * k) : 0.0f));
	}
}

// RemoveJunkmanUpgrades (0x801C50CC): ApplyJunkmanUpgrades backwards, the mass
// divided back and every sum taken off again -- except flag 1's drive split,
// which stays.
void ridephys_remove_junkman(const u8* C, u8* P, const int* jm) {
	if (jm[0])
		wr(P, 0x110, rd(P, 0x110) / (1.0f - rd(C, 0x2B0)));
	if (jm[2])
		wr(P, 0x2AC, rd(P, 0x2AC) - rd(C, 0x2B8));
	if (jm[6])
		wr(P, 0x274, rd(P, 0x274) - rd(C, 0x2BC));
	if (jm[8]) {
		for (int a = 0; a < 2; a++) {
			wr(P, 0xDC + a * 0x20, rd(P, 0xDC + a * 0x20) - rd(C, 0x2C0 + 4 * a));
			wr(P, 0xE0 + a * 0x20, rd(P, 0xE0 + a * 0x20) - rd(C, 0x2C8 + 4 * a));
		}
	}
	for (int k = 0; k < 9; k++) {
		const int t = 0x200 + 4 * k;
		wr(P, t, rd(P, t) - (jm[3] ? rd(C, 0x2D0 + 4 * k) : 0.0f));
		wr(P, t, rd(P, t) - (jm[7] ? rd(C, 0x2F4 + 4 * k) : 0.0f));
		wr(P, t, rd(P, t) - (jm[4] ? rd(C, 0x318 + 4 * k) : 0.0f));
		wr(P, 0x224 + 4 * k, rd(P, 0x224 + 4 * k) - (jm[4] ? rd(C, 0x318 + 4 * k) : 0.0f));
	}
}

// ---------------------------------------------------------------------------
// The acceleration table
// ---------------------------------------------------------------------------

#define STEP_3MPH  1.34108f  // 0x80332CAC: 3 mph in m/s
#define RAD_TO_RPM 9.5493f   // 0x80332CB8
#define MPH100     44.7027f  // 0x80332C14
#define NACCEL     0x5C

// CalculateShiftPoints (0x801C99C8) with GearShiftMethod 0, as shipped: first
// gear at the DriveTrainInfo's fraction of the rev range, every other upshift
// 100 below the redline, the top gear's at the limiter + 500.
static void shift_points(const u8* P, float up[10]) {
	const int gears = rdi(P, 0x1C8);
	const float idle = rd(P, 0x1F0), redline = rd(P, 0x1F4), limiter = rd(P, 0x1F8);
	for (int g = 0; g < 10; g++)
		up[g] = 0.0f;
	up[1] = rd(P, 0x1B0) * (redline - idle) + idle;
	int g = 2;
	for (; g < gears + 1 && g < 9; g++)
		up[g] = redline - 100.0f;
	up[g] = limiter + 500.0f;
}

// MakeAccelerationTable (0x801C940C): from rest, 3 mph a step, the wheel
// force the torque curve gives through the current gear over the mass,
// changing up at each shift point, until the top gear runs out. The last
// sample taken is zeroed, and the run is resampled to 64 entries over the
// speed reached.
static void make_accel_table(RidePhys* r) {
	const u8* P = r->cpi;
	const float split = rd(P, 0x1C0), mass = rd(P, 0x110);
	const float radius = (1.0f - split) * rd(P, 0x20) + split * rd(P, 0x80);
	const float fd = (1.0f - split) * rd(P, 0x1B8) + split * rd(P, 0x1BC);
	const int gears = rdi(P, 0x1C8);
	float up[10];
	shift_points(P, up);
	float torque[9];
	for (int k = 0; k < 9; k++)
		torque[k] = rd(P, 0x200 + 4 * k);
	const float tLo = rd(P, 0x1F0), tHi = rd(P, 0x1F8);
	float acc[NACCEL];
	memset(acc, 0, sizeof(acc));
	int gear = 2;
	float v = 0.0f;
	int i = 0;
	for (; i < NACCEL; i++) {
		const float rpm = (v / radius) * fd * rd(P, 0x1D0 + 4 * gear) * RAD_TO_RPM;
		if (up[gear] <= rpm) {
			gear++;
			if (gears + 2 <= gear || gear > 7)
				break;
		}
		v += STEP_3MPH;
		acc[i] = ridephys_table_get(torque, 9, tLo, tHi, rpm) * fd * rd(P, 0x1D0 + 4 * gear) / radius / mass;
	}
	for (int k = i - 1; k < NACCEL; k++)
		if (k >= 0)
			acc[k] = 0.0f;
	for (int k = 0; k < RIDEPHYS_ACCEL_SAMPLES; k++)
		r->accel[k] = ridephys_table_get(acc, i, 0.0f, v,
			0.0f + ((float)k * (v - 0.0f)) / (float)(RIDEPHYS_ACCEL_SAMPLES - 1));
	r->accelMin = 0.0f;
	r->accelMax = v;
}

static float accel_at(const RidePhys* r, float v) {
	return ridephys_table_get(r->accel, RIDEPHYS_ACCEL_SAMPLES, r->accelMin, r->accelMax, v);
}

// v^2 * Cd / mass: the aero drag every integral below is against.
static float drag_at(const RidePhys* r, float v) {
	return (v * v * rd(r->cpi, 0x260)) / rd(r->cpi, 0x110);
}

// ---------------------------------------------------------------------------

// RebuildPhysicsInfo (0x801C89A8) up to the class. `spec`, when given, is
// bound to the car and, with `tunings`, carries RebuildCustomTunings.
static void build(RidePhys* out, CustTune* spec, const u8* compleat, const PerfDb* pdb,
		const u8* installed, const int* junkman, const u8* tunings) {
	memset(out, 0, sizeof(*out));
	if (spec)
		memset(spec, 0, sizeof(*spec));
	if (!compleat)
		return;
	const u8* C = compleat;
	u8* P = out->cpi;
	// BuildCarPhysicsInfo(.., rebuild = 1): no InstallAllPerfParts, every
	// level zeroed and then set again by its own builder's ComputeUpgradeLevel.
	memcpy(P, C, RIDEPHYS_CPI_SIZE);
	float comp[RIDEPHYS_PATHS][PERFDB_UPGRADE_COMPONENTS];
	float lvl[RIDEPHYS_PATHS];
	const bool parts = pdb && installed;
	for (int t = 0; t < RIDEPHYS_PATHS; t++) {
		lvl[t] = parts ? perfdb_upgrade_components(pdb, installed, t, comp[t]) : 0.0f;
		if (!parts)
			memset(comp[t], 0, sizeof(comp[t]));
		float n = lvl[t];
		if (n < 0.0f) n = 0.0f;
		if (n > 1.0f) n = 1.0f;
		out->level[t] = n * 3.0f;
	}
	build_weight(C, P, lvl[PERF_PKG_WEIGHT_REDUCTION]);
	build_drivetrain(C, P, lvl[PERF_PKG_TRANSMISSION], comp[PERF_PKG_TRANSMISSION]);
	build_nitrous(C, P, lvl[PERF_PKG_NITROUS], comp[PERF_PKG_NITROUS]);
	build_engine(C, P, lvl[PERF_PKG_ENGINE]);
	build_turbo(C, P, lvl[PERF_PKG_TURBO]);
	build_suspension(C, P, lvl[PERF_PKG_SUSPENSION], comp[PERF_PKG_SUSPENSION]);
	build_brakes(C, P, lvl[PERF_PKG_BRAKES]);
	build_ecu(C, P, lvl[PERF_PKG_ECU], out->level[PERF_PKG_ECU], parts ? installed : NULL);
	build_tires(C, P, lvl[PERF_PKG_TIRES]);
	// MatchUpgradeLevel's aero step is skipped: HookupVisualAeroPartsToPhysics
	// (0x8046E490) ships as 1.
	build_aero(C, P, lvl[PERF_PKG_JUNKMAN], comp[PERF_PKG_JUNKMAN]);
	build_inertia(P);
	if (junkman)
		ridephys_apply_junkman(C, P, junkman);
	// The custom tunings, over the car with its junkman parts on: the
	// CustomPhysicsSpec at RidePhysicsInfo +0x368, on this car and its
	// PhysicsUpgradeSpecification, with the RideInfo's tuning block.
	if (spec) {
		custtune_init(spec, C, P, parts ? installed : NULL, junkman, tunings);
		if (custtune_num_active(spec) > 0)
			custtune_rebuild(spec);
	}
	make_accel_table(out);
	// RebuildPhysicsInfo's own integral: the acceleration left over the drag,
	// 3 mph a step, until the drag wins.
	float m = 0.0f;
	for (float v = out->accelMin; v <= out->accelMax; ) {
		const float a = accel_at(out, v), d = drag_at(out, v);
		if (a < d)
			break;
		v += STEP_3MPH;
		m += (a - d) * STEP_3MPH;
	}
	out->measure = m;
}

void ridephys_build(RidePhys* out, const u8* compleat, const PerfDb* pdb,
		const u8* installed, const int* junkman) {
	build(out, NULL, compleat, pdb, installed, junkman, NULL);
}

void ridephys_build_tuned(RidePhys* out, const u8* compleat, const PerfDb* pdb,
		const u8* installed, const int* junkman, const u8* tunings) {
	if (!tunings) {
		build(out, NULL, compleat, pdb, installed, junkman, NULL);
		return;
	}
	CustTune spec;
	build(out, &spec, compleat, pdb, installed, junkman, tunings);
}

void ridephys_build_spec(RidePhys* out, CustTune* spec, const u8* compleat, const PerfDb* pdb,
		const u8* installed, const int* junkman, const u8* tunings) {
	build(out, spec, compleat, pdb, installed, junkman, tunings);
}

float ridephys_top_speed(const RidePhys* r) {
	float v = r->accelMin;
	while (v <= r->accelMax) {
		if (accel_at(r, v) < drag_at(r, v))
			break;
		v += STEP_3MPH;
	}
	// The governor, in mph (0x80332C90 is 1 mph in m/s), caps it.
	const float gov = rd(r->cpi, 0x290);
	if (gov > 0.0f && gov * 0.447027f < v)
		v = gov * 0.447027f;
	return v;
}

float ridephys_accel_measure(const RidePhys* r) {
	float m = 0.0f;
	float v = 0.0f;
	do {
		const float a = accel_at(r, v), d = drag_at(r, v);
		if (a < d)
			return m;
		v += STEP_3MPH;
		m += (a - d) * STEP_3MPH;
	} while (v < MPH100);
	return m;
}

// OverallNOSEffect (0x80470960): how much of the accel and top-speed ratings
// the nitrous carries.
static const float kNosEffect[2] = { 0.1f, 0.085f };
// HandlingVariableWeights (0x80333648), one per handling term below.
static const float kHandlingWeight[9] = { 0.14f, 0.1f, 0.15f, 0.15f, 0.1f, 0.15f, 0.15f, 0.08f,
	0.08f };

static float norm(float v, const float* e, int lo) {
	return (v - e[lo]) / (e[lo + 1] - e[lo]);
}

static float clamp_rating(float v) {
	if (v - 0.03f < 0.0f) v = 0.03f;
	if (1.0f - v < 0.0f) v = 1.0f;
	return v;
}

// The nitrous's share: its strength times its capacity, over Extrema [8]/[9].
static float nos_term(const RidePhys* r, const float* e) {
	return norm(rd(r->cpi, 0x2A0) * (rd(r->cpi, 0x2AC) + 0.0f), e, 8);
}

float ridephys_rating(const RidePhys* r, const float* e, int which) {
	const u8* P = r->cpi;
	switch (which) {
	case RIDEPHYS_RATING_TOP_SPEED:
		// GetTopSpeedRating: the theoretical top speed over Extrema [0]/[1].
		return clamp_rating(kNosEffect[1] * nos_term(r, e) +
			(1.0f - kNosEffect[1]) * norm(ridephys_top_speed(r), e, 0));
	case RIDEPHYS_RATING_ACCEL: {
		// GetAccelRating: the 0-100 mph measure over Extrema [2]/[3], plus a
		// share for the transmission's upgrade level.
		const int gearbox = (int)(r->level[PERF_PKG_TRANSMISSION] + 0.5f);
		return clamp_rating(((float)gearbox / 3.0f) * 0.08f + kNosEffect[0] * nos_term(r, e) +
			(1.0f - (kNosEffect[0] + 0.08f)) * norm(ridephys_accel_measure(r), e, 2));
	}
	case RIDEPHYS_RATING_HANDLING: {
		// GetHandlingRating: nine terms, each a CarPhysicsInfo figure over its
		// Extrema range, weighted.
		const float mass = rd(P, 0x110);
		float t[9];
		t[0] = norm(((((rd(P, 0xDC) + 0.0f) + rd(P, 0xE0)) + 0.0f) * 0.5f +
			(((rd(P, 0xFC) + 0.0f) + rd(P, 0x100)) + 0.0f) * 0.5f) * 0.5f / mass, e, 14);
		t[1] = norm((rd(P, 0x274) * 0.65f + rd(P, 0x278) * 0.35f) / mass, e, 16);
		t[2] = norm(((rd(P, 0x174) * rd(P, 0x170) + rd(P, 0x180)) * 0.5f +
			(rd(P, 0x194) * rd(P, 0x190) + rd(P, 0x1A0)) * 0.5f) * 0.5f / mass, e, 12);
		t[3] = 1.0f - norm(mass, e, 18);
		const float split = rd(P, 0x1C0);
		t[4] = split <= 0.6f ? (split + 0.2f) / 0.8f : (1.0f - split) / 0.4f;
		t[5] = 1.0f - norm(rd(P, 0x008), e, 20);
		t[6] = 1.0f - norm(rd(P, 0x160), e, 22);
		t[7] = norm(rd(P, 0x264), e, 24);
		t[8] = norm(rd(P, 0x268), e, 26);
		float sum = 0.0f;
		for (int k = 0; k < 9; k++)
			sum = t[k] * kHandlingWeight[k] + sum;
		return clamp_rating(sum);
	}
	default:
		return 0.0f;
	}
}

void ridephys_ratings(const RidePhys* r, const float* extrema, float out[RIDEPHYS_RATINGS]) {
	for (int k = 0; k < RIDEPHYS_RATINGS; k++)
		out[k] = ridephys_rating(r, extrema, k);
}

// The sweep GetMaxTorque and GetMaxHP share: the torque curve -- nine samples
// from idle (+0x1F0) to the limiter (+0x1F8), at +0x200 -- read from idle in
// 50 rpm steps while below the limiter, the first highest value kept. Power is
// each sample * 1000 * rpm * (1 / 9550), SPEED2.EXE's constant 0x79BEF8 (Speed.elf
// divides by 9550). With `e`, PC's normalising tail: the peak clamped to
// e[lo]..e[lo + 1], then its share of that range.
static float engine_peak(const RidePhys* r, bool power, float* rpm, const float* e, int lo) {
	const u8* P = r->cpi;
	const float idle = rd(P, 0x1F0), limit = rd(P, 0x1F8);
	float curve[9];
	memcpy(curve, P + 0x200, sizeof(curve));
	float best = 0.0f, at = 0.0f;
	for (float x = idle; x < limit; x += 50.0f) {
		float v = ridephys_table_get(curve, 9, idle, limit, x);
		if (power)
			v = v * 1000.0f * x * 0.00010471204f;
		if (v > best) {
			best = v;
			at = x;
		}
	}
	if (rpm)
		*rpm = at;
	if (!e)
		return best;
	if (!(best > e[lo]))
		best = e[lo];
	if (!(e[lo + 1] > best))
		best = e[lo + 1];
	return (best - e[lo]) / (e[lo + 1] - e[lo]);
}

float ridephys_max_torque(const RidePhys* r, float* rpm, const float* extrema) {
	return engine_peak(r, false, rpm, extrema, 4);
}

float ridephys_max_hp(const RidePhys* r, float* rpm, const float* extrema) {
	return engine_peak(r, true, rpm, extrema, 6);
}

// A performance-config table row: the measure at +0x00, the part bits from
// +0x05 (ridephys.h).
#define PACKED_ROW 0x10
// MatchPerformance's handling weight (0x80332CA0).
#define MATCH_HANDLING_WEIGHT 150.0f

void ridephys_unpack_row(const u8* row, u8* installed) {
	memset(installed, 0, PERFDB_NUM_PARTS);
	for (int id = 0; id < PERFDB_NUM_PARTS; id++)
		if ((row[5 + (id >> 3)] >> (7 - (id & 7))) & 1)
			installed[id] = 1;
}

void ridephys_match_performance(RidePhys* out, u8* installed, const u8* compleat,
		const PerfDb* pdb, const u8* rows, int nrows, float measure, float handling,
		const float* extrema) {
	if (!rows || nrows <= 0 || !pdb)
		return;
	bool again = handling >= 0.0f && extrema;
	for (;;) {
		const u8* pick = rows;
		if (nrows > 1) {
			int i = 0;
			while (i + 1 < nrows - 1 && !(measure < rd(rows, (i + 1) * PACKED_ROW)))
				i++;
			const float dLo = fabsf(rd(rows, i * PACKED_ROW) - measure);
			const float dHi = fabsf(rd(rows, (i + 1) * PACKED_ROW) - measure);
			pick = rows + (dLo < dHi ? i : i + 1) * PACKED_ROW;
		}
		ridephys_unpack_row(pick, installed);
		ridephys_build(out, compleat, pdb, installed, NULL);
		if (!again)
			return;
		again = false;
		measure += (handling - ridephys_rating(out, extrema, RIDEPHYS_RATING_HANDLING)) *
			MATCH_HANDLING_WEIGHT;
	}
}

// ---------------------------------------------------------------------------
// The drag (#268 D)

int ridephys_car_class(const RidePhys* r) {
	static const float kClassRange[4] = { 0.0f, 273.0f, 401.0f, 500.0f };
	if (r->measure > kClassRange[3])
		return 0;
	for (int i = 2; i >= 0; i--)
		if (r->measure > kClassRange[i])
			return 3 - i;
	return 3;
}

// SpecialModeTunings: [0..10] at level 0 and [11..21] at the top. [0] adds
// to the brake bias +0x27C (brakes path), [1] to the drive split +0x1C0
// (transmission), [2]/[3] scale the aero's front and rear downforce areas
// +0x264/+0x268 (junkman), [4] the Izz scale +0x160 and [5] the mass
// (weight), [6] comes off the centre of gravity's height +8 (suspension),
// [7..10] scale each TireInfo's +0xC/+0x10 (tyres).
static const float kDragTunings[22] = {
	0.0f, 0.0f, 1.2f, 1.2f, 0.9f, 0.95f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
	0.0f, 0.0f, 1.4f, 1.4f, 0.9f, 0.95f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f
};

// The drift's SpecialModeTunings, TheMiscCarTunings +0x3C, which differ by
// record (GetMiscCarTunings by the drive split +0x1C0: [0] under 0.2, front
// drive; [2] over 0.8, rear drive; [1] between): a front-drive car gets 0.15
// more rear drive and tyres down to 0.9 at the top tyre level, a rear-drive
// car 0.15 less, an all-wheel-drive car's tyres' +0x10 up to 1.1.
static const float kDriftTunings[3][22] = {
	{ 0.0f, 0.15f, 1.0f, 1.0f, 0.95f, 1.0f, 0.0f, 0.95f, 0.95f, 1.0f, 1.0f,
	  0.0f, 0.15f, 1.0f, 1.0f, 0.95f, 1.0f, 0.0f, 0.9f, 0.9f, 0.9f, 0.9f },
	{ 0.0f, 0.0f, 1.0f, 1.0f, 0.95f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
	  0.0f, 0.0f, 1.0f, 1.0f, 0.9f, 1.0f, 0.0f, 1.0f, 1.0f, 1.1f, 1.1f },
	{ 0.0f, -0.15f, 1.0f, 1.0f, 0.95f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
	  0.0f, -0.15f, 1.0f, 1.0f, 0.95f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
};

// Street X's SpecialModeTunings, TheMiscCarTunings +0x94, by the same record
// (SPEED2.EXE's copy of the table, at file offset 0x3FC33C, holds the same
// floats). Each record raises the downforce, more at the rear and more at the
// top junkman level. It lowers the Izz scale and the centre of gravity (0.02 m
// at level 0) and puts 6 to 35 per cent more on the tyres. The all-wheel-drive
// record also takes 0.015 off the brake bias (0.01 at the top) and 1.5 to
// 5 per cent off the mass.
static const float kShortTrackTunings[3][22] = {
	{ 0.0f, 0.0f, 1.1f, 1.1f, 0.92f, 1.0f, 0.02f, 1.08f, 1.1f, 1.1f, 1.09f,
	  0.0f, 0.0f, 1.12f, 1.4f, 0.88f, 0.95f, 0.02f, 1.1f, 1.12f, 1.15f, 1.1f },
	{ -0.015f, 0.0f, 1.07f, 1.4f, 0.88f, 0.985f, 0.02f, 1.12f, 1.1f, 1.35f, 1.12f,
	  -0.01f, 0.0f, 1.15f, 1.3f, 0.95f, 0.95f, 0.03f, 1.15f, 1.075f, 1.3f, 1.075f },
	{ 0.0f, 0.0f, 1.13f, 1.35f, 0.9f, 1.0f, 0.02f, 1.06f, 1.1f, 1.1f, 1.1f,
	  0.0f, 0.0f, 1.3f, 1.6f, 0.8f, 0.9f, 0.0f, 1.15f, 1.15f, 1.14f, 1.1f },
};

// GetMiscCarTunings (CarPhysicsInfo): TheMiscCarTunings' record by the drive
// split +0x1C0, read before any tuning moves it.
static int misc_record(const RidePhys* r) {
	const float split = rd(r->cpi, 0x1C0);
	return split < 0.2f ? 0 : (split > 0.8f ? 2 : 1);
}

// ApplySpecialTunings (0x801C5C34) over r->cpi, the inertia rebuilt after.
static void apply_special_tunings(RidePhys* r, const float* t) {
	u8* P = r->cpi;
	float n = r->level[PERF_PKG_JUNKMAN] / 3.0f;
	wr(P, 0x264, rd(P, 0x264) * lerp(t[2], t[13], n));
	wr(P, 0x268, rd(P, 0x268) * lerp(t[3], t[14], n));
	n = r->level[PERF_PKG_WEIGHT_REDUCTION] / 3.0f;
	wr(P, 0x160, rd(P, 0x160) * lerp(t[4], t[15], n));
	wr(P, 0x110, rd(P, 0x110) * lerp(t[5], t[16], n));
	n = r->level[PERF_PKG_SUSPENSION] / 3.0f;
	wr(P, 0x8, rd(P, 0x8) - lerp(t[6], t[17], n));
	n = r->level[PERF_PKG_TIRES] / 3.0f;
	for (int i = 0; i < 2; i++) {
		const int a = 0xD0 + i * 0x20;
		wr(P, a + 0xC, rd(P, a + 0xC) * lerp(t[7 + i], t[18 + i], n));
		wr(P, a + 0x10, rd(P, a + 0x10) * lerp(t[9 + i], t[20 + i], n));
	}
	n = r->level[PERF_PKG_BRAKES] / 3.0f;
	wr(P, 0x27C, rd(P, 0x27C) + lerp(t[0], t[11], n));
	float nos = rd(P, 0x27C);
	nos = nos < 0.0f ? 0.0f : (nos > 1.0f ? 1.0f : nos);
	n = r->level[PERF_PKG_TRANSMISSION] / 3.0f;
	wr(P, 0x1C0, rd(P, 0x1C0) + lerp(t[1], t[12], n));
	float s8 = rd(P, 0x8);
	wr(P, 0x8, s8 < 0.0f ? 0.0f : s8);
	wr(P, 0x27C, nos);
	build_inertia(P);
}

void ridephys_tune_for_drag(RidePhys* r) {
	apply_special_tunings(r, kDragTunings);
	make_accel_table(r);
}

void ridephys_tune_for_drift(RidePhys* r) {
	apply_special_tunings(r, kDriftTunings[misc_record(r)]);
	wr(r->cpi, 0x2A0, 0.0f);   // no nitrous in a drift
	make_accel_table(r);
}

void ridephys_tune_for_short_track(RidePhys* r) {
	apply_special_tunings(r, kShortTrackTunings[misc_record(r)]);
	wr(r->cpi, 0x2A0, 0.0f);   // DAT_80332BA8: no nitrous in Street X
	make_accel_table(r);
}

// ComputeMiscStats (Speed.elf 0x801C8BF8), what the dyno's results screen
// shows (#417 E): from rest, a 1/60 s step, the torque curve through the
// current gear over the mass less the aero drag, changing up at each shift
// point less ShiftPoint offset 100 (r13-0x694C, -100) with the clutch out at
// ClutchSpeed[0] (-3 a second) and back in at [2] (1.5), no drive while it
// moves. 0-60 and 0-100 mph are the times taken times the car's
// TimingStatsKludgeFactor060 / 0100 (0x8035FC1C / 0x8035FCD4, by car type);
// the quarter mile (400.5 m) its time and speed. The constants are
// 0x80332C64..0x80332C84. As retail, the shift test is skipped on the step
// after a clutch step, so a change alternates clutch and drive steps.
// PORT: GetValue's argument is lost in the decompile; it is taken as the
// engine speed the shift test computes. A gear past the top one never
// changes up (retail reads past the shift table there).
static const float kKludge060[31] = {
	1.02f, 1.164f, 1.056f, 1.32f, 1.152f, 1.068f, 1.248f, 1.044f, 1.224f, 1.08f,
	1.08f, 1.056f, 1.164f, 1.272f, 1.296f, 1.14f, 1.092f, 1.212f, 1.14f, 1.092f,
	1.236f, 1.284f, 1.344f, 1.176f, 1.212f, 0.972f, 0.96f, 1.008f, 0.81f, 1.008f,
	0.984f };
static const float kKludge0100[31] = {
	1.22f, 1.31f, 1.22f, 1.3f, 1.15f, 1.18f, 1.27f, 1.09f, 1.26f, 1.24f,
	1.38f, 1.15f, 1.18f, 1.28f, 1.27f, 1.05f, 1.12f, 1.06f, 1.17f, 1.31f,
	1.0f, 1.28f, 1.18f, 1.17f, 1.26f, 1.06f, 1.15f, 1.11f, 1.1f, 1.21f,
	1.0f };

void ridephys_misc_stats(const RidePhys* r, int carType, float* t60, float* t100,
		float* qmTime, float* qmSpeed) {
	const u8* P = r->cpi;
	const float split = rd(P, 0x1C0), mass = rd(P, 0x110), cd = rd(P, 0x260);
	const float radius = (1.0f - split) * rd(P, 0x20) + split * rd(P, 0x80);
	const float fd = (1.0f - split) * rd(P, 0x1B8) + split * rd(P, 0x1BC);
	const int gears = rdi(P, 0x1C8);
	const float k60 = carType >= 0 && carType < 31 ? kKludge060[carType] : 1.0f;
	const float k100 = carType >= 0 && carType < 31 ? kKludge0100[carType] : 1.0f;
	const float dt = 0.0166667f;
	float up[10];
	shift_points(P, up);
	float torque[9];
	for (int k = 0; k < 9; k++)
		torque[k] = rd(P, 0x200 + 4 * k);
	const float tLo = rd(P, 0x1F0), tHi = rd(P, 0x1F8);
	*t60 = *t100 = *qmTime = *qmSpeed = 0.0f;
	if (radius <= 0.0f || mass <= 0.0f)
		return;
	int gear = 2;
	float clutch = 1.0f, v = 0.0f, x = 0.0f, t = 0.0f;
	bool out = true, shifting = false;
	for (int it = 0; it < 999999; it++) {
		const bool was = shifting;
		shifting = false;
		const float ratio = rd(P, 0x1D0 + 4 * gear);
		const float rpm = (v / radius) * fd * ratio * RAD_TO_RPM;
		if (!was && gear < gears + 1 && gear < 9 && up[gear] - 100.0f <= rpm)
			shifting = true;
		float a = 0.0f;
		if (shifting) {
			clutch += (out ? -3.0f : 1.5f) * dt;
			if (clutch > 0.0f) {
				if (clutch >= 1.0f) {
					out = true;
					shifting = false;
					gear++;
					clutch = 1.0f;
				}
			} else {
				out = false;
				clutch = 0.0f;
			}
		} else {
			a = ridephys_table_get(torque, 9, tLo, tHi, rpm) * fd * ratio / radius / mass;
		}
		t += dt;
		a -= (v * v * cd) / mass;
		x += v * dt + a * 0.5f * dt * dt;
		v += a * dt;
		if (*t60 == 0.0f && 60.0f <= v * 2.237f)
			*t60 = t * k60;
		if (*t100 == 0.0f && 100.0f <= v * 2.237f)
			*t100 = t * k100;
		if (*qmTime == 0.0f && 400.5f <= x) {
			*qmTime = t;
			*qmSpeed = v;
		}
		if (*t60 != 0.0f && *t100 != 0.0f && *qmTime != 0.0f && *qmSpeed != 0.0f)
			break;
		if (v <= 0.0f && it > 600)
			break;   // PORT: a car that cannot move at all
	}
}
