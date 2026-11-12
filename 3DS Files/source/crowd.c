// The race-end crowd NIS's people, TASKS.md #354. See crowd.h.
#include "crowd.h"
#include "linarena.h"
#include "model3d.h"
#include "texpack.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A skinned vertex: pos[3], normal[3]. The uv never changes, so it is one
// stream per model (model3d_set_streams()) -- 24 bytes an entity's vertex, not
// model3d's interleaved 32: EndingNis01's 28 people fit the world's linear
// memory that way (the 32-byte pools wanted ~464 KB of the 418 KB free).
#define CROWD_VSIZE 24
#define CROWD_VFLOATS 6
#define MODEL_VSIZE 32   // model3d's vertex: pos[3], normal[3], uv[2]

typedef struct { float q[4], t[3]; } CrowdKey;

typedef struct {
	int parent;
	float ib[4][3];      // the inverse bind, affine part (column 3 is 0,0,0,1)
	CrowdKey bind;
} CrowdBone;

typedef struct {
	uint32_t hash;
	int nBones;
	CrowdBone* bone;
} CrowdSkel;

typedef struct {
	uint32_t hash;
	int nBones, nKeys;
	const CrowdKey* key; // nKeys x nBones, into the file buffer
} CrowdAnim;

typedef struct {
	uint8_t b[4];
	float w[3];
} CrowdSkin;

typedef struct {
	uint32_t hash;
	int skel, nVerts;
	Model3D* m;
	float* uv;           // linear, nVerts x 2 floats: m's pool, cut down to the uv
	float* bind;         // nVerts x CROWD_VFLOATS, the bind pose
	float radius;        // the bind pose's reach from the origin, for culling
	const CrowdSkin* skin;
} CrowdModel;

typedef struct {
	uint32_t hash;
	int model, skel, anim;
	uint32_t flags;
	float m[4][3];
	float speed, delay;
	uint32_t loopStart, loopEnd;   // SetLoopRange's anim frames (flags & 0x40)
	void* verts;        // linearAlloc, nVerts x CROWD_VSIZE
	float posedT;        // the scene time `verts` holds, -1 none
	// The ground follow (#483). Retail's per-step follow runs for an entity
	// that names its own skeleton (entity +0x70, CAnimCtrl +0x34 bit 0); Init
	// grounds every one once.
	bool follow;
	bool grounded;       // entity +0x1C: groundZ holds an answer
	bool newPose;        // skinned since the last ground step
	float groundZ;       // entity +0x2C, the smoothed elevation
	float bone1[3];      // bone 1's position in entity space at posedT
	int flareMap;        // entity +0x28: its FlagGirlFlareMap row, -1 none (#497)
	// The ground shadow (#497, crowd_shadow_quads()).
	int shadowRow;       // entity +0x30: its BoneMap row, -1 none
	float shadowBone[2][3];   // the row's two bones in entity space at posedT
	bool shadowNow;      // drawn by the last crowd_draw(): shadowW holds them
	float shadowW[2][3];      // ... in world space
	bool shadowProbed[4];
	float shadowXY[4][2], shadowZ[4];   // the last corner probes (PORT, below)
} CrowdEnt;

typedef struct {
	int nKeys;
	const CrowdKey* key;
} CrowdCar;

struct Crowd {
	uint8_t* file;
	uint32_t fileSize;
	int nModel, nSkel, nAnim, nEnt, nCar;
	CrowdModel* model;
	CrowdSkel* skel;
	CrowdAnim* anim;
	CrowdEnt* ent;
	CrowdCar car[4];
	const void* scene;
	uint32_t sceneBytes;
	float t;             // crowd_update()'s
	int inView;          // the last crowd_draw()'s
	bool haveShadow;     // CHARACTERSHADOW (version 3)
	C3D_Tex shadowTex;
	Tex3DS_SubTexture shadowSub;
};

// BoneMap (Speed.elf 0x80353C8C, SPEED2.EXE 0x7EA1E0): per row the bones
// RenderEffects lays the shadow between, then the two flares' (#497). Init
// (0x8004FB0C) gives an animated entity row 0 on a 35-bone skeleton, row 1 on
// a 24-bone one, else none.
static const int kBoneMap[2][4] = { { 5, 9, 0x12, 0x1C }, { 5, 8, 0x0F, 0x15 } };
#define SHADOW_ALONG  0.25f   // DAT_8031966C (PC 0.5 x 0.5)
#define SHADOW_ACROSS 0.35f   // DAT_80319670 (PC 0.7 x 0.5)
#define SHADOW_LIFT   0.05f   // DAT_80319674
// PORT: a corner's ground is asked again only once it has moved this far;
// retail asks GetElevation for all four every frame.
#define SHADOW_REPROBE 0.1f

// FlagGirlFlareMap (Speed.elf 0x80353BBC, #497): per flare-carrying flag
// girl model, the two flares as bind-pose model-space points -- the lit end,
// then a point down the stick, so end minus that is the flare's axis -- the
// first on BoneMap row 0's +8 bone, the second on its +0xC bone.
#define FLARE_SKEL_BONES 0x23   // Init gives a map row only to this skeleton
#define FLARE_BONE_A     0x12   // BoneMap (0x80353C8C) row 0: 5, 9, 0x12, 0x1C
#define FLARE_BONE_B     0x1C
static const struct {
	const char* name;
	float v[4][3];
} kFlareMap[4] = {
	{ "FLAG_GIRL_MODEL_5_FLARES", { { 0.68651f, -0.0878f, 1.20303f }, { 0.59256f, 0.0478f, 1.20204f },
		{ -0.68651f, -0.0878f, 1.20303f }, { -0.59256f, 0.0478f, 1.20204f } } },
	{ "FLAG_GIRL_MODEL_7_FLARES", { { 0.6642f, -0.22838f, 1.22188f }, { 0.56942f, 0.05576f, 1.20907f },
		{ -0.6642f, -0.22838f, 1.22188f }, { -0.56942f, 0.05576f, 1.20907f } } },
	{ "FLAG_GIRL_MODEL_8_FLARES", { { 0.67353f, -0.03972f, 1.14111f }, { 0.58962f, 0.08162f, 1.20232f },
		{ -0.67353f, -0.03972f, 1.14111f }, { -0.58962f, 0.08162f, 1.20232f } } },
	{ "FLAG_GIRL_MODEL_9_FLARES", { { 0.61865f, -0.08303f, 1.03653f }, { 0.5235f, 0.07247f, 1.09304f },
		{ -0.61865f, -0.08303f, 1.03653f }, { -0.5235f, 0.07247f, 1.09304f } } },
};

static uint32_t bStringHash(const char* s) {
	uint32_t h = 0xFFFFFFFFu;
	while (*s)
		h = h * 33u + (uint8_t)*s++;
	return h;
}

typedef struct {
	const uint8_t* p;
	const uint8_t* end;
	bool bad;
} Rd;

static const void* rdTake(Rd* r, uint32_t n) {
	if (r->bad || (uint32_t)(r->end - r->p) < n) {
		r->bad = true;
		return NULL;
	}
	const void* at = r->p;
	r->p += n;
	return at;
}
static uint32_t rdU32(Rd* r) {
	const void* at = rdTake(r, 4);
	uint32_t v = 0;
	if (at)
		memcpy(&v, at, 4);
	return v;
}
static float rdF(Rd* r) {
	uint32_t u = rdU32(r);
	float f;
	memcpy(&f, &u, 4);
	return f;
}
static void rdAlign(Rd* r, const uint8_t* base) {
	uint32_t o = (uint32_t)(r->p - base);
	uint32_t pad = ((o + 3) & ~3u) - o;
	rdTake(r, pad);
}

// Linear memory for the crowd: the ordinary heap first, then the world's arena
// (linarena.h). The opponents' cars take ~3 MB each of the heap the world
// leaves, so after a race it may hold under 100 KB (2026-11-05: 87 KB, the
// EndingNis02 pools ~400 KB); the arena keeps megabytes spare. The blocks are
// untagged, so compaction leaves them where they are; endNisStop() frees them
// before worldExit() drops the arena.
static void* crowdLinear(size_t bytes) {
	void* p = linearAlloc(bytes);
	if (p || !arena_active())
		return p;
	const ArenaSide side = arena_route(ARENA_LOW);
	const u32 tag = arena_tag(0);
	p = linearAlloc(bytes);
	arena_tag(tag);
	arena_route(side);
	return p;
}
static Model3D* crowdModel(const void* nfsc, uint32_t nb) {
	Model3D* m = model3d_from_memory(nfsc, nb);
	if (m || !arena_active())
		return m;
	const ArenaSide side = arena_route(ARENA_LOW);
	const u32 tag = arena_tag(0);
	m = model3d_from_memory(nfsc, nb);
	arena_tag(tag);
	arena_route(side);
	return m;
}

static void logf_(void (*log)(const char*), const char* fmt, ...) {
	if (!log)
		return;
	char line[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	log(line);
}

void crowd_free(Crowd* c) {
	if (!c)
		return;
	if (c->model) {
		for (int i = 0; i < c->nModel; i++) {
			CrowdModel* m = &c->model[i];
			if (m->m)
				model3d_free(m->m);   // frees m->uv, its pool now
			free(m->bind);
		}
		free(c->model);
	}
	if (c->skel) {
		for (int i = 0; i < c->nSkel; i++)
			free(c->skel[i].bone);
		free(c->skel);
	}
	free(c->anim);
	if (c->ent) {
		for (int i = 0; i < c->nEnt; i++)
			if (c->ent[i].verts)
				linearFree(c->ent[i].verts);
		free(c->ent);
	}
	if (c->haveShadow)
		C3D_TexDelete(&c->shadowTex);
	free(c->file);
	free(c);
}

Crowd* crowd_load(const char* path, void (*log)(const char*)) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		logf_(log, "crowd: %s missing", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	Crowd* c = (Crowd*)calloc(1, sizeof *c);
	if (!c || size < 28) {
		fclose(f);
		free(c);
		logf_(log, "crowd: %s too small", path);
		return NULL;
	}
	c->file = (uint8_t*)malloc((size_t)size);
	c->fileSize = (uint32_t)size;
	bool ok = c->file && fread(c->file, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	if (!ok) {
		crowd_free(c);
		logf_(log, "crowd: %s unreadable", path);
		return NULL;
	}
	c->t = 0.0f;

	Rd r = { c->file, c->file + size, false };
	const char* why = "malformed";
	const uint8_t* base = c->file;
	// Version 2 (#422) adds each entity's loop start; a v1 file loops from 0.
	const uint32_t ver = memcmp(rdTake(&r, 4), "CRWD", 4) == 0 ? rdU32(&r) : 0;
	if (ver < 1 || ver > 3) {
		crowd_free(c);
		logf_(log, "crowd: %s not a v1..v3 .crowd", path);
		return NULL;
	}
	c->nModel = (int)rdU32(&r);
	c->nSkel = (int)rdU32(&r);
	c->nAnim = (int)rdU32(&r);
	c->nEnt = (int)rdU32(&r);
	c->nCar = (int)rdU32(&r);
	if (c->nModel > 16 || c->nSkel > 8 || c->nAnim > 256 || c->nEnt > 128 || c->nCar > 4) {
		crowd_free(c);
		logf_(log, "crowd: %s counts out of range", path);
		return NULL;
	}
	c->model = (CrowdModel*)calloc((size_t)c->nModel + 1, sizeof *c->model);
	c->skel = (CrowdSkel*)calloc((size_t)c->nSkel + 1, sizeof *c->skel);
	c->anim = (CrowdAnim*)calloc((size_t)c->nAnim + 1, sizeof *c->anim);
	c->ent = (CrowdEnt*)calloc((size_t)c->nEnt + 1, sizeof *c->ent);
	if (!c->model || !c->skel || !c->anim || !c->ent) {
		crowd_free(c);
		return NULL;
	}

	for (int i = 0; i < c->nModel && !r.bad; i++) {
		CrowdModel* m = &c->model[i];
		m->hash = rdU32(&r);
		m->skel = (int)rdU32(&r);
		m->nVerts = (int)rdU32(&r);
		uint32_t nb = rdU32(&r);
		const void* nfsc = rdTake(&r, nb);
		rdAlign(&r, base);
		m->skin = (const CrowdSkin*)rdTake(&r, (uint32_t)m->nVerts * sizeof(CrowdSkin));
		if (r.bad)
			break;
		m->m = crowdModel(nfsc, nb);
		if (!m->m) {
			r.bad = true; why = "its model would not build (out of memory?)";
			break;
		}
		// The bind pose to the heap and the uv to a small pool of its own; the
		// model's 32-byte pool goes back to the linear heap.
		float* home = (float*)model3d_vertex_dest(m->m);
		m->bind = (float*)malloc((size_t)m->nVerts * CROWD_VSIZE);
		m->uv = (float*)crowdLinear((size_t)m->nVerts * 8);
		if (!m->bind || !m->uv) {
			if (m->uv)
				linearFree(m->uv);
			m->uv = NULL;
			r.bad = true; why = "out of memory for a model's bind pose / uv";
			break;
		}
		for (int v = 0; v < m->nVerts; v++) {
			const float* s = home + v * (MODEL_VSIZE / 4);
			memcpy(m->bind + v * CROWD_VFLOATS, s, CROWD_VSIZE);
			const float r2 = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
			if (r2 > m->radius * m->radius)
				m->radius = sqrtf(r2);
			m->uv[2 * v] = s[6];
			m->uv[2 * v + 1] = s[7];
		}
		GSPGPU_FlushDataCache(m->uv, (u32)m->nVerts * 8);
		model3d_relocate(m->m, home, m->uv);
		linearFree(home);
	}
	for (int i = 0; i < c->nSkel && !r.bad; i++) {
		CrowdSkel* s = &c->skel[i];
		s->hash = rdU32(&r);
		s->nBones = (int)rdU32(&r);
		if (s->nBones <= 0 || s->nBones > 64) {
			r.bad = true;
			break;
		}
		s->bone = (CrowdBone*)calloc((size_t)s->nBones, sizeof *s->bone);
		if (!s->bone) {
			r.bad = true;
			break;
		}
		for (int b = 0; b < s->nBones; b++) {
			CrowdBone* bn = &s->bone[b];
			bn->parent = (int)rdU32(&r);
			float ib[16];
			for (int k = 0; k < 16; k++)
				ib[k] = rdF(&r);
			for (int row = 0; row < 4; row++)
				for (int col = 0; col < 3; col++)
					bn->ib[row][col] = ib[row * 4 + col];
			for (int k = 0; k < 4; k++)
				bn->bind.q[k] = rdF(&r);
			for (int k = 0; k < 3; k++)
				bn->bind.t[k] = rdF(&r);
			if (bn->parent >= b)
				r.bad = true;   // parents come first
		}
	}
	for (int i = 0; i < c->nAnim && !r.bad; i++) {
		CrowdAnim* a = &c->anim[i];
		a->hash = rdU32(&r);
		a->nBones = (int)rdU32(&r);
		a->nKeys = (int)rdU32(&r);
		a->key = (const CrowdKey*)rdTake(&r, (uint32_t)(a->nBones * a->nKeys) * sizeof(CrowdKey));
	}
	for (int i = 0; i < c->nEnt && !r.bad; i++) {
		CrowdEnt* e = &c->ent[i];
		e->hash = rdU32(&r);
		e->model = (int)rdU32(&r);
		e->skel = (int)rdU32(&r);
		e->follow = e->skel >= 0;
		e->anim = (int)rdU32(&r);
		e->flags = rdU32(&r);
		for (int row = 0; row < 4; row++)
			for (int col = 0; col < 3; col++)
				e->m[row][col] = rdF(&r);
		e->speed = rdF(&r);
		e->delay = rdF(&r);
		e->loopStart = ver >= 2 ? rdU32(&r) : 0;
		e->loopEnd = rdU32(&r);
		if (e->model >= c->nModel || e->skel >= c->nSkel || e->anim >= c->nAnim)
			r.bad = true;
	}
	for (int i = 0; i < c->nCar && !r.bad; i++) {
		c->car[i].nKeys = (int)rdU32(&r);
		c->car[i].key = (const CrowdKey*)rdTake(&r, (uint32_t)c->car[i].nKeys * sizeof(CrowdKey));
	}
	if (!r.bad) {
		c->sceneBytes = rdU32(&r);
		c->scene = rdTake(&r, c->sceneBytes);
	}
	// Version 3: CHARACTERSHADOW's alpha (InitCharacterEffects).
	const uint8_t* shadowA = NULL;
	uint32_t shadowW = 0, shadowH = 0;
	if (!r.bad && ver >= 3) {
		rdAlign(&r, c->file);
		shadowW = rdU32(&r);
		shadowH = rdU32(&r);
		if (shadowW > 256 || shadowH > 256)
			r.bad = true;
		else if (shadowW && shadowH)
			shadowA = (const uint8_t*)rdTake(&r, shadowW * shadowH);
	}
	if (r.bad) {
		crowd_free(c);
		logf_(log, "crowd: %s %s", path, why);
		return NULL;
	}

	// Every drawn entity gets its own pos + normal pool, the bind pose until
	// the first crowd_update(); the uv is the model's (m->uv).
	for (int i = 0; i < c->nEnt; i++) {
		CrowdEnt* e = &c->ent[i];
		if (e->model < 0)
			continue;
		const CrowdModel* m = &c->model[e->model];
		if (e->skel < 0)
			e->skel = m->skel;
		e->verts = crowdLinear((size_t)m->nVerts * CROWD_VSIZE);
		if (!e->verts) {
			crowd_free(c);
			logf_(log, "crowd: %s out of linear memory", path);
			return NULL;
		}
		memcpy(e->verts, m->bind, (size_t)m->nVerts * CROWD_VSIZE);
		e->posedT = -1.0f;
	}
	if (shadowA) {
		uint8_t* rgba = (uint8_t*)malloc((size_t)shadowW * shadowH * 4);
		if (rgba) {
			for (uint32_t k = 0; k < shadowW * shadowH; k++) {
				rgba[k * 4 + 0] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = 0;
				rgba[k * 4 + 3] = shadowA[k];
			}
			c->haveShadow = texpack_upload_rgba(rgba, (u16)shadowW, (u16)shadowH, &c->shadowTex,
				&c->shadowSub);
			free(rgba);
		}
		if (!c->haveShadow)
			logf_(log, "crowd: %s CHARACTERSHADOW did not upload -- no ground shadows", path);
	}
	// Init (Speed.elf 0x8004FB0C): an animated entity on the 35-bone skeleton
	// whose model is one of FlagGirlFlareMap's gets that row (#497); every
	// animated one gets its BoneMap row for the shadow.
	for (int i = 0; i < c->nEnt; i++) {
		CrowdEnt* e = &c->ent[i];
		e->flareMap = -1;
		e->shadowRow = -1;
		if (e->model >= 0 && e->skel >= 0 && e->anim >= 0) {
			const int nb = c->skel[e->skel].nBones;
			e->shadowRow = nb == FLARE_SKEL_BONES ? 0 : nb == 0x18 ? 1 : -1;
		}
		if (e->model < 0 || e->skel < 0 || e->anim < 0 ||
				c->skel[e->skel].nBones != FLARE_SKEL_BONES)
			continue;
		for (int k = 0; k < 4; k++)
			if (c->model[e->model].hash == bStringHash(kFlareMap[k].name)) {
				e->flareMap = k;
				logf_(log, "crowd: entity %d is %s -- two flares", i, kFlareMap[k].name);
			}
	}
	return c;
}

const void* crowd_scene_chunk(const Crowd* c, uint32_t* bytes) {
	*bytes = c ? c->sceneBytes : 0;
	return *bytes ? c->scene : NULL;
}

int crowd_entity_count(const Crowd* c) { return c ? c->nEnt : 0; }
int crowd_in_view(const Crowd* c) { return c ? c->inView : 0; }
int crowd_car_count(const Crowd* c) { return c ? c->nCar : 0; }

// Rows-as-axes rotation of a unit quaternion (x, y, z, w).
static void qmat(const float q[4], float m[3][3]) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y + z * w);     m[0][2] = 2 * (x * z - y * w);
	m[1][0] = 2 * (x * y - z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z + x * w);
	m[2][0] = 2 * (x * z + y * w);     m[2][1] = 2 * (y * z - x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}

static void lerpKey(const CrowdKey* k0, const CrowdKey* k1, float a, CrowdKey* out) {
	float s = (k0->q[0] * k1->q[0] + k0->q[1] * k1->q[1] + k0->q[2] * k1->q[2] + k0->q[3] * k1->q[3]) < 0
		? -1.0f : 1.0f;
	float n = 0;
	for (int i = 0; i < 4; i++) {
		out->q[i] = k0->q[i] * (1 - a) + s * k1->q[i] * a;
		n += out->q[i] * out->q[i];
	}
	n = n > 0 ? 1.0f / sqrtf(n) : 1.0f;
	for (int i = 0; i < 4; i++)
		out->q[i] *= n;
	for (int i = 0; i < 3; i++)
		out->t[i] = k0->t[i] * (1 - a) + k1->t[i] * a;
}

// out = a x b for affine 4x3 row-vector matrices.
static void mulAff(const float a[4][3], const float b[4][3], float out[4][3]) {
	for (int r = 0; r < 4; r++)
		for (int k = 0; k < 3; k++)
			out[r][k] = a[r][0] * b[0][k] + a[r][1] * b[1][k] + a[r][2] * b[2][k] + (r == 3 ? b[3][k] : 0.0f);
}

// CAnimCtrl's pose at scene time `t`: each bone's global matrix `g` and its
// skinning matrix `S` (inverse bind x global); bone 1's position goes to
// e->bone1 for the ground follow.
static void poseSkel(const Crowd* c, CrowdEnt* e, float t, float g[64][4][3], float S[64][4][3]) {
	const CrowdSkel* s = &c->skel[e->skel];
	const CrowdAnim* a = e->anim >= 0 ? &c->anim[e->anim] : NULL;

	int k0 = 0, k1 = 0;
	float frac = 0;
	if (a && a->nKeys > 0) {
		float f = (t > e->delay ? t - e->delay : 0.0f) * 30.0f * e->speed * 0.5f;
		const int last = a->nKeys - 1;
		// AdvanceAnimTime (Speed.elf 0x80054840): the range is [0, length], or
		// with flags & 0x40 SetLoopRange's [start, end] (anim frames, two to a
		// key). The anim runs from 0 to the end once; past it, flags & 0x20
		// loops back into the range, else it holds the end. #422: the walkers
		// of EndingNis02..04 walk in once and then idle (loop 419..539 etc.),
		// not walk in again from frame 0.
		float lo = 0.0f, hi = (float)last;
		if (e->flags & 0x40) {
			lo = (float)e->loopStart * e->speed * 0.5f;
			hi = (float)e->loopEnd * e->speed * 0.5f;
			if (hi > (float)last)
				hi = (float)last;
			if (lo > hi)
				lo = hi;
		}
		if (f > hi) {
			if ((e->flags & 0x20) && hi > lo)
				f = lo + fmodf(f - lo, hi - lo);
			else
				f = hi;
		}
		if (f > (float)last)
			f = (float)last;
		k0 = (int)f;
		k1 = k0 < last ? k0 + 1 : last;
		frac = f - (float)k0;
	}

	for (int b = 0; b < s->nBones; b++) {
		CrowdKey k;
		if (a && b < a->nBones && a->nKeys > 0)
			lerpKey(&a->key[k0 * a->nBones + b], &a->key[k1 * a->nBones + b], frac, &k);
		else
			k = s->bone[b].bind;
		float L[4][3];
		qmat(k.q, (float (*)[3])L);
		L[3][0] = k.t[0];
		L[3][1] = k.t[1];
		L[3][2] = k.t[2];
		const int p = s->bone[b].parent;
		if (p >= 0)
			mulAff((const float (*)[3])L, (const float (*)[3])g[p], g[b]);
		else
			memcpy(g[b], L, sizeof L);
		mulAff((const float (*)[3])s->bone[b].ib, (const float (*)[3])g[b], S[b]);
		// FindWorldBonePosition(1): where bone 1 is, for the ground follow.
		if (b == (s->nBones > 1 ? 1 : 0)) {
			e->bone1[0] = g[b][3][0];
			e->bone1[1] = g[b][3][1];
			e->bone1[2] = g[b][3][2];
		}
		// ... and the shadow's two (RenderEffects).
		for (int j = 0; j < 2; j++)
			if (e->shadowRow >= 0 && b == kBoneMap[e->shadowRow][j])
				memcpy(e->shadowBone[j], g[b][3], sizeof e->shadowBone[j]);
	}
}

static void skinEntity(const Crowd* c, CrowdEnt* e, float t) {
	const CrowdModel* m = &c->model[e->model];
	if (e->skel < 0)
		return;
	const CrowdSkel* s = &c->skel[e->skel];
	float g[64][4][3], S[64][4][3], invScale[64];
	poseSkel(c, e, t, g, S);
	for (int b = 0; b < s->nBones; b++) {
		// A bone's S is a rotation times a uniform scale (1 in these scenes),
		// so one factor keeps its normals unit -- no square root a vertex.
		const float l2 = S[b][0][0] * S[b][0][0] + S[b][0][1] * S[b][0][1] +
		                 S[b][0][2] * S[b][0][2];
		invScale[b] = l2 > 1e-12f ? 1.0f / sqrtf(l2) : 0.0f;
	}

	// Two in three of the crowd's vertices follow one bone (2026-11-05: 2291 of
	// 3384 in EndingNis02); those take the bone's matrix as it is. The rest
	// blend the matrices first and transform once.
	const float* src = m->bind;
	float* dst = (float*)e->verts;
	for (int v = 0; v < m->nVerts; v++, src += CROWD_VFLOATS, dst += CROWD_VFLOATS) {
		const CrowdSkin* sk = &m->skin[v];
		float B[4][3];
		const float (*M)[3];
		float nl;
		const int nw = (sk->w[0] > 0.0f) + (sk->w[1] > 0.0f) + (sk->w[2] > 0.0f);
		if (nw <= 1) {
			const int j = sk->w[0] > 0.0f ? 0 : sk->w[1] > 0.0f ? 1 : sk->w[2] > 0.0f ? 2 : 0;
			const int bi = sk->b[j] < s->nBones ? sk->b[j] : 0;
			M = (const float (*)[3])S[bi];
			nl = invScale[bi];
		} else {
			memset(B, 0, sizeof B);
			for (int j = 0; j < 3; j++) {
				const float w = sk->w[j];
				if (w <= 0.0f)
					continue;
				const int bi = sk->b[j] < s->nBones ? sk->b[j] : 0;
				const float* Sb = &S[bi][0][0];
				float* Bp = &B[0][0];
				for (int k = 0; k < 12; k++)
					Bp[k] += w * Sb[k];
			}
			M = (const float (*)[3])B;
			nl = -1.0f;   // a blend shortens the normal: renormalise below
		}
		float p[3], n[3];
		for (int k = 0; k < 3; k++) {
			p[k] = src[0] * M[0][k] + src[1] * M[1][k] + src[2] * M[2][k] + M[3][k];
			n[k] = src[3] * M[0][k] + src[4] * M[1][k] + src[5] * M[2][k];
		}
		if (nl < 0.0f) {
			nl = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
			nl = nl > 1e-12f ? 1.0f / sqrtf(nl) : 0.0f;
		}
		dst[0] = p[0];
		dst[1] = p[1];
		dst[2] = p[2];
		dst[3] = n[0] * nl;
		dst[4] = n[1] * nl;
		dst[5] = n[2] * nl;
	}
	GSPGPU_FlushDataCache(e->verts, (u32)m->nVerts * CROWD_VSIZE);
}

static bool (*g_groundFn)(const float p[3], float* outZ);

void crowd_ground_source(bool (*fn)(const float p[3], float* outZ)) {
	g_groundFn = fn;
}

#define CROWD_GROUND_LIFT   0.05f   // DAT_80319644 / DAT_80319658
#define CROWD_GROUND_SMOOTH 0.5f    // DAT_80319654

// UpdateTimeStep's follow: GetElevation under bone 1 (W the entity's world
// matrix), smoothed, and the entity's origin stood 0.05 m over it.
static void groundEntity(CrowdEnt* e, float W[4][3]) {
	if (!g_groundFn || (e->grounded && !e->follow))
		return;
	if (e->newPose || !e->grounded) {
		e->newPose = false;
		float p[3];
		for (int k = 0; k < 3; k++)
			p[k] = e->bone1[0] * W[0][k] + e->bone1[1] * W[1][k] + e->bone1[2] * W[2][k] + W[3][k];
		float z;
		if (g_groundFn(p, &z)) {
			e->groundZ = e->grounded
				? z * CROWD_GROUND_SMOOTH + e->groundZ * CROWD_GROUND_SMOOTH : z;
			e->grounded = true;
		}
	}
	if (e->grounded)
		W[3][2] = e->groundZ + CROWD_GROUND_LIFT;
}

void crowd_update(Crowd* c, float t) {
	if (c)
		c->t = t;
}

// Whether a sphere at `p` of radius `r` is inside the four side planes of the
// clip space `vp` makes (Gribb / Hartmann): those meet at the eye, so what is
// behind it fails one too. Near and far are left to the GPU.
static bool sphereInView(const C3D_Mtx* vp, const float p[3], float r) {
	float row[4][4];
	for (int i = 0; i < 4; i++) {
		row[i][0] = vp->r[i].x;
		row[i][1] = vp->r[i].y;
		row[i][2] = vp->r[i].z;
		row[i][3] = vp->r[i].w;
	}
	for (int axis = 0; axis < 2; axis++)
		for (int sgn = -1; sgn <= 1; sgn += 2) {
			float pl[4];
			for (int k = 0; k < 4; k++)
				pl[k] = row[3][k] + (float)sgn * row[axis][k];
			const float len = sqrtf(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
			if (len <= 0.0f)
				continue;
			if (pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] + pl[3] < -r * len)
				return false;
		}
	return true;
}

// Only who the camera sees is skinned (retail poses every CAnimCtrl; the
// pose of one out of view is never seen, and on the 3DS the skinning is the
// crowd's whole CPU cost: 17 people 5.5 ms a frame on Azahar, 2026-11-05).
// One back in view is posed for now before it draws.
void crowd_draw(Crowd* c, const C3D_Mtx* vp, const float scene[4][3]) {
	if (!c)
		return;
	c->inView = 0;
	for (int i = 0; i < c->nEnt; i++) {
		CrowdEnt* e = &c->ent[i];
		e->shadowNow = false;
		if (!e->verts)
			continue;
		const CrowdModel* m = &c->model[e->model];
		float W[4][3];
		mulAff(e->m, scene, W);
		if (e->grounded)
			W[3][2] = e->groundZ + CROWD_GROUND_LIFT;
		// The entity matrix may scale; its longest axis bounds the reach, and a
		// metre more covers what the animation moves past the bind pose.
		float s = 0.0f;
		for (int a = 0; a < 3; a++) {
			const float l = sqrtf(W[a][0] * W[a][0] + W[a][1] * W[a][1] + W[a][2] * W[a][2]);
			if (l > s)
				s = l;
		}
		if (!sphereInView(vp, W[3], m->radius * s + 1.0f))
			continue;
		c->inView++;
		if (e->posedT != c->t) {
			skinEntity(c, e, c->t);
			e->posedT = c->t;
			e->newPose = true;
		}
		groundEntity(e, W);
		if (e->shadowRow >= 0 && c->haveShadow) {
			for (int j = 0; j < 2; j++) {
				const float* p = e->shadowBone[j];
				for (int k = 0; k < 3; k++)
					e->shadowW[j][k] = p[0] * W[0][k] + p[1] * W[1][k] + p[2] * W[2][k] + W[3][k];
			}
			e->shadowNow = true;
		}
		// The draw records the streams' addresses into the command list, so
		// each entity can lend the model its own pool in turn.
		model3d_set_streams(m->m, e->verts, m->uv);
		model3d_draw_car_xform(m->m, vp, (const float (*)[3])W, W[3]);
	}
}

int crowd_flares(Crowd* c, float t, const float scene[4][3], float pos[][3], float dir[][3],
		int max) {
	int n = 0;
	for (int i = 0; c && i < c->nEnt && n + 2 <= max; i++) {
		CrowdEnt* e = &c->ent[i];
		if (e->flareMap < 0)
			continue;
		float g[64][4][3], S[64][4][3];
		poseSkel(c, e, t, g, S);
		float W[4][3];
		mulAff(e->m, scene, W);
		if (e->grounded)
			W[3][2] = e->groundZ + CROWD_GROUND_LIFT;
		const float (*v)[3] = kFlareMap[e->flareMap].v;
		for (int f = 0; f < 2; f++) {
			const float (*B)[3] = (const float (*)[3])S[f ? FLARE_BONE_B : FLARE_BONE_A];
			float p[2][3];
			for (int j = 0; j < 2; j++) {
				float q[3];
				const float* a = v[f * 2 + j];
				for (int k = 0; k < 3; k++)
					q[k] = a[0] * B[0][k] + a[1] * B[1][k] + a[2] * B[2][k] + B[3][k];
				for (int k = 0; k < 3; k++)
					p[j][k] = q[0] * W[0][k] + q[1] * W[1][k] + q[2] * W[2][k] + W[3][k];
			}
			for (int k = 0; k < 3; k++) {
				pos[n][k] = p[0][k];
				dir[n][k] = p[0][k] - p[1][k];
			}
			n++;
		}
	}
	return n;
}

const C3D_Tex* crowd_shadow_tex(const Crowd* c, const Tex3DS_SubTexture** sub) {
	if (!c || !c->haveShadow)
		return NULL;
	if (sub)
		*sub = &c->shadowSub;
	return &c->shadowTex;
}

int crowd_shadow_quads(Crowd* c, float (*quad)[4][3], int max) {
	int n = 0;
	for (int i = 0; c && c->haveShadow && i < c->nEnt && n < max; i++) {
		CrowdEnt* e = &c->ent[i];
		if (!e->shadowNow)
			continue;
		const float* a = e->shadowW[0];
		const float* b = e->shadowW[1];
		// The line from the second bone to the first, (1, 0) when they stand
		// on one spot; its perpendicular (-y, x).
		float d[2] = { 1.0f, 0.0f };
		if (a[0] != b[0] || a[1] != b[1]) {
			d[0] = a[0] - b[0];
			d[1] = a[1] - b[1];
			const float l = sqrtf(d[0] * d[0] + d[1] * d[1]);
			d[0] /= l;
			d[1] /= l;
		}
		const float f[2] = { d[0] * SHADOW_ALONG, d[1] * SHADOW_ALONG };
		const float s[2] = { -d[1] * SHADOW_ACROSS, d[0] * SHADOW_ACROSS };
		// The ePoly's corners: first bone + f - s, + f + s; second - f + s, - f - s.
		const float xy[4][2] = {
			{ a[0] + f[0] - s[0], a[1] + f[1] - s[1] },
			{ a[0] + f[0] + s[0], a[1] + f[1] + s[1] },
			{ b[0] - f[0] + s[0], b[1] - f[1] + s[1] },
			{ b[0] - f[0] - s[0], b[1] - f[1] - s[1] },
		};
		bool ok = true;
		for (int k = 0; k < 4 && ok; k++) {
			const float dx = xy[k][0] - e->shadowXY[k][0], dy = xy[k][1] - e->shadowXY[k][1];
			if (!e->shadowProbed[k] || dx * dx + dy * dy > SHADOW_REPROBE * SHADOW_REPROBE) {
				// GetElevation at the corner, from the second bone's height.
				const float p[3] = { xy[k][0], xy[k][1], b[2] };
				float z;
				if (g_groundFn && g_groundFn(p, &z)) {
					e->shadowZ[k] = z;
				} else if (e->grounded) {
					e->shadowZ[k] = e->groundZ;
				} else {
					ok = false;
					break;
				}
				e->shadowXY[k][0] = xy[k][0];
				e->shadowXY[k][1] = xy[k][1];
				e->shadowProbed[k] = true;
			}
			quad[n][k][0] = xy[k][0];
			quad[n][k][1] = xy[k][1];
			quad[n][k][2] = e->shadowZ[k] + SHADOW_LIFT;
		}
		if (ok)
			n++;
	}
	return n;
}

int crowd_car_keys(const Crowd* c, int idx) {
	return c && idx >= 0 && idx < c->nCar ? c->car[idx].nKeys : 0;
}

bool crowd_car_pose(const Crowd* c, int idx, float t, float rot[3][3], float pos[3]) {
	if (!c || idx < 0 || idx >= c->nCar || c->car[idx].nKeys <= 0)
		return false;
	const CrowdCar* car = &c->car[idx];
	const int last = car->nKeys - 1;
	float f = t > 0 ? t * 15.0f : 0.0f;
	if (f > (float)last)
		f = (float)last;
	const int k0 = (int)f;
	const int k1 = k0 < last ? k0 + 1 : last;
	CrowdKey k;
	lerpKey(&car->key[k0], &car->key[k1], f - (float)k0, &k);
	qmat(k.q, rot);
	pos[0] = k.t[0];
	pos[1] = k.t[1];
	pos[2] = k.t[2];
	return true;
}
