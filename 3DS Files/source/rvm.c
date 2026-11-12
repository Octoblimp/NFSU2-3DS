// See rvm.h. The SPEED2.EXE functions named here are in
// tools/re/wip501/NOTES.md, with the GameCube's (Speed.elf) beside them.
#include "rvm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model3d.h"
#include "shaders.h"

// The target: the PC's 256x256 at the 3:1 the screen shows it at (240x80 of
// 640x480), so no line of it is drawn only to be dropped.
#define RVM_TEX_W  256
#define RVM_TEX_H  64
// RearViewMirrorCameraMover::Update (0x44ba80): bAngle 20000, near 0.75 and
// far 192 (0x7870d4 / 0x7870d8). FUN_005c7ec0 makes a square target's
// frustum square.
#define RVM_FOV_U16 20000
#define RVM_NEAR    0.75f
#define RVM_FAR     192.0f
// FUN_005cbf40's quad, x 200..440 and y 15..95 of 640x480, on the 400x240
// top screen.
#define RVM_X0 (200.0f * 400.0f / 640.0f)
#define RVM_X1 (440.0f * 400.0f / 640.0f)
#define RVM_Y0 (15.0f * 240.0f / 480.0f)
#define RVM_Y1 (95.0f * 240.0f / 480.0f)

typedef struct {
	float x, y;   // clip space
	float u, v;   // the texture's, t = 1 at the top
} RvmVert;

struct Rvm {
	C3D_Tex           tex;
	C3D_RenderTarget* target;
	shaderProgram_s   prog;
	bool              progInit;
	int               uMap[3];
	RvmVert*          quads;   // [0..3] the clear, [4..7] the mirror
	void            (*log)(const char*);
};

static void say(void (*log)(const char*), const char* what) {
	if (!log)
		return;
	char b[200];
	snprintf(b, sizeof(b), "rvm: %s", what);
	log(b);
}

void rvm_destroy(Rvm* r) {
	if (!r)
		return;
	if (r->target)
		C3D_RenderTargetDelete(r->target);
	if (r->tex.data)
		C3D_TexDelete(&r->tex);
	if (r->quads)
		linearFree(r->quads);
	if (r->progInit)
		shaderProgramFree(&r->prog);
	free(r);
}

// Screen (400x240, y down) to the top target's clip space: Mtx_OrthoTilt's
// map, as citro2d projects the top screen (smear.c's toClip).
static void toClip(float sx, float sy, float* cx, float* cy) {
	*cx = 1.0f - sy / 120.0f;
	*cy = 1.0f - sx / 200.0f;
}

Rvm* rvm_create(void (*log)(const char*)) {
	Rvm* r = (Rvm*)calloc(1, sizeof(Rvm));
	if (!r) {
		say(log, "no memory -- no rear-view mirror");
		return NULL;
	}
	r->log = log;
	const u32 vram0 = vramSpaceFree();
	bool ok = C3D_TexInitVRAM(&r->tex, RVM_TEX_W, RVM_TEX_H, GPU_RGB565);
	if (ok) {
		C3D_TexSetFilter(&r->tex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&r->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		r->target = C3D_RenderTargetCreateFromTex(&r->tex, GPU_TEXFACE_2D, 0,
			GPU_RB_DEPTH24_STENCIL8);
		ok = r->target != NULL;
	}
	if (!ok) {
		say(log, "no VRAM for the target -- no rear-view mirror");
		rvm_destroy(r);
		return NULL;
	}
	DVLE_s* dvle = shaders_get(SHADER_POST);
	if (!dvle) {
		say(log, "the post shader would not load -- no rear-view mirror");
		rvm_destroy(r);
		return NULL;
	}
	shaderProgramInit(&r->prog);
	shaderProgramSetVsh(&r->prog, dvle);
	r->progInit = true;
	static const char* const kMap[3] = { "map0", "map1", "map2" };
	for (int i = 0; i < 3; i++)
		r->uMap[i] = shaderInstanceGetUniformLocation(r->prog.vertexShader, kMap[i]);
	r->quads = (RvmVert*)linearAlloc(sizeof(RvmVert) * 8);
	if (!r->quads) {
		say(log, "no linear memory for its quads -- no rear-view mirror");
		rvm_destroy(r);
		return NULL;
	}
	// The clear: the whole target.
	static const RvmVert kClear[4] = {
		{ -1.0f, -1.0f, 0, 0 }, { 1.0f, -1.0f, 0, 0 }, { -1.0f, 1.0f, 0, 0 }, { 1.0f, 1.0f, 0, 0 },
	};
	memcpy(r->quads, kClear, sizeof(kClear));
	// The mirror, strip order (l,t) (r,t) (l,b) (r,b). FUN_005cbf40's UVs put
	// u = 1 at the left: the view mirrored, as a mirror shows it.
	const float sx[2] = { RVM_X0, RVM_X1 }, sy[2] = { RVM_Y0, RVM_Y1 };
	for (int k = 0; k < 4; k++) {
		const int ix = k & 1, iy = k >> 1;
		RvmVert* p = &r->quads[4 + k];
		toClip(sx[ix], sy[iy], &p->x, &p->y);
		p->u = ix ? 0.0f : 1.0f;
		p->v = iy ? 0.0f : 1.0f;
	}
	GSPGPU_FlushDataCache(r->quads, sizeof(RvmVert) * 8);
	char b[160];
	snprintf(b, sizeof(b), "rvm: %dx%d target, %lu KB VRAM (%lu KB left)", RVM_TEX_W, RVM_TEX_H,
		(unsigned long)((vram0 - vramSpaceFree()) >> 10), (unsigned long)(vramSpaceFree() >> 10));
	if (log)
		log(b);
	return r;
}

// The program and the quads bound, every unit's map the quad's own UVs.
static void bindQuads(Rvm* r) {
	C3D_BindProgram(&r->prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, r->quads, sizeof(RvmVert), 2, 0x10);
	for (int i = 0; i < 3; i++)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uMap[i], 1.0f, 1.0f, 0.0f, 0.0f);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	model3d_post_pass_prime();
}

void rvm_begin(Rvm* r, const float eye[3], const float fwd[3], const float up[3],
		C3D_Mtx* vp, CarNeonView* view) {
	// FUN_005caad0's clear, 0xff000000 with Z: drawn, as envmap.c's envClear()
	// is and for its reason (#307) -- black, at depth 0, the far end the
	// world's GPU_GREATER tests against (C3D_DepthMap's zero scale).
	C3D_FrameDrawOn(r->target);
	bindQuads(r);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
	C3D_DepthMap(true, 0.0f, 0.0f);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvColor(env, 0xFF000000u);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

	// The camera: x the way `fwd` x `up` points, y up, looking down -z.
	C3D_Mtx proj, look;
	const float half = (float)RVM_FOV_U16 * ((float)M_PI / 65536.0f);
	Mtx_Persp(&proj, half * 2.0f, 1.0f, RVM_NEAR, RVM_FAR, false);
	Mtx_LookAt(&look, FVec3_New(eye[0], eye[1], eye[2]),
		FVec3_New(eye[0] + fwd[0], eye[1] + fwd[1], eye[2] + fwd[2]),
		FVec3_New(up[0], up[1], up[2]), false);
	Mtx_Multiply(vp, &proj, &look);

	// carlights.c's view: its screen test is +-200 x +-120 "pixels", which
	// here is the frustum's edge.
	float rt[3] = { fwd[1] * up[2] - fwd[2] * up[1], fwd[2] * up[0] - fwd[0] * up[2],
	                fwd[0] * up[1] - fwd[1] * up[0] };
	const float rl = sqrtf(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
	for (int i = 0; i < 3; i++) {
		rt[i] = rl > 1e-6f ? rt[i] / rl : 0.0f;
		view->eye[i] = eye[i];
		view->fwd[i] = fwd[i];
		view->right[i] = rt[i];
	}
	view->up[0] = rt[1] * fwd[2] - rt[2] * fwd[1];
	view->up[1] = rt[2] * fwd[0] - rt[0] * fwd[2];
	view->up[2] = rt[0] * fwd[1] - rt[1] * fwd[0];
	const float t = tanf(half);
	view->pxX = 200.0f / t;
	view->pxY = 120.0f / t;
	view->fov = RVM_FOV_U16;
}

void rvm_draw(Rvm* r, const C3D_Tex* frame, const C3D_Tex* mask) {
	if (!r)
		return;
	bindQuads(r);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	// The screen keeps its alpha.
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_ZERO, GPU_ONE);
	C3D_ProcTexBind(0, NULL);
	C3D_TexBind(0, &r->tex);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if (frame && mask) {
		// RVM blended over the capture (SRCALPHA / INVSRCALPHA, black): the
		// view times 1 - RVM's alpha; then RVM_MASK's alpha written over the
		// capture's, which the screen quad blends with.
		C3D_TexBind(1, (C3D_Tex*)frame);
		C3D_TexBind(2, (C3D_Tex*)mask);
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, GPU_PRIMARY_COLOR);
		C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA,
			GPU_TEVOP_RGB_SRC_COLOR);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE2, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	} else {
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(env, 0xFF000000u);
	}
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 4, 4);
	for (int i = 0; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}
