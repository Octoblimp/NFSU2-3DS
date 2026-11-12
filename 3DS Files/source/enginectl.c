// The player car's engine sound controllers. See enginectl.h.
#define ENGINE_TABLES_TYPES_ONLY
#include "enginectl.h"
#include "rev_patterns.h"

#include "dbglog.h"
#include "difficulty.h"
#include "mixmap.h"
#include "rideinfo.h"
#include "sfxline.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void engLog(const char* fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	dbglog_line("ENGCTL ", b);
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------- helpers

// smooth<int|float> (0x80302E50 .. 0x80302EC8): `a` moved toward `b` by at
// most `up` (rising) or `down` (falling).
static float smooth4f(float a, float b, float up, float down)
{
	if (a + up < b) return a + up;
	if (b < a - down) return a - down;
	return b;
}
static float smoothf(float a, float b, float s) { return smooth4f(a, b, s, s); }

static int32_t smooth4i(int32_t a, int32_t b, int32_t up, int32_t down)
{
	if (a + up < b) return a + up;
	if (b < a - down) return a - down;
	return b;
}
static int32_t smoothi(int32_t a, int32_t b, int32_t s) { return smooth4i(a, b, s, s); }


// RedLineDelayPerGear, a Slope(1, 5, 1, 5) (Regenerate 0x800854E0): the gear
// held to 1..5.
static float redLineDelayPerGear(float gear)
{
	const float out0 = 1.0f, out1 = 5.0f, in0 = 1.0f, in1 = 5.0f;
	const float f = clampf((gear - in0) / (in1 - in0), 0.0f, 1.0f);
	return f * (out1 - out0) + out0;
}

// Graph::GetValue (0x801A5214): straight lines through `n` points.
static float graphValue(const float (*p)[2], int n, float x)
{
	if (n > 1) {
		if (x <= p[0][0]) return p[0][1];
		if (p[n - 1][0] <= x) return p[n - 1][1];
		for (int i = 0; i < n - 1; i++) {
			if (p[i][0] <= x && x < p[i + 1][0]) {
				const float dx = p[i + 1][0] - p[i][0];
				if (fabsf(dx) > 1e-6f)
					return (x - p[i][0]) / dx * (p[i + 1][1] - p[i][1]) + p[i][1];
				return (p[i + 1][1] - p[i][1]) * 0.5f + p[i][1];
			}
		}
	}
	return p[0][1];
}

// bSin(unsigned short) (0x80059238): a turn in 65536 steps, by its polynomial.
static float bSin(uint16_t u)
{
	float x = (float)u * 9.5873799e-05f;   // 2 pi / 65536
	float sign = 1.0f;
	if (x >= 4.712389f) {
		x -= 6.2831855f;
	} else if (x >= 1.5707964f) {
		x -= 3.1415927f;
		sign = -1.0f;
	}
	const float x2 = x * x;
	const float x5 = x * x2 * x2;
	const float x7 = x5 * x2;
	return (x7 * x2 * 2.75573e-06f +
	        ((x5 * 0.00833333f + (x - x * x2 * 0.166667f)) - x7 * 0.000198413f)) * sign;
}

// ---------------------------------------------------------------- state

// CSTATE_Base: the player car's (EAXCar), or an opponent's (EAXAITunerCar).
typedef struct {
	float physTrq;    // +0x44 Physics' throttle, at most 30 a frame
	float sndRpm;     // +0x48 the revs on the sound's scale, 1000 idle .. 10000 limiter
	int   thrOn;      // +0x4C the throttle past 30 %
	int   gear;       // +0x50 0 reverse, 1 neutral, 2 first
	float thrPct;     // +0x54 the throttle, 0..100
	float realRpm;    // +0x58 Engine's revs back on the car's scale
	bool  dual;       // +0x74 a decel Ginsu beside the accel one
	int   engIdx;     // +0x78 the kEngineData entry
	const EngineData*    eng;  // +0x7C
	const DualGinsuData* dg;   // +0x80
	int   engLevel;   // +0x90
	int   transLevel; // +0xA0
	int   pov;        // +0xA4
} CarSnd;

// SFXCTL_Physics (0x60010000): the car's throttle, revs and gear in the
// sound's terms.
typedef struct {
	float   sndRpm;    // +0x70
	float   redFrac;   // +0x74 the redline on the sound's scale (nothing here reads it)
	float   idle;      // +0x78
	float   limiter;   // +0x7C
	float   redline;   // +0x80
	float   trq;       // +0x84 GetPhysTRQ
	float   thr;       // +0x8C the throttle, 0..100
	float   thrLast;   // +0x90 ... last frame
	Average dThr;      // +0x94 Average(2) of its change
	int     thrOn;     // +0xBC
	int     gear;      // +0xC8 GetCurGear
	int     gearLast;  // +0xCC GetLastGear
	// +0xD8: a shop, end or intro NIS is playing (#520: main.c's EngineCtlIn
	// .nis, the shop and dyno scenes). UpdateNIS then plays the scene's revs.
	int     nis;
	// UpdateNIS (0x800921FC): +0xD4 the CAR_AUDIO_REV ticks left, +0xDC its
	// state (1 easing down, 2 a rev, 3 a pattern), +0xE0 the tick within the
	// pattern's step (0..3), +0xE4 the steps left, +0xE8 the step, +0xEC the
	// revs and +0xF0 the throttle it plays.
	int     nisRevLeft;
	int     nisState;
	int     nisTick;
	int     nisLeft;
	const RevStep* nisStep;
	float   nisRpm;
	float   nisThr;
	// EAXCar's inbox, SFXMessage 7 (CAR_AUDIO_PATTERN: +0xBC, +0xC0, +0xC4)
	// and 8 (CAR_AUDIO_REV: +0xB0, +0xB4, +0xB8).
	int     patPending, patId;
	int     revPending;
	float   revTicks, revAmt;
	// Port: UpdateNIS ran once a 30 Hz tick (the patterns are timed to the
	// scenes' 30 Hz frames); the time not yet ticked.
	float   nisAcc;
} PhysCtl;

// SFXCTL_Shifting (0x60010030): a gear change's dip and recovery.
typedef struct {
	const ShiftPattern* pat;  // +0x7C
	int   level;      // +0x64 CSTATE +0xA0
	int   holdOver;   // +0x70 1 the frame the post-shift hold runs out
	int   upBegan;    // +0x74 1 the frame an up-shift begins
	int   upCaught;   // +0x78 1 the frame an up-shift reaches its state 2
	int   state;      // +0x80 0 none, 1..3 an up-shift, 4..7 a down-shift
	int   sub;        // +0x84 the state, the frame it is entered; else 0
	int   volAmp;     // +0x88 the LFOs Engine runs: volume
	int   volMs;      // +0x8C ... its period
	int   trqAmp;     // +0x90 torque (nothing sets it)
	int   trqMs;      // +0x94
	int   rpmAmp;     // +0x98 revs
	int   rpmMs;      // +0x9C
	int   holding;    // +0xA0
	int   wasUp;      // +0xA4
	float holdUntil;  // +0xA8
	float began;      // +0xAC
	float rpmFrom;    // +0xB0 Engine's revs the frame before the shift
	InterpLine trq;   // +0xB4 GetShiftingTRQ
	InterpLine rpm;   // +0xD0 GetShiftingRPM
	InterpLine vol;   // +0xEC GetShiftingVOL
	int   oddLevel;   // +0x108
	InterpLine fxRpm; // +0x10C
	InterpLine fxVol; // +0x128
	int   fxOn;       // +0x144
	InterpLine drop;  // +0x148
} ShiftCtl;

// UP_SHIFTING_TRQ_ATTACH_INITIAL_PERCENT and UP_SHIFTING_TRQ_ATTACK_TIME, by
// gear from first: 1.0 and 0 ms throughout.
static const float kUpShiftTrqInitial[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static const int   kUpShiftTrqAttack[4]  = { 0, 0, 0, 0 };

// SFXCTL_AccelTrans (0x60010040): a stab of throttle, or pulling away.
typedef struct {
	InterpLine rpm;   // +0x74
	InterpLine vol;   // +0x90
	InterpLine trq;   // +0xAC
	int   state;      // +0xC8 0 none, 1 BeginAccelTrans, 2 BeginAccelTrans_Idle, 3 its settle, 4 let go
	float began;      // +0xCC
	int   on;         // +0xD0 CSTATE +0x4C
	int   wasOn;      // +0xD4 ... last frame
	const AccelFromIdle* afi;  // +0xD8 g_SND_ACCELFROMIDLE[ENG +0x18]
	int   engOffSweet;  // +0xDC 1 the frame a high-revving lift calls for it
} AccelCtl;

// SFXCTL_Engine (0x60010050): the revs, torque and level the engine plays.
typedef struct {
	Average trqAvg;    // +0x8C Average(3)
	Average rpmAvg;    // +0xB4 Average(2)
	int32_t vol;       // +0xDC
	int     redlining; // +0xE0
	InterpLine redVol; // +0xE4 the redline sample's level
	InterpLine engVol; // +0x100 the engine's level while on the limiter
	int     wobbleUp;  // +0x11C the redline wobble climbing to 200
	float   wobble;    // +0x120
	float   rpm;       // +0x124 GetEngRPM
	float   rpmLast;   // +0x128
	float   torque;    // +0x12C GetEngTorque
	int32_t f130;      // +0x130
	int32_t lowpass;   // +0x134
	int     clutch;    // +0x138
	float   lfoVol;    // +0x13C
	float   lfoRpm;    // +0x140
	float   lfoTrq;    // +0x144
	uint16_t phRpm;    // +0x148
	uint16_t phTrq;    // +0x14A
	uint16_t phVol;    // +0x14C
	int     compress;  // +0x150 start a compression burst
	PathLine comp;     // +0x154
} EngCtl;

// SFXCTL_HybridMotor (0x60010060): the AEMS / Ginsu balance.
typedef struct {
	float   quietSince;  // +0x78
	int32_t aemsVol;     // +0x7C
	int32_t ginVol;      // +0x80
	int32_t decelVol;    // +0x84
	float   graph[5][2]; // +0x90, the Graph at +0x88
	int32_t redVol;      // +0xB8
	float   lowpass;     // +0xBC
	int16_t compressIn;  // +0xC4
	Average dRpm;        // +0xC8 Average(4)
	float   dSnd;        // +0xF0
	float   dEng;        // +0xF4
	float   sndLast;     // +0xF8
	float   ginRpm;      // +0xFC
	float   ginLvl;      // +0x100
	float   aemsLvl;     // +0x104
	float   f1;          // +0x108
	float   f2;          // +0x10C
} HybridCtl;

// SndAITrigger (ctor 0x8008D0C8, Update 0x8008D1F8): a value's average held
// past one threshold, `beginTime` seconds between the two or at once past
// `begin`, turns it on; back past `end` for `endTime`, off. `sign` -1 watches
// for a fall instead of a rise.
typedef struct {
	float   end;        // [0]
	float   begin;      // [1]
	float   beginTime;  // [2]
	float   endTime;    // [3]
	float   sign;       // [4]
	Average avg;        // [5]
	int     active;     // [0xF]
	float   beginLeft;  // [0x10]
	float   endLeft;    // [0x11]
	float   last;       // [0x12] the average, this frame
} AITrigger;

// SFXCTL_AIPhysics (0x60020000; ctor 0x80094EC4, InitCTL 0x80094FC8): an
// opponent's revs, throttle and gear made up from how it moves -- the AI car
// has no engine model the sound can read -- with SndAIStateManager (+0xFC,
// Update 0x8008D5B4) sorting the car into idling, stopped, accelerating or
// slowing down, straight or turning.
typedef struct {
	AITrigger steerP;     // SM +0x04: the steering, degrees, one way
	AITrigger steerN;     // SM +0x50 ... the other (its record negated)
	AITrigger accel;      // SM +0x9C: the speed gained a physics step
	AITrigger decel;      // SM +0xE8 ... lost
	AITrigger thr;        // SM +0x134: the throttle off (nothing reads it)
	int   state;          // SM +0x184: SND_AI_STATE, 0 init .. 6 slowing, turning left
	int   prev;           // SM +0x188
	int   switched;       // SM +0x18C: 1 the frame the state changes
	float since;          // SM +0x190: when it did
	int   cornering;      // +0x294: state 5 or 6
	float steerScale;     // +0x298: the revs' climb, by how hard the car accelerates
	float rnd;            // +0x29C: a shift's random revs, 0..700
	uint16_t lfo;         // +0x2A0: the low gears' wobble phase
	float lfoOut;         // +0x2A4 (nothing reads it)
	int   upCount;        // +0x2A8
	int   downCount;      // +0x2AC
	float dRpm;           // +0x2B4: GenDeltaRPM, this frame's step toward the target revs
	// The race's kind, which picks the revs' climb and the gears' spacing:
	bool  drift;          // DAT_803FCA84 || g_tweakIsDriftRace
	bool  shortTrack;     // DAT_803FCA98 || g_tweakIsShortTrackRace: Street X
} AIPhysCtl;

// One car's controllers: the PlayerCar state's, or an AICar copy's.
typedef struct {
	bool      on;
	bool      ai;       // an AICar state (type 2), not the PlayerCar (type 1)
	int       copy;     // ... its copy, the opponent's slot
	uint32_t* rng;      // bRandom's state (main.c g_bRandom)
	uint32_t  rngOwn;   // ... or, without one, a private one (port guard)
	// SFXCTL base +0x5C: the time a controller has run. A state's controllers
	// are made and updated together, so one clock stands for each one's own.
	float     t;
	// The car, as the controllers read it off the state's Car (+0x44, +0x30).
	EngineCtlIn car;
	CarSnd    cs;
	PhysCtl   phys;
	ShiftCtl  shift;
	AccelCtl  at;
	EngCtl    eng;
	HybridCtl hy;
	AIPhysCtl aip;
	EngineCtlMix mix;
	float     reportIn;
} Ctx;

static Ctx  s_player;
static Ctx  s_ai[ENGINECTL_AI_MAX];
static Ctx* C = &s_player;   // the car whose controllers are running

// An object's outputs in the mix map, or (port guard) a sink when the map
// does not have it. `id` is the PlayerCar state's; an opponent's controllers
// read and write their own copy's in the AICar state.
static int32_t* outOf(uint32_t id)
{
	static int32_t sink[16];
	if (C->ai) id = MIXMAP_AICAR_ID(id, C->copy);
	int32_t* o = mixmapObjOut(id);
	return o ? o : sink;
}

// bRandom(int) and bRandom(float) (0x80059184).
static int randI(int n)
{
	return (int)rideinfo_random(C->rng, (uint32_t)n);
}
static float randF(float f)
{
	return difficulty_random(C->rng, f);
}

// ---------------------------------------------------------------- Physics

// UpdateNIS (0x800921FC): while a NIS plays, the revs and throttle are the
// scene's -- a CAR_AUDIO_PATTERN (SFXMessage 7) plays its table step by step,
// four ticks a step, eased between steps (Slope 0..4); a CAR_AUDIO_REV
// (SFXMessage 8) climbs 9 x its amount and 15 % throttle a tick for its
// count + 1 ticks; otherwise both ease down, 500 revs and 15 % a tick, to
// idle (1000) and 0. The pattern's scale (EAXCar +0xC4) nothing reads.
static void physNisStepValues(PhysCtl* p)
{
	const RevStep* s = p->nisStep;
	const float k = (float)p->nisTick / 4.0f;
	p->nisRpm = (float)s[0].rpm + ((float)s[1].rpm - (float)s[0].rpm) * k;
	p->nisThr = (float)s[0].thr + ((float)s[1].thr - (float)s[0].thr) * k;
}

static void physNisTick(PhysCtl* p)
{
	if (p->nisState == 1 && p->revPending) {
		p->nisState = 2;
		p->revPending = 0;
		p->nisRevLeft = (int)p->revTicks + 1;
	}
	if (p->patPending) {
		const RevPattern* r = &kRevPatterns[p->patId >= 1 && p->patId <= 15 ? p->patId : 0];
		p->patPending = 0;
		p->nisLeft = r->steps;
		p->nisStep = r->step;
		p->nisTick = 0;
		p->nisState = 3;
		physNisStepValues(p);
		if (C == &s_player)
			engLog("nis: CAR_AUDIO_PATTERN %d -- %s, %d steps from %.0f revs", p->patId, r->name,
			       r->steps, p->nisRpm);
		else
			engLog("nis: AI %d CAR_AUDIO_PATTERN %d -- %s, %d steps from %.0f revs", (int)(C - s_ai),
			       p->patId, r->name, r->steps, p->nisRpm);
	}
	if (p->nisState == 2) {
		if (--p->nisRevLeft <= 0) {
			p->nisRevLeft = 0;
			p->nisState = 1;
		} else {
			p->nisRpm += p->revAmt * 900.0f / 100.0f;
			p->nisThr += 15.0f;
		}
	} else if (p->nisState == 3) {
		if (++p->nisTick == 4) {
			p->nisTick = 0;
			if (--p->nisLeft == 0) p->nisState = 1;
			else p->nisStep++;
		}
		if (p->nisState != 1) physNisStepValues(p);
	} else {
		p->nisRpm -= 500.0f;
		p->nisThr -= 15.0f;
	}
	if (p->nisThr < 0.0f) p->nisThr = 0.0f;
	if (p->nisThr > 100.0f) p->nisThr = 100.0f;
	if (p->nisRpm < 1000.0f) p->nisRpm = 1000.0f;
	if (p->nisRpm > 10000.0f) p->nisRpm = 10000.0f;
}

static void physicsUpdateNis(float dt)
{
	PhysCtl* p = &C->phys;
	for (p->nisAcc += dt; p->nisAcc >= 1.0f / 30.0f; p->nisAcc -= 1.0f / 30.0f)
		physNisTick(p);
	p->thr = p->nisThr;
	p->sndRpm = p->nisRpm;
	p->thrOn = p->nisThr > 30.0f;
	p->trq = p->nisThr;
	C->cs.physTrq = p->nisThr;
	C->cs.sndRpm = p->nisRpm;
	C->cs.thrOn = p->thrOn != 0;
	C->cs.gear = p->gear;
	C->cs.thrPct = p->thr;
}

// UpdateParams (0x8009469C).
static void physicsUpdateParams(float dt)
{
	// IsPlayingShopNIS || IsPlayingEndNIS || IsPlayingIntroNIS (but for its
	// +0xD0 game-state case) || a pending NIS count.
	C->phys.nis = C->car.nis;
	if (C->phys.nis) {
		physicsUpdateNis(dt);
		return;
	}
	C->phys.nisAcc = 0.0f;
	C->phys.gearLast = C->phys.gear;
	const float last = C->phys.thr;
	C->phys.thrLast = last;
	C->phys.gear = C->car.gear;
	C->phys.thr = C->car.throttle * 100.0f;
	avgRecord(&C->phys.dThr, C->phys.thr - last);
	avgRecalc(&C->phys.dThr);
	// +0xC0 / +0xC4 keep CSTATE +0x34 (a time) at each change; nothing reads them.
	if (C->phys.thr <= 30.0f) {
		if (C->phys.thrOn) C->phys.thrOn = 0;
	} else if (!C->phys.thrOn) {
		C->phys.thrOn = 1;
	}
	C->phys.trq = smoothf(C->phys.trq, C->phys.thr, 30.0f);
	// TestRPM (0x8046E318) is 100, which means the car's own revs.
	C->phys.sndRpm = (C->car.rpm - C->phys.idle) / (C->phys.limiter - C->phys.idle) * 9000.0f + 1000.0f;
	C->cs.physTrq = C->phys.trq;
	C->cs.sndRpm = C->phys.sndRpm;
	C->cs.thrOn = C->phys.thrOn != 0;
	C->cs.gear = C->phys.gear;
	C->cs.thrPct = C->phys.thr;
	// Not ported: the blown-engine speech line (IsEngineBlown).
}

// UpdateMixerOutputs (0x80094AB4).
static void physicsMixerOutputs(int32_t* o)
{
	static const float kMph[4] = { 30.0f, 60.0f, 100.0f, 140.0f };
	const float mph = fabsf(C->car.speed * 2.237f);
	for (int i = 0; i < 4; i++)
		o[i] = clampi((int32_t)(mph / kMph[i] * 32767.0f), 0, 0x7fff);
	o[4] = clampi((int32_t)((10000.0f - C->cs.sndRpm) / 9000.0f * 32767.0f), 0, 0x7fff);
	o[5] = C->car.wheelsDown < 1;
	// How far outside the car the view is: the chase views, the hood, the rest.
	int32_t outside = 0;
	switch (C->cs.pov) {
	case 0: case 1: case 5: outside = 32000; break;
	case 4: outside = 4000; break;
	default: outside = 0; break;
	}
	o[6] = smoothi(o[6], outside, 0x3fff);
	// The player in zone 6 or 7 (Car +0x18's Player +0x58), the crash or the
	// jump camera: 8 under a CubicCameraMover (its +8 == 1, the fallback when
	// no trackside camera could be placed), 7 under the trackside one.
	o[7] = 0;
	o[8] = 0;
	if (C->car.zone == 6 || C->car.zone == 7) {
		if (C->car.cubicCam) o[8] = 1;
		else o[7] = 1;
	}
	// Port stand-in: output 9 eases toward the player's place in a race of more
	// than one car; free roam's 0 until races feed the mix.
	o[9] = 0;
}

// ---------------------------------------------------------------- Shifting

// CleanUpShiftFX (0x80090D84), with PostShiftFX_End (0x80090C60).
static void shiftCleanUp(void)
{
	C->shift.rpmMs = 0;
	C->shift.fxOn = 0;
	C->shift.volAmp = C->shift.volMs = 0;
	C->shift.trqAmp = C->shift.trqMs = 0;
	C->shift.rpmAmp = 0;
	C->shift.sub = 0;
	C->shift.state = 0;
}

// PostShiftFX_Init (0x80090C84): the wobble after an up-shift into second,
// third or fourth, smaller the higher the gear.
static void shiftFxInit(void)
{
	const ShiftPattern* p = C->shift.pat;
	const int g = C->phys.gear;
	if (g >= 5) return;
	float scale = 1.0f;
	if (g == 3) scale = 0.85f;
	if (g == 4) scale = 0.7f;
	C->shift.fxOn = 1;
	lineInit(&C->shift.fxRpm, (float)SHIFT_I(p, 0x30) * scale, 0.0f, SHIFT_I(p, 0x38), 0);
	lineInit(&C->shift.fxVol, (float)SHIFT_I(p, 0x24), 0.0f, SHIFT_I(p, 0x2C), 0);
}

// PostShiftFX_Update (0x80090BAC).
static void shiftFxUpdate(float dt)
{
	if (!C->shift.fxOn) return;
	lineUpdate(&C->shift.fxRpm, dt);
	lineUpdate(&C->shift.fxVol, dt);
	if (!C->shift.fxRpm.done) {
		C->shift.rpmAmp = (int)C->shift.fxRpm.cur;
		C->shift.rpmMs = SHIFT_I(C->shift.pat, 0x34);
		C->shift.volAmp = (int)C->shift.fxVol.cur;
		C->shift.volMs = SHIFT_I(C->shift.pat, 0x28);
	} else {
		shiftCleanUp();
	}
}

// Both begins hand Engine its revs of the frame before.
static void shiftSwapEngineRpm(void)
{
	C->shift.rpmFrom = C->eng.rpmLast;
	const float r = C->eng.rpm;
	C->eng.rpm = C->eng.rpmLast;
	C->eng.rpmLast = r;
}

// BeginUpShift (0x80090910).
static void shiftBeginUp(void)
{
	const ShiftPattern* p = C->shift.pat;
	shiftCleanUp();
	if (C->phys.sndRpm < 3000.0f) return;
	C->shift.state = C->shift.sub = 1;
	shiftSwapEngineRpm();
	C->shift.began = C->t;
	const float from = C->shift.rpmFrom;
	lineInit(&C->shift.trq, C->phys.trq, 0.0f, 100, 0);
	lineInit(&C->shift.rpm, from, from - (float)SHIFT_I(p, 0x0C), SHIFT_I(p, 0x10), 0);
	lineInit(&C->shift.vol, 0.0f, 0.0f, 1, 0);
	lineInit(&C->shift.drop, from, from - 1200.0f, SHIFT_I(p, 0x10) + SHIFT_I(p, 0x18), 6);
	C->shift.upBegan = 1;
	C->shift.holding = 1;
	C->shift.holdUntil = C->t + SHIFT_F(p, 0x08);
	C->shift.wasUp = 1;
}

// BeginDownShift (0x80090A64): not into neutral.
static void shiftBeginDown(void)
{
	const ShiftPattern* p = C->shift.pat;
	if (C->phys.gear == 1) return;
	shiftCleanUp();
	C->shift.state = C->shift.sub = 4;
	shiftSwapEngineRpm();
	C->shift.began = C->t;
	const float from = C->shift.rpmFrom;
	lineInit(&C->shift.trq, C->phys.trq, 0.0f, 50, 0);
	const float to = clampf(from - (float)SHIFT_I(p, 0x44), 1000.0f, 10000.0f);
	lineInit(&C->shift.rpm, from, to, SHIFT_I(p, 0x48), 0);
	lineInit(&C->shift.vol, 0.0f, 0.0f, 1, 0);
	C->shift.wasUp = 0;
	C->shift.holding = 1;
	C->shift.holdUntil = C->t + SHIFT_F(p, 0x40);
}

// UpdateRPM (0x80090848) and UpdateTorque (0x800907A0): states 2, 3 and 7
// chase the car; 1, 4, 5 and 6 run free.
static void shiftUpdateLines(float dt)
{
	if (!C->shift.drop.done) lineTrack(&C->shift.drop, dt, C->phys.sndRpm);
	switch (C->shift.state) {
	case 2: case 3: case 7: lineTrack(&C->shift.rpm, dt, C->phys.sndRpm); break;
	case 1: case 4: case 5: case 6: lineUpdate(&C->shift.rpm, dt); break;
	default: break;
	}
	switch (C->shift.state) {
	case 2: case 3: case 7: lineTrack(&C->shift.trq, dt, C->phys.trq); break;
	case 1: case 4: case 5: case 6: lineUpdate(&C->shift.trq, dt); break;
	default: break;
	}
}

// UpdateGearShiftState (0x80090110).
static void shiftUpdateState(float dt)
{
	const ShiftPattern* p = C->shift.pat;
	if (C->shift.holdUntil < C->t && C->shift.holding) {
		C->shift.holdOver = 1;
		C->shift.holding = 0;
	}
	C->shift.sub = 0;
	if (C->shift.state == 0) return;
	shiftUpdateLines(dt);
	lineUpdate(&C->shift.vol, dt);
	shiftFxUpdate(dt);
	switch (C->shift.state) {
	case 1: {
		if (!C->shift.rpm.done) return;
		C->shift.state = C->shift.sub = 2;
		const float r = C->phys.sndRpm;
		const float from = clampf(r + (float)SHIFT_I(p, 0x14), 1000.0f, 10000.0f);
		lineInit(&C->shift.rpm, from, r, SHIFT_I(p, 0x18), 0);
		const int i = clampi(C->cs.gear - 2, 0, 3);
		lineInit(&C->shift.trq, kUpShiftTrqInitial[i] * C->phys.trq, C->phys.trq, kUpShiftTrqAttack[i], 0);
		lineInit(&C->shift.vol, SHIFT_F(p, 0x20), 0.0f, SHIFT_I(p, 0x1C), 0);
		shiftFxInit();
		shiftFxUpdate(dt);
		C->shift.upCaught = 1;
		return;
	}
	case 2:
		if (C->shift.rpm.done) C->shift.state = C->shift.sub = 3;
		return;
	case 3:
		if ((!C->shift.fxRpm.done || !C->shift.fxVol.done) && C->shift.fxOn) return;
		shiftCleanUp();
		return;
	case 4: {
		if (!C->shift.rpm.done) return;
		C->shift.state = C->shift.sub = 5;
		int ms = SHIFT_I(p, 0x50);
		if (C->phys.gear < 3) ms = (int)((float)ms * 0.7f);
		const float to = clampf(C->phys.sndRpm + (float)SHIFT_I(p, 0x4C), 1000.0f, 10000.0f);
		lineInit(&C->shift.rpm, C->eng.rpm, to, ms, 6);
		lineInit(&C->shift.trq, C->eng.torque, 100.0f, 215, 0);
		return;
	}
	case 5: {
		if (!C->shift.rpm.done) return;
		C->shift.state = C->shift.sub = 6;
		int ms = SHIFT_I(p, 0x58);
		if (C->phys.gear < 3 && C->shift.level < 2) ms = (int)((float)ms * 0.7f);
		const float e = C->eng.rpm;
		lineInit(&C->shift.rpm, e, e - (float)SHIFT_I(p, 0x54), ms, 0);
		lineInit(&C->shift.trq, 100.0f, 0.0f, 215, 0);
		return;
	}
	case 6: {
		if (!C->shift.rpm.done) return;
		C->shift.state = C->shift.sub = 7;
		const float ms = clampf(fabsf((C->eng.rpm - C->phys.sndRpm) * SHIFT_F(p, 0x5C)), 0.0f, 800.0f);
		lineInit(&C->shift.rpm, C->eng.rpm, C->phys.sndRpm, (int)ms, 6);
		lineInit(&C->shift.trq, 0.0f, C->phys.trq, 60, 0);
		return;
	}
	case 7:
		if (C->shift.rpm.done) shiftCleanUp();
		return;
	default:
		return;
	}
}

// UpdateParams (0x80090700).
static void shiftUpdateParams(float dt)
{
	C->shift.upCaught = 0;
	C->shift.holdOver = 0;
	C->shift.upBegan = 0;
	if (C->phys.gearLast < C->phys.gear) shiftBeginUp();
	else if (C->phys.gear < C->phys.gearLast) shiftBeginDown();
	shiftUpdateState(dt);
}

// ---------------------------------------------------------------- AccelTrans

static float mph(void)
{
	return C->car.speed * 2.237f;   // EAXCar::GetVelocityMagnitudeMPH (0x8007FE68)
}

// ShouldBeginAccelTrans_Idle (0x8009304C): pulling away in first, from idle.
static bool atShouldBeginIdle(void)
{
	if (!(mph() <= 15.0f && 30.0f <= C->cs.thrPct - C->phys.thrLast && !C->phys.nis)) return false;
	if (C->at.state != 0) return false;
	// Port stand-in: DAT_803FCA54 != 8, a game mode the port has not identified
	// (the race position test reads the same word), taken as true.
	if (C->shift.state == 0 && C->cs.gear == 2)
		return C->cs.sndRpm <= 1500.0f;
	return false;
}

// ShouldBeginAccelTrans (0x80093140): a stab of throttle while moving.
static bool atShouldBegin(void)
{
	if (C->cs.thrPct - C->phys.thrLast < 30.0f || C->at.state != 0 || C->shift.state != 0) return false;
	// The shift point is the car's own revs, the test the sound's (as retail).
	if (C->car.upRpm - 250.0f < C->cs.sndRpm || C->cs.sndRpm < 3000.0f || C->t - C->at.began < 2.0f)
		return false;
	// DriverInfo +4 == 2 (an AI driver) passes in any gear; the player's car
	// needs second or above.
	return C->ai || C->cs.gear > 2;
}

// ShouldPlayEngOffSweet (0x80093264).
static bool atShouldEngOffSweet(void)
{
	if (C->at.state == 0 && C->shift.state == 0 && 6000.0f <= C->cs.sndRpm)
		return C->cs.gear > 2;
	return false;
}

// BeginAccelTrans (0x80092E4C). (It also copies Env_ShiftAttack to the stack
// and does nothing with it.)
static void atBegin(void)
{
	lineInit(&C->at.rpm, C->cs.sndRpm + 1000.0f, C->cs.sndRpm, 500, 6);
	C->at.state = 1;
	lineInit(&C->at.vol, 0.8f, 0.0f, 200, 0);
	lineInit(&C->at.trq, 100.0f, 100.0f, 10, 0);
	C->at.began = C->t;
}

// BeginAccelTrans_Idle (0x80092F90).
static void atBeginIdle(void)
{
	const AccelFromIdle* a = C->at.afi;
	C->at.state = 2;
	C->at.began = C->t;
	const float r = C->cs.sndRpm;
	lineInit(&C->at.rpm, r, r + (float)a->rpmAdd, a->msBegin, 6);
	lineInit(&C->at.vol, 0.0f, a->vol, a->msBegin, 0);
	lineInit(&C->at.trq, C->cs.physTrq, C->cs.physTrq, 10, 0);
}

// UpdateState (0x80092CC8), with UpdateRPM (0x80092BC8) and UpdateTRQ
// (0x80092C4C).
static void atUpdateState(float dt)
{
	const AccelFromIdle* a = C->at.afi;
	lineUpdate(&C->at.vol, dt);
	switch (C->at.state) {
	case 2: lineUpdate(&C->at.rpm, dt); break;
	case 1: case 3: case 4: lineTrack(&C->at.rpm, dt, C->cs.sndRpm); break;
	default: break;
	}
	if (C->at.state >= 1 && C->at.state <= 4) {
		lineTrack(&C->at.trq, dt, C->cs.physTrq);
		if (C->at.trq.done) lineInit(&C->at.trq, C->cs.physTrq, C->cs.physTrq, 0, 0);
	}
	if (C->at.state == 0) return;
	if (!C->cs.thrOn && C->at.state != 4) {
		lineInit(&C->at.rpm, C->eng.rpm, C->cs.sndRpm, a->msRelease, 6);
		C->at.state = 4;
		lineInit(&C->at.vol, C->at.vol.cur, 0.0f, a->msRelease, 0);
	}
	if (C->at.state == 2) {
		if (C->at.rpm.done) {
			C->at.state = 3;
			lineInit(&C->at.rpm, C->at.rpm.cur, C->cs.sndRpm, a->msSettle, 0);
			lineInit(&C->at.vol, C->at.vol.cur, 0.0f, a->msSettle, 0);
		}
	} else if (C->at.state == 1 || C->at.state == 3 || C->at.state == 4) {
		if (C->at.rpm.done) C->at.state = 0;
	}
}

// UpdateParams (0x80092AF4).
static void atUpdateParams(float dt)
{
	const int was = C->at.on;
	C->at.engOffSweet = 0;
	C->at.wasOn = was;
	C->at.on = C->cs.thrOn;
	if (C->at.on) {
		if (!was) {
			if (atShouldBeginIdle()) atBeginIdle();
			else if (atShouldBegin()) atBegin();
		}
	} else if (C->at.wasOn && atShouldEngOffSweet()) {
		C->at.engOffSweet = 1;
	}
	atUpdateState(dt);
}

// ---------------------------------------------------------------- Engine

// ShouldTurnOnClutch (0x800920FC). Port stand-in: DriverInfo +0x8E0, a flag
// the port has not identified, taken as 0.
static int engShouldClutch(void)
{
	if (C->car.human && C->at.state == 0)
		return C->eng.rpm <= 2500.0f;
	return 0;
}

// One LFO step: the phase moves on by dt over the period, wrapping at 0xFFFF.
static uint16_t lfoStep(uint16_t ph, float dt, int periodMs)
{
	uint32_t u = ((uint32_t)ph + (uint32_t)(int32_t)(dt / ((float)periodMs / 1000.0f) * 65535.0f)) & 0xffffu;
	u += u / 0xffffu;
	return (uint16_t)u;
}

// UpdateEngineLFO_FX (0x80091DC4): Shifting's three wobbles.
static void engLfo(float dt)
{
	int volAmp = 0, volMs = 0, trqAmp = 0, trqMs = 0, rpmAmp = 0, rpmMs = 0;
	if (C->shift.state == 0) {
		C->eng.phTrq = 0x4097;
		C->eng.lfoTrq = 0.0f;
		C->eng.lfoVol = 0.0f;
		C->eng.phVol = 0x4097;
		C->eng.lfoRpm = 0.0f;
		C->eng.phRpm = 0x4097;
	} else {
		rpmMs = C->shift.rpmMs;
		volAmp = C->shift.volAmp;
		volMs = C->shift.volMs;
		trqAmp = C->shift.trqAmp;
		trqMs = C->shift.trqMs;
		rpmAmp = C->shift.rpmAmp;
	}
	rpmMs = clampi(rpmMs, 1, 10000);
	trqMs = clampi(trqMs, 1, 10000);
	volMs = clampi(volMs, 1, 10000);
	if (rpmAmp != 0) {
		C->eng.phRpm = lfoStep(C->eng.phRpm, dt, rpmMs);
		C->eng.lfoRpm = (float)rpmAmp * bSin(C->eng.phRpm);
	}
	if (trqAmp != 0) {
		C->eng.phTrq = lfoStep(C->eng.phTrq, dt, trqMs);
		C->eng.lfoTrq = (float)trqAmp * bSin(C->eng.phTrq);
	}
	if (volAmp != 0) {
		C->eng.phVol = lfoStep(C->eng.phVol, dt, volMs);
		C->eng.lfoVol = (float)volAmp * bSin(C->eng.phVol);
	}
}

// UpdateCompression (0x800916D8): HybridMotor's trigger starts a burst, a
// random rise and fall of the revs; output 1 marks the frame.
static void engCompression(float dt, int32_t* o)
{
	o[1] = 0;
	pathUpdate(&C->eng.comp, dt);
	if (C->eng.compress) {
		const int ri = randI(100);
		const float rf = randF(100.0f);
		const float amp = rf + 25.0f;
		pathClear(&C->eng.comp);
		pathAdd(&C->eng.comp, 0.0f, amp, ri + 25, 6);
		pathAdd(&C->eng.comp, amp, 0.0f, ri + 25, 6);
		C->eng.compress = 0;
		o[1] = 1;
	}
}

// UpdateRPM (0x80091AC8).
static void engUpdateRpm(void)
{
	float base;
	if (C->shift.state != 0) base = C->shift.rpm.cur;
	else base = C->at.state != 0 ? C->at.rpm.cur : C->cs.sndRpm;
	if (C->eng.clutch && C->shift.state == 0)
		base = smooth4f(C->eng.rpm, C->cs.sndRpm, 999.0f, 60.0f);
	C->eng.rpmLast = C->eng.rpm;
	// Retail adds the revs LFO twice.
	C->eng.rpm = ((base + C->eng.lfoRpm) + C->eng.comp.cur) + C->eng.lfoRpm;
	float rec = base;
	if ((unsigned)(C->shift.state - 1) < 2u) {
		rec = C->shift.drop.cur;
	} else if (C->at.state == 1) {
		rec = C->cs.sndRpm;
	} else if (C->eng.redlining) {
		if (!C->eng.wobbleUp) {
			C->eng.wobble = smoothf(C->eng.wobble, 0.0f, 50.0f);
			if (C->eng.wobble == 0.0f) C->eng.wobbleUp = 1;
		} else {
			C->eng.wobble = smoothf(C->eng.wobble, 200.0f, 50.0f);
			if (C->eng.wobble == 200.0f) C->eng.wobbleUp = 0;
		}
		rec = base - C->eng.wobble;
	}
	avgRecord(&C->eng.rpmAvg, rec);
	avgRecalc(&C->eng.rpmAvg);
	C->cs.realRpm = (C->eng.rpmAvg.avg - 1000.0f) / 9000.0f * (C->phys.limiter - C->phys.idle) + C->phys.idle;
}

// UpdateTorque (0x80091CF4).
static void engUpdateTorque(void)
{
	if (C->shift.state == 0) {
		if (C->at.state == 0) avgRecord(&C->eng.trqAvg, C->cs.physTrq);
		else avgFlush(&C->eng.trqAvg, C->at.trq.cur);
	} else {
		avgFlush(&C->eng.trqAvg, C->shift.trq.cur);
	}
	avgRecalc(&C->eng.trqAvg);
	C->eng.torque = C->eng.trqAvg.avg + C->eng.lfoTrq;
}

// UpdateVolume (0x80091A10).
static void engUpdateVolume(void)
{
	C->eng.vol = 0x7fff;
	if (C->shift.state != 0)
		C->eng.vol += (int32_t)((float)C->eng.vol * C->shift.vol.cur);
	C->eng.vol += (int32_t)C->eng.lfoVol;
}

// UpdateFilterFX (0x80091500): the low-pass closes as the camera pulls away
// (3DCarPos output 1, last frame's: the mix map runs after these).
static void engFilterFx(void)
{
	// 90 instead for a human player in zone 6 or 7 (Car +0x18's Player +0x58):
	// the far trackside camera does not close it as far.
	const float div = C->car.human && (C->car.zone == 6 || C->car.zone == 7) ? 90.0f : 22.0f;
	const int32_t* pos = outOf(MIXMAP_CTL_3DCARPOS);
	const float f = clampf(((float)pos[1] - 6.5f) / div, 0.0f, 1.0f);
	const int32_t q = mixmapQ15FromDb(mixmapCurveOut(6, (int32_t)(f * 32766.0f), true));
	C->eng.lowpass = clampi((int32_t)((float)q / 32767.0f * 23275.0f + 725.0f), 0, 0x7fff);
}

// UpdateRedlining (0x800917B8): on the limiter the engine dips and the redline
// sample comes up. Not for state type 2: an opponent's engine never redlines,
// and its two levels stay where InitCTL put them.
static void engRedlining(float dt)
{
	if (C->ai) return;
	if (!C->eng.redlining) {
		if (C->shift.state == 0 && C->eng.rpm > 9800.0f) {
			C->eng.redlining = 1;
			const float s = redLineDelayPerGear((float)C->cs.gear);
			const float ev = C->eng.engVol.cur, rv = C->eng.redVol.cur;
			lineInit(&C->eng.engVol, ev, 0.15f, (int)(450.0f * ev * s), 0);
			lineInit(&C->eng.redVol, rv, 0.85f, (int)(120.0f * (1.0f - rv) * s), 0);
			C->eng.wobbleUp = 1;
			C->eng.wobble = 0.0f;
		}
	} else if (C->eng.rpm < 9800.0f || C->shift.state != 0) {
		C->eng.redlining = 0;
		const float ev = C->eng.engVol.cur, rv = C->eng.redVol.cur;
		lineInit(&C->eng.engVol, ev, 1.0f, (int)(50.0f * (1.0f - ev)), 0);
		lineInit(&C->eng.redVol, rv, 0.0f, (int)(50.0f * rv), 0);
	}
	lineUpdate(&C->eng.engVol, dt);
	lineUpdate(&C->eng.redVol, dt);
}

// UpdateParams (0x800912D8). Not ported: its first part hands 3DCarPos the
// car's position (the mix map takes it from the game), and UpdateNIS.
static void engUpdateParams(float dt)
{
	int32_t* o = outOf(MIXMAP_CTL_ENGINE);
	C->eng.clutch = engShouldClutch();
	engLfo(dt);
	engCompression(dt, o);
	engUpdateRpm();
	engUpdateTorque();
	engUpdateVolume();
	engFilterFx();
	engRedlining(dt);
}

// ---------------------------------------------------------------- HybridMotor

// UpdateDeltaRPM (0x80093734): the revs' change, averaged over four frames.
static void hyUpdateDeltaRpm(void)
{
	C->hy.dSnd = C->cs.sndRpm - C->hy.sndLast;
	C->hy.dEng = C->eng.rpm - C->eng.rpmLast;
	C->hy.sndLast = C->cs.sndRpm;
	if (!C->phys.nis) {
		if (C->at.state == 0) {
			if (C->shift.state == 0) avgRecord(&C->hy.dRpm, C->hy.dSnd);
			else if (C->shift.state == 5) avgFlush(&C->hy.dRpm, C->hy.dEng);
			else if (C->shift.sub == 2) avgFlush(&C->hy.dRpm, C->hy.dSnd * 0.2f);
			else avgRecord(&C->hy.dRpm, C->hy.dSnd);
		} else if (C->at.state == 1) {
			avgFlush(&C->hy.dRpm, C->hy.dSnd);
		} else {
			avgRecord(&C->hy.dRpm, C->hy.dEng);
		}
	} else {
		avgRecord(&C->hy.dRpm, C->hy.dEng);
	}
	avgRecalc(&C->hy.dRpm);
}

// UpdateSingleMixEng (0x8009394C): one Ginsu.
static void hySingleMix(void)
{
	const EngineData* e = C->cs.eng;
	bool smooth = true, on = true;
	const float f = clampf(1.0f - (e->dRpmNorm - C->hy.dRpm.avg) / e->dRpmNorm, 0.0f, 1.0f);
	C->hy.f1 = f;
	if (C->phys.nis) {
		on = true;
	} else if (!C->car.shifting && C->shift.state == 0) {
		// (Retail also sets +0xC0 when off; nothing reads it.)
		if (C->at.state == 0) on = C->cs.thrPct > 5.0f;
		else smooth = C->at.state != 1;
	} else {
		switch (C->shift.state) {
		case 2: smooth = false; break;
		case 3: case 5: break;
		case 7:
			if (!C->cs.thrOn) on = false;
			else smooth = false;
			break;
		default: on = false; break;
		}
	}
	float aems, gin;
	if (on) {
		aems = f * (e->aemsOn - e->aemsBase) + e->aemsBase;
		gin = f * (e->ginOn - e->ginBase) + e->ginBase;
	} else {
		aems = e->aemsOff;
		gin = e->ginOff;
	}
	const float lpMin = (float)e->lowpassMin;
	float lp = clampf(C->eng.torque / 100.0f * (24000.0f - lpMin) + lpMin, lpMin, 24000.0f);
	if ((float)C->eng.lowpass < lp) lp = (float)C->eng.lowpass;
	if (smooth) {
		C->hy.aemsLvl = smoothf(C->hy.aemsLvl, aems, 0.2f);
		C->hy.ginLvl = smoothf(C->hy.ginLvl, gin, 0.2f);
		C->hy.lowpass = smoothf(C->hy.lowpass, lp, 6000.0f);
	} else {
		C->hy.aemsLvl = aems;
		C->hy.ginLvl = gin;
		C->hy.lowpass = lp;
	}
	const float fv = (float)C->eng.vol / 32000.0f;
	C->hy.aemsVol = (int32_t)(C->hy.aemsLvl * (float)e->aemsVol * fv);
	C->hy.decelVol = 0;
	C->hy.ginVol = (int32_t)(C->hy.ginLvl * (float)e->ginVol * fv);
	C->hy.redVol = (int32_t)((float)e->aemsVol * fv);
}

// UpdateDualMixEng (0x80093D98): an accel and a decel Ginsu, crossfaded by
// torque.
static void hyDualMix(void)
{
	const EngineData* e = C->cs.eng;
	const DualGinsuData* d = C->cs.dg;
	const float a = fabsf(C->hy.dRpm.avg);
	const float f1 = clampf(1.0f - (e->dRpmNorm - a) / e->dRpmNorm, 0.0f, 1.0f);
	const float f2 = clampf(1.0f - (d->dRpmNorm - a) / d->dRpmNorm, 0.0f, 1.0f);
	C->hy.f1 = f1;
	C->hy.f2 = f2;
	bool smooth = true, decel = false;
	if (C->phys.nis) {
		decel = false;
	} else if (!C->car.shifting && C->shift.state == 0) {
		if (C->at.state == 0) decel = !C->cs.thrOn;
		else smooth = C->at.state != 1;
	} else {
		switch (C->shift.state) {
		case 2: smooth = false; break;
		case 3: case 5: break;
		case 7: if (C->cs.thrOn) smooth = false; break;
		default: decel = true; break;
		}
	}
	float aems, gin;
	if (decel) {
		aems = f2 * (d->aemsOn - d->aemsBase) + d->aemsBase;
		gin = f2 * (d->ginOn - d->ginBase) + d->ginBase;
	} else {
		aems = f1 * (e->aemsOn - e->aemsBase) + e->aemsBase;
		gin = f1 * (e->ginOn - e->ginBase) + e->ginBase;
	}
	if (smooth) {
		C->hy.aemsLvl = smoothf(C->hy.aemsLvl, aems, 0.2f);
		C->hy.ginLvl = smoothf(C->hy.ginLvl, gin, 0.2f);
	} else {
		C->hy.aemsLvl = aems;
		C->hy.ginLvl = gin;
	}
	// The decel graph over the Ginsu RPM, opened up by torque; the levels keep
	// the weighting into the next frame (as retail).
	const float t = C->eng.torque / 100.0f;
	const float w = graphValue(C->hy.graph, 5, C->hy.ginRpm) * (1.0f - t) + t;
	C->hy.aemsLvl = C->hy.aemsLvl * w + (1.0f - w);
	C->hy.ginLvl = C->hy.ginLvl * w;
	C->hy.lowpass = (float)C->eng.lowpass;
	const float fv = (float)C->eng.vol / 32767.0f;
	const int32_t aemsBase = (int32_t)((float)(e->aemsVol - d->aemsVol0) * C->eng.torque / 100.0f) + d->aemsVol0;
	C->hy.aemsVol = (int32_t)(C->hy.aemsLvl * (float)aemsBase * fv);
	C->hy.ginVol = (int32_t)(C->hy.ginLvl * (float)e->ginVol * fv * C->eng.torque / 100.0f);
	C->hy.decelVol = (int32_t)(C->hy.ginLvl * (float)d->ginVol * fv * (1.0f - C->eng.torque / 100.0f));
	C->hy.redVol = (int32_t)((float)e->aemsVol * fv);
}

// UpdateParams (0x80093874), with UpdateVolumeRedlining (0x80094324).
static void hyUpdateParams(void)
{
	const EngineData* e = C->cs.eng;
	hyUpdateDeltaRpm();
	const float r = clampf(C->eng.rpm, 1000.0f, 10000.0f);
	C->hy.ginRpm = (r - 1000.0f) / 9000.0f * (e->ginMaxRpm - e->ginMinRpm) + e->ginMinRpm;
	if (C->cs.dual) hyDualMix();
	else hySingleMix();
	C->hy.redVol = (int32_t)((float)C->hy.redVol * C->eng.redVol.cur);
	C->hy.aemsVol = (int32_t)((float)C->hy.aemsVol * C->eng.engVol.cur);
	C->hy.ginVol = (int32_t)((float)C->hy.ginVol * C->eng.engVol.cur);
	C->hy.decelVol = (int32_t)((float)C->hy.decelVol * C->eng.engVol.cur);
}

// UpdateMixerOutputs (0x80094444): cruising steadily above 30 mph for three
// seconds sets off Engine's compression bursts, a random 60..209 frames apart.
static void hyMixerOutputs(int32_t* o)
{
	bool trig = false;
	if (mph() <= 30.0f || 30.0f <= fabsf(C->hy.dRpm.avg)) C->hy.quietSince = C->t;
	else if (C->hy.quietSince + 3.0f < C->t) trig = true;
	if (trig) {
		if (C->hy.compressIn == 0) {
			C->hy.compressIn = (int16_t)(randI(150) + 60);
			C->eng.compress = 1;
		}
		C->hy.compressIn--;
	}
	o[0] = smooth4i(o[0], trig ? 0x7fff : 0, 0x3d7, 0xc4);
}

// ---------------------------------------------------------------- AIPhysics

// SND_AI_RPM_Lengths_FINE, SND_AI_SHORT_TRACK_RPM_Lengths_FINE,
// SND_AI_DRIFT_RPM_Lengths_FINE and SND_AI_DOWNSHIFT_RPMS by gear (GC
// 0x803218D4.., SPEED2.EXE 0x788630, 0x78865C, 0x788688, 0x7886AC; the two
// agree): how fast the revs climb, and where a gear gives way to the one below.
static const float kAiRpmFine[9]  = { 40.0f, 40.0f, 40.5f, 21.0f, 12.0f, 6.5f, 3.6f, 2.0f, 1.0f };
static const float kAiRpmShort[9] = { 40.0f, 40.0f, 30.5f, 12.5f, 6.0f, 3.5f, 2.6f, 1.5f, 1.0f };
static const float kAiRpmDrift[9] = { 40.0f, 40.0f, 40.5f, 21.0f, 12.0f, 7.5f, 3.6f, 2.0f, 1.0f };
static const float kAiDownRpm[9]  = { 6000.0f, 6000.0f, 5700.0f, 6200.0f, 6650.0f, 7000.0f,
                                      7300.0f, 7400.0f, 7500.0f };

// MIN_StateSustainTime (GC 0x80321898, SPEED2.EXE 0x788578): how long a
// SND_AI_STATE holds before it may change.
static const float kAiSustain[7] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f, 0.5f };

// TWK_SND_SteeringMonitor, AccelMonitor, DeccelMonitor and ThrottleMonitor
// (GC 0x80321838, 0x80321850, 0x80321868, 0x80321880; SPEED2.EXE 0x788518 ..
// 0x788560): the average's length, then end, begin, beginTime, endTime, sign.
static const float kTwkSteer[6]    = { 15.0f, 1.0f, 3.0f, 0.5f, 0.5f, 1.0f };
static const float kTwkAccel[6]    = { 3.0f, 0.0f, 1.0f, 0.4f, 0.5f, 1.0f };
static const float kTwkDecel[6]    = { 3.0f, -0.1f, -1.0f, 0.1f, 0.5f, -1.0f };
static const float kTwkThrottle[6] = { 2.0f, 80.0f, 30.0f, 0.2f, 0.5f, -1.0f };

static int aiGearIdx(int g)
{
	return g < 0 ? 0 : (g > 8 ? 8 : g);   // port guard: the tables' nine gears
}

// SndAITrigger::Initialize (0x8008D184) over its record: the average cleared
// (Average::Init, Reset(0)) and the begin timer wound up.
static void trigInit(AITrigger* t, const float* twk, float flip)
{
	memset(t, 0, sizeof(*t));
	t->end = twk[1] * flip;
	t->begin = twk[2] * flip;
	t->beginTime = twk[3];
	t->endTime = twk[4];
	t->sign = twk[5] * flip;
	t->endLeft = -1.0f;
	t->last = -1.0f;
	avgInit(&t->avg, (int)twk[0]);
	t->beginLeft = t->beginTime;
}

// SndAITrigger::Update (0x8008D1F8), with BeginTrigger (0x8008D1D0) and
// EndTrigger (0x8008D1E4).
static void trigUpdate(AITrigger* t, float v, float dt)
{
	avgRecord(&t->avg, v);
	avgRecalc(&t->avg);
	const float a = t->avg.avg;
	t->last = a;
	if (t->active) {
		if (t->sign * t->end < t->sign * a) {
			t->endLeft = t->endTime;
			return;
		}
		t->endLeft -= dt;
		if (t->endLeft >= 0.0f) return;
		t->active = 0;
		t->beginLeft = t->beginTime;
		return;
	}
	if (t->sign * a <= t->sign * t->begin) {
		if (t->sign * a <= t->sign * t->end) {
			t->beginLeft = t->beginTime;
			return;
		}
		t->beginLeft -= dt;
		if (t->beginLeft >= 0.0f) return;
	}
	t->active = 1;
	t->endLeft = t->endTime;
}

// SndAIStateManager::SwitchState (0x8008D9AC).
static void aiSwitchState(int st)
{
	AIPhysCtl* a = &C->aip;
	a->switched = 1;
	a->since = C->t;
	a->prev = a->state;
	a->state = st;
}

// SndAIStateManager::UpdateState (0x8008D7AC), with GeneratePotentialStates
// (0x8008D9D0). Port: the first potential state, the game modes 0, 2 and 3
// in which an AI car only idles, never holds -- the port's opponents race.
static void aiUpdateState(void)
{
	AIPhysCtl* a = &C->aip;
	a->switched = 0;
	const bool idleMode = false;
	const bool slow = C->car.speed * 2.237f < 5.0f;
	const bool acc = a->accel.active != 0;
	const bool dec = a->decel.active != 0;
	const bool decP = dec && a->steerP.active;
	const bool decN = dec && a->steerN.active;
	const int st = a->state;
	if (st >= 3 && st <= 6 && C->t - kAiSustain[st] < a->since) return;
	switch (st) {
	case 0: aiSwitchState(idleMode ? 1 : 3); return;
	case 1: if (!idleMode) aiSwitchState(3); return;
	case 2: if (acc) aiSwitchState(3); return;
	case 3:
		if (decP) aiSwitchState(5);
		else if (decN) aiSwitchState(6);
		else if (dec) aiSwitchState(4);
		else if (slow) aiSwitchState(2);
		return;
	case 4:
		if (decP) aiSwitchState(5);
		else if (decN) aiSwitchState(6);
		else if (acc && !dec) aiSwitchState(3);
		return;
	case 5: case 6:
		if (acc) aiSwitchState(dec ? 4 : 3);
		else if (dec) aiSwitchState(4);
		return;
	default:
		return;
	}
}

// SndAIStateManager::Update (0x8008D5B4): the steering in degrees either
// side, the speed gained over the last physics step (Car +0x440 less
// |Car +0x518|, BeginMovement's copy of the velocity) both ways, and last
// frame's throttle, then the state.
static void aiStateUpdate(const EngineCtlAiIn* in, float dt)
{
	AIPhysCtl* a = &C->aip;
	float deg = (float)in->steer * 360.0f / 65535.0f;
	if (deg > 180.0f) deg -= 360.0f;
	trigUpdate(&a->steerP, deg, dt);
	trigUpdate(&a->steerN, deg, dt);
	trigUpdate(&a->accel, in->dSpeed, dt);
	trigUpdate(&a->decel, in->dSpeed, dt);
	trigUpdate(&a->thr, C->phys.thr, dt);
	aiUpdateState();
}

// GenDeltaRPM (0x800952EC): this frame's step toward the target revs, by
// gear, faster off the throttle; in the low gears scaled by how hard the car
// pulls, with a wobble (+0x2A4) nothing reads.
static float aiGenDeltaRpm(float dt)
{
	AIPhysCtl* a = &C->aip;
	const int g = aiGearIdx(C->phys.gear);
	float f, off = 1.5f;
	if (a->drift) {
		f = kAiRpmDrift[g] * 3.0f;
	} else if (a->shortTrack) {
		f = kAiRpmShort[g] * 4.0f;
	} else {
		f = kAiRpmFine[g] * 3.02f;
		off = 2.5f;
	}
	if (!C->phys.thrOn) f *= off;
	if (C->phys.gear < 5) {
		a->lfo = lfoStep(a->lfo, dt, 1500);
		a->lfoOut = bSin(a->lfo) * 15.0f;
		f *= a->steerScale;
	}
	return f * (dt / 0.03f);
}

// UpdateRPM (0x800954AC): through a shift the revs jump (8000 for a down-
// shift, the gear's floor and a little for an up-shift); else they climb to
// 10000 (9000 pulling away) on the throttle and fall to 4000 off it, or to
// 1000 below 3 mph.
static void aiUpdateRpm(void)
{
	AIPhysCtl* a = &C->aip;
	PhysCtl* p = &C->phys;
	if (C->shift.state != 0) {
		if ((unsigned)(C->shift.state - 4) <= 3u) p->sndRpm = 8000.0f;
		else p->sndRpm = kAiDownRpm[aiGearIdx(p->gear)] + 100.0f + a->rnd;
		return;
	}
	const float mph = C->car.speed * 2.237f;
	if (!p->thrOn) {
		if (mph < 3.0f) p->sndRpm = smoothf(p->sndRpm, 1000.0f, 50.0f);
		else p->sndRpm = smoothf(p->sndRpm, 4000.0f, a->dRpm);
	} else if (1.0f <= a->accel.avg.avg || 5.0f <= mph) {
		p->sndRpm = smoothf(p->sndRpm, 10000.0f, a->dRpm);
	} else {
		p->sndRpm = smoothf(p->sndRpm, 9000.0f, a->dRpm);
	}
}

// UpdateAccel (0x800955DC): on the throttle while accelerating or on nitrous.
static void aiUpdateAccel(const EngineCtlAiIn* in)
{
	AIPhysCtl* a = &C->aip;
	a->steerScale = clampf(fabsf(a->accel.avg.avg) / 0.18f, 0.1f, 1.0f);
	// (Not in the game modes 0, 2 and 3, as above.)
	if (C->shift.state == 0)
		C->phys.thrOn = in->nitro || a->state == 3;
}

// SuggestGear (0x8009570C): the gear the speed calls for, out of eight across
// the car's top speed, from first to one past the top gear.
static int aiSuggestGear(const EngineCtlAiIn* in)
{
	float k = 1.22f;
	if (C->aip.shortTrack) k = 1.25f;
	else if (C->aip.drift) k = 1.65f;
	// Port guard: a car with no top speed sits in first.
	const int g = in->topSpeed > 0.0f ? (int)(in->speed / in->topSpeed * k * 8.0f) : 2;
	const int r = g > 2 ? g : 2;
	return r < in->numGears + 1 ? r : in->numGears + 1;
}

// UpdateGear (0x800958CC): past 9300 an up-shift, under the gear's floor a
// down-shift -- each told to Shifting by a last gear one off the new one.
static void aiUpdateGear(const EngineCtlAiIn* in)
{
	AIPhysCtl* a = &C->aip;
	PhysCtl* p = &C->phys;
	// (Not in the game modes 0, 2 and 3, as above.)
	if (in->gear == 0) {
		p->gear = 2;
		return;
	}
	const int sug = aiSuggestGear(in);
	if ((unsigned)(p->gear - sug + 1) > 2u) p->gear = p->gearLast = sug;
	if (p->sndRpm <= 9300.0f) {
		if (kAiDownRpm[aiGearIdx(p->gear)] <= p->sndRpm || p->gear < 3) return;
		p->gear = aiSuggestGear(in);
		a->downCount = p->gear == p->gearLast ? a->downCount + 1 : 0;
		if (p->gear == 2 && C->car.speed * 2.237f > 10.0f) p->gear = 3;
		p->gearLast = p->gear + 1;
	} else {
		const int g = aiSuggestGear(in);
		p->gear = g < 3 ? 3 : (g > 7 ? 7 : g);
		a->rnd = randF(700.0f);
		p->gearLast = p->gear - 1;
		a->upCount = 0;
	}
}

// UpdateParams (0x80095014). Not ported: its game state 1 / 7 case, which
// parks the sound at idle in first (the port's opponents exist only racing),
// and the NIS case, as for the player.
static void aiPhysicsUpdateParams(const EngineCtlAiIn* in, float dt)
{
	PhysCtl* p = &C->phys;
	AIPhysCtl* a = &C->aip;
	// IsPlayingShopNIS || IsPlayingEndNIS || IsPlayingIntroNIS (not at race
	// stage 4) || a pending NIS count: the scene's revs (#497, UpdateNIS).
	p->nis = in->nis;
	if (p->nis) {
		physicsUpdateNis(dt);
		return;
	}
	// Otherwise +0xDC, +0xEC and +0xF0 get 1, 1000 and 0: a scene's pattern
	// starts from idle.
	p->nisState = 1;
	p->nisRpm = 1000.0f;
	p->nisThr = 0.0f;
	p->nisAcc = 0.0f;
	aiStateUpdate(in, dt);
	a->cornering = a->state == 5 || a->state == 6;
	avgRecord(&p->dThr, in->throttle * 100.0f);
	avgRecalc(&p->dThr);
	p->thrLast = p->thr;
	a->dRpm = aiGenDeltaRpm(dt);
	aiUpdateAccel(in);
	p->thr = (float)p->thrOn * 100.0f;
	aiUpdateRpm();
	p->trq = smoothf(p->trq, p->thrOn ? 100.0f : 0.0f, 50.0f);   // UpdateTorque (0x800956AC)
	p->gearLast = p->gear;
	aiUpdateGear(in);
	C->cs.physTrq = p->trq;
	C->cs.sndRpm = p->sndRpm;
	C->cs.thrOn = p->thr != 0.0f;
	C->cs.gear = p->gear;
	C->cs.thrPct = p->thr;
}

// ---------------------------------------------------------------- API

// SetupCTL + InitCTL of one car's controllers, into C.
static void ctxSetup(Ctx* c, bool ai, int copy, const EngineCtlSetup* s)
{
	C = c;
	memset(C, 0, sizeof(*C));
	C->ai = ai;
	C->copy = copy;
	if (!s) return;
	if (!s->eng || !s->dual || !s->shift || !s->accel) {
		engLog("%s: no engine data: engine sound off", ai ? "AI" : "player");
		return;
	}
	// Port guard: the revs scale divides by limiter - idle.
	if (!(s->limiter > s->idle)) {
		engLog("%s: idle %.0f limiter %.0f: engine sound off", ai ? "AI" : "player", s->idle, s->limiter);
		return;
	}
	C->rngOwn = 0x12345678u;
	C->rng = s->rng ? s->rng : &C->rngOwn;

	C->cs.engIdx = s->engIdx;
	C->cs.eng = s->eng;
	C->cs.dg = s->dual;
	C->cs.dual = s->eng->dual != 0;
	C->cs.engLevel = s->engLevel;
	C->cs.transLevel = s->transLevel;

	// Physics: ctor 0x80094580, SetupCTL 0x800949FC, InitCTL 0x80094A30.
	avgInit(&C->phys.dThr, 2);
	avgFlush(&C->phys.dThr, 0.0f);
	C->phys.sndRpm = 1000.0f;
	C->phys.gear = C->phys.gearLast = 1;
	// The ctor's (0x80094580): UpdateNIS easing down from idle.
	C->phys.nisState = 1;
	C->phys.nisRpm = 1000.0f;
	C->phys.nisThr = 0.0f;
	C->phys.idle = s->idle;
	C->phys.limiter = s->limiter;
	C->phys.redline = s->redline;
	C->phys.redFrac = s->redline / s->limiter * 10000.0f;
	if (ai) {
		// AIPhysics: ctor 0x80094EC4 (first gear), InitCTL 0x80094FC8, and
		// SndAIStateManager::Initialize (0x8008D410).
		C->phys.gear = C->phys.gearLast = 2;
		C->aip.drift = s->drift;
		C->aip.shortTrack = s->shortTrack;
		trigInit(&C->aip.steerP, kTwkSteer, 1.0f);
		trigInit(&C->aip.steerN, kTwkSteer, -1.0f);
		trigInit(&C->aip.accel, kTwkAccel, 1.0f);
		trigInit(&C->aip.decel, kTwkDecel, 1.0f);
		trigInit(&C->aip.thr, kTwkThrottle, 1.0f);
	}

	// Shifting: SetupCTL 0x8008FFBC, InitCTL 0x8009009C. (An opponent's is
	// SFXCTL_AIShifting, which only drops the mixer outputs.)
	C->shift.pat = s->shift;
	C->shift.level = s->transLevel;
	lineIdle(&C->shift.trq);
	lineIdle(&C->shift.rpm);
	lineIdle(&C->shift.vol);
	lineIdle(&C->shift.fxRpm);
	lineIdle(&C->shift.fxVol);
	lineIdle(&C->shift.drop);
	C->shift.oddLevel = 0;   // SetupCTL's level % 2, which InitCTL then clears
	C->shift.wasUp = 1;

	// AccelTrans: ctor 0x80092870, SetupCTL 0x80092954, InitCTL 0x800929FC.
	lineIdle(&C->at.rpm);
	lineIdle(&C->at.vol);
	lineIdle(&C->at.trq);
	C->at.afi = s->accel;

	// Engine: ctor 0x80090E98, InitCTL 0x800910B8.
	avgInit(&C->eng.trqAvg, 3);
	avgInit(&C->eng.rpmAvg, 2);
	lineInit(&C->eng.engVol, 1.0f, 1.0f, 1, 0);
	lineInit(&C->eng.redVol, 0.0f, 0.0f, 1, 0);
	C->eng.compress = 1;
	pathInit(&C->eng.comp, 0.0f, 0.0f, 1);
	C->eng.f130 = 150;
	avgFlush(&C->eng.trqAvg, 0.0f);
	avgFlush(&C->eng.rpmAvg, 0.0f);
	C->eng.lowpass = 24000;
	int32_t* eo = outOf(MIXMAP_CTL_ENGINE);
	switch (s->engLevel) {
	case 1: eo[0] = 0x2aaa; break;
	case 2: eo[0] = 0x5554; break;
	case 3: eo[0] = 0x7fff; break;
	default: eo[0] = 0; break;
	}

	// HybridMotor: ctor 0x80093328, SetupCTL 0x800934CC, InitCTL 0x8009361C.
	// The AICar state has none: CARSFX_AIEngine plays the CAR graph alone.
	if (!ai) {
		avgInit(&C->hy.dRpm, 4);
		avgFlush(&C->hy.dRpm, 0.0f);
		const DualGinsuData* d = s->dual;
		const float lo = (float)d->graphLo, hi = (float)d->graphHi;
		const float span = (float)(d->graphHi - d->graphLo);
		const float g[5][2] = {
			{ 0.0f, 0.0f }, { lo, 0.0f }, { span * d->graphRise + lo, 1.0f },
			{ hi - span * d->graphFall, 1.0f }, { hi, 0.0f },
		};
		memcpy(C->hy.graph, g, sizeof(g));
	}

	C->on = true;
	if (ai) {
		engLog("AI %d: engine %d (%s), engine level %d, gearbox level %d, idle %.0f limiter %.0f%s%s",
		       copy, (int)s->eng->id, s->eng->spuBank, s->engLevel, s->transLevel, s->idle,
		       s->limiter, s->drift ? ", drift" : "", s->shortTrack ? ", short track" : "");
		return;
	}
	engLog("engine %d (%s, gin %s%s%s), engine level %d, gearbox level %d, "
	       "idle %.0f redline %.0f limiter %.0f",
	       (int)s->eng->id, s->eng->spuBank, s->eng->gin,
	       C->cs.dual ? ", decel " : "", C->cs.dual ? s->dual->gin : "",
	       s->engLevel, s->transLevel, s->idle, s->redline, s->limiter);
}

// What CARSFX_PlayerEngine / CARSFX_AIEngine read off the controllers.
static void ctxMix(void)
{
	C->mix.dual = C->cs.dual;
	C->mix.ginRpm = C->hy.ginRpm;
	C->mix.aemsVol = C->hy.aemsVol;
	C->mix.ginVol = C->hy.ginVol;
	C->mix.decelVol = C->hy.decelVol;
	C->mix.redVol = C->hy.redVol;
	C->mix.lowpass = C->hy.lowpass;
	C->mix.torque = C->eng.torque;
	C->mix.f130 = C->eng.f130;
	C->mix.engRpm = C->eng.rpm;
	C->mix.engVol = C->eng.vol;
	C->mix.engIdx = C->cs.engIdx;
	C->mix.physTrq = C->cs.physTrq;
	C->mix.sndRpm = C->cs.sndRpm;
	C->mix.gear = C->cs.gear;
	C->mix.thrPct = C->cs.thrPct;
	C->mix.holdOver = C->shift.holdOver;
	C->mix.upBegan = C->shift.upBegan;
	C->mix.upCaught = C->shift.upCaught;
	C->mix.shiftState = C->shift.state;
	C->mix.rpmFrom = C->shift.rpmFrom;
}

void enginectlSetup(const EngineCtlSetup* s)
{
	ctxSetup(&s_player, false, 0, s);
}

void enginectlUpdate(const EngineCtlIn* in, float dt)
{
	C = &s_player;
	if (!C->on || !in) return;
	C->car = *in;
	C->cs.pov = in->povType;
	C->t += dt;
	// The PlayerCar state's controllers, in ID order: UpdateParams, then
	// UpdateMixerOutputs. (Shifting's outputs, all master volumes, the mix map
	// works out itself; AccelTrans and Engine have none.)
	physicsUpdateParams(dt);
	shiftUpdateParams(dt);
	atUpdateParams(dt);
	engUpdateParams(dt);
	hyUpdateParams();
	physicsMixerOutputs(outOf(MIXMAP_CTL_PHYSICS));
	hyMixerOutputs(outOf(MIXMAP_CTL_HYBRIDMOTOR));
	ctxMix();

	// Debug: every 5 s, where the engine sound stands.
	if ((C->reportIn -= dt) <= 0.0f) {
		C->reportIn = 5.0f;
		engLog("gear %d rpm %.0f snd %.0f eng %.0f gin %.0f trq %.0f; shift %d accel %d "
		       "redline %d; aems %d gin %d decel %d red %d lp %.0f",
		       C->cs.gear, C->car.rpm, C->cs.sndRpm, C->eng.rpm, C->hy.ginRpm, C->eng.torque,
		       C->shift.state, C->at.state, C->eng.redlining, (int)C->hy.aemsVol,
		       (int)C->hy.ginVol, (int)C->hy.decelVol, (int)C->hy.redVol, C->hy.lowpass);
	}
}

const EngineCtlMix* enginectlMix(void)
{
	return &s_player.mix;
}

void enginectlNisPattern(int id)
{
	s_player.phys.patPending = 1;
	s_player.phys.patId = id;
}

void enginectlNisRev(float ticks, float amount)
{
	s_player.phys.revPending = 1;
	s_player.phys.revTicks = ticks;
	s_player.phys.revAmt = amount;
}

void enginectlAiNisPattern(int copy, int id)
{
	if (copy < 0 || copy >= ENGINECTL_AI_MAX) return;
	s_ai[copy].phys.patPending = 1;
	s_ai[copy].phys.patId = id;
}

void enginectlAiNisRev(int copy, float ticks, float amount)
{
	if (copy < 0 || copy >= ENGINECTL_AI_MAX) return;
	s_ai[copy].phys.revPending = 1;
	s_ai[copy].phys.revTicks = ticks;
	s_ai[copy].phys.revAmt = amount;
}

void enginectlAiSetup(int copy, const EngineCtlSetup* s)
{
	if (copy < 0 || copy >= ENGINECTL_AI_MAX) return;
	ctxSetup(&s_ai[copy], true, copy, s);
	C = &s_player;
}

void enginectlAiUpdate(int copy, const EngineCtlAiIn* in, float dt)
{
	if (copy < 0 || copy >= ENGINECTL_AI_MAX || !in || !s_ai[copy].on) return;
	C = &s_ai[copy];
	// What the shared controllers read off the car: an AI driver, in its own
	// gear (Car +0x444), at its speed, its shift point the car's own.
	C->car.gear = in->gear;
	C->car.speed = in->speed;
	C->car.throttle = in->throttle;
	C->car.upRpm = in->upRpm;
	C->car.human = false;
	C->t += dt;
	// The AICar state's controllers, in ID order: AIPhysics, AIShifting,
	// AccelTrans and Engine. None of their outputs reaches the map (the state
	// has no MixCtls), so there are no UpdateMixerOutputs to run.
	aiPhysicsUpdateParams(in, dt);
	shiftUpdateParams(dt);
	atUpdateParams(dt);
	engUpdateParams(dt);
	ctxMix();

	if ((C->reportIn -= dt) <= 0.0f) {
		C->reportIn = 5.0f;
		engLog("AI %d: %.0f mph, d %+.3f, steer %+.0f; state %d accel %d decel %d; gear %d "
		       "snd %.0f eng %.0f trq %.0f vol %d; shift %d at %d",
		       copy, C->car.speed * 2.237f, C->aip.accel.avg.avg, C->aip.steerP.last,
		       C->aip.state, C->aip.accel.active, C->aip.decel.active, C->cs.gear,
		       C->cs.sndRpm, C->eng.rpm, C->eng.torque, (int)C->eng.vol, C->shift.state,
		       C->at.state);
	}
	C = &s_player;
}

const EngineCtlMix* enginectlAiMix(int copy)
{
	static const EngineCtlMix off;
	if (copy < 0 || copy >= ENGINECTL_AI_MAX || !s_ai[copy].on) return &off;
	return &s_ai[copy].mix;
}

void enginectlAiShutdown(void)
{
	memset(s_ai, 0, sizeof(s_ai));
}
