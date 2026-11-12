// See carshadow.h. Every constant below is Speed.elf's, and the comments name
// the addresses they were read from.
#include "carshadow.h"
#include "shaders.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFSS_MAGIC     0x5353464Eu  // 'NFSS' little-endian
#define NFSS_VERSION   1
#define NFSS_ENTRY     28
#define NFSS_HDR       (0x10 + NFSS_ENTRY * CARSHADOW_TEX_COUNT)

// The in-game constants. cr2 picks between these and the frontend's below.
#define SH_HEIGHT      3.0f     // 0x8033c150: at 3 m off the road the shadow is gone
#define SH_SCALE_X     1.05f    // 0x8033c1a0 (frontend 1.2)
#define SH_SCALE_Y     1.05f    // 0x8033c1a0 (frontend 1.14)
#define SH_GROW        0.75f    // 0x8033c1ac: and it spreads by 75% on the way
#define SH_INNER_LIFT  0.08f    // 0x8033c1a8: the four inner vertices sit higher
#define SH_ROAD_LIFT   0.04f    // 0x8033c1bc: every vertex, off the road
#define SH_MAX_STEP    4.0f     // 0x8033c16c: further from the car's ground point
#define SH_MAX_PLANE   1.5f     // 0x8033c1b8: or from its plane, and it is off
#define SH_ALPHA       114.0f   // 0x8033c1c8
#define SH_CHAN_MAX    0xFE     // 0x8023acd8: alpha is clamped to 0..0xFE
// PORT: the shadow's alpha is doubled before that clamp. SPEED2.EXE adds it to
// itself (0x622746, after both arms pick their alpha); Speed.elf leaves it to
// psModulate's alpha scale, which the port's shader does not apply. So the
// frontend's 128 comes out at 0xFE, and the floor under the car goes dark.
#define SH_ALPHA_MUL   2
#define SH_STRIP_DROP  3        // 0x8023b494: a strip with more misses is skipped
// PORT: the underglow's alpha is the part's times this. Speed.elf doubles it
// (0x8023b0dc); SPEED2.EXE shifts it left by two (0x622a59), and the PC is the
// base, so a plain colour's 64 lights the ground at full 0xFF.
#define SH_GLOW_MUL    4

// The frontend's, off the other arm of each of those branches. The box is a
// little wider and a little longer than the road's, the inner four vertices
// are not lifted (0x8033c14c, 0.0), and the alpha fade has the same value at
// both ends (0x8033c1c4) so it comes out flat.
#define SH_FE_SCALE_X  1.2f     // 0x8033c1a4
#define SH_FE_SCALE_Y  1.14f    // 0x8033c19c
#define SH_FE_ALPHA    128       // 0x8033c1c4
// 0x8033c1c0 names the plane outright: z = 0.152, retail's showroom platform
// with a couple of millimetres' clearance over it. Taken raw it would be no
// use here -- the port's own DISC01 stands the car at 0.16, and a shadow eight
// millimetres inside the disc never passes the depth test -- so the plane
// follows the car and keeps the clearance instead of the absolute height.
#define SH_FE_LIFT     0.002f

// RenderTireMask (0x80236918): the disc's half-size is the rim's radius, in
// inches to metres (0x8033bfac 2.54, 0x8033bfb0 0.5, 0x8033bfb4 100), and a
// quarter again (0x8033bfc0) to reach over the tyre's sidewall. It sits in
// the rim's own plane 0.122 m in from its face (0x8033bfc4), behind the
// spokes and the brake, turned a tenth of a turn about the axle (eRotateY
// 0x1999).
#define TM_INCH        (2.54f * 0.5f / 100.0f)
#define TM_GROW        1.25f
#define TM_INSET       0.122f
#define TM_TURN        0x1999
// PORT: model3d.c's punchthrough cut. TIREMASK is DXT1 with one-bit alpha;
// only the filtered edge falls between 0 and 0xFF.
#define TM_ALPHA_REF   0x40

// The port's ground probe takes a ceiling, retail's GetElevation does not:
// anything more than this over the car's origin is a bridge or an awning.
#define PORT_PROBE_CEILING 1.0f
// ... and at most this many points a call (MODEL3D_MAX_PROBES).
#define PORT_PROBE_BATCH   8

// The hull shadow's (carshadow_draw_hull). Speed.elf's first, SPEED2.EXE's
// twin after it where it was read; they agree.
#define HS_LIGHT_Z     2300.0f  // 0x8033bf84 (0x7a83e8): the light, this high over the world's origin
#define HS_LIGHT_MIN   0x1a90   // sh_Setup: and never lower than this (bAngle, 37.4 degrees)
#define HS_POINT_LIFT  0.275f   // 0x8033c184 (0x7a665c): each hull point, before it is cast
#define HS_ROAD_LIFT   0.06f    // 0x8033c188 (0x7a66b8): each corner, off the road
#define HS_MAX_STEP    4.0f     // 0x8033bf70 (0x7a664c): further from the car's ground point
#define HS_MAX_PLANE   1.5f     // 0x8033bf78: or from its plane, and the corner is dropped
#define HS_CORNER      0.333f   // 0x8033bf7c (0x7a6720, 0x7a6724): how far each corner is rounded
#define HS_ALPHA       55.0f    // 0x8033c18c (0x7a65ac, 0x7a88b8 -55)
#define HS_EDGE        1.4f     // 0x8033c194 (0x7a6658): the rim's reach out from the centre
#define HS_EDGE_RISE   -0.3f    // 0x8033c190 (0x7a6654 1.1, less 1.4): at 3 m up
#define HS_U_HULL      0.155f   // 0x8033c198 (0x7a6650): the ramp's u at the hull's edge
// PORT: smoothing makes three of each corner.
#define HS_SMOOTH_MAX  (CARSIM_HULL_MAX * 3)
// PORT: what a frame's hull shadows are written into, each taking only what
// it needs and the next starting over at the front when it would not fit:
// a car's is 3-5 KB, its most 13 KB, and a frame of the player, a race field
// and sixteen traffic cars, both eyes, stays well inside it.
#define HS_RING_BYTES  (384 * 1024)

#define GRID          4
#define GRID_VERTS    (GRID * GRID)
// Three strips of three quads, two triangles a quad, two passes.
#define PASS_VERTS    ((GRID - 1) * (GRID - 1) * 6)
#define DRAW_VERTS    (PASS_VERTS * 2)
// Draws in flight: the player and a race field, twice over, and that once
// per eye (stereo.h), so a frame's vertices are never rewritten while the
// GPU may still be reading them.
#define RING          32

typedef struct {
	float pos[3];
	u8    rgba[4];
	float uv[2];
} ShadowVert;

// 0x8033c0d4: the wheel each vertex falls back on, FL FR RR RL = 0..3. Rows
// run from the right side of the car to the left, columns rear to front.
static const u8 kWheelOf[GRID_VERTS] = {
	2, 2, 1, 1,
	2, 2, 1, 1,
	3, 3, 0, 0,
	3, 3, 0, 0,
};

static void (*s_log)(const char*);
static ShadowVert* s_vbuf;
static int s_slot;
// The hull shadow's ring (HS_RING_BYTES): each draw's vertices, then its
// indices.
static u8*    s_hbuf;
static size_t s_hoff;
static bool   s_hullLogged;   // the first one drawn, once

static DVLE_s*         s_dvle;   // the shared blob\'s (shaders.h, #352)
static shaderProgram_s s_prog;
static int             s_uMvp = -1;
static bool            s_shader;

void carshadow_set_log(void (*sink)(const char*)) { s_log = sink; }

static void slog(const char* m) {
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
	s_vbuf = (ShadowVert*)linearAlloc(RING * DRAW_VERTS * sizeof(ShadowVert));
	if (!s_vbuf) {
		shaderProgramFree(&s_prog);
		s_dvle = NULL;
		return false;
	}
	s_shader = true;
	return true;
}

static bool isPow2(u32 v) { return v && !(v & (v - 1)); }

static u32 texBpp(u32 fmt) {
	switch (fmt) {
	case GPU_A8: case GPU_L8:          return 1;
	case GPU_RGB565: case GPU_RGBA4:   return 2;
	default:                           return 0;
	}
}

// One entry's mip chain, already tiled by the packer, one C3D_TexLoadImage per
// level the way carneon.c uploads NEONBLUR.
static bool loadTex(C3D_Tex* t, FILE* f, const u8* e, long size) {
	const u32 w = rd32(e), h = rd32(e + 4), fmt = rd32(e + 8), mlvl = rd32(e + 12);
	const u32 off = rd32(e + 16), bytes = rd32(e + 20);
	const u32 bpp = texBpp(fmt);
	if (!bpp || !isPow2(w) || !isPow2(h) || w < 8 || h < 8 || w > 1024 || h > 1024)
		return false;
	if (mlvl > 8 || (w >> mlvl) < 8 || (h >> mlvl) < 8)
		return false;
	const u32 lvl0 = w * h * bpp;
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
	C3D_TexInitParams tp = { (u16)w, (u16)h, (u8)mlvl, (GPU_TEXCOLOR)fmt, GPU_TEX_2D, false };
	if (ok)
		ok = C3D_TexInitWithParams(t, NULL, tp);
	if (ok) {
		const u8* src = buf;
		u32 sz = lvl0;
		for (u32 lv = 0; lv <= mlvl; lv++) {
			C3D_TexLoadImage(t, src, GPU_TEXFACE_2D, (int)lv);
			src += sz;
			sz >>= 2;
		}
		C3D_TexFlush(t);
		C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
		if (mlvl)
			C3D_TexSetFilterMipmap(t, GPU_LINEAR);
		C3D_TexSetWrap(t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	free(buf);
	return ok;
}

bool carshadow_load(CarShadow* s, const char* path) {
	carshadow_free(s);
	char b[200];
	FILE* f = path ? fopen(path, "rb") : NULL;
	if (!f) {
		snprintf(b, sizeof(b), "shadow: no %s -- the car keeps the blob shadow "
			"(run tools/carshadow_pack.py)", path ? path : "<CAR>.nfss");
		slog(b);
		return false;
	}
	u8 h[NFSS_HDR];
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	bool ok = size >= NFSS_HDR && fread(h, 1, NFSS_HDR, f) == NFSS_HDR &&
	          rd32(h) == NFSS_MAGIC && rd32(h + 4) == NFSS_VERSION &&
	          rd32(h + 8) == CARSHADOW_TEX_COUNT && initShader();
	for (int i = 0; ok && i < CARSHADOW_TEX_COUNT; i++) {
		const u8* e = h + 0x10 + i * NFSS_ENTRY;
		if (!rd32(e + 20))
			continue;   // this car has no such texture
		s->have[i] = loadTex(&s->tex[i], f, e, size);
		s->additive[i] = (rd32(e + 24) & 3) == 2;
		ok = s->have[i];
	}
	fclose(f);
	if (!ok || !s->have[CARSHADOW_TEX_IG]) {
		snprintf(b, sizeof(b), "shadow: %s is not a v%d .nfss with SHADOWIG -- "
			"the car keeps the blob shadow", path, NFSS_VERSION);
		slog(b);
		carshadow_free(s);
		return false;
	}
	// SHADOWFE is named too: a card packed before it was carried has an entry
	// of zero length there, and the frontend then draws nothing at all -- it
	// has no blob to fall back on the way the world does.
	snprintf(b, sizeof(b), "shadow: %s, SHADOWIG %ux%u%s%s", path,
		(unsigned)s->tex[CARSHADOW_TEX_IG].width, (unsigned)s->tex[CARSHADOW_TEX_IG].height,
		s->have[CARSHADOW_TEX_FE] ? ", SHADOWFE" : ", no SHADOWFE",
		s->have[CARSHADOW_TEX_NEON] ? ", NEON" : ", no NEON");
	slog(b);
	return true;
}

void carshadow_free(CarShadow* s) {
	if (!s)
		return;
	for (int i = 0; i < CARSHADOW_TEX_COUNT; i++)
		if (s->have[i])
			C3D_TexDelete(&s->tex[i]);
	memset(s, 0, sizeof(*s));
}

void carshadow_set_box(CarShadow* s, const float boxMin[3], const float boxMax[3]) {
	memcpy(s->boxMin, boxMin, sizeof(s->boxMin));
	memcpy(s->boxMax, boxMax, sizeof(s->boxMax));
	s->ready = s->have[CARSHADOW_TEX_IG];
}

// Car space to world, the transform the body is drawn with: p * rot + origin.
static void toWorld(float out[3], const float p[3], const float rot[3][3],
		const float origin[3]) {
	for (int i = 0; i < 3; i++)
		out[i] = p[0] * rot[0][i] + p[1] * rot[1][i] + p[2] * rot[2][i] + origin[i];
}

// GetElevation for n points, in the probe's batches. A miss leaves hit false.
static void elevations(const CarSimWorld* w, const float* xy, int n, float zMax,
		float* z, bool* hit, float* nrm) {
	for (int i = 0; i < n; i += PORT_PROBE_BATCH) {
		const int k = n - i < PORT_PROBE_BATCH ? n - i : PORT_PROBE_BATCH;
		for (int j = 0; j < k; j++)
			hit[i + j] = false;
		w->groundProbe(w->ctx, xy + i * 2, k, zMax, z + i, hit + i, nrm + i * 3, NULL);
	}
}

static int clampChan(int v, int hi) {
	return v <= 0 ? 0 : v > hi ? hi : v;
}

static ShadowVert* put(ShadowVert* q, const float p[3], u32 rgba, float u, float v) {
	memcpy(q->pos, p, sizeof(q->pos));
	q->rgba[0] = (u8)rgba;
	q->rgba[1] = (u8)(rgba >> 8);
	q->rgba[2] = (u8)(rgba >> 16);
	q->rgba[3] = (u8)(rgba >> 24);
	q->uv[0] = u;
	q->uv[1] = 1.0f - v;   // the PICA's V runs up the texture
	return q + 1;
}

// 0x8023b42c: strip k runs between rows k and k+1, column by column. It is
// dropped when the two rows missed the road more than three times between
// them. Written out as triangles so both passes are one draw each.
static ShadowVert* strips(ShadowVert* q, const float v[GRID_VERTS][3],
		const int miss[GRID], u32 rgba) {
	for (int k = 0; k < GRID - 1; k++) {
		if (miss[k] + miss[k + 1] > SH_STRIP_DROP)
			continue;
		for (int c = 0; c < GRID - 1; c++) {
			static const int quad[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 },
			                                { 1, 0 }, { 1, 1 }, { 0, 1 } };
			for (int t = 0; t < 6; t++) {
				const int row = k + quad[t][0], col = c + quad[t][1];
				q = put(q, v[row * GRID + col], rgba, (float)col / 3.0f,
					(float)row / 3.0f);
			}
		}
	}
	return q;
}

static void drawPass(const C3D_Tex* t, bool additive, ShadowVert* vb, int nv) {
	BufInfo_Init(C3D_GetBufInfo());
	BufInfo_Add(C3D_GetBufInfo(), vb, sizeof(ShadowVert), 3, 0x210);
	if (additive)
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE,
			GPU_SRC_ALPHA, GPU_ONE);
	else
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
			GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_TexBind(0, (C3D_Tex*)t);
	C3D_DrawArrays(GPU_TRIANGLES, 0, nv);
}

// The state every pass draws under, and what model3d.c expects back after it.
static void beginPasses(const C3D_Mtx* vp) {
	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);           // v0 position
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);   // v1 colour
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);           // v2 texcoord
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uMvp, vp);

	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	// Both textures write no depth: the car drawn next covers the middle.
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	// Colour doubled, alpha not: the PC's world pixel shader, which is what
	// these PC textures are authored against (afx.c). The GameCube's
	// psModulate doubles alpha too, against its own data.
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
}

static void endPasses(void) {
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
		GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_TexEnvScale(C3D_GetTexEnv(0), C3D_RGB, GPU_TEVSCALE_1);
}

void carshadow_draw(const CarShadow* s, const C3D_Mtx* vp, const CarSimWorld* w,
		const CarSim* sim, u32 neon) {
	if (!s || !s->ready || !s_shader || !w || !w->groundProbe || !sim)
		return;
	const float (*rot)[3] = (const float (*)[3])sim->rot;
	const float* origin = sim->origin;

	// Everything GetElevation is asked: the point under the car's origin
	// (0x80239c74), the four wheels (0x8023a9b4), then the grid (0x8023aa78).
	// Their xy is known before any height is, so it is one batched query.
	enum { P_GROUND = 0, P_WHEEL = 1, P_GRID = 5, P_COUNT = P_GRID + GRID_VERTS };
	float xy[P_COUNT * 2], z[P_COUNT], nrm[P_COUNT * 3];
	bool hit[P_COUNT];
	float wpos[4][3];
	xy[0] = origin[0];
	xy[1] = origin[1];
	for (int i = 0; i < 4; i++) {
		toWorld(wpos[i], sim->wheel[i].pos, rot, origin);
		xy[(P_WHEEL + i) * 2] = wpos[i][0];
		xy[(P_WHEEL + i) * 2 + 1] = wpos[i][1];
	}

	// 0x80239ce0: h, how far the car is off the road, 0 on it and 1 at 3 m.
	// Needed before the grid, which spreads with it, so the ground point is
	// asked on its own first.
	elevations(w, xy, 1, origin[2] + PORT_PROBE_CEILING, z, hit, nrm);
	// PORT: retail's topology always answers under the car; the port's probe
	// can miss off the edge of the loaded world, and then the car's own height
	// stands in, which reads as on the road.
	const float gz = hit[0] ? z[0] : origin[2];
	const float gn[3] = { hit[0] ? nrm[0] : 0.0f, hit[0] ? nrm[1] : 0.0f,
	                      hit[0] ? nrm[2] : 1.0f };
	float h = (origin[2] - gz) / SH_HEIGHT;
	h = h < 0.0f ? 0.0f : h > 1.0f ? 1.0f : h;

	// 0x8023a758: the box, pushed out as the car rises. Rows are y, columns x.
	const float grow = 1.0f + SH_GROW * h;
	const float x0 = s->boxMin[0] * SH_SCALE_X * grow, x1 = s->boxMax[0] * SH_SCALE_X * grow;
	const float y0 = s->boxMin[1] * SH_SCALE_Y * grow, y1 = s->boxMax[1] * SH_SCALE_Y * grow;
	const float dx = (x1 - x0) / 3.0f, dy = (y1 - y0) / 3.0f;
	float v[GRID_VERTS][3];
	for (int i = 0; i < GRID; i++)
		for (int j = 0; j < GRID; j++) {
			const float p[3] = { x0 + (float)j * dx, y0 + (float)i * dy, 0.0f };
			float* o = v[i * GRID + j];
			toWorld(o, p, rot, origin);
			o[2] = origin[2];
			xy[(P_GRID + i * GRID + j) * 2] = o[0];
			xy[(P_GRID + i * GRID + j) * 2 + 1] = o[1];
		}
	elevations(w, xy + P_WHEEL * 2, P_COUNT - P_WHEEL, origin[2] + PORT_PROBE_CEILING,
		z + P_WHEEL, hit + P_WHEEL, nrm + P_WHEEL * 3);

	// 0x8023a9b4: each wheel's elevation and the road's slope there, as dz/dx
	// and dz/dy (the normal over -n.z).
	float wz[4], gx[4], gy[4];
	bool wok[4];
	for (int i = 0; i < 4; i++) {
		const float* n = nrm + (P_WHEEL + i) * 3;
		// PORT: the probe's normals face up, and a wall-steep one would make
		// a slope of thousands; retail divides regardless.
		wok[i] = hit[P_WHEEL + i] && n[2] > 0.001f;
		wz[i] = z[P_WHEEL + i];
		gx[i] = wok[i] ? -n[0] / n[2] : 0.0f;
		gy[i] = wok[i] ? -n[1] / n[2] : 0.0f;
	}

	// 0x8023aa78: each vertex onto the road. One off the road takes its
	// wheel's plane instead; one that is still off, or too far from the car's
	// ground point or its plane, takes the last good height and counts a miss
	// against its row.
	int miss[GRID] = { 0, 0, 0, 0 };
	float last = origin[2];
	for (int i = 0; i < GRID_VERTS; i++) {
		float* o = v[i];
		bool ok = hit[P_GRID + i];
		o[2] = z[P_GRID + i];
		const int k = kWheelOf[i];
		if (!ok && wok[k]) {
			o[2] = wz[k] + (o[0] - wpos[k][0]) * gx[k] + (o[1] - wpos[k][1]) * gy[k];
			ok = true;
		}
		if (((i ^ (i >> 1)) & 5) == 5)   // 5, 6, 9, 10: the inner four
			o[2] += SH_INNER_LIFT;
		if (ok) {
			const float d = o[2] - gz;
			const float plane = (o[0] - origin[0]) * gn[0] + (o[1] - origin[1]) * gn[1] +
			                    d * gn[2];
			ok = fabsf(d) <= SH_MAX_STEP && fabsf(plane) < SH_MAX_PLANE;
		}
		if (ok) {
			last = o[2];
			o[2] += SH_ROAD_LIFT;
		} else {
			o[2] = last + SH_ROAD_LIFT;
			miss[i / GRID]++;
		}
	}

	// 0x8023ac54: fade with the car's tilt and height. The glow keeps only an
	// upright car's share (an upside-down one lights nothing below it).
	const float up = rot[2][2];
	const float fade = 1.0f - h;
	const float shade = fabsf(up) * fade;
	const float glow = (up < 0.0f ? 0.0f : up > 1.0f ? 1.0f : up) * fade;
	const u32 shadow = ((u32)clampChan((int)(SH_ALPHA * shade * SH_ALPHA_MUL), SH_CHAN_MAX)
	                    << 24) | 0x808080u;
	// 0x8023b0c0: +0x354, the underglow's colour with its alpha scaled and
	// multiplied up (SH_GLOW_MUL).
	int ga = (int)((float)(neon >> 24) * glow) * SH_GLOW_MUL;
	ga = ga > 0xFF ? 0xFF : ga;
	const u32 light = (neon & 0xFFFFFFu) | ((u32)ga << 24);

	ShadowVert* const vb = s_vbuf + s_slot * DRAW_VERTS;
	ShadowVert* q = vb;
	int n0 = 0, n1 = 0;
	if (shadow >> 24) {
		q = strips(q, (const float (*)[3])v, miss, shadow);
		n0 = (int)(q - vb);
	}
	if (ga && s->have[CARSHADOW_TEX_NEON]) {
		q = strips(q, (const float (*)[3])v, miss, light);
		n1 = (int)(q - vb) - n0;
	}
	if (!n0 && !n1)
		return;
	GSPGPU_FlushDataCache(vb, (u32)(n0 + n1) * sizeof(ShadowVert));
	s_slot = (s_slot + 1) % RING;

	beginPasses(vp);
	if (n0)
		drawPass(&s->tex[CARSHADOW_TEX_IG], s->additive[CARSHADOW_TEX_IG], vb, n0);
	if (n1)
		drawPass(&s->tex[CARSHADOW_TEX_NEON], s->additive[CARSHADOW_TEX_NEON], vb + n0, n1);
	endPasses();
}

void carshadow_draw_fe(const CarShadow* s, const C3D_Mtx* vp, const float pos[3],
		float yaw, u32 neon)
{
	if (!s || !s->ready || !s_shader || !s->have[CARSHADOW_TEX_FE] || !pos)
		return;

	// 0x8023a758 down the frontend arm: the same render box, the frontend's
	// pair of scales, and nothing to grow it by -- the growth factor is
	// 1 + 0.75*h, and a car standing on a turntable is on the floor, so h is
	// zero and the whole height-and-tilt fade below it goes with it.
	const float x0 = s->boxMin[0] * SH_FE_SCALE_X, x1 = s->boxMax[0] * SH_FE_SCALE_X;
	const float y0 = s->boxMin[1] * SH_FE_SCALE_Y, y1 = s->boxMax[1] * SH_FE_SCALE_Y;
	const float dx = (x1 - x0) / 3.0f, dy = (y1 - y0) / 3.0f;
	// Rows are the car's y, columns its x, as in-game; only the yaw of
	// model3d_draw_car's placement reaches them, the rest being flat.
	const float cy = cosf(yaw), sy = sinf(yaw);
	const float z = pos[2] + SH_FE_LIFT;

	// 0x8023ac10: every vertex onto that one plane, the inner four with the
	// rest -- their lift is 0 on this side.
	float v[GRID_VERTS][3];
	for (int i = 0; i < GRID; i++)
		for (int j = 0; j < GRID; j++) {
			const float px = x0 + (float)j * dx, py = y0 + (float)i * dy;
			float* o = v[i * GRID + j];
			o[0] = pos[0] + px * cy - py * sy;
			o[1] = pos[1] + px * sy + py * cy;
			o[2] = z;
		}

	// Nothing missed the road, because nothing was asked of it: every strip
	// draws.
	static const int kNoMiss[GRID] = { 0, 0, 0, 0 };
	const u32 rgba = ((u32)clampChan(SH_FE_ALPHA * SH_ALPHA_MUL, SH_CHAN_MAX) << 24) |
	                 0x808080u;
	// The glow over it, as in the world (0x8023b0c0): the underglow's colour
	// with its alpha multiplied up, the tilt-and-height fade being 1 on a
	// turntable.
	int ga = (int)(neon >> 24) * SH_GLOW_MUL;
	ga = ga > 0xFF ? 0xFF : ga;
	const u32 light = (neon & 0xFFFFFFu) | ((u32)ga << 24);

	ShadowVert* const vb = s_vbuf + s_slot * DRAW_VERTS;
	ShadowVert* q = strips(vb, (const float (*)[3])v, kNoMiss, rgba);
	const int n0 = (int)(q - vb);
	int n1 = 0;
	if (ga && s->have[CARSHADOW_TEX_NEON]) {
		q = strips(q, (const float (*)[3])v, kNoMiss, light);
		n1 = (int)(q - vb) - n0;
	}
	if (!n0 && !n1)
		return;
	GSPGPU_FlushDataCache(vb, (u32)(n0 + n1) * sizeof(ShadowVert));
	s_slot = (s_slot + 1) % RING;

	beginPasses(vp);
	if (n0)
		drawPass(&s->tex[CARSHADOW_TEX_FE], s->additive[CARSHADOW_TEX_FE], vb, n0);
	if (n1)
		drawPass(&s->tex[CARSHADOW_TEX_NEON], s->additive[CARSHADOW_TEX_NEON], vb + n0, n1);
	endPasses();
}

void carshadow_draw_tiremask(const C3D_Tex* tex, const Tex3DS_SubTexture* sub,
		const C3D_Mtx* vp, const float (*rim)[16], int rims, float inches,
		const float rot[3][3], const float origin[3], const float fwd[3])
{
	if (!tex || !sub || !rim || rims <= 0 || !initShader())
		return;
	const float h = inches * TM_INCH * TM_GROW;
	const float a = (float)TM_TURN * (2.0f * (float)M_PI / 65536.0f);
	const float ca = cosf(a), sa = sinf(a);
	// The ePoly's four corners and their UVs, in its own order.
	static const float kCorner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	static const u8 kTri[6] = { 0, 1, 2, 0, 2, 3 };
	// Every vertex colour is 0x80, 1.0: the TEV doubles the colour, and the
	// alpha is kept whole as psModulate's doubling leaves it.
	const u32 rgba = 0xFF808080u;

	ShadowVert* const vb = s_vbuf + s_slot * DRAW_VERTS;
	ShadowVert* q = vb;
	for (int k = 0; k < rims && k < 4; k++) {
		const float* m = rim[k];
		// CarRenderInfo::Render (0x8023d7fc) asks TireFace (0x802376c0) of
		// each rim: the rim's local +y, which runs inboard, against the
		// camera's direction (Camera+0x50), and draws the disc only below
		// FaceCos, 0 -- so on a wheel whose inner side faces the camera, which
		// is a far-side wheel seen under the car.
		float ax[3];
		for (int c = 0; c < 3; c++)
			ax[c] = m[4] * rot[0][c] + m[5] * rot[1][c] + m[6] * rot[2][c];
		if (fwd && ax[0] * fwd[0] + ax[1] * fwd[1] + ax[2] * fwd[2] >= 0.0f)
			continue;
		float p[4][3];
		for (int i = 0; i < 4; i++) {
			const float x = kCorner[i][0] * h, z = kCorner[i][1] * h;
			const float l[3] = { x * ca + z * sa, TM_INSET, z * ca - x * sa };
			float cp[3];
			for (int c = 0; c < 3; c++)
				cp[c] = l[0] * m[c] + l[1] * m[4 + c] + l[2] * m[8 + c] + m[12 + c];
			toWorld(p[i], cp, rot, origin);
		}
		for (int t = 0; t < 6; t++) {
			const int i = kTri[t];
			// UV (0,0) (1,0) (1,1) (0,1) onto the decoded image; put()
			// turns the V back.
			const float u = sub->left + (kCorner[i][0] > 0.0f ? sub->right - sub->left : 0.0f);
			const float v = sub->top + (kCorner[i][1] > 0.0f ? sub->bottom - sub->top : 0.0f);
			q = put(q, p[i], rgba, u, 1.0f - v);
		}
	}
	const int n = (int)(q - vb);
	if (!n)
		return;
	GSPGPU_FlushDataCache(vb, (u32)n * sizeof(ShadowVert));
	s_slot = (s_slot + 1) % RING;

	beginPasses(vp);
	// TIREMASK's TPK record: punchthrough (alphaUsage 1), not blended
	// (alphaBlend 0), and it writes depth.
	C3D_AlphaTest(true, GPU_GREATER, TM_ALPHA_REF);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	BufInfo_Init(C3D_GetBufInfo());
	BufInfo_Add(C3D_GetBufInfo(), vb, sizeof(ShadowVert), 3, 0x210);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_TexBind(0, (C3D_Tex*)tex);
	C3D_DrawArrays(GPU_TRIANGLES, 0, n);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	endPasses();
}

void carshadow_draw_quads(const C3D_Tex* tex, const Tex3DS_SubTexture* sub,
		const C3D_Mtx* vp, const float (*quad)[4][3], int n)
{
	if (!tex || !sub || !quad || n <= 0 || !initShader())
		return;
	static const u8 kTri[6] = { 0, 1, 2, 0, 2, 3 };
	static const float kUV[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	// The colour 0x80 is 1.0 under the doubled TEV, the alpha whole.
	const u32 rgba = 0xFF808080u;
	beginPasses(vp);
	// A slot holds DRAW_VERTS: so many quads a draw.
	const int per = DRAW_VERTS / 6;
	for (int at = 0; at < n; at += per) {
		const int cnt = n - at < per ? n - at : per;
		ShadowVert* const vb = s_vbuf + s_slot * DRAW_VERTS;
		ShadowVert* q = vb;
		for (int i = 0; i < cnt; i++)
			for (int t = 0; t < 6; t++) {
				const int k = kTri[t];
				const float u = sub->left + kUV[k][0] * (sub->right - sub->left);
				const float v = sub->top + kUV[k][1] * (sub->bottom - sub->top);
				q = put(q, quad[at + i][k], rgba, u, 1.0f - v);
			}
		const int nv = (int)(q - vb);
		GSPGPU_FlushDataCache(vb, (u32)nv * sizeof(ShadowVert));
		s_slot = (s_slot + 1) % RING;
		drawPass(tex, false, vb, nv);
	}
	endPasses();
}

// ---------------------------------------------------------------------------
// The hull shadow (g_CarShadowEnable 2): CarRenderInfo::Render 0x80239de4 ..
// 0x8023a710, SPEED2.EXE FUN_00620AE0 0x62193B .. 0x622103.

// sh_Setup (0x80236730, SPEED2.EXE 0x60D350): the way the light runs to the
// car, from a point HS_LIGHT_Z over the world's origin. One lower than
// HS_LIGHT_MIN is brought up to it along the length it had, its level part
// shared out by |x| + |y| as retail does. Returns 1/z, 0 for a level light.
static float hullLight(const float o[3], float L[3]) {
	L[0] = o[0];
	L[1] = o[1];
	L[2] = o[2] - HS_LIGHT_Z;
	const float r = sqrtf(L[0] * L[0] + L[1] * L[1]);
	const float eMin = (float)HS_LIGHT_MIN * (2.0f * (float)M_PI / 65536.0f);
	// bATan(r, -z) is an unsigned bAngle: a car over the light reads as far
	// past the limit, not under it.
	if (r > 0.0f && L[2] <= 0.0f && atan2f(-L[2], r) < eMin) {
		const float len = sqrtf(r * r + L[2] * L[2]);
		const float k = len * cosf(eMin) / (fabsf(L[0]) + fabsf(L[1]));
		L[2] = -len * sinf(eMin);
		L[0] *= k;
		L[1] *= k;
	}
	return L[2] != 0.0f ? 1.0f / L[2] : 0.0f;
}

// ch2d (0x80236180), Ken Clarkson's monotone chain, which retail carries:
// cmpl and cmph (0x80235ff4, 0x80236058) sort by x, then by y the other way,
// and make_chain (0x80236084) pops a point while the turn past it is not
// counterclockwise.
static int hullCmpl(const void* a, const void* b) {
	const float* p = *(const float* const*)a;
	const float* q = *(const float* const*)b;
	float v = p[0] - q[0];
	if (v > 0.0f) return 1;
	if (v < 0.0f) return -1;
	v = q[1] - p[1];
	if (v > 0.0f) return 1;
	if (v < 0.0f) return -1;
	return 0;
}

static int hullCmph(const void* a, const void* b) { return hullCmpl(b, a); }

static int makeChain(const float** V, int n, int (*cmp)(const void*, const void*)) {
	qsort(V, (size_t)n, sizeof(*V), cmp);
	int s = 1;
	for (int i = 2; i < n; i++) {
		int j = s;
		for (; j >= 1; j--) {
			const float* a = V[i];
			const float* b = V[j];
			const float* c = V[j - 1];
			if ((a[0] - b[0]) * (c[1] - b[1]) - (a[1] - b[1]) * (c[0] - b[0]) > 0.0f)
				break;
		}
		s = j + 1;
		const float* t = V[s];
		V[s] = V[i];
		V[i] = t;
	}
	return s;
}

// The corners of the hull round P[0..n), in order at the front of P, which
// has room for one more.
static int ch2d(const float** P, int n) {
	const int u = makeChain(P, n, hullCmpl);
	P[n] = P[0];
	return u + makeChain(P + u, n - u + 1, hullCmph);
}

void carshadow_draw_hull(const C3D_Tex* ramp, const Tex3DS_SubTexture* sub,
		const C3D_Mtx* vp, const CarSimWorld* w, const CarSim* sim, int lod, float lodFade)
{
	if (!ramp || !sub || !vp || !sim || !w || !w->groundProbe || lod < 0 || lod > 2 ||
	    !initShader())
		return;
	if (!s_hbuf) {
		s_hbuf = (u8*)linearAlloc(HS_RING_BYTES);
		if (!s_hbuf)
			return;
	}
	const float (*rot)[3] = (const float (*)[3])sim->rot;
	const float* origin = sim->origin;

	// The ground under the car's origin and its normal, and h, how far the
	// car is off it: the grid's own (0x80239c74, 0x80239ce0).
	float z0, n0[3];
	bool hit0;
	elevations(w, origin, 1, origin[2] + PORT_PROBE_CEILING, &z0, &hit0, n0);
	// PORT: a miss off the edge of the loaded world reads as on the road, as
	// in carshadow_draw().
	const float gz = hit0 ? z0 : origin[2];
	const float gn[3] = { hit0 ? n0[0] : 0.0f, hit0 ? n0[1] : 0.0f, hit0 ? n0[2] : 1.0f };
	float h = (origin[2] - gz) / SH_HEIGHT;
	h = h < 0.0f ? 0.0f : h > 1.0f ? 1.0f : h;

	// 0x621A00: every point of the car's polytope (GetPoint), lifted
	// HS_POINT_LIFT in the car's frame, put through the matrix the body is
	// drawn with, and slid along the light down to the origin's height. The
	// points are the polytope's, from the centre of gravity, and go through
	// that matrix as they are.
	float L[3];
	const float oneOverZ = hullLight(origin, L);
	int np = carsim_body_point_count(sim);
	np = np > CARSIM_HULL_MAX ? CARSIM_HULL_MAX : np;
	if (np < 3)
		return;
	float pt[CARSIM_HULL_MAX][2];
	const float* P[CARSIM_HULL_MAX + 1];
	for (int k = 0; k < np; k++) {
		float p[3], q[3];
		carsim_body_point(sim, k, p);
		p[2] += HS_POINT_LIFT;
		toWorld(q, p, rot, origin);
		const float t = -(q[2] - origin[2]) * oneOverZ;
		pt[k][0] = q[0] + t * L[0];
		pt[k][1] = q[1] + t * L[1];
		P[k] = pt[k];
	}
	const int nh = ch2d(P, np);

	// convex_hull (0x802361e8, SPEED2.EXE 0x60D000): each corner onto the
	// road, HS_ROAD_LIFT off it. One the road misses, or more than
	// HS_MAX_STEP from the car's ground point or HS_MAX_PLANE off its plane,
	// is dropped. At C one height, the first corner's, serves for them all.
	float xy[CARSIM_HULL_MAX * 2], ez[CARSIM_HULL_MAX], en[CARSIM_HULL_MAX * 3];
	bool eh[CARSIM_HULL_MAX];
	for (int i = 0; i < nh; i++) {
		xy[i * 2] = P[i][0];
		xy[i * 2 + 1] = P[i][1];
	}
	const bool one = lod > 1;
	elevations(w, xy, one ? 1 : nh, origin[2] + PORT_PROBE_CEILING, ez, eh, en);
	float hv[CARSIM_HULL_MAX][3];
	int m = 0;
	for (int i = 0; i < nh; i++) {
		const int e = one ? 0 : i;
		const float z = ez[e] + HS_ROAD_LIFT;
		if (!eh[e] || fabsf(z - (gz + HS_ROAD_LIFT)) > HS_MAX_STEP)
			continue;
		const float plane = (z - gz) * gn[2] + (P[i][0] - origin[0]) * gn[0] +
		                    (P[i][1] - origin[1]) * gn[1];
		if (fabsf(plane) >= HS_MAX_PLANE)
			continue;
		hv[m][0] = P[i][0];
		hv[m][1] = P[i][1];
		hv[m][2] = z;
		m++;
	}
	if (m < 1)
		return;

	// smooth_shadow_corners (0x80236510, SPEED2.EXE 0x60D1B0): each corner
	// made three -- a third of the way back along the edge before it, a third
	// along the one after, and one between them taken a third of the turn in.
	// At every level of detail: the GameCube's OpToM and SPEED2.EXE's
	// 0x8A1C60, which would keep C's corners sharp, are 0.
	float sv[HS_SMOOTH_MAX][3];
	float prev[3];
	for (int c = 0; c < 3; c++)
		prev[c] = (hv[0][c] - hv[m - 1][c]) * HS_CORNER;
	for (int i = 0; i < m; i++) {
		const int j = i + 1 == m ? 0 : i + 1;
		for (int c = 0; c < 3; c++) {
			const float cur = (hv[j][c] - hv[i][c]) * HS_CORNER;
			sv[i * 3][c] = hv[i][c] - prev[c];
			sv[i * 3 + 1][c] = hv[i][c] + (cur - prev[c]) * HS_CORNER;
			sv[i * 3 + 2][c] = hv[i][c] + cur;
			prev[c] = cur;
		}
	}
	const int n = m * 3;

	// 0x621BD0: the centre, half way from the first corner to the middle one;
	// the rim's reach, less as the car rises; and the alpha, gone at 3 m up,
	// and at C faded further with the car's size on screen (`lodFade`).
	float C[3];
	for (int c = 0; c < 3; c++)
		C[c] = (sv[0][c] + sv[n / 2][c]) * 0.5f;
	const float edge = HS_EDGE + HS_EDGE_RISE * h;
	float a = HS_ALPHA - HS_ALPHA * h;
	if (lod == 2)
		a *= lodFade;
	const u32 rgba = ((u32)clampChan((int)a, SH_CHAN_MAX) << 24) | 0x808080u;
	if (!(rgba >> 24))
		return;
	if (!s_hullLogged) {
		s_hullLogged = true;
		char b[160];
		snprintf(b, sizeof(b), "shadow: hull shadow, %d points, %d corners, %d on the road, "
			"%d smoothed; h %.2f, alpha %d, lod %d", np, nh, m, n, (double)h,
			(int)(rgba >> 24), lod);
		slog(b);
	}

	const int nv = 1 + n * 2, ni = n * 9;
	const size_t vbytes = ((size_t)nv * sizeof(ShadowVert) + 15) & ~(size_t)15;
	const size_t need = vbytes + (((size_t)ni * sizeof(u16) + 15) & ~(size_t)15);
	if (s_hoff + need > HS_RING_BYTES)
		s_hoff = 0;
	ShadowVert* const vb = (ShadowVert*)(s_hbuf + s_hoff);
	u16* const ib = (u16*)(s_hbuf + s_hoff + vbytes);
	s_hoff += need;

	// The ramp's u runs from the centre (0) through the hull's edge
	// (HS_U_HULL) to the rim (1); v is its first row. Both through `sub`,
	// and put() turns v back. The rim's vertices are the corners pushed out
	// from the centre in plan, at the corners' own heights (0x60D490).
	const float u0 = sub->left, du = sub->right - sub->left;
	const float v0 = 1.0f - sub->top;
	ShadowVert* q = put(vb, C, rgba, u0, v0);
	for (int i = 0; i < n; i++)
		q = put(q, sv[i], rgba, u0 + HS_U_HULL * du, v0);
	for (int i = 0; i < n; i++) {
		const float o[3] = { C[0] + (sv[i][0] - C[0]) * edge,
		                     C[1] + (sv[i][1] - C[1]) * edge, sv[i][2] };
		q = put(q, o, rgba, u0 + du, v0);
	}
	// Retail's strips -- the fan's four vertices at a time from the centre,
	// with a last triangle when the count is odd, then the rim in three runs
	// of a third each -- come to every triangle from the centre to a pair of
	// neighbouring corners, and a quad from each such pair out to the rim.
	u16* k = ib;
	for (int i = 0; i < n; i++) {
		const u16 v1 = (u16)(1 + i), v2 = (u16)(1 + (i + 1) % n);
		*k++ = 0;
		*k++ = v1;
		*k++ = v2;
		*k++ = v1;
		*k++ = (u16)(v1 + n);
		*k++ = v2;
		*k++ = v2;
		*k++ = (u16)(v1 + n);
		*k++ = (u16)(v2 + n);
	}
	GSPGPU_FlushDataCache(vb, (u32)need);

	beginPasses(vp);
	BufInfo_Init(C3D_GetBufInfo());
	BufInfo_Add(C3D_GetBufInfo(), vb, sizeof(ShadowVert), 3, 0x210);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
		GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_TexBind(0, (C3D_Tex*)ramp);
	C3D_DrawElements(GPU_TRIANGLES, ni, C3D_UNSIGNED_SHORT, ib);
	endPasses();
}
