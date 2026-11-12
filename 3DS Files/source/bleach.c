// Bleach Bypass (TASKS.md #326): SPEED2.EXE's g_BleachByPassEnable
// (DAT_00870d00, +0x50). See bleach.h.
//
// What the PC does. FUN_005caad0, at the end of the in-game view, calls
// FUN_005c68d0(p), p = 0.2 + (1 - 0.2) x (0x7ff180, 0x7ff184), x the view's
// camera's +0x188 when its +0x30 is set and 0 otherwise in a race or free
// roam (0.8 in the frontend). That sets the curves' strengths
// c0..c3 = 0.5 + (0.2 - 0.5) p (0x7ff12c.., 0x7ff13c..) and the tint's
// k = 0.25 p (0x7ff128; DAT_008708ec is never written), rebuilds the 32x32
// gain map when they change (FUN_005bbe80) and runs effect 0x11's technique
// 0, three passes, each over the last one's result:
//
//   p0 (ps_1_4 at 0x444738): L = 0.3 r + 0.59 g + 0.11 b, A = G(L, c3),
//      rgb = G(rgb, c0..c2) (1 - A) + rgb A, alpha = A (1 - A) + G(r, c0) A
//   p1 (0x443ea8): rgb += sat(1 - max(r, g) + b) x 0.4 (SaturationScale,
//      0x7a27d4) x k (t0, t1, t2)
//   p2 (0x4435bc): rgb = alpha Tw + (r, g, R(g)) (1 - Tw)
//
// with the gain map's curve G(x, a) (FUN_005bbdb0, pivot 0.4 at 0x7ff7ac):
// f = (1/a - 2)(1 - x / 0.4), x / (f + 1) below the pivot and
// (f - x) / (f - 1) above it -- the identity at a = 0.5, a gentle S at the
// world's 0.44 -- and the ramp map's blue R(x) = x / (1 + (1/0.45 - 2)(1 - x))
// (0x7ff18c), which p2 reads at the green: the grade takes its blue from its
// green, which is what greys a yellow. The tint (t0..t2, 0x7ff15c..) and Tw
// (0x7ff188) are what the frontend left, (0.5, 0.5, 0.5) and 0.2, unless the
// camera's +0x30 brings its own (+0x178.., +0x1cc).
//
// PORT: the PICA200 reads no texture through another, so the gain map and the
// ramp map are folded into one pass of six TEV stages over the frame's
// capture (smear.h), fitted by least squares to the PC's three at the
// world's p = 0.2 (no camera here sets +0x30):
//
//   a1  = L (1 - L) + r L          (p0's alpha, the curve G left out)
//   out = (0.8246 r, 0.8246 g, g (0.6886 + 0.1274 g)) + 0.2 a1
//
// 2/255 from the PC on average over the colour cube, 8 at worst; p1's tint
// is under 1/100 at p = 0.2 and is left out.
//
// The PC's FUN_005c7860 -- the rain's haze and the area's screen tint --
// runs before the grade (FUN_005caad0 calls it first), so with the grade on
// the two quads main.c draws over the frame with the 2D are drawn here
// instead, before the capture, with the same blends.
#include "bleach.h"

#include <stdio.h>
#include <string.h>

#include "model3d.h"
#include "shaders.h"
#include "smear.h"

#define PANEL_W 240
#define PANEL_H 400
#define TEX_W   256
#define TEX_H   512

typedef struct {
	float x, y;   // clip space
	float u, v;   // 0..1 across the screen
} BleachVert;

static struct {
	bool tried, ready;
	void (*log)(const char*);
	shaderProgram_s prog;
	int uMap0;
	BleachVert* quad;
	bool said;
} s;

void bleach_set_log(void (*sink)(const char* msg)) { s.log = sink; }

static void say(const char* m) {
	if (s.log)
		s.log(m);
}

static bool init(void) {
	if (s.tried)
		return s.ready;
	s.tried = true;
	DVLE_s* dvle = shaders_get(SHADER_POST);
	if (!dvle) {
		say("bleach: the post shader would not load -- no bleach bypass");
		return false;
	}
	shaderProgramInit(&s.prog);
	shaderProgramSetVsh(&s.prog, dvle);
	s.uMap0 = shaderInstanceGetUniformLocation(s.prog.vertexShader, "map0");
	s.quad = (BleachVert*)linearAlloc(sizeof(BleachVert) * 4);
	if (!s.quad) {
		say("bleach: no linear memory for the quad -- no bleach bypass");
		return false;
	}
	// The whole screen as a strip, each corner showing its own corner of the
	// capture (trails.c's quad).
	static const float kClip[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
	for (int k = 0; k < 4; k++) {
		s.quad[k].x = kClip[k][0];
		s.quad[k].y = kClip[k][1];
		s.quad[k].u = (kClip[k][0] + 1.0f) * 0.5f;
		s.quad[k].v = (kClip[k][1] + 1.0f) * 0.5f;
	}
	GSPGPU_FlushDataCache(s.quad, sizeof(BleachVert) * 4);
	s.ready = true;
	return true;
}

// TEV stage `id`, its constant `k` (0xAABBGGRR), each stage reading the
// capture as unit 0.
static C3D_TexEnv* stage(int id, u32 k) {
	C3D_TexEnv* env = C3D_GetTexEnv(id);
	C3D_TexEnvInit(env);
	C3D_TexEnvColor(env, k);
	return env;
}

#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)

// The quad's program, vertices and map, culling, the alpha and depth tests
// off.
static void bindQuad(void) {
	C3D_BindProgram(&s.prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s.quad, sizeof(BleachVert), 2, 0x10);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s.uMap0, (float)PANEL_W / TEX_W, (float)PANEL_H / TEX_H,
		0.0f, 1.0f - (float)PANEL_H / TEX_H);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	model3d_post_pass_prime();   // #486: no fog on these quads, on hardware too
}

// A flat quad of `rgba` over the bound eye, blended `src` x it + `dst` x
// the frame, colour only (main.c's rainHazeDrawRect / screenTintDrawRect).
static void flat(u32 rgba, GPU_BLENDFACTOR src, GPU_BLENDFACTOR dst) {
	C3D_TexEnv* e = stage(0, rgba);
	C3D_TexEnvSrc(e, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, src, dst, GPU_ZERO, GPU_ONE);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
}

bool bleach_draw(C3D_RenderTarget* eyeTarget, const u8* haze, const u8* tint) {
	if (!eyeTarget || !init() || !smear_texture())
		return false;
	if (!s.said) {
		s.said = true;
		say("bleach: the visual treatment's grade over the world (FUN_005c68d0 at p = 0.2)");
	}
	if (haze || tint) {
		C3D_FrameDrawOn(eyeTarget);
		bindQuad();
		C3D_ProcTexBind(0, NULL);
		// The haze: frame x 2 x haze (DST_COLOR, SRC_COLOR); then the tint:
		// frame x (tint + 0x80/255) (DST_COLOR, SRC_ALPHA).
		if (haze)
			flat(RGBA(haze[0], haze[1], haze[2], 0xFF), GPU_DST_COLOR, GPU_SRC_COLOR);
		if (tint)
			flat(RGBA(tint[0], tint[1], tint[2], 0x80), GPU_DST_COLOR, GPU_SRC_ALPHA);
	}
	const C3D_Tex* tex = smear_capture(eyeTarget, false);
	if (!tex)
		return false;
	C3D_FrameDrawOn(eyeTarget);
	bindQuad();
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_ProcTexBind(0, NULL);
	C3D_TexBind(0, (C3D_Tex*)tex);

	// S0: colour (0, 0, 0.1274 g); alpha 0.3 r.
	C3D_TexEnv* e = stage(0, RGBA(0, 0, 32, 77));
	C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvOpAlpha(e, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
	// S1: colour (prev + (0, 0.8246, 0.6886)) g; alpha 0.59 g + prev.
	e = stage(1, RGBA(0, 210, 176, 150));
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEXTURE0);
	C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_G);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_ADD_MULTIPLY);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvOpAlpha(e, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_MULTIPLY_ADD);
	// S2: colour (0.8246 r, 0, 0) + prev; alpha 0.11 b + prev = L, kept in
	// the combiner buffer for S4 (a stage's update is read two stages on).
	e = stage(2, RGBA(210, 0, 0, 28));
	C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvOpAlpha(e, GPU_TEVOP_A_SRC_B, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(e, C3D_Both, GPU_MULTIPLY_ADD);
	C3D_TexEnvBufUpdate(C3D_Alpha, 1 << 2);
	// S3: colour kept; alpha L (1 - L).
	e = stage(3, 0);
	C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvOpAlpha(e, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA,
		GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_MODULATE);
	// S4: colour kept; alpha r L + prev = a1.
	e = stage(4, 0);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_TEXTURE0, GPU_PREVIOUS_BUFFER, GPU_PREVIOUS);
	C3D_TexEnvOpAlpha(e, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_MULTIPLY_ADD);
	// S5: colour 0.2 a1 + prev; alpha the frame's own.
	e = stage(5, RGBA(51, 51, 51, 255));
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MULTIPLY_ADD);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_TEXTURE0, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);

	// The world's passes' state again (trails.c's restore()).
	C3D_TexEnvBufUpdate(C3D_Both, 0);
	for (int i = 0; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	return true;
}
