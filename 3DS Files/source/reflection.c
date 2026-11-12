// See reflection.h. TASKS.md #296 has the trace; the functions named here are
// Speed.elf's.
#include "reflection.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shaders.h"
#include "model3d.h"

// The capture: the top screen's 240x400 framebuffer at about half, rounded
// to the powers of two a texture needs, in the same orientation (x along the
// 240 side), so the main view's projection fills it as it fills the screen.
#define REFL_W 128
#define REFL_H 256

// The blur's taps (reflection_blur), as the TEV's 8-bit greys: fe_post.c's.
#define BLUR_1    85    // 1/3, at -1 and +1
#define BLUR_3A   43    // 1/6, at -3 ...
#define BLUR_3B   42    // ... and +3: the four make 255

typedef struct {
	float x, y;   // clip space
	float s, t;   // unread here (envmap.v.pica's)
	float a;
} ReflVert;

// post.v.pica's (fe_post.c).
typedef struct {
	float x, y;   // clip space
	float u, v;   // 0..1 across the target
} PostVert;

struct Reflection {
	C3D_Tex           tex;
	C3D_RenderTarget* target;
	DVLE_s*           dvle;   // the shared blob's (shaders.h, #352)
	shaderProgram_s   prog;
	ReflVert*         quad;
	// g_RoadReflectionEnable 3's blur: the second target, the capture as a
	// plain 2D texture for the filter to read, and the filter's program.
	bool              blurReady;
	C3D_Tex           tmp;
	C3D_RenderTarget* tmpTarget;
	C3D_Tex           texView;   // tex's memory; owns nothing
	DVLE_s*           postDvle;   // the shared blob's (shaders.h, #352)
	shaderProgram_s   postProg;
	int               uMap[3];
	PostVert*         postQuad;
};

void reflection_destroy(Reflection* r)
{
	if (!r)
		return;
	if (r->target)
		C3D_RenderTargetDelete(r->target);
	if (r->tex.data)
		C3D_TexDelete(&r->tex);
	if (r->quad)
		linearFree(r->quad);
	if (r->dvle) {
		shaderProgramFree(&r->prog);
	}
	if (r->tmpTarget)
		C3D_RenderTargetDelete(r->tmpTarget);
	if (r->tmp.data)
		C3D_TexDelete(&r->tmp);
	if (r->postQuad)
		linearFree(r->postQuad);
	if (r->postDvle) {
		shaderProgramFree(&r->postProg);
	}
	free(r);
}

static void say(void (*log)(const char*), const char* what)
{
	if (!log)
		return;
	char b[200];
	snprintf(b, sizeof(b), "reflection: %s", what);
	log(b);
}

// FUN_005BA060 makes the second target ([0x87089C], its surface [0x870898])
// beside the capture only on a card whose g_RoadReflectionEnable cap is 3; the
// 3DS's cap is 3 (display.c). Without it, level 3 draws as level 2.
static void blurCreate(Reflection* r, void (*log)(const char*))
{
	const C3D_TexInitParams tp = { REFL_W, REFL_H, 0, GPU_RGB565, GPU_TEX_2D, true };
	if (!C3D_TexInitWithParams(&r->tmp, NULL, tp)) {
		say(log, "no VRAM for level 3's blur -- drawn as level 2");
		return;
	}
	C3D_TexSetFilter(&r->tmp, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&r->tmp, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	// No depth: the filter tests none.
	r->tmpTarget = C3D_RenderTargetCreateFromTex(&r->tmp, GPU_TEXFACE_2D, 0, -1);
	if (!r->tmpTarget) {
		say(log, "no VRAM for level 3's blur -- drawn as level 2");
		return;
	}
	// The capture is GPU_TEX_PROJECTION for the wet road's read; the filter
	// reads it flat. PORT: clamped to its edge. The FILTER effect sets no
	// address mode, so the PC's taps past the edge read whatever the sampler
	// was left at.
	r->texView = r->tex;
	r->texView.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
	C3D_TexSetFilter(&r->texView, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&r->texView, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

	r->postDvle = shaders_get(SHADER_POST);
	if (!r->postDvle) {
		say(log, "the filter's shader would not load -- level 3 drawn as level 2");
		return;
	}
	shaderProgramInit(&r->postProg);
	shaderProgramSetVsh(&r->postProg, r->postDvle);
	r->uMap[0] = shaderInstanceGetUniformLocation(r->postProg.vertexShader, "map0");
	r->uMap[1] = shaderInstanceGetUniformLocation(r->postProg.vertexShader, "map1");
	r->uMap[2] = shaderInstanceGetUniformLocation(r->postProg.vertexShader, "map2");
	r->postQuad = (PostVert*)linearAlloc(sizeof(PostVert) * 4);
	if (!r->postQuad) {
		say(log, "no linear memory for the filter -- level 3 drawn as level 2");
		return;
	}
	static const PostVert kQuad[4] = {
		{ -1.0f, -1.0f, 0.0f, 0.0f }, { 1.0f, -1.0f, 1.0f, 0.0f },
		{ -1.0f,  1.0f, 0.0f, 1.0f }, { 1.0f,  1.0f, 1.0f, 1.0f },
	};
	memcpy(r->postQuad, kQuad, sizeof(kQuad));
	GSPGPU_FlushDataCache(r->postQuad, sizeof(kQuad));
	r->blurReady = true;
}

Reflection* reflection_create(void (*log)(const char*))
{
	Reflection* r = (Reflection*)calloc(1, sizeof(Reflection));
	if (!r) {
		say(log, "no memory -- the road is drawn dry");
		return NULL;
	}
	const u32 vram0 = vramSpaceFree();
	// GPU_TEX_PROJECTION: scenerf.v.pica hands texture unit 0 (s w, t w, w).
	const C3D_TexInitParams tp = { REFL_W, REFL_H, 0, GPU_RGB565, GPU_TEX_PROJECTION, true };
	bool ok = C3D_TexInitWithParams(&r->tex, NULL, tp);
	if (ok) {
		C3D_TexSetFilter(&r->tex, GPU_LINEAR, GPU_LINEAR);
		reflection_set_edge_clamp(r, false);
		r->target = C3D_RenderTargetCreateFromTex(&r->tex, GPU_TEXFACE_2D, 0,
			GPU_RB_DEPTH24_STENCIL8);
		ok = r->target != NULL;
	}
	if (!ok) {
		say(log, "no VRAM for the capture -- the road is drawn dry");
		reflection_destroy(r);
		return NULL;
	}
	r->dvle = shaders_get(SHADER_ENVMAP);
	if (!r->dvle) {
		say(log, "shader would not load -- the road is drawn dry");
		reflection_destroy(r);
		return NULL;
	}
	shaderProgramInit(&r->prog);
	shaderProgramSetVsh(&r->prog, r->dvle);
	r->quad = (ReflVert*)linearAlloc(sizeof(ReflVert) * 4);
	if (!r->quad) {
		say(log, "no linear memory -- the road is drawn dry");
		reflection_destroy(r);
		return NULL;
	}
	static const ReflVert kQuad[4] = {
		{ -1.0f, -1.0f, 0, 0, 0 }, { 1.0f, -1.0f, 0, 0, 0 },
		{ -1.0f,  1.0f, 0, 0, 0 }, { 1.0f,  1.0f, 0, 0, 0 },
	};
	memcpy(r->quad, kQuad, sizeof(kQuad));
	GSPGPU_FlushDataCache(r->quad, sizeof(kQuad));
	blurCreate(r, log);
	if (log) {
		char b[160];
		snprintf(b, sizeof(b), "reflection: %dx%d capture%s, %lu KB VRAM (%lu KB left)",
			REFL_W, REFL_H, r->blurReady ? " and its blur target" : "",
			(unsigned long)((vram0 - vramSpaceFree()) >> 10),
			(unsigned long)(vramSpaceFree() >> 10));
		log(b);
	}
	return r;
}

// envmap.c's envClear(), and drawn for the same reason (TASKS.md #307): a
// quad writing colour 0 and depth 0, the far end of the world's GPU_GREATER.
//
// PORT: black. The capture's EFB holds whatever the copy before it cleared to
// (CaptureEFB's clear flag), which is not traced; black is eInitEnvMap's, and
// it adds nothing to the road where nothing is reflected. Unfogged too: the
// road the capture lands on is fogged in the main view, so a far reflection
// fades with it.
void reflection_begin(Reflection* r)
{
	if (!r)
		return;
	C3D_FrameDrawOn(r->target);
	C3D_BindProgram(&r->prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 3);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, r->quad, sizeof(ReflVert), 2, 0x10);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
	C3D_DepthMap(true, 0.0f, 0.0f);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvColor(env, 0);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

const C3D_Tex* reflection_texture(const Reflection* r)
{
	return r ? &r->tex : NULL;
}

// fe_post.c's stage(): TEV stage `id`, `func` over (a, b, c) for the colour,
// its constant the grey `grey`/255, the alpha an opaque constant.
static void stage(int id, GPU_COMBINEFUNC func, GPU_TEVSRC a, GPU_TEVSRC b, GPU_TEVSRC c,
	u8 grey)
{
	C3D_TexEnv* env = C3D_GetTexEnv(id);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, a, b, c);
	C3D_TexEnvFunc(env, C3D_RGB, func);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, 0xFF000000u | grey * 0x010101u);
}

// Texture unit `unit` reads the source moved by `n` taps of (ds, dt).
static void tapAt(const Reflection* r, int unit, float n, float ds, float dt)
{
	C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uMap[unit], 1.0f, 1.0f, n * ds, n * dt);
}

// One FUN_005C5F70 pass, `src` filtered into `dst`. Two draws, there being
// three texture units: -3, -1 and +1, then +3 added over them, as fe_post.c's
// blur() does it.
static void blurPass(Reflection* r, C3D_RenderTarget* dst, C3D_Tex* src, float ds, float dt)
{
	C3D_FrameDrawOn(dst);
	C3D_BindProgram(&r->postProg);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, r->postQuad, sizeof(PostVert), 2, 0x10);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthMap(true, 0.0f, 0.0f);
	// The capture's depth stays as the props and the car left it, for the
	// lights drawn into it after.
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_ProcTexBind(0, NULL);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	for (int i = 0; i < 3; i++)
		C3D_TexBind(i, src);
	tapAt(r, 0, -3.0f, ds, dt);
	tapAt(r, 1, -1.0f, ds, dt);
	tapAt(r, 2, 1.0f, ds, dt);
	stage(0, GPU_MODULATE, GPU_TEXTURE1, GPU_CONSTANT, GPU_PRIMARY_COLOR, BLUR_1);
	stage(1, GPU_MULTIPLY_ADD, GPU_TEXTURE2, GPU_CONSTANT, GPU_PREVIOUS, BLUR_1);
	stage(2, GPU_MULTIPLY_ADD, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS, BLUR_3A);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	tapAt(r, 0, 3.0f, ds, dt);
	stage(0, GPU_MODULATE, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR, BLUR_3B);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
}

// FUN_005C7E40. The taps are in the PC capture's texels, 320x240 over its
// 640x480. The camera's angle is the horizontal one, as fe_post.c's glow has
// it, so a PC texel is 1/320 of the screen's width: here 1.25 of the panel's
// 400, which the capture's t (256 over the 400) and s (128 over the 240) take
// as 1/320 and 1.25/240 of themselves.
bool reflection_blur(Reflection* r)
{
	if (!r || !r->blurReady)
		return false;
	blurPass(r, r->tmpTarget, &r->texView, 0.0f, 1.0f / 320.0f);
	blurPass(r, r->target, &r->tmp, 1.25f / 240.0f, 0.0f);
	// The world's own state back (envmap.c's envClear()), the capture bound
	// for what goes in after, and units 1 and 2 off (model3d.h).
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_TexEnvInit(C3D_GetTexEnv(0));
	model3d_tex_unit_off(1, &r->tmp);
	model3d_tex_unit_off(2, &r->tmp);
	return true;
}

// PORT (#328): on the 3DS the street now and then takes a flat pale blue
// where the wet road adds the capture, nearly the same colour at every pixel.
// One way to get that is a projection gone out of range, which reads the
// edge texel under GPU_CLAMP_TO_EDGE. A black border makes such a read add
// nothing. On screen the reads stay inside, so the border only touches the
// outermost half texel.
void reflection_set_edge_clamp(Reflection* r, bool edge)
{
	if (!r)
		return;
	const GPU_TEXTURE_WRAP_PARAM w = edge ? GPU_CLAMP_TO_EDGE : GPU_CLAMP_TO_BORDER;
	C3D_TexSetWrap(&r->tex, w, w);
	r->tex.border = 0;
}

// The capture's texel (x along its 128 side, y down its 256 rows) in the
// GPU's tiling: 8x8 tiles row by row, Morton order inside each.
static u32 tiledAt(u32 x, u32 y)
{
	u32 m = 0;
	for (int i = 0; i < 3; i++)
		m |= (((x >> i) & 1u) << (2 * i)) | (((y >> i) & 1u) << (2 * i + 1));
	return ((y >> 3) * (REFL_W >> 3) + (x >> 3)) * 64u + m;
}

static void rgbOf(u16 p, u32 out[3])
{
	out[0] = ((p >> 11) & 0x1Fu) * 255u / 31u;
	out[1] = ((p >> 5) & 0x3Fu) * 255u / 63u;
	out[2] = (p & 0x1Fu) * 255u / 31u;
}

bool reflection_probe(const Reflection* r, const char* bmpPath, char* stats, int statsSize)
{
	if (stats && statsSize > 0)
		stats[0] = 0;
	if (!r || !r->tex.data)
		return false;
	const u32 size = REFL_W * REFL_H * 2;
	u16* px = (u16*)linearAlloc(size);
	if (!px)
		return false;
	// C3D_TexLoadImage()'s copy the other way: a raw TextureCopy, after the
	// GPU queue has run dry.
	// #481: the copy submits the draws so far itself, unflushed; on the 3DS
	// a running queue then reads the list before the cache has written it.
	C3D_FrameSplit(GX_CMDLIST_FLUSH);
	C3D_SyncTextureCopy((u32*)r->tex.data, 0, (u32*)px, 0, size, 8);
	GSPGPU_InvalidateDataCache(px, size);

	u64 sum[3] = { 0, 0, 0 };
	u32 hi[3] = { 0, 0, 0 };
	u32 lit = 0;
	for (u32 i = 0; i < REFL_W * REFL_H; i++) {
		u32 c[3];
		rgbOf(px[i], c);
		for (int k = 0; k < 3; k++) {
			sum[k] += c[k];
			if (c[k] > hi[k])
				hi[k] = c[k];
		}
		if (c[0] > 8 || c[1] > 8 || c[2] > 8)
			lit++;
	}
	static const u16 kAt[5][2] = {
		{ 0, 0 }, { REFL_W - 1, 0 }, { 0, REFL_H - 1 }, { REFL_W - 1, REFL_H - 1 },
		{ REFL_W / 2, REFL_H / 2 },
	};
	u32 at[5][3];
	for (int k = 0; k < 5; k++)
		rgbOf(px[tiledAt(kAt[k][0], kAt[k][1])], at[k]);
	const u32 n = REFL_W * REFL_H;
	if (stats && statsSize > 0)
		snprintf(stats, (size_t)statsSize, "capture mean %lu,%lu,%lu max %lu,%lu,%lu lit %lu%%, "
			"corners %lu,%lu,%lu %lu,%lu,%lu %lu,%lu,%lu %lu,%lu,%lu centre %lu,%lu,%lu",
			(unsigned long)(sum[0] / n), (unsigned long)(sum[1] / n), (unsigned long)(sum[2] / n),
			(unsigned long)hi[0], (unsigned long)hi[1], (unsigned long)hi[2],
			(unsigned long)(lit * 100u / n),
			(unsigned long)at[0][0], (unsigned long)at[0][1], (unsigned long)at[0][2],
			(unsigned long)at[1][0], (unsigned long)at[1][1], (unsigned long)at[1][2],
			(unsigned long)at[2][0], (unsigned long)at[2][1], (unsigned long)at[2][2],
			(unsigned long)at[3][0], (unsigned long)at[3][1], (unsigned long)at[3][2],
			(unsigned long)at[4][0], (unsigned long)at[4][1], (unsigned long)at[4][2]);

	bool ok = true;
	if (bmpPath) {
		// 256x128, bottom row first: a BMP row is one of the capture's 128
		// columns (the screen's height), a BMP column one of its rows (the
		// screen's width), as topShotSave() lays the framebuffer out.
		FILE* f = fopen(bmpPath, "wb");
		ok = f != NULL;
		if (f) {
			static const u8 head[54] = {
				'B', 'M', 0x36, 0x80, 0x01, 0, 0, 0, 0, 0, 54, 0, 0, 0,   // 54 + 98304 bytes
				40, 0, 0, 0, 0x00, 0x01, 0, 0, 0x80, 0, 0, 0,             // 256 x 128, bottom up
				1, 0, 24, 0, 0, 0, 0, 0, 0x00, 0x80, 0x01, 0,             // 24-bit, 98304 bytes
				0x13, 0x0B, 0, 0, 0x13, 0x0B, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			};
			static u8 row[REFL_H * 3];
			ok = fwrite(head, 1, sizeof(head), f) == sizeof(head);
			for (u32 x = 0; x < REFL_W && ok; x++) {
				for (u32 y = 0; y < REFL_H; y++) {
					u32 c[3];
					rgbOf(px[tiledAt(x, y)], c);
					row[y * 3 + 0] = (u8)c[2];
					row[y * 3 + 1] = (u8)c[1];
					row[y * 3 + 2] = (u8)c[0];
				}
				ok = fwrite(row, 1, sizeof(row), f) == sizeof(row);
			}
			fclose(f);
		}
	}
	linearFree(px);
	return ok;
}

// RenderCarReflections (0x80240c88) draws the car through its world matrix
// with row 2 negated: model space mirrored in z, about the plane under the
// wheels. Here that is folded into the view-projection as the same mirror in
// world space, p - 2 ((p - o).n) n with n the car's up axis, so the body's
// light (model3d.c's carLightContext, from rot) stays the real car's.
void reflection_car_vp(C3D_Mtx* out, const C3D_Mtx* vp, const float rot[3][3],
	const float origin[3])
{
	float n[3] = { rot[2][0], rot[2][1], rot[2][2] };
	const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (l < 1e-6f) {
		*out = *vp;
		return;
	}
	for (int i = 0; i < 3; i++)
		n[i] /= l;
	const float on = origin[0] * n[0] + origin[1] * n[1] + origin[2] * n[2];
	C3D_Mtx m;
	Mtx_Identity(&m);
	for (int i = 0; i < 3; i++) {
		m.r[i].x = (i == 0 ? 1.0f : 0.0f) - 2.0f * n[i] * n[0];
		m.r[i].y = (i == 1 ? 1.0f : 0.0f) - 2.0f * n[i] * n[1];
		m.r[i].z = (i == 2 ? 1.0f : 0.0f) - 2.0f * n[i] * n[2];
		m.r[i].w = 2.0f * on * n[i];
	}
	Mtx_Multiply(out, (C3D_Mtx*)vp, &m);
}
