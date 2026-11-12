// The race replay's recording and playback -- see replay.h (TASKS.md #498).
#include "replay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heapcfg.h"

#define REPLAY_HZ          15.0f
#define REPLAY_BLOCK       (64u * 1024u)
#define REPLAY_BUDGET      (3u * 1024u * 1024u)
#define REPLAY_MAX_BLOCKS  (REPLAY_BUDGET / REPLAY_BLOCK)
#define REPLAY_HEAP_FLOOR  (8u * 1024u * 1024u)

// MaybeTakeInstantReplaySnapshot: every 300 world frames (60 Hz), two kept.
#define REPLAY_INSTANT_SEC 5.0f
#define REPLAY_INSTANT_N   2
// MaybeTakeLapSnapshot: DAT_8032e8c0 15 s, +0xB0 five.
#define REPLAY_LAP_SEC     15.0f
#define REPLAY_LAP_N       5
// ReplayMenu's ctor: a lap snapshot this close to the end is left out.
#define REPLAY_LAP_TAIL    20.0f

#define PI_F 3.14159265f

// ---------------------------------------------------------------------------
// The frame records, read and written with memcpy into aligned locals (the
// blocks pack them end to end, so a record in one need not be aligned).

typedef struct {
	u8  nFull;
	u8  events;      // bit 0 the backfire
	s8  bottomOut;   // -1 none
	u8  throttle;    // the player's, x255
	s8  lap;
	u8  pad;
	u16 trMask;      // the traffic slots that follow
	s32 lapTicks;
} FrameHdr;

typedef struct {
	float rbPos[3];
	float clock;
	s16 rot[6];        // rows 0 and 1, x32767
	s16 vel[3];        // x100, m/s
	s16 wAng[4];       // x32768/pi
	s16 wW[4];         // x64, rad/s
	s16 suspX[4];      // x10000, m
	s16 hydExt[4];     // x10000, m
	s16 spinner[4];    // x32768/pi
	s16 steerVis;      // degrees x100
	u16 steerU16;
	u16 rpm;
	u8  brake, ebrake, thr;
	s8  dtGear;
	u8  dtShifting;
	u8  wheelsDown;
	u8  mellow;
	u8  flags;         // 1 simReady, 2 inactive, 4 totaled
	u8  blink;         // blinkSec x50
	s32 nosLeft, nosOnAt, nosShot;
	u8  wGround;       // bit per wheel
	u8  terrain[4];
	u8  grip[4];       // x255
	s8  slip0[4];      // x4, m/s
	s8  slip1[4];
} FullRec;

typedef struct {
	float rbPos[3];
	s16 rot[6];
	s16 vel[3];
	s16 wAng[4];
	s16 wW[4];
	u16 steerU16;
	u8  flags;         // 1 used, 2 active, 4 mellow, 8 onGround
	u8  wheelsDown;
	u8  type;
	u8  brake;         // x255
	u32 tex;           // the skin's texture array
} TrafRec;

typedef struct {
	float t;
	u16 block;
	u16 size;
	u32 off;
} FrameIdx;

static struct {
	ReplayCarRef full[REPLAY_FULL_MAX];
	int nFull;
	Traffic* tr;
	bool bound;

	u8* block[REPLAY_MAX_BLOCKS];
	int nBlocks;
	int wBlock;
	u32 wOff;
	u32 bytes;
	FrameIdx* idx;
	int nIdx, capIdx;

	float clock;       // seconds of race recorded
	float interval;    // seconds a frame
	float sinceRec;
	bool  evBackfire;
	int   evBottom;
	int   thins;

	float lap[REPLAY_LAP_N];
	int   nLap;
	float lapIv;
	float inst[REPLAY_INSTANT_N];
	int   nInst;
	float nextInst;

	// THE_PRESENT
	CarSim* presentSim;
	TrafficCar* presentTr;
	struct { bool simReady, inactive; float blinkSec, lastThr; } presentX[REPLAY_FULL_MAX];
	bool present;
} R;

// ---------------------------------------------------------------------------
// Packing

static s16 packS16(float v, float scale) {
	float x = v * scale;
	if (x > 32767.0f) x = 32767.0f;
	if (x < -32767.0f) x = -32767.0f;
	return (s16)lrintf(x);
}
static s8 packS8(float v, float scale) {
	float x = v * scale;
	if (x > 127.0f) x = 127.0f;
	if (x < -127.0f) x = -127.0f;
	return (s8)lrintf(x);
}
static u8 packU8(float v, float scale) {
	float x = v * scale;
	if (x > 255.0f) x = 255.0f;
	if (x < 0.0f) x = 0.0f;
	return (u8)lrintf(x);
}
static float wrapPi(float a) {
	a = fmodf(a + PI_F, 2.0f * PI_F);
	if (a < 0.0f) a += 2.0f * PI_F;
	return a - PI_F;
}
static s16 packAngle(float a) {
	return packS16(wrapPi(a), 32767.0f / PI_F);
}
static float unpackAngle(s16 v) {
	return (float)v * (PI_F / 32767.0f);
}
static void packRot(const float rot[3][3], s16 out[6]) {
	for (int r = 0; r < 2; r++)
		for (int c = 0; c < 3; c++)
			out[r * 3 + c] = packS16(rot[r][c], 32767.0f);
}

static void encodeFull(const ReplayCarRef* ref, FullRec* o) {
	const CarSim* s = ref->sim;
	memset(o, 0, sizeof(*o));
	memcpy(o->rbPos, s->rbPos, sizeof(o->rbPos));
	o->clock = s->clock;
	packRot(s->rot, o->rot);
	for (int k = 0; k < 3; k++) o->vel[k] = packS16(s->rbVel[k], 100.0f);
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		const CarSimWheel* w = &s->wheel[i];
		o->wAng[i] = packAngle(w->angle);
		o->wW[i] = packS16(w->w, 64.0f);
		o->suspX[i] = packS16(s->susp[i].x, 10000.0f);
		o->hydExt[i] = packS16(s->hydExt[i], 10000.0f);
		o->spinner[i] = packAngle(s->spinnerAngle[i]);
		if (w->onGround) o->wGround |= (u8)(1u << i);
		o->terrain[i] = (u8)w->terrain;
		o->grip[i] = packU8(w->gripUse, 255.0f);
		o->slip0[i] = packS8(w->sndSlip[0], 4.0f);
		o->slip1[i] = packS8(w->sndSlip[1], 4.0f);
	}
	o->steerVis = packS16(s->mlSteerVis, 100.0f);
	o->steerU16 = s->steerU16;
	o->rpm = (u16)(s->rpmSmooth < 0.0f ? 0.0f : (s->rpmSmooth > 65535.0f ? 65535.0f : s->rpmSmooth));
	o->brake = packU8(s->brakeIn, 255.0f);
	o->ebrake = packU8(s->ebrakeIn, 255.0f);
	o->thr = packU8(ref->lastThr ? *ref->lastThr : s->thr, 255.0f);
	o->dtGear = (s8)s->dtGear;
	o->dtShifting = (u8)(s->dtShifting < 0 ? 0 : (s->dtShifting > 255 ? 255 : s->dtShifting));
	o->wheelsDown = (u8)s->wheelsDown;
	o->mellow = (u8)s->mellow;
	o->flags = (u8)((ref->simReady && *ref->simReady ? 1 : 0) | (ref->inactive && *ref->inactive ? 2 : 0) |
		(s->totaled ? 4 : 0));
	o->blink = ref->blinkSec ? packU8(*ref->blinkSec, 50.0f) : 0;
	o->nosLeft = s->nosLeft;
	o->nosOnAt = s->nosOnAt;
	o->nosShot = s->nosShot;
}

static void encodeTraf(const TrafficCar* c, TrafRec* o) {
	const CarSim* s = &c->sim;
	memset(o, 0, sizeof(*o));
	memcpy(o->rbPos, s->rbPos, sizeof(o->rbPos));
	packRot(s->rot, o->rot);
	for (int k = 0; k < 3; k++) o->vel[k] = packS16(s->rbVel[k], 100.0f);
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		o->wAng[i] = packAngle(s->wheel[i].angle);
		o->wW[i] = packS16(s->wheel[i].w, 64.0f);
	}
	o->steerU16 = c->drv.steerU16;
	o->flags = (u8)((c->used ? 1 : 0) | (c->active ? 2 : 0) | (c->mellow ? 4 : 0) | (s->onGround ? 8 : 0));
	o->wheelsDown = (u8)s->wheelsDown;
	o->type = (u8)c->type;
	o->brake = packU8(c->drv.prevBrake, 255.0f);
	o->tex = (u32)(uintptr_t)c->tex;
}

// ---------------------------------------------------------------------------
// Recording

void replay_reset(void) {
	for (int b = 0; b < R.nBlocks; b++)
		free(R.block[b]);
	free(R.idx);
	free(R.presentSim);
	free(R.presentTr);
	memset(&R, 0, sizeof(R));
	R.wBlock = -1;
	R.interval = 1.0f / REPLAY_HZ;
	R.evBottom = -1;
	R.lapIv = REPLAY_LAP_SEC;
}

void replay_bind(const ReplayCarRef* full, int nFull, Traffic* traffic) {
	if (nFull > REPLAY_FULL_MAX)
		nFull = REPLAY_FULL_MAX;
	R.nFull = 0;
	for (int k = 0; k < nFull; k++)
		if (full[k].sim)
			R.full[R.nFull++] = full[k];
	R.tr = traffic;
	R.bound = R.nFull > 0;
}

bool replay_bound(void) { return R.bound; }

static u16 trafficMask(void) {
	u16 m = 0;
	if (!R.tr || !R.tr->open)
		return 0;
	for (int i = 0; i < TRAFFIC_MAX && i < REPLAY_TRAFFIC_MAX; i++)
		if (R.tr->car[i].used && R.tr->car[i].active)
			m |= (u16)(1u << i);
	return m;
}

static int popcount16(u16 m) {
	int n = 0;
	for (; m; m &= (u16)(m - 1)) n++;
	return n;
}

// MaybeTakeLapSnapshot's thinning, on the frames: every other one goes
// (the last stays, so the end is where it was) and the rate halves.
static void thin(void (*log)(const char*)) {
	int n = 0, wb = 0;
	u32 wo = 0;
	for (int i = 0; i < R.nIdx; i++) {
		if ((i & 1) && i != R.nIdx - 1)
			continue;
		const FrameIdx f = R.idx[i];
		if (wo + f.size > REPLAY_BLOCK) {
			wb++;
			wo = 0;
		}
		memmove(R.block[wb] + wo, R.block[f.block] + f.off, f.size);
		R.idx[n].t = f.t;
		R.idx[n].block = (u16)wb;
		R.idx[n].size = f.size;
		R.idx[n].off = wo;
		wo += f.size;
		n++;
	}
	const int was = R.nIdx;
	R.nIdx = n;
	R.wBlock = wb;
	R.wOff = wo;
	R.bytes = (u32)wb * REPLAY_BLOCK + wo;
	R.interval *= 2.0f;
	R.thins++;
	if (log) {
		char b[160];
		snprintf(b, sizeof(b), "replay: full at %.1f s -- %d frames thinned to %d, now %.2f a second "
			"(%d block(s), heap %u KB free)", R.clock, was, n, 1.0f / R.interval, R.nBlocks,
			(unsigned)(heap_malloc_free() >> 10));
		log(b);
	}
}

// Room for `size` bytes at the write cursor: the next block, a new one, or
// the frames thinned.
static bool makeRoom(u32 size, void (*log)(const char*)) {
	for (int tries = 0; tries < 3; tries++) {
		if (R.wBlock >= 0 && R.wOff + size <= REPLAY_BLOCK)
			return true;
		if (R.wBlock + 1 < R.nBlocks) {
			R.wBlock++;
			R.wOff = 0;
			continue;
		}
		if (R.nBlocks < (int)REPLAY_MAX_BLOCKS && heap_malloc_free() > REPLAY_HEAP_FLOOR + REPLAY_BLOCK) {
			u8* b = (u8*)malloc(REPLAY_BLOCK);
			if (b) {
				R.block[R.nBlocks++] = b;
				R.wBlock = R.nBlocks - 1;
				R.wOff = 0;
				continue;
			}
		}
		if (R.nIdx < 4)
			return false;
		thin(log);
	}
	return R.wBlock >= 0 && R.wOff + size <= REPLAY_BLOCK;
}

static void marks(void) {
	// The lap snapshots: one when the interval times the count is up, and
	// when they are full, the interval doubled and every other one gone.
	if (R.lapIv * (float)R.nLap <= R.clock) {
		if (R.nLap == REPLAY_LAP_N) {
			R.lapIv *= 2.0f;
			int n = 0;
			for (int i = 0; i < R.nLap; i += 2)
				R.lap[n++] = R.lap[i];
			R.nLap = n;
		}
		if (R.nLap < REPLAY_LAP_N && R.lapIv * (float)R.nLap <= R.clock)
			R.lap[R.nLap++] = R.clock;
	}
	// The instant replays: one every 300 frames, the oldest out past two.
	if (R.clock >= R.nextInst) {
		R.nextInst = R.clock + REPLAY_INSTANT_SEC;
		if (R.nInst == REPLAY_INSTANT_N) {
			for (int i = 1; i < R.nInst; i++)
				R.inst[i - 1] = R.inst[i];
			R.nInst--;
		}
		R.inst[R.nInst++] = R.clock;
	}
}

void replay_record(float dt, const ReplayFrameIn* in, void (*log)(const char*)) {
	if (!R.bound || dt <= 0.0f)
		return;
	const bool first = R.nIdx == 0;
	if (!first)
		R.clock += dt;
	if (in && in->backfire)
		R.evBackfire = true;
	if (in && in->bottomOut > R.evBottom)
		R.evBottom = in->bottomOut;
	marks();
	R.sinceRec += first ? 0.0f : dt;
	if (!first && R.sinceRec < R.interval)
		return;
	R.sinceRec -= R.interval;
	if (R.sinceRec < 0.0f || R.sinceRec > R.interval)
		R.sinceRec = 0.0f;

	const u16 mask = trafficMask();
	const u32 size = (u32)(sizeof(FrameHdr) + R.nFull * sizeof(FullRec) +
		popcount16(mask) * sizeof(TrafRec));
	if (!makeRoom(size, log))
		return;
	if (R.nIdx == R.capIdx) {
		const int cap = R.capIdx ? R.capIdx * 2 : 1024;
		FrameIdx* n = (FrameIdx*)realloc(R.idx, (size_t)cap * sizeof(FrameIdx));
		if (!n)
			return;
		R.idx = n;
		R.capIdx = cap;
	}
	u8* p = R.block[R.wBlock] + R.wOff;
	FrameHdr h;
	memset(&h, 0, sizeof(h));
	h.nFull = (u8)R.nFull;
	h.events = R.evBackfire ? 1 : 0;
	h.bottomOut = (s8)(R.evBottom > 127 ? 127 : R.evBottom);
	h.throttle = in ? packU8(in->throttle, 255.0f) : 0;
	h.lap = in ? (s8)in->lap : -1;
	h.trMask = mask;
	h.lapTicks = in ? in->lapTicks : 0;
	memcpy(p, &h, sizeof(h));
	u32 o = sizeof(h);
	for (int k = 0; k < R.nFull; k++) {
		FullRec f;
		encodeFull(&R.full[k], &f);
		memcpy(p + o, &f, sizeof(f));
		o += sizeof(f);
	}
	for (int i = 0; i < REPLAY_TRAFFIC_MAX; i++) {
		if (!(mask & (1u << i)))
			continue;
		TrafRec t;
		encodeTraf(&R.tr->car[i], &t);
		memcpy(p + o, &t, sizeof(t));
		o += sizeof(t);
	}
	R.idx[R.nIdx].t = R.clock;
	R.idx[R.nIdx].block = (u16)R.wBlock;
	R.idx[R.nIdx].size = (u16)size;
	R.idx[R.nIdx].off = R.wOff;
	R.nIdx++;
	R.wOff += size;
	R.bytes += size;
	R.evBackfire = false;
	R.evBottom = -1;
	if (first && log) {
		char b[192];
		snprintf(b, sizeof(b), "replay: recording -- %d car(s) and %d traffic, %u bytes a frame at %.0f a "
			"second (FullRec %u, TrafRec %u), heap %u KB free", R.nFull, popcount16(mask),
			(unsigned)size, REPLAY_HZ, (unsigned)sizeof(FullRec), (unsigned)sizeof(TrafRec),
			(unsigned)(heap_malloc_free() >> 10));
		log(b);
	}
}

bool  replay_has_data(void) { return R.bound && R.nIdx >= 2 && R.idx[R.nIdx - 1].t - R.idx[0].t >= 1.0f; }
float replay_start(void) { return R.nIdx ? R.idx[0].t : 0.0f; }
float replay_end(void) { return R.nIdx ? R.idx[R.nIdx - 1].t : 0.0f; }
int   replay_frames(void) { return R.nIdx; }
u32   replay_bytes(void) { return R.bytes; }
float replay_rate(void) { return 1.0f / R.interval; }

int replay_marks(float* out, int max) {
	int n = 0;
	const float end = replay_end();
	for (int i = 0; i < R.nLap && n < max; i++)
		if (n == 0 || end - R.lap[i] >= REPLAY_LAP_TAIL)
			out[n++] = R.lap[i];
	// FindSnapshot("INSTANT_REPLAY", 0): the older of the two.
	if (R.nInst > 0 && n < max)
		out[n++] = R.inst[0];
	// By time (an insertion sort: ten at most).
	for (int i = 1; i < n; i++) {
		const float v = out[i];
		int j = i - 1;
		while (j >= 0 && out[j] > v) {
			out[j + 1] = out[j];
			j--;
		}
		out[j + 1] = v;
	}
	return n;
}

// ---------------------------------------------------------------------------
// Playback

bool replay_capture_present(void) {
	if (!R.bound)
		return false;
	free(R.presentSim);
	free(R.presentTr);
	R.presentTr = NULL;
	R.presentSim = (CarSim*)malloc(sizeof(CarSim) * (size_t)R.nFull);
	if (!R.presentSim)
		return false;
	for (int k = 0; k < R.nFull; k++) {
		const ReplayCarRef* r = &R.full[k];
		memcpy(&R.presentSim[k], r->sim, sizeof(CarSim));
		R.presentX[k].simReady = r->simReady ? *r->simReady : false;
		R.presentX[k].inactive = r->inactive ? *r->inactive : false;
		R.presentX[k].blinkSec = r->blinkSec ? *r->blinkSec : 0.0f;
		R.presentX[k].lastThr = r->lastThr ? *r->lastThr : 0.0f;
	}
	if (R.tr && R.tr->open) {
		R.presentTr = (TrafficCar*)malloc(sizeof(TrafficCar) * TRAFFIC_MAX);
		if (!R.presentTr) {
			free(R.presentSim);
			R.presentSim = NULL;
			return false;
		}
		memcpy(R.presentTr, R.tr->car, sizeof(TrafficCar) * TRAFFIC_MAX);
	}
	R.present = true;
	return true;
}

void replay_restore_present(void) {
	if (!R.present)
		return;
	for (int k = 0; k < R.nFull; k++) {
		const ReplayCarRef* r = &R.full[k];
		memcpy(r->sim, &R.presentSim[k], sizeof(CarSim));
		if (r->simReady) *r->simReady = R.presentX[k].simReady;
		if (r->inactive) *r->inactive = R.presentX[k].inactive;
		if (r->blinkSec) *r->blinkSec = R.presentX[k].blinkSec;
		if (r->lastThr) *r->lastThr = R.presentX[k].lastThr;
	}
	if (R.presentTr && R.tr)
		memcpy(R.tr->car, R.presentTr, sizeof(TrafficCar) * TRAFFIC_MAX);
	free(R.presentSim);
	free(R.presentTr);
	R.presentSim = NULL;
	R.presentTr = NULL;
	R.present = false;
}

// The frame at or before t (0 when t is before the first).
static int frameAt(float t) {
	int lo = 0, hi = R.nIdx - 1;
	if (hi < 0)
		return -1;
	if (t <= R.idx[0].t)
		return 0;
	if (t >= R.idx[hi].t)
		return hi;
	while (lo < hi) {
		const int mid = (lo + hi + 1) / 2;
		if (R.idx[mid].t <= t)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

static const u8* frameData(int i, FrameHdr* h) {
	const u8* p = R.block[R.idx[i].block] + R.idx[i].off;
	memcpy(h, p, sizeof(*h));
	return p + sizeof(*h);
}

static void unpackRot(const s16 in[6], float rot[3][3]) {
	for (int r = 0; r < 2; r++)
		for (int c = 0; c < 3; c++)
			rot[r][c] = (float)in[r * 3 + c] * (1.0f / 32767.0f);
}

// Rows 0 and 1 blended, made orthonormal, row 2 their cross: forward, left,
// up.
static void blendRot(const s16 a[6], const s16 b[6], float u, float rot[3][3]) {
	float ra[3][3], rb[3][3];
	unpackRot(a, ra);
	unpackRot(b, rb);
	float f[3], l[3];
	for (int c = 0; c < 3; c++) {
		f[c] = ra[0][c] + (rb[0][c] - ra[0][c]) * u;
		l[c] = ra[1][c] + (rb[1][c] - ra[1][c]) * u;
	}
	float n = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	if (n < 1e-6f) { f[0] = 1.0f; f[1] = f[2] = 0.0f; n = 1.0f; }
	for (int c = 0; c < 3; c++) f[c] /= n;
	const float d = l[0] * f[0] + l[1] * f[1] + l[2] * f[2];
	for (int c = 0; c < 3; c++) l[c] -= d * f[c];
	n = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
	if (n < 1e-6f) { l[0] = -f[1]; l[1] = f[0]; l[2] = 0.0f; n = sqrtf(l[0] * l[0] + l[1] * l[1]); if (n < 1e-6f) { l[0] = 0.0f; l[1] = 1.0f; n = 1.0f; } }
	for (int c = 0; c < 3; c++) l[c] /= n;
	for (int c = 0; c < 3; c++) {
		rot[0][c] = f[c];
		rot[1][c] = l[c];
	}
	rot[2][0] = f[1] * l[2] - f[2] * l[1];
	rot[2][1] = f[2] * l[0] - f[0] * l[2];
	rot[2][2] = f[0] * l[1] - f[1] * l[0];
}

// The position on the cubic through both frames with their velocities as
// its slopes -- a pose between two frames 1/15 s or more apart.
static void hermite(const float pa[3], const float pb[3], const float va[3], const float vb[3],
		float dt, float u, float out[3]) {
	const float u2 = u * u, u3 = u2 * u;
	const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f, h10 = u3 - 2.0f * u2 + u;
	const float h01 = -2.0f * u3 + 3.0f * u2, h11 = u3 - u2;
	for (int k = 0; k < 3; k++)
		out[k] = h00 * pa[k] + h10 * dt * va[k] + h01 * pb[k] + h11 * dt * vb[k];
}

static float lerpf(float a, float b, float u) { return a + (b - a) * u; }

// A jump between the frames -- a reset, a teleported traffic car -- is not
// swept through.
static bool jumped(const float pa[3], const float pb[3], const float va[3], float dt) {
	const float dx = pb[0] - pa[0], dy = pb[1] - pa[1], dz = pb[2] - pa[2];
	const float v = sqrtf(va[0] * va[0] + va[1] * va[1] + va[2] * va[2]);
	const float lim = 5.0f + v * dt * 2.0f;
	return dx * dx + dy * dy + dz * dz > lim * lim;
}

static void applyFull(const ReplayCarRef* ref, const FullRec* a, const FullRec* b, float u, float dt) {
	CarSim* s = ref->sim;
	float va[3], vb[3];
	for (int k = 0; k < 3; k++) {
		va[k] = (float)a->vel[k] * 0.01f;
		vb[k] = (float)b->vel[k] * 0.01f;
	}
	if (jumped(a->rbPos, b->rbPos, va, dt)) {
		if (u >= 0.5f) {
			a = b;
			memcpy(va, vb, sizeof(va));
		}
		b = a;
		u = 0.0f;
		dt = 0.0f;
	}
	const FullRec* n = u < 0.5f ? a : b;
	if (dt > 0.0f)
		hermite(a->rbPos, b->rbPos, va, vb, dt, u, s->rbPos);
	else
		memcpy(s->rbPos, a->rbPos, sizeof(s->rbPos));
	blendRot(a->rot, b->rot, u, s->rot);
	for (int k = 0; k < 3; k++)
		s->rbVel[k] = lerpf(va[k], vb[k], u);
	s->clock = lerpf(a->clock, b->clock, u);
	const float since = u * dt;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		CarSimWheel* w = &s->wheel[i];
		const float wa = (float)a->wW[i] / 64.0f, wb = (float)b->wW[i] / 64.0f;
		// The spin carried on from the frame before at its own rate: a
		// wheel turns more than half a turn a frame at speed, which no
		// blend of two angles can show.
		w->angle = wrapPi(unpackAngle(a->wAng[i]) + wa * since);
		w->w = lerpf(wa, wb, u);
		s->susp[i].x = lerpf((float)a->suspX[i], (float)b->suspX[i], u) / 10000.0f;
		s->hydExt[i] = lerpf((float)a->hydExt[i], (float)b->hydExt[i], u) / 10000.0f;
		s->spinnerAngle[i] = unpackAngle(n->spinner[i]);
		w->onGround = (n->wGround >> i) & 1;
		w->terrain = n->terrain[i];
		w->gripUse = (float)n->grip[i] / 255.0f;
		w->sndSlip[0] = (float)n->slip0[i] / 4.0f;
		w->sndSlip[1] = (float)n->slip1[i] / 4.0f;
	}
	s->mlSteerVis = lerpf((float)a->steerVis, (float)b->steerVis, u) / 100.0f;
	s->steerU16 = (u16)((s32)a->steerU16 + (s32)lrintf((float)(s16)(b->steerU16 - a->steerU16) * u));
	s->rpmSmooth = lerpf((float)a->rpm, (float)b->rpm, u);
	s->brakeIn = lerpf((float)a->brake, (float)b->brake, u) / 255.0f;
	s->ebrakeIn = (float)n->ebrake / 255.0f;
	s->thr = lerpf((float)a->thr, (float)b->thr, u) / 255.0f;
	s->dtGear = n->dtGear;
	s->dtShifting = n->dtShifting;
	s->wheelsDown = n->wheelsDown;
	s->mellow = n->mellow;
	s->totaled = (n->flags & 4) != 0;
	s->nosLeft = n->nosLeft;
	s->nosOnAt = n->nosOnAt;
	s->nosShot = n->nosShot;
	s->vfwd = s->rbVel[0] * s->rot[0][0] + s->rbVel[1] * s->rot[0][1] + s->rbVel[2] * s->rot[0][2];
	s->speedAbs = sqrtf(s->rbVel[0] * s->rbVel[0] + s->rbVel[1] * s->rbVel[1] + s->rbVel[2] * s->rbVel[2]);
	carsim_publish(s);
	if (ref->simReady) *ref->simReady = (n->flags & 1) != 0;
	if (ref->inactive) *ref->inactive = (n->flags & 2) != 0;
	if (ref->blinkSec) *ref->blinkSec = (float)n->blink / 50.0f;
	if (ref->lastThr) *ref->lastThr = s->thr;
}

static void applyTraf(TrafficCar* c, const TrafRec* a, const TrafRec* b, float u, float dt) {
	CarSim* s = &c->sim;
	float va[3], vb[3];
	for (int k = 0; k < 3; k++) {
		va[k] = (float)a->vel[k] * 0.01f;
		vb[k] = (float)b->vel[k] * 0.01f;
	}
	if (a != b && (a->type != b->type || a->tex != b->tex || jumped(a->rbPos, b->rbPos, va, dt))) {
		if (u >= 0.5f) {
			a = b;
			memcpy(va, vb, sizeof(va));
		}
		b = a;
		u = 0.0f;
		dt = 0.0f;
	}
	const TrafRec* n = u < 0.5f ? a : b;
	if (dt > 0.0f)
		hermite(a->rbPos, b->rbPos, va, vb, dt, u, s->rbPos);
	else
		memcpy(s->rbPos, a->rbPos, sizeof(s->rbPos));
	blendRot(a->rot, b->rot, u, s->rot);
	for (int k = 0; k < 3; k++)
		s->rbVel[k] = lerpf(va[k], vb[k], u);
	const float since = u * dt;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		const float wa = (float)a->wW[i] / 64.0f, wb = (float)b->wW[i] / 64.0f;
		s->wheel[i].angle = wrapPi(unpackAngle(a->wAng[i]) + wa * since);
		s->wheel[i].w = lerpf(wa, wb, u);
	}
	c->drv.steerU16 = (u16)((s32)a->steerU16 + (s32)lrintf((float)(s16)(b->steerU16 - a->steerU16) * u));
	c->drv.prevBrake = (float)n->brake / 255.0f;
	c->used = (n->flags & 1) != 0;
	c->active = (n->flags & 2) != 0;
	c->mellow = (n->flags & 4) != 0;
	s->wheelsDown = n->wheelsDown;
	c->type = n->type;
	c->tex = (C3D_Tex**)(uintptr_t)n->tex;
	s->vfwd = s->rbVel[0] * s->rot[0][0] + s->rbVel[1] * s->rot[0][1] + s->rbVel[2] * s->rot[0][2];
	s->speedAbs = sqrtf(s->rbVel[0] * s->rbVel[0] + s->rbVel[1] * s->rbVel[1] + s->rbVel[2] * s->rbVel[2]);
	carsim_publish(s);
	s->onGround = (n->flags & 8) != 0;
}

void replay_apply(float t) {
	const int ia = frameAt(t);
	if (ia < 0 || !R.bound)
		return;
	const int ib = ia + 1 < R.nIdx ? ia + 1 : ia;
	const float ta = R.idx[ia].t, tb = R.idx[ib].t;
	float u = tb > ta ? (t - ta) / (tb - ta) : 0.0f;
	if (u < 0.0f) u = 0.0f;
	if (u > 1.0f) u = 1.0f;
	const float dt = tb - ta;
	FrameHdr ha, hb;
	const u8* pa = frameData(ia, &ha);
	const u8* pb = frameData(ib, &hb);
	const int nFull = ha.nFull < hb.nFull ? ha.nFull : hb.nFull;
	for (int k = 0; k < nFull && k < R.nFull; k++) {
		FullRec a, b;
		memcpy(&a, pa + k * sizeof(FullRec), sizeof(a));
		memcpy(&b, pb + k * sizeof(FullRec), sizeof(b));
		applyFull(&R.full[k], &a, &b, u, dt);
	}
	if (!R.tr || !R.tr->open)
		return;
	const u8* ta0 = pa + ha.nFull * sizeof(FullRec);
	const u8* tb0 = pb + hb.nFull * sizeof(FullRec);
	int ka = 0, kb = 0;
	for (int i = 0; i < REPLAY_TRAFFIC_MAX && i < TRAFFIC_MAX; i++) {
		const bool inA = (ha.trMask >> i) & 1, inB = (hb.trMask >> i) & 1;
		TrafRec a, b;
		if (inA) memcpy(&a, ta0 + (ka++) * sizeof(TrafRec), sizeof(a));
		if (inB) memcpy(&b, tb0 + (kb++) * sizeof(TrafRec), sizeof(b));
		TrafficCar* c = &R.tr->car[i];
		if (inA && inB)
			applyTraf(c, &a, &b, u, dt);
		else if (inA && u < 0.5f)
			applyTraf(c, &a, &a, 0.0f, 0.0f);
		else if (inB && u >= 0.5f)
			applyTraf(c, &b, &b, 0.0f, 0.0f);
		else
			c->active = false;
	}
}

void replay_events(float t0, float t1, ReplayEvents* ev) {
	ev->backfire = false;
	ev->bottomOut = -1;
	if (R.nIdx <= 0 || t1 <= t0)
		return;
	for (int i = frameAt(t0); i >= 0 && i < R.nIdx && R.idx[i].t <= t1; i++) {
		if (R.idx[i].t <= t0)
			continue;
		FrameHdr h;
		frameData(i, &h);
		if (h.events & 1)
			ev->backfire = true;
		if (h.bottomOut > ev->bottomOut)
			ev->bottomOut = h.bottomOut;
	}
}

float replay_throttle_at(float t) {
	const int i = frameAt(t);
	if (i < 0)
		return 0.0f;
	FrameHdr h;
	frameData(i, &h);
	return (float)h.throttle / 255.0f;
}

int replay_lap_at(float t) {
	const int i = frameAt(t);
	if (i < 0)
		return -1;
	FrameHdr h;
	frameData(i, &h);
	return h.lap;
}

s32 replay_lap_ticks_at(float t) {
	const int i = frameAt(t);
	if (i < 0)
		return 0;
	FrameHdr h;
	frameData(i, &h);
	float since = t - R.idx[i].t;
	if (since < 0.0f)
		since = 0.0f;
	return h.lapTicks + (s32)(since * 4000.0f);
}
