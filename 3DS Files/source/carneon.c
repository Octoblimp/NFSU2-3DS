// See carneon.h. Every constant below is Speed.elf's, and the comments name
// the addresses they were read from.
#include "carneon.h"
#include "shaders.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEON_MAGIC     0x4E53464Eu  // 'NFSN' little-endian
#define NEON_VERSION   1
#define NEON_HDR       0x30
#define NEON_COLOUR    12
#define NEON_MAX_COLS  256
#define TEXFMT_RGB565  0
#define TEXFMT_RGBA8   2

// RenderNeon's underglow call (0x80237864): the tube's half-width, its alpha
// scale, and the flag that shortens the second tube.
#define NEON_WIDTH     0.045f       // 0x8033c03c
#define NEON_ALPHA_MUL 1.0f         // 0x8033c040
// Its engine and trunk calls (#506): each set twice, a thin bright tube then
// a wide one at a share of the alpha, faded by the hood's / trunk's
// animation time (0 shut: 0x8033c044), never shortened.
#define NEON_ENGINE_W1 0.0175f      // 0x8033c048
#define NEON_ENGINE_W2 0.175f       // 0x8033c05c
#define NEON_ENGINE_A2 0.3f         // 0x8033c058
#define NEON_TRUNK_W1  0.0225f      // 0x8033c060
#define NEON_TRUNK_W2  0.07875f     // 0x8033c068
#define NEON_TRUNK_A2  0.35f        // 0x8033c064
// 0x80237cac: the fade. Both distances are scaled by (13018 / fov)^2, which
// is 1.0 at the chase camera's 0x32DC, so ~10 m and ~75 m there. In between
// the tube fades out while it widens to twenty times its width.
#define NEON_FOV_REF   13018.0f     // 0x8033c088
#define NEON_NEAR      10.0f        // 0x8033c08c
#define NEON_FAR       75.0f        // 0x8033c090
#define NEON_GROW      20.0f        // 0x8033c094
#define NEON_DIST_EPS  5e-11f       // 0x8033c06c: below this, distance 0
// 0x80237ef0: with the flag set, tube 1 loses 6% of its length at each end,
// so it does not overlap the two front corner tubes it meets.
#define NEON_SHRINK    0.06f        // 0x8033c0a0
// Render's pulse (0x8023ad88): WorldTimer counts 4000 ticks a second
// (0x8033c1cc = 0.00025), and three sines of it at these rates, each wrapped
// to one turn first (0x8033c1dc, 0x8033c1e0).
#define NEON_RATE_A    6.1f         // 0x8033c1d0
#define NEON_RATE_B    7.2f         // 0x8033c1d4
#define NEON_RATE_C    4.7f         // 0x8033c1d8
#define NEON_FLICKER   1.2f         // 0x8033c1a4
#define NEON_TWO_PI    6.2831855f
#define NEON_PI        3.1415927f
// Every packed channel is clamped to 0..0xFE (0x8023b014).
#define NEON_CHAN_MAX  0xFE

// Four quads a tube, two triangles a quad.
#define NEON_TUBE_VERTS (4 * 6)
// One car's most: the underglow, and the engine and trunk sets twice each.
#define NEON_VERTS     ((CARKIT_NEON_SEGMENTS + 2 * CARKIT_ENGINE_NEON + \
                         2 * CARKIT_TRUNK_NEON) * NEON_TUBE_VERTS)
// The vertices of the draws in flight: every car's, once per eye (stereo.h),
// so a frame's vertices are never rewritten while the GPU may still read
// them. Each draw takes the next run of the pool it needs, back to the start
// when the rest is too short: room for sixteen underglows (two buffers taken
// in turn were overwritten as soon as a third car had neon) and one frontend
// car's every set in both eyes.
#define NEON_POOL      (16 * CARKIT_NEON_SEGMENTS * NEON_TUBE_VERTS + 2 * NEON_VERTS)

typedef struct {
	float pos[3];
	u8    rgba[4];
	float uv[2];
} NeonVert;

typedef struct {
	u32 hash;
	u8  base[4];   // UpdateNeonColours: RED, GREEN, BLUE, ALPHA
	u8  delta[4];  // and RED2 - RED etc., stored as a byte and read back
	               // unsigned (lbz, 0x8023aef0), so a darker second colour
	               // wraps rather than dims -- no shipped part has one
} NeonColour;

static void (*s_log)(const char*);
static NeonColour* s_cols;
static int s_nCols;
static C3D_Tex s_tex;
static bool s_texOk;
static NeonVert* s_vbuf;    // NEON_POOL vertices
static int s_used;          // the pool's next free vertex

static DVLE_s*         s_dvle;   // the shared blob\'s (shaders.h, #352)
static shaderProgram_s s_prog;
static int             s_uMvp = -1;
static bool            s_shader;

void carneon_set_log(void (*sink)(const char*)) { s_log = sink; }

static void nlog(const char* m) {
	if (s_log)
		s_log(m);
}

static u32 rd32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool initShader(void) {
	if (s_shader)
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
	s_shader = true;
	return true;
}

static bool isPow2(u32 v) { return v && !(v & (v - 1)); }

// NEONBLUR's mip chain, already tiled by the packer, one C3D_TexLoadImage per
// level the way afx.c and model3d.c upload theirs.
static bool loadTex(FILE* f, const u8* h, long size) {
	const u32 w = rd32(h + 0x10), ht = rd32(h + 0x14), fld = rd32(h + 0x18);
	const u32 off = rd32(h + 0x1c), bytes = rd32(h + 0x20);
	const u32 fmt = fld & 0xff, mlvl = (fld >> 8) & 0xf;
	if (!isPow2(w) || !isPow2(ht) || w < 8 || ht < 8 || w > 1024 || ht > 1024)
		return false;
	if ((w >> mlvl) < 8 || (ht >> mlvl) < 8)
		return false;
	const u32 bpp = fmt == TEXFMT_RGBA8 ? 4 : 2;
	const u32 lvl0 = w * ht * bpp;
	u32 need = lvl0;
	for (u32 lv = 0, sz = lvl0; lv < mlvl; lv++) {
		sz >>= 2;
		need += sz;
	}
	if (need > bytes || (u64)off + bytes > (u64)size)
		return false;
	u8* buf = (u8*)malloc(need);
	if (!buf)
		return false;
	bool ok = fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, need, f) == need;
	const GPU_TEXCOLOR gfmt = fmt == TEXFMT_RGBA8 ? GPU_RGBA8
	                        : fmt == TEXFMT_RGB565 ? GPU_RGB565 : GPU_RGBA4;
	C3D_TexInitParams tp = { (u16)w, (u16)ht, (u8)mlvl, gfmt, GPU_TEX_2D, false };
	if (ok)
		ok = C3D_TexInitWithParams(&s_tex, NULL, tp);
	if (ok) {
		const u8* src = buf;
		u32 sz = lvl0;
		for (u32 lv = 0; lv <= mlvl; lv++) {
			C3D_TexLoadImage(&s_tex, src, GPU_TEXFACE_2D, (int)lv);
			src += sz;
			sz >>= 2;
		}
		C3D_TexFlush(&s_tex);
		C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
		if (mlvl)
			C3D_TexSetFilterMipmap(&s_tex, GPU_LINEAR);
		C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	free(buf);
	return ok;
}

bool carneon_open(const char* path) {
	carneon_close();
	char b[200];
	FILE* f = path ? fopen(path, "rb") : NULL;
	if (!f) {
		snprintf(b, sizeof(b), "neon: no %s -- cars draw without neon (run tools/neon_pack.py)",
			path ? path : "NEON.nfsn");
		nlog(b);
		return false;
	}
	u8 h[NEON_HDR];
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	bool ok = size >= NEON_HDR && fread(h, 1, NEON_HDR, f) == NEON_HDR &&
	          rd32(h) == NEON_MAGIC && rd32(h + 4) == NEON_VERSION;
	const u32 n = ok ? rd32(h + 0x08) : 0, colOff = ok ? rd32(h + 0x0c) : 0;
	ok = ok && n > 0 && n <= NEON_MAX_COLS && (u64)colOff + n * NEON_COLOUR <= (u64)size;
	u8* raw = ok ? (u8*)malloc(n * NEON_COLOUR) : NULL;
	ok = ok && raw && fseek(f, (long)colOff, SEEK_SET) == 0 &&
	     fread(raw, 1, n * NEON_COLOUR, f) == n * NEON_COLOUR;
	if (ok) {
		s_cols = (NeonColour*)malloc(n * sizeof(NeonColour));
		ok = s_cols != NULL;
	}
	if (ok) {
		for (u32 i = 0; i < n; i++) {
			const u8* r = raw + i * NEON_COLOUR;
			s_cols[i].hash = rd32(r);
			for (int c = 0; c < 4; c++) {
				s_cols[i].base[c] = r[4 + c];
				s_cols[i].delta[c] = (u8)(r[8 + c] - r[4 + c]);
			}
		}
		s_nCols = (int)n;
		s_texOk = loadTex(f, h, size);
	}
	free(raw);
	fclose(f);
	if (ok && s_texOk && initShader()) {
		s_vbuf = (NeonVert*)linearAlloc(NEON_POOL * sizeof(NeonVert));
		s_used = 0;
		ok = s_vbuf != NULL;
	} else {
		ok = false;
	}
	if (!ok) {
		snprintf(b, sizeof(b), "neon: %s is not a v%d NEON.nfsn with its texture -- no neon",
			path, NEON_VERSION);
		nlog(b);
		carneon_close();
		return false;
	}
	snprintf(b, sizeof(b), "neon: %d colours, NEONBLUR %ux%u", s_nCols,
		(unsigned)s_tex.width, (unsigned)s_tex.height);
	nlog(b);
	return true;
}

void carneon_close(void) {
	free(s_cols);
	s_cols = NULL;
	s_nCols = 0;
	if (s_texOk)
		C3D_TexDelete(&s_tex);
	s_texOk = false;
	if (s_vbuf)
		linearFree(s_vbuf);
	s_vbuf = NULL;
}

static const NeonColour* lookup(u32 hash) {
	int lo = 0, hi = s_nCols;
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		if (s_cols[mid].hash == hash)
			return &s_cols[mid];
		if (s_cols[mid].hash < hash) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

void carneon_setup(CarNeon* n, const CarKitInfo* info) {
	carneon_clear(n);
	if (!info)
		return;
	n->segs = info->neonSegs;
	memcpy(n->seg, info->neon, sizeof(n->seg));
	n->part = info->neonPart;
	n->engineSegs = info->engineNeonSegs;
	memcpy(n->engineSeg, info->engineNeon, sizeof(n->engineSeg));
	n->enginePart = info->engineNeonPart;
	n->trunkSegs = info->trunkNeonSegs;
	memcpy(n->trunkSeg, info->trunkNeon, sizeof(n->trunkSeg));
	n->trunkPart = info->trunkNeonPart;
	n->bayLit[0] = info->bayLit[0];
	n->bayLit[1] = info->bayLit[1];
}

void carneon_clear(CarNeon* n) {
	memset(n, 0, sizeof(*n));
}

void carneon_tick(CarNeon* n, float dt) {
	if (dt > 0.0f)
		n->clock += dt;
}

// x less its whole turns, as Render wraps each phase before bSin.
static float wrapTurn(float x) {
	const int k = (int)(x / NEON_TWO_PI);
	return x - (float)(k + k) * NEON_PI;
}

static int clampChan(int v) {
	return v <= 0 ? 0 : v > NEON_CHAN_MAX ? NEON_CHAN_MAX : v;
}

// Render, 0x8023ad88-0x8023b0b8: the underglow's packed colour this frame
// (+0x358). The third sine scales the part's second-minus-first colour in;
// the product of the other two adds a flicker of at most one step to alpha.
static u32 pulse(const NeonColour* c, double clock) {
	const float t = (float)clock;
	const float a = wrapTurn(t * NEON_RATE_A);
	const float b = wrapTurn(t * NEON_RATE_B);
	const float s = wrapTurn(t * NEON_RATE_C);
	const int flick = (int)(fabsf(sinf(a)) * fabsf(sinf(b)) * NEON_FLICKER);
	const float f = fabsf(sinf(s));
	int ch[4];
	for (int i = 0; i < 4; i++)
		ch[i] = clampChan(c->base[i] + (int)(f * (float)c->delta[i]) + (i == 3 ? flick : 0));
	return (u32)ch[0] | ((u32)ch[1] << 8) | ((u32)ch[2] << 16) | ((u32)ch[3] << 24);
}

u32 carneon_colour(const CarNeon* n) {
	const NeonColour* col = n && s_cols ? lookup(n->part) : NULL;
	return col ? pulse(col, n->clock) : 0;
}

// Render's +0x364 / +0x37c: the engine's and the trunk's pulsed colours, by
// the same pulse as the underglow's (one phase for all three), 0 without the
// part. The constructor zeroes them, and Render writes each only with its
// part, so 0 is what the light add then reads.
static u32 bayColour(const CarNeon* n, int b) {
	const u32 part = b ? n->trunkPart : n->enginePart;
	const NeonColour* col = s_cols && part ? lookup(part) : NULL;
	return col ? pulse(col, n->clock) : 0;
}

void carneon_bay_light(const CarNeon* n, float out[2][3]) {
	memset(out, 0, 2 * 3 * sizeof(float));
	for (int b = 0; n && b < 2; b++) {
		if (!n->bayLit[b])
			continue;
		const u32 c = bayColour(n, b);
		// Speed.elf 0x8033c1fc and the double at 0x8033c200 (read as a float
		// it looks like 0.9375; SPEED2.EXE's twin at 0x7a88b0 is the same
		// double): 1/256 on every channel.
		const float a = (float)(c >> 24) * (1.0f / 256.0f);
		for (int ch = 0; ch < 3; ch++)
			out[b][ch] = (float)((c >> (8 * ch)) & 0xff) * (1.0f / 256.0f) * a;
	}
}

// Car space to world, the transform the body is drawn with: p * rot + origin.
static void toWorld(float out[3], const float p[3], const float rot[3][3],
		const float origin[3]) {
	for (int i = 0; i < 3; i++)
		out[i] = p[0] * rot[0][i] + p[1] * rot[1][i] + p[2] * rot[2][i] + origin[i];
}

// GetScreenPosition, kept to the one thing RenderNeon uses it for: a direction
// in top-screen pixels. False for a point at or behind the eye.
static bool screenPos(const CarNeonView* v, const float p[3], float* sx, float* sy) {
	const float d[3] = { p[0] - v->eye[0], p[1] - v->eye[1], p[2] - v->eye[2] };
	const float z = d[0] * v->fwd[0] + d[1] * v->fwd[1] + d[2] * v->fwd[2];
	if (z <= 1e-4f)
		return false;
	*sx = (d[0] * v->right[0] + d[1] * v->right[1] + d[2] * v->right[2]) / z * v->pxX;
	*sy = (d[0] * v->up[0] + d[1] * v->up[1] + d[2] * v->up[2]) / z * v->pxY;
	return true;
}

static void put(NeonVert* q, const float p[3], const u8 rgba[4], float u, float tv) {
	memcpy(q->pos, p, sizeof(q->pos));
	memcpy(q->rgba, rgba, 4);
	q->uv[0] = u;
	q->uv[1] = 1.0f - tv;   // the PICA's V runs up the texture
}

// Corners in RenderNeon's order, as the two triangles 0-1-2 and 0-2-3.
static NeonVert* quad(NeonVert* q, const float c[4][3], const float uv[4][2],
		const u8 rgba[4]) {
	static const int k[6] = { 0, 1, 2, 0, 2, 3 };
	for (int i = 0; i < 6; i++)
		put(q + i, c[k[i]], rgba, uv[k[i]][0], uv[k[i]][1]);
	return q + 6;
}

void carneon_light_env(const float gain[3]) {
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	if (!gain) {
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE,
			GPU_SRC_ALPHA, GPU_ONE);
		return;
	}
	// The alpha first, as the blend would have, then the gain: a stage clamps
	// at 1, and the gain alone would clip a colour its alpha was about to dim.
	env = C3D_GetTexEnv(1);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA,
		GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	env = C3D_GetTexEnv(2);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_4);
	u32 k = 0xFF000000u;
	for (int c = 0; c < 3; c++) {
		const float q = gain[c] * (255.0f / 4.0f) + 0.5f;
		k |= (u32)(q >= 255.0f ? 255.0f : q) << (8 * c);
	}
	C3D_TexEnvColor(env, k);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
}

void carneon_light_env_end(void) {
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
		GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_TexEnvScale(C3D_GetTexEnv(0), C3D_RGB, GPU_TEVSCALE_1);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
}

// RenderNeon's one set (0x80237af4): the tubes seg[0..segs), `width` wide
// and alpha-scaled by `alphaMul` at this distance, in `packed`; `shrink`
// shortens tube 1 (the underglow's flag). Into `q`, which it returns past
// what it wrote.
static NeonVert* emitSet(NeonVert* q, const CarKitNeonSeg* seg, int segs, float width,
		float alphaMul, u32 packed, bool shrink, const CarNeonView* v,
		const float rot[3][3], const float origin[3], float scale, float fade) {
	const float w = width * scale;
	int alpha = (int)((float)(packed >> 24) * (fade * alphaMul));
	if (alpha == 0)
		return q;
	// PORT: SPEED2.EXE's RenderNeon (0x60d996) doubles the tube's alpha
	// before the clamp, where Speed.elf's (0x80237d88) takes it as it is. The
	// PC is the base, and without it the tubes add half what they do there.
	alpha += alpha;
	if (alpha >= NEON_CHAN_MAX)
		alpha = NEON_CHAN_MAX;
	const u8 rgba[4] = { (u8)packed, (u8)(packed >> 8), (u8)(packed >> 16), (u8)alpha };

	static const float uvSide[4][2] = { { .5f, .5f }, { .5f, .5f }, { .5f, 0 }, { .5f, 0 } };
	static const float uvCapA[4][2] = { { .5f, 0 }, { 0, 0 }, { 0, 1 }, { .5f, 1 } };
	static const float uvCapB[4][2] = { { .5f, 0 }, { 1, 0 }, { 1, 1 }, { .5f, 1 } };
	for (int i = 0; i < segs; i++) {
		float A[3], B[3];
		toWorld(A, seg[i].a, rot, origin);
		toWorld(B, seg[i].b, rot, origin);
		if (shrink && i == 1) {
			for (int j = 0; j < 3; j++) {
				const float e = (B[j] - A[j]) * NEON_SHRINK;
				A[j] += e;
				B[j] -= e;
			}
		}
		// The tube's direction on screen, then Q along it and P across it,
		// both lying in the screen's plane.
		float ax, ay, bx, by;
		if (!screenPos(v, A, &ax, &ay) || !screenPos(v, B, &bx, &by))
			continue;
		float qx = bx - ax, qy = by - ay;
		const float ql = sqrtf(qx * qx + qy * qy);
		if (ql < 1e-6f)
			continue;
		qx /= ql;
		qy /= ql;
		float P[3], Q[3];
		for (int j = 0; j < 3; j++) {
			P[j] = (v->up[j] * qx - v->right[j] * qy) * w;
			Q[j] = (v->right[j] * qx + v->up[j] * qy) * w;
		}
		float c[4][3];
		for (int j = 0; j < 3; j++) {
			c[0][j] = A[j];         c[1][j] = B[j];
			c[2][j] = B[j] - P[j];  c[3][j] = A[j] - P[j];
		}
		q = quad(q, c, uvSide, rgba);
		for (int j = 0; j < 3; j++) {
			c[2][j] = B[j] + P[j];  c[3][j] = A[j] + P[j];
		}
		q = quad(q, c, uvSide, rgba);
		for (int j = 0; j < 3; j++) {
			c[0][j] = A[j] - P[j];         c[1][j] = A[j] - P[j] - Q[j];
			c[2][j] = A[j] + P[j] - Q[j];  c[3][j] = A[j] + P[j];
		}
		q = quad(q, c, uvCapA, rgba);
		for (int j = 0; j < 3; j++) {
			c[0][j] = B[j] - P[j];         c[1][j] = B[j] - P[j] + Q[j];
			c[2][j] = B[j] + P[j] + Q[j];  c[3][j] = B[j] + P[j];
		}
		q = quad(q, c, uvCapB, rgba);
	}
	return q;
}

// The second call's colour: the alpha cut to `k` of itself (the int cast
// RenderNeon packs back in), the colour as it is.
static u32 cutAlpha(u32 packed, float k) {
	return (packed & 0xFFFFFFu) | ((u32)(int)((float)(packed >> 24) * k) << 24);
}

void carneon_draw(const CarNeon* n, const C3D_Mtx* vp, const CarNeonView* v,
		const float rot[3][3], const float origin[3], const float gain[3],
		const float bayT[2]) {
	if (!n || !s_texOk || !s_shader || !s_vbuf || !v || !v->fov)
		return;
	// RenderNeon (0x802377f0): which sets draw. The underglow with its
	// part; the engine's with its part and the hood not shut, the trunk's
	// with its part and the trunk not shut (its animation time is 0 shut,
	// which fades the tubes out whatever retail's other trunk tests say).
	const NeonColour* col = n->segs > 0 ? lookup(n->part) : NULL;
	const float hoodT = bayT ? bayT[0] : 0.0f, trunkT = bayT ? bayT[1] : 0.0f;
	const bool eng = n->engineSegs > 0 && hoodT > 0.0f && n->enginePart;
	const bool trk = n->trunkSegs > 0 && trunkT > 0.0f && n->trunkPart;
	if (!col && !eng && !trk)
		return;

	// 0x80237bbc: how far the eye is from the car, against the fov's distances.
	const float dx = origin[0] - v->eye[0], dy = origin[1] - v->eye[1],
	            dz = origin[2] - v->eye[2];
	const float d2 = dx * dx + dy * dy + dz * dz;
	const float d = d2 > NEON_DIST_EPS ? sqrtf(d2) : 0.0f;
	const float k = NEON_FOV_REF / (float)v->fov;
	const float nearD = NEON_NEAR * k * k, farD = NEON_FAR * k * k;
	float scale = 1.0f, fade = 1.0f;
	if (d > nearD) {
		if (d < farD) {
			const float t = (d - nearD) / (farD - nearD);
			fade = 1.0f - t;
			scale = t * NEON_GROW + fade;
		} else {
			fade = 0.0f;
		}
	}

	const int need = (col ? n->segs : 0) + (eng ? 2 * n->engineSegs : 0) +
	                 (trk ? 2 * n->trunkSegs : 0);
	if (s_used + need * NEON_TUBE_VERTS > NEON_POOL)
		s_used = 0;
	NeonVert* const vb = s_vbuf + s_used;
	NeonVert* q = vb;
	if (col)
		q = emitSet(q, n->seg, n->segs, NEON_WIDTH, NEON_ALPHA_MUL, pulse(col, n->clock),
			true, v, rot, origin, scale, fade);
	if (eng) {
		const u32 c = bayColour(n, 0);
		q = emitSet(q, n->engineSeg, n->engineSegs, NEON_ENGINE_W1, hoodT, c, false, v,
			rot, origin, scale, fade);
		q = emitSet(q, n->engineSeg, n->engineSegs, NEON_ENGINE_W2, hoodT,
			cutAlpha(c, NEON_ENGINE_A2), false, v, rot, origin, scale, fade);
	}
	if (trk) {
		const u32 c = bayColour(n, 1);
		q = emitSet(q, n->trunkSeg, n->trunkSegs, NEON_TRUNK_W1, trunkT, c, false, v,
			rot, origin, scale, fade);
		q = emitSet(q, n->trunkSeg, n->trunkSegs, NEON_TRUNK_W2, trunkT,
			cutAlpha(c, NEON_TRUNK_A2), false, v, rot, origin, scale, fade);
	}
	const int nv = (int)(q - vb);
	if (!nv)
		return;
	GSPGPU_FlushDataCache(vb, (u32)nv * sizeof(NeonVert));
	s_used += nv;

	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);           // v0 position
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);   // v1 colour
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);           // v2 texcoord
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, vb, sizeof(NeonVert), 3, 0x210);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uMvp, vp);

	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	// NEONBLUR is alphaBlendType 2 (additive) and writes no depth: the tubes
	// test against the car and the street but never hide anything.
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	carneon_light_env(gain);
	C3D_TexBind(0, &s_tex);
	C3D_DrawArrays(GPU_TRIANGLES, 0, nv);

	carneon_light_env_end();
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}
