// The car's opening parts (TASKS.md #372 step 7); see carpartanim.h.
#include "carpartanim.h"

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFSA_MAGIC    0x4153464Eu  // "NFSA"
#define NFSA_ENTITIES 21
#define NFSA_MARKERS  0x10
#define NFSA_TABLE    0x550

// The entity order of tools/carpartanim_pack.py.
enum {
	E_HOOD_REG, E_HOOD_FRONT, E_LEFT_HOOD_SPLIT, E_RIGHT_HOOD_SPLIT,
	E_FRONT_HOOD_SPLIT, E_BACK_HOOD_SPLIT,
	E_LEFT_DOOR_REG, E_LEFT_DOOR_SCISSOR, E_LEFT_DOOR_SUICIDE,
	E_RIGHT_DOOR_REG, E_RIGHT_DOOR_SCISSOR, E_RIGHT_DOOR_SUICIDE,
	E_TRUNK_REG,
	E_LEFT_DOOR_WIDE1, E_RIGHT_DOOR_WIDE1 = E_LEFT_DOOR_WIDE1 + 4,
};

typedef struct {
	int numQ, numT;
	const float* q;   // x y z w per key
	const float* t;   // x y z per key, NULL = the marker's
	float fps;
	const float* marker;  // row-major bMatrix4, row vectors
} Entity;

// CarPartAnimInstanceMapping: one per location once it has moved.
typedef struct {
	bool live;
	int state;
	float start;      // s, the clock at which the current swing's time 0 lies
	int style;
	bool hood22;
	// CarPartAnimInstanceMapping+8: the swing's speed, AnimateCarPart's
	// float -- 1 from the shops and Show Off, 20 from FEDoCarPartAnimNow
	// on a magazine cover's setup.
	float speed;
} Inst;

static u8* s_file;
static Entity s_ent[NFSA_ENTITIES];
static Inst s_inst[4];
static int s_hoodStyle, s_doorStyle, s_doorStyleOnOpen[4];
static bool s_hood22;

// Seconds since the first call: small enough to keep float precision.
static float now_s(void) {
	static u64 base;
	if (!base)
		base = osGetTime();
	return (float)(osGetTime() - base) * 0.001f;
}

static u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }

void carpartanim_unload(void) {
	free(s_file);
	s_file = NULL;
	memset(s_ent, 0, sizeof(s_ent));
	carpartanim_reset();
}

bool carpartanim_loaded(void) {
	return s_file != NULL;
}

bool carpartanim_load(const char* dataRoot, const char* car) {
	carpartanim_unload();
	char path[320];
	snprintf(path, sizeof(path), "%s/CARS/%s.nfsa", dataRoot, car);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* d = n > NFSA_TABLE + NFSA_ENTITIES * 16 ? (u8*)malloc((size_t)n) : NULL;
	const bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n &&
		rd32(d) == NFSA_MAGIC && rd32(d + 4) == 1 && rd32(d + 12) == NFSA_ENTITIES;
	fclose(f);
	if (!ok) {
		free(d);
		return false;
	}
	for (int e = 0; e < NFSA_ENTITIES; e++) {
		const u8* t = d + NFSA_TABLE + e * 16;
		Entity* E = &s_ent[e];
		u16 nq, nt;
		memcpy(&nq, t, 2);
		memcpy(&nt, t + 2, 2);
		const u32 qo = rd32(t + 4), to = rd32(t + 8);
		memcpy(&E->fps, t + 12, 4);
		E->marker = (const float*)(d + NFSA_MARKERS + e * 64);
		if (nq && qo + nq * 16u <= (u32)n) {
			E->numQ = nq;
			E->q = (const float*)(d + qo);
		}
		if (nt && to + nt * 12u <= (u32)n) {
			E->numT = nt;
			E->t = (const float*)(d + to);
		}
		if (!(E->fps > 0.0f))
			E->fps = 30.0f;
	}
	s_file = d;
	return true;
}

void carpartanim_set_styles(int hoodStyle, bool hood22, int doorStyle) {
	s_hoodStyle = hoodStyle;
	s_hood22 = hood22;
	s_doorStyle = doorStyle;
}

void carpartanim_reset(void) {
	memset(s_inst, 0, sizeof(s_inst));
	for (int i = 0; i < 4; i++)
		s_doorStyleOnOpen[i] = -1;
}

// GetCarPartEntity (0x8005256c): the entity a location's style draws, for
// the hood's model 0 or 1 (GetCarPartAnimMatrix's choice); -1 none.
static int entity_of(int loc, int style, int model) {
	int e = -1;
	switch (loc) {
	case CARANIM_HOOD:
		if (style == 2 || style == 3)
			e = model ? E_RIGHT_HOOD_SPLIT : E_LEFT_HOOD_SPLIT;
		else if (style == 4 || style == 5)
			e = model ? E_FRONT_HOOD_SPLIT : E_BACK_HOOD_SPLIT;
		else
			e = style == 1 ? E_HOOD_FRONT : E_HOOD_REG;
		break;
	case CARANIM_TRUNK:
		e = E_TRUNK_REG;
		break;
	case CARANIM_DOOR_LEFT:
	case CARANIM_DOOR_RIGHT: {
		const int base = loc == CARANIM_DOOR_LEFT ? E_LEFT_DOOR_REG : E_RIGHT_DOOR_REG;
		const int wide = loc == CARANIM_DOOR_LEFT ? E_LEFT_DOOR_WIDE1 : E_RIGHT_DOOR_WIDE1;
		e = style >= 3 && style <= 6 ? wide + style - 3 : base + (style >= 0 && style <= 2 ? style : 0);
		break;
	}
	default:
		return -1;
	}
	return s_ent[e].numQ ? e : -1;
}

static float raw_length(int e) {
	if (e < 0)
		return 0.0f;
	const Entity* E = &s_ent[e];
	return E->numQ > 1 ? (float)(E->numQ - 1) / E->fps : 0.0f;
}

static bool split_hood(int loc, const Inst* in) {
	return loc == CARANIM_HOOD && (in->style == 2 || in->style == 3 ||
	                               in->style == 4 || in->style == 5);
}

// GetCarPartAnimMatrix's dVar13: the swing's length. A split hood's is taken
// at 0.75 times the rate, the slower half's.
static float eval_length(int loc, const Inst* in) {
	const float raw = raw_length(entity_of(loc, in->style, 0));
	return split_hood(loc, in) ? raw / 0.75f : raw;
}

float carpartanim_length(int loc) {
	if (loc < 0 || loc > 3 || !s_inst[loc].live)
		return 0.0f;
	return eval_length(loc, &s_inst[loc]);
}

// ChangeCarPartAnimationState (0x80052904). GetAnimLength there is the
// location's style-0 entity's.
static void change_state(int loc, Inst* in, int to) {
	const int from = in->state;
	if (from == to)
		return;
	const float t = now_s();
	if ((from == CARANIM_CLOSED && to == CARANIM_OPENING) ||
	    (from == CARANIM_OPEN && to == CARANIM_CLOSING)) {
		in->state = to;
		in->start = t;
	} else if ((from == CARANIM_OPENING && to == CARANIM_CLOSING) ||
	           (from == CARANIM_CLOSING && to == CARANIM_OPENING)) {
		in->state = to;
		const float len = raw_length(entity_of(loc, 0, 0)) / in->speed;
		in->start = t - (len - (t - in->start));
	} else if ((from == CARANIM_OPENING && to == CARANIM_OPEN) ||
	           (from == CARANIM_CLOSING && to == CARANIM_CLOSED)) {
		in->state = to;
	}
}

void carpartanim_trigger(int loc, bool open) {
	carpartanim_trigger_speed(loc, open, 1.0f);
}

void carpartanim_trigger_speed(int loc, bool open, float speed) {
	if (!s_file || loc < 0 || loc > 3)
		return;
	if (!(speed > 0.0f))
		speed = 1.0f;
	const bool door = loc >= CARANIM_DOOR_LEFT;
	int style = loc == CARANIM_HOOD ? s_hoodStyle : door ? s_doorStyle : 0;
	Inst* in = &s_inst[loc];
	if (!in->live) {
		// A new instance starts opening, whichever way it was asked
		// (CarPartAnimInstanceMapping's constructor).
		s_doorStyleOnOpen[loc] = style;
		in->live = true;
		in->state = CARANIM_OPENING;
		in->start = now_s();
		in->style = style;
		in->hood22 = loc == CARANIM_HOOD && s_hood22;
		in->speed = speed;
		return;
	}
	if (s_doorStyleOnOpen[loc] == -1)
		s_doorStyleOnOpen[loc] = style;
	if (open ? (in->state == CARANIM_OPENING || in->state == CARANIM_OPEN)
	         : (in->state == CARANIM_CLOSING || in->state == CARANIM_CLOSED))
		return;
	in->hood22 = loc == CARANIM_HOOD && s_hood22;
	if (door) {
		// A door closes with the swing it opened with.
		if (in->state == CARANIM_CLOSED && open)
			s_doorStyleOnOpen[loc] = style;
		else if (in->state == CARANIM_OPEN && !open)
			style = s_doorStyleOnOpen[loc];
	}
	in->style = style;
	if (in->state == CARANIM_OPENING || in->state == CARANIM_CLOSING) {
		const float t = now_s();
		in->start = t - (t - in->start) * in->speed / speed;
	}
	in->speed = speed;
	change_state(loc, in, open ? CARANIM_OPENING : CARANIM_CLOSING);
}

int carpartanim_state(int loc) {
	if (loc < 0 || loc > 3 || !s_inst[loc].live)
		return CARANIM_CLOSED;
	return s_inst[loc].state;
}

void carpartanim_kill_doors_and_split_hood(void) {
	for (int loc = 0; loc < 4; loc++) {
		Inst* in = &s_inst[loc];
		if (in->live && (loc >= CARANIM_DOOR_LEFT || (loc == CARANIM_HOOD && in->style != 0)))
			in->live = false;
	}
}

// The entity's key pose at time t (s): rotation (column form) and
// translation, the keys linearly blended (nlerp).
static void sample(const Entity* E, float t, float R[3][3], float T[3]) {
	float k = t * E->fps;
	if (k < 0.0f)
		k = 0.0f;
	const float last = (float)(E->numQ - 1);
	if (k > last)
		k = last;
	int i = (int)k;
	if (i >= E->numQ - 1)
		i = E->numQ > 1 ? E->numQ - 2 : 0;
	const float u = E->numQ > 1 ? k - (float)i : 0.0f;
	const float* a = E->q + i * 4;
	const float* b = E->q + (E->numQ > 1 ? i + 1 : i) * 4;
	const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	const float sb = dot < 0.0f ? -u : u;
	float q[4];
	for (int c = 0; c < 4; c++)
		q[c] = a[c] * (1.0f - u) + b[c] * sb;
	const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (n > 1e-8f)
		for (int c = 0; c < 4; c++)
			q[c] /= n;
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - z * w); R[0][2] = 2 * (x * z + y * w);
	R[1][0] = 2 * (x * y + z * w); R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - x * w);
	R[2][0] = 2 * (x * z - y * w); R[2][1] = 2 * (y * z + x * w); R[2][2] = 1 - 2 * (x * x + y * y);
	// GetCarPartAnimMatrix: a translation within 1e-6 of zero on every axis
	// is the marker's.
	const float* M = E->marker;
	T[0] = M[12]; T[1] = M[13]; T[2] = M[14];
	if (E->numT) {
		int j = (int)k, jn = j + 1;
		if (j >= E->numT) j = E->numT - 1;
		if (jn >= E->numT) jn = E->numT - 1;
		const float v = E->numT > 1 ? k - (float)(int)k : 0.0f;
		float t3[3];
		for (int c = 0; c < 3; c++)
			t3[c] = E->t[j * 3 + c] * (1.0f - v) + E->t[jn * 3 + c] * v;
		if (fabsf(t3[0]) >= 1e-6f || fabsf(t3[1]) >= 1e-6f || fabsf(t3[2]) >= 1e-6f)
			memcpy(T, t3, sizeof(t3));
	}
}

// Render's part matrix: the animation matrix after the inverse of the rest
// one, v' = R(q) Rrest^-1 (v - T0) + T, T0 the marker's translation. The
// rest rotation is the marker's for the hood and the scissor-type doors
// (GetCarPartInitAnimMatrix), none for the trunk and the hinged doors.
static void frame_of(int loc, int style, int e, float t, float rot[3][3], float tr[3]) {
	const Entity* E = &s_ent[e];
	float R[3][3], T[3];
	sample(E, t, R, T);
	const float* M = E->marker;
	const bool rest = loc == CARANIM_HOOD ||
		(loc >= CARANIM_DOOR_LEFT && (style == 1 || (style >= 3 && style <= 6)));
	if (rest) {
		// Rrest (column form) is the marker's 3x3 transposed, so its inverse
		// is the marker's 3x3 as it is stored.
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				rot[i][j] = R[i][0] * M[0 * 4 + j] + R[i][1] * M[1 * 4 + j] + R[i][2] * M[2 * 4 + j];
	} else {
		memcpy(rot, R, sizeof(R));
	}
	for (int i = 0; i < 3; i++)
		tr[i] = T[i] - (rot[i][0] * M[12] + rot[i][1] * M[13] + rot[i][2] * M[14]);
}

void carpartanim_pose(Model3DPartPose* p, bool fe) {
	memset(p, 0, sizeof(*p));
	p->fe = fe;
	if (!s_file)
		return;
	const float now = now_s();
	for (int loc = 0; loc < 4; loc++) {
		Inst* in = &s_inst[loc];
		if (!in->live || in->state == CARANIM_CLOSED)
			continue;
		// In wall seconds: GetAnimLengthInSeconds after the rate is set
		// (the speed, times 0.75 on a split hood).
		const float len = eval_length(loc, in) / in->speed;
		const float el = now - in->start;
		float et;
		if (in->state == CARANIM_OPEN) {
			et = len;
		} else if (in->state == CARANIM_OPENING) {
			et = el < len ? el : len;
			if (len <= el)
				change_state(loc, in, CARANIM_OPEN);
		} else {
			et = len - (el < len ? el : len);
			if (len <= el)
				change_state(loc, in, CARANIM_CLOSED);
		}
		if (et < 0.0f)
			et = 0.0f;
		// The trunk underside shows once the trunk is all the way open
		// (Render's TRUNK_UNDER test); every other conditional part as soon
		// as its location leaves closed.
		p->open[loc] = loc == CARANIM_TRUNK ? in->state == CARANIM_OPEN
		                                    : in->state != CARANIM_CLOSED;
		// GetCurAnimNormalizedTime (0x80053e14): eval time / length.
		p->animT[loc] = len > 0.0f ? et / len : 1.0f;
		if (p->animT[loc] > 1.0f)
			p->animT[loc] = 1.0f;
		const int nModels = loc == CARANIM_HOOD ? 2 : 1;
		for (int m = 0; m < nModels; m++) {
			const int e = entity_of(loc, in->style, m);
			if (e < 0)
				continue;
			// GetCarPartAnimMatrix's eval rate: the speed; a split
			// _STYLE22_ hood's halves at 0.75 and 1.25 of it; any other split
			// hood at 1 whatever the speed (so a fast one stops short of its
			// end, as retail's does). The eval clamps at the swing's end.
			float rate = in->speed;
			if (split_hood(loc, in))
				rate = in->hood22 ? in->speed * (m ? 1.25f : 0.75f) : 1.0f;
			float t = et * rate;
			const float raw = raw_length(e);
			if (t > raw)
				t = raw;
			const int f = loc == CARANIM_HOOD ? m : loc + 1;
			frame_of(loc, in->style, e, t, p->rot[f], p->t[f]);
			p->moved[f] = true;
		}
		if (loc == CARANIM_HOOD && !p->moved[1] && p->moved[0]) {
			memcpy(p->rot[1], p->rot[0], sizeof(p->rot[0]));
			memcpy(p->t[1], p->t[0], sizeof(p->t[0]));
			p->moved[1] = true;
		}
	}
}
