#include "custtune.h"
#include "ridephys.h"
#include <math.h>
#include <string.h>

// See custtune.h. Offsets are the decompile's: P is the CarPhysicsInfo being
// tuned (the spec's +0x0C), C the car's CompleatCarPhysicsInfo, whose first
// RIDEPHYS_CPI_SIZE bytes are the stock CarPhysicsInfo. Where retail's code
// is a fused multiply-add (fmadds, read off the disassembly), this is fmaf.

// CustomTuningParams, one 0x18-byte row of GLOBALB.BUN's 0x34A1A chunk.
typedef struct {
	int part;           // +0x00 ePhysicsPerformanceParts that must be fitted
	signed char level;  // +0x04 the highest fitted row wins
	signed char type;   // +0x05 ePhysicsTuningType
	unsigned char mode; // +0x06 how +0x08/+0x0C shape the range
	float p[4];         // +0x08 low, +0x0C high, +0x10 / +0x14 display low / high
} TuneParams;

// tools/custtune_params.py's output, the chunk's rows in its order (both of
// GLOBALB's copies are the same).
static const TuneParams kParams[] = {
	{  8, 1,  9, 1, { -0.050000004f, 0.10000001f, -99.99f, -99.99f } },
	{  9, 2,  9, 1, { -0.10000001f, 0.20000002f, -99.99f, -99.99f } },
	{ 11, 3,  9, 0, { -0.20000002f, 0.40000004f, -99.99f, -99.99f } },
	{ 11, 3, 10, 1, { -0.8000001f, 1.0f, -99.99f, -99.99f } },
	{ 11, 3, 11, 1, { -0.6000001f, 0.8000001f, -99.99f, -99.99f } },
	{ 11, 3, 12, 1, { -0.40000004f, 0.6000001f, -99.99f, -99.99f } },
	{ 11, 3, 13, 1, { -0.30000004f, 0.40000004f, -99.99f, -99.99f } },
	{ 11, 3, 14, 1, { -0.10000001f, 0.25000003f, -99.99f, -99.99f } },
	{ 11, 3, 15, 1, { -0.0f, 0.10000001f, -99.99f, -99.99f } },
	{ 12, 1, 16, 4, { 1.0f, 3.0f, -99.99f, -99.99f } },
	{ 12, 1, 17, 4, { 0.40000004f, 0.6000001f, -99.99f, -99.99f } },
	{ 13, 2, 16, 4, { 1.0f, 7.0f, -99.99f, -99.99f } },
	{ 13, 2, 17, 4, { 0.25000003f, 0.75000006f, -99.99f, -99.99f } },
	{ 14, 3, 16, 4, { 1.0f, 11.0f, -99.99f, -99.99f } },
	{ 14, 3, 17, 4, { 0.10000001f, 0.9000001f, -99.99f, -99.99f } },
	{ 26, 1, 24, 1, { -0.030000003f, 0.0150000015f, -99.99f, -99.99f } },
	{ 27, 2, 24, 1, { -0.035000004f, 0.020000003f, -99.99f, -99.99f } },
	{ 28, 3, 24, 1, { -0.040000007f, 0.025000002f, -99.99f, -99.99f } },
	{ 29, 1,  0, 2, { -0.19999999f, 0.099999994f, 0.0f, 10.0f } },
	{ 29, 1,  1, 2, { -0.19999999f, 0.099999994f, 0.0f, 10.0f } },
	{ 29, 1,  4, 2, { -0.19999999f, 0.099999994f, 0.0f, 10.0f } },
	{ 29, 1,  5, 2, { -0.19999999f, 0.099999994f, 0.0f, 10.0f } },
	{ 29, 1,  3, 2, { -0.099999994f, 0.099999994f, -99.99f, -99.99f } },
	{ 29, 1,  7, 2, { -0.099999994f, 0.099999994f, -99.99f, -99.99f } },
	{ 30, 1,  2, 2, { -0.14999999f, 0.099999994f, 0.0f, 10.0f } },
	{ 30, 1,  6, 2, { -0.14999999f, 0.099999994f, 0.0f, 10.0f } },
	{ 31, 2,  0, 2, { -0.29999998f, 0.099999994f, 0.0f, 10.0f } },
	{ 31, 2,  1, 2, { -0.29999998f, 0.099999994f, 0.0f, 10.0f } },
	{ 31, 2,  4, 2, { -0.29999998f, 0.099999994f, 0.0f, 10.0f } },
	{ 31, 2,  5, 2, { -0.29999998f, 0.099999994f, 0.0f, 10.0f } },
	{ 31, 2,  3, 2, { -0.099999994f, 0.099999994f, -99.99f, -99.99f } },
	{ 31, 2,  7, 2, { -0.099999994f, 0.099999994f, -99.99f, -99.99f } },
	{ 32, 2,  2, 2, { -0.14999999f, 0.099999994f, 0.0f, 10.0f } },
	{ 32, 2,  6, 2, { -0.14999999f, 0.099999994f, 0.0f, 10.0f } },
	{ 36, 2,  8, 1, { -0.050000004f, 0.050000004f, -99.99f, -99.99f } },
	{ 35, 3,  0, 2, { -0.39999998f, 0.19999999f, 0.0f, 10.0f } },
	{ 35, 3,  1, 2, { -0.39999998f, 0.099999994f, 0.0f, 10.0f } },
	{ 35, 3,  4, 2, { -0.39999998f, 0.19999999f, 0.0f, 10.0f } },
	{ 35, 3,  5, 2, { -0.39999998f, 0.099999994f, 0.0f, 10.0f } },
	{ 35, 3,  3, 2, { -0.099999994f, 0.099999994f, -99.99f, -99.99f } },
	{ 35, 3,  7, 2, { -0.099999994f, 0.099999994f, -99.99f, -99.99f } },
	{ 35, 3,  8, 1, { -0.10000001f, 0.10000001f, -99.99f, -99.99f } },
	{ 37, 3,  2, 2, { -0.14999999f, 0.19999999f, 0.0f, 10.0f } },
	{ 37, 3,  6, 2, { -0.14999999f, 0.19999999f, 0.0f, 10.0f } },
	{ 40, 1, 18, 4, { 0.40000004f, 0.6000001f, -1.0f, 1.0f } },
	{ 41, 2, 18, 4, { 0.32500005f, 0.6750001f, -1.0f, 1.0f } },
	{ 44, 3, 18, 4, { 0.25000003f, 0.75000006f, -1.0f, 1.0f } },
	{ 48, 1, 23, 1, { -0.060000006f, 0.030000003f, -99.99f, -99.99f } },
	{ 46, 2, 23, 1, { -0.07000001f, 0.040000007f, -99.99f, -99.99f } },
	{ 51, 3, 23, 1, { -0.08000001f, 0.050000004f, -99.99f, -99.99f } },
	{ 53, 1, 19, 2, { 0.0f, 0.0f, 0.0f, 10.0f } },
	{ 53, 1, 20, 2, { 0.0f, 0.0f, 0.0f, 10.0f } },
	{ 54, 2, 19, 2, { 0.0f, 0.0f, 0.0f, 10.0f } },
	{ 54, 2, 20, 2, { 0.0f, 0.0f, 0.0f, 10.0f } },
	{ 55, 3, 19, 2, { 0.0f, 0.0f, 0.0f, 10.0f } },
	{ 55, 3, 20, 2, { 0.0f, 0.0f, 0.0f, 10.0f } },
	{ 56, 1, 21, 2, { -0.14999999f, 0.0f, 0.0f, 10.0f } },
	{ 65, 1, 22, 2, { -0.14999999f, 0.0f, 0.0f, 10.0f } },
	{ 57, 2, 21, 2, { -0.14999999f, 0.0f, 0.0f, 10.0f } },
	{ 66, 2, 22, 2, { -0.14999999f, 0.0f, 0.0f, 10.0f } },
	{ 58, 3, 21, 2, { -0.14999999f, 0.099999994f, 0.0f, 10.0f } },
	{ 67, 3, 22, 2, { -0.14999999f, 0.099999994f, 0.0f, 10.0f } },
};
#define NUM_PARAMS ((int)(sizeof(kParams) / sizeof(kParams[0])))

// 0x80332D70: a display end the row does not give.
#define NO_DISPLAY (-99.99f)

// ElasticTorqueAdjustments (0x80360168): how much of a curve point's change
// SetTorqueValue spreads onto its neighbours, nearest first. The loop reads one
// word past the eight, 0x80360188, whose bits are 3 (a denormal).
static const float kElastic[9] = { 0.25f, 0.25f, -0.25f, -0.25f, -0.25f, -0.25f, -0.25f,
	-0.25f, 0x1.8p-148f };

static float rd(const u8* b, int off) {
	float v;
	memcpy(&v, b + off, sizeof(v));
	return v;
}

static void wr(u8* b, int off, float v) {
	memcpy(b + off, &v, sizeof(v));
}

static float minf(float a, float b) { return a - b < 0.0f ? a : b; }
static float maxf(float a, float b) { return a - b < 0.0f ? b : a; }
static float clampf(float v, float lo, float hi) { return minf(maxf(v, lo), hi); }

static bool is_gear(int t) { return t >= CUSTTUNE_GEAR1 && t <= CUSTTUNE_GEAR6; }

// GetInstalledParams (0x801C9E20): the row for `t` whose part is fitted, the
// highest level winning (the first of a tie).
static const TuneParams* installed_params(const CustTune* s, int t) {
	const TuneParams* best = NULL;
	if (!s->installed)
		return NULL;
	for (int i = 0; i < NUM_PARAMS; i++) {
		const TuneParams* r = &kParams[i];
		if (r->type != t || r->part < 0 || r->part >= PERFDB_NUM_PARTS || !s->installed[r->part])
			continue;
		if (!best || best->level < r->level)
			best = r;
	}
	return best;
}

// GetRealValuePtr (0x801CBFD0) as an offset into a CarPhysicsInfo; -1 for a
// type it has none for.
static int value_off(int t, int idx) {
	switch (t) {
	case 0:  return 0x174;
	case 1:  return 0x178 + idx * 4;
	case 2:  return 0x180;
	case 3:  return 0x18 + idx * 0x30;
	case 4:  return 0x194;
	case 5:  return 0x198 + idx * 4;
	case 6:  return 0x1A0;
	case 7:  return 0x78 + idx * 0x30;
	case 8:  return 0x270;
	case 9:  return 0x1B8 + idx * 4;
	case 16: return 0x2A0;
	case 17: return 0x2AC;
	case 18: return 0x27C;
	case 19: return idx ? 0xE0 : 0xDC;
	case 20: return idx ? 0x100 : 0xFC;
	case 21: return 0x264;
	case 22: return 0x268;
	case 23: return 0x200 + idx * 4;
	case 24: return 0x224 + idx * 4;
	default:
		if (is_gear(t))
			return 0x1B0 + t * 4;
		return -1;
	}
}

// RemoveUniqueUpgrades (0x801CCB20) / ApplyUniqueUpgrades (0x801CCB80): the
// junkman parts come off around anything that reads or writes the car's own
// values, nested by a count, and go back on when it returns to 0.
static void remove_unique(CustTune* s) {
	if (s->uniqueDepth == 0 && s->junkman)
		ridephys_remove_junkman(s->compleat, s->cpi, s->junkman);
	s->uniqueDepth++;
}

static void apply_unique(CustTune* s) {
	if (--s->uniqueDepth == 0 && s->junkman)
		ridephys_apply_junkman(s->compleat, s->cpi, s->junkman);
}

// DefaultTuningsStructure::GetValue (0x801C9F14), SetValue (0x801C9F60) and
// Reset (0x801C9FA4).
static float def_get(const CustTune* s, int t, int idx) {
	if (t < 0x17)
		return s->defaults[t];
	if (t == CUSTTUNE_TORQUE)
		return s->defaults[0x17 + idx];
	if (t == CUSTTUNE_BOOST)
		return s->defaults[0x20 + idx];
	return 0.0f;
}

static void def_set(CustTune* s, float v, int t, int idx) {
	if (t < 0x17)
		s->defaults[t] = v;
	else if (t == CUSTTUNE_TORQUE)
		s->defaults[0x17 + idx] = v;
	else if (t == CUSTTUNE_BOOST)
		s->defaults[0x20 + idx] = v;
}

// ---------------------------------------------------------------------------
// CustomPhysicsTunings
// ---------------------------------------------------------------------------

void custtune_tunings_reset(u8* tn, int t) {
	if (t == -1) {
		memset(tn, CUSTTUNE_DEFAULT_BYTE, CUSTTUNE_SIZE);
	} else if (t < 0x17) {
		custtune_tunings_set(tn, 0.0f, t, 0);
	} else {
		for (int i = 0; i < CUSTTUNE_CURVE_POINTS; i++)
			custtune_tunings_set(tn, 0.0f, t, i);
	}
}

float custtune_tunings_get(const u8* tn, int t, int idx) {
	float f;
	if (t >= 0 && t < 0x17)
		f = (float)tn[t];
	else if (t == CUSTTUNE_TORQUE)
		f = (float)tn[0x17 + idx];
	else if (t == CUSTTUNE_BOOST)
		f = (float)tn[0x20 + idx];
	else
		f = 0.0f;
	return clampf((f - 100.0f) / 100.0f, -1.0f, 1.0f);
}

void custtune_tunings_set(u8* tn, float v, int t, int idx) {
	int i = (int)fmaf(v, 100.0f, 100.5f);
	if (i < 0)
		i = 0;
	if (i > 200)
		i = 200;
	if (t >= 0 && t < 0x17)
		tn[t] = (u8)i;
	else if (t == CUSTTUNE_TORQUE)
		tn[0x17 + idx] = (u8)i;
	else if (t == CUSTTUNE_BOOST)
		tn[0x20 + idx] = (u8)i;
}

bool custtune_tunings_equal(const u8* a, const u8* b) {
	return memcmp(a, b, CUSTTUNE_SIZE) == 0;
}

// ---------------------------------------------------------------------------
// The value helpers
// ---------------------------------------------------------------------------

float custtune_trend(int t) {
	return (t == 3 || t == 7 || t == 9) ? -1.0f : 1.0f;
}

// MapGearRatioToTopSpeed (0x801CC9FC): a gear ratio and the speed it tops out
// at on the redline, either way round (`toSpeed` false takes a speed and gives
// the ratio). The radius is the front wheel's, the rear's, or the drive
// split's mix of them.
static float map_gear(const CustTune* s, float x, bool toSpeed) {
	const u8* P = s->cpi;
	const float split = rd(P, 0x1C0);
	const float w = rd(P, 0x1F4) * 0.10471967f;
	float radius = 0.0f;
	if (0.2f <= split) {
		if (split <= 0.8f)
			radius = fmaf(split, rd(P, 0x80), (1.0f - split) * rd(P, 0x50));
		else
			radius = rd(P, 0x80);
	} else {
		radius = rd(P, 0x50);
	}
	if (x == 0.0f)
		return 0.0f;
	if (!toSpeed)
		return (w / rd(P, 0x1B8)) * (radius / x);
	return radius * (w / (rd(P, 0x1B8) * x));
}

// ApplySpecialRemapping (0x801CC85C).
static float special_remap(const CustTune* s, float v, int t, bool toDisplay) {
	return is_gear(t) ? map_gear(s, v, toDisplay) : v;
}

// ApplyUnitRemapping (0x801CC88C): ride heights in cm / inches, top speeds in
// km/h / mph, the curves in N m / ft lb.
static float unit_remap(const CustTune* s, float v, int t, bool toDisplay) {
	const bool metric = s->units == 1;
	if (t == CUSTTUNE_TORQUE || t == CUSTTUNE_BOOST) {
		if (!metric)
			return toDisplay ? (v * 1000.0f) * 0.7375621f
				: (float)(((double)v * 1.35581795) / 1000.0);
		return toDisplay ? v * 1000.0f : v / 1000.0f;
	}
	if (is_gear(t)) {
		if (!metric)
			return toDisplay ? v * 2.237f : v * 0.44702727f;
		return toDisplay ? v * 3.6f : v * 0.2777778f;
	}
	if (t != 3 && t != 7)
		return v;
	if (!metric)
		return toDisplay ? v * 39.37f : v / 39.37f;
	return toDisplay ? v * 100.0f : v / 100.0f;
}

// AdjustForTuneableRange (0x801CBCAC): the fitted part's row widens or moves
// a range -- by a share of its width (modes 2, 3), by a fixed amount (0, 1)
// or out to fixed ends (4).
static void adjust_tuneable(const CustTune* s, int t, float* lo, float* hi) {
	const TuneParams* p = installed_params(s, t);
	if (!p)
		return;
	const float l = *lo, h = *hi;
	if (p->mode == 2 || p->mode == 3) {
		*lo = fmaf(p->p[0], h - l, l);
		*hi = fmaf(p->p[1], h - l, *hi);
	} else if (p->mode < 2) {
		*lo = l + p->p[0];
		*hi = *hi + p->p[1];
	} else if (p->mode == 4) {
		*lo = minf(l, p->p[0]);
		*hi = maxf(*hi, p->p[1]);
	}
}

// GetGripMinMax (0x801CB6E0) and GetDampingMinMax (0x801CB844): each of the
// pair's two fields from its stock value to its upgrade target, adjusted;
// one of them, or with `avg` their mean.
static void pair_minmax(const CustTune* s, int t, int idx, bool avg, float* lo, float* hi,
		int firstType, const int* stockOff, const int* targetOff) {
	const u8* C = s->compleat;
	float l[4], h[4];
	for (int k = 0; k < 4; k++) {
		l[k] = rd(C, stockOff[k]);
		h[k] = rd(C, targetOff[k]);
		adjust_tuneable(s, k < 2 ? firstType : firstType + (firstType == 19 ? 1 : 4), &l[k], &h[k]);
	}
	const int u = t != firstType;
	if (!avg) {
		*lo = l[u * 2 + idx];
		*hi = h[u * 2 + idx];
	} else {
		*lo = (l[u * 2] + l[u * 2 + 1]) * 0.5f;
		*hi = (h[u * 2] + h[u * 2 + 1]) * 0.5f;
	}
}

static void grip_minmax(const CustTune* s, int t, int idx, bool avg, float* lo, float* hi) {
	static const int stock[4] = { 0xDC, 0xE0, 0xFC, 0x100 };
	static const int target[4] = { 0x54C, 0x550, 0x56C, 0x570 };
	pair_minmax(s, t, idx, avg, lo, hi, CUSTTUNE_FRONT_GRIP, stock, target);
}

// GetDampingMinMax adjusts in the order front k, rear k, front k+1, rear k+1;
// AdjustForTuneableRange is pure, so the order does not matter.
static void damping_minmax(const CustTune* s, int t, int idx, bool avg, float* lo, float* hi) {
	static const int stock[4] = { 0x178, 0x17C, 0x198, 0x19C };
	static const int target[4] = { 0x5C8, 0x5CC, 0x5E8, 0x5EC };
	pair_minmax(s, t, idx, avg, lo, hi, CUSTTUNE_FRONT_DAMPING, stock, target);
}

// GetRealMinMax (0x801CB0E4): the range a type's real value can take on this
// car, from its stock value(s) to its upgrade targets, widened by the fitted
// part's row.
static void real_minmax(CustTune* s, int t, float* lo, float* hi, int idx, bool avg) {
	remove_unique(s);
	const u8* C = s->compleat;
	float l = 100000.0f, h = -100000.0f;
	bool adjust = true;
#define MM(x) do { const float v_ = (x); l = minf(l, v_); h = maxf(h, v_); } while (0)
	switch (t) {
	case 0: case 4:
		MM(rd(C, 0x174)); MM(rd(C, 0x194)); MM(rd(C, 0x5C4)); MM(rd(C, 0x5E4));
		break;
	case 2: case 6:
		MM(rd(C, 0x180)); MM(rd(C, 0x1A0)); MM(rd(C, 0x5D0)); MM(rd(C, 0x5F0));
		break;
	case 8:
		MM(rd(C, 0x270)); MM(rd(C, 0x5BC));
		break;
	case 9:
		MM(rd(C, 0x1B8)); MM(rd(C, 0x3D8));
		break;
	case 1: case 5:
		damping_minmax(s, t, idx, avg, &l, &h);
		adjust = false;
		break;
	case 3: case 7:
		MM(-rd(C, 0x604)); MM(-rd(C, 0x608)); MM(0.0f);
		break;
	case 16:
		l = 1.0f;
		h = 11.0f;
		adjust = false;
		break;
	case 17:
		l = 0.1f;
		h = 0.9f;
		break;
	case 18:
		l = 0.0f;
		h = 1.0f;
		break;
	case 19: case 20:
		grip_minmax(s, t, idx, avg, &l, &h);
		adjust = false;
		break;
	case 21: case 22:
		MM(rd(C, 0x264)); MM((rd(C, 0x264) + rd(C, 0x5A0)) + rd(C, 0x590));
		MM(rd(C, 0x268)); MM((rd(C, 0x268) + rd(C, 0x5A4)) + rd(C, 0x594));
		break;
	case 23: case 24:
		for (int k = 0; k < CUSTTUNE_CURVE_POINTS; k++) {
			float a, b;
			if (t == CUSTTUNE_TORQUE) {
				a = rd(C, 0x200 + 4 * k);
				b = (a + rd(C, 0x420 + 4 * k)) + rd(C, 0x4E0 + 4 * k);
			} else {
				a = rd(C, 0x224 + 4 * k);
				b = rd(C, 0x510 + 4 * k);
			}
			const float la = minf(l, a);
			h = maxf(h, a);
			h = maxf(h, b);
			l = minf(la, b);
		}
		adjust_tuneable(s, t, &l, &h);
		l = maxf(0.0f, l);
		h = maxf(0.0f, h);
		adjust = false;
		break;
	default:
		if (is_gear(t)) {
			// The ratio that tops out at 250 mph, and at 2.5 mph.
			l = map_gear(s, 111.75681f, false);
			h = map_gear(s, 1.1175681f, false);
			adjust = false;
		}
		break;
	}
#undef MM
	if (adjust)
		adjust_tuneable(s, t, &l, &h);
	*lo = l;
	*hi = h;
	apply_unique(s);
}

// GetRealValue (0x801CB9A0).
static float real_get(CustTune* s, int t, int idx) {
	remove_unique(s);
	const u8* P = s->cpi;
	float r;
	if (t == CUSTTUNE_TORQUE) {
		r = rd(P, value_off(23, idx)) - rd(P, value_off(24, idx));
	} else if (is_gear(t)) {
		r = rd(P, value_off(t, 0));     // CalcGearRatio (0x801CC4D4)
	} else if (t == 1 || t == 5 || t == 19 || t == 20) {
		float a = 0.0f;
		for (int i = 0; i < 2; i++)
			a += rd(P, value_off(t, i));
		r = a * 0.5f;
	} else if (t == 3 || t == 7) {
		r = -(rd(P, value_off(t, 0)) - rd(s->compleat, value_off(t, 0)));
	} else {
		const int o = value_off(t, idx);
		r = o >= 0 ? rd(P, o) : 0.0f;
	}
	apply_unique(s);
	return r;
}

// GetMaxRelatedDefaultValue (0x801CAF38): a front/rear pair is limited by the
// larger (or, trend -1, the smaller) of the two untuned values.
static float max_related_default(const CustTune* s, int t, int idx) {
	int e[2] = { t, -1 };
	if (t == 0 || t == 4) {
		e[0] = 0; e[1] = 4;
	} else if (t == 2 || t == 6) {
		e[0] = 2; e[1] = 6;
	} else if (t == 3 || t == 7) {
		e[0] = 3; e[1] = 7;
	} else if (t == 21 || t == 22) {
		e[0] = 21; e[1] = 22;
	}
	const bool up = 0.0f < custtune_trend(t);
	float r = up ? -10000000.0f : 10000000.0f;
	for (int k = 0; k < 2; k++) {
		if (e[k] == -1)
			continue;
		const float d = def_get(s, e[k], idx);
		r = up ? maxf(r, d) : minf(r, d);
	}
	return r;
}

// CalculateMaxHP (0x801C9C18): the power curve's peak over [lo, hi] every
// 50 rpm, of the torque curve `curve` spread over the same range.
static float max_hp(float lo, float hi, const float* curve, int n) {
	float best = 0.0f;
	for (float x = lo; x < hi; x = x + 50.0f) {
		const float v = ((ridephys_table_get(curve, n, lo, hi, x) * 1000.0f) * x) / 9550.0f;
		if (best < v)
			best = v;
	}
	return best;
}

// AdjustForTorqueConstraints (0x801CBB40): a curve point may not rise past the
// highest untuned point, nor make more power at its rpm than the untuned
// curve's peak.
static void torque_constraints(const CustTune* s, int t, float* lo, float* hi, int idx) {
	float d[CUSTTUNE_CURVE_POINTS];
	float top = 0.0f;
	for (int k = 0; k < CUSTTUNE_CURVE_POINTS; k++) {
		d[k] = def_get(s, t, k);
		if (top < d[k])
			top = d[k];
	}
	*hi = minf(top, *hi);
	const u8* P = s->cpi;
	const float idle = rd(P, 0x1F0), limiter = rd(P, 0x1F8);
	const float peak = max_hp(idle, limiter, d, CUSTTUNE_CURVE_POINTS);
	const float rpm = fmaf((float)idx * (limiter - idle), 0.125f, idle);
	const float hp = (*hi * 1000.0f * rpm) / 9550.0f;
	if (peak < hp)
		*hi = maxf(0.0f, *hi + (peak - hp) / ((rpm * 1000.0f) / 9550.0f));
	*lo = maxf(0.0f, *lo);
	*hi = maxf(0.0f, *hi);
}

static float remap(CustTune* s, float v, int t, bool toDisplay);

// GetTuneableRangeMinMax (0x801CAC58).
static bool tuneable_range(CustTune* s, int t, float* mn, float* mx, int idx, bool doRemap) {
	const TuneParams* p = installed_params(s, t);
	if (!p)
		return false;
	float lo = 0.0f, hi = 0.0f;
	real_minmax(s, t, &lo, &hi, idx, true);
	const float d = max_related_default(s, t, idx);
	if (t == CUSTTUNE_TORQUE || t == CUSTTUNE_BOOST)
		torque_constraints(s, t, &lo, &hi, idx);
	switch (p->mode) {
	case 2:
		if (custtune_trend(t) <= 0.0f) {
			const float f = p->p[1] * (hi - lo);
			*mn = maxf(lo, d - f);
			*mx = hi;
		} else {
			*mn = lo;
			*mx = minf(hi, fmaf(p->p[1], hi - lo, d));
		}
		break;
	case 3:
		*mn = maxf(lo, fmaf(p->p[0], hi - lo, d));
		*mx = minf(hi, fmaf(p->p[1], hi - lo, d));
		break;
	case 0:
		if (0.0f < custtune_trend(t)) {
			*mn = lo;
			*mx = minf(hi, d + p->p[1]);
		} else {
			*mn = maxf(lo, d - p->p[1]);
			*mx = hi;
		}
		break;
	case 1:
		*mn = maxf(lo, d + p->p[0]);
		*mx = minf(hi, d + p->p[1]);
		break;
	case 4:
		*mn = p->p[0];
		*mx = p->p[1];
		break;
	default:
		break;
	}
	if (doRemap) {
		const float a = remap(s, *mn, t, true);
		const float b = remap(s, *mx, t, true);
		*mn = a;
		*mx = b;
		if (b < a) {
			*mn = b;
			*mx = a;
		}
	}
	if (*mn == *mx)
		*mx += 0.01f;
	return true;
}

// GetTotalRangeMinMax (0x801CAAC4): GetRealMinMax, or with `doRemap` the
// row's display ends where it gives them and the real ends converted where
// it does not.
static bool total_range(CustTune* s, int t, float* lo, float* hi, int idx, bool doRemap) {
	real_minmax(s, t, lo, hi, idx, true);
	const TuneParams* p = installed_params(s, t);
	if (!p)
		return false;
	if (doRemap) {
		if (p->p[2] == NO_DISPLAY)
			*lo = unit_remap(s, special_remap(s, *lo, t, true), t, true);
		else
			*lo = p->p[2];
		if (p->p[3] == NO_DISPLAY)
			*hi = unit_remap(s, special_remap(s, *hi, t, true), t, true);
		else
			*hi = p->p[3];
		if (*hi < *lo) {
			const float f = *lo;
			*lo = *hi;
			*hi = f;
		}
	}
	if (*lo == *hi)
		*hi += 0.01f;
	return true;
}

// RemapValue (0x801CA934): a real value to the slider's display scale, or back.
static float remap(CustTune* s, float v, int t, bool toDisplay) {
	float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
	total_range(s, t, &a, &b, 0, false);
	total_range(s, t, &c, &d, 0, true);
	float lo, hi, olo, ohi;
	if (is_gear(t)) {
		if (!toDisplay)
			return special_remap(s, unit_remap(s, v, t, false), t, false);
		v = special_remap(s, v, t, true);
		a = special_remap(s, a, t, true);
		b = special_remap(s, b, t, true);
		if (b < a) {
			const float f = a;
			a = b;
			b = f;
		}
		lo = a; hi = b; olo = c; ohi = d;
	} else if (!toDisplay) {
		lo = c; hi = d; olo = a; ohi = b;
	} else {
		lo = a; hi = b; olo = c; ohi = d;
	}
	if (v - lo < 0.0f)
		v = lo;
	const float width = hi - lo;
	if (hi - v < 0.0f)
		v = hi;
	return fmaf((v - lo) / width, ohi - olo, olo);
}

// UpdateStoredTuning (0x801CA52C): `v` clamped to the tuneable range and kept
// as the byte; the clamped value is returned.
static float update_stored(CustTune* s, float v, int t, int idx) {
	const float d = def_get(s, t, idx);
	float mn = 0.0f, mx = 0.0f;
	tuneable_range(s, t, &mn, &mx, idx, false);
	v = clampf(v, mn, mx);
	custtune_tunings_set(s->tunings, (v - d) / (mx - mn), t, idx);
	return v;
}

// SetTorqueValue (0x801CC194): one curve point to `v` (clamped), and with
// `elastic` the change spread onto its neighbours by ElasticTorqueAdjustments,
// alternating sides outwards, until it is used up or the table and eleven more
// steps are. The torque curve is kept as the base curve plus the boost.
static void set_torque(CustTune* s, float v, int t, int idx, bool elastic) {
	float vals[CUSTTUNE_CURVE_POINTS];
	for (int i = 0; i < CUSTTUNE_CURVE_POINTS; i++)
		vals[i] = real_get(s, t, i);
	const float delta = v - vals[idx];
	float mn = 0.0f, mx = 0.0f;
	tuneable_range(s, t, &mn, &mx, idx, false);
	vals[idx] = clampf(v, mn, mx);
	if (delta == 0.0f)
		return;
	if (1e-07f < fabsf(delta) && elastic) {
		int e = 0;
		int k = 0;
		float rem = delta;
		do {
			float f = -rem;
			if (e <= 8) {
				const float a = delta * kElastic[e];
				f = rem <= 0.0f ? maxf(a, rem) : minf(a, rem);
			}
			k = -k;
			if (k >= 0 && ++k > 8)
				k = 1;
			const int j = idx + k;
			if ((unsigned)j < CUSTTUNE_CURVE_POINTS) {
				tuneable_range(s, t, &mn, &mx, j, false);
				e++;
				const float old = vals[j];
				const float nv = clampf(old + f, mn, mx);
				vals[j] = nv;
				rem = rem + (nv - old);
			}
		} while (1e-07f < fabsf(rem) && e <= 19);
	}
	u8* P = s->cpi;
	if (t == CUSTTUNE_TORQUE) {
		for (int i = 0; i < CUSTTUNE_CURVE_POINTS; i++)
			wr(P, value_off(23, i), vals[i] + rd(P, value_off(24, i)));
	} else if (t == CUSTTUNE_BOOST) {
		for (int i = 0; i < CUSTTUNE_CURVE_POINTS; i++) {
			wr(P, value_off(23, i), real_get(s, CUSTTUNE_TORQUE, i) + vals[i]);
			wr(P, value_off(24, i), vals[i]);
		}
	}
}

// FixTorqueCurve (0x801CC46C): the curve moved evenly back to `target`'s sum.
static void fix_torque_curve(CustTune* s, float target) {
	float sum = 0.0f;
	for (int k = 0; k < 9; k++)
		sum = sum + rd(s->cpi, 0x200 + 4 * k);
	const float f = (target - sum) / 9.0f;
	for (int k = 0; k < 9; k++)
		wr(s->cpi, 0x200 + 4 * k, rd(s->cpi, 0x200 + 4 * k) + f);
}

// EnforceGearSpacingConstraint (0x801CC504): gear `t` to `v` (clamped), pushing
// the next gear in the direction of the change (a shorter gear pushes the one
// below, a taller one the one above) to stay 0.05 apart, recursively. Returns
// how much of `v` could not be taken.
static float enforce_gear_spacing(CustTune* s, float v, int t, float* arr) {
	float mn = 0.0f, mx = 0.0f;
	tuneable_range(s, t, &mn, &mx, 0, false);
	const float c = clampf(v, mn, mx);
	const float delta = v - arr[t - 8];
	if (delta == 0.0f)
		return 0.0f;
	int dir = 0;
	if (delta <= 0.0f || t < 11) {
		if (delta < 0.0f && t < 15)
			dir = 1;
	} else {
		dir = -1;
	}
	float ex = v - c;
	if (dir != 0) {
		const int nb = t + dir;
		const float n0 = arr[nb - 8];
		float nt = c - 0.05f;
		if (n0 <= nt || nb <= t) {
			const float up = c + 0.05f;
			nt = n0;
			if (n0 < up && nb < t && n0 - up < 0.0f)
				nt = up;
		} else if (n0 - nt < 0.0f) {
			nt = n0;
		}
		if (nt != n0)
			ex = ex + enforce_gear_spacing(s, nt, nb, arr);
	}
	arr[t - 8] = v - ex;
	return ex;
}

// SetGearRatio (0x801CC660): the six forward ratios through the spacing rule,
// the ones past the car's gear count zeroed.
static void set_gear_ratio(CustTune* s, float v, int t) {
	float arr[8] = { 0 };
	for (int g = CUSTTUNE_GEAR1; g <= CUSTTUNE_GEAR6; g++)
		arr[g - 8] = rd(s->cpi, value_off(g, 0));
	enforce_gear_spacing(s, v, t, arr);
	int n;
	memcpy(&n, s->cpi + 0x1C8, sizeof(n));
	n += 2;
	for (int i = n; i >= 0 && i < 8; i++)
		arr[i] = 0.0f;
	for (int g = CUSTTUNE_GEAR1; g <= CUSTTUNE_GEAR6; g++)
		wr(s->cpi, value_off(g, 0), arr[g - 8]);
}

// SetNitrousValue (0x801CC750): types 16 and 17 trade against each other --
// moving one moves the other the same share of its own range the other way.
static void set_nitrous(CustTune* s, float v, int t) {
	const int o = t == CUSTTUNE_NOS_2AC ? CUSTTUNE_NOS_2A0 : CUSTTUNE_NOS_2AC;
	float lo = 0.0f, hi = 0.0f, lo2 = 0.0f, hi2 = 0.0f;
	tuneable_range(s, t, &lo, &hi, 0, false);
	tuneable_range(s, o, &lo2, &hi2, 0, false);
	const float d = def_get(s, t, 0);
	const float d2 = def_get(s, o, 0);
	wr(s->cpi, value_off(t, 0), v);
	const float f = ((v - d) / (hi - lo)) * (hi2 - lo2);
	wr(s->cpi, value_off(o, 0), d2 - f);
}

// SetRealValue (0x801CBD84).
static void real_set(CustTune* s, float v, int t, int idx) {
	remove_unique(s);
	u8* P = s->cpi;
	if (t == CUSTTUNE_NOS_2A0 || t == CUSTTUNE_NOS_2AC) {
		set_nitrous(s, v, t);
	} else if (t == CUSTTUNE_TORQUE || t == CUSTTUNE_BOOST) {
		set_torque(s, v, t, idx, true);
	} else if (t == 1 || t == 5 || t == 19 || t == 20) {
		// The mean is set: each of the pair keeps its place in its own range.
		float lo = 0.0f, hi = 0.0f;
		real_minmax(s, t, &lo, &hi, 0, true);
		for (int i = 0; i < 2; i++) {
			float li = 0.0f, hi_ = 0.0f;
			real_minmax(s, t, &li, &hi_, i, false);
			const float c = clampf(v, lo, hi);
			wr(P, value_off(t, i), fmaf((c - lo) / (hi - lo), hi_ - li, li));
		}
	} else if (t == 3 || t == 7) {
		for (int i = 0; i < 2; i++)
			wr(P, value_off(t, i), rd(s->compleat, value_off(t, i)) - v);
	} else if (t == 9) {
		wr(P, value_off(9, 0), v);
		wr(P, value_off(9, 1), v);
	} else if (is_gear(t)) {
		set_gear_ratio(s, v, t);
	} else {
		const int o = value_off(t, idx);
		if (o >= 0)
			wr(P, o, v);
	}
	apply_unique(s);
}

// CustomPhysicsSpec::GetValue (0x801CA360).
static float spec_get(CustTune* s, int t, int idx, bool doRemap) {
	const float d = def_get(s, t, idx);
	float mn = 0.0f, mx = 0.0f;
	tuneable_range(s, t, &mn, &mx, idx, false);
	const float r = fmaf(custtune_tunings_get(s->tunings, t, idx), mx - mn, d);
	return doRemap ? remap(s, r, t, true) : r;
}

// CustomPhysicsSpec::SetValue (0x801CA3FC).
static void spec_set(CustTune* s, float v, int t, int idx, bool doRemap) {
	if (doRemap)
		v = remap(s, v, t, false);
	v = update_stored(s, v, t, idx);
	real_set(s, v, t, idx);
	if (t == CUSTTUNE_TORQUE || t == CUSTTUNE_BOOST) {
		for (int i = 0; i < CUSTTUNE_CURVE_POINTS; i++)
			update_stored(s, real_get(s, t, i), t, i);
	} else if (is_gear(t)) {
		for (int g = CUSTTUNE_GEAR1; g <= CUSTTUNE_GEAR6; g++)
			if (custtune_is_active(s, g))
				update_stored(s, real_get(s, g, 0), g, 0);
	} else if (t == CUSTTUNE_NOS_2A0 || t == CUSTTUNE_NOS_2AC) {
		const int o = t == CUSTTUNE_NOS_2AC ? CUSTTUNE_NOS_2A0 : CUSTTUNE_NOS_2AC;
		update_stored(s, real_get(s, o, 0), o, 0);
	}
}

// ---------------------------------------------------------------------------
// The spec
// ---------------------------------------------------------------------------

void custtune_init(CustTune* s, const u8* compleat, u8* cpi, const u8* installed,
		const int* junkman, const u8* tunings) {
	memset(s, 0, sizeof(*s));
	s->units = 1;
	s->compleat = compleat;
	s->cpi = cpi;
	s->installed = installed;
	s->junkman = junkman;
	if (tunings)
		memcpy(s->tunings, tunings, CUSTTUNE_SIZE);
	else
		custtune_tunings_reset(s->tunings, -1);
}

bool custtune_is_active(const CustTune* s, int t) {
	if (t < 0 || t >= CUSTTUNE_NUM_TYPES || !installed_params(s, t))
		return false;
	if (is_gear(t)) {
		int gears;
		memcpy(&gears, s->cpi + 0x1C8, sizeof(gears));
		return t - 8 < gears + 2;
	}
	return true;
}

int custtune_num_active(CustTune* s) {
	s->numActive = 0;
	for (int t = 0; t < CUSTTUNE_NUM_TYPES; t++)
		if (custtune_is_active(s, t))
			s->numActive++;
	return s->numActive;
}

void custtune_rebuild(CustTune* s) {
	remove_unique(s);
	memset(s->defaults, 0, sizeof(s->defaults));
	for (int t = 0; t < 0x17; t++)
		if (custtune_is_active(s, t))
			def_set(s, real_get(s, t, 0), t, 0);
	for (int t = 0; t < 0x17; t++) {
		if (!custtune_is_active(s, t))
			continue;
		const float v = spec_get(s, t, 0, false);
		if (is_gear(t))
			real_set(s, v, t, 0);
		else
			spec_set(s, v, t, 0, false);
	}
	// The gears again, now that every one has its new ratio: the spacing rule
	// of each against its tuned neighbours.
	for (int t = CUSTTUNE_GEAR1; t <= CUSTTUNE_GEAR6; t++)
		if (custtune_is_active(s, t))
			spec_set(s, spec_get(s, t, 0, false), t, 0, false);
	float before = 0.0f;
	for (int k = 0; k < 9; k++)
		before = before + rd(s->cpi, 0x200 + 4 * k);
	for (int t = CUSTTUNE_TORQUE; t <= CUSTTUNE_BOOST; t++) {
		if (!custtune_is_active(s, t))
			continue;
		for (int i = 0; i < CUSTTUNE_CURVE_POINTS; i++)
			def_set(s, real_get(s, t, i), t, i);
		for (int i = 0; i < CUSTTUNE_CURVE_POINTS; i++)
			set_torque(s, spec_get(s, t, i, false), t, i, false);
	}
	float after = 0.0f;
	for (int k = 0; k < 9; k++)
		after = after + rd(s->cpi, 0x200 + 4 * k);
	if (0.001f < fabsf(before - after))
		fix_torque_curve(s, before);
	apply_unique(s);
}

float custtune_get_value(CustTune* s, int t, int idx, bool doRemap) {
	if (t < 0 || t >= CUSTTUNE_NUM_TYPES)
		return 0.0f;
	return spec_get(s, t, idx, doRemap);
}

void custtune_set_value(CustTune* s, float v, int t, int idx, bool doRemap) {
	if (t < 0 || t >= CUSTTUNE_NUM_TYPES)
		return;
	spec_set(s, v, t, idx, doRemap);
}

bool custtune_tuneable_range(CustTune* s, int t, int idx, bool doRemap, float* mn, float* mx) {
	if (t < 0 || t >= CUSTTUNE_NUM_TYPES)
		return false;
	return tuneable_range(s, t, mn, mx, idx, doRemap);
}

bool custtune_total_range(CustTune* s, int t, int idx, bool doRemap, float* lo, float* hi) {
	if (t < 0 || t >= CUSTTUNE_NUM_TYPES)
		return false;
	return total_range(s, t, lo, hi, idx, doRemap);
}

void custtune_reset(CustTune* s, int t) {
	custtune_tunings_reset(s->tunings, t);
}
