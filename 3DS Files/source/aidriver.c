#include "aidriver.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG2RAD 0.0174532925f
#define RAD2DEG 57.2957795f

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// Table::GetValue (0x801A4EC8), the same reader carsim.c uses: n samples
// spread evenly over [min, max], linear between, clamped at both ends.
static float tableGet(const float* d, int n, float min, float max, float x) {
	float t = (x - min) * ((float)(n - 1) / (max - min));
	if (t <= 0.0f) return d[0];
	int i = (int)t;
	if (i >= n - 1) return d[n - 1];
	return d[i] + (t - (float)i) * (d[i + 1] - d[i]);
}

// Graph::GetValue (0x801A5214): a point list walked linearly, clamped outside
// the first and last x.
static float graphGet(const float (*p)[2], int n, float x) {
	if (x <= p[0][0]) return p[0][1];
	for (int i = 1; i < n; i++) {
		if (x <= p[i][0]) {
			float dx = p[i][0] - p[i - 1][0];
			float t = dx > 0.0f ? (x - p[i - 1][0]) / dx : 0.0f;
			return p[i - 1][1] + t * (p[i][1] - p[i - 1][1]);
		}
	}
	return p[n - 1][1];
}

// ---------------------------------------------------------------------------
// The engine's own numbers. Every array here was read out of Speed.elf; see
// docs/retail_ai_driver.md for where each one came from.

// The 7-wide driving-style arrays, indexed by GetMovementIndex (0x8001FA8C)
// on the car's movement mode: 1 on the PhysicsMover (mode 1); on the
// MellowMover (mode 3) 0, or 4 for a car in control mode 4 -- traffic; 3 on
// the TrafficMover (mode 2). A circuit or sprint racer is only ever on the
// first two (opponent.h), a traffic car on the last two (traffic.h).
static int movementIndex(const AiDriver* d, const CarSim* s) {
	if (d->trafficMover) return 3;
	if (s->mellow) return d->traffic ? 4 : 0;
	return 1;
}

static const float kSteerP[7] = { 0.9f, 1.0f, 0.8f,  0.7f, 0.85f, 0.5f, 0.0f };
static const float kSteerI[7] = { 0.2f, 0.0f, 0.1f,  1.42f, 0.79f, 1.0f, 0.0f };
static const float kSteerD[7] = { 0.3f, 0.08f, 0.18f, 0.0f, 0.12f, 0.0f, 0.0f };
static const float kAheadSec[7] = { 0.55f, 0.4f, 0.35f, 0.55f, 0.9f, 0.4f, 0.0f };
static const float kAheadMin[7] = { 15.0f, 15.0f, 15.0f, 5.0f, 9.0f, 15.0f, 0.0f };

static const float kThrPAcc[7] = { 0.1f, 0.1f, 0.1f, 0.1f, 0.005f, 0.1f, 0.1f };
static const float kThrIAcc[7] = { 0.02f, 0.02f, 0.02f, 0.02f, 0.005f, 0.02f, 0.02f };
static const float kThrDAcc[7] = { 0.02f, 0.02f, 0.01f, 0.01f, 0.01f, 0.01f, 0.01f };
static const float kThrPDec[7] = { 0.1f, 0.1f, 0.1f, 0.1f, 0.005f, 0.1f, 0.1f };
static const float kThrIDec[7] = { 0.02f, 0.02f, 0.02f, 0.02f, 0.0f, 0.02f, 0.02f };
static const float kThrDDec[7] = { 0.02f, 0.02f, 0.01f, 0.01f, 0.008f, 0.01f, 0.01f };

// The Tables, all 10 samples over [0, 160] m/s unless noted.
static const float kThrottleSpeed[10] = { 30, 15, 7.5f, 6, 6, 6, 6, 6, 6, 6 };
static const float kSteerTo[10] =
	{ 22.8972f, 22.8972f, 24, 27.9f, 33, 39, 45.8f, 49.4f, 53.1f, 55.9f };
static const float kSteerRange[10] = { 45, 30, 5.5f, 5, 3, 2, 2, 2, 2, 2 };
static const float kSteerSpeedVsCar[10] =
	{ 4, 3, 2, 1, 0.45f, 0.14f, 0.1f, 0.1f, 0.1f, 0.1f };
// 20 samples over [0, 95] degrees; every one is 1.0 in the shipped image, so
// this is an identity today. Kept because the lookup is retail's.
static const float kSteerSpeedVsAngle[20] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};
// Indexed by car class, not speed: 2 samples over [0, 3]. Real on the
// PhysicsMover, Mellow on any other.
static const float kSteerToScaleReal[2] = { 1.3f, 1.1f };
static const float kSteerToScaleMellow[2] = { 0.6f, 1.1f };

// The steering controller's coefficients below 10 m/s, by speed.
static const float kPidProportional[10] =
	{ 0.328f, 0.22f, 0.148f, 0.115f, 0.09f, 0.074f, 0.057f, 0.049f, 0.043f, 0.04f };
static const float kPidIntegral[10] =
	{ 0.21f, 0.244f, 0.267f, 0.29f, 0.305f, 0.321f, 0.328f, 0.336f, 0.341f, 0.344f };
static const float kPidDerivative[10] =
	{ 0.0f, 0.07583f, 0.07583f, 0.07383f, 0.0604f, 0.04698f, 0.04698f, 0.04027f,
	  0.04027f, 0.04027f };

// The Graphs.
static const float gPidHeadingBlend[2][2] = { { 5.0f, 0.0f }, { 10.0f, 1.0f } };
// HeadingErrorModelGraph (r13-0x5404): how fast, in degrees a second, a
// heading error of so many degrees should be closing -- half of it a second,
// never more than 5.
static const float gHeadingErrorModel[10][2] = {
	{ 0, 0 }, { 1, -0.5f }, { 3, -1.5f }, { 5, -2.5f }, { 10, -5 },
	{ 50, -5 }, { 60, -5 }, { 70, -5 }, { 80, -5 }, { 90, -5 }
};
// VelocityErrorModelGraph (r13-0x53FC): how fast, in m/s a second, a speed
// error (speed less the desired speed) should be closing.
static const float gVelocityErrorModel[9][2] = {
	{ -20, 100 }, { -10, 10 }, { -5, 7 }, { -2, 3 }, { 0, 0 },
	{ 2, 0 }, { 10, -3 }, { 20, -10 }, { 30, -100 }
};

// fSteeringIntegralClamp (0x8046F9FC), six degrees.
// The non-drift steering rate, 0x80317614 -- normalised units per second.
#define AI_STEER_RATE 100.0f
#define AI_STEER_INTEGRAL_CLAMP 0.10471975f
// fThrottleDerivativeClamp (0x8046CE64) and fPsychoMaxBrake (0x8046CE68).
#define AI_THROTTLE_DERIV_CLAMP 10.0f
#define AI_MAX_BRAKE 1.0f
// GetSteeringError's lateral arm (0x8031757C).
#define AI_LATERAL_ARM 5.0f
// The slip window RecordWheelSlip ignores (0x80317590 / 0x80317594).
#define AI_SLIP_WINDOW 4.0f
// Drive's steer-away: a Collision touched within this long (0x803175FC), and
// older than this (0x80317618), against a car slower than this (0x8031761C,
// 60 mph) or a wall.
#define AI_STEERAWAY_LIVE 0.1f
#define AI_STEERAWAY_AGE  0.6f
#define AI_STEERAWAY_CAR  26.821636f

// fSmoothCurvatureCarFriction / fSmoothCurvatureCarBraking (0x80339174 /
// 0x80339164), by car class.
static const float kClassFriction[4] = { 3.7f, 3.4f, 3.1f, 2.8f };
static const float kClassBraking[4] = { 12.0f, 10.6f, 9.3f, 8.0f };
// PerformanceClassRange (0x80333638).
static const float kClassRange[4] = { 0.0f, 273.0f, 401.0f, 500.0f };

// ---------------------------------------------------------------------------
// Average and PidError.

static void avgInit(AiAverage* a, int cap) {
	memset(a, 0, sizeof(*a));
	a->cap = (u8)(cap > AI_AVG_MAX ? AI_AVG_MAX : cap);
}

// Average::Record (0x801A571C).
static void avgRecord(AiAverage* a, float v) {
	if (a->count < a->cap) a->count++;
	a->sum = a->sum + v - a->d[a->idx];
	a->d[a->idx] = v;
	if (++a->idx >= a->cap) a->idx = 0;
	a->avg = a->sum / (float)a->count;
}

// Average::Reset (0x801A57C0): every slot takes `v`, and the count goes back
// to zero so the next average is over the samples since the reset.
static void avgReset(AiAverage* a, float v) {
	for (int i = 0; i < a->cap; i++) a->d[i] = v;
	a->count = 0;
	a->sum = v * (float)a->cap;
}

static void pidInit(AiPidError* p, float k) {
	avgInit(&p->dt, AI_AVG_MAX);
	avgInit(&p->integ, AI_AVG_MAX);
	avgInit(&p->deriv, AI_AVG_MAX);
	p->k = k;
	p->last = p->prev = 0.0f;
}

static void pidReset(AiPidError* p) {
	avgReset(&p->integ, 0.0f);
	avgReset(&p->deriv, 0.0f);
	p->last = p->prev = 0.0f;
}

// PidError::Reset(v): ResetIntegral(v) then ResetDerivative(v). Each puts
// `last` back to 0 and `prev` to v, and fills its own Average with v.
static void pidResetTo(AiPidError* p, float v) {
	avgReset(&p->integ, v);
	avgReset(&p->deriv, v);
	p->last = 0.0f;
	p->prev = v;
}

// PidError::Record (0x801A5C18). The first Average takes the frame time, the
// second the trapezoidal dt*error, the third the error's rate of change.
// `noDeriv` / `noInteg` (its two bools) record a 0 in that Average instead,
// for a frame where the error jumped because its target did.
static void pidRecordEx(AiPidError* p, float err, float dt, bool noDeriv, bool noInteg) {
	float prev = p->last;
	p->last = err;
	p->prev = prev;
	float delta = err - prev;
	avgRecord(&p->dt, dt);
	avgRecord(&p->integ, noInteg ? 0.0f : dt * (prev + delta * 0.5f));
	avgRecord(&p->deriv, noDeriv ? 0.0f : (dt > 0.0f ? delta / dt : 0.0f));
}

static void pidRecord(AiPidError* p, float err, float dt) {
	pidRecordEx(p, err, dt, false, false);
}

// What Drive and CalcThrottleAndBrake read back as the integral term:
// integ.sum * count / (K * dt.sum), and zero before the first sample.
static float pidIntegral(const AiPidError* p) {
	if (p->integ.count == 0) return 0.0f;
	float den = p->k * p->dt.sum;
	if (den == 0.0f) return 0.0f;
	return p->integ.sum * (float)p->integ.count / den;
}

// ---------------------------------------------------------------------------
// AdaptivePIDControllerBase / Complicated: the circuit and sprint steering.
// Adaptation rule 0 is the only one PsychoDriver asks for, so it is the only
// one here.

// AverageWindow(0.2 s, 45 a second): (int)(0.2 x 45 + 0.5) slots.
#define AI_APID_WINDOW_SEC 0.2f
#define AI_APID_WINDOW_CAP 9

// The base constructor (0x8003014C); Complicated's (0x80030B74) zeroes the
// terms. PsychoDriver's own figures are Drive's, set every frame (apidLimits).
static void apidInit(AiAdaptivePid* c) {
	memset(c, 0, sizeof(*c));
	c->window = 0.2f;
	for (int i = 0; i < AI_APID_TERMS; i++) {
		c->termMin[i] = -99999.0f;
		c->termMax[i] = 99999.0f;
		c->coefMax[i] = 1.0f;
		c->threshold[i] = 0.01f;
		c->rate[i] = 5e-05f;
		carsim_window_init(&c->coefRate[i], AI_APID_WINDOW_CAP, AI_APID_WINDOW_SEC);
	}
	carsim_window_init(&c->errRate, AI_APID_WINDOW_CAP, AI_APID_WINDOW_SEC);
}

// GetSensitivityDerivative (0x80030878): how the error has been moving over
// how coefficient `rate` has, or just which way the error moves while the
// coefficient has not.
static float apidSensitivity(const AiAdaptivePid* c, float rate) {
	const float e = c->errRate.avg;
	if (fabsf(rate) > 1e-9f) return clampf(e / rate, -1000.0f, 1000.0f);
	if (fabsf(e) <= 0.001f) return 0.0f;
	return e <= 0.0f ? -1.0f : 1.0f;
}

// UpdateBase (0x80030444), through Complicated's Update (0x80030C44), whose
// error is the caller's two arguments' difference. `t` is the world clock:
// each `window` seconds of it tune the next term in turn.
static void apidUpdate(AiAdaptivePid* c, float err, float dt, float t) {
	const int i = (int)(t / c->window) % AI_APID_TERMS;
	float dcoef = 0.0f;
	if (c->threshold[i] <= fabsf(c->term[i])) {
		// GetNewCoefficientDerivative (0x80030740), rule 0.
		dcoef = -c->rate[i] * err * apidSensitivity(c, c->coefRate[i].avg);
		c->coef[i] = clampf(c->coef[i] + dcoef * dt, c->coefMin[i], c->coefMax[i]);
	}
	carsim_window_record(&c->coefRate[i], dcoef, t);
	for (int k = 0; k < AI_APID_TERMS; k++)
		if (k != i) carsim_window_record(&c->coefRate[k], 0.0f, t);
	carsim_window_record(&c->errRate, (err - c->lastErr) / (dt < 0.001f ? 0.001f : dt), t);
	c->lastErr = err;
}

// GetOutput (0x80030634).
static float apidOutput(const AiAdaptivePid* c) {
	float out = 0.0f;
	for (int i = 0; i < AI_APID_TERMS; i++)
		out += c->coef[i] * clampf(c->term[i], c->termMin[i], c->termMax[i]);
	return out;
}

// The limits Drive sets before every update (0x8002015C..0x800202A8): the
// PhysicsMover's (an SUV's, Car +0xC's +0x88A, higher), Street X's on any
// other mover, or the MellowMover's.
static void apidLimits(const AiDriver* d, const CarSim* s, AiAdaptivePid* c) {
	float pLo, pHi, iHi, dLo, dHi;
	c->window = 0.2f;
	c->rate[0] = 5e-05f;
	c->rate[1] = 5e-05f;
	c->rate[2] = 5e-06f;
	if (!s->mellow) {
		pLo = d->suv ? 0.4f : 0.15f;
		pHi = d->suv ? 0.7f : 0.5f;
		iHi = 0.5f;
		dLo = 0.0f;
		dHi = 0.08f;
	} else if (d->shortTrack) {
		c->rate[0] = 5e-06f;
		pLo = 0.275f;
		pHi = 0.3f;
		iHi = 0.3f;
		dLo = 0.0f;
		dHi = 0.06f;
	} else {
		pLo = 0.15f;
		pHi = 0.4f;
		iHi = 0.1f;
		dLo = 0.04f;
		dHi = 0.06f;
	}
	c->coefMin[0] = pLo; c->coefMax[0] = pHi;
	c->coefMin[1] = 0.0f; c->coefMax[1] = iHi;
	c->coefMin[2] = dLo; c->coefMax[2] = dHi;
	for (int i = 0; i < AI_APID_TERMS; i++) c->threshold[i] = 0.01f;
}

// ---------------------------------------------------------------------------

int aidriver_car_class_raw(float pts) {
	if (pts > kClassRange[3]) return 0;
	for (int k = 2; k >= 0; k--)
		if (pts > kClassRange[k]) return 3 - k;
	return 3;
}

int aidriver_car_class(float measure, float lo, float hi) {
	float m = clampf(measure, 0.0f, 10.0f);
	return aidriver_car_class_raw(lo + (m / 10.0f) * (hi - lo));
}

float aidriver_class_grip(int carClass) {
	return kClassFriction[carClass < 0 ? 0 : (carClass > 3 ? 3 : carClass)];
}

float aidriver_class_braking(int carClass) {
	return kClassBraking[carClass < 0 ? 0 : (carClass > 3 ? 3 : carClass)];
}

void aidriver_init(AiDriver* d, int carClass) {
	memset(d, 0, sizeof(*d));
	// PsychoDriver's constructor (0x8001EFF4) gives every PidError K = 1.
	pidInit(&d->heading, 1.0f);
	pidInit(&d->lateral, 1.0f);
	pidInit(&d->blended, 1.0f);
	pidInit(&d->throttle, 1.0f);
	pidInit(&d->slipSigned, 1.0f);
	pidInit(&d->slipAbs, 1.0f);
	d->throttleMax = 1.0f;
	d->dirState = 1;
	d->carClass = carClass < 0 ? 0 : (carClass > 3 ? 3 : carClass);
	apidInit(&d->steer);
	// AdaptivePIDControllerSimple's constructor (0x8003094C), called with
	// nThrottleIntegralTerms / nThrottleDerivativeTerms (4 / 4): its PidError
	// keeps four samples and K = 30.
	apidInit(&d->throttleAdaptive);
	pidInit(&d->throttleSpeed, 30.0f);
	d->throttleSpeed.dt.cap = d->throttleSpeed.integ.cap = d->throttleSpeed.deriv.cap = 4;
	d->airSince = -1.0f;
}

void aidriver_zero_memory(AiDriver* d) {
	pidReset(&d->heading);
	pidReset(&d->blended);
	pidReset(&d->throttle);
	d->steerU16 = 0;
	d->prevThrottle = d->prevBrake = 0.0f;
}

float aidriver_lookahead(const AiDriver* d, const CarSim* s, float speed) {
	const int mi = movementIndex(d, s);
	if (mi == 3 || mi == 4) {
		float a = speed * kAheadSec[mi];
		return a > kAheadMin[mi] ? a : kAheadMin[mi];
	}
	// The speed table, scaled by the car's class.
	return tableGet(kSteerTo, 10, 0.0f, 160.0f, speed) *
		tableGet(s->mellow ? kSteerToScaleMellow : kSteerToScaleReal, 2, 0.0f, 3.0f,
			(float)d->carClass);
}

// bATan (0x8005961C) in radians, then RadianWrap: the signed angle from `b`
// to `a`, in (-pi, pi].
static float wrapPi(float a) {
	while (a > (float)M_PI) a -= 2.0f * (float)M_PI;
	while (a <= -(float)M_PI) a += 2.0f * (float)M_PI;
	return a;
}

// PsychoDriver::GetSteeringError (0x8001F7DC).
static void steeringError(const AiDriver* d, const CarSim* s, float speed,
		float* outHeading, float* outBlended, float* outLateral) {
	float bearing = atan2f(d->steerTo[1] - s->pos[1], d->steerTo[0] - s->pos[0]);
	float eH = wrapPi(s->yaw - bearing);
	*outHeading = eH;

	float blend = graphGet(gPidHeadingBlend, 2, speed);
	if (blend <= 0.0f) {
		*outBlended = eH;
	} else {
		float eV = wrapPi(atan2f(s->vel[1], s->vel[0]) - bearing);
		*outBlended = blend * eV + (1.0f - blend) * eH;
	}
	*outLateral = wrapPi(atan2f(d->lateral_m, AI_LATERAL_ARM));
}

void aidriver_clear_steering(AiDriver* d, const CarSim* s) {
	float eH, eB, eL;
	steeringError(d, s, s->speed, &eH, &eB, &eL);
	pidResetTo(&d->heading, eH);
	pidResetTo(&d->lateral, eL);
	pidResetTo(&d->blended, eB);
}

// PsychoDriver::RecordWheelSlip (0x8001FAF0): only the wheels whose slip is
// outside +/-4 count, which is retail's way of ignoring ordinary cornering.
static void recordWheelSlip(AiDriver* d, const CarSim* s, float dt, float vfwd) {
	float sumSigned = 0.0f, sumAbs = 0.0f;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		float a = s->wheel[i].s;
		float b = fabsf(s->wheel[i].slip);
		if (a < -AI_SLIP_WINDOW || a > AI_SLIP_WINDOW) sumSigned += a;
		if (b < -AI_SLIP_WINDOW || b > AI_SLIP_WINDOW) sumAbs += b;
	}
	if (vfwd < 0.0f) sumSigned = -sumSigned;
	pidRecord(&d->slipSigned, sumSigned, dt);
	pidRecord(&d->slipAbs, sumAbs, dt);
}

// Drive's steering for a circuit or sprint racer (0x80020050..0x80020398):
// the self-tuning controller over the blended P, I and D. The error it tunes
// out is how fast the heading error closes, in degrees a second, against
// HeadingErrorModelGraph; below 10 m/s forward its coefficients are the speed
// tables', every frame.
//
// PORT (TASKS.md #280): off. Retail tunes it against its own drive target,
// FuturePoints with its corner smoothing; the port's target is the line at
// the look-ahead, cut back to as little as 8 m through a tight bend
// (opponent.c's chordAhead), and steering at a point that close takes more
// than the 0.4 the MellowMover lets the proportional term reach. On it, the
// whole field across every track ran 3,241 s off the line and needed 87
// unsticks; on the fixed weights, 121 s and 13. So every racer takes the
// fixed-weight branch that retail keeps for the drift and drag styles.
static const bool kSteerAdaptive = false;

static float steerAdaptive(AiDriver* d, const CarSim* s, float speed, float vfwd,
		float P, float I, float D, float dt) {
	AiAdaptivePid* c = &d->steer;
	// D, signed + while the error grows and - while it closes.
	const float dS = P * D < 0.0f ? -fabsf(D) : fabsf(D);
	c->term[0] = -P;
	c->term[1] = -I;
	c->term[2] = -D;
	apidLimits(d, s, c);
	apidUpdate(c, dS * RAD2DEG - graphGet(gHeadingErrorModel, 10, fabsf(P * RAD2DEG)),
		dt, d->clock);
	if (vfwd < 10.0f) {
		c->coef[0] = tableGet(kPidProportional, 10, 0.0f, 160.0f, speed);
		c->coef[1] = tableGet(kPidIntegral, 10, 0.0f, 160.0f, speed);
		c->coef[2] = tableGet(kPidDerivative, 10, 0.0f, 160.0f, speed);
	}
	float out = apidOutput(c);
	if (c->term[2] > 999.0f) out *= 999.0f;   // 0x8031760C; D is held to 1
	return out;
}

// Drive with neither front wheel down (0x800203A0..0x800204D4): for 0.3 s
// the wheel stays where it is, then it lines up with the way the car is
// travelling, no more than 5 degrees either way on the MellowMover. The
// controller's coefficients are held down meanwhile, so it lands gentle and
// tunes itself back up.
static float steerAirborne(AiDriver* d, const CarSim* s, float curDeg, float vfwd) {
	if (d->airSince < 0.0f) d->airSince = d->clock;
	float cmd;
	if (d->clock - d->airSince <= 0.3f) {
		cmd = curDeg * DEG2RAD;
	} else {
		// CarState::GetYaw (0x801AA320): the heading less the direction of
		// travel, 0 below 1 m/s forward.
		cmd = vfwd < 1.0f ? 0.0f : -wrapPi(s->yaw - atan2f(s->vel[1], s->vel[0]));
		if (s->mellow) cmd = clampf(cmd, -5.0f * DEG2RAD, 5.0f * DEG2RAD);
	}
	AiAdaptivePid* c = &d->steer;
	const bool suvReal = !s->mellow && d->suv;
	const float pHi = suvReal ? 0.4f : 0.15f, dHi = suvReal ? 0.1f : 0.05f;
	if (c->coef[0] > pHi) c->coef[0] = pHi;
	if (c->coef[1] > 0.0f) c->coef[1] = 0.0f;
	if (c->coef[2] > dHi) c->coef[2] = dHi;
	return cmd;
}

// AdaptivePIDControllerSimple (PsychoDriver +0x318): the throttle above
// AI_THR_ADAPTIVE_SPEED. CalcThrottleAndBrake sets its limits every frame
// (0x800212A8..0x80021354), then Update (0x80030AD4) records the speed error
// in the controller's own PidError and tunes against how fast that error
// closes, less VelocityErrorModelGraph's figure for it. The terms are that
// PidError's (GetTerm, 0x80030A3C): the error, its integral and its rate.
// Below 10 m/s the coefficients are -1, 0, 0 (0x800213A8), so a car under
// way after a stop is on full throttle.
//
// PORT (TASKS.md #280): off, as the steering's is (kSteerAdaptive). Retail
// tunes it against the speeds its FuturePoints carry; the port's limit is the
// speed profile's, cornered on the tyres' own grip (aispeed_build), and on it
// the rate term -- up to 0.6 of the throttle per m/s^2 -- brakes the car under
// the limit wherever the limit falls, so the field arrives at each corner
// bunched. Across every track, against the fixed weights on the same
// profiles, it raced 1.2% slower and touched 3,719 times to 2,554 (485 s of
// contact to 337), ran 65 s off the road to 44, and four racers left it on
// 4101F and 4107B who otherwise did not.
static const bool kThrottleAdaptive = false;
#define AI_THR_ADAPTIVE_SPEED 30.0f   // 0x8031768C
static float throttleAdaptive(AiDriver* d, float speed, float want, float dt) {
	AiAdaptivePid* c = &d->throttleAdaptive;
	c->window = 0.2f;
	c->coefMin[0] = -0.3f; c->coefMax[0] = -0.2f;
	c->coefMin[1] = 0.0f;  c->coefMax[1] = 0.0f;
	c->coefMin[2] = -0.6f; c->coefMax[2] = 0.0f;
	c->termMin[2] = -AI_THROTTLE_DERIV_CLAMP;
	c->termMax[2] = AI_THROTTLE_DERIV_CLAMP;
	for (int i = 0; i < AI_APID_TERMS; i++) {
		c->threshold[i] = 0.01f;
		c->rate[i] = 5e-06f;
	}
	// A desired speed that moved by over 1 m/s since last frame records no
	// rate of change, so the step is not taken for the car's own.
	const bool jump = fabsf(d->desired - d->desiredPrev) > 1.0f;
	AiPidError* e = &d->throttleSpeed;
	pidRecordEx(e, speed - want, dt, jump, false);
	c->term[0] = e->last;
	c->term[1] = pidIntegral(e);
	c->term[2] = e->deriv.avg;
	apidUpdate(c, c->term[2] - graphGet(gVelocityErrorModel, 9, speed - want), dt, d->clock);
	if (speed < 10.0f) {
		c->coef[0] = -1.0f;
		c->coef[1] = c->coef[2] = 0.0f;
	}
	return apidOutput(c);
}

// PsychoDriver::CalcThrottleAndBrake (0x80021114). The gate in front of the
// self-tuning branch (0x800211E8..0x800212A4) is open for an AI racer
// (AdaptiveControlIsDesired: DriverInfo +4 is not 3) off line and outside the
// drift and drag events, which is every racer here; the port keeps it shut
// (kThrottleAdaptive), so every racer takes the fixed weights at any speed.
static void calcThrottleAndBrake(AiDriver* d, const CarSim* s, float dt,
		float speed, float* outThrottle, float* outBrake) {
	const int mi = movementIndex(d, s);
	float want = fabsf(d->desired);

	// Below 5 m/s the driver does not modulate: it is on or hard on the brake.
	if (want < 5.0f) {
		if (fabsf(want - speed) <= 1.0f) { *outThrottle = 0.0f; *outBrake = 0.0f; }
		else if (want <= speed)          { *outThrottle = 0.0f; *outBrake = 1.0f; }
		else                             { *outThrottle = 1.0f; *outBrake = 0.0f; }
		return;
	}

	float out, lo, hi;
	if (kThrottleAdaptive && want > AI_THR_ADAPTIVE_SPEED) {
		out = throttleAdaptive(d, speed, want, dt);
		lo = -1.0f;
		hi = 1.0f;
	} else {
		float integ = pidIntegral(&d->throttle);
		float deriv = clampf(d->throttle.deriv.avg,
			-AI_THROTTLE_DERIV_CLAMP, AI_THROTTLE_DERIV_CLAMP);
		float p, i, dd;
		if (want + 2.0f <= speed) {       // too fast: the braking gains
			p = kThrPDec[mi]; i = kThrIDec[mi]; dd = kThrDDec[mi];
			lo = -AI_MAX_BRAKE; hi = 0.0f;
		} else {                          // the accelerating gains
			p = kThrPAcc[mi]; i = kThrIAcc[mi]; dd = kThrDAcc[mi];
			lo = 0.0f; hi = d->throttleMax;
		}
		out = integ * i + d->throttle.last * p + deriv * dd;
	}

	// The control may only move this far in a frame, by the car's speed
	// (0x80021550 reads Car +0x440, not the desired speed): quick off the
	// line, a fifth of that flat out.
	float lim = tableGet(kThrottleSpeed, 10, 0.0f, 160.0f, speed) * dt;
	out = clampf(out, -lim, lim);
	float cmd = clampf((d->prevThrottle - d->prevBrake) + out, lo, hi);

	// Traction control: a ceiling that falls as the wheels give up.
	float tc = 1.0f;
	{
		float sI = pidIntegral(&d->slipSigned);
		float a, b, c;
		if (cmd < 0.0f) { a = 0.02f;  b = 0.02f; c = 0.005f; }
		else            { a = -0.02f; b = -0.2f; c = -0.005f; }
		tc = 1.0f + sI * a + d->slipSigned.last * b + d->slipSigned.deriv.avg * c;
		if (tc < 0.0f) tc = 0.0f;
	}

	if (cmd >= 0.0f) {
		float v = cmd < tc ? cmd : tc;
		*outThrottle = v;
		*outBrake = 0.0f;
	} else {
		float v = -cmd < tc ? -cmd : tc;
		*outThrottle = 0.0f;
		*outBrake = v > 0.0f ? v : 0.0f;
	}
	*outThrottle = clampf(*outThrottle, 0.0f, d->throttleMax);
	*outBrake = clampf(*outBrake, 0.0f, 1.0f);
}

// Drive's steer-away (0x800203F8..0x80020534), after the PID and only with a
// front wheel down: a Collision on the body (Car+0x3C) still going and past
// AI_STEERAWAY_AGE, against a wall or a car under AI_STEERAWAY_CAR, takes the
// wheel instead -- PsychoSteeringRangeTable's lock at this speed, eased in
// over the second after that, away from where it touched (WorldToCar's x).
// The first one Drive comes to wins. PORT: carsim keeps a run of contact per
// side of the body (recordTouch), so these are the four sides in
// CARSIM_TOUCH_* order, not retail's Collisions in CollisionBody order.
static float steerAway(const CarSim* s, float sdir, float cmd) {
	for (int c = 0; c < 4; c++) {
		if (s->touchLast[c] < 0.0f) continue;
		const float age = s->clock - s->touchStart[c];
		if (s->clock - s->touchLast[c] >= AI_STEERAWAY_LIVE || age <= AI_STEERAWAY_AGE)
			continue;
		if (s->touchWith[c] != CARSIM_TOUCH_WALL && s->touchWith[c] >= AI_STEERAWAY_CAR)
			continue;
		float ease = age - AI_STEERAWAY_AGE;
		if (ease > 1.0f) ease = 1.0f;
		const float away = ease * tableGet(kSteerRange, 10, 0.0f, 160.0f, sdir) * DEG2RAD;
		return s->touchX[c] > 0.0f ? away : -away;
	}
	return cmd;
}

void aidriver_drive(AiDriver* d, const CarSim* s, float dt, CarSimInput* out) {
	memset(out, 0, sizeof(*out));
	if (dt < 0.0001f) dt = 0.0001f;       // Drive's own floor (0x8031759C)
	d->clock += dt;                       // WorldTimer runs before GO too

	// CarDriver +0x1C4 read back as degrees, wrapped the way Drive wraps it.
	float curDeg = (float)d->steerU16 * (360.0f / 65536.0f);
	if (curDeg > 180.0f) curDeg -= 360.0f;

	float vfwd = s->vel[0] * s->rot[0][0] + s->vel[1] * s->rot[0][1] +
		s->vel[2] * s->rot[0][2];
	float speed = s->speed;               // CarState +0x440
	float sdir = vfwd < 0.0f ? -speed : speed;   // the signed speed the tables take

	out->racing = d->racing;
	out->steerIsAngle = true;
	out->aiDriver = true;
	out->gear = -1;

	// Still on the grid: hold everything, as RaceCarController does before GO.
	// The handbrake and not the brake -- carsim's brake pedal is also reverse
	// from a standstill (RealDriver), so a driver standing on it before GO
	// backs its car off the line instead of holding it there.
	if (!d->racing) {
		d->prevThrottle = d->prevBrake = 0.0f;
		out->steerAngleU16 = d->steerU16;
		out->handbrake = true;
		return;
	}

	// ---- steering
	// The drift and drag styles (index 3 / 4: cr5 in Drive) steer on fixed
	// weights and the rate tables; every other style, which is every racer
	// in a circuit or sprint, through the self-tuning controller -- which the
	// port leaves off (kSteerAdaptive). (Movement mode 4 and index 5, a timed
	// steering action, take the fixed weights too; no car here is in either.)
	const int mi = movementIndex(d, s);
	const bool drifting = ((unsigned)(mi - 3)) <= 1u;
	float P = 0.0f, cmd;
	if (carsim_front_wheel_down(s)) {
		d->airSince = -1.0f;
		float eH, eB, eL;
		steeringError(d, s, speed, &eH, &eB, &eL);
		pidRecord(&d->heading, eH, dt);
		pidRecord(&d->lateral, eL, dt);
		pidRecord(&d->blended, eB, dt);

		// The squared-error blend of the heading and velocity-blended errors.
		// Pulling opposite ways, the driver trusts the heading error alone.
		float e1 = d->heading.last, e2 = d->blended.last;
		float wH = 1.0f, wB = 0.0f;
		if (e1 * e2 >= 0.0f) {
			float ss = e1 * e1 + e2 * e2;
			wH = wB = 0.5f;
			if (ss >= 1e-5f) { wH = (e1 * e1) / ss; wB = (e2 * e2) / ss; }
		}

		P = wH * e1 + wB * e2;
		float D = clampf(wH * d->heading.deriv.avg + wB * d->blended.deriv.avg,
			-1.0f, 1.0f);
		float I = clampf(wH * pidIntegral(&d->heading) + wB * pidIntegral(&d->blended),
			-AI_STEER_INTEGRAL_CLAMP, AI_STEER_INTEGRAL_CLAMP);

		if (kSteerAdaptive && !drifting)
			cmd = steerAdaptive(d, s, speed, vfwd, P, I, D, dt);
		else
			cmd = -P * kSteerP[mi] - D * kSteerD[mi] - I * kSteerI[mi];
		cmd = steerAway(s, sdir, cmd);
	} else {
		cmd = steerAirborne(d, s, curDeg, vfwd);
	}
	cmd = clampf(cmd, -45.0f * DEG2RAD, 45.0f * DEG2RAD);

	// ---- which way to drive (0x80020654..0x80020730)
	// Retail decides the pedals here, between the steering command and its
	// range, because backing up turns the wheel the other way: in reverse the
	// tail leads, so the lock that would swing the nose toward the point
	// swings it away.
	pidRecord(&d->throttle, fabsf(d->desired) - speed, dt);
	recordWheelSlip(d, s, dt, vfwd);

	float thr = 0.0f, brk = 0.0f;
	bool calc = false;
	out->gear = -1;                       // SetLowGear: the box's own choice
	if (!carsim_drive_wheel_down(s)) {
		// Nothing to push with: the throttle open, so the wheels are already
		// turning when they come down.
		thr = d->throttleMax;
	} else if (fabsf(d->desired) < 2.0f) {
		brk = 1.0f;
	} else if (d->desired <= 0.0f) {
		// Backwards: brake while still rolling forward past 7 m/s, and under
		// 2 m/s ask for reverse and turn the wheel the other way.
		if (sdir > 7.0f) d->dirState = 0;
		else if (sdir < 2.0f) d->dirState = 1;
		if (d->dirState == 0) {
			brk = 0.9f;
		} else {
			cmd = -cmd;
			out->gear = 0;                // GEAR_R
			calc = true;
		}
	} else {
		// Forwards, but rolling back faster than 7 m/s (off the end of a
		// reverse): brake against it, the wheel still turned for reverse.
		if (sdir < -7.0f) d->dirState = 0;
		else if (sdir > -2.0f) d->dirState = 1;
		if (d->dirState == 0) {
			cmd = -cmd;
			brk = 0.9f;
		} else {
			calc = true;
		}
	}
	if (calc) calcThrottleAndBrake(d, s, dt, speed, &thr, &brk);

	// The range on offer now, and how fast the wheel may move toward it.
	//
	// Everything from here to the store into +0x1C4 branches on cr4 in the
	// retail function (0x80020920 / 0x80020a00 / 0x80020aa4), and cr4 is
	// `(unsigned)(GetMovementIndex() - 3) <= 1` -- the drift and drag styles.
	// A normal driver, which is every racer in a circuit or sprint, takes the
	// other side of all three:
	//
	//   * the steering range still comes from PsychoSteeringRangeTable, max'd
	//     with 0 and min'd with 45 (0x80020998);
	//   * the wheel may move 100 normalised units a second (0x80020a7c), not
	//     the SpeedVsCarSpeed x SpeedVsAngleError product -- at any frame rate
	//     that is no limit at all, so the wheel simply goes where the PID
	//     asks and the rate tables, and the rule that doubles the rate coming
	//     back toward centre, belong to the drift styles alone;
	//   * and the car's own steering scale (CarPhysicsInfo control +0x270) is
	//     NOT applied (0x80020aa4 skips it).
	//
	// Both of the last two matter: `cur` is the stored angle over the range,
	// so scaling only the way back out multiplies the angle by +0x270 every
	// frame. With a real rate limit that settles at a small offset, but a
	// nudge past it winds the wheel up to the 180 degree wrap and parks it
	// there -- which is exactly what the port did before this was checked
	// against the disassembly.
	float range = clampf(tableGet(kSteerRange, 10, 0.0f, 160.0f, sdir), 0.0f, 45.0f);
	if (range <= 0.0f) range = 45.0f;
	float cur = curDeg / range;
	float wantN = clampf(cmd / (range * DEG2RAD), -1.0f, 1.0f);
	float delta = wantN - cur;
	float rate;
	if (drifting) {
		rate = tableGet(kSteerSpeedVsCar, 10, 0.0f, 160.0f, sdir) *
			tableGet(kSteerSpeedVsAngle, 20, 0.0f, 95.0f, fabsf(P) * RAD2DEG);
		// Coming back toward centre is allowed to happen twice as fast.
		if ((cur > 0.0f && wantN < cur) || (cur < 0.0f && cur < wantN)) rate += rate;
	} else {
		rate = AI_STEER_RATE;
	}
	float lim = rate * dt;
	delta = clampf(delta, -lim, lim);
	float newDeg = (cur + delta) * range;
	if (drifting) newDeg *= carsim_steer_scale(s);
	d->steerU16 = (u16)((int)(newDeg * 65536.0f) / 360);
	out->steerAngleU16 = d->steerU16;

	// ---- throttle and brake, as decided above
	d->prevThrottle = thr;
	d->prevBrake = brk;
	d->desiredPrev = d->desired;

	out->throttle = thr;
	out->brake = brk;
}

// ---------------------------------------------------------------------------
// The speed profile.

#define AISPEED_STEP 5.0f

// How far either side of a sample its curvature triangle reaches. Retail does
// not choose this: a CurvaturePoint IS a TrackRoutePoint, so the triangle is
// the two route segments meeting there, and on Bayview's Routes files those
// segments run a median 14.8 m (mean 18.9, p10 6.4, p90 39.9). Measured at
// AISPEED_STEP instead, the triangle spans 10 m -- three times finer than
// anything retail's tool ever saw -- and on the racing line, which weaves a
// metre or two as the authored offsets change, that reads the weave as
// corners. The profile stays at AISPEED_STEP so the braking and acceleration
// scrub keeps its resolution; only the curvature is measured at retail's.
// PORT: one span for the whole route, where retail's varies with the data --
// EA put points closer together where the road bends, so retail sees a tight
// corner over about 6 m and a straight over 40. The median is the compromise:
// against retail this reads the tightest corners a little wider than they are.
// PORT (TASKS.md #280): on a line the span, and the braking and acceleration
// between samples, are the line's own metres (ailine_along), as retail's are
// its route points' -- the same along the race route's road, and the only
// measure that sees a corner where the line leaves it.
#define AISPEED_CURV_SPAN 15.0f
// How much of the tyres' grip the profile plans a corner on (TASKS.md #280).
#define AISPEED_GRIP_USE 1.0f
// PORT: the forward pass's acceleration for a car with no drivetrain record,
// m/s^2 -- retail always has the RidePhysicsInfo table.
#define AISPEED_ACCEL_NONE 4.0f

struct AiSpeedProfile {
	int   n;
	float step;
	float len;
	bool  loops;
	float min;
	float fr, brake, top;   // what it was planned on, for aispeed_corner
	// m/s; the array runs on past this. Floats as retail's FuturePoints keep
	// them: TASKS.md #280, rounded to whole m/s the limit rose in steps, and
	// the throttle's derivative read every step as a lift.
	float v[1];
};

// Curvature (0x801FA46C): the circumscribed circle through three points, as
// 1/R, signed by which way the middle point bends. Retail builds the two
// perpendicular bisectors and intersects them; this is the same circle, taken
// from the triangle's area so there is no line-intersection special case.
static float curvature(const float a[2], const float b[2], const float c[2]) {
	float abx = b[0] - a[0], aby = b[1] - a[1];
	float bcx = c[0] - b[0], bcy = c[1] - b[1];
	float cax = a[0] - c[0], cay = a[1] - c[1];
	float ab = sqrtf(abx * abx + aby * aby);
	float bc = sqrtf(bcx * bcx + bcy * bcy);
	float ca = sqrtf(cax * cax + cay * cay);
	float den = ab * bc * ca;
	if (den < 1e-6f) return 0.0f;
	float cross = abx * bcy - aby * bcx;      // twice the signed area
	if (fabsf(cross) < 1e-6f) return 0.0f;    // three points in a line
	// R = abc / (4 * area); TASKS.md #280: this read abc / (2 * area), twice
	// the radius, and every corner was planned at twice the class's grip.
	float r = den / (2.0f * fabsf(cross));
	if (r < 0.001f) r = 0.001f;               // retail's floor (0x80337548)
	return (cross < 0.0f ? -1.0f : 1.0f) / r;
}

// GetSpeedLimitForCurvature (0x801FA2E4): sqrt(friction * g / curvature),
// with the curvature floored so the answer never passes `maxSpeed`.
static float speedLimitForCurvature(float friction, float curv, float maxSpeed) {
	float a = friction * 9.8f;
	float cMin = a / (maxSpeed * maxSpeed);
	float c = fabsf(curv);
	if (c < cMin) c = cMin;
	float v2 = a / c;
	return v2 > 5e-11f ? sqrtf(v2) : 0.0f;
}

// The acceleration table at `speed`: Table::GetValue over [0, top], held at
// either end -- past the table's top the car pulls nothing more.
static float accelAt(const float* accel, float top, float speed) {
	if (!accel || !(top > 0.0f)) return AISPEED_ACCEL_NONE;
	float x = speed / top * (float)(CARSIM_ACCEL_N - 1);
	if (x <= 0.0f) return accel[0];
	if (x >= (float)(CARSIM_ACCEL_N - 1)) return accel[CARSIM_ACCEL_N - 1];
	int i = (int)x;
	float t = x - (float)i;
	return accel[i] + (accel[i + 1] - accel[i]) * t;
}

AiSpeedProfile* aispeed_build(const RaceRoute* r, const AiLine* line, int carClass,
		float topSpeed, const float* accel, float accelTop, float grip,
		void (*log)(const char* msg)) {
	if (!r) return NULL;
	float len = race_route_length(r);
	if (len <= AISPEED_STEP * 3.0f) return NULL;
	if (carClass < 0) carClass = 0;
	if (carClass > 3) carClass = 3;
	if (topSpeed < 10.0f) topSpeed = 10.0f;

	int n = (int)(len / AISPEED_STEP) + 1;
	AiSpeedProfile* p = (AiSpeedProfile*)malloc(sizeof(AiSpeedProfile) + sizeof(float) * (size_t)n);
	if (!p) return NULL;
	p->n = n;
	p->step = AISPEED_STEP;
	p->len = len;
	p->loops = race_route_loops(r);
	p->min = topSpeed;

	float* pos = (float*)malloc(sizeof(float) * 2 * (size_t)n);
	float* v = (float*)malloc(sizeof(float) * (size_t)n);
	float* run = (float*)malloc(sizeof(float) * (size_t)n);   // metres to the next sample
	if (!pos || !v || !run) { free(pos); free(v); free(run); free(p); return NULL; }

	// Each CurvaturePoint at its own driving-line offset, so the curvature is
	// the racing line's and not the spline's (see the header).
	int onLine = 0;
	for (int i = 0; i < n; i++) {
		const float d = (float)i * AISPEED_STEP;
		AiLineSample s;
		float lat = 0.0f;
		if (ailine_at(line, d, &s)) {
			lat = s.line;
			onLine++;
		}
		float xy[2];
		if (!race_route_sample(r, d, lat, xy, NULL)) {
			// A gap in the splines: carry the last point so the curvature of
			// the two samples either side of it stays finite.
			xy[0] = i > 0 ? pos[2 * (i - 1)] : 0.0f;
			xy[1] = i > 0 ? pos[2 * (i - 1) + 1] : 0.0f;
		}
		pos[2 * i] = xy[0];
		pos[2 * i + 1] = xy[1];
	}

	// CurvaturePoint::CalculateSpeedLimits: the cornering limit, point by point.
	// PORT: no more than the tyres hold (see the header).
	float fr = kClassFriction[carClass];
	if (grip > 0.0f && grip * AISPEED_GRIP_USE < fr) fr = grip * AISPEED_GRIP_USE;
	int span = (int)(AISPEED_CURV_SPAN / AISPEED_STEP + 0.5f);
	if (span < 1) span = 1;
	if (span > n / 3) span = n / 3;
	if (span < 1) span = 1;
	for (int i = 0; i < n; i++) {
		const float d = (float)i * AISPEED_STEP;
		float k;
		if (line) {
			float pa[2], pb[2], pc[2];
			ailine_pose(line, ailine_along(line, d, -AISPEED_CURV_SPAN), 0.0f, pa, NULL);
			ailine_pose(line, d, 0.0f, pb, NULL);
			ailine_pose(line, ailine_along(line, d, AISPEED_CURV_SPAN), 0.0f, pc, NULL);
			k = curvature(pa, pb, pc);
		} else {
			int a = i - span, c = i + span;
			if (a < 0) a = p->loops ? a + n : 0;
			if (c >= n) c = p->loops ? c - n : n - 1;
			k = (a == i || c == i) ? 0.0f
				: curvature(&pos[2 * a], &pos[2 * i], &pos[2 * c]);
		}
		v[i] = speedLimitForCurvature(fr, k, topSpeed);
		const float e = i + 1 < n ? d + AISPEED_STEP : len;
		run[i] = line ? ailine_arc(line, e) - ailine_arc(line, d) : AISPEED_STEP;
		if (run[i] < 0.0f) run[i] = 0.0f;
	}

	// ScrubSpeeds (0x80036CD8): walk backwards so a corner slows the straight
	// before it, then forwards so the car is not asked to accelerate harder
	// than it can. The braking figure is retail's own, by class (GetCarBraking);
	// the acceleration is the car's own RidePhysicsInfo table, read at the
	// speed the car is leaving the last sample with (carsim_accel_table).
	float brake = kClassBraking[carClass];
	p->fr = fr;
	p->brake = brake;
	p->top = topSpeed;
	int passes = p->loops ? 2 : 1;   // a loop needs a second lap to settle
	for (int pass = 0; pass < passes; pass++) {
		for (int i = n - 1; i >= 0; i--) {
			int j = i + 1;
			if (j >= n) { if (!p->loops) continue; j = 0; }
			float cap = sqrtf(v[j] * v[j] + 2.0f * brake * run[i]);
			if (cap < v[i]) v[i] = cap;
		}
		for (int i = 0; i < n; i++) {
			int j = i - 1;
			if (j < 0) { if (!p->loops) continue; j = n - 1; }
			const float v2 = v[j] * v[j] + 2.0f * accelAt(accel, accelTop, v[j]) * run[j];
			const float cap = v2 > 5e-11f ? sqrtf(v2) : 0.0f;
			if (cap < v[i]) v[i] = cap;
		}
	}

	for (int i = 0; i < n; i++) {
		float x = v[i];
		if (x < 0.0f) x = 0.0f;
		if (x < p->min) p->min = x;
		p->v[i] = x;
	}
	free(pos);
	free(v);
	free(run);

	if (log) {
		char b[160];
		snprintf(b, sizeof(b), "ai: speed profile %d samples over %.0f m, "
			"class %d, %.2f g, slowest %.1f m/s, top %.1f m/s, %d on the racing line",
			n, len, carClass, fr, p->min, topSpeed, onLine);
		log(b);
	}
	return p;
}

void aispeed_free(AiSpeedProfile* p) { free(p); }

float aispeed_at(const AiSpeedProfile* p, float dist) {
	if (!p || p->n <= 0) return 0.0f;
	if (p->loops) {
		dist = fmodf(dist, p->len);
		if (dist < 0.0f) dist += p->len;
	} else {
		if (dist < 0.0f) dist = 0.0f;
		if (dist > p->len) dist = p->len;
	}
	float t = dist / p->step;
	int i = (int)t;
	if (i < 0) i = 0;
	if (i >= p->n - 1) return p->v[p->n - 1];
	float f = t - (float)i;
	return p->v[i] + f * (p->v[i + 1] - p->v[i]);
}

float aispeed_min(const AiSpeedProfile* p) { return p ? p->min : 0.0f; }

float aispeed_corner(const AiSpeedProfile* p, float curv) {
	return p ? speedLimitForCurvature(p->fr, curv, p->top) : 0.0f;
}

float aispeed_braking(const AiSpeedProfile* p) { return p ? p->brake : 0.0f; }
int   aispeed_count(const AiSpeedProfile* p) { return p ? p->n : 0; }

// ---------------------------------------------------------------------------
// DriftDriver (#268 E step 5): Drive 0x80021E78, SetSteeringClamp 0x80021D60.

#define DRIFT_TWO_PI    6.28318531f
#define DRIFT_U16_RAD   (DRIFT_TWO_PI / 65536.0f)   // 0x803177A0
#define DRIFT_MAX_SPEED 31.2919f                    // 0x80317820, 70 mph
#define DRIFT_STEER_MAX (45.0f * DEG2RAD)           // SetSteeringClamp's
#define DRIFT_STEER_RATE (360.0f * DEG2RAD)         // a second

// x - floor(x / 2pi + 0.5) * 2pi: Drive's own wrap into [-pi, pi).
static float driftWrap(float x) {
	return x - floorf(x / DRIFT_TWO_PI + 0.5f) * DRIFT_TWO_PI;
}

// bATan's u16, back in radians: 0..2pi.
static float driftATan(float x, float y) {
	const float a = atan2f(y, x);
	const u16 u = (u16)(s32)(a * (65536.0f / DRIFT_TWO_PI));
	return (float)u * DRIFT_U16_RAD;
}

void aidrift_init(AiDriftDriver* d) {
	memset(d, 0, sizeof(*d));
	d->gear = -1;
	d->mode = 1;
}

void aidrift_set_desired(AiDriftDriver* d, float speed) {
	d->desired = speed < DRIFT_MAX_SPEED ? speed : DRIFT_MAX_SPEED;
}

void aidrift_ahead(const CarSim* s, float lateral, float* d1, float* d2) {
	const float speed = s->speed;
	const float a = clampf(speed * 0.04f + fabsf(lateral) * 0.75f + 15.0f, 15.0f, 30.0f);
	*d1 = a;
	*d2 = clampf(speed * 0.08f + a, 8.0f, 12.0f);
}

// SetSteeringClamp: the wheel turned toward `target` (rad) at no more than
// 360 degrees a second, held to 45 degrees either way.
static void driftSteer(AiDriftDriver* d, float target, float dt) {
	const float cur = driftWrap((float)d->steerU16 * DRIFT_U16_RAD);
	const float lim = dt * DRIFT_STEER_RATE;
	const float nv = clampf(cur + clampf(target - cur, -lim, lim), -DRIFT_STEER_MAX,
		DRIFT_STEER_MAX);
	d->steerU16 = (u16)(s16)(int)(nv * 10430.378f);
}

void aidrift_drive(AiDriftDriver* d, const CarSim* s, float dt, const float* p1,
		const float* p2, bool physicsMover, CarSimInput* out) {
	memset(out, 0, sizeof(*out));
	if (dt < 0.0001f) dt = 0.0001f;
	out->racing = d->mode == 0;
	out->steerIsAngle = true;
	out->aiDriver = true;

	const float speed = s->speed;   // Car +0x440
	float lineH = 0.0f;
	if (!p1) {
		// SteerStraightVector3D(car, 10): 10 m down the car's heading.
		const float h = driftATan(s->rot[0][0], s->rot[0][1]);
		d->steerTo[0] = s->pos[0] + cosf(h) * 10.0f;
		d->steerTo[1] = s->pos[1] + sinf(h) * 10.0f;
	} else {
		// The second point is the nearer (12 m against at least 15), so this
		// heading runs back down the line; its term is 0 at any forward
		// speed, as retail's.
		if (p2)
			lineH = driftATan(p2[0] - p1[0], p2[1] - p1[1]);
		d->steerTo[0] = p1[0];
		d->steerTo[1] = p1[1];
	}
	const float aim = driftWrap(driftATan(d->steerTo[0] - s->pos[0], d->steerTo[1] - s->pos[1]));
	const float heading = driftATan(s->rot[0][0], s->rot[0][1]);   // Car +0x41C
	float thr = 120.0f * DEG2RAD;
	if (d->gear == 0)
		thr -= 30.0f * DEG2RAD;
	const bool reverse = d->desired < 0.0f || thr < fabsf(driftWrap(heading - aim));

	if (d->mode == 0) {
		if (reverse || speed < 4.47027f) {
			// Slow, or backing round: the pedals on the speed error, the
			// wheel straight at the point (turned the other way in reverse).
			const float want = fabsf(d->desired);
			if (want >= 2.0f) {
				const float t = clampf(want - speed, -1.0f, 1.0f);
				if (t >= 0.0f) {
					d->brake = 0.0f;
					d->throttle = t;
				} else {
					d->brake = -t;
					d->throttle = 0.0f;
				}
			} else {
				d->brake = 1.0f;
				d->throttle = 0.0f;
			}
			d->ebrake = 0.0f;
			d->gear = reverse ? 0 : -1;
			float st = driftWrap(aim - heading);
			if (reverse)
				st = -st;
			driftSteer(d, st, dt);
		} else {
			// The drift: a slip angle to hold, by how far the point is off
			// the direction of travel, and the wheel and the e-brake on the
			// slip's error from it.
			const float velH = driftATan(s->vel[0], s->vel[1]);
			const float yawA = carsim_yaw_always(s);
			const float dYaw = driftWrap(yawA - d->lastYaw);
			d->lastYaw = yawA;
			const float a = clampf(driftWrap(aim - velH) * clampf(speed * 0.05f, 0.0f, 2.0f),
				-45.0f * DEG2RAD, 45.0f * DEG2RAD);
			const float b = clampf(driftWrap(lineH - velH) * clampf(speed * -0.1f, 0.0f, 1.0f),
				-35.0f * DEG2RAD, 35.0f * DEG2RAD);
			const float lim = clampf(speed * 0.03f + 45.0f * DEG2RAD, 45.0f * DEG2RAD,
				70.0f * DEG2RAD);
			const float target = clampf(a + b, -lim, lim);
			d->targetYaw = target;
			const float e = driftWrap(yawA - target);
			const float sc = e * 1.5f + (dYaw / dt) * -0.02f;
			driftSteer(d, -sc, dt);
			const float eS = target < 0.0f ? -e : e;
			const float sc2 = e > 0.0f ? -sc : sc;
			const float k1 = clampf((-20.0f * DEG2RAD - eS) / (20.0f * DEG2RAD), 0.0f, 1.0f);
			const float k2 = clampf((15.0f * DEG2RAD - sc2) / (10.0f * DEG2RAD), 0.0f, 1.0f);
			const float k3 = clampf((speed - 8.94055f) / 8.94055f, 0.0f, 1.0f);
			d->ebrake = k1 * k2 * k3;
			if (d->desired >= 2.0f) {
				const float r = clampf(speed / 8.94055f, 0.0f, 1.0f);
				float t = clampf((d->desired - speed + 2.23514f) / 8.94055f, 0.0f, 1.0f);
				t = clampf(t - r * (eS - 20.0f * DEG2RAD) * 0.9f, 0.0f, 1.0f);
				d->throttle = t;
				d->brake = 0.0f;
			} else {
				d->brake = 1.0f;
				d->throttle = 0.0f;
			}
			d->gear = -1;
		}
	} else {
		// On the start line: both pedals down in neutral, the wheel straight.
		d->brake = 1.0f;
		d->throttle = 1.0f;
		driftSteer(d, 0.0f, 1.0f);
		d->gear = 1;
	}

	out->throttle = d->throttle;
	out->brake = d->brake;
	out->gear = d->gear;
	out->steerAngleU16 = d->steerU16;
	// In a drift, on the PhysicsMover, the e-brake past 0.8 is the handbrake.
	out->handbrake = physicsMover && d->ebrake > 0.8f;
}
