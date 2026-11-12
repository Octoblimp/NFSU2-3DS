#include "difficulty.h"
#include "bunidx.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define DIFFICULTY_CHUNK_TYPE 0x00034B00u

// Where the pieces sit in the chunk payload. See difficulty.h for how each
// was established.
#define DIFF_OFF_RECORDS 0x524
#define DIFF_RECORD_SIZE 0x68      // == DIFFICULTY_FIELDS * 4
#define DIFF_OFF_PICK 0x97C
#define DIFF_OFF_LOOKAHEAD_SWITCH 0xAD4
#define DIFF_OFF_TRAFFIC_SPACING 0xA8C
// The whole payload, kept for the catch-up boost's Graphs and Tables.
#define DIFF_PAYLOAD_SIZE 0xAE0

static u8 s_pay[DIFF_PAYLOAD_SIZE];
static bool s_payOk;

// Not loaded until difficulty_load() says so, but the switch interval has to
// be sane from the first frame: a zero there would re-roll every driver's
// lookahead every tick.
static DifficultyInfo g_diff = {
	.lookaheadSwitchSec = DIFFICULTY_LOOKAHEAD_SWITCH_SEC
};

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static float rdf32le(const u8* p) {
	u32 bits = rd32le(p);
	float out;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

// What a driver gets when the chunk is not on the card: the shipped skill-9
// record, so the AI still drives properly and simply loses the spread between
// skills. Transcribed from the figures above, not invented.
static const float kFallback[DIFFICULTY_FIELDS] = {
	0.0f, 0.9f, 1.0f, 1.0f, 2.1f, 1.0f, 0.92f, 0.93f, 0.79f, -1.5f,
	-0.8f, 0.0f, 0.01f, -110.0f, 15.0f, -110.0f, 15.0f, 10.0f, 5.0f, 0.5f,
	4.0f, 0.95f, 1.1f, 19.0f, 20.0f, 0.0f
};

// The shipped +0xA8C table, for the same reason.
static const float kTrafficSpacing[DIFFICULTY_TRAFFIC_SPACINGS][2] = {
	{ 240.0f, 300.0f }, { 110.0f, 145.0f }, { 80.0f, 95.0f }, { 90.0f, 110.0f },
	{ 70.0f, 100.0f }, { 50.0f, 65.0f }, { 50.0f, 70.0f }, { 35.0f, 45.0f },
	{ 20.0f, 25.0f }
};

bool difficulty_load(const char* bunPath, void (*log)(const char* msg)) {
	memset(&g_diff, 0, sizeof(g_diff));
	g_diff.lookaheadSwitchSec = DIFFICULTY_LOOKAHEAD_SWITCH_SEC;
	if (!bunPath)
		return false;

	FILE* f = fopen(bunPath, "rb");
	if (!f)
		return false;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long fileSize = ftell(f);
	if (fileSize <= 0) {
		fclose(f);
		return false;
	}

	// 0x34B00 is a top-level chunk; the shared index (bunidx.h) knows where,
	// instead of an 8-byte read per chunk from the start of an 8MB file.
	long chunkOff = -1;
	u32 chunkSize = 0;
	if (!bunidx_find(bunPath, DIFFICULTY_CHUNK_TYPE, &chunkOff, &chunkSize))
		chunkOff = -1;
	if (chunkOff < 0) {
		fclose(f);
		if (log)
			log("difficulty: no 0x34B00 chunk, AI skills fall back to 9");
		return false;
	}

	// pTheDifficultyInfo is the payload itself, not the 16-byte-aligned base
	// cardb's record array uses: every offset the engine indexes with lands on
	// the right float only when measured from chunkOff + 8. (Checked both ways
	// against the shipped file -- from the aligned base the speed scales come
	// out as the cop column, two floats along.)
	long pay = chunkOff + 8;
	long need = DIFF_OFF_LOOKAHEAD_SWITCH + 4;
	if ((long)chunkSize < need) {
		fclose(f);
		if (log)
			log("difficulty: 0x34B00 chunk is short, AI skills fall back to 9");
		return false;
	}

	u8 buf[DIFF_RECORD_SIZE];
	bool ok = true;
	for (int s = 0; s < DIFFICULTY_SKILLS && ok; s++) {
		if (fseek(f, pay + DIFF_OFF_RECORDS + (long)s * DIFF_RECORD_SIZE,
				SEEK_SET) != 0 ||
			fread(buf, 1, DIFF_RECORD_SIZE, f) != DIFF_RECORD_SIZE) {
			ok = false;
			break;
		}
		for (int i = 0; i < DIFFICULTY_FIELDS; i++)
			g_diff.rec[s][i] = rdf32le(buf + i * 4);
	}

	for (int b = 0; b < 2 && ok; b++) {
		for (int t = 0; t < DIFFICULTY_STRENGTHS && ok; t++) {
			long at = pay + DIFF_OFF_PICK +
				(long)b * DIFFICULTY_STRENGTHS * DIFFICULTY_SKILLS * 4 +
				(long)t * DIFFICULTY_SKILLS * 4;
			if (fseek(f, at, SEEK_SET) != 0 ||
				fread(buf, 1, DIFFICULTY_SKILLS * 4, f)
					!= DIFFICULTY_SKILLS * 4) {
				ok = false;
				break;
			}
			for (int i = 0; i < DIFFICULTY_SKILLS; i++)
				g_diff.pick[b][t][i] = rdf32le(buf + i * 4);
		}
	}

	if (ok) {
		if (fseek(f, pay + DIFF_OFF_TRAFFIC_SPACING, SEEK_SET) != 0 ||
				fread(buf, 1, DIFFICULTY_TRAFFIC_SPACINGS * 8, f) != DIFFICULTY_TRAFFIC_SPACINGS * 8)
			ok = false;
		for (int i = 0; ok && i < DIFFICULTY_TRAFFIC_SPACINGS; i++) {
			g_diff.trafficSpacing[i][0] = rdf32le(buf + i * 8);
			g_diff.trafficSpacing[i][1] = rdf32le(buf + i * 8 + 4);
		}
	}
	s_payOk = false;
	if (ok && chunkSize >= DIFF_PAYLOAD_SIZE && fseek(f, pay, SEEK_SET) == 0 &&
			fread(s_pay, 1, DIFF_PAYLOAD_SIZE, f) == DIFF_PAYLOAD_SIZE)
		s_payOk = true;
	if (ok) {
		if (fseek(f, pay + DIFF_OFF_LOOKAHEAD_SWITCH, SEEK_SET) == 0 &&
			fread(buf, 1, 4, f) == 4) {
			float v = rdf32le(buf);
			if (v > 0.1f && v < 120.0f)
				g_diff.lookaheadSwitchSec = v;
		}
	}
	fclose(f);

	if (!ok) {
		memset(&g_diff, 0, sizeof(g_diff));
		g_diff.lookaheadSwitchSec = DIFFICULTY_LOOKAHEAD_SWITCH_SEC;
		if (log)
			log("difficulty: 0x34B00 chunk unreadable, AI skills fall back to 9");
		return false;
	}

	g_diff.loaded = true;
	if (log) {
		char b2[160];
		snprintf(b2, sizeof(b2),
			"difficulty: 0x34B00 at %ld, %d skills, speed %.0f%%..%.0f%%,"
			" lookahead jitter %.0f..%.0f m every %.1f s",
			chunkOff, DIFFICULTY_SKILLS,
			g_diff.rec[0][DIFF_RACER_SPEED_SCALE] * 100.0f,
			g_diff.rec[DIFFICULTY_SKILLS - 1][DIFF_RACER_SPEED_SCALE] * 100.0f,
			g_diff.rec[0][DIFF_LOOKAHEAD_JITTER],
			g_diff.rec[DIFFICULTY_SKILLS - 1][DIFF_LOOKAHEAD_JITTER],
			g_diff.lookaheadSwitchSec);
		log(b2);
	}
	return true;
}

const DifficultyInfo* difficulty_get(void) {
	return &g_diff;
}

float difficulty_field(int skill, int field) {
	if (field < 0 || field >= DIFFICULTY_FIELDS)
		return 0.0f;
	if (skill < 0) skill = 0;
	if (skill >= DIFFICULTY_SKILLS) skill = DIFFICULTY_SKILLS - 1;
	return g_diff.loaded ? g_diff.rec[skill][field] : kFallback[field];
}

// bRandom(float, u32*) (0x800590F8): bRandom(0x7FFFFFFF) scaled into [0, f).
// The integer draw is the same two-tap xorshift opponent.c documents -- the
// value returned is the state that came in, modulo n, and the state is
// advanced afterwards.
float difficulty_random(u32* rng, float f) {
	if (!rng)
		return 0.0f;
	const u32 s = *rng;
	u32 x = s ^ 0x1D872B41u;
	u32 y = x ^ (x >> 5);
	x ^= (y << 27);
	y ^= x;
	*rng = y;
	return f * (float)(s % 0x7FFFFFFFu) * (1.0f / 2147483647.0f);
}

void difficulty_traffic_spacing(int idx, float* minDist, float* maxDist) {
	if (idx < 0) idx = 0;
	if (idx >= DIFFICULTY_TRAFFIC_SPACINGS) idx = DIFFICULTY_TRAFFIC_SPACINGS - 1;
	const float* p = g_diff.loaded ? g_diff.trafficSpacing[idx] : kTrafficSpacing[idx];
	if (minDist) *minDist = p[0];
	if (maxDist) *maxDist = p[1];
}

int difficulty_pick_skill(int strength, u32* rng) {
	if (strength < 0) strength = 0;
	if (strength >= DIFFICULTY_STRENGTHS) strength = DIFFICULTY_STRENGTHS - 1;
	// Retail hands the first axis a hardcoded 1 at 0x8011AC20.
	const float* row = g_diff.loaded ? g_diff.pick[1][strength] : NULL;
	float r = difficulty_random(rng, 1.0f);
	if (!row) {
		// Without the chunk, the shipped block-1 rows amount to three bands of
		// three; the midpoint of the band is the honest stand-in.
		static const int kMid[DIFFICULTY_STRENGTHS] = { 1, 4, 7 };
		return kMid[strength];
	}
	// The scan itself: the first entry the draw does not exceed. A row of
	// leading zeroes is what keeps a strong opponent out of the low skills.
	if (r <= row[0])
		return 0;
	for (int i = 1; i < DIFFICULTY_SKILLS; i++)
		if (r <= row[i])
			return i;
	return 0;   // retail's `if (skill == -1) skill = 0` tail
}

// ---------------------------------------------------------------------------
// The catch-up boost (TASKS.md #255).

static float payF(int off) {
	return (s_payOk && off >= 0 && off + 4 <= DIFF_PAYLOAD_SIZE) ? rdf32le(s_pay + off) : 0.0f;
}
static u8 payB(int off) {
	return (s_payOk && off >= 0 && off < DIFF_PAYLOAD_SIZE) ? s_pay[off] : 0;
}

// Graph::GetValue (0x801A5214): the first point's y at or before the first x,
// the last's at or past the last, else the segment's lerp (a zero-width
// segment takes its midpoint, DAT_80331194 = 0.5).
float difficulty_graph(int off, int n, float x) {
	if (!s_payOk || n < 1) return 0.0f;
	if (n > 1) {
		const float x0 = payF(off), xl = payF(off + (n - 1) * 8);
		if (x <= x0) return payF(off + 4);
		if (xl <= x) return payF(off + (n - 1) * 8 + 4);
		for (int i = 0; i < n - 1; i++) {
			const float a = payF(off + i * 8), b = payF(off + i * 8 + 8);
			if (a <= x && x < b) {
				const float ya = payF(off + i * 8 + 4), yb = payF(off + i * 8 + 12);
				const float dx = b - a;
				if (fabsf(dx) > 1e-6f) return (x - a) / dx * (yb - ya) + ya;
				return (yb - ya) * 0.5f + ya;
			}
		}
	}
	return payF(off + 4);
}

// Table::GetValue (0x801A4EC8) over `n` floats at `off`, spread evenly from
// `lo` to `hi` (SetMinMax's CalcIndexMultiplier, (n - 1) / (hi - lo)).
static float payTable(int off, int n, float lo, float hi, float x) {
	const float f = (float)(n - 1) / (hi - lo) * (x - lo);
	const int i = (int)f;
	if (i < 0 || f < 0.0f) return payF(off);
	if (i < n - 1) {
		float fi = (float)i;
		if (f < fi) fi -= 1.0f;
		const float t = f - fi;
		return (1.0f - t) * payF(off + i * 4) + t * payF(off + i * 4 + 4);
	}
	return payF(off + (n - 1) * 4);
}

// Linear::Init(0, a, 9, b) then GetValue(skill).
static float skillLinear(float a, float b, float skill) {
	const float m = (b - a) / 9.0f;
	return m * skill + (a - m * 0.0f);
}

float difficulty_boost_interval(void) {
	const float v = payF(0xAD8);
	return v > 0.0f ? v : 0.3f;
}

// CalculateDistanceAndPositionBoost (0x80029954): `dist` from how far the car
// leads its target, `pos` from the places between them.
static void distAndPosBoost(const DiffBoostIn* in, int skill, float* dist, float* pos) {
	*dist = 0.0f;
	*pos = 0.0f;
	if (!in->catchupOn) return;
	// Metres the car is ahead of its target: laps apart on the race's length,
	// or the getaway's gaps to the leader.
	const float d = in->gameMode == 7 ? in->tGap - in->gap
		: in->lapLength * (in->laps_done - in->tLaps);
	if (d > payF(0xADC)) return;   // 100000: no car there
	int offA, offB;
	if (in->streetX) {
		offA = 0x2C0;
		offB = 0x318;
	} else {
		// r13-0x7EE0's {0, 4}: a circuit of four laps or more takes the
		// second pair. The PC data has the same curves in both, and in both
		// traffic halves (+0x58 at traffic level 2 or 3).
		const int idx = (!in->pointToPoint && in->laps >= 4) ? 1 : 0;
		const int tr = (unsigned)(in->trafficLevel - 2) < 2u ? 0x58 : 0;
		offA = idx * 0xB0 + tr;
		offB = offA + 0x160;
	}
	const float a = difficulty_graph(offA, 11, d), b = difficulty_graph(offB, 11, d);
	*dist = skillLinear(a, b, (float)skill);
	if (in->gameMode == 7 && in->place != 1) *dist *= 1.5f;
	// The closing speed, the more the nearer they are: -0.005 a m/s over
	// 0.01 d^2 + 1, within +/- 0.05.
	float s = (in->speed - in->tSpeed) * -0.005f / (d * 0.01f * d + 1.0f);
	if (s < -0.05f) s = -0.05f;
	if (s > 0.05f) s = 0.05f;
	*dist += s;
	if (!in->streetX || payB(0x420)) {
		const int k = in->tPlace - in->place;
		int idx = k < 0 ? k + 8 : k + 7;
		if (idx < 0) idx = 0;
		if (idx > 15) idx = 15;
		*pos = skillLinear(payF(0x424 + idx * 4), payF(0x464 + idx * 4), (float)skill);
	}
}

float difficulty_decide_boost(const DiffBoostIn* in, float* since, float prev) {
	// The gate: an AI racer, not in a drag or a drift, a race on, and not the
	// getaway's leader. Free roam (game mode 0) answers 0.75.
	bool on = in->racer && !in->dragOrDrift;
	float boost = 1.0f;
	if (in->gameMode == 0) {
		on = false;
		boost = 0.75f;
	}
	if (in->gameMode == 7 && in->place == 1) on = false;
	if (!on || !s_payOk) return boost;

	int skill = in->skill;
	if (skill < 0) skill = 0;
	if (skill >= DIFFICULTY_SKILLS) skill = DIFFICULTY_SKILLS - 1;
	if (*since <= difficulty_boost_interval()) return prev;
	*since = 0.0f;

	const bool act = in->actionAllows;
	bool target = false;
	boost = 1.0f;
	// CarControllerDisableCatchup (r13-0x7EEC) is 0 in the image.
	if (in->players > 0 && act && difficulty_field(skill, 23) <= in->raceSec) {
		target = true;
		const float handicap = (!in->streetX || payB(0x421)) ? in->handicap : 1.0f;
		float dist, pos;
		distAndPosBoost(in, skill, &dist, &pos);
		boost = pos * handicap + dist + 1.0f;
	}
	if (act && target && (!in->streetX || payB(0x422)) && in->catchupOn) {
		// Player::GetSpeedRatio against the Table for the target's place.
		const float floor = difficulty_field(skill, in->streetX ? 8 : 6);
		int place = in->tPlace;
		if (place < 0) place = 0;
		if (place > 5) place = 5;
		const float r = in->tRatio;
		const float T = payTable((r < 1.0f ? 0x4CC : 0x49C) + place * 8, 2, 0.0f, 9.0f,
			(float)skill);
		float f = (r - 1.0f) * T + 1.0f;
		if (f < floor) f = floor;
		boost *= f;
	}
	if (act && target && (!in->streetX || payB(0x423))) {
		// Faded over the race: (0,1) (0.33,1) (0.67,1) (1,0) in the PC data.
		const float g = difficulty_graph(0x504, 4,
			in->laps_done / (float)(in->laps > 1 ? in->laps : 1));
		boost = (boost - 1.0f) * g + 1.0f;
	}
	return boost;
}
