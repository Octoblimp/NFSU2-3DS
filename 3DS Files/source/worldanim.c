// The world animations (TASKS.md #239). See worldanim.h; the spec, with every
// address, is tools/re/wip239/NOTES.md.
#include "worldanim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model3d.h"

// WORLD.NFSW (tools/world_anim.py).
#define NFSW_MAGIC   0x4157464Eu   // "NFWA"
#define NFSW_VERSION 4u   // 3 still read: no names / ids, so no worldanim_position;
                          // 2 too: no zones table either, so no doors
#define NFSW_HDR     32u
#define NFSW_MAX     (1u << 20)

typedef struct {
	float dummy[3];   // where WANIM baked it, identity rotation
	u32   hash;
	float lo[3], hi[3];   // its box, model space
} WaModelRec;

typedef struct {
	u16 first, count;
} WaTreeRec;

typedef struct {
	s16   model;      // -1: draws nothing (a tree's root)
	s8    parent;     // -1: the root
	u8    flags;      // WA_ENT_NLERP
	u16   frames;     // keys, one a frame; 0: no anim, the ctrl was deleted
	u16   length;     // the ctrl's +0x60
	u32   firstKey;
	float local[12];  // the record's +30: the node's pose before its first update
} WaEntRec;

typedef struct {
	u16   tree;
	u16   scenario;   // inst +3c: nonzero, the FwdRev scenario plays it
	u32   flags;      // inst +0c (0 already made 0x28)
	float speed;      // +10 (0 already made 1)
	float startDelay, loopDelay;   // +14, +18
	float world[12];  // +40: the root node's local
	float bbmin[3], bbmax[3];      // +80, +90: the view gate's box
	u32   raceGate;   // +38: the two track ranges it is made for (raceGateOpen)
} WaInstRec;

_Static_assert(sizeof(WaModelRec) == 40, "NFSW model record");
_Static_assert(sizeof(WaEntRec) == 60, "NFSW entity record");
_Static_assert(sizeof(WaInstRec) == 96, "NFSW instance record");

// CWorldAnimCtrl's time state (0x74 bytes in retail; these are the fields
// AdvanceAnimTime reads and writes).
typedef struct {
	float t;          // +0x64, frames
	float acc0;       // +0x14, start delay so far (s)
	float acc1;       // +0x18, loop delay so far (s)
	u32   flags;      // +0x34
	float mod;        // +0x40, the speed modifier: 1 but for a door's NIS move
	u8    state;      // +0x10: 1 playing, 2 stopped
	u8    dir;        // +0x08: the ping-pong leg
} WaCtrl;

// One of the models in WANIM, found by its dummy translation.
typedef struct {
	Model3D* model;
	u32      serial;   // its part's load, as world_anim_part() gave it
	int      part;
	u32      inst;
	u8*      mask;
	bool     found;
} WaModelRt;

struct WorldAnims {
	u8* blob;
	const WaModelRec* models;
	const WaTreeRec*  trees;
	const WaEntRec*   ents;
	const WaInstRec*  insts;
	const float*      keys;   // 10 a key: S[3], Q[4], T[3]
	const u32*        zones;  // per instance (v3): its trigger zone, 0 none; NULL in v2
	const u32*        names;  // per entity (v4): its node-name hash; NULL before v4
	const u32*        ids;    // per instance (v4): WorldAnimInstance +0; NULL before v4
	int nModels, nTrees, nEnts, nInsts;
	u32 nKeys;
	// Per instance x entity of its tree, from instBase.
	u32*     instBase;
	u8*      live;      // per instance: past the race gate
	WaCtrl*  ctrl;
	float  (*local)[12];
	float  (*world)[12];
	int      slots;
	WaModelRt* mr;
	u32*     partSerial;   // the serials the lookup was made against
	int      parts;
	bool     haveVp;
	bool     forcePose;   // a shop NIS or its door timeout: every door posed
	int      doors;
	C3D_Mtx  vp;
	int      drawn, posed, found, drawable;
	void   (*log)(const char* msg);
};

// out = a * b over row vectors (v' = v * a * b): the child's local times its
// parent's world. `out` may not be either input.
static void mul12(float out[12], const float a[12], const float b[12]) {
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			out[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
	for (int j = 0; j < 3; j++)
		out[9 + j] = a[9] * b[j] + a[10] * b[3 + j] + a[11] * b[6 + j] + b[9 + j];
}

// BuildSQT (Speed.elf 0x802a7078): each axis row is the quaternion's row
// times its S; T goes in unscaled. PoseSQTToGlobal then multiplies column 0
// by the pose's fourth S float, which is 1 for every world anim.
static void buildSQT(float m[12], const float k[10]) {
	const float sx = k[0], sy = k[1], sz = k[2];
	const float x = k[3], y = k[4], z = k[5], w = k[6];
	const float x2 = x + x, y2 = y + y, z2 = z + z;
	m[0] = sx * (1.0f - (y * y2 + z * z2));
	m[1] = sx * (x * y2 + w * z2);
	m[2] = sx * (x * z2 - w * y2);
	m[3] = sy * (x * y2 - w * z2);
	m[4] = sy * (1.0f - (x * x2 + z * z2));
	m[5] = sy * (y * z2 + w * x2);
	m[6] = sz * (x * z2 + w * y2);
	m[7] = sz * (y * z2 - w * x2);
	m[8] = sz * (1.0f - (x * x2 + y * y2));
	m[9] = k[7];
	m[10] = k[8];
	m[11] = k[9];
}

// AdvanceAnimTime (Speed.elf 0x800557e4, SPEED2.EXE 0x435140), with no loop
// range (0x40; world_anim.py refuses a track that has one). The speed is the
// instance's times the ctrl's modifier +0x40, 1 but for a door a shop NIS
// moves (#408). K is 30.0 (0x80319cd0): the time is in raw frames at 30 a
// second times the speed.
static void advance(WaCtrl* c, const WaInstRec* in, float len, float dt) {
	const float K = 30.0f;
	const float S = in->speed * c->mod;
	const float step = dt * K * S;
	const float d0 = in->startDelay * K, d1 = in->loopDelay * K;
	const float a0 = c->acc0 * K * S, a1 = c->acc1 * K * S;
	const float start = 0.0f, end = len;
	u32 f = c->flags;
	const bool stopAtEnd = (f & 0x800) != 0;
	const bool repeat = (f & 0x20) && !stopAtEnd;
	bool loop = (f & 0x08) != 0, pingpong = (f & 0x10) != 0;
	// Neither: loop. Both: the loop wins.
	if (!loop && !pingpong) {
		f |= 0x08;
		loop = true;
		c->flags = f;
	} else if (loop && pingpong) {
		f &= ~0x10u;
		pingpong = false;
		c->flags = f;
	}
	float t = c->t;
	const bool startDelaying = (f & 0x80) && a0 < d0 && t < end;
	const bool loopDelaying = (f & 0x100) && a1 < d1 && t < end;
	if (c->state != 1)
		return;
	if (startDelaying) {
		const float n = a0 + step;
		if (d0 < n)
			t += fmodf(n, d0);
		c->acc0 = n / S / K;
	} else if (loopDelaying) {
		const float n = a1 + step;
		if (d1 < n)
			t += fmodf(n, d1);
		c->acc1 = n / S / K;
	} else if (loop) {
		if (!(f & 0x1000)) {
			t += step;
			if (t > end) {
				if (repeat) {
					t -= end - start;
					c->acc1 = 0.0f;
				} else {
					t = end;
				}
			}
		} else {
			t -= step;
			if (t < start) {
				if (repeat) {
					t += end - start;
					c->acc1 = 0.0f;
				} else {
					t = start;
				}
			}
		}
	} else if (pingpong) {
		if (c->dir == 0) {
			t += step;
			if (t > end) {
				if (stopAtEnd)
					c->state = 2;
				c->dir = 1;
				t = end - (t - end);
			}
		} else {
			t -= step;
			if (t < start) {
				if (stopAtEnd)
					c->state = 2;
				c->dir = 0;
				c->acc1 = 0.0f;
				t = start;
			}
		}
	}
	c->t = t;
}

// Entity flag: its quaternion is a raw pose channel's (EAGL type 0, L4RB's
// drag trams), whose QuatF4Interp (Speed.elf 0x80314E8C) lerps to the nearer
// sign of the next key and normalises.
#define WA_ENT_NLERP 1u

// The EAGL channels at time `t` (UpdateAnimPose's EvalSQT): the integer frame
// clamped to the keys, lerped to the next by the fraction -- the quaternion
// too, unnormalised, as the delta channel (0xB) does it, or as QuatF4Interp
// does it for WA_ENT_NLERP.
static void evalPose(float m[12], const float* keys, u32 frames, float t, bool nlerp) {
	if (!(t > 0.0f))
		t = 0.0f;
	u32 k = (u32)t;
	float fr = t - (float)k;
	if (k + 1 >= frames) {
		k = frames - 1;
		fr = 0.0f;
	}
	const float* a = keys + k * 10;
	float v[10];
	if (fr > 0.0f) {
		const float* b = a + 10;
		for (int i = 0; i < 10; i++)
			v[i] = a[i] + (b[i] - a[i]) * fr;
		if (nlerp) {
			const float dot = a[3] * b[3] + a[4] * b[4] + a[5] * b[5] + a[6] * b[6];
			float n = 0.0f;
			for (int i = 3; i < 7; i++) {
				if (dot <= 0.0f)
					v[i] = a[i] - fr * (b[i] + a[i]);
				n += v[i] * v[i];
			}
			n = n > 0.0f ? 1.0f / sqrtf(n) : 1.0f;
			for (int i = 3; i < 7; i++)
				v[i] *= n;
		}
	} else {
		memcpy(v, a, sizeof(v));
	}
	buildSQT(m, v);
}

// Whether any of the box's corners could be on screen: false when all eight
// are outside one side plane, or behind the eye. `m`, if given, carries the
// box from model space (row vectors) first.
static bool boxInView(const C3D_Mtx* vp, const float lo[3], const float hi[3],
		const float* m) {
	u32 outAll = 0x1F;
	for (int c = 0; c < 8; c++) {
		float p[3] = { (c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1],
			(c & 4) ? hi[2] : lo[2] };
		if (m) {
			const float q[3] = {
				p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9],
				p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10],
				p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11] };
			memcpy(p, q, sizeof(p));
		}
		float cl[4];
		for (int r = 0; r < 4; r++)
			cl[r] = vp->r[r].x * p[0] + vp->r[r].y * p[1] + vp->r[r].z * p[2] + vp->r[r].w;
		u32 o = 0;
		if (cl[0] < -cl[3]) o |= 1;
		if (cl[0] > cl[3])  o |= 2;
		if (cl[1] < -cl[3]) o |= 4;
		if (cl[1] > cl[3])  o |= 8;
		if (cl[3] <= 0.0f)  o |= 16;
		outAll &= o;
		if (!outAll)
			return true;
	}
	return false;
}

static void dropModels(WorldAnims* a) {
	for (int k = 0; k < a->nModels; k++) {
		free(a->mr[k].mask);
		memset(&a->mr[k], 0, sizeof(a->mr[k]));
	}
}

// Finds every model in WANIM again whenever one of its parts' loads changed.
static void findModels(WorldAnims* a, const World* w) {
	const int parts = world_anim_parts(w);
	bool same = parts == a->parts;
	u32 serials[8] = { 0 };
	Model3D* models[8] = { NULL };
	const int np = parts < 8 ? parts : 8;
	for (int p = 0; p < np; p++) {
		models[p] = world_anim_part(w, p, &serials[p]);
		if (serials[p] != a->partSerial[p])
			same = false;
	}
	if (same)
		return;
	dropModels(a);
	a->parts = parts;
	a->found = 0;
	for (int p = 0; p < 8; p++)
		a->partSerial[p] = p < np ? serials[p] : 0;
	for (int k = 0; k < a->nModels; k++) {
		WaModelRt* r = &a->mr[k];
		for (int p = 0; p < np && !r->found; p++) {
			u32 inst;
			if (!models[p] || !model3d_find_instance(models[p], a->models[k].dummy, &inst))
				continue;
			const int nb = model3d_batch_count(models[p]);
			r->mask = (u8*)malloc(((size_t)nb + 7) / 8);
			if (!r->mask || !model3d_instance_batches(models[p], inst, r->mask)) {
				free(r->mask);
				r->mask = NULL;
				continue;
			}
			r->model = models[p];
			r->serial = serials[p];
			r->part = p;
			r->inst = inst;
			r->found = true;
			a->found++;
		}
	}
	if (a->log && np) {
		char b[160];
		snprintf(b, sizeof(b), "worldanim: %d of %d models found in WANIM (%d part(s))",
			a->found, a->nModels, np);
		a->log(b);
	}
}

// InstantiateAnimTree's (Speed.elf 0x80049b4c) race gate: an instance with
// +38 set is made only when TheRaceParameters' track % 100 is in [b1, b0] or
// in [b3, b2] (b0 the low byte). L4RB's four 0x141d trees are 4x20..4x29's and
// free roam's (x00).
static bool raceGateOpen(u32 g, int track) {
	if (!g)
		return true;
	const int r = track % 100;
	if (r < (int)((g >> 8) & 0xff) || (int)(g & 0xff) < r) {
		if (r < (int)(g >> 24))
			return false;
		if ((int)((g >> 16) & 0xff) < r)
			return false;
	}
	return true;
}

WorldAnims* worldanim_open(const char* path, int track, bool reversed,
		void (*log)(const char* msg)) {
	char b[600];
	FILE* f = fopen(path, "rb");
	if (!f) {
		if (log) {
			snprintf(b, sizeof(b), "worldanim: %s missing (run tools/pack_world.py) -- "
				"no jets, trains, cranes or spotlights move", path);
			log(b);
		}
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* blob = (size >= (long)NFSW_HDR && (u32)size <= NFSW_MAX) ? (u8*)malloc((size_t)size) : NULL;
	const bool read = blob && fread(blob, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	u32 h[7] = { 0 };
	if (read)
		memcpy(h, blob, sizeof(h));
	const bool v4 = h[1] == NFSW_VERSION;
	const bool v3 = v4 || h[1] == 3u;
	const u64 need = NFSW_HDR + (u64)h[2] * sizeof(WaModelRec) + (u64)h[3] * sizeof(WaTreeRec) +
		(u64)h[4] * sizeof(WaEntRec) + (u64)h[5] * sizeof(WaInstRec) + (u64)h[6] * 40u +
		(v3 ? (u64)h[5] * 4u : 0u) + (v4 ? ((u64)h[4] + h[5]) * 4u : 0u);
	bool ok = read && h[0] == NFSW_MAGIC && (v3 || h[1] == 2u) && h[2] && h[2] < 0x8000 &&
		h[3] && h[3] < 0x8000 && h[4] && h[4] < 0x8000 && h[5] && h[5] < 0x8000 &&
		need == (u64)size;
	WorldAnims* a = ok ? (WorldAnims*)calloc(1, sizeof(WorldAnims)) : NULL;
	if (a) {
		a->blob = blob;
		a->log = log;
		a->nModels = (int)h[2];
		a->nTrees = (int)h[3];
		a->nEnts = (int)h[4];
		a->nInsts = (int)h[5];
		a->nKeys = h[6];
		u8* p = blob + NFSW_HDR;
		a->models = (const WaModelRec*)p;  p += a->nModels * sizeof(WaModelRec);
		a->trees = (const WaTreeRec*)p;    p += a->nTrees * sizeof(WaTreeRec);
		a->ents = (const WaEntRec*)p;      p += a->nEnts * sizeof(WaEntRec);
		a->insts = (const WaInstRec*)p;    p += a->nInsts * sizeof(WaInstRec);
		a->keys = (const float*)p;         p += (size_t)a->nKeys * 40u;
		a->zones = v3 ? (const u32*)p : NULL;
		if (v4) {
			p += a->nInsts * 4u;
			a->names = (const u32*)p;
			a->ids = (const u32*)(p + a->nEnts * 4u);
		}
		// Every reference in range, so the per-frame code needs no checks.
		for (int i = 0; ok && i < a->nTrees; i++)
			ok = a->trees[i].count && (u32)a->trees[i].first + a->trees[i].count <= (u32)a->nEnts;
		for (int e = 0; ok && e < a->nEnts; e++) {
			const WaEntRec* r = &a->ents[e];
			ok = r->model < a->nModels &&
				(!r->frames || (u64)r->firstKey + r->frames <= a->nKeys);
		}
		for (int i = 0; ok && i < a->nInsts; i++) {
			ok = a->insts[i].tree < a->nTrees && a->insts[i].speed > 0.0f;
			const WaTreeRec* t = ok ? &a->trees[a->insts[i].tree] : NULL;
			for (int e = 0; ok && e < t->count; e++) {
				const s8 par = a->ents[t->first + e].parent;
				ok = e == 0 ? par < 0 : (par >= 0 && par < e);
			}
		}
		if (ok) {
			a->instBase = (u32*)calloc((size_t)a->nInsts, sizeof(u32));
			a->live = (u8*)calloc((size_t)a->nInsts, 1);
			for (int i = 0; a->instBase && i < a->nInsts; i++) {
				a->instBase[i] = (u32)a->slots;
				a->slots += a->trees[a->insts[i].tree].count;
			}
			a->ctrl = (WaCtrl*)calloc((size_t)a->slots, sizeof(WaCtrl));
			a->local = (float(*)[12])calloc((size_t)a->slots, sizeof(*a->local));
			a->world = (float(*)[12])calloc((size_t)a->slots, sizeof(*a->world));
			a->mr = (WaModelRt*)calloc((size_t)a->nModels, sizeof(WaModelRt));
			a->partSerial = (u32*)calloc(8, sizeof(u32));
			ok = a->instBase && a->live && a->ctrl && a->local && a->world && a->mr &&
				a->partSerial;
		}
	}
	if (!a || !ok) {
		if (log) {
			snprintf(b, sizeof(b), "worldanim: %s %s", path,
				!read ? "unreadable" : !a && ok ? "out of memory" : "malformed or the wrong version");
			log(b);
		}
		if (a)
			worldanim_close(a);
		else
			free(blob);
		return NULL;
	}
	// InstantiateAnimTree + CWorldAnimEntity::Init: the root's local is the
	// instance's matrix, every other node starts at its record's; each ctrl
	// ORs 0x80 | 0x100 into the instance's flags and, with no trigger zone,
	// plays at once (state 1). PORT: the 0x200 / 0x400 "random" delays and the
	// loop range never occur in this data (world_anim.py checks).
	//
	// A tree with +3c set does not play at Init but on StartWorldAnimations'
	// (0x80049fc8) FwdRev message, the same frame: FwdRevTrackControlScenario's
	// handler (0x80057060) Plays each ctrl, first, when the race runs reversed
	// (TheRaceParameters +4, eTrackDirection, 1), setting the reverse flag
	// 0x1000 and the time to the end (+0x64 = +0x60).
	//
	// A door (a trigger zone, #408) does not play at Init: CWorldAnimEntity::
	// Init gives a zoned instance's ctrls ping-pong with stop-at-end (0x810,
	// the loop bit 8 cleared), stopped, so each Play a shop NIS's
	// DOOR_POSITION gives it runs one leg -- open, then close. Its pose starts
	// at frame 0, where g_aNonAnimatedDoors' copies stand in retail.
	int gated = 0, rev = 0;
	for (int i = 0; i < a->nInsts; i++) {
		const WaInstRec* in = &a->insts[i];
		const WaTreeRec* t = &a->trees[in->tree];
		const bool door = a->zones && a->zones[i];
		a->doors += door;
		a->live[i] = raceGateOpen(in->raceGate, track);
		if (!a->live[i]) {
			gated++;
			continue;
		}
		const bool back = in->scenario && reversed;
		rev += back;
		for (int e = 0; e < t->count; e++) {
			const u32 s = a->instBase[i] + (u32)e;
			const WaEntRec* r = &a->ents[t->first + e];
			if (e == 0)
				memcpy(a->local[s], in->world, sizeof(a->local[s]));
			else
				memcpy(a->local[s], r->local, sizeof(a->local[s]));
			a->ctrl[s].flags = in->flags | 0x80 | 0x100;
			a->ctrl[s].state = 1;
			a->ctrl[s].mod = 1.0f;
			if (door) {
				a->ctrl[s].flags = (in->flags & ~0x08u) | 0x810 | 0x80 | 0x100;
				a->ctrl[s].state = 0;
				if (r->frames)
					evalPose(a->local[s], a->keys + (size_t)r->firstKey * 10, r->frames, 0.0f,
						(r->flags & WA_ENT_NLERP) != 0);
			}
			if (back) {
				a->ctrl[s].flags |= 0x1000;
				a->ctrl[s].t = (float)r->length;
			}
			if (r->model >= 0)
				a->drawable++;
		}
		for (int e = 0; e < t->count; e++) {
			const u32 s = a->instBase[i] + (u32)e;
			const s8 par = a->ents[t->first + e].parent;
			if (par < 0)
				memcpy(a->world[s], a->local[s], sizeof(a->world[s]));
			else
				mul12(a->world[s], a->local[s], a->world[a->instBase[i] + (u32)par]);
		}
	}
	if (log) {
		snprintf(b, sizeof(b), "worldanim: %d instance(s) (%d shop door(s)%s), %d tree(s), "
			"%d model(s) drawn by %d entities, %lu pose keys; track %d%s: %d gated out, "
			"%d played reversed",
			a->nInsts, a->doors, a->zones ? "" : ", v2 file: the doors are scenery", a->nTrees,
			a->nModels, a->drawable, (unsigned long)a->nKeys, track,
			reversed ? " reversed" : "", gated, rev);
		log(b);
	}
	return a;
}

void worldanim_close(WorldAnims* a) {
	if (!a)
		return;
	if (a->mr)
		dropModels(a);
	free(a->mr);
	free(a->partSerial);
	free(a->instBase);
	free(a->live);
	free(a->ctrl);
	free(a->local);
	free(a->world);
	free(a->blob);
	free(a);
}

void worldanim_update(WorldAnims* a, const World* w, float dt) {
	if (!a || !(dt > 0.0f))
		return;
	(void)w;
	a->posed = 0;
	for (int i = 0; i < a->nInsts; i++) {
		if (!a->live[i])
			continue;
		const WaInstRec* in = &a->insts[i];
		const WaTreeRec* t = &a->trees[in->tree];
		const u32 base = a->instBase[i];
		// UpdateTimeStep (0x80048b00): the pose only for flag 0x2000, or with
		// no trigger zone the box in the view -- IsAABBCurrentlyVisible tests
		// eGetView(1), the player's -- or, for a door, while the shop NIS or
		// its timeout forces it (NotifyEnterShop / the 10 s door timeout). The
		// time always runs.
		const bool zoned = a->zones && a->zones[i];
		const bool pose = (in->flags & 0x2000) || (zoned && a->forcePose) ||
			(!zoned && a->haveVp && boxInView(&a->vp, in->bbmin, in->bbmax, NULL));
		bool moved = false;
		for (int e = 0; e < t->count; e++) {
			const WaEntRec* r = &a->ents[t->first + e];
			if (!r->frames)
				continue;   // no anim: Init deleted the ctrl
			WaCtrl* c = &a->ctrl[base + (u32)e];
			advance(c, in, (float)r->length, dt);
			if (pose) {
				evalPose(a->local[base + (u32)e], a->keys + (size_t)r->firstKey * 10,
					r->frames, c->t, (r->flags & WA_ENT_NLERP) != 0);
				moved = true;
				a->posed++;
			}
		}
		if (!moved)
			continue;
		for (int e = 1; e < t->count; e++) {
			const u32 s = base + (u32)e;
			mul12(a->world[s], a->local[s], a->world[base + (u32)a->ents[t->first + e].parent]);
		}
	}
}

void worldanim_draw(WorldAnims* a, const World* w, const C3D_Mtx* vp, float ambient,
		float gain, float invGamma) {
	if (!a)
		return;
	a->vp = *vp;
	a->haveVp = true;
	findModels(a, w);
	a->drawn = 0;
	if (!a->found)
		return;
	// The screen-height scale for the pixel-size cull: clip units per metre at
	// one metre from the eye, either screen axis, times the 120 pixels from
	// the top screen's centre to its edge.
	const float sx = sqrtf(vp->r[0].x * vp->r[0].x + vp->r[0].y * vp->r[0].y + vp->r[0].z * vp->r[0].z);
	const float sy = sqrtf(vp->r[1].x * vp->r[1].x + vp->r[1].y * vp->r[1].y + vp->r[1].z * vp->r[1].z);
	const float pixScale = 120.0f * (sx > sy ? sx : sy);
	for (int i = 0; i < a->nInsts; i++) {
		if (!a->live[i])
			continue;
		const WaTreeRec* t = &a->trees[a->insts[i].tree];
		for (int e = 0; e < t->count; e++) {
			const WaEntRec* r = &a->ents[t->first + e];
			if (r->model < 0)
				continue;
			const WaModelRt* mr = &a->mr[r->model];
			if (!mr->found)
				continue;
			// The part may have paged out (or in again) since findModels.
			u32 serial;
			if (world_anim_part(w, mr->part, &serial) != mr->model || serial != mr->serial)
				continue;
			const float* m = a->world[a->instBase[i] + (u32)e];
			// WorldModel::Render (0x8024cdf0): GetPixelSize at radius 35
			// (0x8033e5e4) under 2 pixels, then GetVisibleState on the model's
			// box. PORT: the pixel size is the clip w of the node's origin
			// against this view's scale.
			const float cw = vp->r[3].x * m[9] + vp->r[3].y * m[10] + vp->r[3].z * m[11] + vp->r[3].w;
			if (cw > 35.0f && 35.0f * pixScale / cw < 2.0f)
				continue;
			const WaModelRec* mo = &a->models[r->model];
			if (!boxInView(vp, mo->lo, mo->hi, m))
				continue;
			// D carries the model from where WANIM baked it (identity at the
			// dummy translation) to the node: D = M^T T(-dummy), column form.
			C3D_Mtx D, mvp;
			Mtx_Identity(&D);
			for (int k = 0; k < 3; k++) {
				D.r[k].x = m[k];
				D.r[k].y = m[3 + k];
				D.r[k].z = m[6 + k];
				D.r[k].w = m[9 + k] - (mo->dummy[0] * m[k] + mo->dummy[1] * m[3 + k] +
					mo->dummy[2] * m[6 + k]);
			}
			Mtx_Multiply(&mvp, vp, &D);
			model3d_draw_instance(mr->model, mr->inst, mr->mask, &m[9], &mvp, ambient, gain,
				invGamma);
			a->drawn++;
		}
	}
}

int worldanim_door(WorldAnims* a, u32 zone, float pos, float time) {
	if (!a || !a->zones || !zone)
		return 0;
	// SetAnimToNormalizedPosOverTime (Speed.elf 0x80056da8): 0x80319e00 the
	// epsilon, 0x80319e08 1.0.
	const float eps = 1e-4f;
	const bool over = fabsf(time) >= eps;
	const bool atEnd = fabsf(pos - 1.0f) < eps;
	int n = 0;
	for (int i = 0; i < a->nInsts; i++) {
		if (!a->live[i] || a->zones[i] != zone)
			continue;
		const WaTreeRec* t = &a->trees[a->insts[i].tree];
		for (int e = 0; e < t->count; e++) {
			const WaEntRec* r = &a->ents[t->first + e];
			if (!r->frames)
				continue;   // no ctrl
			WaCtrl* c = &a->ctrl[a->instBase[i] + (u32)e];
			if (over) {
				// ApplySpeedModifier(length / time), then Play: the next leg
				// takes `time` frames.
				c->mod = (float)r->length / time;
				c->state = 1;
			} else if (atEnd) {
				// JumpToEndForTrigger (0x80055680)
				c->state = 2;
				c->dir = 1;
				c->t = (float)r->length;
			} else {
				// JumpToBeginForTrigger (0x8005569c)
				c->state = 2;
				c->dir = 0;
				c->t = 0.0f;
			}
		}
		n++;
	}
	return n;
}

bool worldanim_position(const WorldAnims* a, u32 id, u32 nameHash, float pos[3],
		bool* delaying) {
	if (delaying)
		*delaying = false;
	if (!a || !a->ids)
		return false;
	// GetAnimTree (0x800499BC): the scene's first tree made with this id --
	// one the race gate kept out was never made.
	int i = 0;
	while (i < a->nInsts && !(a->live[i] && a->ids[i] == id))
		i++;
	if (i == a->nInsts)
		return false;
	// The root, then its children in order, by GetInstanceNameHash.
	const WaTreeRec* t = &a->trees[a->insts[i].tree];
	int e = 0;
	while (e < t->count && a->names[t->first + e] != nameHash)
		e++;
	if (e == t->count)
		return false;
	const u32 s = a->instBase[i] + (u32)e;
	pos[0] = a->world[s][9];
	pos[1] = a->world[s][10];
	pos[2] = a->world[s][11];
	// IsCurrentlyDelayingBeforeWorldStart (0x800556C4) / BetweenLoops
	// (0x80055754) of the entity's ctrl, the tests advance() makes; an entity
	// with no anim has none.
	const WaEntRec* r = &a->ents[t->first + e];
	if (delaying && r->frames) {
		const WaCtrl* c = &a->ctrl[s];
		const WaInstRec* in = &a->insts[i];
		const float K = 30.0f, S = in->speed * c->mod;
		const bool before = (c->flags & 0x80) && c->acc0 * K * S < in->startDelay * K;
		const bool between = (c->flags & 0x100) && c->acc1 * K * S < in->loopDelay * K;
		*delaying = (before || between) && c->t < (float)r->length;
	}
	return true;
}

void worldanim_force_pose(WorldAnims* a, bool on) {
	if (a)
		a->forcePose = on;
}

void worldanim_stats(const WorldAnims* a, WorldAnimStats* out) {
	memset(out, 0, sizeof(*out));
	if (!a)
		return;
	out->instances = a->nInsts;
	out->drawable = a->drawable;
	out->models = a->nModels;
	out->found = a->found;
	out->drawn = a->drawn;
	out->posed = a->posed;
}
