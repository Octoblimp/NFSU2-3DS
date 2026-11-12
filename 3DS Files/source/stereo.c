// Stereoscopic 3D on the top screen (TASKS.md #326). See stereo.h.
//
// The right eye shares the left's depth buffer: the left eye's 3D is done
// with it before the right eye's starts, and the 2D after both writes no
// depth (the overlay has none). That saves 375 KB of VRAM; the cost is one
// memory fill of the depth between the eyes, queued with the right eye's
// colour clear.
//
// The fill goes in mid-frame, after world_draw() may have started the GPU on
// the frame (world.c's "GPU kicks"). A running queue starts each entry the
// moment it is queued, so what the left eye drew since the last kick has to
// be out of the CPU's cache first -- citro2d's vertices for the tint, above
// all -- and the list it is in goes with GX_CMDLIST_FLUSH.
//
// The overlay is 256x400, a power of two across so its colour buffer is also
// a texture (fe_post.c's scene buffer is the same), read as 256x512 with only
// the first 400 rows ever touched. citro2d draws into it through the top
// screen's projection and a 240x400 viewport. Cleared to transparent black,
// with citro2d's colour blend and an alpha blend of one over one minus the
// source's alpha, it ends up holding the 2D premultiplied, which is what the
// composite lays over each eye: out = overlay + eye * (1 - overlay alpha), the
// same as drawing the 2D straight onto the eye. No 2D draw in the overlay's
// span sets a blend of its own: the tint's goes to each eye, before it opens,
// and the minimap's to the bottom screen.
//
// Orientation, as fe_post.c has it: clip-space +y is the buffer's first row,
// which a texture reads as t = 1, and the screen's left edge. A shift of p
// pixels to the right is -2p/400 in clip y.

#include "stereo.h"

#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "shaders.h"

#define PANEL_W  240
#define PANEL_H  400
#define OV_W     256    // the overlay's buffer, a power of two across
#define OV_TEX_H 512    // its texture's rows: the 400 and 112 never read

// citro3d's linear heap, for the flush before the fill (world.c does the
// same at each kick).
extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;

typedef struct {
	float x, y;   // clip space
	float u, v;   // 0..1 across the overlay's 240x400
} PostVert;

static struct {
	bool tried, ready;
	C3D_RenderTarget* left;
	C3D_RenderTarget* right;
	C3D_RenderTarget* overlay;
	C3D_Tex overlayTex;     // a view of overlay's colour buffer; owns nothing
	DVLE_s* dvle;   // the shared blob's (shaders.h, #352)
	shaderProgram_s prog;
	int uMap;
	PostVert* quad;         // two quads, the left eye's then the right's
	int quadShift;          // the shift the quads hold, -1 not yet written
	bool mode3d;            // what gfxSet3D() was last given
	bool on;                // this frame's world goes down twice
	bool overlayOn;
	float slider;           // this frame's, 0..1
	float pxInf;            // the far distance's separation, pixels
	float screen;           // the screen plane's distance, metres
	int hudShift;           // each eye's shift of the overlay, pixels
	float dbgSlider;        // DbgStereo, 0 the hardware's
	bool swap;              // DbgStereoSwap
	void (*log)(const char*);
	int streak;             // frames in a row drawn in stereo, for check()
	int checkTick;
	u32 cmdEye;             // the command buffer's words left at the left eye's start
	u32 skipped;            // frames stereo_right_fits() turned flat
} s;

// The GPU command buffer's words left. citro3d writes a whole frame into the
// one buffer: a split moves the buffer's start, never its end.
static u32 cmdLeft(void) {
	u32 size, offset;
	GPUCMD_GetBuffer(NULL, &size, &offset);
	return size > offset ? size - offset : 0;
}

static void say(void (*log)(const char*), const char* what) {
	if (!log)
		return;
	char b[200];
	snprintf(b, sizeof(b), "stereo: %s", what);
	log(b);
}

bool stereo_init(void (*log)(const char*), C3D_RenderTarget* left, u32 transferFlags) {
	if (s.tried)
		return s.ready;
	s.tried = true;
	s.quadShift = -1;
	s.log = log;
	if (!left || !left->frameBuf.depthBuf) {
		say(log, "no top-screen target to pair with -- the top screen stays flat");
		return false;
	}
	s.left = left;
	const u32 vram0 = vramSpaceFree();
	s.right = C3D_RenderTargetCreate(PANEL_W, PANEL_H, GPU_RB_RGBA8, -1);
	if (!s.right) {
		say(log, "no VRAM for the right eye -- the top screen stays flat");
		return false;
	}
	C3D_FrameBufDepth(&s.right->frameBuf, left->frameBuf.depthBuf, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(s.right, GFX_TOP, GFX_RIGHT, transferFlags);
	s.overlay = C3D_RenderTargetCreate(OV_W, PANEL_H, GPU_RB_RGBA8, -1);
	if (!s.overlay) {
		say(log, "no VRAM for the 2D's overlay -- the top screen stays flat");
		return false;
	}
	// Nearest: the composite moves it by whole pixels, texel for pixel.
	memset(&s.overlayTex, 0, sizeof(s.overlayTex));
	s.overlayTex.data = s.overlay->frameBuf.colorBuf;
	s.overlayTex.fmt = GPU_RGBA8;
	s.overlayTex.size = (u32)OV_W * OV_TEX_H * 4;
	s.overlayTex.width = OV_W;
	s.overlayTex.height = OV_TEX_H;
	s.overlayTex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
	C3D_TexSetFilter(&s.overlayTex, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&s.overlayTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

	s.dvle = shaders_get(SHADER_POST);
	if (!s.dvle) {
		say(log, "shader would not load -- the top screen stays flat");
		return false;
	}
	shaderProgramInit(&s.prog);
	shaderProgramSetVsh(&s.prog, s.dvle);
	s.uMap = shaderInstanceGetUniformLocation(s.prog.vertexShader, "map0");
	s.quad = (PostVert*)linearAlloc(sizeof(PostVert) * 8);
	if (!s.quad) {
		say(log, "no linear memory for the overlay's quads -- the top screen stays flat");
		return false;
	}
	s.ready = true;
	if (log) {
		char b[200];
		snprintf(b, sizeof(b), "stereo: the right eye and the 2D's overlay: %lu KB VRAM "
			"(%lu KB left)", (unsigned long)((vram0 - vramSpaceFree()) >> 10),
			(unsigned long)(vramSpaceFree() >> 10));
		log(b);
	}
	return true;
}

// DbgStereo's check, as the screenshots show the left eye only: how far the
// right eye's image of a few parts of the panel sits from the left's, found
// by the shift that matches them best. A frame's both images are complete in
// the framebuffers this frame draws into (two frames back, double-buffered)
// once the frames since were in stereo too. Positive is the right eye's image
// further right: behind the screen.
typedef struct { const char* name; int x0, x1, y0, y1; } CheckBand;

static int lumaAt(const u8* fb, int x, int y) {
	// The top screen's framebuffer: a column of 240 BGR pixels a screen x,
	// the bottom pixel first.
	const u8* p = fb + ((x * PANEL_W) + (PANEL_W - 1 - y)) * 3;
	return p[0] + 2 * p[1] + p[2];
}

static int bestShift(const u8* l, const u8* r, const CheckBand* b, float* ratio) {
	int best = 0;
	float bestSad = 1e30f, sad0 = 0.0f;
	for (int d = -24; d <= 24; d++) {
		u32 sum = 0, n = 0;
		for (int y = b->y0; y < b->y1; y += 2)
			for (int x = b->x0; x < b->x1; x++) {
				const int xr = x + d;
				if (xr < 0 || xr >= PANEL_H)
					continue;
				const int e = lumaAt(l, x, y) - lumaAt(r, xr, y);
				sum += (u32)(e < 0 ? -e : e);
				n++;
			}
		const float sad = n ? (float)sum / (float)n : 1e30f;
		if (d == 0)
			sad0 = sad;
		if (sad < bestSad) {
			bestSad = sad;
			best = d;
		}
	}
	*ratio = sad0 > 0.0f ? bestSad / sad0 : 1.0f;
	return best;
}

static void check(void) {
	u16 w, h;
	u8* l = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
	u8* r = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &w, &h);
	if (!l || !r || l == r || w != PANEL_W || h != PANEL_H)
		return;
	GSPGPU_InvalidateDataCache(l, PANEL_W * PANEL_H * 3);
	GSPGPU_InvalidateDataCache(r, PANEL_W * PANEL_H * 3);
	static const CheckBand bands[] = {
		{ "far", 30, 370, 15, 75 },      // the skyline
		{ "car", 150, 250, 140, 210 },   // the player's car
		{ "road", 10, 140, 215, 239 },   // the street at the bottom edge
		{ "HUD", 310, 399, 160, 239 },   // the speedometer
	};
	char b[240];
	int n = snprintf(b, sizeof(b), "stereo check:");
	for (int i = 0; i < (int)(sizeof(bands) / sizeof(bands[0])) && n < (int)sizeof(b); i++) {
		float ratio;
		const int d = bestShift(l, r, &bands[i], &ratio);
		n += snprintf(b + n, sizeof(b) - (size_t)n, " %s %+d px (%.2f)", bands[i].name, d,
			(double)ratio);
	}
	if (n < (int)sizeof(b))
		snprintf(b + n, sizeof(b) - (size_t)n, "; set: far %+.1f, HUD %+d",
			(double)(s.swap ? -s.pxInf : s.pxInf), s.swap ? 2 * s.hudShift : -2 * s.hudShift);
	s.log(b);
}

void stereo_frame_start(void) {
	s.streak = s.on ? s.streak + 1 : 0;
	if (s.dbgSlider > 0.0f && s.log && s.streak >= 4 && ++s.checkTick % 150 == 0)
		check();
	s.on = false;
	s.overlayOn = false;
	const bool want = s.ready && g_display.stereo != 0;
	if (want != s.mode3d) {
		gfxSet3D(want);
		s.mode3d = want;
	}
}

static float stepOf(u32 v) {
	return (float)(v > DISPLAY_STEREO_STEPS ? DISPLAY_STEREO_STEPS : v);
}

bool stereo_world_begin(bool allowed) {
	s.on = false;
	s.slider = s.dbgSlider > 0.0f ? s.dbgSlider : osGet3DSliderState();
	if (!allowed || !s.ready || !s.mode3d || s.slider <= 0.0f)
		return false;
	s.pxInf = stepOf(g_display.stereoDepth) * STEREO_PX_PER_DEPTH * s.slider;
	s.screen = STEREO_FOCUS_MIN + stepOf(g_display.stereoFocus) * STEREO_FOCUS_STEP;
	float hud = stepOf(g_display.stereoHud) * STEREO_PX_PER_HUD * s.slider;
	if (g_display.stereoStyle == DISPLAY_STEREO_POP) {
		// A point at Into Screen's plane z0 sits pxInf (1 - S/z0) apart. With
		// S = z0 + extra and pxInf scaled by z0 / S the span from z0 to the
		// far distance stays pxInf, and z0 -- the car -- comes out by
		// pxInf (S - z0) / S, which the HUD's separation adds to stay in front.
		const float z0 = s.screen;
		const float S = z0 + STEREO_POP_EXTRA;
		const float pop = s.pxInf * (S - z0) / S;
		s.pxInf *= z0 / S;
		s.screen = S;
		hud += pop;
	}
	s.hudShift = (int)(hud * 0.5f + 0.5f);
	s.cmdEye = cmdLeft();
	s.on = true;
	return true;
}

bool stereo_right_fits(void) {
	if (!s.on)
		return false;
	const u32 left = cmdLeft();
	const u32 eye = s.cmdEye > left ? s.cmdEye - left : 0;
	if (left > eye + eye / 4 + STEREO_CMD_RESERVE)
		return true;
	s.on = false;
	if (s.log && (s.skipped++ % 300) == 0) {
		char b[200];
		snprintf(b, sizeof(b), "stereo: the right eye left out (%lu frames so far): the left eye "
			"wrote %lu KB of GPU commands and %lu KB is left -- the frame is flat",
			(unsigned long)s.skipped, (unsigned long)(eye >> 8), (unsigned long)(left >> 8));
		s.log(b);
	}
	return false;
}

void stereo_eye(int eye, float tanHalfX, float* iod, float* screen) {
	// The separation at depth z is 200 iod (1/screen - 1/z) / tanHalfX
	// pixels across the panel's 400, so pxInf in the far distance.
	const float d = s.on ? s.pxInf * s.screen * tanHalfX / 200.0f : 0.0f;
	*iod = (eye != 0) != s.swap ? d : -d;
	*screen = s.screen > 0.0f ? s.screen : STEREO_FOCUS_MIN;
}

void stereo_right_begin(u32 clearColour) {
	if (!s.on)
		return;
	C2D_Flush();
	GSPGPU_FlushDataCache((const void*)__ctru_linear_heap, __ctru_linear_heap_size);
	C3D_FrameSplit(GX_CMDLIST_FLUSH);
	// C2D_TargetClear()'s, colour and depth: C2D_Color32 is ABGR, the fill
	// RGBA.
	C3D_RenderTargetClear(s.right, C3D_CLEAR_ALL, __builtin_bswap32(clearColour), 0);
	C3D_RenderTargetClear(s.overlay, C3D_CLEAR_COLOR, 0, 0);
	C3D_FrameDrawOn(s.right);
}

C3D_RenderTarget* stereo_right(void) {
	return s.ready ? s.right : NULL;
}

void stereo_overlay_begin(void) {
	s.overlayOn = s.on;
}

bool stereo_scene_begin(C3D_RenderTarget* target) {
	if (!s.overlayOn || target != s.left)
		return false;
	C2D_Flush();
	C3D_FrameDrawOn(s.overlay);
	C2D_SceneSize(PANEL_W, PANEL_H, true);
	C3D_SetViewport(0, 0, PANEL_W, PANEL_H);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
	return true;
}

// The left eye's quad moved `shift` pixels right, the right eye's as far left.
static void writeQuads(int shift) {
	if (shift == s.quadShift)
		return;
	const float dy = 2.0f * (float)shift / PANEL_H;
	for (int e = 0; e < 2; e++) {
		const float off = (e != 0) != s.swap ? dy : -dy;
		PostVert* q = s.quad + e * 4;
		const PostVert k[4] = {
			{ -1.0f, -1.0f + off, 0.0f, 0.0f }, { 1.0f, -1.0f + off, 1.0f, 0.0f },
			{ -1.0f,  1.0f + off, 0.0f, 1.0f }, { 1.0f,  1.0f + off, 1.0f, 1.0f },
		};
		memcpy(q, k, sizeof(k));
	}
	GSPGPU_FlushDataCache(s.quad, sizeof(PostVert) * 8);
	s.quadShift = shift;
}

void stereo_composite(void) {
	if (!s.overlayOn)
		return;
	s.overlayOn = false;
	C2D_Flush();
	writeQuads(s.hudShift);

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
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_ProcTexBind(0, NULL);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_TexBind(0, &s.overlayTex);
	// The 240x400 the 2D drew, at the texture's top rows (fe_post.c's
	// sceneMap()).
	const float ms = (float)PANEL_W / OV_W, mt = (float)PANEL_H / OV_TEX_H;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s.uMap, ms, mt, 0.0f, 1.0f - mt);
	for (int e = 0; e < 2; e++) {
		C3D_FrameDrawOn(e ? s.right : s.left);
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, e * 4, 4);
	}

	// C3D_Init's blend and citro2d's own state, then the 2D's depth test the
	// overlay was opened under -- AFTER C2D_Prepare(), which sets citro2d's
	// GEQUAL, exactly as main.c does before the world's 2D. The bottom screen
	// comes next, and under GEQUAL the minimap's frame (z 0.05) failed against
	// the map (0.1) and its mask (0.15) wherever they had written depth, so
	// only its four flats outside the widget square survived -- a box, with 3D
	// on only (TASKS.md #470).
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
		GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C2D_Prepare();
	C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
}

void stereo_debug(float slider, bool swap) {
	s.dbgSlider = slider > 1.0f ? 1.0f : slider;
	s.swap = swap;
	s.quadShift = -1;
}

void stereo_describe(char* out, int outSize) {
	if (!s.ready) {
		snprintf(out, (size_t)outSize, "stereo unavailable");
		return;
	}
	snprintf(out, (size_t)outSize, "stereo: 3D %s%s %s, slider %.2f%s: far %.1f px, screen plane "
		"%.1f m, HUD %d px an eye (depth %lu, focus %lu, HUD %lu)",
		g_display.stereo ? "on" : "off", s.on ? " and drawn" : "",
		g_display.stereoStyle == DISPLAY_STEREO_POP ? "pop-out" : "into-screen", (double)s.slider,
		s.dbgSlider > 0.0f ? " (DbgStereo)" : "", (double)s.pxInf, (double)s.screen, s.hudShift,
		(unsigned long)g_display.stereoDepth, (unsigned long)g_display.stereoFocus,
		(unsigned long)g_display.stereoHud);
}

// The top screen's framebuffer registers, 0x10400468..0x10400498 (3dbrew's
// "GPU/External Registers"), through GSPGPU_ReadHWRegs' window at 0x10400000.
#define PDC_TOP_REGS 0x400468u
enum { PDC_LEFT_A, PDC_LEFT_B, PDC_FORMAT, PDC_SELECT = 4, PDC_RIGHT_A = 11, PDC_RIGHT_B,
	PDC_COUNT };

// A framebuffer's physical address back to one the CPU reads: the linear
// heap (gfxInitDefault's framebuffers) or VRAM.
static const u8* physToVirt(u32 pa) {
	const u32 lin = osConvertVirtToPhys((const void*)__ctru_linear_heap);
	if (pa >= lin && pa - lin < __ctru_linear_heap_size)
		return (const u8*)(__ctru_linear_heap + (pa - lin));
	if (pa >= 0x18000000u && pa < 0x18600000u)
		return (const u8*)(0x1F000000u + (pa - 0x18000000u));
	return NULL;
}

static unsigned meanGreen(const u8* fb) {
	if (!fb)
		return 0;
	GSPGPU_InvalidateDataCache((void*)fb, PANEL_W * PANEL_H * 3);
	u32 sum = 0, n = 0;
	for (int x = 0; x < PANEL_H; x += 8)
		for (int y = 0; y < PANEL_W; y += 8, n++)
			sum += fb[(x * PANEL_W + y) * 3 + 1];
	return n ? (unsigned)(sum / n) : 0;
}

// The top screen's colour lookup table, which every pixel the panel shows goes
// through (0x10400480 selects an entry, 0x10400484 reads it and steps on): a
// few entries as 0xBBGGRR, and how many of the 256 are not the identity.
#define PDC_TOP_LUT_INDEX 0x400480u
#define PDC_TOP_LUT_DATA  0x400484u

static int lutProbe(char* out, int outSize) {
	static const int at[] = { 0, 8, 32, 64, 128, 255 };
	u32 lut[256];
	u32 zero = 0;
	if (R_FAILED(GSPGPU_WriteHWRegs(PDC_TOP_LUT_INDEX, &zero, 4)))
		return snprintf(out, (size_t)outSize, "LUT unreadable");
	int off = 0;
	for (int i = 0; i < 256; i++) {
		lut[i] = 0;
		GSPGPU_ReadHWRegs(PDC_TOP_LUT_DATA, &lut[i], 4);
		const u32 id = (u32)i * 0x010101u;
		if ((lut[i] & 0xFFFFFFu) != id)
			off++;
	}
	int n = snprintf(out, (size_t)outSize, "LUT %d of 256 off identity:", off);
	for (int k = 0; k < (int)(sizeof(at) / sizeof(at[0])) && n < outSize; k++)
		n += snprintf(out + n, (size_t)(outSize - n), " [%d] %06lX", at[k],
			(unsigned long)(lut[at[k]] & 0xFFFFFFu));
	return n;
}

unsigned stereo_shown_green(const u8** left) {
	u32 r[PDC_COUNT];
	memset(r, 0, sizeof(r));
	const u8* l = NULL;
	if (R_SUCCEEDED(GSPGPU_ReadHWRegs(PDC_TOP_REGS, r, sizeof(r))))
		l = physToVirt((r[PDC_SELECT] & 1) ? r[PDC_LEFT_B] : r[PDC_LEFT_A]);
	if (left)
		*left = l;
	return meanGreen(l);
}

void stereo_present_probe(char* out, int outSize, const u8** left, const u8** right) {
	u32 r[PDC_COUNT];
	memset(r, 0, sizeof(r));
	const Result rc = GSPGPU_ReadHWRegs(PDC_TOP_REGS, r, sizeof(r));
	const bool second = (r[PDC_SELECT] & 1) != 0;
	const u32 paL = second ? r[PDC_LEFT_B] : r[PDC_LEFT_A];
	const u32 paR = second ? r[PDC_RIGHT_B] : r[PDC_RIGHT_A];
	const u8* l = physToVirt(paL);
	const u8* rt = physToVirt(paR);
	if (left)
		*left = l;
	if (right)
		*right = rt;
	snprintf(out, (size_t)outSize, "present probe: rc %08lX format %08lX (3D %s) select %lu, "
		"shown left %08lX right %08lX%s, mean G left %u right %u; 3D %s, libctru %s, "
		"slider %.2f, right eye %s",
		(unsigned long)rc, (unsigned long)r[PDC_FORMAT],
		(r[PDC_FORMAT] & 0x20) ? "on" : "off", (unsigned long)r[PDC_SELECT],
		(unsigned long)paL, (unsigned long)paR, paL == paR ? " (the same)" : "",
		meanGreen(l), paR == paL ? meanGreen(l) : meanGreen(rt),
		g_display.stereo ? "on" : "off", gfxIs3D() ? "3D" : "2D",
		(double)osGet3DSliderState(), s.on ? "drawn" : "not drawn");
	const int n = (int)strlen(out);
	if (n + 2 < outSize) {
		out[n] = ';';
		out[n + 1] = ' ';
		lutProbe(out + n + 2, outSize - n - 2);
	}
}
