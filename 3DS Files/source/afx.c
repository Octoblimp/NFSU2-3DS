// The retail particle system for free roam's markers and smoke; see afx.h.
// Every constant and every step below is SPEED2.EXE's unless a comment says
// otherwise, and the comments name the addresses they were read from.
#include "afx.h"
#include "shaders.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AFX_VERSION     1
#define AFX_HDR_SIZE    36
#define AFX_TEX_SIZE    16
#define AFX_TMPL_SIZE   0x184
#define AFX_GROUP_SIZE  16
#define AFX_INST_SIZE   88
#define AFX_GROUP_MAX   AFX_PROP_EMITTERS   // a group's u16 tmpl[4]
#define AFX_ATTACH      64   // the props' emitters at once (afx_attach)
#define AFX_NO_TEX      0xFFFF
#define TEXFMT_RGB565   0
#define TEXFMT_RGBA8    2

// 0x488730: the particle pool is 1000 entries of 0x20 (300 at the lowest
// detail setting, which the GameCube build does not have).
#define AFX_POOL        1000
#define AFX_NIL         0xFFFF
// TriggerEmitter caps a particle's life at 60 s before storing it in 1/1024 s.
#define AFX_MAX_LIFE    60.0f
// Not retail: a bound on one call's spawn loop, so a corrupt rate cannot hang
// the frame. The shipped worst case is 100/s, a few per frame.
#define AFX_MAX_SPAWNS  4096

static void (*s_log)(const char*) = NULL;
// #521: a 3DS-only cap on one particle's on-screen size, in 240-line pixels
// (0 = only the template's). Near a marker its haze stacks ~22 screens of
// blended fill (7-8 ms on the New 3DS), but a cap shrinks the ring up close:
// the user (2026-11-11), seeing 128 px: "Circle is a little small, shouldn't
// it look normal?" -- so it ships off (AFX_SCREEN_CAP_3DS 0), DbgAfxCap only.
static float s_screenCap = 0.0f;
void afx_set_screen_cap(float px) { s_screenCap = px > 0.0f ? px : 0.0f; }
// #521: the faint-particle skip and the zero-alpha test below, off for the
// tour's "fxold" step.
static bool s_lean = true;
void afx_set_lean(bool on) { s_lean = on; }
// g_ParticleSystemEnable (#326): afx_set_particles().
static bool s_particlesOn = true;

void afx_set_log(void (*sink)(const char* msg)) { s_log = sink; }

static void alog(const char* fmt, ...) {
	if (!s_log)
		return;
	char buf[192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	s_log(buf);
}

// ------------------------------------------------------------------ random
//
// One xorshift step (17, 13, 5) everywhere. The global stream (0x7f3940) only
// seeds emitters; each emitter then draws its particles from its own word.
static u32 s_globalSeed = 0x23985727u;

static inline u32 xs(u32 x) {
	x ^= x << 17;
	x ^= x >> 13;
	x ^= x << 5;
	return x;
}

static inline float rnd(u32 x) {
	return (float)(x & 0x7fffffffu) * (1.0f / 2147483648.0f);
}

static u32 newSeed(void) {
	s_globalSeed = xs(s_globalSeed);
	return s_globalSeed & 0x7fffffffu;
}

// 0x487360: a uniform draw in [0, range) off an emitter's own word.
static float rndRange(float range, u32* seed) {
	*seed = xs(*seed);
	return rnd(*seed) * range;
}

// 0x43c370 / 0x43c2b0 take a 16-bit angle.
static inline float cosU(u16 a) { return cosf((float)a * (6.28318531f / 65536.0f)); }
static inline float sinU(u16 a) { return sinf((float)a * (6.28318531f / 65536.0f)); }

// A cone angle off one draw: degrees, centred on zero, to 16-bit units with
// TriggerEmitter's two truncations.
static inline u16 coneAngle(u32 s, float spread) {
	int v = (int)((rnd(s) * spread - spread * 0.5f) * 65536.0f);
	return (u16)(v / 360);
}

// ---------------------------------------------------------------- the data

// AcidEmitterData's runtime prefix, as afx_pack.py stores it.
typedef struct {
	u8    cone;          // +0x18  0: random rotation about all three axes
	u8    ubits, vbits;  // +0x19  atlas cells, log2 across and down
	u8    fps;           // +0x1b  atlas animation rate, 0 = none
	u8    randomCell;    // +0x1c
	u8    spinFlip;      // +0x1d  odd rotation = spin the other way
	u8    carInherit;    // +0x1e
	s8    sizeIdx;       // +0x1f  aspect table index
	u8    orient;        // +0x20  0 = camera-facing billboard (list 0)
	u8    lowDetail;     // +0x21  spawns with g_ParticleSystemEnable 0 too
	float onTime, onFrac, offTime, offFrac;    // +0x24
	float rate, rateRand;                      // +0x34
	float life, lifeRand;                      // +0x3c
	float speed, speedRand;                    // +0x44
	float rotRange;                            // +0x4c  degrees
	float spread;                              // +0x50  degrees
	float inherit, inheritRand;                // +0x54
	float offset[3];                           // +0x60
	float box[3];                              // +0x70
	float maxDist;                             // +0x80
	float gravity;                             // +0x84
	float drag;                                // +0x88
	float maxScreen;                           // +0x8c  pixels, 0 = 512
	u32   posType;                             // +0x5c  CarRenderInfo position list
	float color[16];                           // +0x100 RGBA cubic over life
	float size[16];                            // +0x140 x size, y spin
	u16   tex;
} AfxTmpl;

// A particle, 0x20 bytes in retail; the list links shrink to one index here.
typedef struct {
	float pos[3];
	u16   next;
	u16   life;        // 1/1024 s left, read signed
	u16   rot;         // 16-bit angle, bit 0 = spin direction
	u16   speed;       // m/s * 128
	u8    inherit;     // 0..255
	u8    orientSize;  // orientation | aspect index << 4
	s8    dir[3];      // unit direction * 127
	u8    cell;
} AfxParticle;

// An AcidEmitter (0xd0 in retail) with its holder folded in. Retail allocates
// a holder (0x491c00) the first time an emitter stores a particle, appends it
// to one list, and frees it once both its particle lists run dry; that list
// is the draw order, so it is kept here as hPrev/hNext.
typedef struct {
	const AfxTmpl* t;
	u32   cond;           // +0x2c
	float m[16];          // +0x40  rows: x, y, z axes, position
	float inherit[3];     // +0x80
	float timer;          // +0x14
	float next;           // +0x18
	u32   seed;           // +0x24
	s16   hold;           // +0x28
	u8    state;          // +0x10  1 = emitting, 3 = fired by afx_burst only
	// +0x30: the colour cubic the particles are drawn with, NULL for the
	// template's -- a car's purge override (afx_burst).
	const float* color;
	bool  used;
	bool  holder;
	// Fired for a car (afx_car_emitter): TriggerEmitter's Car*, which the
	// holder keeps (+0x10) and DoTimeStep drags +0x1e templates' particles
	// along by. Its velocity is `inherit`, kept current by the caller.
	bool  car;
	u16   head[2], tail[2], count[2];
	s16   hPrev, hNext;
} AfxEmitter;

typedef struct {
	float pos[3];
	u8    rgba[4];
	float uv[2];
} AfxVert;

typedef struct {
	u16 firstQuad, quads, tex;
} AfxRun;

struct Afx {
	int         nTex, nTmpl, nInst;
	C3D_Tex*    tex;
	bool*       texValid;
	u8*         texBlend;
	AfxTmpl*    tmpl;
	u8          groupCount[AFX_SLOTS];
	u16         groupTmpl[AFX_SLOTS][AFX_GROUP_MAX];
	bool        slotActive[AFX_SLOTS];
	float       slotPos[AFX_SLOTS][3];
	// The props' groups, after the slots' in the file.
	int         nProp;
	u32*        propHash;
	u8*         propCount;
	u16       (*propTmpl)[AFX_GROUP_MAX];
	int         nEm;          // placed, AFX_GROUP_MAX per slot, then AFX_ATTACH
	int         attachBase;
	AfxEmitter* em;
	s16         hHead, hTail;
	u32         cond;
	u32         cond2;        // a second name on the list (#381), 0 for none
	u16         freeHead;
	int         live;
	AfxParticle pool[AFX_POOL];
	AfxVert*    vbuf[2];
	u16*        ibuf;
	AfxRun*     runs;
	int         frame;
	int         lastQuads;    // debug (#317): what the last afx_draw() put up
	float       lastArea;     // debug (#521): those quads' summed screen area, px^2
};

// ------------------------------------------------------------------ shader

static DVLE_s*         s_dvle;   // the shared blob\'s (shaders.h, #352)
static shaderProgram_s s_prog;
static int             s_uMvp = -1;
static bool            s_ready;

static bool initShader(void) {
	if (s_ready)
		return true;
	s_dvle = shaders_get(SHADER_PARTICLE);
	if (!s_dvle)
		return false;
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, s_dvle);
	s_uMvp = shaderInstanceGetUniformLocation(s_prog.vertexShader, "mvp");
	if (s_uMvp < 0) {
		shaderProgramFree(&s_prog);
		s_dvle = NULL;
		return false;
	}
	s_ready = true;
	return true;
}

// -------------------------------------------------------------------- load

static u32 rd32(const u8* p, u32 off) {
	return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) |
	       ((u32)p[off + 3] << 24);
}

static u16 rd16(const u8* p, u32 off) {
	return (u16)((u32)p[off] | ((u32)p[off + 1] << 8));
}

static float rdf(const u8* p, u32 off) {
	u32 v = rd32(p, off);
	float f;
	memcpy(&f, &v, 4);
	return f;
}

static void parseTmpl(AfxTmpl* t, const u8* r) {
	t->cone       = r[0x18];
	t->ubits      = r[0x19] & 7;
	t->vbits      = r[0x1a] & 7;
	t->fps        = r[0x1b];
	t->randomCell = r[0x1c];
	t->spinFlip   = r[0x1d];
	t->carInherit = r[0x1e];
	t->sizeIdx    = (s8)r[0x1f];
	t->orient     = r[0x20];
	t->lowDetail  = r[0x21];
	t->onTime     = rdf(r, 0x24);
	t->onFrac     = rdf(r, 0x28);
	t->offTime    = rdf(r, 0x2c);
	t->offFrac    = rdf(r, 0x30);
	t->rate       = rdf(r, 0x34);
	t->rateRand   = rdf(r, 0x38);
	t->life       = rdf(r, 0x3c);
	t->lifeRand   = rdf(r, 0x40);
	t->speed      = rdf(r, 0x44);
	t->speedRand  = rdf(r, 0x48);
	t->rotRange   = rdf(r, 0x4c);
	t->spread     = rdf(r, 0x50);
	t->inherit    = rdf(r, 0x54);
	t->inheritRand = rdf(r, 0x58);
	for (int i = 0; i < 3; i++) {
		t->offset[i] = rdf(r, 0x60 + i * 4);
		t->box[i]    = rdf(r, 0x70 + i * 4);
	}
	t->maxDist    = rdf(r, 0x80);
	t->gravity    = rdf(r, 0x84);
	t->drag       = rdf(r, 0x88);
	t->maxScreen  = rdf(r, 0x8c);
	t->posType    = rd32(r, 0x5c);
	for (int i = 0; i < 16; i++) {
		t->color[i] = rdf(r, 0x100 + i * 4);
		t->size[i]  = rdf(r, 0x140 + i * 4);
	}
	t->tex = rd16(r, 0x180);
	// RenderEffects divides by the life; a zero would be a corrupt record.
	if (!(t->life > 0.0f))
		t->life = 1.0f;
}

static bool isPow2(u32 v) { return v && !(v & (v - 1)); }

// The texture chain at `off` past the pixel base, the way model3d.c uploads a
// car's: one C3D_TexLoadImage per level, already tiled by the packer.
static bool loadTex(Afx* a, int i, FILE* f, u32 pixOff, u32 fileSize,
		const u8* e) {
	u16 w = rd16(e, 4), h = rd16(e, 6), fld = rd16(e, 8);
	u32 off = rd32(e, 12);
	u32 fmt = fld & 0xff, mlvl = (fld >> 8) & 0xf;
	a->texBlend[i] = e[10];
	if (!isPow2(w) || !isPow2(h) || w < 8 || h < 8 || w > 1024 || h > 1024)
		return false;
	if ((w >> mlvl) < 8 || (h >> mlvl) < 8)
		return false;
	u32 bpp = fmt == TEXFMT_RGBA8 ? 4 : 2;
	u32 lvl0 = (u32)w * h * bpp, bytes = lvl0;
	for (u32 lv = 0, sz = lvl0; lv < mlvl; lv++) {
		sz >>= 2;
		bytes += sz;
	}
	if ((u64)pixOff + off + bytes > fileSize)
		return false;
	u8* buf = (u8*)malloc(bytes);
	if (!buf)
		return false;
	bool ok = fseek(f, (long)(pixOff + off), SEEK_SET) == 0 &&
	          fread(buf, 1, bytes, f) == bytes;
	GPU_TEXCOLOR gfmt = fmt == TEXFMT_RGBA8 ? GPU_RGBA8
	                  : fmt == TEXFMT_RGB565 ? GPU_RGB565 : GPU_RGBA4;
	C3D_TexInitParams tp = { w, h, (u8)mlvl, gfmt, GPU_TEX_2D, false };
	if (ok)
		ok = C3D_TexInitWithParams(&a->tex[i], NULL, tp);
	if (ok) {
		const u8* src = buf;
		u32 sz = lvl0;
		for (u32 lv = 0; lv <= mlvl; lv++) {
			C3D_TexLoadImage(&a->tex[i], src, GPU_TEXFACE_2D, (int)lv);
			src += sz;
			sz >>= 2;
		}
		C3D_TexFlush(&a->tex[i]);
		C3D_TexSetFilter(&a->tex[i], GPU_LINEAR, GPU_LINEAR);
		if (mlvl)
			C3D_TexSetFilterMipmap(&a->tex[i], GPU_LINEAR);
		// Atlas cells end on the texture's edges; repeat would pull the far
		// side's texels into a cell's border under bilinear filtering.
		C3D_TexSetWrap(&a->tex[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	free(buf);
	return ok;
}

static void addEmitter(Afx* a, AfxEmitter* e);

Afx* afx_open(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		alog("afx: no %s -- marker effects off", path);
		return NULL;
	}
	u8 hdr[AFX_HDR_SIZE];
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsz < AFX_HDR_SIZE || fread(hdr, 1, AFX_HDR_SIZE, f) != AFX_HDR_SIZE ||
			memcmp(hdr, "NFSA", 4) != 0 || rd32(hdr, 4) != AFX_VERSION) {
		alog("afx: %s is not a version %d pack", path, AFX_VERSION);
		fclose(f);
		return NULL;
	}
	u32 fileSize = (u32)fsz;
	int nTex = rd16(hdr, 8), nTmpl = rd16(hdr, 10);
	int nGroup = rd16(hdr, 12), nInst = rd16(hdr, 14);
	u32 texOff = rd32(hdr, 16), tmplOff = rd32(hdr, 20), groupOff = rd32(hdr, 24);
	u32 instOff = rd32(hdr, 28), pixOff = rd32(hdr, 32);
	if (texOff + (u32)nTex * AFX_TEX_SIZE > tmplOff ||
			tmplOff + (u32)nTmpl * AFX_TMPL_SIZE > groupOff ||
			groupOff + (u32)nGroup * AFX_GROUP_SIZE > instOff ||
			instOff + (u32)nInst * AFX_INST_SIZE > pixOff || pixOff > fileSize ||
			texOff < AFX_HDR_SIZE || nTmpl == 0) {
		alog("afx: %s: bad table offsets", path);
		fclose(f);
		return NULL;
	}

	u8* tables = (u8*)malloc(pixOff);
	Afx* a = (Afx*)calloc(1, sizeof(Afx));
	bool ok = tables && a && fseek(f, 0, SEEK_SET) == 0 &&
	          fread(tables, 1, pixOff, f) == pixOff;
	if (ok) {
		a->nTex = nTex;
		a->nTmpl = nTmpl;
		a->nInst = nInst;
		a->attachBase = nInst + AFX_SLOTS * AFX_GROUP_MAX;
		a->nEm = a->attachBase + AFX_ATTACH;
		a->nProp = nGroup > AFX_SLOTS ? nGroup - AFX_SLOTS : 0;
		a->propHash = (u32*)calloc(a->nProp ? a->nProp : 1, sizeof(u32));
		a->propCount = (u8*)calloc(a->nProp ? a->nProp : 1, 1);
		a->propTmpl = (u16(*)[AFX_GROUP_MAX])calloc(a->nProp ? a->nProp : 1,
			sizeof(*a->propTmpl));
		a->tex = (C3D_Tex*)calloc(nTex ? nTex : 1, sizeof(C3D_Tex));
		a->texValid = (bool*)calloc(nTex ? nTex : 1, sizeof(bool));
		a->texBlend = (u8*)calloc(nTex ? nTex : 1, 1);
		a->tmpl = (AfxTmpl*)calloc(nTmpl, sizeof(AfxTmpl));
		a->em = (AfxEmitter*)calloc(a->nEm, sizeof(AfxEmitter));
		a->runs = (AfxRun*)calloc(a->nEm * 2, sizeof(AfxRun));
		a->vbuf[0] = (AfxVert*)linearAlloc(AFX_POOL * 4 * sizeof(AfxVert));
		a->vbuf[1] = (AfxVert*)linearAlloc(AFX_POOL * 4 * sizeof(AfxVert));
		a->ibuf = (u16*)linearAlloc(AFX_POOL * 6 * sizeof(u16));
		ok = a->tex && a->texValid && a->texBlend && a->tmpl && a->em &&
		     a->runs && a->vbuf[0] && a->vbuf[1] && a->ibuf && a->propHash &&
		     a->propCount && a->propTmpl && initShader();
	}
	if (!ok) {
		alog("afx: %s: out of memory or unreadable", path);
		free(tables);
		fclose(f);
		afx_close(a);
		return NULL;
	}

	int texOk = 0;
	for (int i = 0; i < nTex; i++) {
		a->texValid[i] = loadTex(a, i, f, pixOff, fileSize,
		                         tables + texOff + i * AFX_TEX_SIZE);
		texOk += a->texValid[i];
	}
	fclose(f);

	for (int i = 0; i < nTmpl; i++) {
		parseTmpl(&a->tmpl[i], tables + tmplOff + i * AFX_TMPL_SIZE);
		if (a->tmpl[i].tex >= nTex)
			a->tmpl[i].tex = AFX_NO_TEX;
	}
	for (int s = 0; s < nGroup; s++) {
		const u8* g = tables + groupOff + s * AFX_GROUP_SIZE;
		const bool slot = s < AFX_SLOTS;
		u16* tl = slot ? a->groupTmpl[s] : a->propTmpl[s - AFX_SLOTS];
		int n = g[4] > AFX_GROUP_MAX ? AFX_GROUP_MAX : g[4];
		int k = 0;
		for (int j = 0; j < n; j++) {
			u16 ti = rd16(g, 8 + j * 2);
			if (ti < nTmpl)
				tl[k++] = ti;
		}
		if (slot) {
			a->groupCount[s] = (u8)k;
		} else {
			a->propHash[s - AFX_SLOTS] = rd32(g, 0);
			a->propCount[s - AFX_SLOTS] = (u8)k;
		}
	}

	a->hHead = a->hTail = -1;
	for (int i = 0; i < AFX_POOL; i++)
		a->pool[i].next = (u16)(i + 1 < AFX_POOL ? i + 1 : AFX_NIL);
	a->freeHead = 0;
	for (int q = 0; q < AFX_POOL; q++) {
		u16* ix = a->ibuf + q * 6;
		u16 b = (u16)(q * 4);
		ix[0] = b; ix[1] = (u16)(b + 1); ix[2] = (u16)(b + 2);
		ix[3] = b; ix[4] = (u16)(b + 2); ix[5] = (u16)(b + 3);
	}
	GSPGPU_FlushDataCache(a->ibuf, AFX_POOL * 6 * sizeof(u16));

	// The placed emitters, added in file order as the sections would add
	// them; every one afx_pack.py keeps is state 1.
	int placed = 0;
	for (int i = 0; i < nInst; i++) {
		const u8* r = tables + instOff + i * AFX_INST_SIZE;
		u16 ti = rd16(r, 0);
		if (ti >= nTmpl)
			continue;
		AfxEmitter* e = &a->em[i];
		memset(e, 0, sizeof(*e));
		e->t = &a->tmpl[ti];
		e->hold = (s16)rd16(r, 2);
		e->cond = rd32(r, 4);
		for (int k = 0; k < 16; k++)
			e->m[k] = rdf(r, 12 + k * 4);
		for (int k = 0; k < 3; k++)
			e->inherit[k] = rdf(r, 76 + k * 4);
		e->state = 1;
		e->used = true;
		addEmitter(a, e);
		placed++;
	}
	free(tables);
	alog("afx: %d/%d textures, %d templates, %d placed, %d trigger groups, %d prop groups",
		texOk, nTex, nTmpl, placed, nGroup < AFX_SLOTS ? nGroup : AFX_SLOTS, a->nProp);
	return a;
}

void afx_close(Afx* a) {
	if (!a)
		return;
	for (int i = 0; a->tex && a->texValid && i < a->nTex; i++)
		if (a->texValid[i])
			C3D_TexDelete(&a->tex[i]);
	if (a->vbuf[0]) linearFree(a->vbuf[0]);
	if (a->vbuf[1]) linearFree(a->vbuf[1]);
	if (a->ibuf) linearFree(a->ibuf);
	free(a->tex);
	free(a->texValid);
	free(a->texBlend);
	free(a->tmpl);
	free(a->em);
	free(a->runs);
	free(a->propHash);
	free(a->propCount);
	free(a->propTmpl);
	free(a);
}

// --------------------------------------------------------------- emitters

// The on/off period for the state an emitter is entering, randomised by its
// fraction the way 0x48bc50 does it: period - frac*period + rand(frac*period).
static float emitterPeriod(AfxEmitter* e) {
	const AfxTmpl* t = e->t;
	float period = e->state == 1 ? t->onTime : t->offTime;
	float frac   = e->state == 1 ? t->onFrac : t->offFrac;
	if (period == 0.0f)
		return 0.0f;
	float span = frac * period;
	return rndRange(span, &e->seed) + (period - span);
}

// 0x48bc50, the list add: seed the emitter off the global stream if it has no
// word of its own yet, and start its first on (or off) period.
static void addEmitter(Afx* a, AfxEmitter* e) {
	(void)a;
	if (e->seed == 0)
		e->seed = newSeed();
	if (e->state <= 1)
		e->timer = emitterPeriod(e);
	e->next = 0.0f;
	e->holder = false;
	e->hPrev = e->hNext = -1;
}

static bool newHolder(Afx* a, int ei) {
	AfxEmitter* e = &a->em[ei];
	e->holder = true;
	e->head[0] = e->head[1] = e->tail[0] = e->tail[1] = AFX_NIL;
	e->count[0] = e->count[1] = 0;
	e->hPrev = a->hTail;
	e->hNext = -1;
	if (a->hTail >= 0)
		a->em[a->hTail].hNext = (s16)ei;
	else
		a->hHead = (s16)ei;
	a->hTail = (s16)ei;
	return true;
}

// The holder goes, and every particle still in it (0x494690 when a trigger
// slot is cleared; otherwise the lists are already empty).
static void freeHolder(Afx* a, int ei) {
	AfxEmitter* e = &a->em[ei];
	if (!e->holder)
		return;
	for (int l = 0; l < 2; l++) {
		for (u16 i = e->head[l]; i != AFX_NIL; ) {
			u16 n = a->pool[i].next;
			a->pool[i].next = a->freeHead;
			a->freeHead = i;
			a->live--;
			i = n;
		}
		e->head[l] = e->tail[l] = AFX_NIL;
		e->count[l] = 0;
	}
	if (e->hPrev >= 0)
		a->em[e->hPrev].hNext = e->hNext;
	else
		a->hHead = e->hNext;
	if (e->hNext >= 0)
		a->em[e->hNext].hPrev = e->hPrev;
	else
		a->hTail = e->hPrev;
	e->hPrev = e->hNext = -1;
	e->holder = false;
}

// RotX/RotY/RotZ about a 3x3 held as rows, row-vector convention (v' = v M),
// each applied after what M already does. Checked against SPEED2.EXE (#242):
// 0x5BACF0 / 0x5BADA0 / 0x5BAE60 (its matrix from 0x5BAC90) build exactly
// these R from cos 0x43C370 and sin 0x43C2B0, and multiply out = M x R
// (0x63CEDB); TriggerEmitter's cone-0 arm calls them X, Y, Z off the three
// draws in order on the identity.
static void rotPost(float M[9], int axis, u16 ang) {
	float c = cosU(ang), s = sinU(ang);
	float R[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	if (axis == 0) {
		R[4] = c;  R[5] = s;  R[7] = -s; R[8] = c;
	} else if (axis == 1) {
		R[0] = c;  R[2] = -s; R[6] = s;  R[8] = c;
	} else {
		R[0] = c;  R[1] = s;  R[3] = -s; R[4] = c;
	}
	float o[9];
	for (int r = 0; r < 3; r++)
		for (int k = 0; k < 3; k++)
			o[r * 3 + k] = M[r * 3] * R[k] + M[r * 3 + 1] * R[3 + k] +
			               M[r * 3 + 2] * R[6 + k];
	memcpy(M, o, sizeof(o));
}

// TriggerEmitter (0x491e50) for one emitter and one step.
static void triggerEmitter(Afx* a, int ei, float dt, const float view[3]) {
	AfxEmitter* e = &a->em[ei];
	const AfxTmpl* t = e->t;

	// Retail asks each player's view; there is one here.
	float dx = e->m[12] - view[0], dy = e->m[13] - view[1], dz = e->m[14] - view[2];
	bool inRange = sqrtf(dx * dx + dy * dy + dz * dz) < t->maxDist;
	if (e->seed == 0)
		e->seed = newSeed();
	if (!inRange)
		return;
	if (e->state != 1 && !(dt < 0.0f))
		return;

	float tt = dt;
	if (dt < 0.0f)
		tt = -dt;
	else if (e->timer != 0.0f && dt > e->timer)
		tt = e->timer;

	float interval = e->next;
	if (tt > e->next && (e->holder || newHolder(a, ei))) {
		const float rateR  = t->rate * t->rateRand;
		const float rateB  = t->rate - rateR;
		const float speedR = t->speedRand * t->speed;
		const float speedB = t->speed - speedR;
		const float lifeR  = t->lifeRand * t->life;
		const float lifeB  = t->life - lifeR;
		const float inhR   = t->inheritRand * t->inherit;
		const float inhB   = t->inherit - inhR;
		const float rotR   = t->rotRange * (65536.0f / 360.0f);
		const float rotB   = -0.5f * rotR;
		const float cells  = t->randomCell
			? (float)((1 << t->ubits) * (1 << t->vbits)) : 1.0f;
		const int list = t->orient != 0;
		int sizeIdx = t->sizeIdx;
		if (sizeIdx < 0 || sizeIdx > 3)
			sizeIdx = 0;
		const u8 orientSize = (u8)((t->orient & 0xF) | (sizeIdx << 4));
		const float* m = e->m;
		u32 s = e->seed;
		int guard = 0;

		do {
			tt -= interval;

			float dir[3];
			if (t->cone == 0) {
				u32 s1 = xs(s), s2 = xs(s1), s3 = xs(s2);
				float M[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
				rotPost(M, 0, coneAngle(s1, t->spread));
				rotPost(M, 1, coneAngle(s2, t->spread));
				rotPost(M, 2, coneAngle(s3, t->spread));
				for (int k = 0; k < 3; k++)
					dir[k] = m[8] * M[k] + m[9] * M[3 + k] + m[10] * M[6 + k];
				s = s3;
			} else {
				// A flat spray: a random heading in the emitter's XY plane,
				// tipped by the spread about Y.
				u32 s1 = xs(s), s2 = xs(s1);
				int yv = (int)((rnd(s2) * 360.0f - 180.0f) * 65536.0f);
				u16 yaw = (u16)(yv / 360);
				u16 pitch = coneAngle(s1, t->spread);
				float cy = cosU(yaw), sy = sinU(yaw);
				float cp = cosU(pitch), sp = sinU(pitch);
				float l[3] = { cy * cp, sy, -cy * sp };
				for (int k = 0; k < 3; k++)
					dir[k] = l[0] * m[k] + l[1] * m[4 + k] + l[2] * m[8 + k];
				s = s2;
			}

			float p[3];
			for (int k = 0; k < 3; k++) {
				s = xs(s);
				p[k] = rnd(s) * t->box[k] - t->box[k] * 0.5f + t->offset[k];
			}
			float wpos[3];
			for (int k = 0; k < 3; k++)
				wpos[k] = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k] + m[12 + k];

			s = xs(s);
			int cell = (int)(rnd(s) * cells);
			s = xs(s);
			float life = rnd(s) * lifeR + lifeB;
			s = xs(s);
			float rot = rnd(s) * rotR + rotB;
			s = xs(s);
			float speed = rnd(s) * speedR + speedB;
			float inh = 0.0f;
			if (inhB != 0.0f) {
				s = xs(s);
				inh = rnd(s) * inhR + inhB;
				inh = inh < 0.0f ? 0.0f : (inh > 1.0f ? 1.0f : inh);
				if (!t->carInherit) {
					float v[3];
					for (int k = 0; k < 3; k++)
						v[k] = dir[k] * speed + inh * e->inherit[k];
					speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
					float inv = speed > 0.0f ? 1.0f / speed : 0.0f;
					for (int k = 0; k < 3; k++)
						dir[k] = v[k] * inv;
				}
			}
			u16 rotU = (u16)((int)rot & 0xfffe);
			if (t->spinFlip)
				rotU |= (u16)(s & 1);
			if (life > AFX_MAX_LIFE)
				life = AFX_MAX_LIFE;

			// g_ParticleSystemEnable 0 (#326): TriggerEmitter still makes every
			// draw, so the emitter's stream runs on, but stores the particle only
			// for a template with +0x21 set.
			const bool gated = !s_particlesOn && !t->lowDetail;
			if (e->hold == 0 && a->freeHead != AFX_NIL && !gated) {
				u16 pi = a->freeHead;
				AfxParticle* pp = &a->pool[pi];
				a->freeHead = pp->next;
				a->live++;
				float sp = speed * 128.0f;
				pp->life = (u16)(int)(life * 1024.0f);
				pp->rot = rotU;
				pp->speed = (u16)(int)(sp < 65535.0f ? sp : 65535.0f);
				pp->inherit = (u8)(int)(inh * 255.0f);
				pp->orientSize = orientSize;
				for (int k = 0; k < 3; k++) {
					pp->pos[k] = wpos[k];
					pp->dir[k] = (s8)(int)(dir[k] * 127.0f);
				}
				pp->cell = (u8)cell;
				pp->next = AFX_NIL;
				if (e->tail[list] != AFX_NIL)
					a->pool[e->tail[list]].next = pi;
				else
					e->head[list] = pi;
				e->tail[list] = pi;
				e->count[list]++;
			}

			s = xs(s);
			float rate = rnd(s) * rateR + rateB;
			if (rate > 0.0f) {
				interval = 1.0f / rate;
				if (interval > 240.0f)
					interval = 240.0f;
			} else {
				interval = 0.1f;
			}
		} while (tt >= interval && ++guard < AFX_MAX_SPAWNS);

		e->seed = s;
		if (e->count[0] == 0 && e->count[1] == 0)
			freeHolder(a, ei);
	}
	e->next = dt < 0.0f ? 0.0f : interval - tt;
}

void afx_set_particles(bool on) {
	s_particlesOn = on;
}

void afx_set_condition(Afx* a, u32 hash, u32 also) {
	if (a) {
		a->cond = hash;
		a->cond2 = hash ? also : 0;
	}
}

// 0x602220.
void afx_clear_trigger(Afx* a, int slot) {
	if (!a || slot < 0 || slot >= AFX_SLOTS || !a->slotActive[slot])
		return;
	for (int k = 0; k < AFX_GROUP_MAX; k++) {
		int ei = a->nInst + slot * AFX_GROUP_MAX + k;
		freeHolder(a, ei);
		a->em[ei].used = false;
	}
	a->slotActive[slot] = false;
}

// 0x6054b0: a fresh copy of each group member (0x48b830 zeroes it, then takes
// the data and the matrix), with identity orientation at `pos`, state 1, no
// condition and no inherited velocity.
void afx_set_trigger(Afx* a, int slot, const float pos[3]) {
	if (!a || slot < 0 || slot >= AFX_SLOTS)
		return;
	if (a->slotActive[slot]) {
		const float* q = a->slotPos[slot];
		if (fabsf(q[0] - pos[0]) <= 0.001f && fabsf(q[1] - pos[1]) <= 0.001f &&
				fabsf(q[2] - pos[2]) <= 0.001f)
			return;
		afx_clear_trigger(a, slot);
	}
	for (int k = 0; k < a->groupCount[slot]; k++) {
		int ei = a->nInst + slot * AFX_GROUP_MAX + k;
		AfxEmitter* e = &a->em[ei];
		memset(e, 0, sizeof(*e));
		e->t = &a->tmpl[a->groupTmpl[slot][k]];
		e->m[0] = e->m[5] = e->m[10] = e->m[15] = 1.0f;
		e->m[12] = pos[0];
		e->m[13] = pos[1];
		e->m[14] = pos[2];
		e->state = 1;
		e->used = true;
		addEmitter(a, e);
	}
	a->slotActive[slot] = true;
	memcpy(a->slotPos[slot], pos, sizeof(a->slotPos[slot]));
	alog("afx: slot %d at %.1f,%.1f,%.1f (%d emitters)", slot, pos[0], pos[1], pos[2],
		a->groupCount[slot]);
}

// Smokeable::Init's copies (afxCopyEmitter 0x800C0BFC, state 1) started by
// AddAutoEmitter (0x800C1208): each in a free prop emitter, one whose
// particles from an earlier prop are gone too.
int afx_attach(Afx* a, u32 group, const float m[16], s16 out[AFX_PROP_EMITTERS]) {
	if (!a || !group)
		return 0;
	int g = 0;
	while (g < a->nProp && a->propHash[g] != group)
		g++;
	if (g == a->nProp)
		return 0;
	int n = 0, ei = a->attachBase;
	for (int k = 0; k < a->propCount[g] && k < AFX_PROP_EMITTERS; k++) {
		while (ei < a->nEm && (a->em[ei].used || a->em[ei].holder))
			ei++;
		if (ei == a->nEm)
			break;
		AfxEmitter* e = &a->em[ei];
		memset(e, 0, sizeof(*e));
		e->t = &a->tmpl[a->propTmpl[g][k]];
		memcpy(e->m, m, sizeof(e->m));
		e->state = 1;
		e->used = true;
		addEmitter(a, e);
		out[n++] = (s16)ei;
	}
	return n;
}

// ServiceEmitters (0x801D4418): the matrix and the velocity the particles
// inherit (+0x40, +0x80).
void afx_attach_move(Afx* a, s16 h, const float m[16], const float vel[3]) {
	if (!a || h < a->attachBase || h >= a->nEm || !a->em[h].used)
		return;
	AfxEmitter* e = &a->em[h];
	if (m)
		memcpy(e->m, m, sizeof(e->m));
	if (vel)
		memcpy(e->inherit, vel, sizeof(e->inherit));
}

// RemoveAutoEmitter (0x800C1398) with RemoveReferencesToEmitter (0x800C15F0):
// the emitter goes and its particles stay, aged and drawn through the holder
// until the last one dies.
void afx_detach(Afx* a, s16 h) {
	if (!a || h < a->attachBase || h >= a->nEm)
		return;
	a->em[h].used = false;
}

int afx_tmpl_count(const Afx* a) {
	return a ? a->nTmpl : 0;
}

u32 afx_tmpl_pos_type(const Afx* a, int tmpl) {
	return a && tmpl >= 0 && tmpl < a->nTmpl ? a->tmpl[tmpl].posType : 0;
}

// afxCopyEmitter (Speed.elf 0x800C0BFC) into a CarRenderInfo effect list: the
// copy keeps the template's zeroed state, so DoTimeStep never emits it on its
// own; CarRenderInfo::TriggerEffect fires it (afx_burst).
s16 afx_car_emitter(Afx* a, int tmpl) {
	if (!a || tmpl < 0 || tmpl >= a->nTmpl)
		return -1;
	for (int ei = a->attachBase; ei < a->nEm; ei++) {
		AfxEmitter* e = &a->em[ei];
		if (e->used || e->holder)
			continue;
		memset(e, 0, sizeof(*e));
		e->t = &a->tmpl[tmpl];
		e->m[0] = e->m[5] = e->m[10] = e->m[15] = 1.0f;
		e->state = 3;
		e->used = true;
		e->car = true;
		addEmitter(a, e);
		return (s16)ei;
	}
	return -1;
}

// CarRenderInfo::TriggerEffect (Speed.elf 0x80238A78) for one copy at one
// position: the emitter's matrix and inherited velocity set, then
// TriggerEmitter with the step negated -- one burst whatever the state.
void afx_burst(Afx* a, s16 h, const float m[16], const float vel[3], float intensity,
		const float* color, const float view[3]) {
	if (!a || h < a->attachBase || h >= a->nEm || !a->em[h].used || !(intensity > 0.0f))
		return;
	AfxEmitter* e = &a->em[h];
	memcpy(e->m, m, sizeof(e->m));
	if (vel)
		memcpy(e->inherit, vel, sizeof(e->inherit));
	e->color = color;
	triggerEmitter(a, h, -intensity, view);
}

// DoTimeStep (0x492e10).
void afx_update(Afx* a, float dt, const float view[3]) {
	if (!a || !(dt > 0.0f))
		return;

	for (int i = 0; i < a->nEm; i++) {
		AfxEmitter* e = &a->em[i];
		if (!e->used || (e->cond != 0 && e->cond != a->cond && (a->cond2 == 0 || e->cond != a->cond2)))
			continue;
		if (e->state == 1)
			triggerEmitter(a, i, dt, view);
		if (e->timer != 0.0f && e->state <= 1) {
			e->timer -= dt;
			if (e->timer <= 0.0f) {
				float left = e->timer;
				e->state ^= 1;
				float period = emitterPeriod(e);
				e->timer = period == 0.0f ? 0.0f : period + left;
			}
		}
	}

	const int dtTicks = (int)(dt * 1024.0f);
	for (s16 h = a->hHead; h >= 0; ) {
		AfxEmitter* e = &a->em[h];
		const s16 hn = e->hNext;
		const AfxTmpl* t = e->t;
		const int fpsTicks = t->fps ? (int)(1024.0f / (float)t->fps) : 0;
		const int cellMask = (1 << (t->ubits + t->vbits)) - 1;
		const float negG = -t->gravity, drag = t->drag;
		// DoTimeStep (0x800C2634): a template with +0x1e set, held for a car
		// and inheriting at all (+0x54 > 0), has every particle carried this
		// step by the car's velocity times its own inherit fraction -- the
		// nitrous flame stays at the exhaust however fast the car goes.
		const bool withCar = t->carInherit && e->car && t->inherit > 0.0f;
		for (int l = 0; l < 2; l++) {
			u16 prev = AFX_NIL;
			for (u16 i = e->head[l]; i != AFX_NIL; ) {
				AfxParticle* p = &a->pool[i];
				const u16 ni = p->next;
				const int old = (s16)p->life;
				const int rem = old - dtTicks;
				if (rem <= 0) {
					if (prev != AFX_NIL)
						a->pool[prev].next = ni;
					else
						e->head[l] = ni;
					if (e->tail[l] == i)
						e->tail[l] = prev;
					e->count[l]--;
					p->next = a->freeHead;
					a->freeHead = i;
					a->live--;
					i = ni;
					continue;
				}
				if (fpsTicks)
					p->cell = (u8)((old / fpsTicks - rem / fpsTicks + p->cell) & cellMask);
				const float spd = (float)p->speed * (1.0f / 128.0f);
				float v[3];
				for (int k = 0; k < 3; k++)
					v[k] = (float)p->dir[k] * (1.0f / 127.0f) * spd;
				v[0] += (v[0] < 0.0f ? 1.0f : -1.0f) * v[0] * v[0] * drag * dt;
				v[1] += (v[1] < 0.0f ? 1.0f : -1.0f) * v[1] * v[1] * drag * dt;
				v[2] = ((v[2] < 0.0f ? 1.0f : -1.0f) * v[2] * v[2] * drag + negG) * dt + v[2];
				for (int k = 0; k < 3; k++)
					p->pos[k] += v[k] * dt;
				if (withCar) {
					const float f = (float)p->inherit * (1.0f / 255.0f) * dt;
					for (int k = 0; k < 3; k++)
						p->pos[k] += e->inherit[k] * f;
				}
				const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
				p->speed = (u16)(int)(len * 128.0f);
				const float inv = len > 1e-5f ? 1.0f / len : 1e10f;
				for (int k = 0; k < 3; k++)
					p->dir[k] = (s8)(int)(v[k] * inv * 127.0f);
				p->life = (u16)rem;
				prev = i;
				i = ni;
			}
		}
		if (e->count[0] == 0 && e->count[1] == 0)
			freeHolder(a, h);
		h = hn;
	}
}

// ------------------------------------------------------------------ render

// The aspect table at 0x7f3944, by the particle's size index. Index 3 reads
// past it in retail (two denormals), i.e. nothing visible.
static const float kAspect[4][2] = {
	{ 1.0f, 1.0f }, { 1.0f, 0.5f }, { 0.5f, 1.0f }, { 0.0f, 0.0f }
};

static inline void cubic(float* out, const float* c, float t, int n) {
	const float t2 = t * t, t3 = t2 * t;
	for (int i = 0; i < n; i++)
		out[i] = t3 * c[i] + t2 * c[4 + i] + t * c[8 + i] + c[12 + i];
}

// A debug probe (#317): up to `max` of emitter `h`'s particles, oldest first,
// with their life fraction, colour and size as afx_draw() would draw them.
int afx_last_quads(const Afx* a) { return a ? a->lastQuads : -2; }
float afx_last_area(const Afx* a) { return a ? a->lastArea : 0.0f; }

int afx_probe(const Afx* a, s16 h, int max, float (*pos)[3], float* tl, float (*col)[4],
		float* size) {
	if (!a || h < 0 || h >= a->nEm || !a->em[h].used)
		return 0;
	const AfxEmitter* e = &a->em[h];
	const AfxTmpl* t = e->t;
	int n = 0;
	for (int l = 0; l < 2; l++)
		for (u16 i = e->head[l]; i != AFX_NIL && n < max; i = a->pool[i].next) {
			const AfxParticle* p = &a->pool[i];
			memcpy(pos[n], p->pos, sizeof(pos[n]));
			tl[n] = 1.0f - (float)(s16)p->life * (1.0f / 1024.0f) / t->life;
			cubic(col[n], e->color ? e->color : t->color, tl[n], 4);
			float sz[2];
			cubic(sz, t->size, tl[n], 2);
			size[n] = sz[0];
			n++;
		}
	return n;
}

static inline void putVert(AfxVert* v, const float p[3], float r0, float r1,
		const float* r, const float* u, const u8 rgba[4], float tu, float tv) {
	for (int k = 0; k < 3; k++)
		v->pos[k] = p[k] + r[k] * r0 + u[k] * r1;
	memcpy(v->rgba, rgba, 4);
	v->uv[0] = tu;
	v->uv[1] = 1.0f - tv;   // the PICA's V runs up the texture
}

// RenderEffects (0x48bdf0): billboards for every holder, then the oriented
// quads for every holder, holders in creation order and particles oldest
// first. Consecutive holders on one texture share a draw.
void afx_draw(Afx* a, const C3D_Mtx* vp, const AfxView* view) {
	if (!a || !a->live || !s_ready)
		return;
	AfxVert* vb = a->vbuf[a->frame];
	a->lastQuads = -1;
	int quads = 0, nRuns = 0;
	float area = 0.0f;
	// The per-emitter screen-size cap is in 480-line pixels on the PC; this
	// measures the 240-line top screen.
	const float projScale = view->projScale;

	for (int pass = 0; pass < 2; pass++) {
		for (s16 h = a->hHead; h >= 0 && quads < AFX_POOL; h = a->em[h].hNext) {
			const AfxEmitter* e = &a->em[h];
			const AfxTmpl* t = e->t;
			if (!e->count[pass] || t->tex == AFX_NO_TEX || !a->texValid[t->tex])
				continue;
			if (!nRuns || a->runs[nRuns - 1].tex != t->tex) {
				a->runs[nRuns].firstQuad = (u16)quads;
				a->runs[nRuns].quads = 0;
				a->runs[nRuns].tex = t->tex;
				nRuns++;
			}
			const float invLife = 1.0f / t->life;
			float maxS = t->maxScreen;
			if (maxS == 0.0f || maxS >= 512.0f)
				maxS = 512.0f;
			maxS *= 0.5f;
			if (s_screenCap > 0.0f && maxS > s_screenCap)
				maxS = s_screenCap;
			// #521: what a particle adds is tex * colour * 2 * alpha; under
			// half a step of the 8-bit screen it adds nothing, so it is not
			// drawn (fill saved, the picture the same).
			const bool additive = a->texBlend[t->tex] == 2;
			const float du = 1.0f / (float)(1 << t->ubits);
			const float dv = 1.0f / (float)(1 << t->vbits);
			const int umask = (1 << t->ubits) - 1, vmask = (1 << t->vbits) - 1;
			const int start = quads;
			for (u16 i = e->head[pass]; i != AFX_NIL && quads < AFX_POOL;
					i = a->pool[i].next) {
				const AfxParticle* p = &a->pool[i];
				const float tl = 1.0f - (float)(s16)p->life * (1.0f / 1024.0f) * invLife;
				float col[4], sz[2];
				cubic(col, e->color ? e->color : t->color, tl, 4);
				cubic(sz, t->size, tl, 2);
				// The curve is R, G, B, A. The GameCube packs it that way
				// (c0 << 24 | c1 << 16 | c2 << 8 | c3, a GXColor, Speed.elf
				// 0x800c38b4), and so every event's marker matches its minimap
				// colour. The PC packs c2 as red (0x48cbf0), which swaps red
				// and blue: drag markers come out red, circuit ones pink.
				u8 rgba[4];
				for (int k = 0; k < 4; k++) {
					float c = col[k] > 0.0f ? (col[k] < 255.0f ? col[k] : 255.0f) : 0.0f;
					rgba[k] = (u8)(int)c;
				}
				const int peak = rgba[0] > rgba[1] ? (rgba[0] > rgba[2] ? rgba[0] : rgba[2])
				                                   : (rgba[1] > rgba[2] ? rgba[1] : rgba[2]);
				if (s_lean && (additive ? peak * rgba[3] < 64 : rgba[3] == 0))
					continue;
				float size = sz[0];
				const float depth = (p->pos[0] - view->eye[0]) * view->fwd[0] +
				                    (p->pos[1] - view->eye[1]) * view->fwd[1] +
				                    (p->pos[2] - view->eye[2]) * view->fwd[2];
				float screen = 0.0f;
				if (size > 0.0f && depth > 0.0f)
					screen = size / depth * projScale;
				if (maxS < screen)
					size = depth / projScale * maxS;
				int spin = (int)sz[1];
				if (p->rot & 1)
					spin = -spin;
				const u16 angle = (u16)(p->rot + spin);
				const float u0 = (float)(p->cell & umask) * du;
				const float v0 = (float)((p->cell >> t->ubits) & vmask) * dv;
				const float u1 = u0 + du, v1 = v0 + dv;
				const int si = p->orientSize >> 4;
				const float hw = size * kAspect[si][0] * 0.5f;
				const float hh = size * kAspect[si][1] * 0.5f;
				if (depth > 0.0f) {
					const float k = projScale / depth;
					area += (2.0f * hw * k) * (2.0f * hh * k);
				}
				AfxVert* q = vb + quads * 4;
				if (pass == 0) {
					const float* R = view->right;
					const float* U = view->up;
					putVert(q + 0, p->pos, -hw,  hh, R, U, rgba, u0, v0);
					putVert(q + 1, p->pos,  hw,  hh, R, U, rgba, u1, v0);
					putVert(q + 2, p->pos,  hw, -hh, R, U, rgba, u1, v1);
					putVert(q + 3, p->pos, -hw, -hh, R, U, rgba, u0, v1);
				} else {
					float R[3], U[3];
					const int o = p->orientSize & 0xF;
					const float c = cosU(angle), s = sinU(angle);
					if (o == 1) {
						R[0] = c;  R[1] = s;  R[2] = 0;
						U[0] = -s; U[1] = c;  U[2] = 0;
					} else if (o == 2) {
						R[0] = c;  R[1] = 0;  R[2] = -s;
						U[0] = s;  U[1] = 0;  U[2] = c;
					} else if (o == 3) {
						R[0] = 0;  R[1] = c;  R[2] = s;
						U[0] = 0;  U[1] = -s; U[2] = c;
					} else {
						// Camera-facing, turned in the view plane.
						for (int k = 0; k < 3; k++) {
							R[k] =  c * view->right[k] + s * view->up[k];
							U[k] = -s * view->right[k] + c * view->up[k];
						}
					}
					putVert(q + 0, p->pos, -hw,  hh, R, U, rgba, u0, v0);
					putVert(q + 1, p->pos, -hw, -hh, R, U, rgba, u1, v0);
					putVert(q + 2, p->pos,  hw, -hh, R, U, rgba, u1, v1);
					putVert(q + 3, p->pos,  hw,  hh, R, U, rgba, u0, v1);
				}
				quads++;
			}
			a->runs[nRuns - 1].quads = (u16)(a->runs[nRuns - 1].quads + (quads - start));
		}
	}
	a->lastQuads = quads;
	a->lastArea = area;
	if (!quads)
		return;
	GSPGPU_FlushDataCache(vb, (u32)quads * 4 * sizeof(AfxVert));

	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);           // v0 position
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);   // v1 colour
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);           // v2 texcoord
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, vb, sizeof(AfxVert), 3, 0x210);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uMvp, vp);

	C3D_CullFace(GPU_CULL_NONE);
	// #521: a texel the texture leaves at zero alpha adds nothing in either
	// blend, so it is dropped before the blend's read-modify-write (the
	// picture is the same; the marker haze's soft edges are mostly zero).
	C3D_AlphaTest(s_lean, s_lean ? GPU_GREATER : GPU_ALWAYS, 0);
	// Tested against the street, never written: particles do not hide each
	// other or anything drawn after them.
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	// Every PC world pixel shader is "mul r0, t0, v0; add r0.xyz, r0, r0":
	// colour at twice texture times vertex, alpha not doubled. The PS2-era
	// data relies on it (the smoke textures are mid grey). The alpha is full
	// range on PC (#242: SFX_SMOKE_B_CS reaches 136, XETT_GLASSB 221, the
	// rings and sparks 255, HEADLIGHTGLOW 254 -- a PS2 texture stops at 0x80),
	// so the GameCube TEV's alpha doubling, made for its own data, is not
	// wanted here.
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));

	int boundBlend = -1;
	for (int r = 0; r < nRuns; r++) {
		const AfxRun* run = &a->runs[r];
		if (!run->quads)
			continue;
		// The texture's alphaBlendType: 2 is additive, the rest blend.
		const int blend = a->texBlend[run->tex] == 2;
		if (blend != boundBlend) {
			if (blend)
				C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE,
					GPU_SRC_ALPHA, GPU_ONE);
			else
				C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
					GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
			boundBlend = blend;
		}
		C3D_TexBind(0, &a->tex[run->tex]);
		C3D_DrawElements(GPU_TRIANGLES, (int)run->quads * 6, C3D_UNSIGNED_SHORT,
			a->ibuf + run->firstQuad * 6);
	}
	// Put the blend and depth state back the way model3d.c leaves it.
	// C2D_Prepare does not reset either, and the ring is usually the last
	// run: an additive blend left up draws the whole HUD additively, which
	// washes the minimap's markers out over the map.
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
		GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_1);
	a->frame ^= 1;
}

void afx_stats(const Afx* a, int* emitters, int* particles) {
	int n = 0;
	for (int i = 0; a && i < a->nEm; i++)
		n += a->em[i].used;
	if (emitters)
		*emitters = n;
	if (particles)
		*particles = a ? a->live : 0;
}
