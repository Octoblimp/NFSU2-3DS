// See smokeable.h. The file layout is documented at the top of
// tools/smokeable_pack.py.
//
// What is retail's and what is not:
//   * Conversion, the anchor, the impulses, the damping modes, sleeping, the
//     timers and the clearing away are Speed.elf's, with its numbers (below).
//   * PORT: a prop meets the ground as its box's corners against the plane the
//     topology gives under its centre of gravity, once a frame, where retail
//     runs its GJK polytope through the world's collision geometry. The
//     contact is Contact::CollisionImpulse's: the larger of the two bounces
//     and frictions, the minimum impulse, the scrape ramp.
//   * PORT: against the walls it is a footprint through world_collide_coord,
//     the cars' wall test, its speed into the wall taken away; props do not
//     meet each other, and IsCarSmokeableWallContact has no counterpart.
//   * PORT: the drawn prop is the section's own scenery instance moved by a
//     matrix (model3d_draw_instance), so a prop goes when its section
//     unloads; retail's has its own model and outlives it.
//   * No pieces, as in retail (TASKS.md #517): Smokeable::Break (GC
//     0x801D4DA0, PC 0x5AE000) swaps a prop for its info's +0x8C pieces
//     ("<model>_CHILD1"), but only once its break strength (GC +0x148, PC
//     +0x154) reads -1. Init sets that to 0 and ConsiderImpulse wears it down
//     only while it is non-zero; no other code in either exe writes it, so
//     no prop ever breaks (13 L4RA infos list a piece, with no info for it).
//   * The emitters are retail's: the SmokeableInfo's two groups (+0x84 when
//     the anchor gives, +0x88 when it wakes) copied by Init, started by
//     ActivateAnchorEmitters and Awaken, carried by ServiceEmitters (afx.c).
//   * The prop's own contacts with the world go to the impulse hooks
//     (carsim_prop_world_impulse), so the collision sounds hear them.
#include "smokeable.h"
#include "model3d.h"
#include "topology.h"   // TOPO_COORD_PROP

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFSK_MAGIC   0x4B53464Eu  // 'NFSK' little-endian
#define NFSK_VERSION 3
#define NFSK_HDR     0x20
#define NFSK_INFO    0x90
#define NFSK_SPAWNER 0x50
#define NFSK_MAX     (1u * 1024 * 1024)

// Retail's numbers.
#define SMK_POOL          32               // ConvertToSmokeable's slot pool
#define SMK_MAX_STEP      (1.0f / 60.0f)   // SmokeableMaximumTimeStep (r13-0x68BC)
#define SMK_LIFT_SHRINK   0.2f             // ZOffsetShrinkSpeed (r13-0x68B8), m/s
#define SMK_LIFT_CLEAR    0.05f            // ConvertToSmokeable: the gap under the box
#define SMK_GRAVITY       9.8f             // ComputeAuxiliaries (0x801D5054)
#define SMK_FAST          2.0f             // CheckDampingSuitability (0x801D4EB4): mode 0 above
#define SMK_SLOW          0.8f             //   mode 1 above, mode 2 below
#define SMK_DAMP          0.95f            //   mode 1's momentum scale
#define SMK_CAR_SETTLE    0.1f             //   s since a car before mode 2; ConsiderSleeping's too
#define SMK_SLEEP_SPEED   0.1f             // ConsiderSleeping (0x801D4A84)
#define SMK_SLEEP_TOUCH   0.025f           //   s in contact first
#define SMK_REVERT        2.0f             // CheckTimers (0x801D45A8): unwoken, back to a virgin
#define SMK_THINK_MASK    0x7F             // SmokeableThink (0x801D21A8): every 128 frames
#define SMK_THINK_PHASE   0x4D
#define SMK_FREE_LOW      10               // DeleteFarAway: the far pass under this many free
#define SMK_FREE_CRITICAL 4                //   and at once, nearer, under this
#define SMK_FAR           300.0f
#define SMK_FAR_CRITICAL  20.0f
#define SMK_NEAR          120.0f
#define SMK_OLD           15.0f            //   s since it woke
#define SMK_BOUNCE        0.10f            // Contact: the ground's, the larger of the two
#define SMK_FRICTION      0.30f
#define SMK_WALL_BOUNCE   0.05f
#define SMK_MIN_J         0.30f            // MinimumContactImpulseFactor
#define SMK_SCRAPE        1.00f            // MaxFrictionScrapeSpeed
#define SMK_MAX_J         1000.0f          // RigidBody::ApplyImpulse's clamps
#define SMK_MAX_L         10.0f

// PORT: how near the ground counts as still touching it (Collision objects
// persist between steps in retail), how high above the box the plane probe
// looks, and the kerb a prop's wall band starts above.
#define SMK_TOUCH_MARGIN  0.02f
#define SMK_PROBE_ABOVE   1.0f
#define SMK_KERB          0.2f
#define SMK_CELL          64.0f            // the spawner grid
#define SMK_STEP_TRAVEL   0.1f             // #329: the most a box point moves a substep
#define SMK_MAX_STEPS     16

typedef struct {
	u32   hash;
	float mass, invMass;
	float invI[3][3];
	float cg[3], lo[3], hi[3];
	float anchor, lifetime, bounce, friction;
	u32   flags;
	char  name[24];
	u32   emitAnchor;    // +0x84, the emitter group the anchor's giving starts
	u32   emitAwaken;    // +0x88, the one waking starts
	u8    category;      // +0xD8, MatchSmokeableCategory's
} SmkInfo;

typedef struct {
	u16   section, info;
	u32   id;
	float rot[3][3];     // the matrix's rows: the model's axes, world
	float pos[3];        // its translation, the scenery instance's
	float lo[3], hi[3];  // the virgin's box, world
	// The scenery instance that draws it, looked up as its section came in.
	u32      serial;     // world_section_serial() it was found under, 0 none
	Model3D* model;
	u32      inst;
	bool     found;
	bool     hidden;     // out of its section's own draw
	bool     disabled;   // SmokeableSpawnerDisabledBits
} SmkSpawner;

typedef struct {
	bool       used;
	int        spawner;
	CarSimProp b;
	float      rot0[3][3];  // the spawner's axes, orthonormal: what the draw turns from
	float      lift;        // +0x15C: how far the body sits above where it is drawn
	float      gravity;     // RigidBody +0x14C
	int        mode;        // +0x4F0, CheckDampingSuitability's last
	bool       colliding;   // +0x16
	bool       touching;    // +0x14: a car this frame
	float      born, woke, lastCar, touchSince;  // the clock, s; woke < 0 never
	float      gp[3], gn[3];  // the ground plane under it, the last probe's
	bool       gHit;
	float      radius;      // half its box's diagonal
	u8*        mask;        // the batches drawing its instance
	// Its emitter copies (Smokeable +0x4DC, +0x4E4), afx.c's handles, and
	// +0x4EC: 1 once the anchor's have started, 2 once they have their
	// velocity.
	s16        fxAnchor[AFX_PROP_EMITTERS], fxAwaken[AFX_PROP_EMITTERS];
	int        nAnchor, nAwaken;
	int        anchorState;
} SmkProp;

struct Smokeables {
	void      (*log)(const char*);
	SmkInfo*    info;
	int         infoCount;
	SmkSpawner* sp;
	int         spCount;
	// The spawners by where they are: SMK_CELL cells over their boxes.
	float       gx0, gy0;
	int         gnx, gny;
	u16*        cellStart;  // gnx * gny + 1
	u16*        cellList;
	SmkProp     prop[SMK_POOL];
	float       clock;
	u32         frame;
	u32         serial;     // the last prop's, for colsfx.c
	Afx*        afx;
	SmokeableStats st;
};

static u32 rd32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd16(const u8* p) {
	return (u16)(p[0] | (p[1] << 8));
}

static float rdf(const u8* p) {
	const u32 u = rd32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static void rdfv(const u8* p, float* out, int n) {
	for (int i = 0; i < n; i++)
		out[i] = rdf(p + i * 4);
}

static float dot3(const float* a, const float* b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float* a, const float* b, float* o) {
	const float x = a[1] * b[2] - a[2] * b[1];
	const float y = a[2] * b[0] - a[0] * b[2];
	const float z = a[0] * b[1] - a[1] * b[0];
	o[0] = x;
	o[1] = y;
	o[2] = z;
}

static float len3(const float* a) {
	return sqrtf(dot3(a, a));
}

static void clampLen3(float* v, float max) {
	const float l = len3(v);
	if (l > max) {
		const float s = max / l;
		v[0] *= s;
		v[1] *= s;
		v[2] *= s;
	}
}

static void say(const Smokeables* s, const char* msg) {
	if (s && s->log)
		s->log(msg);
}

// Gram-Schmidt over the rows, as RigidBody keeps its matrix orthonormal.
static void orthonormalise(float r[3][3]) {
	float l = len3(r[0]);
	if (l < 1e-6f) {
		memset(r, 0, sizeof(float) * 9);
		r[0][0] = r[1][1] = r[2][2] = 1.0f;
		return;
	}
	for (int k = 0; k < 3; k++) r[0][k] /= l;
	const float d = dot3(r[0], r[1]);
	for (int k = 0; k < 3; k++) r[1][k] -= d * r[0][k];
	l = len3(r[1]);
	if (l < 1e-6f) {
		// The second axis lay along the first: any perpendicular will do.
		const float up[3] = { 0.0f, 0.0f, 1.0f }, side[3] = { 0.0f, 1.0f, 0.0f };
		cross3(fabsf(r[0][2]) < 0.9f ? up : side, r[0], r[1]);
		l = len3(r[1]);
	}
	for (int k = 0; k < 3; k++) r[1][k] /= l;
	cross3(r[0], r[1], r[2]);
}

// The prop's world inverse inertia applied to `v` (carsim.c's twin).
static void invInertiaWorld(const CarSimProp* p, const float* v, float* o) {
	float b[3], t[3];
	for (int k = 0; k < 3; k++) b[k] = dot3(p->rot[k], v);
	for (int k = 0; k < 3; k++)
		t[k] = p->invI[k][0] * b[0] + p->invI[k][1] * b[1] + p->invI[k][2] * b[2];
	for (int k = 0; k < 3; k++)
		o[k] = p->rot[0][k] * t[0] + p->rot[1][k] * t[1] + p->rot[2][k] * t[2];
}

// ComputeAuxiliaries__13PhysicsSystem: the velocities from the momenta.
static void velocities(CarSimProp* p) {
	for (int k = 0; k < 3; k++) p->vel[k] = p->P[k] * p->invMass;
	invInertiaWorld(p, p->L, p->w);
}

// RigidBody::ApplyImpulse, `r` from the centre of gravity.
static void applyImpulse(CarSimProp* p, const float* J, const float* r) {
	float j[3] = { J[0], J[1], J[2] }, l[3];
	clampLen3(j, SMK_MAX_J);
	cross3(r, j, l);
	clampLen3(l, SMK_MAX_L);
	for (int k = 0; k < 3; k++) {
		p->P[k] += j[k];
		p->L[k] += l[k];
	}
	velocities(p);
}

// The corner `i` of the prop's box (bit k: hi along axis k), world.
static void corner(const CarSimProp* p, int i, float* o) {
	float c[3];
	for (int k = 0; k < 3; k++) c[k] = (i >> k) & 1 ? p->hi[k] : p->lo[k];
	for (int k = 0; k < 3; k++)
		o[k] = p->pos[k] + c[0] * p->rot[0][k] + c[1] * p->rot[1][k] + c[2] * p->rot[2][k];
}

// How far the box reaches below and above its centre of gravity.
static void heightRange(const CarSimProp* p, float* lo, float* hi) {
	float a = 1e30f, b = -1e30f;
	for (int i = 0; i < 8; i++) {
		float c[3];
		corner(p, i, c);
		if (c[2] < a) a = c[2];
		if (c[2] > b) b = c[2];
	}
	*lo = a;
	*hi = b;
}

// ------------------------------------------------------------------ loading

static bool buildGrid(Smokeables* s) {
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	for (int i = 0; i < s->spCount; i++) {
		const SmkSpawner* p = &s->sp[i];
		if (p->lo[0] < x0) x0 = p->lo[0];
		if (p->lo[1] < y0) y0 = p->lo[1];
		if (p->hi[0] > x1) x1 = p->hi[0];
		if (p->hi[1] > y1) y1 = p->hi[1];
	}
	s->gx0 = x0;
	s->gy0 = y0;
	s->gnx = (int)((x1 - x0) / SMK_CELL) + 1;
	s->gny = (int)((y1 - y0) / SMK_CELL) + 1;
	if (s->gnx < 1 || s->gny < 1 || s->gnx > 1024 || s->gny > 1024)
		return false;
	const int cells = s->gnx * s->gny;
	s->cellStart = (u16*)calloc((size_t)cells + 1, sizeof(u16));
	if (!s->cellStart)
		return false;
	// Count, then fill: a spawner goes in every cell its box touches.
	int total = 0;
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			int run = 0;
			for (int c = 0; c <= cells; c++) {
				const int n = s->cellStart[c];
				s->cellStart[c] = (u16)run;
				run += n;
			}
			total = run;
			if (total > 0xFFFF)
				return false;
			s->cellList = (u16*)malloc((size_t)(total ? total : 1) * sizeof(u16));
			if (!s->cellList)
				return false;
		}
		u16* fill = pass ? (u16*)calloc((size_t)cells, sizeof(u16)) : NULL;
		if (pass && !fill)
			return false;
		for (int i = 0; i < s->spCount; i++) {
			const SmkSpawner* p = &s->sp[i];
			const int cx0 = (int)((p->lo[0] - x0) / SMK_CELL), cx1 = (int)((p->hi[0] - x0) / SMK_CELL);
			const int cy0 = (int)((p->lo[1] - y0) / SMK_CELL), cy1 = (int)((p->hi[1] - y0) / SMK_CELL);
			for (int cy = cy0; cy <= cy1; cy++)
				for (int cx = cx0; cx <= cx1; cx++) {
					const int c = cy * s->gnx + cx;
					if (!pass)
						s->cellStart[c]++;
					else
						s->cellList[s->cellStart[c] + fill[c]++] = (u16)i;
				}
		}
		free(fill);
	}
	return true;
}

Smokeables* smokeable_open(const char* path, void (*log)(const char* msg)) {
	char b[600];
	FILE* f = fopen(path, "rb");
	if (!f) {
		if (log) {
			snprintf(b, sizeof(b), "smokeable: %s missing (run tools/smokeable_pack.py) -- "
				"the cars pass through the barrels and signs", path);
			log(b);
		}
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* blob = (size >= NFSK_HDR && (u32)size <= NFSK_MAX) ? (u8*)malloc((size_t)size) : NULL;
	const bool read = blob && fread(blob, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	const u32 infos = read ? rd32(blob + 0x08) : 0, spawners = read ? rd32(blob + 0x0c) : 0;
	const u32 io = read ? rd32(blob + 0x10) : 0, so = read ? rd32(blob + 0x14) : 0;
	const bool ok = read && rd32(blob) == NFSK_MAGIC && rd32(blob + 4) == NFSK_VERSION &&
		infos && infos < 256 && spawners && spawners < 0x8000 &&
		(u64)io + (u64)infos * NFSK_INFO <= (u64)size &&
		(u64)so + (u64)spawners * NFSK_SPAWNER <= (u64)size;
	Smokeables* s = ok ? (Smokeables*)calloc(1, sizeof(Smokeables)) : NULL;
	if (s) {
		s->log = log;
		s->info = (SmkInfo*)calloc(infos, sizeof(SmkInfo));
		s->sp = (SmkSpawner*)calloc(spawners, sizeof(SmkSpawner));
	}
	if (!s || !s->info || !s->sp) {
		if (log) {
			snprintf(b, sizeof(b), "smokeable: %s %s", path,
				!read ? "unreadable" : !ok ? "malformed or the wrong version" : "out of memory");
			log(b);
		}
		free(blob);
		smokeable_close(s);
		return NULL;
	}
	s->infoCount = (int)infos;
	for (u32 i = 0; i < infos; i++) {
		const u8* r = blob + io + i * NFSK_INFO;
		SmkInfo* in = &s->info[i];
		in->hash = rd32(r);
		in->mass = rdf(r + 0x04);
		in->invMass = rdf(r + 0x08);
		rdfv(r + 0x0c, &in->invI[0][0], 9);
		rdfv(r + 0x30, in->cg, 3);
		rdfv(r + 0x3c, in->lo, 3);
		rdfv(r + 0x48, in->hi, 3);
		in->anchor = rdf(r + 0x54);
		in->lifetime = rdf(r + 0x58);
		in->bounce = rdf(r + 0x5c);
		in->friction = rdf(r + 0x60);
		in->flags = rd32(r + 0x64);
		memcpy(in->name, r + 0x68, sizeof(in->name) - 1);
		in->emitAnchor = rd32(r + 0x80);
		in->emitAwaken = rd32(r + 0x84);
		in->category = (u8)rd32(r + 0x88);
	}
	s->spCount = (int)spawners;
	for (u32 i = 0; i < spawners; i++) {
		const u8* r = blob + so + i * NFSK_SPAWNER;
		SmkSpawner* p = &s->sp[i];
		p->section = rd16(r);
		p->info = rd16(r + 2);
		p->id = rd32(r + 4);
		rdfv(r + 0x08, &p->rot[0][0], 9);
		rdfv(r + 0x2c, p->pos, 3);
		rdfv(r + 0x38, p->lo, 3);
		rdfv(r + 0x44, p->hi, 3);
		if (p->info >= infos)
			p->disabled = true;   // a bad record: never converted
	}
	free(blob);
	if (!buildGrid(s)) {
		say(s, "smokeable: spawner grid failed -- the cars pass through the props");
		smokeable_close(s);
		return NULL;
	}
	snprintf(b, sizeof(b), "smokeable: %s: %d infos, %d spawners, grid %dx%d", path,
		s->infoCount, s->spCount, s->gnx, s->gny);
	say(s, b);
	return s;
}

static void detachEmitters(Smokeables* s, SmkProp* p);

void smokeable_close(Smokeables* s) {
	if (!s)
		return;
	for (int k = 0; k < SMK_POOL; k++) {
		detachEmitters(s, &s->prop[k]);
		free(s->prop[k].mask);
	}
	free(s->cellStart);
	free(s->cellList);
	free(s->sp);
	free(s->info);
	free(s);
}

void smokeable_set_afx(Smokeables* s, Afx* a) {
	if (s)
		s->afx = a;
}

// -------------------------------------------------------------- the emitters

// A bMatrix4 as afx.c takes it: the axes as rows, then the position.
static void matrix4(const float rot[3][3], const float pos[3], float m[16]) {
	for (int r = 0; r < 3; r++) {
		for (int k = 0; k < 3; k++)
			m[r * 4 + k] = rot[r][k];
		m[r * 4 + 3] = 0.0f;
	}
	for (int k = 0; k < 3; k++)
		m[12 + k] = pos[k];
	m[15] = 1.0f;
}

// RemoveAutoEmitter for every copy (~Smokeable): the water already in the air
// falls on.
static void detachEmitters(Smokeables* s, SmkProp* p) {
	for (int i = 0; i < p->nAnchor; i++)
		afx_detach(s->afx, p->fxAnchor[i]);
	for (int i = 0; i < p->nAwaken; i++)
		afx_detach(s->afx, p->fxAwaken[i]);
	p->nAnchor = p->nAwaken = 0;
}

// Awaken (0x801D4CE4): the first time the prop wakes, its +0x88 group starts
// where the body is (+0x4ED set, never again).
static void awakenEmitters(Smokeables* s, SmkProp* p) {
	CarSimProp* b = &p->b;
	if (b->emitted)
		return;
	b->emitted = true;
	const SmkInfo* in = &s->info[s->sp[p->spawner].info];
	float m[16];
	matrix4(b->rot, b->pos, m);
	p->nAwaken = afx_attach(s->afx, in->emitAwaken, m, p->fxAwaken);
	if (p->nAwaken) {
		char b2[120];
		snprintf(b2, sizeof(b2), "smokeable: %s #%u awaken emitters %d (group %08x)", in->name,
			(unsigned)s->sp[p->spawner].id, p->nAwaken, (unsigned)in->emitAwaken);
		say(s, b2);
	}
}

// ActivateAnchorEmitters (0x801D4768): the anchor gave, and its +0x84 group
// starts where the spawner put it (SetInitialPosition) and stays there.
static void anchorEmitters(Smokeables* s, SmkProp* p) {
	const SmkSpawner* sp = &s->sp[p->spawner];
	const SmkInfo* in = &s->info[sp->info];
	float m[16];
	matrix4(sp->rot, sp->pos, m);
	p->nAnchor = afx_attach(s->afx, in->emitAnchor, m, p->fxAnchor);
	p->anchorState = 1;
	if (p->nAnchor) {
		char b2[120];
		snprintf(b2, sizeof(b2), "smokeable: %s #%u anchor emitters %d (group %08x)", in->name,
			(unsigned)sp->id, p->nAnchor, (unsigned)in->emitAnchor);
		say(s, b2);
	}
}

// ServiceEmitters (0x801D4418), at the end of Move while the prop is awake:
// the awaken copies ride on the body and their particles take its velocity;
// the anchor copies take the velocity once.
static void serviceEmitters(Smokeables* s, SmkProp* p) {
	const CarSimProp* b = &p->b;
	if (!b->awake)
		return;
	float m[16];
	matrix4(b->rot, b->pos, m);
	for (int i = 0; i < p->nAwaken; i++)
		afx_attach_move(s->afx, p->fxAwaken[i], m, b->vel);
	if (p->anchorState == 1) {
		for (int i = 0; i < p->nAnchor; i++)
			afx_attach_move(s->afx, p->fxAnchor[i], NULL, b->vel);
		p->anchorState = 2;
	}
}

// --------------------------------------------------------------- the virgins

// The prop's spawner back in its section's draw and convertible again
// (EnableSmokeableSpawner), or not; the prop slot freed either way.
static void drop(Smokeables* s, int k, bool reenable) {
	SmkProp* p = &s->prop[k];
	SmkSpawner* sp = &s->sp[p->spawner];
	detachEmitters(s, p);
	if (reenable) {
		sp->disabled = false;
		if (sp->found && sp->hidden) {
			model3d_hide_instance(sp->model, sp->inst, false);
			sp->hidden = false;
		}
	}
	free(p->mask);
	memset(p, 0, sizeof(*p));
}

// Each spawner's instance, found again whenever its section's load changes,
// and hidden again if it was knocked loose earlier in the session.
static void syncSpawners(Smokeables* s, const World* w) {
	for (int i = 0; i < s->spCount; i++) {
		SmkSpawner* p = &s->sp[i];
		const u32 serial = world_section_serial(w, p->section);
		if (serial == p->serial)
			continue;
		p->serial = serial;
		p->found = p->hidden = false;
		p->model = NULL;
		if (serial && world_find_instance(w, p->section, p->pos, &p->model, &p->inst)) {
			p->found = true;
			if (p->disabled) {
				model3d_hide_instance(p->model, p->inst, true);
				p->hidden = true;
			}
		}
	}
}

// The ground under (x, y) at or below zMax through the props' coordinate:
// its height and normal. False where there is none.
static bool probe(const World* w, float x, float y, float zMax, float* z, float* n) {
	const float xy[2] = { x, y };
	bool hit = false;
	world_ground_probe_coord(w, TOPO_COORD_PROP, xy, 1, zMax, z, &hit, n, NULL);
	return hit;
}

// ConvertToSmokeable (0x801D2ED8): the virgin becomes a Smokeable at the
// spawner's matrix, lifted clear of the ground under it; the spawner is
// disabled and its scenery hidden. -1 when the pool is full.
static int convert(Smokeables* s, const World* w, int si) {
	int k = 0;
	while (k < SMK_POOL && s->prop[k].used)
		k++;
	if (k == SMK_POOL)
		return -1;
	SmkSpawner* sp = &s->sp[si];
	const SmkInfo* in = &s->info[sp->info];
	SmkProp* p = &s->prop[k];
	memset(p, 0, sizeof(*p));
	p->used = true;
	p->spawner = si;
	memcpy(p->rot0, sp->rot, sizeof(p->rot0));
	orthonormalise(p->rot0);
	CarSimProp* b = &p->b;
	memcpy(b->rot, p->rot0, sizeof(b->rot));
	for (int a = 0; a < 3; a++) {
		b->pos[a] = sp->pos[a] + in->cg[0] * b->rot[0][a] + in->cg[1] * b->rot[1][a] +
			in->cg[2] * b->rot[2][a];
		b->cg[a] = in->cg[a];
		b->lo[a] = in->lo[a];
		b->hi[a] = in->hi[a];
	}
	b->invMass = in->invMass;
	memcpy(b->invI, in->invI, sizeof(b->invI));
	b->anchor = in->anchor;
	b->bounce = in->bounce;
	b->friction = in->friction;
	b->category = in->category;
	if (++s->serial == 0)
		s->serial = 1;
	b->serial = s->serial;
	// The lift: the ground under the spawner against the bottom of the box,
	// 5 cm clear. The body goes up by it and the drawn model does not.
	float gz, gn[3];
	p->gHit = probe(w, sp->pos[0], sp->pos[1], sp->hi[2] + SMK_PROBE_ABOVE, &gz, gn);
	if (p->gHit) {
		const float bottom = (in->lo[2] + in->cg[2]) * b->rot[2][2] + sp->pos[2];
		const float lift = gz - bottom + SMK_LIFT_CLEAR;
		if (lift > 0.0f) {
			p->lift = lift;
			b->pos[2] += lift;
		}
		p->gp[0] = sp->pos[0];
		p->gp[1] = sp->pos[1];
		p->gp[2] = gz;
		memcpy(p->gn, gn, sizeof(p->gn));
	}
	float d[3];
	for (int a = 0; a < 3; a++) d[a] = in->hi[a] - in->lo[a];
	p->radius = len3(d) * 0.5f;
	p->gravity = SMK_GRAVITY;
	p->born = s->clock;
	p->woke = -1.0f;
	p->lastCar = s->clock;
	if (sp->found && world_section_serial(w, sp->section) == sp->serial) {
		const int n = model3d_batch_count(sp->model);
		p->mask = n > 0 ? (u8*)calloc((size_t)(n + 7) / 8, 1) : NULL;
		if (p->mask && !model3d_instance_batches(sp->model, sp->inst, p->mask)) {
			free(p->mask);
			p->mask = NULL;
		}
		model3d_hide_instance(sp->model, sp->inst, true);
		sp->hidden = true;
	}
	sp->disabled = true;
	s->st.converted++;
	char m[200];
	snprintf(m, sizeof(m), "smokeable: %s #%u converted at %.1f,%.1f,%.1f (slot %d, lift %.2f)",
		in->name, (unsigned)sp->id, sp->pos[0], sp->pos[1], sp->pos[2], k, p->lift);
	say(s, m);
	return k;
}

// ------------------------------------------------------------- the movement

// CheckDampingSuitability then ComputeAuxiliaries: which mode the prop is in
// and what it does to the momenta and gravity.
static void auxiliaries(const Smokeables* s, SmkProp* p) {
	CarSimProp* b = &p->b;
	const float v = len3(b->vel), w = len3(b->w);
	int mode;
	if (v > SMK_FAST || w > SMK_FAST || (!p->colliding && p->mode != 2))
		mode = 0;
	else if (v > SMK_SLOW || w > SMK_SLOW)
		mode = 1;
	else
		mode = s->clock - p->lastCar >= SMK_CAR_SETTLE ? 2 : 1;
	p->mode = mode;
	if (mode == 0) {
		p->gravity = SMK_GRAVITY;
	} else if (mode == 1) {
		for (int k = 0; k < 3; k++) {
			b->P[k] *= SMK_DAMP;
			b->L[k] *= SMK_DAMP;
		}
		p->gravity = SMK_GRAVITY;
	} else {
		memset(b->P, 0, sizeof(b->P));
		memset(b->L, 0, sizeof(b->L));
		p->gravity = 0.0f;
	}
	velocities(b);
}

// PORT (#329): a friction impulse no larger than the one that stops the slip
// at `r`, as the car's contact caps it (carsim_prop_contact). Retail's
// Coulomb term alone, on a 5 kg sign's corner, turns the slip round at many
// times its speed.
static float smkFrictionCap(const CarSimProp* b, const float* r, const float* vt, float vtl,
		float jf) {
	if (vtl <= 1e-5f)
		return jf;
	float td[3], rt[3], tt[3], ut[3];
	for (int k = 0; k < 3; k++) td[k] = vt[k] / vtl;
	cross3(r, td, rt);
	invInertiaWorld(b, rt, tt);
	cross3(tt, r, ut);
	const float kt = b->invMass + dot3(td, ut);
	return kt > 1e-6f && jf * kt > vtl ? vtl / kt : jf;
}

// PORT: the box against the ground plane. The corners below it are pushed
// out along its normal and the impulse goes in at their depth-weighted
// middle, the one contact retail's GJK would give a face lying on a plane.
// True while the box is on or within SMK_TOUCH_MARGIN of the ground.
static bool groundContact(SmkProp* p) {
	if (!p->gHit)
		return false;
	CarSimProp* b = &p->b;
	const float* n = p->gn;
	const float d = dot3(n, p->gp);
	float cp[3] = { 0.0f, 0.0f, 0.0f }, wsum = 0.0f, deepest = 0.0f;
	bool near = false;
	for (int i = 0; i < 8; i++) {
		float c[3];
		corner(b, i, c);
		const float depth = d - dot3(n, c);
		if (depth > -SMK_TOUCH_MARGIN)
			near = true;
		if (depth > 0.0f) {
			for (int k = 0; k < 3; k++) cp[k] += c[k] * depth;
			wsum += depth;
			if (depth > deepest) deepest = depth;
		}
	}
	if (wsum <= 0.0f)
		return near;
	float r[3];
	for (int k = 0; k < 3; k++) {
		b->pos[k] += n[k] * deepest;
		cp[k] = cp[k] / wsum + n[k] * deepest;
		r[k] = cp[k] - b->pos[k];
	}
	float wxr[3], vp[3];
	cross3(b->w, r, wxr);
	for (int k = 0; k < 3; k++) vp[k] = b->vel[k] + wxr[k];
	const float vn = dot3(vp, n);
	if (vn >= 0.0f)
		return true;
	float rn[3], t[3], u[3];
	cross3(r, n, rn);
	invInertiaWorld(b, rn, t);
	cross3(t, r, u);
	const float denom = b->invMass + dot3(n, u);
	if (denom <= 1e-6f)
		return true;
	const float bounce = b->bounce > SMK_BOUNCE ? b->bounce : SMK_BOUNCE;
	const float mu = b->friction > SMK_FRICTION ? b->friction : SMK_FRICTION;
	float j = -(bounce + 1.0f) * vn / denom;
	if (b->invMass > 0.0f && j < SMK_MIN_J / b->invMass)
		j = SMK_MIN_J / b->invMass;
	float vt[3];
	for (int k = 0; k < 3; k++) vt[k] = vp[k] - n[k] * vn;
	const float vtl = len3(vt);
	float jf = j * mu;
	if (vtl < SMK_SCRAPE) jf *= vtl / SMK_SCRAPE;
	jf = smkFrictionCap(b, r, vt, vtl, jf);
	float J[3];
	for (int k = 0; k < 3; k++)
		J[k] = n[k] * j - (vtl > 1e-5f ? vt[k] / vtl * jf : 0.0f);
	applyImpulse(b, J, r);
	carsim_prop_world_impulse(b, true, cp, J, vtl);
	return true;
}

// PORT: the walls, once a frame, as the cars meet them: a footprint the size
// of the box seen from above carried from where the frame began to where it
// ends, and the speed into the wall taken away with the wall's bounce.
static void wallContact(const World* w, SmkProp* p, const float from[3]) {
	CarSimProp* b = &p->b;
	float rx = 0.0f, ry = 0.0f;
	for (int k = 0; k < 3; k++) {
		const float h = (b->hi[k] - b->lo[k]) * 0.5f;
		rx += fabsf(b->rot[k][0]) * h;
		ry += fabsf(b->rot[k][1]) * h;
	}
	float zLo, zHi;
	heightRange(b, &zLo, &zHi);
	if (zHi - zLo <= SMK_KERB)
		return;
	float to[3] = { b->pos[0], b->pos[1], b->pos[2] };
	float nrm[2] = { 0.0f, 0.0f };
	if (!world_collide_coord(w, TOPO_COORD_PROP, from, to, 0.0f, rx, ry, zLo + SMK_KERB, zHi,
			nrm, NULL, NULL))
		return;
	b->pos[0] = to[0];
	b->pos[1] = to[1];
	const float vn = b->vel[0] * nrm[0] + b->vel[1] * nrm[1];
	if (vn < 0.0f && b->invMass > 0.0f) {
		const float j = -(1.0f + SMK_WALL_BOUNCE) * vn / b->invMass;
		const float tx = b->vel[0] - nrm[0] * vn, ty = b->vel[1] - nrm[1] * vn;
		b->P[0] += nrm[0] * j;
		b->P[1] += nrm[1] * j;
		velocities(b);
		// At the box's side facing the wall, halfway up its band.
		const float J[3] = { nrm[0] * j, nrm[1] * j, 0.0f };
		const float at[3] = { b->pos[0] - nrm[0] * rx, b->pos[1] - nrm[1] * ry,
			(zLo + SMK_KERB + zHi) * 0.5f };
		carsim_prop_world_impulse(b, false, at, J, sqrtf(tx * tx + ty * ty));
	}
	p->colliding = true;
}

// Smokeable::Move (0x801D3EC0) for an awake prop: SmokeableMaximumTimeStep
// substeps of IntegrateEuler, each followed by the ground and the
// auxiliaries, then ConsiderSleeping.
static void move(Smokeables* s, const World* w, SmkProp* p, float dt) {
	CarSimProp* b = &p->b;
	const float from[3] = { b->pos[0], b->pos[1], b->pos[2] };
	{
		float gz, gn[3];
		float zLo, zHi;
		heightRange(b, &zLo, &zHi);
		if (probe(w, b->pos[0], b->pos[1], zHi + SMK_PROBE_ABOVE, &gz, gn)) {
			p->gHit = true;
			p->gp[0] = b->pos[0];
			p->gp[1] = b->pos[1];
			p->gp[2] = gz;
			memcpy(p->gn, gn, sizeof(p->gn));
		}
	}
	// PORT (#329): retail's GJK backs a contact up to where it began; the
	// plane test here pushes the box out by however deep a corner went, so
	// the steps are cut short enough that no point of the box moves more
	// than SMK_STEP_TRAVEL in one.
	int n = (int)(dt / SMK_MAX_STEP) + 1;
	const float travel = (len3(b->vel) + len3(b->w) * p->radius) * dt;
	const int nt = (int)ceilf(travel / SMK_STEP_TRAVEL);
	if (nt > n)
		n = nt < SMK_MAX_STEPS ? nt : SMK_MAX_STEPS;
	const float h = dt / (float)n;
	const float mass = b->invMass > 0.0f ? 1.0f / b->invMass : 0.0f;
	for (int i = 0; i < n; i++) {
		b->P[2] -= mass * p->gravity * h;
		velocities(b);
		for (int k = 0; k < 3; k++) b->pos[k] += b->vel[k] * h;
		const float spin0 = dot3(b->L, b->w);
		for (int a = 0; a < 3; a++) {
			float d[3];
			cross3(b->w, b->rot[a], d);
			for (int k = 0; k < 3; k++) b->rot[a][k] += d[k] * h;
		}
		orthonormalise(b->rot);
		// PORT (#329): IntegrateEuler's rotation, on a body as lopsided as a
		// sign (1:11 inverse inertia), gains spin energy every step in the
		// air -- 8 to 89 rad/s in a second. L is scaled back to the energy
		// it had, so a free prop never spins up by itself.
		velocities(b);
		const float spin1 = dot3(b->L, b->w);
		if (spin1 > spin0 && spin0 > 0.0f) {
			const float s = sqrtf(spin0 / spin1);
			for (int k = 0; k < 3; k++) b->L[k] *= s;
			velocities(b);
		}
		const bool ground = groundContact(p);
		const bool col = ground || p->touching;
		if (col && !p->colliding)
			p->touchSince = s->clock;
		p->colliding = col;
		auxiliaries(s, p);
	}
	wallContact(w, p, from);
	if (s->clock - p->lastCar > SMK_CAR_SETTLE && p->colliding &&
			s->clock - p->touchSince > SMK_SLEEP_TOUCH &&
			len3(b->vel) < SMK_SLEEP_SPEED && len3(b->w) < SMK_SLEEP_SPEED) {
		b->awake = false;
		memset(b->P, 0, sizeof(b->P));
		memset(b->L, 0, sizeof(b->L));
		velocities(b);
	}
}

// CheckTimers: one a touch converted without waking it turns back into a
// virgin 2 s on; one with a lifetime goes when it is up. True if it went.
static bool checkTimers(Smokeables* s, int k) {
	SmkProp* p = &s->prop[k];
	if (p->woke < 0.0f) {
		if (!p->touching && s->clock - p->born > SMK_REVERT) {
			char m[120];
			snprintf(m, sizeof(m), "smokeable: %s #%u back to a virgin, never woken (slot %d)",
				s->info[s->sp[p->spawner].info].name, (unsigned)s->sp[p->spawner].id, k);
			say(s, m);
			drop(s, k, true);
			s->st.reverted++;
			return true;
		}
		return false;
	}
	const float life = s->info[s->sp[p->spawner].info].lifetime;
	if (life != 0.0f && s->clock - p->woke >= life) {
		drop(s, k, false);
		s->st.killed++;
		return true;
	}
	return false;
}

static int countFree(const Smokeables* s) {
	int n = 0;
	for (int k = 0; k < SMK_POOL; k++)
		n += !s->prop[k].used;
	return n;
}

// DeleteFarAwaySmokeableCollisionBodies. The periodic pass first drops an
// awake prop the ground has come up through, or that is off the topology
// (DeleteFarAwayUseElevation is 1). Then, with the pool running short, a
// sleeping one goes when it is beyond 300 m of the player (20 m when nearly
// full), or beyond 120 m and woken more than 15 s ago.
static void deleteFarAway(Smokeables* s, const World* w, const float player[3], bool critical) {
	if (!critical) {
		for (int k = 0; k < SMK_POOL; k++) {
			SmkProp* p = &s->prop[k];
			if (!p->used || !p->b.awake)
				continue;
			float gz, gn[3];
			const float* c = p->b.pos;
			if (probe(w, c[0], c[1], c[2] + p->radius + SMK_PROBE_ABOVE, &gz, gn) &&
					gz <= c[2] + p->radius)
				continue;
			drop(s, k, false);
			s->st.killed++;
		}
	}
	const int nFree = countFree(s);
	if (nFree >= SMK_FREE_LOW)
		return;
	const float far = nFree < SMK_FREE_CRITICAL ? SMK_FAR_CRITICAL : SMK_FAR;
	for (int k = 0; k < SMK_POOL; k++) {
		SmkProp* p = &s->prop[k];
		if (!p->used || p->b.awake)
			continue;
		float d[3];
		for (int a = 0; a < 3; a++) d[a] = p->b.pos[a] - player[a];
		const float dist = len3(d);
		if (dist <= far) {
			const float age = p->woke >= 0.0f ? s->clock - p->woke : 0.0f;
			if (age <= SMK_OLD || dist <= SMK_NEAR)
				continue;
		}
		drop(s, k, false);
		s->st.killed++;
	}
}

void smokeable_update(Smokeables* s, World* w, const float player[3], float dt) {
	if (!s || !w)
		return;
	s->clock += dt;
	s->frame++;
	// A prop whose section went out goes with it (PORT: its drawing is the
	// section's); one that never woke is a virgin again when it comes back.
	for (int k = 0; k < SMK_POOL; k++) {
		SmkProp* p = &s->prop[k];
		if (!p->used)
			continue;
		SmkSpawner* sp = &s->sp[p->spawner];
		if (world_section_serial(w, sp->section) != sp->serial) {
			// Its model went with the old load: forget it before drop()
			// would show the instance again through it (a write into freed
			// memory). syncSpawners() below finds it in the new load.
			sp->found = sp->hidden = false;
			sp->model = NULL;
			drop(s, k, p->woke < 0.0f);
		}
	}
	syncSpawners(s, w);
	for (int k = 0; k < SMK_POOL; k++) {
		SmkProp* p = &s->prop[k];
		if (!p->used)
			continue;
		if (p->b.awake) {
			move(s, w, p, dt);
			serviceEmitters(s, p);
		}
		if (p->b.awake && p->b.anchor == 0.0f && p->lift > 0.0f) {
			p->lift -= dt * SMK_LIFT_SHRINK;
			if (p->lift < 0.0f) p->lift = 0.0f;
		}
		if (!checkTimers(s, k))
			p->touching = false;
	}
	// SmokeableThink.
	if (countFree(s) < SMK_FREE_CRITICAL)
		deleteFarAway(s, w, player, true);
	else if (((s->frame + SMK_THINK_PHASE) & SMK_THINK_MASK) == 0)
		deleteFarAway(s, w, player, false);
}

// -------------------------------------------------------------- the contacts

void smokeable_car(Smokeables* s, World* w, CarSim* car, float dt) {
	if (!s || !w || !car)
		return;
	float c[2], reach, zLo, zHi;
	carsim_body_extent(car, c, &reach, &zLo, &zHi);
	// The body's plan rectangle swept over the frame -- where it is and where
	// it was -- as one rectangle along its own axes, and the box round that.
	const float* v = car->rbVel;
	float pc[2], ax[2][2], half[2];
	carsim_plan_box(car, pc, ax, half);
	const float mid[2] = { -v[0] * dt * 0.5f, -v[1] * dt * 0.5f };
	for (int k = 0; k < 2; k++) {
		pc[k] += mid[k];
		half[k] += fabsf(mid[0] * ax[k][0] + mid[1] * ax[k][1]);
	}
	const float ex = half[0] * fabsf(ax[0][0]) + half[1] * fabsf(ax[1][0]);
	const float ey = half[0] * fabsf(ax[0][1]) + half[1] * fabsf(ax[1][1]);
	float lo[3] = { pc[0] - ex, pc[1] - ey, zLo }, hi[3] = { pc[0] + ex, pc[1] + ey, zHi };
	if (v[2] > 0.0f) lo[2] -= v[2] * dt;
	else hi[2] -= v[2] * dt;
	// The virgins it touched (a VirginSmokeable is a static box; the first
	// contact converts it): the boxes' axes first, then the car's.
	const int cx0 = (int)floorf((lo[0] - s->gx0) / SMK_CELL), cx1 = (int)floorf((hi[0] - s->gx0) / SMK_CELL);
	const int cy0 = (int)floorf((lo[1] - s->gy0) / SMK_CELL), cy1 = (int)floorf((hi[1] - s->gy0) / SMK_CELL);
	for (int cy = cy0 < 0 ? 0 : cy0; cy <= cy1 && cy < s->gny; cy++)
		for (int cx = cx0 < 0 ? 0 : cx0; cx <= cx1 && cx < s->gnx; cx++) {
			const int cell = cy * s->gnx + cx;
			for (int e = s->cellStart[cell]; e < s->cellStart[cell + 1]; e++) {
				const int i = s->cellList[e];
				const SmkSpawner* sp = &s->sp[i];
				if (sp->disabled || !sp->found)
					continue;
				if (sp->hi[0] < lo[0] || sp->lo[0] > hi[0] || sp->hi[1] < lo[1] ||
						sp->lo[1] > hi[1] || sp->hi[2] < lo[2] || sp->lo[2] > hi[2])
					continue;
				const float bc[2] = { (sp->lo[0] + sp->hi[0]) * 0.5f - pc[0],
					(sp->lo[1] + sp->hi[1]) * 0.5f - pc[1] };
				const float bh[2] = { (sp->hi[0] - sp->lo[0]) * 0.5f, (sp->hi[1] - sp->lo[1]) * 0.5f };
				bool apart = false;
				for (int k = 0; k < 2 && !apart; k++)
					apart = fabsf(bc[0] * ax[k][0] + bc[1] * ax[k][1]) >
						half[k] + bh[0] * fabsf(ax[k][0]) + bh[1] * fabsf(ax[k][1]);
				if (!apart)
					convert(s, w, i);
			}
		}
	// Then every loose prop within reach.
	for (int k = 0; k < SMK_POOL; k++) {
		SmkProp* p = &s->prop[k];
		if (!p->used)
			continue;
		const float dx = p->b.pos[0] - c[0], dy = p->b.pos[1] - c[1];
		const float vx = v[0] - p->b.vel[0], vy = v[1] - p->b.vel[1];
		const float r = reach + p->radius + sqrtf(vx * vx + vy * vy) * dt + 0.5f;
		if (dx * dx + dy * dy > r * r)
			continue;
		const bool wasAwake = p->b.awake;
		if (!carsim_prop_contact(car, &p->b, dt))
			continue;
		p->touching = true;
		p->colliding = true;
		p->lastCar = s->clock;
		// ConsiderImpulse's order: Awaken, then ActivateAnchorEmitters.
		if (p->b.woke)
			awakenEmitters(s, p);
		if (p->b.anchorGave) {
			p->b.anchorGave = false;
			anchorEmitters(s, p);
		}
		if (p->b.woke) {
			p->b.woke = false;
			if (!wasAwake && p->woke < 0.0f) {
				// Awaken: the time it woke, for CheckTimers and DeleteFarAway.
				p->woke = s->clock;
				p->touchSince = s->clock;
				s->st.woken++;
				char m[160];
				snprintf(m, sizeof(m), "smokeable: %s #%u knocked loose at %.1f km/h (slot %d)",
					s->info[s->sp[p->spawner].info].name, (unsigned)s->sp[p->spawner].id,
					carsim_speed_kmh(car), k);
				say(s, m);
			}
		}
	}
}

// ----------------------------------------------------------------- the draw

void smokeable_draw(const World* w, const C3D_Mtx* vp, float ambient, float gain,
		float invGamma, void* ud) {
	const Smokeables* s = (const Smokeables*)ud;
	if (!s)
		return;
	for (int k = 0; k < SMK_POOL; k++) {
		const SmkProp* p = &s->prop[k];
		if (!p->used || !p->mask)
			continue;
		const SmkSpawner* sp = &s->sp[p->spawner];
		if (!sp->found)
			continue;
		// The section may have paged out (or in again) since the last
		// smokeable_update() looked: a race restart's jump to the grid pages
		// in the same frame it draws. sp->model is then freed; the update
		// drops the prop next frame. (The 2026-11-05 tester crash: a data
		// abort in model3d_draw_instance reading a freed model's insts.)
		if (world_section_serial(w, sp->section) != sp->serial)
			continue;
		const CarSimProp* b = &p->b;
		// A turns the baked axes onto the body's: A = R1^T R0 over the rows.
		float A[3][3], o1[3], t[3];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				A[i][j] = b->rot[0][i] * p->rot0[0][j] + b->rot[1][i] * p->rot0[1][j] +
					b->rot[2][i] * p->rot0[2][j];
		// Where the model's origin is now, lowered by the lift still left.
		for (int a = 0; a < 3; a++)
			o1[a] = b->pos[a] - b->cg[0] * b->rot[0][a] - b->cg[1] * b->rot[1][a] -
				b->cg[2] * b->rot[2][a];
		o1[2] -= p->lift;
		for (int i = 0; i < 3; i++)
			t[i] = o1[i] - (A[i][0] * sp->pos[0] + A[i][1] * sp->pos[1] + A[i][2] * sp->pos[2]);
		C3D_Mtx D, mvp;
		Mtx_Identity(&D);
		for (int i = 0; i < 3; i++) {
			D.r[i].x = A[i][0];
			D.r[i].y = A[i][1];
			D.r[i].z = A[i][2];
			D.r[i].w = t[i];
		}
		Mtx_Multiply(&mvp, vp, &D);
		model3d_draw_instance(sp->model, sp->inst, p->mask, o1, &mvp, ambient, gain, invGamma);
	}
}

void smokeable_stats(const Smokeables* s, SmokeableStats* out) {
	memset(out, 0, sizeof(*out));
	if (!s)
		return;
	*out = s->st;
	out->active = SMK_POOL - countFree(s);
}
