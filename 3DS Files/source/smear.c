// The speed smear and its nitrous boost (TASKS.md #399): Speed.elf's
// eMotionBlurEffect (0x801DC9B4), which eDisplayFrame runs over each player
// view after the frame is captured (DrawSmear 2, the GameCube's
// g_MotionBlurEnable). See smear.h.
//
// What it draws: the finished frame, captured, laid back over itself up to
// six times, each copy pulled in from the screen's edges by its distance
// along the camera's motion and blended in at its alpha -- a smear trailing
// behind everything the camera is moving past. Its tuning, all Speed.elf's
// .sdata / .data:
//
//   SmearMinSpeed 3, SmearMaxSpeed 20  the camera's speed (m/s) the smear
//                                      fades in over; under 1/255 of it
//                                      nothing is drawn
//   SmearMaxAlphaScale 1, SmearAlphaScale 0 (no override)
//   SmearDistances {2.3, 6.4, 8.8, 11.4, 22, 40}, SmearAlphas {50, 35, 28,
//   17, 8, 4} (the last two rewritten under nitrous), SmearSubStageEnable
//   {1, 1, 1, 1, 0, 0} (the last two on under nitrous)
//   SmearDistanceMultiplier 1.2, SmearInvert 1, num_smears 6
//   SmearR/G/B 130, Rsub/Gsub/Bsub -10/-10/-28 (each copy under nitrous is
//   10, 10 and 28 brighter than the last)
//   NOStimer 15, SmearDistModNOS 4, SmearNOSAlphaBonus 1.5
//
// The nitrous look (ScreenEffectDB effect 1, which UpdateAllScreenEmotion
// 0x8020CB6C adds with palette effect 0 while the player's bottle burns and
// is not a blow-off) ramps a counter up 2 a frame to NOStimer and, once it
// goes, down 1 a frame; while it is not 0 the distances are scaled by 2 up
// to SmearDistModNOS (4) along it, the alphas by SmearNOSAlphaBonus, the
// last two copies fade in to 8 and 4 along it, and the copies take the
// brightening colours.
//
// The capture is CaptureEFB(&DAT_803e19e8, 0, 1, GX_TF_I8): the frame's
// luminance, which the TEV (psMotionBlur 0x801E4F8C) multiplies by the
// copy's colour, the alpha the copy's own, blended SRC_ALPHA /
// INV_SRC_ALPHA (eViewPlatInterface::Render's 0x66). The EFB copy's Y is
// the GameCube's: 0.257 R + 0.504 G + 0.098 B + 16/255.
//
// The quad (in the view's 640x480): left X0 + (x + z) D, right X1 + (x - z) D,
// top Y0 + (y + z) D, bottom Y1 + (y - z) D, where (x, y, z) is the camera's
// velocity in its own frame, unit length, and D the copy's distance times
// the multipliers; the texture covers the whole screen on it. Moving
// straight ahead the copy shrinks to the middle, so each point of the screen
// trails a ghost of itself back towards where it came from; a sideways or
// vertical drift slides the copies with it. PORT: the frame here is x right,
// y down the screen, z ahead -- the one that keeps all three trailing, which
// SmearInvert 1 was set for; the GameCube's 640x480 is scaled to the 400x240
// screen per axis.
//
// PORT, the rest:
// - The capture is the eye's colour buffer copied raw (TextureCopy) into the
//   frontend's scene buffer (fe_post.h), idle while the world draws; the
//   luminance is the TEV's, from the RGBA8 copy.
// - The ramp counts the GameCube's frames (30 a second) in seconds, so it
//   takes as long as there at any frame rate.
// - The camera's velocity is its eye's move over the frame; a move of more
//   than SMEAR_CUT_M in a frame is a cut, not motion.
#include "smear.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fe_post.h"
#include "model3d.h"
#include "shaders.h"

#define N_SMEARS        6
#define NOS_TIMER       15.0f     // NOStimer
#define GC_FPS          30.0f
#define SMEAR_MIN_SPEED 3.0f
#define SMEAR_MAX_SPEED 20.0f
#define SMEAR_MAX_ALPHA 1.0f      // SmearMaxAlphaScale
#define SMEAR_DIST_MUL  1.2f      // SmearDistanceMultiplier
#define SMEAR_DIST_BASE 2.0f      // DAT_80333c9c, the distances' scale without nitrous
#define SMEAR_DIST_NOS  4.0f      // SmearDistModNOS
#define SMEAR_NOS_ALPHA 1.5f      // SmearNOSAlphaBonus
#define SMEAR_RGB       130       // SmearR, SmearG, SmearB
#define SMEAR_CUT_M     20.0f

#define PANEL_W 240
#define PANEL_H 400
#define TEX_W   256
#define TEX_H   512
#define GC_W    640.0f
#define GC_H    480.0f

static const float kDist[N_SMEARS] = { 2.3f, 6.4f, 8.8f, 11.4f, 22.0f, 40.0f };
static const int kSub[3] = { -10, -10, -28 };   // Rsub, Gsub, Bsub

typedef struct {
	float x, y;   // clip space
	float u, v;   // 0..1 across the screen
} SmearVert;

#define RING_FRAMES 3
#define RING_VERTS  (2 * N_SMEARS * 4)

static struct {
	bool tried, ready;
	void (*log)(const char*);
	shaderProgram_s prog;
	int uMap0;
	C3D_Tex tex;
	void* own;              // our buffer, when fe_post has none
	bool ownVram;           // own is smear_world_buffer()'s VRAM
	SmearVert* ring;
	int ringFrame, ringUsed;

	bool haveEye;
	float lastEye[3];
	float speedK;           // fVar7
	float vel[3];           // the camera frame's unit velocity
	// The nitrous ramp: r13-0x6614 and -0x6618.
	int nosState;
	float nosT;
	bool look;              // iVar21
	bool en4, en5;          // SmearSubStageEnable[4], [5]
	float alpha45[2];       // SmearAlphas[4], [5]
} s = { .alpha45 = { 8.0f, 4.0f } };

void smear_set_log(void (*sink)(const char* msg)) { s.log = sink; }

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
		say("smear: the post shader would not load -- no motion blur");
		return false;
	}
	shaderProgramInit(&s.prog);
	shaderProgramSetVsh(&s.prog, dvle);
	s.uMap0 = shaderInstanceGetUniformLocation(s.prog.vertexShader, "map0");
	s.ring = (SmearVert*)linearAlloc(sizeof(SmearVert) * RING_VERTS * RING_FRAMES);
	if (!s.ring) {
		say("smear: no linear memory for its quads -- no motion blur");
		return false;
	}
	s.ready = true;
	return true;
}

// The capture's texture: the frontend's scene buffer (at least 256x512
// RGBA8), or one of our own in linear memory without it.
static bool bindBuffer(void) {
	C3D_RenderTarget* fe = fepost_target();
	void* data = fe ? fe->frameBuf.colorBuf : NULL;
	if (!data) {
		if (!s.own) {
			s.own = linearAlloc(TEX_W * TEX_H * 4);
			char b[120];
			snprintf(b, sizeof(b), "smear: no frontend scene buffer -- %s",
				s.own ? "512 KB of linear memory for the capture" : "and no memory: no motion blur");
			say(b);
		}
		data = s.own;
	}
	if (!data)
		return false;
	if (s.tex.data != data) {
		memset(&s.tex, 0, sizeof(s.tex));
		s.tex.data = data;
		s.tex.fmt = GPU_RGBA8;
		s.tex.size = TEX_W * TEX_H * 4;
		s.tex.width = TEX_W;
		s.tex.height = TEX_H;
		s.tex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
		C3D_TexSetFilter(&s.tex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&s.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	return true;
}

void smear_update(bool on, float dt, const float eye[3], const float right[3],
	const float up[3], const float fwd[3], bool nos) {
	s.ringFrame = (s.ringFrame + 1) % RING_FRAMES;
	s.ringUsed = 0;
	if (!on) {
		s.haveEye = false;
		s.speedK = 0.0f;
		s.nosState = 0;
		s.nosT = 0.0f;
		s.look = s.en4 = s.en5 = false;
		return;
	}

	// The camera's velocity, and its speed factor (the SmearStage2 branch).
	float v[3] = { 0.0f, 0.0f, 0.0f };
	if (s.haveEye && dt > 0.0f) {
		const float d[3] = { eye[0] - s.lastEye[0], eye[1] - s.lastEye[1], eye[2] - s.lastEye[2] };
		const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (dl <= SMEAR_CUT_M)
			for (int i = 0; i < 3; i++)
				v[i] = d[i] / dt;
	}
	memcpy(s.lastEye, eye, sizeof(s.lastEye));
	s.haveEye = true;
	const float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	float k = 0.0f;
	if (speed > SMEAR_MIN_SPEED) {
		k = 1.0f;
		if (speed < SMEAR_MAX_SPEED)
			k = (speed - SMEAR_MIN_SPEED) / (SMEAR_MAX_SPEED - SMEAR_MIN_SPEED);
	}
	if (k > SMEAR_MAX_ALPHA)
		k = SMEAR_MAX_ALPHA;
	s.speedK = k;
	if (speed > 0.0f) {
		// x right, y down the screen, z ahead (smear.c's PORT note).
		s.vel[0] = (v[0] * right[0] + v[1] * right[1] + v[2] * right[2]) / speed;
		s.vel[1] = -(v[0] * up[0] + v[1] * up[1] + v[2] * up[2]) / speed;
		s.vel[2] = (v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2]) / speed;
	}

	// The nitrous ramp, eMotionBlurEffect's head, in the GameCube's frames.
	const float frames = dt * GC_FPS;
	if (!nos) {
		s.look = false;
		if (s.nosState != 0) {
			s.nosState = 2;
			s.look = true;
		} else {
			s.en4 = s.en5 = false;
		}
	} else {
		if (s.nosState == 0) {
			s.nosT = 0.0f;
			s.nosState = 1;
		}
		s.look = true;
		s.en4 = s.en5 = true;
	}
	if (s.nosState == 1) {
		s.nosT += 2.0f * frames;
		if (s.nosT >= NOS_TIMER) {
			s.nosT = NOS_TIMER;
			s.nosState = 3;
		}
	} else if (s.nosState == 2) {
		s.nosT -= frames;
		if (s.nosT < 1.0f) {
			s.nosState = 0;
			s.nosT = 0.0f;
		}
	}
	if (s.look) {
		s.alpha45[0] = s.nosT * 8.0f / NOS_TIMER;
		s.alpha45[1] = s.nosT * 4.0f / NOS_TIMER;
	}
}

// Screen (400x240, y down) to the top target's clip space: Mtx_OrthoTilt's
// map, as citro2d projects the top screen.
static void toClip(float sx, float sy, float* cx, float* cy) {
	*cx = 1.0f - sy / 120.0f;
	*cy = 1.0f - sx / 200.0f;
}

// TEV stage `id` with constant `rgba` (0xAABBGGRR); the alpha passes through
// from `asrc`.
static C3D_TexEnv* stage(int id, GPU_COMBINEFUNC func, GPU_TEVSRC a, GPU_TEVSRC b,
	GPU_TEVSRC c, GPU_TEVSRC asrc, u32 rgba) {
	C3D_TexEnv* env = C3D_GetTexEnv(id);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, a, b, c);
	C3D_TexEnvFunc(env, C3D_RGB, func);
	C3D_TexEnvSrc(env, C3D_Alpha, asrc, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, rgba);
	return env;
}

// The capture: the eye's 240x400 colour buffer, a tile row (8 lines of 240
// RGBA8 pixels, 7680 bytes) at a time, into the texture's rows of 256 (8192),
// queued after the commands so far; `target` bound again after it.
static void capture(C3D_RenderTarget* target) {
	const u32 line = PANEL_W * 8 * 4;
	const u32 gap = (TEX_W - PANEL_W) * 8 * 4;
	// #481: the copy submits the draws so far itself, unflushed; on the 3DS
	// a running queue then reads the list before the cache has written it.
	C3D_FrameSplit(GX_CMDLIST_FLUSH);
	C3D_SyncTextureCopy((u32*)target->frameBuf.colorBuf, GX_BUFFER_DIM(line >> 4, 0),
		(u32*)s.tex.data, GX_BUFFER_DIM(line >> 4, gap >> 4), line * (PANEL_H / 8),
		GX_TRANSFER_RAW_COPY(1));
	C3D_FrameDrawOn(target);
}

const C3D_Tex* smear_capture(C3D_RenderTarget* target, bool copied) {
	if (!target || !bindBuffer())
		return NULL;
	if (!copied)
		capture(target);
	return &s.tex;
}

const C3D_Tex* smear_texture(void) {
	return bindBuffer() ? &s.tex : NULL;
}

bool smear_draw(C3D_RenderTarget* target) {
	if (s.speedK < 1.0f / 255.0f || !target || !init() || !bindBuffer())
		return false;
	if (s.ringUsed + N_SMEARS * 4 > RING_VERTS)
		return false;

	// The copy's colours and alphas, packed as the GameCube packs them (an
	// OR, so the sixth copy's blue of 270 spills into its green).
	const float bonus = s.look ? SMEAR_NOS_ALPHA : 1.0f;
	const float distK = s.look
		? s.nosT * (SMEAR_DIST_NOS - SMEAR_DIST_BASE) / NOS_TIMER + SMEAR_DIST_BASE
		: SMEAR_DIST_BASE;
	static const float kAlpha[4] = { 50.0f, 35.0f, 28.0f, 17.0f };
	SmearVert* q = s.ring + s.ringFrame * RING_VERTS + s.ringUsed;
	u32 rgba[N_SMEARS];
	int n = 0;
	int first = s.ringUsed;
	for (int i = 0; i < N_SMEARS; i++) {
		if (i == 4 && !s.en4) continue;
		if (i == 5 && !s.en5) continue;
		const float a = i < 4 ? kAlpha[i] : s.alpha45[i - 4];
		const u32 alpha = (u32)(int)(a * s.speedK * bonus);
		if (!alpha)
			continue;
		u32 c[3];
		for (int j = 0; j < 3; j++)
			c[j] = (u32)(SMEAR_RGB - (s.look ? i * kSub[j] : 0));
		const u32 gx = alpha | c[2] << 8 | c[1] << 16 | c[0] << 24;   // RGBA, as GX
		rgba[n] = (gx >> 24) | ((gx >> 8) & 0xFF00u) | ((gx << 8) & 0xFF0000u) | (gx << 24);

		// The quad in the GameCube's 640x480, then the screen's 400x240.
		const float D = kDist[i] * distK * SMEAR_DIST_MUL;
		const float x = s.vel[0], y = s.vel[1], z = s.vel[2];
		float l = (x + z) * D, t = (y + z) * D;
		float r = GC_W + (x - z) * D, b = GC_H + (y - z) * D;
		r = l + fabsf(r - l);
		b = t + fabsf(b - t);
		const float sx[2] = { l * (400.0f / GC_W), r * (400.0f / GC_W) };
		const float sy[2] = { t * (240.0f / GC_H), b * (240.0f / GC_H) };
		// Strip order: (l,t) (r,t) (l,b) (r,b); each corner shows the screen
		// corner it stands for.
		for (int k = 0; k < 4; k++) {
			const int ix = k & 1, iy = k >> 1;
			SmearVert* p = &q[n * 4 + k];
			toClip(sx[ix], sy[iy], &p->x, &p->y);
			float ux, uy;
			toClip(ix ? 400.0f : 0.0f, iy ? 240.0f : 0.0f, &ux, &uy);
			p->u = (ux + 1.0f) * 0.5f;
			p->v = (uy + 1.0f) * 0.5f;
		}
		n++;
	}
	if (!n)
		return false;
	s.ringUsed += n * 4;
	GSPGPU_FlushDataCache(q, sizeof(SmearVert) * 4 * n);

	capture(target);

	C3D_BindProgram(&s.prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s.ring + s.ringFrame * RING_VERTS, sizeof(SmearVert), 2, 0x10);
	// The screen's place in the texture: its 240 columns of 256 and its 400
	// rows of 512 at the top (t = 1 is the buffer's first row).
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s.uMap0, (float)PANEL_W / TEX_W, (float)PANEL_H / TEX_H,
		0.0f, 1.0f - (float)PANEL_H / TEX_H);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	model3d_post_pass_prime();   // #486: no fog on these quads, on hardware too
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_ZERO, GPU_ONE);
	C3D_ProcTexBind(0, NULL);
	C3D_TexBind(0, &s.tex);
	// The luminance: stage 0 t/2 + 1/2 (0x80 both), stage 1 the dot product
	// with 1/2 + w/2 (DOT3 takes 4 (a - 1/2).(b - 1/2), which is t.w), stage 2
	// + 16/255, stage 3 times the copy's colour, its alpha the copy's.
	stage(0, GPU_MULTIPLY_ADD, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT,
		0xFF808080u);
	stage(1, GPU_DOT3_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT, GPU_PREVIOUS,
		0xFF000000u | (u32)(0.5f * 255 + 0.098f * 127.5f + 0.5f) << 16 |
		(u32)(0.5f * 255 + 0.504f * 127.5f + 0.5f) << 8 | (u32)(0.5f * 255 + 0.257f * 127.5f + 0.5f));
	stage(2, GPU_ADD, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT, GPU_PREVIOUS, 0xFF101010u);
	for (int i = 4; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	for (int i = 0; i < n; i++) {
		// Stage 0 carries the copy's alpha down; stage 3 its colour.
		C3D_TexEnvColor(C3D_GetTexEnv(0), 0x00808080u | (rgba[i] & 0xFF000000u));
		stage(3, GPU_MODULATE, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT, GPU_PREVIOUS, rgba[i]);
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, first + i * 4, 4);
	}

	for (int i = 0; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	return true;
}

void smear_world_buffer(bool on) {
	if (on) {
		if (s.own || fepost_target())
			return;
		s.own = vramAlloc(TEX_W * TEX_H * 4);
		s.ownVram = s.own != NULL;
		char b[120];
		snprintf(b, sizeof(b), "smear: the capture in VRAM of its own while the world is open, "
			"512 KB (%lu KB free)", (unsigned long)(vramSpaceFree() >> 10));
		if (s.own)
			say(b);
	} else if (s.own && s.ownVram) {
		vramFree(s.own);
		s.own = NULL;
		s.ownVram = false;
		memset(&s.tex, 0, sizeof(s.tex));
	}
}

void smear_debug(float* speedK, float* nosT, int* nosState) {
	if (speedK) *speedK = s.speedK;
	if (nosT) *nosT = s.nosT;
	if (nosState) *nosState = s.nosState;
}
