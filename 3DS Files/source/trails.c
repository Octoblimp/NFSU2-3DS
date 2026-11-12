// Light Trails (TASKS.md #326): SPEED2.EXE's g_LightStreaksEnable
// (DAT_00870ce0, the display block's +0x30, "Light Trails" in the options).
// See trails.h.
//
// What the PC does (FUN_005caad0, the in-game view's passes): with Light
// Glow (DAT_00870ce4) off no flare pass runs; with it on and the trails off
// the flare pass draws on the back buffer -- FUN_00491860 and FUN_00494270
// (the world's lamps and the props' flare pool), FUN_0061ae90 (RenderFlares:
// every car's flares and their nitrous streak), FUN_0061ace0, and
// FUN_0061b1f0 / FUN_0061ac90 (the cars' headlight light on the road). With
// both on, FUN_005cbb00 draws that same pass into a buffer of its own
// (DAT_0086ff70, the screen's size, on the scene's depth-stencil) instead:
//
//   1. the buffer faded: a quad of WHITE16X16_NOALPHA over it blended
//      ZERO / BLENDFACTOR with D3DRS_BLENDFACTOR 0xFF785AAA -- each frame
//      the buffer keeps 0x78/255 of its red, 0x5A/255 of its green and
//      0xAA/255 of its blue, so a light leaves a trail that fades through
//      blue as the camera moves past it;
//   2. the flare pass into it, as it draws on the screen;
//   3. the buffer added to the back buffer, ONE / ONE (FUN_005c6be0's
//      blend 1, 2, 2) -- straight away with Over Bright off (DAT_00870cfc),
//      in the visual treatment's composite with it on (DAT_008707ce, which
//      Over Bright, Light Glow and Light Trails set together).
//
// The buffer is never cleared: a light that holds still settles at its
// flare over (1 - keep), up to 2.9 times as bright in blue.
//
// PORT:
// - One buffer per eye (the right eye's only with the 3-D on), 240x400
//   RGBA8, in VRAM when it has room (FCRAM otherwise), on the eye's own
//   depth buffer (stereo.c shares the left eye's with the right): the flares
//   test against the street the eye drew, as they do on the screen.
// - The fade is the PC's per frame at 60 frames a second, so a trail lasts
//   as long at any frame rate: keep^(60 dt).
// - A buffer not drawn into last frame (the trails off, the world shut, a
//   cut to the frontend) is cleared by the same quad at keep 0 instead of
//   carrying stale light.
// - The composite samples a copy: the buffer TextureCopied into the speed
//   smear's capture texture (smear.h), which the smear only takes after.
//
// Over Bright (#326): g_OverBrightEnable (DAT_00870cfc, +0x4C), which counts
// only with Light Glow and Light Trails on (DAT_008707ce). Then the buffer is
// not added over the screen after the flare pass but at the end of the view
// (the visual treatment, FUN_005cc8b0 / FUN_005cca00), after the PC has put
// the frame's own glow into it:
//
//   C    the frame, boxed down to half the screen (StretchRect to 0x86ffa8,
//        copied to 0x86ffb0)
//   A    the overbright ps (0x4407d0) over C: (a l)^2, a = sat(4 (C - o)),
//        l = GreyScale . a, o = [0x7ff10c] / 255 / 2
//   A    blurred along x then y, twice ([0x7ff110] = 2): 1/3 at +-1 texel,
//        1/6 at +-3 (0x7ff1cc, 0x7ff19c)
//   A    -= C x BLENDFACTOR (D3DBLENDOP_REVSUBTRACT): 0xb0 in a race or free
//        roam (gameflow 6), 0xa0 in the frontend
//   buf  += A x BLENDFACTOR [0x7ff114]
//
// [0x7ff10c] and [0x7ff114] are FUN_005bc100's, once a view (FUN_005d2850 ->
// FUN_005bc180): s eases 0.2 a frame toward 0.2 + 0.8 x, x the view's
// camera's +0x1e0 when its +0x3c is set and 0 otherwise in the world (0.45
// in the frontend), and the threshold is 216 + (62 - 216) s, the add
// 10 + (72 - 10) s, both truncated. With no camera asking otherwise s
// settles at 0.2: a threshold of 185, an add of 22. The port takes those
// settled values; no camera here sets +0x3c.
//
// PORT: the glow is 120x200, half the panel's 240x400, as the PC's is half
// its 640x480 (fe_post.c's glowDiv 2, whose passes these are), and the blur's
// taps are spaced to the PC's 320 across the screen's long axis.
#include "trails.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "model3d.h"
#include "shaders.h"
#include "smear.h"

#define PANEL_W    240
#define PANEL_H    400
#define TEX_W      256
#define TEX_H      512
#define TRAIL_EYES 2
#define PC_FPS     60.0f

// D3DRS_BLENDFACTOR 0xFF785AAA: what the fade keeps of red, green and blue.
static const float kKeep[3] = { 0x78 / 255.0f, 0x5A / 255.0f, 0xAA / 255.0f };

// Over Bright's glow: half the panel, in buffers a texture's width wide and
// 8 rows past the glow, left black for the blur to read past its edge.
#define GLOW_W     120
#define GLOW_H     200
#define GLOW_BUF_W 128
#define GLOW_BUF_H 208
#define GLOW_TEX_H 256
#define PC_GLOW_W  320   // the PC's glow across its screen, half of 640

// SPEED2.EXE's in-world constants (see above), as the TEV's 8-bit greys.
#define OB_THRESHOLD 185  // [0x7ff10c], settled
#define OB_OFF  (OB_THRESHOLD / 2)  // the ps halves it
#define OB_GREY 75        // GreyScale, 150 halved (fe_post.c has why)
#define BLUR_1  85        // 1/3, at -1 and +1
#define BLUR_3A 43        // 1/6, at -3 ...
#define BLUR_3B 42        // ... and +3: the four make 255
#define OB_SUB  0xB0      // the reverse subtract's BLENDFACTOR in the world
#define OB_ADD  22        // [0x7ff114], settled

typedef struct {
	C3D_RenderTarget target;   // not linked, no depth
	C3D_Tex tex;               // a view of its colour buffer; owns nothing
	void* buf;
	bool vram;
} GlowBuf;

typedef struct {
	float x, y;   // clip space
	float u, v;   // 0..1 across the screen
} TrailVert;

typedef struct {
	C3D_RenderTarget target;   // not linked: never shown, never transferred
	void* buf;
	bool vram;
	void* depth;               // its own depth, the eye's copied in each frame
	bool depthVram;
	GPU_DEPTHBUF depthFmt;
	bool failed;
	u16 width;                 // the eye target's, 240
	u32 drawn;                 // the frame it was last drawn into
} TrailEye;

static struct {
	bool tried, ready;
	void (*log)(const char*);
	shaderProgram_s prog;
	int uMap0;
	int uMap[3];               // map0 is uMap0 too
	TrailVert* quad;
	TrailEye eye[TRAIL_EYES];
	u32 frame;
	bool on;
	bool post;                 // Over Bright: the composite at the view's end
	u32 keep;                  // the fade's colour, 0xAABBGGRR
	GlowBuf glow[2];
	bool glowFailed, glowCleared;
} s;

void trails_set_log(void (*sink)(const char* msg)) { s.log = sink; }

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
		say("trails: the post shader would not load -- no light trails");
		return false;
	}
	shaderProgramInit(&s.prog);
	shaderProgramSetVsh(&s.prog, dvle);
	s.uMap0 = shaderInstanceGetUniformLocation(s.prog.vertexShader, "map0");
	s.uMap[0] = s.uMap0;
	s.uMap[1] = shaderInstanceGetUniformLocation(s.prog.vertexShader, "map1");
	s.uMap[2] = shaderInstanceGetUniformLocation(s.prog.vertexShader, "map2");
	s.quad = (TrailVert*)linearAlloc(sizeof(TrailVert) * 4);
	if (!s.quad) {
		say("trails: no linear memory for the quad -- no light trails");
		return false;
	}
	// The whole screen, as a strip: (left, top) (right, top) (left, bottom)
	// (right, bottom) in the top screen's 400x240, through Mtx_OrthoTilt's
	// map (smear.c's toClip); each corner shows its own corner of the buffer.
	static const float kClip[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
	for (int k = 0; k < 4; k++) {
		s.quad[k].x = kClip[k][0];
		s.quad[k].y = kClip[k][1];
		s.quad[k].u = (kClip[k][0] + 1.0f) * 0.5f;
		s.quad[k].v = (kClip[k][1] + 1.0f) * 0.5f;
	}
	GSPGPU_FlushDataCache(s.quad, sizeof(TrailVert) * 4);
	s.ready = true;
	return true;
}

void trails_frame(bool on, bool overBright, float dt) {
	s.frame++;
	s.on = on;
	s.post = on && overBright;
	if (!on)
		return;
	float f = dt * PC_FPS;
	if (f < 1.0f) f = 1.0f;
	if (f > 15.0f) f = 15.0f;
	u32 c = 0xFF000000u;
	for (int i = 0; i < 3; i++) {
		int v = (int)(powf(kKeep[i], f) * 255.0f + 0.5f);
		if (v > 255) v = 255;
		c |= (u32)v << (8 * i);
	}
	s.keep = c;
}

static void freeMem(void* p, bool vram) {
	if (!p)
		return;
	if (vram)
		vramFree(p);
	else
		linearFree(p);
}

static void freeEye(TrailEye* e) {
	freeMem(e->buf, e->vram);
	freeMem(e->depth, e->depthVram);
	memset(e, 0, sizeof(*e));
}

// The largest block one bank would give now (16 KB steps), for the log.
static u32 largestBlock(vramAllocPos bank) {
	u32 lo = 0, hi = 3u << 20;
	while (hi - lo > 16u << 10) {
		const u32 mid = (lo + hi) / 2;
		void* p = vramAllocAt(mid, bank);
		if (p) {
			vramFree(p);
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return lo;
}

// `width` and `depthFmt` the eye target's: the buffer's depth is a copy of
// that target's, so it is made again, cleared, when either changes.
//
// The buffer once drew on the eye's own depth buffer, bound under its own
// colour buffer mid-frame, and the 3DS's GPU froze on the first world frame
// with it (a tester's log, 2026-11-09: "light trails NOT done after 1000 ms",
// level 3, which turns Light Trails on). Azahar drew it. Every target the
// port has drawn on hardware is laid out as C3D_RenderTargetCreate() lays
// one out -- a colour buffer and a depth buffer of its own, in opposite VRAM
// banks where they fit -- so this one now is too, and trails_begin() copies
// the eye's depth into it (a TextureCopy, as smear_capture() copies colour).
static bool makeEye(int i, u16 width, GPU_DEPTHBUF depthFmt) {
	TrailEye* e = &s.eye[i];
	if (e->width != width || (e->buf && e->depthFmt != depthFmt))
		freeEye(e);
	e->width = width;
	if (e->buf)
		return true;
	if (e->failed)
		return false;
	const u32 bytes = (u32)width * PANEL_H * 4;
	const u32 dbytes = C3D_CalcDepthBufSize(width, PANEL_H, depthFmt);
	// VRAM only, as citro3d's targets: no target with its buffers in linear
	// memory has been drawn on on hardware, so none is risked here.
	// Every placement of the pair, opposite banks first (citro3d's
	// preference): the free VRAM can be in pieces, and the first colour
	// placement can leave no room for the depth where a second would.
	static const vramAllocPos pairs[4][2] = {
		{VRAM_ALLOC_A, VRAM_ALLOC_B}, {VRAM_ALLOC_B, VRAM_ALLOC_A},
		{VRAM_ALLOC_A, VRAM_ALLOC_A}, {VRAM_ALLOC_B, VRAM_ALLOC_B},
	};
	for (int k = 0; k < 4 && !e->depth; k++) {
		e->buf = vramAllocAt(bytes, pairs[k][0]);
		if (!e->buf)
			continue;
		e->depth = vramAllocAt(dbytes, pairs[k][1]);
		if (!e->depth) {
			vramFree(e->buf);
			e->buf = NULL;
		}
	}
	e->vram = e->buf != NULL;
	e->depthVram = e->depth != NULL;
	char b[200];
	if (!e->buf || !e->depth) {
		freeEye(e);
		e->failed = true;
		e->width = width;   // not tried again until the width changes
		snprintf(b, sizeof(b), "trails: no VRAM for eye %d's %ux400 buffer (%u + %u KB, %lu KB "
			"left, largest A %lu KB B %lu KB) -- no light trails there", i, (unsigned)width,
			(unsigned)(bytes >> 10), (unsigned)(dbytes >> 10),
			(unsigned long)(vramSpaceFree() >> 10), (unsigned long)(largestBlock(VRAM_ALLOC_A) >> 10),
			(unsigned long)(largestBlock(VRAM_ALLOC_B) >> 10));
		say(b);
		return false;
	}
	e->depthFmt = depthFmt;
	e->width = width;
	memset(&e->target, 0, sizeof(e->target));
	C3D_FrameBufAttrib(&e->target.frameBuf, width, PANEL_H, false);
	C3D_FrameBufColor(&e->target.frameBuf, e->buf, GPU_RB_RGBA8);
	C3D_FrameBufDepth(&e->target.frameBuf, e->depth, depthFmt);
	e->drawn = 0;
	snprintf(b, sizeof(b), "trails: eye %d's %ux400 light trail buffer, colour %u KB %s, its own "
		"depth %u KB %s (%lu KB VRAM left)", i, (unsigned)width, (unsigned)(bytes >> 10),
		e->vram ? "VRAM" : "linear", (unsigned)(dbytes >> 10), e->depthVram ? "VRAM" : "linear",
		(unsigned long)(vramSpaceFree() >> 10));
	say(b);
	return true;
}

void trails_world_buffer(bool want, u16 width, GPU_DEPTHBUF depthFmt) {
	if (!want)
		return;
	s.eye[0].failed = false;
	makeEye(0, width, depthFmt);
}

// The eye's depth so far into the buffer's own, a tile row at a time
// (smear.c's capture()), queued after the commands so far.
static void copyDepth(const C3D_RenderTarget* eyeTarget, TrailEye* e) {
	const u32 dbytes = C3D_CalcDepthBufSize(e->width, PANEL_H, e->depthFmt);
	const u32 line = dbytes / (PANEL_H / 8);
	// #481: the copy submits the draws so far itself, unflushed; on the 3DS
	// a running queue then reads the list before the cache has written it.
	C3D_FrameSplit(GX_CMDLIST_FLUSH);
	C3D_SyncTextureCopy((u32*)eyeTarget->frameBuf.depthBuf, GX_BUFFER_DIM(line >> 4, 0),
		(u32*)e->depth, GX_BUFFER_DIM(line >> 4, 0), dbytes, GX_TRANSFER_RAW_COPY(1));
}

// The quad's program, vertices and screen-to-texture map bound, culling,
// the alpha test and the depth test off.
static void bindQuad(void) {
	C3D_BindProgram(&s.prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s.quad, sizeof(TrailVert), 2, 0x10);
	// The screen's place in the capture: its 240 columns of 256 and its 400
	// rows of 512 at the top (smear.c's map).
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s.uMap0, (float)PANEL_W / TEX_W, (float)PANEL_H / TEX_H,
		0.0f, 1.0f - (float)PANEL_H / TEX_H);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	model3d_post_pass_prime();   // #486: no fog on these quads, on hardware too
}

// The world's passes' state again, as smear_draw() leaves it.
static void restore(void) {
	for (int i = 0; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

bool trails_begin(C3D_RenderTarget* eyeTarget, int eye) {
	if (!s.on || eye < 0 || eye >= TRAIL_EYES || !eyeTarget || !eyeTarget->frameBuf.depthBuf)
		return false;
	// The composite needs the smear's capture; without it the flares stay
	// on the screen.
	if (!init() || !smear_texture() ||
			!makeEye(eye, eyeTarget->frameBuf.width, eyeTarget->frameBuf.depthFmt))
		return false;
	TrailEye* e = &s.eye[eye];
	copyDepth(eyeTarget, e);
	const bool fresh = e->drawn + 1 != s.frame;
	e->drawn = s.frame;
	C3D_FrameDrawOn(&e->target);

	// The fade: dst *= keep (ZERO / BLENDFACTOR; here the quad's colour as
	// SRC_COLOR), alpha left. Fresh, keep 0: cleared.
	bindQuad();
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvColor(env, fresh ? 0xFF000000u : s.keep);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_SRC_COLOR, GPU_ZERO, GPU_ONE);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	restore();
	return true;
}

// The buffer added over the eye, ONE / ONE, its alpha left; `eyeTarget`
// bound after.
static void composite(C3D_RenderTarget* eyeTarget, TrailEye* e) {
	const C3D_Tex* tex = e->buf ? smear_capture(&e->target, false) : NULL;
	C3D_FrameDrawOn(eyeTarget);
	if (!tex)
		return;
	bindQuad();
	C3D_ProcTexBind(0, NULL);
	C3D_TexBind(0, (C3D_Tex*)tex);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	restore();
}

void trails_end(C3D_RenderTarget* eyeTarget, int eye) {
	if (eye < 0 || eye >= TRAIL_EYES || !eyeTarget)
		return;
	if (s.post) {
		// Over Bright: the buffer waits for trails_post().
		C3D_FrameDrawOn(eyeTarget);
		return;
	}
	composite(eyeTarget, &s.eye[eye]);
}

// ---- Over Bright -------------------------------------------------------

static bool makeGlow(void) {
	if (s.glow[1].buf)
		return true;
	if (s.glowFailed)
		return false;
	const u32 bytes = GLOW_BUF_W * GLOW_BUF_H * 4;
	for (int i = 0; i < 2; i++) {
		GlowBuf* g = &s.glow[i];
		g->buf = vramAlloc(bytes);
		g->vram = g->buf != NULL;   // VRAM only, as the trail buffers
		if (!g->buf) {
			s.glowFailed = true;
			say("trails: no memory for Over Bright's glow -- the trails go on without it");
			return false;
		}
		memset(&g->target, 0, sizeof(g->target));
		C3D_FrameBufAttrib(&g->target.frameBuf, GLOW_BUF_W, GLOW_BUF_H, false);
		C3D_FrameBufColor(&g->target.frameBuf, g->buf, GPU_RB_RGBA8);
		memset(&g->tex, 0, sizeof(g->tex));
		g->tex.data = g->buf;
		g->tex.fmt = GPU_RGBA8;
		g->tex.size = GLOW_BUF_W * GLOW_TEX_H * 4;
		g->tex.width = GLOW_BUF_W;
		g->tex.height = GLOW_TEX_H;
		g->tex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
		C3D_TexSetFilter(&g->tex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&g->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	s.glowCleared = false;
	char b[140];
	snprintf(b, sizeof(b), "trails: Over Bright's glow, 2 x %u KB %s (%lu KB VRAM left)",
		(unsigned)(bytes >> 10), s.glow[0].vram && s.glow[1].vram ? "VRAM" : "partly linear memory",
		(unsigned long)(vramSpaceFree() >> 10));
	say(b);
	return true;
}

// Unit `unit` reads through `m`, moved by (du, dv): the filter's tap.
static void unitMap(int unit, const float m[4], float du, float dv) {
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s.uMap[unit], m[0], m[1], m[2] + du, m[3] + dv);
}

// The frame's place in the capture (smear.c's map) and the glow's in its
// buffer: the first 240 or 120 columns and the first 400 or 200 rows.
static const float kSceneMap[4] = {
	(float)PANEL_W / TEX_W, (float)PANEL_H / TEX_H, 0.0f, 1.0f - (float)PANEL_H / TEX_H,
};
static const float kGlowMap[4] = {
	(float)GLOW_W / GLOW_BUF_W, (float)GLOW_H / GLOW_TEX_H, 0.0f, 1.0f - (float)GLOW_H / GLOW_TEX_H,
};

// TEV stage `id`: `func` over (a, b, c) for the colour, its constant the grey
// `grey`/255, the alpha an opaque constant (fe_post.c's).
static C3D_TexEnv* stage(int id, GPU_COMBINEFUNC func, GPU_TEVSRC a, GPU_TEVSRC b,
	GPU_TEVSRC c, u8 grey) {
	C3D_TexEnv* env = C3D_GetTexEnv(id);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, a, b, c);
	C3D_TexEnvFunc(env, C3D_RGB, func);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, 0xFF000000u | grey * 0x010101u);
	return env;
}

// The quad, no blending, every stage passing the previous one through.
static void passSetup(void) {
	bindQuad();
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_ProcTexBind(0, NULL);
	for (int i = 0; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
}

// Glow buffer `i` bound, drawing into the glow's 120x200 (a viewport counts
// its y from the buffer's last row).
static void glowOn(int i) {
	C3D_FrameDrawOn(&s.glow[i].target);
	C3D_SetViewport(0, GLOW_BUF_H - GLOW_H, GLOW_W, GLOW_H);
}

// One blur pass along the panel's 400 (the textures' t) or its 240: -3, -1
// and +1 in one draw, +3 added over them, in the PC's texels.
static void blur(int dst, int src, bool along400) {
	const float k = (float)GLOW_H / PC_GLOW_W;
	const float du = along400 ? 0.0f : k / GLOW_BUF_W;
	const float dv = along400 ? k / GLOW_TEX_H : 0.0f;
	glowOn(dst);
	passSetup();
	for (int i = 0; i < 3; i++)
		C3D_TexBind(i, &s.glow[src].tex);
	unitMap(0, kGlowMap, -3.0f * du, -3.0f * dv);
	unitMap(1, kGlowMap, -du, -dv);
	unitMap(2, kGlowMap, du, dv);
	stage(0, GPU_MODULATE, GPU_TEXTURE1, GPU_CONSTANT, GPU_PRIMARY_COLOR, BLUR_1);
	stage(1, GPU_MULTIPLY_ADD, GPU_TEXTURE2, GPU_CONSTANT, GPU_PREVIOUS, BLUR_1);
	stage(2, GPU_MULTIPLY_ADD, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS, BLUR_3A);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	unitMap(0, kGlowMap, 3.0f * du, 3.0f * dv);
	stage(0, GPU_MODULATE, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR, BLUR_3B);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
}

// The frame's glow into the eye's buffer (see the top): `scene` is the
// eye's frame as a texture.
// Once, all of each buffer: the passes write only the glow's 120x200, and
// the blur and the bilinear reads take the black past it.
static void clearGlow(void) {
	if (s.glowCleared)
		return;
	for (int i = 0; i < 2; i++) {
		C3D_FrameDrawOn(&s.glow[i].target);
		passSetup();
		stage(0, GPU_REPLACE, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT, 0);
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	}
	s.glowCleared = true;
}

static void overBright(TrailEye* e, const C3D_Tex* scene) {
	clearGlow();
	// A: the overbright ps. Stage 0 a, kept in the combiner buffer; stages
	// 1-3 the halved l; 4 a l / 2; 5 x4 its square, which is (a l)^2.
	glowOn(0);
	passSetup();
	unitMap(0, kSceneMap, 0.0f, 0.0f);
	C3D_TexBind(0, (C3D_Tex*)scene);
	C3D_TexEnvScale(stage(0, GPU_SUBTRACT, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR, OB_OFF),
		C3D_RGB, GPU_TEVSCALE_4);
	C3D_TexEnvBufUpdate(C3D_RGB, 1);
	C3D_TexEnvOpRgb(stage(1, GPU_MODULATE, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR, OB_GREY),
		GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvOpRgb(stage(2, GPU_MULTIPLY_ADD, GPU_PREVIOUS_BUFFER, GPU_CONSTANT, GPU_PREVIOUS,
		OB_GREY), GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvOpRgb(stage(3, GPU_MULTIPLY_ADD, GPU_PREVIOUS_BUFFER, GPU_CONSTANT, GPU_PREVIOUS,
		OB_GREY), GPU_TEVOP_RGB_SRC_B, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	stage(4, GPU_MODULATE, GPU_PREVIOUS_BUFFER, GPU_PREVIOUS, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvScale(stage(5, GPU_MODULATE, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PRIMARY_COLOR, 0),
		C3D_RGB, GPU_TEVSCALE_4);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	C3D_TexEnvBufUpdate(C3D_Both, 0);
	for (int i = 0; i < 2; i++) {
		blur(1, 0, true);
		blur(0, 1, false);
	}
	// A -= C x 0xb0, into glow 1.
	glowOn(1);
	passSetup();
	unitMap(0, kSceneMap, 0.0f, 0.0f);
	unitMap(1, kGlowMap, 0.0f, 0.0f);
	C3D_TexBind(0, (C3D_Tex*)scene);
	C3D_TexBind(1, &s.glow[0].tex);
	stage(0, GPU_MODULATE, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR, OB_SUB);
	stage(1, GPU_SUBTRACT, GPU_TEXTURE1, GPU_PREVIOUS, GPU_PRIMARY_COLOR, 0);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	// The buffer += A x [0x7ff114], ONE / ONE, over all of it.
	C3D_FrameDrawOn(&e->target);
	passSetup();
	unitMap(0, kGlowMap, 0.0f, 0.0f);
	C3D_TexBind(0, &s.glow[1].tex);
	stage(0, GPU_MODULATE, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR, OB_ADD);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	// Units 1 and 2 still hold the glow; the world's passes expect them off.
	model3d_tex_unit_off(1, &s.glow[0].tex);
	model3d_tex_unit_off(2, &s.glow[1].tex);
	restore();
}

bool trails_post(C3D_RenderTarget* eyeTarget, int eye) {
	if (!s.post || eye < 0 || eye >= TRAIL_EYES || !eyeTarget)
		return false;
	TrailEye* e = &s.eye[eye];
	if (!e->buf || e->drawn != s.frame)
		return false;
	if (makeGlow()) {
		const C3D_Tex* scene = smear_capture(eyeTarget, false);
		if (scene)
			overBright(e, scene);
	}
	composite(eyeTarget, e);
	return true;
}

// ---- Depth of Field ------------------------------------------------------
//
// g_DepthOfFieldEnable (DAT_00870cf0, the display block's +0x40), which
// counts (DAT_008707cc) only while the view's camera has a focus: camera
// +0xB4 (the middle of the sharp span, metres) or +0xB8 (its length) not 0.
// Only an NIS camera's keys ever open the aperture (nis_dof()); the garage's,
// the photo shoot's and the magazines' records all leave both at 0.
//
// What the PC does: every world shader writes the view depth times
// FocalRange (1/1024) into the frame's alpha; FUN_005cc8b0 copies the frame
// and boxes it down to half the screen, FUN_005cc9a0 blurs that as Over
// Bright's blur does (twice along x then y, 1/3 at +-1 texel and 1/6 at
// +-3), and FUN_005cccd0 lays the blurred copy over the frame through the
// DOF ps (FUN_005c6be0 technique 5), per pixel:
//
//   w = sat08(a F + O1) + sat08(O2 - a F)     sat08: clamped to 0..0.8
//   F = 51.2 / span, O1 = -(focus / 1024 + span / 2048) F,
//   O2 = (focus / 1024 - span / 2048) F
//   out = lerp(frame, blurred, w)
//
// a the alpha, so with z the pixel's depth: w ramps up 0.05 / span a metre
// past the far end of the sharp span (focus + span / 2) and short of its near
// end (focus - span / 2), to 0.8 at most.
//
// PORT: no world shader here writes its depth anywhere a pass can read it,
// but the depth buffer holds it: the blurred copy goes over the frame in
// bands, a quad for each 0.05 of w, at the depth where w crosses halfway to
// it (C3D_DepthMap's zero scale puts a quad at any depth) and depth tested
// against the frame -- GREATER for the far side (every pixel behind it),
// LESS for the near -- each quad's alpha what takes the pixels it covers
// from the weight before to its own. The weight is so within 0.025 of the
// PC's; the PC's own steps 4 m (its 8-bit alpha) of depth at a time.
#define DOF_LEVELS 16
#define DOF_MAX    0.8f

bool trails_dof(C3D_RenderTarget* eyeTarget, float focus, float span, float nearZ,
		float farZ, float (*depthAt)(float z)) {
	if (span <= 0.0f || !depthAt || !eyeTarget || !eyeTarget->frameBuf.depthBuf)
		return false;
	if (!init() || !smear_texture() || !makeGlow())
		return false;
	const C3D_Tex* scene = smear_capture(eyeTarget, false);
	if (!scene)
		return false;
	clearGlow();
	// The frame boxed down: each glow texel's one bilinear tap falls between
	// four of the frame's.
	glowOn(0);
	passSetup();
	unitMap(0, kSceneMap, 0.0f, 0.0f);
	C3D_TexBind(0, (C3D_Tex*)scene);
	stage(0, GPU_REPLACE, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, 0);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	for (int i = 0; i < 2; i++) {
		blur(1, 0, true);
		blur(0, 1, false);
	}
	// The bands over the eye, on its depth.
	C3D_FrameDrawOn(eyeTarget);
	passSetup();
	unitMap(0, kGlowMap, 0.0f, 0.0f);
	C3D_TexBind(0, &s.glow[0].tex);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_ZERO, GPU_ONE);
	const float perM = 0.05f / span;
	int bands = 0;
	for (int side = 0; side < 2; side++) {
		const bool far = side == 0;
		C3D_DepthTest(true, far ? GPU_GREATER : GPU_LESS, GPU_WRITE_COLOR);
		float before = 0.0f;
		for (int i = 1; i <= DOF_LEVELS; i++) {
			const float w = DOF_MAX * (float)i / DOF_LEVELS;
			const float past = (w - DOF_MAX * 0.5f / DOF_LEVELS) / perM;
			float z = far ? focus + span * 0.5f + past : focus - span * 0.5f - past;
			if (!far && z <= nearZ)
				break;
			if (far && z > farZ)
				z = farZ;   // only the sky (cleared to 0) is behind it
			const float a = 1.0f - (1.0f - w) / (1.0f - before);
			before = w;
			int ai = (int)(a * 255.0f + 0.5f);
			if (ai < 1)
				continue;
			C3D_TexEnvColor(env, (u32)(ai > 255 ? 255 : ai) << 24);
			C3D_DepthMap(true, 0.0f, depthAt(z));
			C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
			bands++;
		}
	}
	C3D_DepthMap(true, -1.0f, 0.0f);
	// Units 1 and 2 still hold the blur's last source; unit 0 the glow.
	model3d_tex_unit_off(1, &s.glow[1].tex);
	model3d_tex_unit_off(2, &s.glow[1].tex);
	restore();
	return bands > 0;
}

void trails_release(void) {
	for (int i = 0; i < TRAIL_EYES; i++)
		freeEye(&s.eye[i]);
	for (int i = 0; i < 2; i++) {
		GlowBuf* g = &s.glow[i];
		if (g->buf) {
			if (g->vram)
				vramFree(g->buf);
			else
				linearFree(g->buf);
		}
		memset(g, 0, sizeof(*g));
	}
	s.glowFailed = false;
	s.glowCleared = false;
}
