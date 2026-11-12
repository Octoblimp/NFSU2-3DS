// The frontend's top screen as SPEED2.EXE presents it (TASKS.md #312). See
// fe_post.h.
//
// The scene target is 256 or 512 wide where the screen needs 240 or 480: the
// PICA200's colour buffer is laid out in 8x8 tiles row by row, the same as a
// texture's, so a buffer whose width is a power of two is also a texture --
// 512x1024 for the biggest, with only the first 800 rows ever written or
// read. The panel's own target cannot be: 240 is not a power of two. So the
// scene is drawn here, read back by the filters and the box-down pass, and
// the panel only takes the result. The display transfer used to do the box
// down (DISPLAY_TRANSFER_FLAGS_SS, #312's first cut), but that leaves no
// texture to filter.
//
// How big (#319): #312 drew the scene 2x2, 480x800, and on the card the
// frontend fell from 60 FPS to about 20 (UI_Main 45.4 ms of GPU a frame
// against 14 before). fepost_mode() picks the scene's scale on each axis
// and the glow's size; the buffer is made for the largest the caller will
// ask for, and a smaller scene fills its first rows and columns.
//
// Orientation, which a render target and a texture agree on: clip-space +y is
// the buffer's first row, which a texture reads as t = 1 (envmap.c reads its
// faces the same way). A scene of 400 sy rows in a texture of 512 maxSy is
// therefore t = 1 - 400 sy / (512 maxSy) .. 1, and its 240 sx columns
// s = 0 .. 240 sx / (256 maxSx). Each texture unit has its own map from the
// quad's 0..1 to its source (post.v.pica), so the scene and the glow need not
// sit alike in their buffers.
//
// The filters are the PC's overbright glow as its tier 3 runs it in the
// frontend (0x5cca00), as the PC runs it at half its screen's resolution:
//
//   h    the scene, boxed down (the PC point-samples its back buffer)
//   O    (a * 0.588 (a.r + a.g + a.b))^2, a = sat(4 (h - 0.2529)): the
//        overbright ps (0x4407d0) at the frontend's settled threshold,
//        [0x7ff10c] = 129, which the ps halves
//   D    O blurred along x then y, twice ([0x7ff110]): 1/3 at +-1 texel and
//        1/6 at +-3, the centre left out (0x7ff1cc, 0x7ff19c)
//   out  h + 0.3137 max(0, D - 0.6275 h): the blend factors 0x50 and 0xa0 the
//        frontend sets for the add into the glow buffer and for the reverse
//        subtract before it
//
// The glow is 240x400, the panel's own size, or 120x200 (glowDiv 2), the
// shipped one (#319: on the card the 240x400 glow was 6.4 ms of GPU a frame,
// the 120x200 1.6). The PC's half of 640x480 is 320x240, so its blur's taps
// are 1/320 of its screen's width apart: the camera's angle is the horizontal
// one, as on the PC, so the taps here are spaced to that same part of the
// panel's 400 -- 0.625 of a texel of the 200, 1.25 of the 400 -- and land
// between texels, where the filter takes both.
//
// That glow buffer ([0x86ff70]) is not cleared between frames. 0x5cbb00
// scales it by the BLENDFACTOR 0xff785aaa -- it keeps 120, 90 and 170 /255 of
// its red, green and blue -- then draws the car's neon (RenderNeon 0x60d7f0)
// and flares (0x60c8a0) into it, and the add above goes in after them; the
// back buffer then takes the whole of it, ONE/ONE (0x5ccba5). Held still,
// what goes in each frame settles at 1 / (1 - kept): 1.89, 1.55 and 3 times
// itself, which is where the PC's violet tube and orange disc come from.
// PORT: the settled buffer, not the buffer -- the lights drawn into the
// scene at that gain, after the glow has read it, and the add at 0.3137 of
// it. Kept, the buffer would be the scene's size (the lights test depth) and
// the VRAM is not there; what it loses is the trail the lights leave for a
// few frames as the car turns.
//
// Tier 4 grades the scene first (the bleach screen filter, 0x5c68d0), which
// takes its blue from its green and turns the garage's yellow disc grey; the
// PC shot of #312 has the yellow, so the tier 3 chain is the one here.
//
// The glow targets are 256x408: a texture's width, and the 400 rows the glow
// fills plus 8 left black, which the blur reads past the screen's far edge.
// Their texture view is 256x512, laid out like the scene's. A viewport counts
// its y from the buffer's last row, so the glow's 400 / glowDiv rows are
// y = 408 - 400 / glowDiv .. 408.
//
// The clears are the memory fill's (C3D_RenderTargetClear, as
// C2D_TargetClear does it), not a quad drawn over the buffer: #312 drew one
// over all of the 2x2 buffer, 409,600 pixels of colour and depth a frame on
// the GPU, before the room.
#include "fe_post.h"

#include <stdio.h>
#include <string.h>

#include "model3d.h"
#include "shaders.h"

#define PANEL_W  240    // the top screen's short axis, the buffer's x
#define PANEL_H  400    // its long axis

#define GL_BUF_H 408    // the glow targets' rows: its 400 and 8 black ones
#define PC_GLOW_W 320   // the PC's glow across its screen, half of 640
#define GL_TEX_W 256
#define GL_TEX_H 512

// SPEED2.EXE's constants, as the TEV's 8-bit greys.
#define OB_OFF    65    // the overbright's threshold, 129 halved
#define OB_GREY   75    // its GreyScale, 150 halved: the sum reaches 1.76,
                        // and a stage clamps at 1, so the product is doubled
                        // back by the square's x4
#define BLUR_1    85    // 1/3, at -1 and +1
#define BLUR_3A   43    // 1/6, at -3 ...
#define BLUR_3B   42    // ... and +3: the four make 255
#define GLOW_SUB 160    // BLENDFACTOR 0xa0, the reverse subtract's
#define GLOW_ADD  80    // BLENDFACTOR 0x50, the add's ([0x7875b8] = 80.0f)

// What the glow buffer keeps of itself each frame, BLENDFACTOR 0xff785aaa,
// and what that makes of what goes in.
#define KEEP_R 0x78
#define KEEP_G 0x5a
#define KEEP_B 0xaa
static const float kSettled[3] = {
	255.0f / (255 - KEEP_R), 255.0f / (255 - KEEP_G), 255.0f / (255 - KEEP_B),
};

typedef struct {
	float x, y;   // clip space
	float u, v;   // 0..1 across the target
} PostVert;

static struct {
	bool tried, ready;
	int maxSx, maxSy;        // what the scene buffer was made for
	int sx, sy, glowDiv;     // fepost_mode()'s, for the next frame
	int fsx, fsy, fglow;     // this frame's, from fepost_begin()
	C3D_RenderTarget* scene;
	C3D_Tex sceneTex;   // a view of scene's colour buffer; owns nothing
	bool glowReady, glowRan;
	int glowCleared;    // the glowDiv the glow buffers were last cleared for, 0 none
	C3D_RenderTarget* glow[2];
	C3D_Tex glowTex[2];
	DVLE_s* dvle;   // the shared blob's (shaders.h, #352)
	bool progReady; // prog and quad made (kept over fepost_release())
	shaderProgram_s prog;
	int uMap[3];
	PostVert* quad;
} s;

static void say(void (*log)(const char*), const char* what) {
	if (!log)
		return;
	char b[200];
	snprintf(b, sizeof(b), "fe_post: %s", what);
	log(b);
}

// The texture over a target's colour buffer: what C3D_TexInitVRAM() would
// set, without the allocation.
static void texOver(C3D_Tex* t, C3D_RenderTarget* rt, u16 w, u16 h,
	GPU_TEXTURE_FILTER_PARAM filter) {
	memset(t, 0, sizeof(*t));
	t->data = rt->frameBuf.colorBuf;
	t->fmt = GPU_RGBA8;
	t->size = (u32)w * h * 4;
	t->width = w;
	t->height = h;
	t->param = GPU_TEXTURE_MODE(GPU_TEX_2D);
	C3D_TexSetFilter(t, filter, filter);
	C3D_TexSetWrap(t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
}

static int clampScale(int v, int max) {
	return v < 1 ? 1 : v > max ? max : v;
}

bool fepost_init(void (*log)(const char*), int maxSx, int maxSy) {
	if (s.tried)
		return s.ready;
	s.tried = true;
	s.maxSx = clampScale(maxSx, 2);
	s.maxSy = clampScale(maxSy, 2);
	s.sx = s.fsx = 1;
	s.sy = s.fsy = 1;
	s.glowDiv = s.fglow = 1;
	const u32 vram0 = vramSpaceFree();
	const u16 bufW = (u16)(256 * s.maxSx), bufH = (u16)(PANEL_H * s.maxSy);
	s.scene = C3D_RenderTargetCreate(bufW, bufH, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (!s.scene) {
		say(log, "no VRAM for the scene -- the frontend draws straight onto the panel, unfiltered");
		return false;
	}
	texOver(&s.sceneTex, s.scene, bufW, (u16)(512 * s.maxSy), GPU_LINEAR);
	// No depth: no pass tests any. Linear, so a 120x200 glow is spread over
	// the panel smoothly, and the blur's taps, which fall between texels, take
	// the two either side of them.
	s.glow[0] = C3D_RenderTargetCreate(GL_TEX_W, GL_BUF_H, GPU_RB_RGBA8, -1);
	s.glow[1] = s.glow[0] ? C3D_RenderTargetCreate(GL_TEX_W, GL_BUF_H, GPU_RB_RGBA8, -1) : NULL;
	s.glowReady = s.glow[1] != NULL;
	if (s.glowReady) {
		for (int i = 0; i < 2; i++)
			texOver(&s.glowTex[i], s.glow[i], GL_TEX_W, GL_TEX_H, GPU_LINEAR);
	} else
		say(log, "no VRAM for the glow -- the frontend's top screen goes unfiltered");

	if (!s.progReady) {
		s.dvle = shaders_get(SHADER_POST);
		if (!s.dvle) {
			say(log, "shader would not load -- the frontend draws straight onto the panel, unfiltered");
			return false;
		}
		shaderProgramInit(&s.prog);
		shaderProgramSetVsh(&s.prog, s.dvle);
		s.uMap[0] = shaderInstanceGetUniformLocation(s.prog.vertexShader, "map0");
		s.uMap[1] = shaderInstanceGetUniformLocation(s.prog.vertexShader, "map1");
		s.uMap[2] = shaderInstanceGetUniformLocation(s.prog.vertexShader, "map2");

		s.quad = (PostVert*)linearAlloc(sizeof(PostVert) * 4);
		if (!s.quad) {
			say(log, "no linear memory for the quad -- the frontend draws straight onto the panel, unfiltered");
			return false;
		}
		static const PostVert kQuad[4] = {
			{ -1.0f, -1.0f, 0.0f, 0.0f }, { 1.0f, -1.0f, 1.0f, 0.0f },
			{ -1.0f,  1.0f, 0.0f, 1.0f }, { 1.0f,  1.0f, 1.0f, 1.0f },
		};
		memcpy(s.quad, kQuad, sizeof(kQuad));
		GSPGPU_FlushDataCache(s.quad, sizeof(kQuad));
		s.progReady = true;
	}

	s.ready = true;
	if (log) {
		char b[200];
		snprintf(b, sizeof(b), "fe_post: the frontend's top screen in a target of its own, "
			"up to %dx%d the panel%s: %lu KB VRAM (%lu KB left)", s.maxSx, s.maxSy,
			s.glowReady ? ", and filtered" : "",
			(unsigned long)((vram0 - vramSpaceFree()) >> 10),
			(unsigned long)(vramSpaceFree() >> 10));
		log(b);
	}
	return true;
}

void fepost_release(void (*log)(const char*)) {
	s.tried = false;
	s.ready = false;
	if (!s.scene && !s.glow[0])
		return;
	const u32 vram0 = vramSpaceFree();
	for (int i = 1; i >= 0; i--) {
		if (s.glow[i])
			C3D_RenderTargetDelete(s.glow[i]);
		s.glow[i] = NULL;
	}
	if (s.scene)
		C3D_RenderTargetDelete(s.scene);
	s.scene = NULL;
	s.glowReady = s.glowRan = false;
	s.glowCleared = 0;
	if (log) {
		char b[160];
		snprintf(b, sizeof(b), "fe_post: targets released for the world (#352): %lu KB VRAM back "
			"(%lu KB free)", (unsigned long)((vramSpaceFree() - vram0) >> 10),
			(unsigned long)(vramSpaceFree() >> 10));
		log(b);
	}
}

void fepost_mode(int sx, int sy, int glowDiv) {
	s.sx = clampScale(sx, s.maxSx);
	s.sy = clampScale(sy, s.maxSy);
	s.glowDiv = clampScale(glowDiv, 2);
}

// The maps, as post.v.pica takes them: (scale s, scale t, offset s, offset t)
// from the quad's 0..1 to where the source covers the panel.
static void sceneMap(float m[4]) {
	const float tw = 256.0f * s.maxSx, th = 512.0f * s.maxSy;
	m[0] = PANEL_W * s.fsx / tw;
	m[1] = PANEL_H * s.fsy / th;
	m[2] = 0.0f;
	m[3] = 1.0f - m[1];
}

static void glowMap(float m[4]) {
	m[0] = (float)(PANEL_W / s.fglow) / GL_TEX_W;
	m[1] = (float)(PANEL_H / s.fglow) / GL_TEX_H;
	m[2] = 0.0f;
	m[3] = 1.0f - m[1];
}

// Texture unit `unit` reads through `m`, moved by (du, dv) in the 0..1 of its
// texture: the filter's tap.
static void unitMap(int unit, const float m[4], float du, float dv) {
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s.uMap[unit], m[0], m[1], m[2] + du, m[3] + dv);
}

// Everything a pass draws with: the quad, the shader, no culling, no alpha
// test, depth through C3D_DepthMap's zero scale (0 whatever the vertex's z),
// no blending, and stages 1..5 passing the previous one through. The units'
// maps are the pass's own.
static void passSetup(void) {
	C3D_BindProgram(&s.prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s.quad, sizeof(PostVert), 2, 0x10);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthMap(true, 0.0f, 0.0f);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_ProcTexBind(0, NULL);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
}

// TEV stage `id`: `func` over (a, b, c) for the colour, its constant the grey
// `grey`/255, the alpha an opaque constant.
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

static void passDraw(void) {
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
}

// citro3d's own depth map (C3D_Init's) and the 3D's test, as envmap.c puts
// them back, no stage writing the combiner buffer, and C3D_Init's alpha
// blend: C2D_Prepare() does not set one, so the passes' replace would draw
// every 2D quad after them solid -- text as bars -- until something else
// set it (#319: the GPU sweep's label, with the menu off).
static void passDone(void) {
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_TexEnvBufUpdate(C3D_Both, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
		GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
}

// The memory fill, after the GPU's commands so far: what C2D_TargetClear()
// does.
static void fill(C3D_RenderTarget* t, C3D_ClearBits bits) {
	C3D_FrameSplit(GX_CMDLIST_FLUSH);
	C3D_RenderTargetClear(t, bits, 0, 0);
}

// Binds glow target `i` with the viewport of this frame's glow.
static void glowOn(int i) {
	C3D_FrameDrawOn(s.glow[i]);
	const int w = PANEL_W / s.fglow, h = PANEL_H / s.fglow;
	C3D_SetViewport(0, GL_BUF_H - h, w, h);
}

// O, into glow 0, from the scene boxed down (unit 0's bilinear at each block's
// shared corner where the scene has two texels a glow texel). Stage 0 is a,
// kept in the combiner buffer; stages 1-3 the halved l, 0.294 (a.r + a.g +
// a.b); stage 4 m = a times that; stage 5 4 m^2, which is (a l)^2.
static void overbright(void) {
	float sm[4];
	sceneMap(sm);
	glowOn(0);
	passSetup();
	unitMap(0, sm, 0.0f, 0.0f);
	C3D_TexBind(0, &s.sceneTex);
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
	passDraw();
	passDone();
}

// One blur pass, from glow `src` into glow `dst`, along x (the panel's 400,
// the textures' t) or y. Two draws, there being three texture units: -3, -1
// and +1, then +3 added over them, in the PC's texels (PC_GLOW_W).
static void blur(int dst, int src, bool alongX) {
	const float k = (float)(PANEL_H / s.fglow) / PC_GLOW_W;
	const float du = alongX ? 0.0f : k / GL_TEX_W;
	const float dv = alongX ? k / GL_TEX_H : 0.0f;
	float gm[4];
	glowMap(gm);
	glowOn(dst);
	passSetup();
	for (int i = 0; i < 3; i++)
		C3D_TexBind(i, &s.glowTex[src]);
	unitMap(0, gm, -3.0f * du, -3.0f * dv);
	unitMap(1, gm, -du, -dv);
	unitMap(2, gm, du, dv);
	stage(0, GPU_MODULATE, GPU_TEXTURE1, GPU_CONSTANT, GPU_PRIMARY_COLOR, BLUR_1);
	stage(1, GPU_MULTIPLY_ADD, GPU_TEXTURE2, GPU_CONSTANT, GPU_PREVIOUS, BLUR_1);
	stage(2, GPU_MULTIPLY_ADD, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS, BLUR_3A);
	passDraw();
	unitMap(0, gm, 3.0f * du, 3.0f * dv);
	stage(0, GPU_MODULATE, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR, BLUR_3B);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
	passDraw();
	passDone();
}

void fepost_bind(void) {
	C3D_FrameDrawOn(s.scene);
	C3D_SetViewport(0, PANEL_H * (s.maxSy - s.fsy), PANEL_W * s.fsx, PANEL_H * s.fsy);
}

void fepost_begin(bool clear) {
	s.fsx = s.sx;
	s.fsy = s.sy;
	s.fglow = s.glowDiv;
	// The whole buffer, past the scene's rows and columns too, so a filter
	// tap that strays over the edge reads black.
	if (clear)
		fill(s.scene, C3D_CLEAR_ALL);
	fepost_bind();
}

C3D_RenderTarget* fepost_target(void) {
	return s.ready ? s.scene : NULL;
}

// The glow, from the scene as it stands, into glow 1: O, blurred, less the
// reverse subtract's 0.6275 h. The subtract is here, not in the composite,
// because the lights drawn after it are the PC's glow buffer's, not the back
// buffer the subtract reads. Each glow texel's centre maps to the corner its
// block of the scene shares, where the bilinear filter weighs the block's
// texels alike, or to the scene texel's own centre when there is one.
static void glowPass(void) {
	if (s.glowCleared != s.fglow) {
		// The whole of each, when the glow's size changes: the passes write
		// only the glow's own rows and columns, and the blur reads past them.
		for (int i = 0; i < 2; i++)
			fill(s.glow[i], C3D_CLEAR_COLOR);
		s.glowCleared = s.fglow;
	}
	overbright();
	for (int i = 0; i < 2; i++) {
		blur(1, 0, true);
		blur(0, 1, false);
	}
	float sm[4], gm[4];
	sceneMap(sm);
	glowMap(gm);
	glowOn(1);
	passSetup();
	unitMap(0, sm, 0.0f, 0.0f);
	unitMap(1, gm, 0.0f, 0.0f);
	C3D_TexBind(0, &s.sceneTex);
	C3D_TexBind(1, &s.glowTex[0]);
	stage(0, GPU_MODULATE, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR, GLOW_SUB);
	stage(1, GPU_SUBTRACT, GPU_TEXTURE1, GPU_PREVIOUS, GPU_PRIMARY_COLOR, 0);
	passDraw();
	passDone();
	// Unit 1 this pass's glow, unit 2 the last blur's source.
	model3d_tex_unit_off(1, &s.glowTex[0]);
	model3d_tex_unit_off(2, &s.glowTex[1]);
	s.glowRan = true;
}

const float* fepost_glow(bool filters) {
	if (!filters || !s.glowReady)
		return NULL;
	glowPass();
	fepost_bind();
	return kSettled;
}

void fepost_end(C3D_RenderTarget* out, bool filters) {
	if (filters && s.glowReady && !s.glowRan)
		glowPass();
	const bool glow = s.glowRan;
	s.glowRan = false;
	// The box down, with the glow added over it at the add's factor times
	// what the buffer settles at. Each panel pixel's centre is its scene
	// block's shared corner (or its one texel's centre at 1x), and the glow
	// is read there too -- between texels when it is the panel's half.
	float sm[4], gm[4];
	sceneMap(sm);
	glowMap(gm);
	C3D_FrameDrawOn(out);
	passSetup();
	C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
	unitMap(0, sm, 0.0f, 0.0f);
	C3D_TexBind(0, &s.sceneTex);
	if (glow) {
		unitMap(1, gm, 0.0f, 0.0f);
		C3D_TexBind(1, &s.glowTex[1]);
		u32 add = 0xFF000000u;
		for (int c = 0; c < 3; c++)
			add |= (u32)(GLOW_ADD * kSettled[c] + 0.5f) << (8 * c);
		C3D_TexEnvColor(stage(0, GPU_MULTIPLY_ADD, GPU_TEXTURE1, GPU_CONSTANT, GPU_TEXTURE0, 0),
			add);
	} else
		stage(0, GPU_REPLACE, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0, 0);
	passDraw();
	passDone();
	if (glow)
		model3d_tex_unit_off(1, &s.glowTex[1]);
}
