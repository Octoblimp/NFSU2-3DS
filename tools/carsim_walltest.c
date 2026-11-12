// Offline checks of the car model (TASKS.md #150, #222): carsim.c, unmodified,
// driven on a flat plane (or a tilted one) and against one straight wall. The
// box-vs-wall resolve is world.c's own code, pulled in by carsim_walltest.py
// rather than copied, so a change there is a change here. Every case prints
// the car's track and a line of measured results.
//
// The car is the one carsim_init() falls back to without a record: the 350Z's
// own shipped physics block, so the numbers are a real car's.
//
// Built and run by tools/carsim_walltest.py; not part of the 3DS build.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include "carsim.h"
#include "carhull.h"
#include "aidriver.h"

// The two world.h/model3d.h types the extracted resolver needs, field for field.
typedef struct {
	float x0, y0, x1, y1, zmin, zmax, dist2;
	float z0min, z0max, z1min, z1max;
} Model3DWall;
typedef struct {
	int gathered, blocking;
	float firstZLo, firstZHi;
} WorldCollideInfo;

#include "world_resolve.inc"   // world.c, COLLIDE_MAX_WALLS .. resolveBox()
#include "carperf.inc"         // cardb.c, carperf_torque_at() .. carperf_rpm_at_speed()

#define WORLD_FOOTPRINT 0.92f  // main.c's, the footprint as a fraction of the body box

// The wall is the line y = 0 from x = -500 to 500, when it is up.
static Model3DWall g_wall[1] = { { -500.0f, 0.0f, 500.0f, 0.0f, -10.0f, 10.0f, 0.0f,
	-10.0f, 10.0f, -10.0f, 10.0f } };
static bool g_wallUp;
static int g_hits;
static float g_slope;   // ground z = x * g_slope
// Past the wall (y < 0), the only surface is a deck this high, when it is
// non-zero: what a section tree hands back past a kerb with a road over it
// (TASKS.md #251).
static float g_deck;
// A kerb (TASKS.md #266): ground g_curbH higher for y > g_curbY, when non-zero.
static float g_curbH, g_curbY;

static int probe(void* ctx, const float* xy, int n, float zMax, float* outZ, bool* outHit,
	float* outN, signed char* outTerrain)
{
	(void)ctx; (void)zMax;
	float l = sqrtf(1.0f + g_slope * g_slope);
	for (int i = 0; i < n; i++) {
		if (outTerrain) outTerrain[i] = 1;
		if (g_deck != 0.0f && xy[i * 2 + 1] < 0.0f) {
			outZ[i] = g_deck;
			outHit[i] = true;
			if (outN) {
				outN[i * 3] = outN[i * 3 + 1] = 0.0f;
				outN[i * 3 + 2] = 1.0f;
			}
			continue;
		}
		outZ[i] = xy[i * 2] * g_slope;
		if (g_curbH != 0.0f && xy[i * 2 + 1] > g_curbY)
			outZ[i] += g_curbH;
		outHit[i] = true;
		if (outN) {
			outN[i * 3] = -g_slope / l;
			outN[i * 3 + 1] = 0.0f;
			outN[i * 3 + 2] = 1.0f / l;
		}
	}
	return n;
}

// world_collide()'s substep loop, minus the gather.
static bool collide(void* ctx, const float from[3], float to[3], float yaw,
	float halfLength, float halfWidth, float* outNx, float* outNy, float outArm[2])
{
	(void)ctx;
	if (!g_wallUp) return false;
	halfLength *= WORLD_FOOTPRINT;
	halfWidth *= WORLD_FOOTPRINT;
	outArm[0] = outArm[1] = 0.0f;
	float dx = to[0] - from[0], dy = to[1] - from[1];
	float travel = sqrtf(dx * dx + dy * dy);
	float thin = halfLength < halfWidth ? halfLength : halfWidth;
	int steps = 1;
	if (thin > 0.0f && travel > thin * 0.5f) {
		steps = (int)(travel / (thin * 0.5f)) + 1;
		if (steps > COLLIDE_MAX_STEPS) steps = COLLIDE_MAX_STEPS;
	}
	float fx = cosf(yaw), fy = sinf(yaw);
	float x = from[0], y = from[1];
	float sx = dx / (float)steps, sy = dy / (float)steps;
	float nx = 0.0f, ny = 0.0f, deepest = 0.0f;
	bool pushed = false;
	for (int i = 0; i < steps; i++) {
		x += sx;
		y += sy;
		if (resolveBox(g_wall, 1, fx, fy, halfLength, halfWidth, &x, &y, 0.0f, 3.0f,
				&deepest, &nx, &ny, outArm, NULL))
			pushed = true;
	}
	to[0] = x;
	to[1] = y;
	if (pushed) { *outNx = nx; *outNy = ny; g_hits++; }
	return pushed;
}

static CarSimWorld g_w = { probe, collide, NULL };
static float kDt = 1.0f / 30.0f;

// CARSIM_TP=1: the body meets the world as retail's does (TASKS.md #223,
// carsim.c "Track polygons") -- the same plane, or the deck past the wall,
// as 4 m road quads, and the wall as an unbounded wall polygon -- in place of
// the planes and the footprint resolver. The wheels still take probe().
#define TP_CELL    4.0f
#define TP_WALL_ID 0u
static bool g_tp;

static u32 tpCellId(int i, int j) {
	return ((u32)(i + 1024) << 11 | (u32)(j + 1024)) + 1u;
}

static void tpCellBox(int i, int j, float* b) {
	b[0] = (float)i * TP_CELL;
	b[1] = (float)j * TP_CELL;
	b[2] = b[0] + TP_CELL;
	b[3] = b[1] + TP_CELL;
}

static bool tpPoly(void* ctx, u32 id, TrackPoly* p) {
	(void)ctx;
	memset(p, 0, sizeof(*p));
	p->nv = 4;
	p->terrain = 1;
	if (id == TP_WALL_ID) {
		// Unbounded: z -2048 / +2048 / +2048 / -2048, as TrackPolygon decodes
		// one. Single-sided (group 0), facing +y, where the car starts.
		if (!g_wallUp) return false;
		const float v[4][3] = { { -500.0f, 0.0f, -2048.0f }, { -500.0f, 0.0f, 2048.0f },
			{ 500.0f, 0.0f, 2048.0f }, { 500.0f, 0.0f, -2048.0f } };
		memcpy(p->v, v, sizeof(v));
		p->n[1] = 1.0f;
		p->box[0] = -500.0f;
		p->box[2] = 500.0f;
		p->flags = 0x02 | 0x04 | 0x10;
		return true;
	}
	id -= 1u;
	const int i = (int)(id >> 11) - 1024, j = (int)(id & 2047u) - 1024;
	tpCellBox(i, j, p->box);
	const bool deck = g_deck != 0.0f && p->box[1] < 0.0f;
	const float xs[4] = { p->box[0], p->box[2], p->box[2], p->box[0] };
	const float ys[4] = { p->box[1], p->box[1], p->box[3], p->box[3] };
	for (int k = 0; k < 4; k++) {
		p->v[k][0] = xs[k];
		p->v[k][1] = ys[k];
		p->v[k][2] = deck ? g_deck : xs[k] * g_slope;
	}
	const float sl = deck ? 0.0f : g_slope, l = sqrtf(1.0f + sl * sl);
	p->n[0] = -sl / l;
	p->n[2] = 1.0f / l;
	p->d = p->v[0][0] * p->n[0] + p->v[0][2] * p->n[2];
	p->flags = 0x10;
	p->group = 1;   // two-sided: the side the car is on
	return true;
}

static int tpPolys(void* ctx, const float box[4], u32* ids, float* boxes, int max) {
	(void)ctx;
	int n = 0;
	const int i0 = (int)floorf(box[0] / TP_CELL), i1 = (int)floorf(box[2] / TP_CELL);
	const int j0 = (int)floorf(box[1] / TP_CELL), j1 = (int)floorf(box[3] / TP_CELL);
	for (int i = i0; i <= i1; i++)
		for (int j = j0; j <= j1 && n < max; j++) {
			ids[n] = tpCellId(i, j);
			tpCellBox(i, j, &boxes[n * 4]);
			n++;
		}
	if (g_wallUp && n < max && box[1] <= 0.0f && box[3] >= 0.0f && box[0] <= 500.0f &&
	    box[2] >= -500.0f) {
		ids[n] = TP_WALL_ID;
		const float b[4] = { -500.0f, 0.0f, 500.0f, 0.0f };
		memcpy(&boxes[n * 4], b, sizeof(b));
		n++;
	}
	return n;
}

// Every case's step. With the track polygons a step in which a side of the
// car touched a wall is a hit, as a step the footprint resolver pushed is.
static void wtUpdate(CarSim* s, const CarSimInput* in, const CarSimWorld* w, float dt) {
	carsim_update(s, in, w, dt);
	if (!g_tp) return;
	for (int c = 0; c < 4; c++)
		if (s->touchLast[c] == s->clock && s->touchWith[c] == CARSIM_TOUCH_WALL) {
			g_hits++;
			break;
		}
}
#define carsim_update wtUpdate

// Sets the car rolling straight ahead at `speed`, wheels and engine to match,
// in whichever gear puts the engine nearest the middle of its range.
static void roll(CarSim* s, float speed) {
	for (int k = 0; k < 3; k++) s->P[k] = s->rot[0][k] * speed * s->mass;
	for (int i = 0; i < CARPHYS_WHEELS; i++) s->wheel[i].w = speed / s->wheel[i].radius;
	s->rbVel[0] = s->rot[0][0] * speed;
	s->rbVel[1] = s->rot[0][1] * speed;
	s->rbVel[2] = 0.0f;
	s->vfwd = s->speedAbs = speed;
	// Engine rad/s in gear g (2 = 1st): the rear wheels through the rear final
	// drive (DTI +0x0C) and the gear's ratio (DTI +0x20).
	const float wheelW = speed / s->wheel[2].radius;
	const float rearFinal = s->cpi[(0x1B0 + 0x0C) >> 2];
	int best = s->perf.numGears + 1;
	for (int g = 2; g <= s->perf.numGears + 1; g++) {
		float rpm = wheelW * rearFinal * s->cpi[(0x1D0 >> 2) + g] * 9.5493f;
		if (rpm < s->perf.redlineRpm - 1500.0f) { best = g; break; }
	}
	s->dtGear = s->desired = best;
	s->engW = wheelW * rearFinal * s->cpi[(0x1D0 >> 2) + best];
	if (s->engW < s->perf.idleRpm * 0.10472f) s->engW = s->perf.idleRpm * 0.10472f;
	s->rpmSmooth = s->engW * 9.5493f;
	s->clutchState = 3;
	s->clutch = 1.0f;
}

static void line(const char* tag, float t, const CarSim* s) {
	printf("  %-4s t %5.2f  pos %8.2f,%7.2f,%6.3f  hdg %6.1f  p %5.1f r %5.1f  spd %6.2f (%5.1f mph)  vz %5.2f  gear %d  rpm %5.0f  down %d  hits %d  imp %.2f\n",
		tag, t, s->pos[0], s->pos[1], s->pos[2], s->yaw * 57.29578f, s->pitch * 57.29578f,
		s->roll * 57.29578f, s->speed, s->speed * 2.237f, s->vel[2], s->gear, s->rpm,
		s->wheelsDown, g_hits, s->lastImpact);
}

// The handling mode every case's car drives in (CarSim.controlMode): 1, the
// game's default (Stability Control Off), unless CARSIM_MODE says otherwise.
static int g_mode = 1;

// CARSIM_CAR=<name>: that car's CarPhysicsInfo out of PC Files/GLOBAL/
// GLOBALB.BUN (the CarTypeInfo array, chunk 0x34600, as cardb.c reads it)
// instead of the 350Z every case drives by default.
static CarPhysInfo g_phys;
static bool g_physTried;

static const CarPhysInfo* carPhys(void) {
	const char* want = getenv("CARSIM_CAR");
	if (!want) return NULL;
	if (g_physTried) return g_phys.valid ? &g_phys : NULL;
	g_physTried = true;
	FILE* f = fopen("../PC Files/GLOBAL/GLOBALB.BUN", "rb");
	if (!f) return NULL;
	unsigned char h[8];
	long at = 0;
	while (fseek(f, at, SEEK_SET) == 0 && fread(h, 1, 8, f) == 8) {
		const unsigned id = h[0] | h[1] << 8 | h[2] << 16 | (unsigned)h[3] << 24;
		const long size = (long)(h[4] | h[5] << 8 | h[6] << 16 | (unsigned)h[7] << 24);
		if (id != 0x34600u) {
			at += 8 + size;
			continue;
		}
		const long base = (at + 0x17) & ~0xFL;
		for (long i = 0; base + (i + 1) * 0x890 <= at + 8 + size; i++) {
			unsigned char rec[0x890];
			fseek(f, base + i * 0x890, SEEK_SET);
			if (fread(rec, 1, sizeof(rec), f) != sizeof(rec)) break;
			if (strncmp((const char*)rec, want, 32)) continue;
			memcpy(g_phys.raw, rec + 0x110, CARPHYS_CPI_SIZE);
			g_phys.boostFlag = (s8)rec[0xD8];
			g_phys.valid = true;
			// Its collision hull as the 3DS gets it: CARHULLS.nfsg, packed
			// next to the CARSIM_HULLS path by carsim_walltest.py.
			// CARSIM_HULLS=0 keeps the record's body box.
			const char* hulls = getenv("CARSIM_HULLS");
			if (hulls && strcmp(hulls, "0")) {
				carhull_load(hulls);
				g_phys.hullN = carhull_find(want, &g_phys.hull);
			}
			printf("(%s's CarPhysicsInfo from GLOBALB.BUN, %s)\n", want,
				g_phys.hullN ? "its collision hull" : "the body box");
			break;
		}
		break;
	}
	fclose(f);
	if (!g_phys.valid) printf("(no %s in GLOBALB.BUN; the 350Z)\n", want);
	return g_phys.valid ? &g_phys : NULL;
}

static void fresh(CarSim* s, float x, float y, float yawDeg) {
	float pos[3] = { x, y, x * g_slope };
	carsim_init(s, NULL, NULL, carPhys(), pos, yawDeg * 0.0174533f);
	s->controlMode = g_mode;
	carsim_place(s, &g_w, pos, yawDeg * 0.0174533f);
	g_hits = 0;
	// CARSIM_DRIFT=<skill 0..2>: TheRaceParameters +0x44, the drift's
	// steering, tyres and DriftStabilizer (#268 E). The drift's
	// SpecialModeTunings (ridephys_tune_for_drift) are not applied here.
	if (getenv("CARSIM_DRIFT"))
		carsim_set_drift(s, true, atoi(getenv("CARSIM_DRIFT")));
	// CARSIM_TPV: the body's GJK shape as BuildBoundingVolumes sees it.
	if (getenv("CARSIM_TPV"))
		printf("  (body %s: box %.3f,%.3f,%.3f .. %.3f,%.3f,%.3f from the CG, R %.3f; "
			"CG %.3f,%.3f,%.3f in the model, hRest %.3f)\n",
			s->hullN ? "hull" : "fallback box", s->tpMin[0], s->tpMin[1], s->tpMin[2],
			s->tpMax[0], s->tpMax[1], s->tpMax[2], s->tpR, s->cg[0], s->cg[1], s->cg[2],
			s->hRest);
}

// ---------------------------------------------------------------------------

static void caseRest(void) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	printf("== at rest, no input, 5 s (hRest %.3f, mass %.2f t)\n", s.hRest, s.mass);
	float z0 = s.pos[2];
	for (int f = 0; f <= 150; f++) {
		if (f % 30 == 0) line("", f * kDt, &s);
		carsim_update(&s, &in, &g_w, kDt);
	}
	printf("  RESULT rest: drift %.4f m, z change %.4f m, speed %.4f, dousing %d\n",
		sqrtf(s.pos[0] * s.pos[0] + s.pos[1] * s.pos[1]), s.pos[2] - z0, s.speed, s.dousing);
}

// Full throttle for `onSec`, then off: does the car keep pulling after the
// release (a tester's "keeps giving gas for a sec when I let go")? Every
// frame for the first second after, then every 0.25 s.
static void caseRelease(float onSec) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	printf("== release after %.1f s of full throttle\n", onSec);
	in.throttle = 1.0f;
	const int off = (int)(onSec / kDt + 0.5f);
	float vRel = 0.0f, vMax = 0.0f, tMax = 0.0f;
	for (int f = 0; f <= off + (int)(3.0f / kDt); f++) {
		const float t = f * kDt;
		if (f == off) {
			in.throttle = 0.0f;
			vRel = s.speed;
			vMax = s.speed;
			tMax = 0.0f;
			printf("  -- released at %.2f s, %.2f m/s, gear %d, rpm %.0f\n", t, s.speed, s.gear, s.rpm);
		}
		if (f >= off) {
			const int k = f - off;
			if (k <= (int)(1.0f / kDt) || k % (int)(0.25f / kDt) == 0)
				printf("  +%.3f spd %7.3f gear %d rpm %5.0f thr %.2f engThr %.2f\n", k * kDt,
					s.speed, s.gear, s.rpm, s.thr, s.engThr);
			if (s.speed > vMax) { vMax = s.speed; tMax = k * kDt; }
		}
		carsim_update(&s, &in, &g_w, kDt);
	}
	printf("  RESULT release after %.1f s: %.2f m/s at release, peak %.2f m/s %.3f s later (+%.3f)\n",
		onSec, vRel, vMax, tMax, vMax - vRel);
}

static void caseLaunch(void) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	printf("== launch, full throttle, 60 s (governor %.0f mph, top gearing %.1f mph)\n",
		s.cpi[0x290 >> 2], s.topSpeed * 2.237f);
	in.throttle = 1.0f;
	float t60 = -1.0f, t100k = -1.0f, tQuarter = -1.0f, vQuarter = 0.0f, vmax = 0.0f;
	int lastGear = s.gear;
	for (int f = 0; f <= 1800; f++) {
		float t = f * kDt;
		if (f % 30 == 0 && f <= 600) line("", t, &s);
		else if (f % 300 == 0) line("", t, &s);
		if (s.gear != lastGear) {
			printf("    shift %d -> %d at t %.2f, %.1f mph, rpm %.0f\n", lastGear, s.gear, t,
				s.speed * 2.237f, s.rpm);
			lastGear = s.gear;
		}
		if (t60 < 0.0f && s.speed * 2.237f >= 60.0f) t60 = t;
		if (t100k < 0.0f && s.speed * 3.6f >= 100.0f) t100k = t;
		if (tQuarter < 0.0f && s.pos[0] >= 402.3f) { tQuarter = t; vQuarter = s.speed * 2.237f; }
		if (s.speed > vmax) vmax = s.speed;
		carsim_update(&s, &in, &g_w, kDt);
	}
	printf("  RESULT launch: 0-60 mph %.2f s, 0-100 km/h %.2f s, 1/4 mile %.2f s at %.1f mph, top %.1f mph, lateral drift %.2f m\n",
		t60, t100k, tQuarter, vQuarter, vmax * 2.237f, s.pos[1]);
}

// The manual box (carsim_set_transmission): full throttle from rest with an
// up-shift each time the engine reaches redline, then down to neutral.
static void caseManual(void) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	carsim_set_transmission(&s, 1);
	printf("== manual box: full throttle, shift up at redline %.0f, 30 s, then down to N\n",
		s.perf.redlineRpm);
	in.throttle = 1.0f;
	float t60 = -1.0f;
	int lastGear = s.gear, held = 0, shifts = 0;
	for (int f = 0; f <= 900; f++) {
		float t = f * kDt;
		in.shiftUp = false;
		if (s.gear != lastGear) {
			printf("    gear %d -> %d at t %.2f, %.1f mph, rpm %.0f\n", lastGear, s.gear, t,
				s.speed * 2.237f, s.rpm);
			lastGear = s.gear;
			held = 0;
		}
		// One press, then wait for the box to take it before the next.
		if (s.rpm >= s.perf.redlineRpm && held == 0) { in.shiftUp = true; held = 1; shifts++; }
		if (t60 < 0.0f && s.speed * 2.237f >= 60.0f) t60 = t;
		if (f % 150 == 0) line("", t, &s);
		carsim_update(&s, &in, &g_w, kDt);
	}
	int topGear = s.gear;
	in.throttle = 0.0f;
	in.shiftUp = false;
	for (int k = 0; k < 12; k++) {
		in.shiftDown = k % 2 == 0;
		for (int f = 0; f < 15; f++) {
			carsim_update(&s, &in, &g_w, kDt);
			in.shiftDown = false;
		}
	}
	line("DOWN", 0.0f, &s);
	printf("  RESULT manual: 0-60 mph %.2f s, %d presses, top gear %d, after 6 down-shifts gear %d\n",
		t60, shifts, topGear, s.gear);
}

static void caseBrake(float mph) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	roll(&s, mph / 2.237f);
	printf("== brake from %.0f mph, full brake\n", mph);
	in.brake = 1.0f;
	float x0 = s.pos[0], tStop = -1.0f;
	for (int f = 0; f <= 300; f++) {
		float t = f * kDt;
		if (f % 15 == 0) line("", t, &s);
		carsim_update(&s, &in, &g_w, kDt);
		if (tStop < 0.0f && s.speed < 0.3f) { tStop = t + kDt; line("STOP", tStop, &s); break; }
	}
	printf("  RESULT brake %.0f mph: stop %.2f s, distance %.1f m, heading %.2f deg, lateral %.2f m\n",
		mph, tStop, s.pos[0] - x0, s.yaw * 57.29578f, s.pos[1]);
}

static void caseCorner(float mph, float steer, float thr) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	roll(&s, mph / 2.237f);
	printf("== corner at %.0f mph, steer %.2f, throttle %.2f, 6 s\n", mph, steer, thr);
	in.steer = steer;
	in.throttle = thr;
	float prevYaw = s.yaw, rate = 0.0f;
	for (int f = 0; f <= 180; f++) {
		float t = f * kDt;
		if (f % 15 == 0) {
			line("", t, &s);
			printf("        steer FL %.2f FR %.2f deg  yawRate %.1f deg/s  slip %.1f deg  rear grip1 %.2f\n",
				s.steerFL * 57.29578f, s.steerFR * 57.29578f, rate * 57.29578f,
				(s.yaw - atan2f(s.vel[1], s.vel[0])) * 57.29578f, s.wheel[2].grip1);
		}
		carsim_update(&s, &in, &g_w, kDt);
		float d = s.yaw - prevYaw;
		while (d > 3.14159f) d -= 6.28318f;
		while (d < -3.14159f) d += 6.28318f;
		rate = d / kDt;
		prevYaw = s.yaw;
	}
	float lat = rate * s.speed;
	printf("  RESULT corner %.0f mph steer %.2f: yaw rate %.1f deg/s, speed %.1f mph, lateral %.2f g, radius %.1f m\n",
		mph, steer, rate * 57.29578f, s.speed * 2.237f, lat / 9.8f,
		fabsf(rate) > 0.001f ? s.speed / fabsf(rate) : 0.0f);
}

// A drift (#268 E): at `mph`, a flick of the e-brake with the wheel full
// over, then the throttle held and the wheel at `hold` for 5 s. The slip
// angle and the yaw rate show what the DriftStabilizer and the drift steering
// make of it; run it with and without CARSIM_DRIFT.
static void caseDrift(float mph, float hold, float thr) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	roll(&s, mph / 2.237f);
	printf("== drift at %.0f mph: steer full + e-brake 0.4 s, then throttle %.1f, steer %.2f, 5 s%s\n",
		mph, thr, hold, s.drift ? " -- drift physics on" : "");
	in.steer = 1.0f;
	in.handbrake = true;
	float prevYaw = s.yaw, rate = 0.0f, maxSlip = 0.0f, sumSlip = 0.0f;
	int n = 0;
	for (int f = 0; f <= 162; f++) {
		float t = f * kDt;
		if (f == 12) { in.handbrake = false; in.throttle = thr; in.steer = hold; }
		float slip = s.yaw - atan2f(s.vel[1], s.vel[0]);
		while (slip > 3.14159f) slip -= 6.28318f;
		while (slip < -3.14159f) slip += 6.28318f;
		if (s.speed < 1.0f) slip = 0.0f;
		if (f > 12) {
			if (fabsf(slip) > maxSlip) maxSlip = fabsf(slip);
			sumSlip += fabsf(slip);
			n++;
		}
		if (f % 15 == 0) {
			line("", t, &s);
			printf("        slip %6.1f deg  yawRate %6.1f deg/s  yawAccel %7.1f  steer FL %5.1f  rear grip3 %.2f\n",
				slip * 57.29578f, rate * 57.29578f, s.yawAccel * 57.29578f, s.steerFL * 57.29578f,
				s.wheel[2].grip3);
		}
		carsim_update(&s, &in, &g_w, kDt);
		float d = s.yaw - prevYaw;
		while (d > 3.14159f) d -= 6.28318f;
		while (d < -3.14159f) d += 6.28318f;
		rate = d / kDt;
		prevYaw = s.yaw;
	}
	printf("  RESULT drift %.0f mph hold %.2f%s: slip max %.1f deg, mean %.1f deg; end speed %.1f mph, yaw rate %.1f deg/s\n",
		mph, hold, s.drift ? " (drift)" : "", maxSlip * 57.29578f,
		n ? sumSlip / n * 57.29578f : 0.0f, s.speed * 2.237f, rate * 57.29578f);
}

static void caseReverse(void) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	printf("== reverse: brake held from rest, 4 s, then throttle\n");
	in.brake = 1.0f;
	for (int f = 0; f <= 180; f++) {
		float t = f * kDt;
		if (f == 120) { in.brake = 0.0f; in.throttle = 1.0f; }
		if (f % 15 == 0) line("", t, &s);
		carsim_update(&s, &in, &g_w, kDt);
	}
	printf("  RESULT reverse: x %.2f m, gear %d\n", s.pos[0], s.gear);
}

static void caseHandbrake(void) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	roll(&s, 50.0f / 2.237f);
	printf("== handbrake turn at 50 mph: steer full left + e-brake 0.6 s, then countersteer\n");
	in.steer = 1.0f;
	in.handbrake = true;
	for (int f = 0; f <= 120; f++) {
		float t = f * kDt;
		if (f == 18) { in.handbrake = false; in.throttle = 0.6f; }
		if (f == 40) in.steer = -0.3f;
		if (f % 10 == 0) line("", t, &s);
		carsim_update(&s, &in, &g_w, kDt);
	}
	printf("  RESULT handbrake: heading %.1f deg, speed %.1f mph\n", s.yaw * 57.29578f,
		s.speed * 2.237f);
}

static void caseWall(const char* name, float y0, float yawDeg, float mph, float thr,
	float steer, float seconds)
{
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = true;
	fresh(&s, 0.0f, y0, yawDeg);
	roll(&s, mph / 2.237f);
	printf("== wall, %s: from y %.1f at %.0f deg, %.0f mph, thr %.1f steer %.2f\n",
		name, y0, yawDeg, mph, thr, steer);
	in.throttle = thr;
	in.steer = steer;
	int lastHits = 0;
	float worst = 0.0f, minY = 1e9f;
	int frames = (int)(seconds / kDt);
	for (int f = 0; f <= frames; f++) {
		if (f % 6 == 0 || g_hits != lastHits) line("", f * kDt, &s);
		lastHits = g_hits;
		carsim_update(&s, &in, &g_w, kDt);
		if (s.lastImpact > worst) worst = s.lastImpact;
		if (s.pos[1] < minY) minY = s.pos[1];
	}
	printf("  RESULT wall %s: hits %d, hardest %.2f m/s, min CG y %.2f, end speed %.1f mph heading %.1f\n",
		name, g_hits, worst, minY, s.speed * 2.237f, s.yaw * 57.29578f);
	g_wallUp = false;
}

// TASKS.md #266: the user's hardware log read roll 34.9, 113.4, then 10.4 a
// second apart at 20.7 m/s, full brake, steer 0.94, near a wall. Each run
// prints every frame whose |roll| tops 20 deg and says whether the car went
// past 90 and came back under 20 inside a second.
static void caseRollTrip(float y0, float yawDeg, float mph, float brk, float steer,
	float seconds)
{
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = y0 > 0.0f;
	fresh(&s, 0.0f, y0 > 0.0f ? y0 : 0.0f, yawDeg);
	roll(&s, mph / 2.237f);
	printf("== rolltrip: y %.1f%s at %.0f deg, %.0f mph, brk %.1f steer %.2f\n", y0,
		y0 > 0.0f ? "" : " (no wall)", yawDeg, mph, brk, steer);
	in.brake = brk;
	in.steer = steer;
	float maxR = 0.0f, tPast = -1.0f, tBack = -1.0f;
	const int frames = (int)(seconds / kDt);
	for (int f = 0; f <= frames; f++) {
		const float t = f * kDt;
		carsim_update(&s, &in, &g_w, kDt);
		const float r = fabsf(s.roll * 57.29578f);
		if (r > maxR) maxR = r;
		if (r > 20.0f) line("ROLL", t, &s);
		if (r > 90.0f && tPast < 0.0f) tPast = t;
		if (tPast >= 0.0f && tBack < 0.0f && r < 20.0f) tBack = t;
	}
	printf("  RESULT rolltrip y %.1f hdg %.0f steer %.2f: max |roll| %.1f%s\n", y0, yawDeg,
		steer, maxR, tPast < 0.0f ? "" : tBack >= 0.0f && tBack - tPast < 1.0f
		? "  -- PAST 90 AND BACK IN UNDER 1 S" : "  -- past 90");
	g_wallUp = false;
}

// The same inputs sliding into a kerb `h` high, `dist` m to the car's left,
// heading `yawDeg` (positive turns toward it).
static void caseCurb(float h, float dist, float yawDeg, float mph, float steer) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	g_curbH = h;
	g_curbY = dist;
	fresh(&s, 0.0f, 0.0f, yawDeg);
	roll(&s, mph / 2.237f);
	in.brake = 1.0f;
	in.steer = steer;
	float maxR = 0.0f, tPast = -1.0f, tBack = -1.0f;
	const int frames = (int)(3.0f / kDt);
	for (int f = 0; f <= frames; f++) {
		const float t = f * kDt;
		carsim_update(&s, &in, &g_w, kDt);
		const float r = fabsf(s.roll * 57.29578f);
		if (r > maxR) maxR = r;
		if (r > 20.0f && getenv("CARSIM_CURBV")) line("ROLL", t, &s);
		if (r > 90.0f && tPast < 0.0f) tPast = t;
		if (tPast >= 0.0f && tBack < 0.0f && r < 20.0f) tBack = t;
	}
	printf("  RESULT curb %.2f m at %.0f m, hdg %3.0f, %.0f mph, steer %+.2f: max |roll| %5.1f%s%s\n",
		h, dist, yawDeg, mph, steer, maxR, s.resetRequest ? "  (reset asked)" : "",
		tPast < 0.0f ? "" : tBack >= 0.0f && tBack - tPast < 1.0f
		? "  -- PAST 90 AND BACK IN UNDER 1 S" : "  -- PAST 90");
	g_curbH = 0.0f;
}

static void caseCurbSweep(void) {
	static const float hs[] = { 0.1f, 0.15f, 0.2f, 0.3f };
	static const float yaws[] = { 0.0f, 30.0f, 60.0f, 90.0f };
	for (int a = 0; a < 4; a++)
		for (int b = 0; b < 4; b++)
			for (int c = -1; c <= 1; c += 2)
				caseCurb(hs[a], 3.0f, yaws[b], 46.3f, 0.94f * c);
}

static void caseRollTripSweep(void) {
	static const float ys[] = { 0.0f, 3.0f, 8.0f, 15.0f };
	static const float hs[] = { 0.0f, -30.0f, -60.0f, -90.0f };
	for (int a = 0; a < 4; a++)
		for (int b = 0; b < 4; b++)
			for (int c = -1; c <= 1; c += 2)
				caseRollTrip(ys[a], hs[b], 46.3f, 1.0f, 0.94f * c, 3.0f);
}

// CalculateMaxSteering's post-collision block (TASKS.md #337): a glancing
// scrape at 55 mph, then full lock away from the wall. The lock on offer
// (steerLastMax) for the second after the last touch, against the same run
// with the hit timer held a minute old (`timerOff`, the block never firing).
static void caseHitSteer(bool timerOff) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = true;
	fresh(&s, 0.0f, 4.0f, -20.0f);
	roll(&s, 55.0f / 2.237f);
	printf("== posthit, %s: glancing 20 deg at 55 mph, then steer 1.0 away\n",
		timerOff ? "timer off" : "as retail");
	in.throttle = 0.5f;
	float tHit = -1.0f, sum = 0.0f;
	int n = 0;
	const int frames = (int)(2.5f / kDt);
	for (int f = 0; f <= frames; f++) {
		const float t = f * kDt;
		if (g_hits > 0 && tHit < 0.0f) tHit = t;
		in.steer = tHit >= 0.0f ? 1.0f : 0.0f;
		if (timerOff) s.yawHitAt = s.clock - 60.0f;
		carsim_update(&s, &in, &g_w, kDt);
		const float since = s.clock - s.yawHitAt;
		if (tHit >= 0.0f && f % 3 == 0)
			printf("       t %5.2f  since hit %5.2f  lock %5.2f deg  spd %5.1f mph  top %5.1f mph  hits %d\n",
				t, timerOff ? -1.0f : since, s.steerLastMax, s.speed * 2.237f, s.theoTop * 2.237f, g_hits);
		if (tHit >= 0.0f && t - tHit < 1.0f) { sum += s.steerLastMax; n++; }
	}
	printf("  RESULT posthit %s: mean lock over the first second %.2f deg, theoretical top %.1f mph\n",
		timerOff ? "timer off" : "as retail", n ? sum / n : 0.0f, s.theoTop * 2.237f);
	g_wallUp = false;
}

// The 2026-10-23 hardware lift (TASKS.md #251): the car against a wall with a
// road 10.8 m over the far side. Nothing may stand it on that road.
static void caseDeck(const char* name, float y0, float yawDeg, float mph, float thr,
	float brk, float steer, float seconds)
{
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = true;
	g_deck = 10.8f;
	fresh(&s, 0.0f, y0, yawDeg);
	if (mph > 0.0f) roll(&s, mph / 2.237f);
	printf("== deck past the wall, %s: from y %.1f at %.0f deg, %.0f mph, thr %.1f brk %.1f steer %.2f\n",
		name, y0, yawDeg, mph, thr, brk, steer);
	in.throttle = thr;
	in.brake = brk;
	in.steer = steer;
	int lastHits = 0;
	float zTop = s.pos[2], minY = 1e9f;
	int frames = (int)(seconds / kDt);
	for (int f = 0; f <= frames; f++) {
		if (f % 6 == 0 || g_hits != lastHits) line("", f * kDt, &s);
		lastHits = g_hits;
		carsim_update(&s, &in, &g_w, kDt);
		if (s.pos[2] > zTop) zTop = s.pos[2];
		if (s.pos[1] < minY) minY = s.pos[1];
	}
	printf("  RESULT deck %s: hits %d, highest z %.3f, end z %.3f, min CG y %.2f -- %s\n",
		name, g_hits, zTop, s.pos[2], minY, zTop > 1.0f ? "LIFTED" : "ok");
	g_wallUp = false;
	g_deck = 0.0f;
}

static void caseRoof(void) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	// Turned over about its forward axis and dropped from a little height.
	for (int k = 0; k < 3; k++) {
		s.rot[1][k] = -s.rot[1][k];
		s.rot[2][k] = -s.rot[2][k];
	}
	s.rbPos[2] = 1.5f;
	// Nothing touching, as there would not be in mid-air.
	for (int i = 0; i < CARPHYS_WHEELS; i++) s.wheel[i].onGround = 0;
	s.wheelsDown = 0;
	printf("== on its roof: dropped upside down from CG 1.5 m, 4 s\n");
	float tReset = -1.0f;
	for (int f = 0; f <= 120; f++) {
		float t = f * kDt;
		if (f % 10 == 0) line("", t, &s);
		carsim_update(&s, &in, &g_w, kDt);
		if (s.resetRequest && tReset < 0.0f) {
			tReset = t;
			line("RST", t, &s);
		}
	}
	printf("  RESULT roof: reset requested at %.2f s, rest z %.3f, up.z %.2f\n", tReset,
		s.rbPos[2], s.rot[2][2]);
}

// A car knocked onto its side (a traffic car the player hit, 2026-10-31,
// sank through the road from there; the next build's slid sideways on its
// side and stood itself back up): rolled `deg` about its forward axis and
// dropped from CG 1.5 m, on the MellowMover when `mellow`.
static void caseSide(float deg, bool mellow) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	if (mellow) carsim_set_mellow(&s, true, 0.0f, carsim_mellow_mu(false, 0u));
	const float a = deg * 0.0174533f;
	const float left[3] = { 0.0f, cosf(a), sinf(a) }, up[3] = { 0.0f, -sinf(a), cosf(a) };
	for (int k = 0; k < 3; k++) {
		s.rot[1][k] = left[k];
		s.rot[2][k] = up[k];
	}
	s.rbPos[2] = 1.5f;
	for (int i = 0; i < CARPHYS_WHEELS; i++) s.wheel[i].onGround = 0;
	s.wheelsDown = 0;
	s.dousing = 0;   // set_mellow held it still on its wheels; a held car stays held in the air
	printf("== on its side%s: rolled %.0f deg, dropped from CG 1.5 m, 10 s\n",
		mellow ? " (MellowMover)" : "", deg);
	float lowest = s.rbPos[2], maxUp = -1.0f, drift = 0.0f;
	for (int f = 0; f <= 300; f++) {
		float t = f * kDt;
		if (f % 30 == 0) line("", t, &s);
		carsim_update(&s, &in, &g_w, kDt);
		if (s.rbPos[2] < lowest) lowest = s.rbPos[2];
		if (f > 45 && s.rot[2][2] > maxUp) maxUp = s.rot[2][2];
		const float d = sqrtf(s.rbPos[0] * s.rbPos[0] + s.rbPos[1] * s.rbPos[1]);
		if (d > drift) drift = d;
	}
	printf("  RESULT side %.0f%s: rest z %.3f, lowest %.3f, up.z %.2f (highest after 1.5 s "
		"%.2f), slid %.2f m\n", deg, mellow ? " mellow" : "", s.rbPos[2], lowest, s.rot[2][2],
		maxUp, drift);
}

// A traffic car thrown onto its side by a hit (the user's 2026-10-31 run: "THAT
// CAR I FLIPPED OVER LITERALLY MOONWALKED ON ITS SIDE TO THE LEFT PART OF THE
// ROAD, AND THEN WAS IN THE UPRIGHT POSITION"). On the MellowMover, rolled `deg`,
// moving `vlat` m/s toward its roof side and `vfwd` along, spinning `wroll`
// deg/s about its nose, the AI's `thr` held. 10 s.
// CARSIM_THROWNV: each ground impulse, its lever arm and the turn it gives, in
// the car's own frame (x nose, y left, z up).
static int g_thrownFrame;
static void thrownHook(const CarSim* car, const CarSim* other, CarSimProp* prop, bool ground,
		const float* pos, const float* imp, float slip) {
	(void)other;
	(void)prop;
	if (!car || g_thrownFrame >= 75) return;
	float r[3], J[3], tq[3], rc[3], Jc[3], tc[3];
	for (int k = 0; k < 3; k++) {
		r[k] = pos[k] - car->rbPos[k];
		J[k] = -imp[k];
	}
	tq[0] = r[1] * J[2] - r[2] * J[1];
	tq[1] = r[2] * J[0] - r[0] * J[2];
	tq[2] = r[0] * J[1] - r[1] * J[0];
	for (int i = 0; i < 3; i++) {
		rc[i] = car->rot[i][0] * r[0] + car->rot[i][1] * r[1] + car->rot[i][2] * r[2];
		Jc[i] = car->rot[i][0] * J[0] + car->rot[i][1] * J[1] + car->rot[i][2] * J[2];
		tc[i] = car->rot[i][0] * tq[0] + car->rot[i][1] * tq[1] + car->rot[i][2] * tq[2];
	}
	printf("      %s r %+.2f %+.2f %+.2f  J %+.4f %+.4f %+.4f  turn %+.5f %+.5f %+.5f slip %.1f\n",
		ground ? "ground" : "wall  ", rc[0], rc[1], rc[2], Jc[0], Jc[1], Jc[2], tc[0], tc[1],
		tc[2], slip);
}

static void caseThrown(float deg, float vlat, float vfwd, float wroll, float thr) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	in.throttle = thr;
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	carsim_set_mellow(&s, true, 0.0f, carsim_mellow_mu(false, 0u));
	const float a = deg * 0.0174533f;
	const float left[3] = { 0.0f, cosf(a), sinf(a) }, up[3] = { 0.0f, -sinf(a), cosf(a) };
	for (int k = 0; k < 3; k++) {
		s.rot[1][k] = left[k];
		s.rot[2][k] = up[k];
	}
	s.rbPos[2] = 1.2f;
	for (int i = 0; i < CARPHYS_WHEELS; i++) s.wheel[i].onGround = 0;
	s.wheelsDown = 0;
	s.dousing = 0;
	const float v[3] = { vfwd, -vlat, 0.0f };   // the roof faces -y
	for (int k = 0; k < 3; k++) {
		s.rbVel[k] = v[k];
		s.P[k] = v[k] * s.mass;
		s.w[k] = s.L[k] = 0.0f;
	}
	s.w[0] = wroll * 0.0174533f;   // about the nose, which is +x
	s.L[0] = s.w[0] / s.Iinv[0];
	printf("== thrown (MellowMover): rolled %.0f deg, %.1f m/s across, %.1f along, rolling "
		"%.0f deg/s, throttle %.1f, 10 s\n", deg, vlat, vfwd, wroll, thr);
	float maxUp = -1.0f, drift = 0.0f;
	if (getenv("CARSIM_THROWNV")) {
		carsim_add_impulse_hook(thrownHook);
		printf("    cg %.3f %.3f %.3f  wheel z %.3f %.3f  radius %.3f %.3f  attach z %.3f %.3f  "
			"hRest %.3f\n", s.cg[0], s.cg[1], s.cg[2], s.wheel[0].pos[2], s.wheel[2].pos[2],
			s.wheel[0].radius, s.wheel[2].radius, s.susp[0].attach[2], s.susp[2].attach[2],
			s.hRest);
	}
	for (int f = 0; f <= 300; f++) {
		float t = f * kDt;
		if (f % 15 == 0) line("", t, &s);
		const float L0 = s.L[0];
		g_thrownFrame = f;
		carsim_update(&s, &in, &g_w, kDt);
		if (getenv("CARSIM_THROWNV") && f < 75)
			printf("    f%3d roll %6.1f dLx %8.1f Lx %8.1f wx %6.2f z %.3f down %d touch %d douse %d "
				"susp %d%d%d%d F %.0f %.0f %.0f %.0f wheelW %.0f\n", f, s.roll * 57.29578f,
				s.L[0] - L0, s.L[0], s.w[0], s.rbPos[2], s.wheelsDown, s.touching, s.dousing,
				s.susp[0].onGround, s.susp[1].onGround, s.susp[2].onGround, s.susp[3].onGround,
				s.susp[0].F, s.susp[1].F, s.susp[2].F, s.susp[3].F, s.wheel[2].w);
		if (f > 30 && s.rot[2][2] > maxUp) maxUp = s.rot[2][2];
		if (fabsf(s.rbPos[1]) > drift) drift = fabsf(s.rbPos[1]);
	}
	carsim_remove_impulse_hook(thrownHook);
	printf("  RESULT thrown %.0f/%.1f/%.0f/%.1f: at x %.2f y %.2f z %.3f, up.z %.2f (highest "
		"after 1 s %.2f), most across %.2f m\n", deg, vlat, wroll, thr, s.rbPos[0], s.rbPos[1],
		s.rbPos[2], s.rot[2][2], maxUp, drift);
}

static void caseDrop(void) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	roll(&s, 30.0f);
	s.rbPos[2] += 2.0f;
	printf("== jump: 2 m drop at 30 m/s, 2 s\n");
	float worstPitch = 0.0f;
	for (int f = 0; f <= 60; f++) {
		float t = f * kDt;
		if (f % 3 == 0) line("", t, &s);
		carsim_update(&s, &in, &g_w, kDt);
		if (fabsf(s.pitch) > worstPitch) worstPitch = fabsf(s.pitch);
	}
	printf("  RESULT drop: settled z %.3f, speed %.1f m/s, worst pitch %.1f deg\n", s.pos[2],
		s.speed, worstPitch * 57.29578f);
}

static void caseSlope(void) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.15f;   // 8.5 deg, nose uphill
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	printf("== parked on an 8.5 deg hill, nose up, no input, 5 s; then throttle\n");
	for (int f = 0; f <= 240; f++) {
		float t = f * kDt;
		if (f == 150) in.throttle = 1.0f;
		if (f % 30 == 0) line("", t, &s);
		carsim_update(&s, &in, &g_w, kDt);
	}
	printf("  RESULT slope: x %.2f m, pitch %.1f deg, speed %.1f\n", s.pos[0],
		s.pitch * 57.29578f, s.speed);
	g_slope = 0.0f;
}

// Car on car (TASKS.md #223): two cars, both on a flat plane, driven into
// each other. `offsetY` is how far across the second car sits -- 0 is a
// straight rear-end, a couple of metres a side-swipe -- and `yaw2` which way
// it faces, so a head-on is 180.
static void caseCarCar(const char* name, float gap, float offsetY, float yaw2,
	float mph1, float mph2, float seconds)
{
	CarSim a, b;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&a, 0.0f, 0.0f, 0.0f);
	roll(&a, mph1 / 2.237f);
	fresh(&b, gap, offsetY, yaw2);
	roll(&b, mph2 / 2.237f);
	printf("== car on car, %s: gap %.1f m, across %+.1f m, b at %.0f deg,"
		" %.0f vs %.0f mph\n", name, gap, offsetY, yaw2, mph1, mph2);
	int frames = (int)(seconds / kDt), touches = 0, impulses = 0, wild = 0;
	float worst = 0.0f, yawRate[2] = { 0.0f, 0.0f }, rollMax[2] = { 0.0f, 0.0f };
	for (int f = 0; f <= frames; f++) {
		if (f % 12 == 0) {
			line("a", f * kDt, &a);
			line("b", f * kDt, &b);
		}
		carsim_update(&a, &in, &g_w, kDt);
		carsim_update(&b, &in, &g_w, kDt);
		float wz = a.w[2];
		if (carsim_car_contact(&a, &b, getenv("CARSIM_NOBACKUP") ? 0.0f : kDt)) {
			touches++;
			if (fabsf(a.w[2] - wz) > 1e-7f)
				impulses++;
			if (a.lastImpact > worst) worst = a.lastImpact;
			if (a.dampenMode == 1 || b.dampenMode == 1) wild++;
		}
		const CarSim* c[2] = { &a, &b };
		for (int i = 0; i < 2; i++) {
			if (fabsf(c[i]->w[2]) > yawRate[i]) yawRate[i] = fabsf(c[i]->w[2]);
			if (fabsf(c[i]->roll) > rollMax[i]) rollMax[i] = fabsf(c[i]->roll);
		}
	}
	float dx = a.pos[0] - b.pos[0], dy = a.pos[1] - b.pos[1];
	printf("  RESULT carcar %s: touches %d, hardest %.2f m/s, apart %.2f m,"
		" impulses %d, a %.1f mph hdg %.1f, b %.1f mph hdg %.1f\n", name, touches, worst,
		sqrtf(dx * dx + dy * dy), impulses, a.speed * 2.237f, a.yaw * 57.29578f,
		b.speed * 2.237f, b.yaw * 57.29578f);
	printf("  RESULT carcar %s spin: peak yaw rate a %.0f b %.0f deg/s, peak roll a %.1f"
		" b %.1f deg, wild contacts %d\n", name, yawRate[0] * 57.29578f,
		yawRate[1] * 57.29578f, rollMax[0] * 57.29578f, rollMax[1] * 57.29578f, wild);
}

// A fast car into a slower one (TASKS.md #316, the user's hardware run: "whenever
// I was speeding with my nitrous, I went THROUGH a car I was intentionally trying
// to crash into"). The same hit from ten starting gaps 0.3 m apart, so the frame
// the boxes first overlap on lands at every depth; counts the runs where the fast
// car's centre comes within 2 m of the other's end-on while they are side by side
// and level within 0.5 m (a car is 4.3 m long) and the two hulls still overlap
// after the contact, i.e. it drove into the other car's body rather than off it.
// The hull test keeps out a hit that spins both cars round side by side, touching
// but apart (#223). A hard square hit may still pitch it up 2 m and throw it past
// or down beside the other car, which is retail's wild collision, not this.
// CARSIM_NOBACKUP=1 hands the contact no step time, which turns FindContactTime's
// backup off: the old behaviour. CARSIM_THROUGHV=1 prints each frame counted, with
// how high the car had been thrown.
static void caseCarThrough(const char* name, float offsetY, float yaw2, float mph1, float mph2) {
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	int through = 0, missed = 0;
	const float stepDt = getenv("CARSIM_NOBACKUP") ? 0.0f : kDt;
	float worst = 0.0f, endA = 0.0f;
	for (int i = 0; i < 10; i++) {
		CarSim a, b;
		const float gap = 12.0f + 0.3f * (float)i;
		fresh(&a, 0.0f, 0.0f, 0.0f);
		roll(&a, mph1 / 2.237f);
		fresh(&b, gap, offsetY, yaw2);
		roll(&b, mph2 / 2.237f);
		int touches = 0;
		bool in2 = false;
		float airMax = 0.0f;
		const int frames = (int)(2.0f / kDt);
		for (int f = 0; f <= frames; f++) {
			carsim_update(&a, &in, &g_w, kDt);
			carsim_update(&b, &in, &g_w, kDt);
			if (carsim_car_contact(&a, &b, stepDt)) touches++;
			if (a.lastImpact > worst) worst = a.lastImpact;
			const float dx = a.pos[0] - b.pos[0], dy = a.pos[1] - b.pos[1], dz = a.pos[2] - b.pos[2];
			const bool inside = fabsf(dx) < 2.0f && fabsf(dy) < 1.6f && fabsf(dz) < 0.5f &&
				carsim_car_distance(&a, &b) < 1e-5f;
			if (inside) in2 = true;
			if (a.pos[2] > airMax) airMax = a.pos[2];
			if (getenv("CARSIM_THROUGHV") && inside)
				printf("    gap %.1f f %d dx %.2f dy %.2f dz %.2f, a was up to %.2f m, hdg a %.0f"
					" b %.0f, hulls %.3f m apart\n", gap, f, dx, dy, dz, airMax, a.yaw * 57.29578f,
					b.yaw * 57.29578f, carsim_car_distance(&a, &b));
		}
		if (!touches) missed++;
		else if (in2) through++;
		endA += a.speed * 2.237f;
	}
	printf("  RESULT through %s: %.0f vs %.0f mph, across %+.1f m, b at %.0f deg: through %d/10,"
		" no contact %d, hardest %.1f m/s, a ends %.0f mph (mean)\n", name, mph1, mph2, offsetY,
		yaw2, through, missed, worst, endA / 10.0f);
}

// aidriver.c's speed profile reads the race route; no case here builds one.
float race_route_length(const RaceRoute* r) { (void)r; return 0.0f; }
bool race_route_loops(const RaceRoute* r) { (void)r; return false; }
bool race_route_sample(const RaceRoute* r, float dist, float lateral,
	float outPos[2], float outDir[2]) { (void)r; (void)dist; (void)lateral;
	(void)outPos; (void)outDir; return false; }

// The AI reset (Car::Move -> ResetToRoutePoint -> ResetCore): placed, then left
// rolling at RollingResetSpeed with the wheels spun up to match. With no input
// it has to carry on straight at that speed, on all four wheels, not lurch.
static void caseResetRoll(void) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 30.0f);
	carsim_set_rolling(&s, 8.0f);
	printf("== reset roll: placed at 30 deg, rolling at 8 m/s, no input, 1.5 s\n");
	float minDown = 4, peakPitch = 0.0f, peakRoll = 0.0f;
	for (int f = 0; f <= 45; f++) {
		if (f % 5 == 0) line("", f * kDt, &s);
		carsim_update(&s, &in, &g_w, kDt);
		if (s.wheelsDown < minDown) minDown = s.wheelsDown;
		if (fabsf(s.pitch) > peakPitch) peakPitch = fabsf(s.pitch);
		if (fabsf(s.roll) > peakRoll) peakRoll = fabsf(s.roll);
	}
	printf("  RESULT reset roll: end speed %.2f m/s heading %.1f, min wheels down %.0f,"
		" peak pitch %.1f roll %.1f deg\n", s.speed, s.yaw * 57.29578f, minDown,
		peakPitch * 57.29578f, peakRoll * 57.29578f);
}

// The player stuck on a hill (TASKS.md #279): parked nose up with no input,
// then full throttle. In free roam the port used to leave TuneBoostAndDampers'
// motion damping on under throttle (CarSimInput.racing false), and on a steep
// enough hill it held the car to a walking pace with the engine at idle; retail
// runs free roam at race stage 4, where the damping lets go. `racing` picks
// which of the two the car drives under.
static void caseHill(float deg, bool racing) {
	CarSim s;
	CarSimInput in;
	memset(&in, 0, sizeof(in));
	in.racing = racing;
	g_slope = tanf(deg * 0.0174533f);
	g_wallUp = false;
	fresh(&s, 0.0f, 0.0f, 0.0f);
	printf("== %.1f deg hill, nose up, 1 s parked, then full throttle 6 s (racing %d)\n", deg,
		racing);
	float minX = 0.0f;
	for (int f = 0; f <= 210; f++) {
		if (f == 30) in.throttle = 1.0f;
		if (f % 30 == 0) {
			line(f < 30 ? "park" : "thr", f * kDt, &s);
			printf("         clutch %d  engW %5.1f diffW %5.1f  damping %.3f\n", s.clutchState,
				s.engW, s.diffW, s.damping);
		}
		carsim_update(&s, &in, &g_w, kDt);
		if (s.pos[0] < minX) minX = s.pos[0];
	}
	printf("  RESULT hill %.1f racing %d: rolled back to x %.2f, end x %.2f speed %.2f m/s\n",
		deg, racing, minX, s.pos[0], s.speed);
	g_slope = 0.0f;
}

// The AI's reverse-out (the testers' "the AI can't go backwards", 2026-10-26):
// PsychoDriver drives the car nose first into the wall at y = 0 as if its
// steer point were behind it, then opponent.c's GetUnstuck takes over -- the
// same trigger, goal, speed and end tests, inlined -- and the car has to back
// round to face down the road (+x) and drive off along it.
static void caseAiReverse(float yawDeg) {
	CarSim s;
	AiDriver d;
	CarSimInput in;
	g_slope = 0.0f;
	g_wallUp = true;
	fresh(&s, 0.0f, 3.0f, yawDeg);
	aidriver_init(&d, 1);
	d.racing = true;
	printf("== AI reverse-out: nose into the wall at %.0f deg, then GetUnstuck\n", yawDeg);
	int phase = 0;        // 0 pinning, 1 backing out, 2 driving on
	float tPhase = 0.0f, tTrig = -1.0f, tDone = -1.0f, minY = 1e9f;
	const char* end = "none";
	int frontAgain = 0;
	for (int f = 0; f <= 450; f++) {
		float t = f * kDt;
		const float goal[2] = { s.pos[0] + 75.0f, 3.0f };
		if (phase == 0) {
			d.steerTo[0] = s.pos[0] + 5.0f * cosf(yawDeg * 0.0174533f);
			d.steerTo[1] = -10.0f;
			d.desired = 15.0f;
			// AIActionGetUnstuck::CalculateScore (opponent.c unstuckWanted):
			// held up front or side on. A hull's corner on the wall is a side
			// touch, as GetCollisionClassification bands it.
			float age = carsim_touch_age(&s, CARSIM_TOUCH_FRONT);
			if (carsim_touch_age(&s, CARSIM_TOUCH_LEFT) > age)
				age = carsim_touch_age(&s, CARSIM_TOUCH_LEFT);
			if (carsim_touch_age(&s, CARSIM_TOUCH_RIGHT) > age)
				age = carsim_touch_age(&s, CARSIM_TOUCH_RIGHT);
			if (t > 2.0f && age >= 0.4f && s.speed <= 5.001f) {
				phase = 1; tPhase = 0.0f; tTrig = t;
			}
		}
		if (phase == 1) {
			d.steerTo[0] = goal[0];
			d.steerTo[1] = goal[1];
			d.desired = -15.0f;
			float gx = goal[0] - s.pos[0], gy = goal[1] - s.pos[1];
			float gl = sqrtf(gx * gx + gy * gy);
			float dotv = (gx * s.rot[0][0] + gy * s.rot[0][1]) / gl;
			bool pinned = carsim_touch_age(&s, CARSIM_TOUCH_FRONT) >= 0.4f && s.speed < 5.0f;
			bool backed = carsim_touch_age(&s, CARSIM_TOUCH_BEHIND) >= 0.4f && s.speed < 5.001f;
			const char* why = NULL;
			if (tPhase >= 1.3f && dotv >= 0.9f && !pinned) why = "accomplished";
			else if (tPhase >= 1.3f && backed) why = "failed";
			else if (tPhase > 5.0f) why = "expired";
			if (why) {
				end = why; tDone = t; phase = 2; tPhase = 0.0f;
				aidriver_clear_steering(&d, &s);
			}
		}
		if (phase == 2) {
			d.steerTo[0] = s.pos[0] + 20.0f;
			d.steerTo[1] = 3.0f;
			d.desired = 20.0f;
			if (carsim_touch_age(&s, CARSIM_TOUCH_FRONT) >= 0.4f) frontAgain++;
		}
		d.lateral_m = 0.0f;
		aidriver_drive(&d, &s, kDt, &in);
		if (f % 15 == 0) {
			if (getenv("CARSIM_TPV"))
				printf("  touch ages F %.2f B %.2f L %.2f R %.2f, x %.2f |",
					carsim_touch_age(&s, CARSIM_TOUCH_FRONT),
					carsim_touch_age(&s, CARSIM_TOUCH_BEHIND),
					carsim_touch_age(&s, CARSIM_TOUCH_LEFT),
					carsim_touch_age(&s, CARSIM_TOUCH_RIGHT), s.touchX[CARSIM_TOUCH_FRONT]);
			printf("  ph %d thr %.2f brk %.2f gearReq %2d |", phase, in.throttle, in.brake, in.gear),
			line("", t, &s);
		}
		carsim_update(&s, &in, &g_w, kDt);
		tPhase += kDt;
		if (phase >= 1 && s.pos[1] < minY) minY = s.pos[1];
	}
	printf("  RESULT ai reverse %.0f deg: triggered %.2f s, ended %s at %.2f s, "
		"end x %.1f y %.2f heading %.1f speed %.1f mph, min y %.2f, pinned again %d frames\n",
		yawDeg, tTrig, end, tDone, s.pos[0], s.pos[1], s.yaw * 57.29578f,
		s.speed * 2.237f, minY, frontAgain);
	g_wallUp = false;
}

// A traffic car on the MellowMover (TASKS.md #316): what traffic.c's driveCar
// does once an avoid or a hit has put it on physics -- PsychoDriver in
// movement index 4, steering at the lane (the line y = 0, down +x) at
// aidriver_lookahead, on carsim_set_mellow's mover. It starts `y0` m off the
// lane at `yawDeg`, holds `v` m/s, and for `stopSec` from 2 s wants 0 (the
// avoid's brake). The lane has to be held without a weave -- which it is at
// retail's 1/60 s step and is not at the 3DS's 1/30 s frame (CARSIM_FPS=30):
// why traffic.c cuts the frame into TR_STEP_SEC steps. 12 s, 1/60 s steps
// unless CARSIM_FPS says otherwise.
// #401's knocked cases: the lane's speed when it is not the starting speed.
static float g_tlWant = 0.0f;
static void caseTrafficLane(const char* name, float y0, float yawDeg, float v, float stopSec) {
	CarSim s;
	AiDriver d;
	CarSimInput in;
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, 0.0f, y0, yawDeg);
	carsim_set_rolling(&s, v);
	aidriver_init(&d, 1);
	d.racing = true;
	d.traffic = true;
	d.trafficMover = false;
	carsim_set_mellow(&s, true, 0.0f, carsim_mellow_mu(false, 0u));
	printf("== traffic lane, %s: %.1f m off at %.0f deg, %.0f m/s, stop %.1f s\n", name, y0,
		yawDeg, v, stopSec);
	float maxY = 0.0f, maxYaw = 0.0f, maxSteer = 0.0f;
	int flips = 0;
	float lastSteer = 0.0f;
	// #401: how far the car turned in all, to catch a donut.
	float turned = 0.0f, prevYaw = s.yaw;
	const float h = getenv("CARSIM_FPS") ? kDt : 1.0f / 60.0f;
	for (int f = 0; f * h <= 12.0f; f++) {
		{
			float dy = s.yaw - prevYaw;
			if (dy > 3.14159265f) dy -= 6.2831853f;
			else if (dy < -3.14159265f) dy += 6.2831853f;
			turned += dy;
			prevYaw = s.yaw;
		}
		const float t = f * h;
		const float bodySpeed = s.vel[0] * s.rot[0][0] + s.vel[1] * s.rot[0][1];
		const float look = aidriver_lookahead(&d, &s, bodySpeed);
		d.steerTo[0] = s.pos[0] + look;
		d.steerTo[1] = 0.0f;
		d.lateral_m = -s.pos[1];
		d.desired = t >= 2.0f && t < 2.0f + stopSec ? 0.0f : (g_tlWant > 0.0f ? g_tlWant : v);
		aidriver_drive(&d, &s, h, &in);
		float steer = (float)d.steerU16 * (360.0f / 65536.0f);
		if (steer > 180.0f) steer -= 360.0f;
		if (f % (int)(0.5f / h) == 0)
			printf("  steer %6.1f thr %.2f brk %.2f |", steer, in.throttle, in.brake),
			line("", t, &s);
		carsim_update(&s, &in, &g_w, h);
		if (t > 3.0f + stopSec) {
			if (fabsf(s.pos[1]) > maxY) maxY = fabsf(s.pos[1]);
			if (fabsf(s.yaw) > maxYaw) maxYaw = fabsf(s.yaw);
			if (fabsf(steer) > maxSteer) maxSteer = fabsf(steer);
			if (fabsf(steer) > 2.0f && steer * lastSteer < 0.0f) flips++;
		}
		if (fabsf(steer) > 2.0f) lastSteer = steer;
	}
	printf("  RESULT traffic lane %s: settled max off %.2f m, max heading %.1f deg, max steer "
		"%.1f deg, %d steering reversals; end speed %.1f m/s; turned %.0f deg in all, end at "
		"(%.1f, %.1f) heading %.0f\n", name, maxY, maxYaw * 57.29578f, maxSteer, flips, s.speed,
		turned * 57.29578f, s.pos[0], s.pos[1], s.yaw * 57.29578f);
}

// #401: a lane with a sharp corner, tracked as traffic.c's walkTrack does
// (the car's segment advances once it is past the far node), and Think's
// ChangeCurrentL taking the car's own L every 0.2 s. `bisect` adds the fix:
// a car inside the corner, past the bisector at the node, moves on too.
typedef struct { float x, y, s; } TlNode;
static void tlTrack(const TlNode* n, int nn, int* cur, float qx, float qy, bool bisect,
		float* prog, float* lat, float dir[2]) {
	for (;;) {
		const TlNode* a = &n[*cur];
		const TlNode* b = &n[*cur + 1];
		const float ex = b->x - a->x, ey = b->y - a->y, l = sqrtf(ex * ex + ey * ey);
		float u = ((qx - a->x) * ex + (qy - a->y) * ey) / (l * l);
		if (*cur + 2 < nn) {
			bool past = u >= 1.0f;
			if (!past && bisect) {
				const TlNode* c = &n[*cur + 2];
				const float fx = c->x - b->x, fy = c->y - b->y, m = sqrtf(fx * fx + fy * fy);
				const float bx = ex / l + fx / m, by = ey / l + fy / m;
				past = bx * bx + by * by > 0.01f && (qx - b->x) * bx + (qy - b->y) * by >= 0.0f;
			}
			if (past) {
				(*cur)++;
				continue;
			}
		}
		u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
		*prog = a->s + (b->s - a->s) * u;
		dir[0] = ex / l;
		dir[1] = ey / l;
		*lat = (qx - (a->x + ex * u)) * dir[1] - (qy - (a->y + ey * u)) * dir[0];
		return;
	}
}
static void tlSample(const TlNode* n, int nn, int cur, float s, float p[2], float d[2]) {
	int i = cur;
	while (i + 1 < nn - 1 && n[i + 1].s < s) i++;
	const float len = n[i + 1].s - n[i].s;
	float u = (s - n[i].s) / len;
	u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
	p[0] = n[i].x + (n[i + 1].x - n[i].x) * u;
	p[1] = n[i].y + (n[i + 1].y - n[i].y) * u;
	d[0] = (n[i + 1].x - n[i].x) / len;
	d[1] = (n[i + 1].y - n[i].y) / len;
}
static void caseTrafficCorner(const char* name, float turnDeg, float v, float y0, bool bisect) {
	TlNode n[3] = { { -80.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 80.0f }, { 0.0f, 0.0f, 680.0f } };
	n[2].x = 600.0f * cosf(turnDeg / 57.29578f);
	n[2].y = 600.0f * sinf(turnDeg / 57.29578f);
	CarSim s;
	AiDriver d;
	CarSimInput in;
	g_slope = 0.0f;
	g_wallUp = false;
	fresh(&s, -40.0f, y0, 0.0f);
	carsim_set_rolling(&s, v);
	aidriver_init(&d, 1);
	d.racing = true;
	d.traffic = true;
	d.trafficMover = false;
	carsim_set_mellow(&s, true, 0.0f, carsim_mellow_mu(false, 0u));
	printf("== traffic corner, %s: %.0f deg turn, %.0f m/s, %s\n", name, turnDeg, v,
		bisect ? "bisector fix" : "as walkTrack was");
	int cur = 0;
	float driveL = 0.0f, driveS = 0.0f, think = 0.0f, turned = 0.0f, prevYaw = s.yaw;
	float prog = 0.0f, lat = 0.0f, dir[2];
	const float h = 1.0f / 60.0f;
	for (int f = 0; f * h <= 25.0f; f++) {
		float dy = s.yaw - prevYaw;
		if (dy > 3.14159265f) dy -= 6.2831853f;
		else if (dy < -3.14159265f) dy += 6.2831853f;
		turned += dy;
		prevYaw = s.yaw;
		tlTrack(n, 3, &cur, s.pos[0], s.pos[1], bisect, &prog, &lat, dir);
		if ((think -= h) <= 0.0f) {
			think += 0.2f;
			driveL = lat;
			driveS = prog;
		}
		const float bodySpeed = s.vel[0] * s.rot[0][0] + s.vel[1] * s.rot[0][1];
		const float look = aidriver_lookahead(&d, &s, bodySpeed);
		float tp[2], td[2];
		tlSample(n, 3, cur, prog + look, tp, td);
		const float lm = fmaxf(0.0f, fabsf(driveL) - 0.151f * fmaxf(0.0f, prog + look - driveS));
		const float l = driveL < 0.0f ? -lm : lm;
		d.steerTo[0] = tp[0] + l * td[1];
		d.steerTo[1] = tp[1] - l * td[0];
		const float lm0 = fmaxf(0.0f, fabsf(driveL) - 0.151f * fmaxf(0.0f, prog - driveS));
		d.lateral_m = lat - (driveL < 0.0f ? -lm0 : lm0);
		d.desired = v;
		aidriver_drive(&d, &s, h, &in);
		if (f % 60 == 0)
			printf("  t %4.1f seg %d prog %6.1f lat %6.1f L %6.1f |", f * h, cur, prog, lat, driveL),
			line("", f * h, &s);
		carsim_update(&s, &in, &g_w, h);
	}
	printf("  RESULT traffic corner %s (%s): turned %.0f deg, progress %.1f of 680, end at (%.1f, "
		"%.1f) lat %.1f, %s\n", name, bisect ? "fix" : "old", turned * 57.29578f, prog, s.pos[0],
		s.pos[1], lat, fabsf(turned * 57.29578f) > 400.0f ? "DONUT" : "ok");
}

int main(int argc, char** argv) {
	const char* only = argc > 1 ? argv[1] : NULL;
	const char* mode = getenv("CARSIM_MODE");
	if (mode) g_mode = atoi(mode);
	const char* fps = getenv("CARSIM_FPS");
	if (fps) kDt = 1.0f / (float)atof(fps);
	printf("control mode %d (Stability Control %s)\n", g_mode, g_mode == 0 ? "On" : "Off");
	const char* tp = getenv("CARSIM_TP");
	if (tp && strcmp(tp, "0")) {
		g_tp = true;
		g_w.trackPolys = tpPolys;
		g_w.trackPoly = tpPoly;
	}
	printf("the body against %s\n",
		g_tp ? "track polygons (CARSIM_TP)" : "the planes and the footprint resolver");
#define RUN(n, call) if (!only || !strcmp(only, n)) { call; }
	RUN("rest", caseRest());
	RUN("launch", caseLaunch());
	RUN("release", caseRelease(1.5f); caseRelease(4.0f); caseRelease(9.0f));
	RUN("manual", caseManual());
	RUN("brake", caseBrake(60.0f); caseBrake(120.0f));
	RUN("corner", caseCorner(30.0f, 1.0f, 0.3f); caseCorner(60.0f, 0.5f, 0.5f);
		caseCorner(90.0f, 0.3f, 0.6f));
	RUN("reverse", caseReverse());
	RUN("handbrake", caseHandbrake());
	RUN("drift", caseDrift(50.0f, 0.4f, 1.0f); caseDrift(50.0f, -0.2f, 1.0f);
		caseDrift(50.0f, -0.6f, 0.7f); caseDrift(70.0f, 0.3f, 0.8f));
	// Heading -90 is straight into the wall.
	RUN("wall", caseWall("head-on", 4.0f, -90.0f, 45.0f, 0.0f, 0.0f, 1.2f);
		caseWall("glancing 20 deg", 4.0f, -20.0f, 55.0f, 0.5f, 0.0f, 2.5f);
		caseWall("oblique 45 deg", 4.0f, -45.0f, 45.0f, 0.5f, 0.0f, 2.5f);
		caseWall("grind, steering in", 2.0f, -5.0f, 45.0f, 1.0f, -0.4f, 4.0f));
	// Heading 90 faces away from the wall: the brake from rest is reverse.
	RUN("posthit", caseHitSteer(false); caseHitSteer(true));
	RUN("rolltrip", caseRollTripSweep());
	RUN("curb", caseCurbSweep());
	RUN("deck", caseDeck("reversing into it", 3.0f, 90.0f, 0.0f, 0.0f, 1.0f, 0.0f, 4.0f);
		caseDeck("reversing, steering", 3.0f, 70.0f, 0.0f, 0.0f, 1.0f, -0.9f, 4.0f);
		caseDeck("head-on 20 mph", 4.0f, -90.0f, 20.0f, 0.3f, 0.0f, 0.0f, 2.0f);
		caseDeck("grind, steering in", 2.0f, -5.0f, 45.0f, 1.0f, 0.0f, -0.4f, 4.0f));
	RUN("roof", caseRoof());
	RUN("side", caseSide(90.0f, false); caseSide(80.0f, false); caseSide(100.0f, false);
		caseSide(90.0f, true); caseSide(95.0f, true));
	RUN("thrown", caseThrown(90.0f, 4.0f, 0.0f, 0.0f, 0.0f); caseThrown(90.0f, 4.0f, 8.0f, 0.0f, 0.3f);
		caseThrown(70.0f, 5.0f, 0.0f, 180.0f, 0.0f); caseThrown(110.0f, 3.0f, 5.0f, -90.0f, 0.3f);
		caseThrown(90.0f, 0.0f, 10.0f, 0.0f, 0.5f));
	if (only && !strcmp(only, "thrownx") && getenv("CARSIM_THROWN")) {
		float a[5] = { 90.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		sscanf(getenv("CARSIM_THROWN"), "%f,%f,%f,%f,%f", &a[0], &a[1], &a[2], &a[3], &a[4]);
		caseThrown(a[0], a[1], a[2], a[3], a[4]);
	}
	RUN("drop", caseDrop());
	RUN("slope", caseSlope());
	RUN("resetroll", caseResetRoll());
	RUN("hill", caseHill(10.5f, false); caseHill(18.0f, false); caseHill(10.5f, true);
		caseHill(18.0f, true); caseHill(24.0f, true); caseHill(30.0f, true));
	RUN("aireverse", caseAiReverse(-60.0f); caseAiReverse(-90.0f); caseAiReverse(-30.0f));
	RUN("traffic", caseTrafficLane("on the lane", 0.0f, 0.0f, 13.0f, 0.0f);
		caseTrafficLane("2 m off", 2.0f, 0.0f, 13.0f, 0.0f);
		caseTrafficLane("15 deg out", 0.0f, 15.0f, 13.0f, 0.0f);
		caseTrafficLane("braked 1.5 s", 1.0f, 5.0f, 13.0f, 1.5f);
		caseTrafficLane("20 m/s, 1 m off", 1.0f, 0.0f, 20.0f, 0.0f));
	// #401: knocked round by a hit, from a standstill and still rolling.
	g_tlWant = 13.0f;
	for (int fix = 0; fix < 2; fix++)
		RUN("trafficcorner", caseTrafficCorner("right 90", -90.0f, 6.0f, 0.0f, fix);
			caseTrafficCorner("left 90", 90.0f, 6.0f, 0.0f, fix);
			caseTrafficCorner("right 90 fast", -90.0f, 13.0f, 0.0f, fix);
			caseTrafficCorner("right 120", -120.0f, 6.0f, 0.0f, fix);
			caseTrafficCorner("left 120 fast", 120.0f, 11.0f, 0.0f, fix);
			caseTrafficCorner("right 90, 3 m right", -90.0f, 8.0f, -3.0f, fix));
	RUN("trafficspin",caseTrafficLane("knocked 90, stopped", 0.0f, 90.0f, 0.0f, 0.0f);
		caseTrafficLane("knocked 135, stopped", 1.0f, 135.0f, 0.0f, 0.0f);
		caseTrafficLane("knocked 180, stopped", 0.0f, 180.0f, 0.0f, 0.0f);
		caseTrafficLane("knocked 90, 8 m/s", 0.0f, 90.0f, 8.0f, 0.0f);
		caseTrafficLane("knocked 160, 6 m/s", 2.0f, 160.0f, 6.0f, 0.0f));
	RUN("carcar", caseCarCar("rear-end", 8.0f, 0.0f, 0.0f, 45.0f, 0.0f, 2.5f);
		caseCarCar("side-swipe", 10.0f, 1.8f, -8.0f, 45.0f, 25.0f, 3.0f);
		caseCarCar("head-on", 12.0f, 0.0f, 180.0f, 30.0f, 30.0f, 2.5f);
		caseCarCar("already overlapping", 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 2.0f);
		// A tap on the rear quarter of a car alongside -- the AI spin-outs the
		// testers saw (2026-10-26) -- and a T-bone fast enough to be a wild
		// collision.
		caseCarCar("rear-quarter tap", 6.0f, 1.4f, 0.0f, 60.0f, 40.0f, 3.0f);
		caseCarCar("t-bone 80 mph", 10.0f, 0.0f, 90.0f, 80.0f, 0.0f, 3.0f));
	RUN("through", caseCarThrough("rear-end, square", 0.0f, 0.0f, 160.0f, 30.0f);
		caseCarThrough("rear-end, 1 m across", 1.0f, 0.0f, 160.0f, 30.0f);
		caseCarThrough("rear-end, 1.4 m across", 1.4f, 0.0f, 160.0f, 30.0f);
		caseCarThrough("rear-end, 1 m across, 100 mph", 1.0f, 0.0f, 100.0f, 30.0f);
		caseCarThrough("head-on, 1 m across", 1.0f, 180.0f, 140.0f, 30.0f);
		caseCarThrough("into a stopped car, 0.8 m across", 0.8f, 0.0f, 150.0f, 0.0f));
	return 0;
}
