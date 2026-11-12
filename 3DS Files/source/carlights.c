// See carlights.h. Every constant below is retail's: the flares' Speed.elf's,
// the light on the road's SPEED2.EXE's (the GameCube's draws nothing; see
// ROAD_ALPHA). The comments name the addresses they were read from.
#include "carlights.h"
#include "shaders.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIGHTS_MAGIC    0x4C53464Eu  // 'NFSL' little-endian
#define LIGHTS_VERSION  3
#define LIGHTS_HDR      0x30
#define LIGHTS_BULB     8
#define LIGHTS_TEX      24
#define LIGHTS_MAX_BULB 64
#define LIGHTS_NTEX     5
#define TEX_ROAD        3            // 2PLAYERHEADLIGHT1, after the flares' three
#define TEX_BEAM        4            // HEADLIGHTS, the projected headlight (v3)
#define TEXFMT_RGB565   0
#define TEXFMT_RGBA8    2

// The CarRenderInfo constructor's colour for an empty HEADLIGHT_BULB slot,
// SPEED2.EXE's packed 0x0090aa90.
static const u8 kDefaultBulb[3] = { 0x90, 0xaa, 0x90 };

// RenderFlaresOnCar's intensities (0x8033c00c..0x8033c030): a lit headlight
// is its driver's base of 1 plus 1, and brake lights 0.85 at rest and that
// plus 16 while braking, the centre one half of it -- all clamped to 1, so
// lit and braking both come out at full strength.
#define HEAD_LIT        2.0f
#define BRAKE_REST      0.85f
#define BRAKE_ON        (0.85f + 16.0f)
#define BRAKE_CENTRE    0.5f

// eRenderLightFlare's fade (0x80322180/4): on the squared distance, full up to
// 32000 and gone at 40000.
#define FADE_NEAR2      32000.0f
#define FADE_FAR2       40000.0f
#define DIST_EPS        5e-11f       // 0x80322190
// A reflection's flare (flareType 1, CARLIGHTS_REFLECTION): faded between
// 3900 and 4000 (0x80322188/c), some 63 m, and in view while a box 2 m out
// from the lamp each way (r13-0x7498, 0x8046d948) meets the frustum --
// GetVisibleState where the main view's flare asks GetVisibleStateSB of the
// lamp alone.
#define REFL_FADE_NEAR2 3900.0f
#define REFL_FADE_FAR2  4000.0f
#define REFL_BOX        2.0f
// A world lamp's reflection on the wet road (flareType 2, #323): drawn at its
// mirror image under the road, in view while a box reflectionStretch out from
// that each way meets the frustum, and stretched down the screen by as much
// -- by half when the lamp is under 4 m (0x803221a4) off the road -- at a
// fixed alpha of 255 (0x803221a8). reflectionStretch (r13-0x7508) is
// MINreflectionStretch 1.2 + (MAXreflectionStretch 10 - 1.2) times the rain
// intensity (Rain +0x43e8), which SetRainBase sets to 1 and nothing changes:
// 10.
#define WREFL_STRETCH   10.0f
#define WREFL_LOW       4.0f
#define WREFL_ALPHA     255
// eLightFlare +0x1c, the height of the lamp's mirror image under the road:
// WREFL_UNSET (999, 0x80322170) until worked out, WREFL_NONE (9999,
// 0x80322174) for a lamp with no reflection -- within 0.05 m (0x80322178) of
// the road, or over none.
#define WREFL_UNSET     999.0f
#define WREFL_NONE      9999.0f
#define WREFL_GROUNDED  0.05f
// PORT: retail works a lamp's out the first time it draws, every lamp in the
// visible packs at once; the port asks the ground for at most this many a
// frame and leaves the rest a frame or two without their reflections, so
// that coming round a corner into a lit street is not a spike.
#define WREFL_PROBES    16
// FlareSweep (0x8046d95c): the quad turns this many degrees from the left
// edge of the screen to the right.
#define FLARE_SWEEP     240.0f
// eView+0xc, CalculateH: the eye's distance to a 256-unit-wide image plane.
#define H_HALF_WIDTH    256.0f
#define SCREEN_W        400.0f
#define SCREEN_HALF_W   200.0f
#define SCREEN_HALF_H   120.0f

// RenderTextureHeadlights' quad (SPEED2.EXE 0x8028cc..0x802900, Speed.elf
// 0x8046ee68..0x8046eea0): hOffX 13 m ahead of the car's origin, hRad 10 m
// either way along the car and 4.9 m across it, and hOffY, which nothing
// sets (SPEED2.EXE 0x8a1c4c), 0. The car has to be upright for it: its up
// axis's height at least 0.707 (0x78a7d4), within 45 degrees of vertical.
#define ROAD_OFF_X      13.0f
#define ROAD_OFF_Y      0.0f
#define ROAD_RAD_X      10.0f
#define ROAD_RAD_Y      4.9f
#define ROAD_UPRIGHT    0.707f
// The quad's vertex alpha: the colour OR 0x80000000. Speed.elf ORs the same
// constant into its RGBA-ordered colour, which sets red's top bit and leaves
// alpha 0, so the GameCube draws nothing; this is the PC's.
#define ROAD_ALPHA      0x80
// PORT (TASKS.md #362): retail's ePoly is one flat quad in the car's floor
// plane, so over a crest, a dip or a camber change its far end, 23 m out, sinks
// under the road and the depth test cuts a piece out of the light. Here the
// same quad -- corners, UVs and colour -- is split ROAD_GRID_X by ROAD_GRID_Y
// and each inner point dropped onto the road (the world lamps' ground hook)
// ROAD_LIFT above it, so the light lies on the road the way retail means it to.
// Only a ground within ROAD_DRAPE_MAX of the floor plane is taken: a road
// under a bridge, or none at all, leaves that point on the plane.
//
// A kerb (TASKS.md #385) is a step, not a slope: a cell with road at one edge
// and pavement at the other runs straight from one to the other, under the
// pavement, and the depth test cut the light off along every kerb. #385 drew
// such a cell flat at the step's height; the testers then saw "hollow grey and
// white squares that extend from the car" (#391) -- cells lifted whole over
// the road beside them, and over the top of any barrier within ROAD_DRAPE_MAX.
// Now (#391) the main view's road light is moved toward the eye, each point
// along its own line of sight, by ROAD_PULL of its distance (at most
// ROAD_PULL_MAX): nothing moves on the screen and the texture keeps its
// perspective, only the depth test gives way, by a margin that grows with the
// distance as a kerb's cover of the road behind it does under a low camera.
// A 0.15 m kerb seen from the chase camera's ~1.5-2 m needs about a tenth of
// the distance. ROAD_PULL_MAX stays under ROAD_LAMP_GAP, so a car's own body
// still hides the light under its nose. The road's reflection keeps its light
// where it is: its eye is the mirror image, not v->eye.
#define ROAD_GRID_X     6
#define ROAD_GRID_Y     4
#define ROAD_LIFT       0.06f
#define ROAD_DRAPE_MAX  3.0f
#define ROAD_PULL       0.1f
#define ROAD_PULL_MAX   1.0f
// #394 / #392: the drape is for a car on the road. A car in the air -- a jump,
// or the crash camera's wild hit, which throws it metres up -- kept its light
// on the road under it, up to ROAD_DRAPE_MAX below the lamps: the testers saw
// the player's light come from "like a 4 feet below the headlights" and the
// traffic's from under their front wheels, big. Retail's quad is in the car's
// floor plane whatever is under it, so it goes up with the car; here, while
// no wheel is down (CARLIGHTS_AIRBORNE), the drape fades out between
// ROAD_AIR_LO and ROAD_AIR_HI of the car's origin over the road under it (it
// rides about 0.2 m up on the road), and past that the quad is retail's flat
// one. The wheels, not the probe, say whether the car is up: the road under
// an origin is now and then a lower surface (0.9 m logged on a flat highway).
#define ROAD_AIR_LO     0.4f
#define ROAD_AIR_HI     1.0f
// #411: "headlight light clipping through the ground when driving up or down
// hills, especially when driving on the sidewalk or very close to walls". A
// point of the drape with no road under it -- inside the building beside the
// pavement, past the outside wall of a corner -- stayed on the car's floor
// plane. On a slope change that plane rises out of the ground or sinks into
// it, through the walls, and the depth pull (#391) brought what sank back
// through the road. Now:
//   - each row of points looks for the road where the rows nearer the car
//     say it goes (their slope carried on), within ROAD_DRAPE_MAX of that, so
//     the drape follows a hill instead of the floor plane;
//   - a point with no road there has no light: its vertex is faded to 0, so
//     the light ends at the road's edge instead of standing in a wall.
// A car with no road under itself keeps retail's flat quad, as in the air.
// The gap between a racer's foremost headlamp and the quad's near edge:
// retail's 3 m less the racers' lamps (1.71..1.75 m in the log). #384 kept it
// for the traffic's longer noses too; undone 2026-11-09 (retail).
#define ROAD_LAMP_GAP   1.25f
#define ROAD_QUADS      (ROAD_GRID_X * ROAD_GRID_Y)
// The drape is the same for a car's reflection pass and both eyes' main
// passes, so it is worked out once a frame per car and kept here.
#define ROAD_CACHE      48

// One row of LightFlareParameters, the order the table stores it in.
typedef struct {
	float minSize, maxSize;
	float r, g, b, a;
	float power;      // pow(facing, power) shapes the intensity
	float pull;       // metres toward the eye
	float minScale, maxScale;
	int tex;
} FlareParams;

// LightFlareParameterIndicies (0x80359128): headlights draw rows 0 and 1,
// brake lights row 2. The values are SingleLightFlareParameters
// (0x80358e94) as RenderFlares (0x80240840) leaves it for the main view in
// a race: it puts the inner row's power at 1.45 while TheGameFlowManager's
// state is 6, the one BeginGameFlowRacing sets, and 2.8 (0x8033c2d0) in any
// other. eDisplayFrame calls it right before RenderFEFlares on the same main
// view, so the frontend's cars (game flow 3) draw with 2.8.
static const FlareParams kHeadInner = { 0, 64, 100, 120, 170, 80, 1.45f, 0.8f, 0.01f, 0.048f, 0 };
static const FlareParams kHeadInnerFE = { 0, 64, 100, 120, 170, 80, 2.8f, 0.8f, 0.01f, 0.048f, 0 };
// The rear-view mirror's (CARLIGHTS_MIRROR): FUN_0061ae90 for view 3 scales
// the inner row's min and max size by 2.7 (0x7a6764) and its power by 0.75
// (0x7a6760).
static const FlareParams kHeadInnerRVM = { 0, 64 * 2.7f, 100, 120, 170, 80, 1.45f * 0.75f, 0.8f,
	0.01f, 0.048f, 0 };
static const FlareParams kHeadOuter ={ 28, 28, 100, 120, 140, 20, 3.0f, 0.8f, 0.01f, 0.015f, 1 };
static const FlareParams kBrake     = { 20, 30, 128, 25, 25, 120, 0.45f, 0.2f, 0.01f, 0.03f, 2 };

typedef struct {
	float pos[3];
	u8    rgba[4];
	float uv[2];
} FlareVert;

// eLightFlare's flags (+0x2d), what eRenderLightFlare does with the lamp's
// facing, the cosine between its direction and the way to the eye: bit 1 takes
// its size (a lamp lit both ways), else it is clamped at 0; bit 4 shapes the
// intensity by pow(facing, power), and without it the caller's intensity
// stands. A car's lamps are 4; the world's are almost all 2 -- neither -- and
// draw full whichever way they face.
#define FLARE_TWO_SIDED 1
#define FLARE_SHAPED    4

// Six vertices a quad. A stock car draws at most seven (two layers on each
// headlight, one on each of three brake lights) and its light on the road,
// ROAD_QUADS since #362 draped it; every car goes in four times -- the road's
// reflection and the main view, once an eye (stereo.h, #364). A frame has room
// for 3072 (TASKS.md #383: 1024 overflowed with 16 traffic cars, 18 cars x 19
// quads x 4 passes, and the cars drawn last lost their lights at random). A
// car that would not fit is skipped, and that is logged. The world's lamps (#323) have a buffer of
// their own, open while the world is: one quad a lamp in view, each eye's
// view and the reflection's, and the frame's stop at 2048.
#define QUAD_VERTS      6
// 4096 since #385's 6 x 4 road light: 31 quads a car, four passes, 25 cars.
#define FRAME_VERTS     (4096 * QUAD_VERTS)
#define WORLD_VERTS     (2048 * QUAD_VERTS)

typedef struct {
	u32 hash;
	u8 rgb[3];
} Bulb;

static void (*s_log)(const char*);
static Bulb* s_bulbs;
static int s_nBulbs;
static C3D_Tex s_tex[LIGHTS_NTEX];
static bool s_texOk[LIGHTS_NTEX];
static FlareVert* s_vbuf;
static int s_half, s_used;
static FlareVert* s_wbuf;    // the world's lamps', WORLD_VERTS a half
static int s_wused;
static int s_wprobes;        // ground probes left this frame (reflZ())

static DVLE_s*         s_dvle;   // the shared blob\'s (shaders.h, #352)
static shaderProgram_s s_prog;
static int             s_uMvp = -1;
static bool            s_shader;

void carlights_set_log(void (*sink)(const char*)) { s_log = sink; }

static void llog(const char* m) {
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

// One Tex entry's mip chain, already tiled by the packer, a C3D_TexLoadImage
// per level the way carneon.c uploads NEONBLUR. `type` is GPU_TEX_2D, or
// GPU_TEX_PROJECTION for the beam, which only texture unit 0 can sample.
static bool loadTex(FILE* f, const u8* e, long size, C3D_Tex* tex, GPU_TEXTURE_MODE_PARAM type) {
	const u32 w = rd32(e + 4), ht = rd32(e + 8), fld = rd32(e + 12);
	const u32 off = rd32(e + 16), bytes = rd32(e + 20);
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
	C3D_TexInitParams tp = { (u16)w, (u16)ht, (u8)mlvl, gfmt, type, false };
	if (ok)
		ok = C3D_TexInitWithParams(tex, NULL, tp);
	if (ok) {
		const u8* src = buf;
		u32 sz = lvl0;
		for (u32 lv = 0; lv <= mlvl; lv++) {
			C3D_TexLoadImage(tex, src, GPU_TEXFACE_2D, (int)lv);
			src += sz;
			sz >>= 2;
		}
		C3D_TexFlush(tex);
		C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
		if (mlvl)
			C3D_TexSetFilterMipmap(tex, GPU_LINEAR);
		C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	free(buf);
	return ok;
}

// The projected headlight's clip (FUN_005bc5d0): a 64x1 ramp along the beam,
// texels 33 to 47 (int)(255 * sqrt(1 - i / 15)) for i = 0..14 and the rest
// 0, every channel the same. HeadlightClipMat looks it up at 0.5 + 0.005 w,
// so the beam starts 1.6 m out, fades to nothing at 51.6 m and is gone
// behind the projector. The PICA's smallest texture is 8 high, so the row is
// repeated; L8 gives the TEV the same value in red, green and blue.
#define HL_CLIP_W       64
#define HL_CLIP_H       8
#define HL_CLIP_FIRST   33           // 0x21
#define HL_CLIP_END     48           // 0x30

static C3D_Tex s_clip;
static bool s_clipOk;

static bool makeClip(void) {
	if (!C3D_TexInit(&s_clip, HL_CLIP_W, HL_CLIP_H, GPU_L8))
		return false;
	u8 ramp[HL_CLIP_W];
	for (int i = 0; i < HL_CLIP_W; i++)
		ramp[i] = (i >= HL_CLIP_FIRST && i < HL_CLIP_END)
			? (u8)(int)(sqrtf(1.0f - (float)(i - HL_CLIP_FIRST) * (1.0f / 15.0f)) * 255.0f)
			: 0;
	// 8x8 tiles, Morton order inside each; every row is the same, so only x
	// decides a texel.
	u8* px = (u8*)s_clip.data;
	for (int y = 0; y < HL_CLIP_H; y++)
		for (int x = 0; x < HL_CLIP_W; x++) {
			const int mx = x & 7, my = y & 7;
			const int m = (mx & 1) | ((my & 1) << 1) | ((mx & 2) << 1) |
			              ((my & 2) << 2) | ((mx & 4) << 2) | ((my & 4) << 3);
			px[((y >> 3) * (HL_CLIP_W >> 3) + (x >> 3)) * 64 + m] = ramp[x];
		}
	C3D_TexFlush(&s_clip);
	C3D_TexSetFilter(&s_clip, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&s_clip, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	s_clipOk = true;
	return true;
}

// #412: how far from the middle HEADLIGHTS is lit, as a fraction of the
// textured frustum's half-width (s) and half-height (t): the largest
// |texel centre - 1/2| of any texel with colour, plus one texel for the
// bilinear filter, doubled; over every mip level. HEADLIGHTS (128x64) has
// colour in columns 22..106 and rows 12..58 only, so the beam is black over a
// third of its frustum's width. |c - 1/2| reads the same whichever way the
// tiles run, so the swizzle's order does not matter, only the texel's spot.
static float s_beamExtS = 1.0f, s_beamExtT = 1.0f;

static void beamExtent(C3D_Tex* t) {
	float es = 0.0f, et = 0.0f;
	bool any = false;
	const bool four = t->fmt == GPU_RGBA8;
	for (int lv = 0; lv <= (int)t->maxLevel; lv++) {
		const u32 w = (u32)t->width >> lv, h = (u32)t->height >> lv;
		const u8* px = (const u8*)C3D_Tex2DGetImagePtr(t, lv, NULL);
		for (u32 i = 0; i < w * h; i++) {
			bool lit;
			if (four)        // A, B, G, R in memory
				lit = px[i * 4 + 1] | px[i * 4 + 2] | px[i * 4 + 3];
			else {
				const u16 v = (u16)(px[i * 2] | (px[i * 2 + 1] << 8));
				lit = t->fmt == GPU_RGBA4 ? (v & 0xFFF0u) != 0 : v != 0;
			}
			if (!lit)
				continue;
			const u32 tile = i >> 6, m = i & 63;
			const u32 x = ((tile % (w >> 3)) << 3) | (m & 1) | ((m >> 1) & 2) | ((m >> 2) & 4);
			const u32 y = ((tile / (w >> 3)) << 3) | ((m >> 1) & 1) | ((m >> 2) & 2) | ((m >> 3) & 4);
			const float ds = fabsf(((float)x + 0.5f) / (float)w - 0.5f) + 1.0f / (float)w;
			const float dt = fabsf(((float)y + 0.5f) / (float)h - 0.5f) + 1.0f / (float)h;
			if (ds > es) es = ds;
			if (dt > et) et = dt;
			any = true;
		}
	}
	s_beamExtS = any && 2.0f * es < 1.0f ? 2.0f * es : 1.0f;
	s_beamExtT = any && 2.0f * et < 1.0f ? 2.0f * et : 1.0f;
}

bool carlights_open(const char* path) {
	carlights_close();
	char b[200];
	FILE* f = path ? fopen(path, "rb") : NULL;
	if (!f) {
		snprintf(b, sizeof(b), "lights: no %s -- cars draw without light flares (run tools/carlights_pack.py)",
			path ? path : "CARLIGHTS.nfsl");
		llog(b);
		return false;
	}
	u8 h[LIGHTS_HDR];
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	bool ok = size >= LIGHTS_HDR && fread(h, 1, LIGHTS_HDR, f) == LIGHTS_HDR &&
	          rd32(h) == LIGHTS_MAGIC && rd32(h + 4) == LIGHTS_VERSION;
	const u32 n = ok ? rd32(h + 0x08) : 0, bulbOff = ok ? rd32(h + 0x0c) : 0;
	const u32 nTex = ok ? rd32(h + 0x10) : 0, texOff = ok ? rd32(h + 0x14) : 0;
	ok = ok && n <= LIGHTS_MAX_BULB && (u64)bulbOff + n * LIGHTS_BULB <= (u64)size &&
	     nTex == LIGHTS_NTEX && (u64)texOff + nTex * LIGHTS_TEX <= (u64)size;
	u8 raw[LIGHTS_MAX_BULB * LIGHTS_BULB];
	u8 ents[LIGHTS_NTEX * LIGHTS_TEX];
	ok = ok && fseek(f, (long)bulbOff, SEEK_SET) == 0 &&
	     fread(raw, 1, n * LIGHTS_BULB, f) == n * LIGHTS_BULB &&
	     fseek(f, (long)texOff, SEEK_SET) == 0 &&
	     fread(ents, 1, sizeof(ents), f) == sizeof(ents);
	if (ok && n) {
		s_bulbs = (Bulb*)malloc(n * sizeof(Bulb));
		ok = s_bulbs != NULL;
	}
	if (ok) {
		for (u32 i = 0; i < n; i++) {
			s_bulbs[i].hash = rd32(raw + i * LIGHTS_BULB);
			memcpy(s_bulbs[i].rgb, raw + i * LIGHTS_BULB + 4, 3);
		}
		s_nBulbs = (int)n;
		for (int t = 0; t < LIGHTS_NTEX && ok; t++)
			ok = s_texOk[t] = loadTex(f, ents + t * LIGHTS_TEX, size, &s_tex[t],
				t == TEX_BEAM ? GPU_TEX_PROJECTION : GPU_TEX_2D);
	}
	fclose(f);
	if (ok)
		beamExtent(&s_tex[TEX_BEAM]);
	ok = ok && makeClip();
	if (ok && initShader()) {
		s_vbuf = (FlareVert*)linearAlloc(2 * FRAME_VERTS * sizeof(FlareVert));
		ok = s_vbuf != NULL;
	} else {
		ok = false;
	}
	if (!ok) {
		snprintf(b, sizeof(b), "lights: %s is not a v%d CARLIGHTS.nfsl with its textures -- no light flares",
			path, LIGHTS_VERSION);
		llog(b);
		carlights_close();
		return false;
	}
	snprintf(b, sizeof(b), "lights: %d bulb colours, flare textures %ux%u %ux%u %ux%u, road %ux%u, beam %ux%u lit %.2f x %.2f",
		s_nBulbs, (unsigned)s_tex[0].width, (unsigned)s_tex[0].height,
		(unsigned)s_tex[1].width, (unsigned)s_tex[1].height, (unsigned)s_tex[2].width,
		(unsigned)s_tex[2].height, (unsigned)s_tex[TEX_ROAD].width,
		(unsigned)s_tex[TEX_ROAD].height, (unsigned)s_tex[TEX_BEAM].width,
		(unsigned)s_tex[TEX_BEAM].height, s_beamExtS, s_beamExtT);
	llog(b);
	return true;
}

void carlights_close(void) {
	free(s_bulbs);
	s_bulbs = NULL;
	s_nBulbs = 0;
	for (int t = 0; t < LIGHTS_NTEX; t++) {
		if (s_texOk[t])
			C3D_TexDelete(&s_tex[t]);
		s_texOk[t] = false;
	}
	if (s_clipOk)
		C3D_TexDelete(&s_clip);
	s_clipOk = false;
	if (s_vbuf)
		linearFree(s_vbuf);
	s_vbuf = NULL;
}

static const Bulb* lookup(u32 hash) {
	int lo = 0, hi = s_nBulbs;
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		if (s_bulbs[mid].hash == hash)
			return &s_bulbs[mid];
		if (s_bulbs[mid].hash < hash) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

void carlights_clear(CarLights* l) {
	memset(l, 0, sizeof(*l));
}

void carlights_setup(CarLights* l, const CarKitInfo* info) {
	carlights_clear(l);
	if (!info)
		return;
	l->lights = info->lights < CARKIT_LIGHTS ? info->lights : CARKIT_LIGHTS;
	memcpy(l->light, info->light, (size_t)l->lights * sizeof(CarKitLight));
	l->centreBrakeOff = info->centreBrakeOff;
	// The bulb's RED/GREEN/BLUE, which the GC build stores halved: its world
	// vertex colours count 0x80 as full, and the TEV doubles them back.
	// The light on the road follows the PC build (roadLight()), which keeps
	// them whole.
	const Bulb* bl = info->bulbPart ? lookup(info->bulbPart) : NULL;
	const u8* rgb = bl ? bl->rgb : kDefaultBulb;
	for (int c = 0; c < 3; c++) {
		l->bulb[c] = (u8)(rgb[c] >> 1);
		l->beam[c] = rgb[c];
	}
	l->set = true;
}

// InitCarDynamicLights' tweakables (Speed.elf .sdata 0x8046eee8..): reach,
// inner radius, offset along the car, colour and cone, head then tail.
static const float kDynSize[CARLIGHTS_DYNAMIC]   = { 90.0f, 10.0f };  // CarLightSize
static const float kDynInner[CARLIGHTS_DYNAMIC]  = { 0.0f, 0.0f };    // CarLightFarStart
static const float kDynOffset[CARLIGHTS_DYNAMIC] = { 0.0f, 1.0f };    // CarLightOffsets
// CarLightAngles: 16-bit angles, which the cone field takes as a float and
// the light query then reads as radians. A units slip in retail, kept: the
// cones it gives (cos 0.86 ahead, 0.29 behind) are what the cars are lit by.
static const u16 kDynAngle[CARLIGHTS_DYNAMIC]    = { 14000, 16384 };
// CarLightColours[1]: 0x90 read as R | G << 8 | B << 16, a dim red.
#define DYN_TAIL_COLOUR 0x00000090u

void carlights_dynamic(const CarLights* l, const float rot[3][3], const float origin[3],
		u32 flags, LightDbLight out[CARLIGHTS_DYNAMIC]) {
	// The headlight's colour is CarRenderInfo+0x380, which on the PC is the
	// whole bulb as B << 16 | G << 8 | R (SPEED2.EXE 0x635d52) -- what this
	// light reads as R, G, B. The GameCube's +0x380 is the halved bulb in
	// RGBA order, which the same read turns into (0, B/2, G/2); the port
	// follows the PC, as roadLight() does. Car+0x93c off makes it black.
	const u8* bulb = l && l->set ? l->beam : kDefaultBulb;
	const u32 head = (flags & CARLIGHTS_HEAD_ON)
		? (u32)bulb[0] | ((u32)bulb[1] << 8) | ((u32)bulb[2] << 16) : 0u;
	for (int i = 0; i < CARLIGHTS_DYNAMIC; i++) {
		LightDbLight* L = &out[i];
		memset(L, 0, sizeof(*L));
		L->type = LIGHTDB_TYPE_SPOT;
		L->b5 = 2;
		L->state = 1;
		L->colour = i == 0 ? head : DYN_TAIL_COLOUR;
		// Ahead for the headlight, behind for the tail light, along the
		// car's forward axis (Car+0x88).
		const float sgn = i == 0 ? 1.0f : -1.0f;
		for (int c = 0; c < 3; c++) {
			L->dir[c] = sgn * rot[0][c];
			L->pos[c] = origin[c] + kDynOffset[i] * L->dir[c];
		}
		L->radius = kDynSize[i];
		L->inner = kDynInner[i];
		L->cone = (float)kDynAngle[i];
	}
}

static u32 s_frameNo = 1;   // carlights_frame()'s count, for the road light's cache
// For the log: road lights draped since the last line, which
// carlights_frame() writes every ROAD_LOG_FRAMES.
#define ROAD_LOG_FRAMES 300u
static u32 s_roadDrapes;
static u32 s_roadAir;              // drapes faded for a car in the air (#394)
static float s_roadClrLo = 1e9f, s_roadClrHi = -1e9f;   // floor over the road

void carlights_frame(void) {
	s_frameNo++;
	if (s_frameNo % ROAD_LOG_FRAMES == 0 && s_log && s_roadDrapes) {
		char b[176];
		snprintf(b, sizeof(b), "car lights: road light, %lu drapes, "
			"%lu in the air, origin %.2f..%.2f m over the road up there (#394, %u frames)",
			(unsigned long)s_roadDrapes, (unsigned long)s_roadAir,
			(double)(s_roadClrLo < 1e8f ? s_roadClrLo : 0.0f),
			(double)(s_roadClrHi > -1e8f ? s_roadClrHi : 0.0f), ROAD_LOG_FRAMES);
		s_log(b);
		s_roadDrapes = s_roadAir = 0;
		s_roadClrLo = 1e9f;
		s_roadClrHi = -1e9f;
	}
	s_half ^= 1;
	s_used = 0;
	s_wused = 0;
	s_wprobes = WREFL_PROBES;
}

static void toWorld(float out[3], const float p[3], const float rot[3][3],
		const float origin[3]) {
	for (int i = 0; i < 3; i++)
		out[i] = p[0] * rot[0][i] + p[1] * rot[1][i] + p[2] * rot[2][i] + origin[i];
}

static float dot3(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void put(FlareVert* q, const float p[3], const u8 rgba[4], float u, float v) {
	memcpy(q->pos, p, sizeof(q->pos));
	memcpy(q->rgba, rgba, 4);
	q->uv[0] = u;
	q->uv[1] = 1.0f - v;   // the PICA's V runs up the texture
}

typedef struct {
	FlareVert v[QUAD_VERTS];
	int tex;
} Quad;

// GetVisibleState: an axis-aligned box, its centre `d` from the eye and
// `half` its half-size, against the view's four sides and the eye's plane --
// outside when its corner furthest in along a plane's normal is behind it.
// The sides are the screen test's, |right.d| pxX <= 200 fwd.d and
// |up.d| pxY <= 120 fwd.d.
static bool boxInView(const float d[3], const float half[3], const CarNeonView* v) {
	float n[5][3];
	for (int i = 0; i < 3; i++) {
		n[0][i] = SCREEN_HALF_W * v->fwd[i] - v->pxX * v->right[i];
		n[1][i] = SCREEN_HALF_W * v->fwd[i] + v->pxX * v->right[i];
		n[2][i] = SCREEN_HALF_H * v->fwd[i] - v->pxY * v->up[i];
		n[3][i] = SCREEN_HALF_H * v->fwd[i] + v->pxY * v->up[i];
		n[4][i] = v->fwd[i];
	}
	for (int k = 0; k < 5; k++) {
		const float reach = half[0] * fabsf(n[k][0]) + half[1] * fabsf(n[k][1]) +
			half[2] * fabsf(n[k][2]);
		if (dot3(n[k], d) + reach < 0.0f)
			return false;
	}
	return true;
}

// eRenderLightFlare's flareType: the main view's, a car's in the road's
// reflection, a world lamp's in it, and the light flare pool's in the main
// view (flareType 3: the main view's test and fade, no turn).
enum { FLARE_MAIN, FLARE_REFL_CAR, FLARE_REFL_WORLD, FLARE_POOL };

// eRenderLightFlare for one lamp, both of its layers, into `out`. `right`
// and `down` are the camera matrix's first two columns; like retail, each
// layer turns and scales them in place, so a headlight's outer layer comes
// out turned twice and sized by both layers' radii. `type` is a FLARE_*:
// FLARE_REFL_CAR, flareType 1, is the same quad at the same place, with its
// own visibility and fade and no turn (eCreateAxisRotationMatrix is type 0's
// alone); FLARE_REFL_WORLD, flareType 2, the lamp's mirror image, its `wp`
// and `dir` mirrored by the caller, its down axis times `stretch`, at
// WREFL_ALPHA; FLARE_POOL, flareType 3, FLARE_MAIN with no turn, the caller
// having worked its intensity out. `flags` is the lamp's FLARE_*.
static int flare(Quad* out, const FlareParams* const* rows, int nRows, const float wp[3],
		const float dir[3], float intensity, const u8* override, const CarNeonView* v,
		float H, int type, u32 flags, float stretch) {
	const float d[3] = { wp[0] - v->eye[0], wp[1] - v->eye[1], wp[2] - v->eye[2] };
	const float z = dot3(d, v->fwd);
	float sx = 0.0f;
	if (type == FLARE_REFL_CAR) {
		static const float box[3] = { REFL_BOX, REFL_BOX, REFL_BOX };
		if (!boxInView(d, box, v))
			return 0;
	} else if (type == FLARE_REFL_WORLD) {
		static const float box[3] = { WREFL_STRETCH, WREFL_STRETCH, WREFL_STRETCH };
		if (!boxInView(d, box, v))
			return 0;
	} else {
		// GetVisibleStateSB: the lamp itself has to be inside the view.
		if (z <= 1e-4f)
			return 0;
		sx = dot3(d, v->right) / z * v->pxX;
		const float sy = dot3(d, v->up) / z * v->pxY;
		if (sx < -SCREEN_HALF_W || sx > SCREEN_HALF_W || sy < -SCREEN_HALF_H ||
		    sy > SCREEN_HALF_H)
			return 0;
	}

	float toCam[3] = { -d[0], -d[1], -d[2] };
	const float d2 = dot3(toCam, toCam);
	const bool refl = type == FLARE_REFL_CAR;
	const float fadeNear = refl ? REFL_FADE_NEAR2 : FADE_NEAR2;
	const float fadeFar = refl ? REFL_FADE_FAR2 : FADE_FAR2;
	if (d2 > fadeFar || d2 <= DIST_EPS)
		return 0;
	const float fade = d2 > fadeNear ? 1.0f - (d2 - fadeNear) / (fadeFar - fadeNear) : 1.0f;
	const float inv = 1.0f / sqrtf(d2);
	for (int i = 0; i < 3; i++)
		toCam[i] *= inv;
	float facing = dot3(toCam, dir);
	if (flags & FLARE_TWO_SIDED)
		facing = fabsf(facing);
	else if (facing < 0.0f)
		facing = 0.0f;

	// Behind the eye -- a reflection's box can be in view with its lamp not
	// -- this is under the rows' minScale, which it is clamped to.
	float scale = z / H;
	// The turn, from where the lamp sits across the screen: GetScreenPosition
	// over GetPixelWidth, 0 at the left edge.
	const float ang = type != FLARE_MAIN ? 0.0f
		: FLARE_SWEEP * ((sx + SCREEN_HALF_W) / SCREEN_W) * ((float)M_PI / 180.0f);
	const float cs = cosf(ang), sn = sinf(ang);
	float R[3], D[3];
	for (int i = 0; i < 3; i++) {
		R[i] = v->right[i];
		D[i] = -v->up[i];
	}

	int n = 0;
	for (int k = 0; k < nRows; k++) {
		const FlareParams* p = rows[k];
		float I = intensity;
		if (flags & FLARE_SHAPED) {
			I *= powf(facing, p->power);
			if (I > 1.0f)
				I = 1.0f;
		}
		const float size = (p->maxSize - p->minSize) * I + p->minSize;
		if (size <= 0.0f)
			continue;
		float c[3];
		for (int i = 0; i < 3; i++)
			c[i] = wp[i] + toCam[i] * p->pull;
		if (scale < p->minScale)
			scale = p->minScale;
		if (scale > p->maxScale)
			scale = p->maxScale;
		const float rad = size * scale;
		// eCreateAxisRotationMatrix about the view axis, applied as v * M:
		// screen-right turns toward screen-up.
		for (int i = 0; i < 3; i++) {
			const float r = R[i] * cs - D[i] * sn;
			const float dn = D[i] * cs + R[i] * sn;
			R[i] = r * rad;
			D[i] = dn * rad;
		}
		if (type == FLARE_REFL_WORLD)
			for (int i = 0; i < 3; i++)
				D[i] *= stretch;
		int a = type == FLARE_REFL_WORLD ? WREFL_ALPHA : (int)(p->a * I * fade);
		if (a <= 0)
			continue;
		if (a > 255)
			a = 255;
		u8 rgba[4];
		if (override) {
			memcpy(rgba, override, 3);
		} else {
			rgba[0] = (u8)(int)(p->r * I);
			rgba[1] = (u8)(int)(p->g * I);
			rgba[2] = (u8)(int)(p->b * I);
		}
		rgba[3] = (u8)a;
		// ePoly's corners and its default UVs: c-r-d (0,0), c+r-d (1,0),
		// c+r+d (1,1), c-r+d (0,1); `d` points down the screen.
		float q[4][3];
		for (int i = 0; i < 3; i++) {
			q[0][i] = c[i] - R[i] - D[i];
			q[1][i] = c[i] + R[i] - D[i];
			q[2][i] = c[i] + R[i] + D[i];
			q[3][i] = c[i] - R[i] + D[i];
		}
		static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		static const int tri[QUAD_VERTS] = { 0, 1, 2, 0, 2, 3 };
		Quad* o = &out[n++];
		for (int j = 0; j < QUAD_VERTS; j++)
			put(&o->v[j], q[tri[j]], rgba, uv[tri[j]][0], uv[tri[j]][1]);
		o->tex = p->tex;
	}
	return n;
}

static CarLightsGround s_ground;
static void* s_groundUd;

// RenderTextureHeadlights (Speed.elf 0x80236f98, SPEED2.EXE 0x60d5d0): the
// headlights' light on the road, 2PLAYERHEADLIGHT1 on one ePoly lying in the
// car's z = 0 plane, in the bulb's colour. U runs along the car, so the
// texture's left edge, where its two lamps' hot spots are, lies 3 m ahead
// and its right edge 23 m ahead.
//
// RenderCarsTexturedHeadlights (0x80236d24 / 0x615490) draws it for every
// car in the world whose lamps are on, after every flare, under
// TexturedHeadLightsON (1 in both builds, and nothing writes it). Its other
// tests are the flares': the car drawn and not blinking, which the caller
// owns, and the render box at ten times its size in view, which the GPU's
// clipping does.
// The ground under one point of the quad, ROAD_LIFT over it, or false for no
// ground within ROAD_DRAPE_MAX of the floor plane's height `z`.
static bool roadGround(const float p[3], float z, float* out) {
	if (!s_ground)
		return false;
	const float probe[3] = { p[0], p[1], z + ROAD_DRAPE_MAX };
	float g;
	if (!s_ground(probe, &g, s_groundUd) || fabsf(g - z) > ROAD_DRAPE_MAX)
		return false;
	*out = g + ROAD_LIFT;
	return true;
}

typedef struct {
	const CarLights* l;
	u32   frame;
	float origin[3];
	float pts[ROAD_GRID_X + 1][ROAD_GRID_Y + 1][3];
	u8    lit[ROAD_GRID_X + 1][ROAD_GRID_Y + 1];   // 0 = no road there (#411)
} RoadDrape;

static RoadDrape s_drape[ROAD_CACHE];
static int s_drapeNext;

// This frame's drape for car `l` at `rot`/`origin`, worked out if it is not
// in the cache yet.
static const RoadDrape* roadDrape(const CarLights* l, const float rot[3][3],
		const float origin[3], float offX, bool airborne) {
	for (int c = 0; c < ROAD_CACHE; c++) {
		const RoadDrape* d = &s_drape[c];
		if (d->l == l && d->frame == s_frameNo && !memcmp(d->origin, origin, sizeof(d->origin)))
			return d;
	}
	RoadDrape* d = &s_drape[s_drapeNext];
	s_drapeNext = (s_drapeNext + 1) % ROAD_CACHE;
	d->l = l;
	d->frame = s_frameNo;
	memcpy(d->origin, origin, sizeof(d->origin));
	s_roadDrapes++;
	// How much of the drape a car this far over the road gets (#394): 1 on
	// the road, 0 in the air. No road under it leaves nothing to drape onto.
	float drape = 1.0f, gO;
	// No road under the car itself: retail's flat quad, nothing faded (#411).
	const bool onRoad = roadGround(origin, origin[2], &gO);
	if (airborne && onRoad) {
		const float clr = origin[2] - (gO - ROAD_LIFT);
		if (clr < s_roadClrLo) s_roadClrLo = clr;
		if (clr > s_roadClrHi) s_roadClrHi = clr;
		if (clr > ROAD_AIR_LO) {
			drape = clr >= ROAD_AIR_HI ? 0.0f
			      : 1.0f - (clr - ROAD_AIR_LO) / (ROAD_AIR_HI - ROAD_AIR_LO);
			s_roadAir++;
		}
	}
	// The corners in ePoly order with its default UVs (ePoly::ePoly,
	// SPEED2.EXE 0x5bb1c0): (0,0) at -x -y, (1,0) at +x -y, (1,1) at +x +y,
	// (0,1) at -x +y -- U along the car, V across it.
	// Rows from the car out (i is along it): a row's road is looked for where
	// the two rows before it, found, say it goes (#411).
	float found[ROAD_GRID_X + 1][ROAD_GRID_Y + 1];
	bool has[ROAD_GRID_X + 1][ROAD_GRID_Y + 1];
	for (int i = 0; i <= ROAD_GRID_X; i++) {
		for (int j = 0; j <= ROAD_GRID_Y; j++) {
			const float u = (float)i / ROAD_GRID_X, v = (float)j / ROAD_GRID_Y;
			const float local[3] = { offX - ROAD_RAD_X + 2.0f * ROAD_RAD_X * u,
			                         ROAD_OFF_Y - ROAD_RAD_Y + 2.0f * ROAD_RAD_Y * v, 0.0f };
			float* q = d->pts[i][j];
			toWorld(q, local, rot, origin);
			d->lit[i][j] = 1;
			has[i][j] = false;
			if (drape <= 0.0f)
				continue;
			float want = q[2];
			if (i >= 2 && has[i - 1][j] && has[i - 2][j])
				want = 2.0f * found[i - 1][j] - found[i - 2][j];
			else if (i >= 1 && has[i - 1][j])
				want = found[i - 1][j] + (q[2] - d->pts[i - 1][j][2]);
			float g;
			if (roadGround(q, want, &g) || (want != q[2] && roadGround(q, q[2], &g))) {
				found[i][j] = g;
				has[i][j] = true;
				q[2] += drape * (g - q[2]);
			} else if (onRoad) {
				d->lit[i][j] = 0;
			}
		}
	}
	return d;
}

float carlights_head_x(const CarLights* l) {
	float x = 0.0f;
	for (int i = 0; l && i < l->lights; i++)
		if (l->light[i].type == CARKIT_LIGHT_HEAD && l->light[i].pos[0] > x)
			x = l->light[i].pos[0];
	return x;
}

static int roadLight(Quad* out, const CarLights* l, const float rot[3][3],
		const float origin[3], bool airborne, const float* eye) {
	if (rot[2][2] < ROAD_UPRIGHT)
		return 0;
	// Retail's quad starts 3 m ahead of every car's origin, whatever the car:
	// under a van's or a bus's nose, where the crash camera's low view sees
	// the light come out from under the wheels. That is the PC's own look;
	// #384's forward shift for long noses was undone at the user's word
	// (2026-11-09).
	const RoadDrape* d = roadDrape(l, rot, origin, ROAD_OFF_X, airborne);
	// The PC's +0x380 keeps the bulb whole (0x635d52: B << 16 | G << 8 | R),
	// and its no-fog world shader doubles colour, not alpha.
	const u8 rgba[4] = { l->beam[0], l->beam[1], l->beam[2], ROAD_ALPHA };
	static const int tri[QUAD_VERTS][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
	int n = 0;
	for (int i = 0; i < ROAD_GRID_X; i++) {
		for (int j = 0; j < ROAD_GRID_Y; j++) {
			Quad* o = &out[n++];
			for (int k = 0; k < QUAD_VERTS; k++) {
				const int a = i + tri[k][0], b = j + tri[k][1];
				float p[3];
				memcpy(p, d->pts[a][b], sizeof(p));
				static const u8 kDark[4] = { 0, 0, 0, 0 };
				if (eye) {
					// Toward the eye along its line of sight (#391).
					const float e[3] = { eye[0] - p[0], eye[1] - p[1], eye[2] - p[2] };
					const float dist = sqrtf(dot3(e, e));
					float t = ROAD_PULL;
					if (dist * t > ROAD_PULL_MAX)
						t = ROAD_PULL_MAX / dist;
					for (int c = 0; c < 3; c++)
						p[c] += e[c] * t;
				}
				put(&o->v[k], p, d->lit[a][b] ? rgba : kDark, (float)a / ROAD_GRID_X,
					(float)b / ROAD_GRID_Y);
			}
			o->tex = TEX_ROAD;
		}
	}
	return n;
}

static void drawQuads(FlareVert* base, int nv, const int start[LIGHTS_NTEX],
	const int count[LIGHTS_NTEX], const C3D_Mtx* vp, const float gain[3]);

void carlights_draw(const CarLights* l, const C3D_Mtx* vp, const CarNeonView* v,
		const float rot[3][3], const float origin[3], u32 flags, const float gain[3]) {
	if (!l || !l->set || !s_vbuf || !s_shader || !v || !v->fov)
		return;
	// RenderFlaresOnCar's first two tests -- the car at least 3 pixels big
	// and its render box in view -- only drop cars whose lamps the distance
	// fade and the per-lamp view test below drop anyway.
	const float H = H_HALF_WIDTH / tanf((float)v->fov * ((float)M_PI / 65536.0f));
	const bool braking = (flags & CARLIGHTS_BRAKING) != 0;
	const float brake = braking ? BRAKE_ON : BRAKE_REST;
	const float centre = l->centreBrakeOff ? 0.0f : braking ? BRAKE_ON * BRAKE_CENTRE : 0.0f;
	static const FlareParams* const raceRows[2] = { &kHeadInner, &kHeadOuter };
	static const FlareParams* const feRows[2] = { &kHeadInnerFE, &kHeadOuter };
	static const FlareParams* const rvmRows[2] = { &kHeadInnerRVM, &kHeadOuter };
	static const FlareParams* const brakeRows[1] = { &kBrake };
	const FlareParams* const* headRows = (flags & CARLIGHTS_FRONTEND) ? feRows
		: (flags & CARLIGHTS_MIRROR) ? rvmRows : raceRows;

	static Quad quads[CARKIT_LIGHTS * 2 + ROAD_QUADS];
	int nq = 0;
	const int lamps = (flags & CARLIGHTS_NO_FLARES) ? 0 : l->lights;
	for (int i = 0; i < lamps; i++) {
		const CarKitLight* lt = &l->light[i];
		float in;
		if (lt->type == CARKIT_LIGHT_HEAD) {
			// Only LEFT_ and RIGHT_HEADLIGHT have a case; a CENTRE_ one is 0.
			if (lt->side == CARKIT_LIGHT_CENTRE || !(flags & CARLIGHTS_HEAD_ON))
				continue;
			in = HEAD_LIT;
		} else {
			in = lt->side == CARKIT_LIGHT_CENTRE ? centre : brake;
		}
		if (in <= 0.0f)
			continue;
		if (in > 1.0f)
			in = 1.0f;
		float wp[3], dir[3];
		toWorld(wp, lt->pos, rot, origin);
		for (int c = 0; c < 3; c++)
			dir[c] = lt->dir[0] * rot[0][c] + lt->dir[1] * rot[1][c] + lt->dir[2] * rot[2][c];
		const bool head = lt->type == CARKIT_LIGHT_HEAD;
		nq += flare(quads + nq, head ? headRows : brakeRows, head ? 2 : 1, wp, dir, in,
			head ? l->bulb : NULL, v, H,
			(flags & CARLIGHTS_REFLECTION) ? FLARE_REFL_CAR : FLARE_MAIN, FLARE_SHAPED, 1.0f);
	}
	if ((flags & CARLIGHTS_HEAD_ON) && !(flags & (CARLIGHTS_FRONTEND | CARLIGHTS_MIRROR)))
		nq += roadLight(quads + nq, l, rot, origin, (flags & CARLIGHTS_AIRBORNE) != 0,
			(flags & CARLIGHTS_REFLECTION) ? NULL : v->eye);
	if (!nq)
		return;
	if (s_used + nq * QUAD_VERTS > FRAME_VERTS) {
		static bool said;
		if (!said && s_log) {
			char b[96];
			snprintf(b, sizeof(b), "car lights: the frame's %d quads are full -- a car's lights skipped",
				FRAME_VERTS / QUAD_VERTS);
			s_log(b);
			said = true;
		}
		return;
	}

	// Additive quads commute, so they go in a texture at a time, and the
	// light on the road can go with its car's flares instead of after every
	// car's. 2PLAYERHEADLIGHT1 is additive too (alpha blend 2, mat 0xaa).
	FlareVert* const base = s_vbuf + s_half * FRAME_VERTS + s_used;
	int start[LIGHTS_NTEX], count[LIGHTS_NTEX];
	FlareVert* w = base;
	for (int t = 0; t < LIGHTS_NTEX; t++) {
		start[t] = (int)(w - base);
		for (int k = 0; k < nq; k++) {
			if (quads[k].tex != t)
				continue;
			memcpy(w, quads[k].v, sizeof(quads[k].v));
			w += QUAD_VERTS;
		}
		count[t] = (int)(w - base) - start[t];
	}
	s_used += (int)(w - base);
	drawQuads(base, (int)(w - base), start, count, vp, gain);
}

// The quads `base` holds, `count[t]` vertices from `start[t]` in texture t,
// drawn. The caller has counted them used: they stay for the GPU until this
// half of the buffer comes round again.
static void drawQuads(FlareVert* base, int nv, const int start[LIGHTS_NTEX],
		const int count[LIGHTS_NTEX], const C3D_Mtx* vp, const float gain[3]) {
	GSPGPU_FlushDataCache(base, (u32)nv * sizeof(FlareVert));

	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);           // v0 position
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);   // v1 colour
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);           // v2 texcoord
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, base, sizeof(FlareVert), 3, 0x210);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uMvp, vp);

	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	// The flare textures are black-edged and drawn additive, testing depth
	// against the car and the street without writing it.
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	// The vertex colours count 0x80 as full; carneon_light_env() doubles
	// them back. Colour only: the GC's psModulate (0x801e4c54, the ePoly
	// default) doubles alpha too,
	// but its textures keep the PS2's halved alpha, which the PC's carry
	// already doubled -- the flare textures here are 253..254 throughout --
	// and the PC's shaders leave alpha alone (afx.c). Either way a flare's
	// opacity is its row's A / 255. The light on the road is the PC's
	// throughout: the whole bulb colour doubled, at half opacity.
	carneon_light_env(gain);
	for (int t = 0; t < LIGHTS_NTEX; t++) {
		if (!count[t])
			continue;
		C3D_TexBind(0, &s_tex[t]);
		C3D_DrawArrays(GPU_TRIANGLES, start[t], count[t]);
	}

	carneon_light_env_end();
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

// ------------------------------------------------------- the world's lamps
//
// TASKS.md #323. Every streamed section with lamps carries a light flare pack
// (chunk 0x80135100); while the section is loaded its header hangs off the
// section's slot in DAT_803f50b0. eRenderWorldLightFlares (0x800cd1bc) -- in
// game flow 6, the world, with light flares on (r13-0x74b8) -- finds the
// drivable section under the view's eye (FindDrivableSection) and walks its
// visible list: for each loaded section on it whose pack's box GetVisibleState
// passes, every flare goes through eRenderLightFlare with intensity 1.0
// (0x803221c8), its reflection's z as cached (0.0, 0x803221cc), the identity
// for its matrix, no reflexion and the view's flareType. tools/flare_pack.py
// lays the packs out flat in WORLD.NFSF.

#define WFLARE_MAGIC    0x4653464Eu  // 'NFSF' little-endian
#define WFLARE_VERSION  2
#define WFLARE_HDR      0x30

typedef struct {
	u16 section, count;
	u32 first;
	float lo[3], hi[3];
} WorldPack;

typedef struct {
	float pos[3];
	float dir[3];
	u8 type, flags;          // LightFlareParameterIndicies' pair, FLARE_*
	u16 section;
	u32 hash;
} WorldFlare;

// A scenery instance whose models carry markers, and the markers placed in
// the world: nA for its A model, then nB for its B (flare_pack.py).
typedef struct {
	float pos[3];            // its translation, which finds it in the section
	u16 section;
	u8 nA, nB;
	u32 first;
} WorldInst;

typedef struct {
	float pos[3];
	u8 type, pad[3];         // the ePositionMarker's
} WorldMark;

_Static_assert(sizeof(WorldPack) == 32, "WORLD.NFSF's packs are 32 bytes");
_Static_assert(sizeof(WorldFlare) == 32, "WORLD.NFSF's flares are 32 bytes");
_Static_assert(sizeof(WorldInst) == 20, "WORLD.NFSF's instances are 20 bytes");
_Static_assert(sizeof(WorldMark) == 16, "WORLD.NFSF's marks are 16 bytes");

// LightFlareParameterIndicies' pair for type 9 (0x80359128 + 9 * 8): row
// 0x80358ff4 and none. Every lamp in L4RA is type 9; one of another type has
// no row here and is not drawn (the open counts them).
#define WFLARE_TYPE     9
static const FlareParams kWorld = { 30, 30, 110, 100, 75, 110, 1.0f, 4.0f, 0.001f, 0.1f, 2 };

// The light flare pool. DrawAScenery (0x80208fa4) takes a flare out of it
// (eGetNextLightFlareInPool, 0x800cbc58) for each ePositionMarker of the model
// it draws a scenery instance at in the player's view, view 1 or 2: a marker
// of type 3 into the second pool, the rest into the first, WPOOL_SIZE each and
// none once one is full. The flare is of type marker type + 10, at the
// instance's matrix times the marker less the model's pivot (flare_pack.py
// places them all); a type 3 faces out from the instance's translation across
// the ground and is shaped by facing (flags 4), the rest are flags 2, drawn
// whichever way they face. eRenderLightFlarePool (0x800cbdb0) draws both
// pools in the main view after the world's lamps, and eResestLightFlarePool
// (0x800cbf20) empties them at the end of eDisplayFrame -- so the road's
// reflection, which asks for the second pool before the main view's scenery
// has filled it, always finds it empty, and the port draws none there. (The
// GameCube's sphere map takes the second pool too, eRenderLightFlarePool(view,
// 0, 4); the port's env map is the PC's faces, which draw no flares.)
#define WPOOL_SIZE      200
#define WPOOL_TYPE      10        // the flare type of a marker of type 0
#define WPOOL_TYPES     5         // 10..14, the rows below
#define WPOOL_SPLIT     3         // the marker type the second pool takes
// eRenderLightFlarePool skips a type 13 in the second pool -- the median
// poles' lamps -- while the blink (0x8046d940) is off; eResestLightFlarePool
// flips it each time the world time it adds up passes half a second.
#define WPOOL_BLINKER   13
#define WPOOL_BLINK     0.5f
// eRenderLightFlare's flareType 3: a flare of any type but 13 and 14 is lit
// only inside the player's headlights, the cosine between the car's forward
// axis (+0x88) and the way from the car (+0x58) to the flare at least
// RainInTheHeadlights (0x8046f114), 0.96 -- 16 degrees either way -- its
// intensity rising from 0 there to 1 dead ahead.
#define WPOOL_CONE      0.96f
#define WPOOL_UNLIT     13        // types this and above draw at 1.0

// LightFlareParameterIndicies' rows for types 10..14 (0x80359128 + type * 8,
// one row each; SPEED2.EXE's are the same): the amber reflectors (the
// guardrails' and the cat's eyes'), the red (the post boots'), a blue one, the
// median poles' amber lamp and a type 14 L4RA's props never name -- the flag
// girl's flares' (#497, carlights_pool_flare()).
static const FlareParams kPool[WPOOL_TYPES] = {
	{ 10, 10, 110, 75, 0, 150, 1.0f, 2.7f, 0.001f, 0.1f, 2 },
	{ 10, 10, 110, 0, 0, 150, 1.0f, 4.0f, 0.001f, 0.1f, 2 },
	{ 10, 10, 0, 0, 120, 150, 1.0f, 4.0f, 0.001f, 0.1f, 2 },
	{ 30, 30, 110, 75, 0, 150, 1.0f, 4.0f, 0.001f, 0.1f, 2 },
	{ 1, 10, 105, 26.5f, 0, 70, 1.0f, 1.0f, 0.07f, 0.07f, 2 },
};

static u8* s_wfile;
static const WorldPack* s_wpacks;
static const WorldFlare* s_wflares;
static float* s_wreflZ;      // a lamp's eLightFlare +0x1c
static s16* s_wpackOf;       // section number -> its pack, -1 for none
static int s_wnumbers;       // entries in s_wpackOf, s_wiFirst and s_wiCount
static int s_wpeak[2];       // the most quads a view has drawn, for the log
static bool s_wfullLogged;
static const WorldInst* s_winsts;
static const WorldMark* s_wmarks;
static u16* s_wiFirst;       // section number -> its first marked instance
static u16* s_wiCount;       // and how many it has
static CarLightsInstRef* s_wrefs;
static u32 s_wmissing;       // marked instances their section did not have
static int s_wpoolPeak;      // the most props' flares a view has drawn
static CarLightsInstLod s_instLod;
static void* s_instLodUd;
static float s_blinkTime;
static bool s_blink;
static bool s_carSet;
static float s_carPos[3], s_carFwd[3];
// The pool's flares that are not a prop's (#497): FlagGirlFlareEffect::
// TriggerEffect takes one from the first pool each update, type 14, flags 2.
#define WPOOL_EXTRA     4
static struct { int type; float pos[3]; } s_wextra[WPOOL_EXTRA];
static bool s_wextraOn[WPOOL_EXTRA];

void carlights_pool_flare(int slot, int type, const float pos[3]) {
	if (slot < 0 || slot >= WPOOL_EXTRA)
		return;
	s_wextraOn[slot] = pos && type >= WPOOL_TYPE && type < WPOOL_TYPE + WPOOL_TYPES;
	if (s_wextraOn[slot]) {
		s_wextra[slot].type = type;
		memcpy(s_wextra[slot].pos, pos, sizeof(s_wextra[slot].pos));
	}
}

void carlights_world_set_ground(CarLightsGround fn, void* ud) {
	s_ground = fn;
	s_groundUd = ud;
}

void carlights_world_set_instances(CarLightsInstLod fn, void* ud) {
	s_instLod = fn;
	s_instLodUd = ud;
}

void carlights_world_tick(float dt) {
	s_blinkTime += dt;
	if (s_blinkTime > WPOOL_BLINK) {
		s_blink = !s_blink;
		s_blinkTime = 0.0f;
	}
}

void carlights_world_set_car(const float pos[3], const float fwd[3]) {
	s_carSet = pos && fwd;
	if (s_carSet) {
		memcpy(s_carPos, pos, sizeof(s_carPos));
		memcpy(s_carFwd, fwd, sizeof(s_carFwd));
	}
}

void carlights_world_close(void) {
	free(s_wfile);
	s_wfile = NULL;
	s_wpacks = NULL;
	s_wflares = NULL;
	free(s_wreflZ);
	s_wreflZ = NULL;
	free(s_wpackOf);
	s_wpackOf = NULL;
	s_wnumbers = 0;
	s_winsts = NULL;
	s_wmarks = NULL;
	free(s_wiFirst);
	s_wiFirst = NULL;
	free(s_wiCount);
	s_wiCount = NULL;
	free(s_wrefs);
	s_wrefs = NULL;
	s_wmissing = 0;
	s_wpoolPeak = 0;
	if (s_wbuf)
		linearFree(s_wbuf);
	s_wbuf = NULL;
	s_wpeak[0] = s_wpeak[1] = 0;
	s_wfullLogged = false;
}

bool carlights_world_open(const char* path) {
	carlights_world_close();
	char b[220];
	FILE* f = path ? fopen(path, "rb") : NULL;
	if (!f) {
		snprintf(b, sizeof(b), "lights: no %s -- the world's lamps draw without flares (run tools/flare_pack.py)",
			path ? path : "WORLD.NFSF");
		llog(b);
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	s_wfile = size >= WFLARE_HDR ? (u8*)malloc((size_t)size) : NULL;
	bool ok = s_wfile && fread(s_wfile, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	const u32 packs = ok ? rd32(s_wfile + 0x08) : 0, flares = ok ? rd32(s_wfile + 0x0c) : 0;
	const u32 po = ok ? rd32(s_wfile + 0x10) : 0, fo = ok ? rd32(s_wfile + 0x14) : 0;
	const u32 insts = ok ? rd32(s_wfile + 0x18) : 0, marks = ok ? rd32(s_wfile + 0x1c) : 0;
	const u32 io = ok ? rd32(s_wfile + 0x20) : 0, mo = ok ? rd32(s_wfile + 0x24) : 0;
	ok = ok && rd32(s_wfile) == WFLARE_MAGIC && rd32(s_wfile + 4) == WFLARE_VERSION &&
	     !(po & 3) && !(fo & 3) && !(io & 3) && !(mo & 3) && packs < 0x8000 &&
	     insts < 0x10000 &&
	     (u64)po + (u64)packs * sizeof(WorldPack) <= (u64)size &&
	     (u64)fo + (u64)flares * sizeof(WorldFlare) <= (u64)size &&
	     (u64)io + (u64)insts * sizeof(WorldInst) <= (u64)size &&
	     (u64)mo + (u64)marks * sizeof(WorldMark) <= (u64)size;
	int maxNum = -1;
	u32 untyped = 0;
	if (ok) {
		s_wpacks = (const WorldPack*)(s_wfile + po);
		s_wflares = (const WorldFlare*)(s_wfile + fo);
		s_winsts = (const WorldInst*)(s_wfile + io);
		s_wmarks = (const WorldMark*)(s_wfile + mo);
		for (u32 i = 0; i < packs && ok; i++) {
			ok = (u64)s_wpacks[i].first + s_wpacks[i].count <= flares;
			if (s_wpacks[i].section > maxNum)
				maxNum = s_wpacks[i].section;
		}
		for (u32 i = 0; i < flares; i++)
			untyped += s_wflares[i].type != WFLARE_TYPE;
		// By section, which the index below needs.
		for (u32 i = 0; i < insts && ok; i++) {
			const WorldInst* in = &s_winsts[i];
			ok = (u64)in->first + in->nA + in->nB <= marks &&
			     (i == 0 || in->section >= s_winsts[i - 1].section);
			if (in->section > maxNum)
				maxNum = in->section;
		}
	}
	if (ok) {
		s_wnumbers = maxNum + 1;
		const size_t nums = (size_t)(s_wnumbers > 0 ? s_wnumbers : 1);
		s_wpackOf = (s16*)malloc(nums * sizeof(s16));
		s_wiFirst = (u16*)calloc(nums, sizeof(u16));
		s_wiCount = (u16*)calloc(nums, sizeof(u16));
		s_wreflZ = (float*)malloc((size_t)(flares ? flares : 1) * sizeof(float));
		s_wrefs = (CarLightsInstRef*)calloc(insts ? insts : 1, sizeof(CarLightsInstRef));
		ok = s_wpackOf && s_wiFirst && s_wiCount && s_wreflZ && s_wrefs;
	}
	if (ok) {
		for (u32 i = 0; i < flares; i++)
			s_wreflZ[i] = WREFL_UNSET;
		for (int i = 0; i < s_wnumbers; i++)
			s_wpackOf[i] = -1;
		for (u32 i = 0; i < packs; i++)
			s_wpackOf[s_wpacks[i].section] = (s16)i;
		for (u32 i = 0; i < insts; i++) {
			const u16 sec = s_winsts[i].section;
			if (!s_wiCount[sec])
				s_wiFirst[sec] = (u16)i;
			s_wiCount[sec]++;
		}
		s_wbuf = (FlareVert*)linearAlloc(2 * WORLD_VERTS * sizeof(FlareVert));
		ok = s_wbuf != NULL;
	}
	if (!ok) {
		snprintf(b, sizeof(b), "lights: %s is not a v%d WORLD.NFSF -- the world's lamps draw without flares (rerun tools/flare_pack.py)",
			path, WFLARE_VERSION);
		llog(b);
		carlights_world_close();
		return false;
	}
	snprintf(b, sizeof(b), "lights: %u world flares in %u section packs (%u of a type with no row, not drawn); %u props carrying %u more",
		(unsigned)flares, (unsigned)packs, (unsigned)untyped, (unsigned)insts, (unsigned)marks);
	llog(b);
	return true;
}

// eLightFlare +0x1c for lamp `i`: TopologyCoordinate::GetElevation under it,
// and the lamp's height mirrored in that -- false while it is still to be
// worked out and this frame's probes are spent.
static bool reflZ(u32 i, float* out) {
	float z = s_wreflZ[i];
	if (z == WREFL_UNSET) {
		if (s_wprobes <= 0)
			return false;
		s_wprobes--;
		const float* pos = s_wflares[i].pos;
		float g;
		z = WREFL_NONE;
		if (s_ground && s_ground(pos, &g, s_groundUd) && fabsf(g - pos[2]) >= WREFL_GROUNDED)
			z = g - (pos[2] - g);
		s_wreflZ[i] = z;
	}
	*out = z;
	return true;
}

// eRenderLightFlarePool(view, 0, 0), the main view's: both pools, flareType
// 3, into `w` up to `end`. The pools are filled here as well, from the
// instances in `sections` the last world_draw() drew. PORT: in the sections'
// order and each one's instances in the pack's, where retail fills them in its
// scenery tree's -- which is only seen past WPOOL_SIZE in a pool. Sets `*full`
// when the buffer runs out; returns where it got to.
static FlareVert* drawPool(FlareVert* w, FlareVert* end, const CarNeonView* v, float H,
		const u16* sections, int n, bool* full) {
	int used[2] = { 0, 0 };
	// The flares put in the pool by hand (#497): unlit types, drawn at 1.0.
	for (int i = 0; i < WPOOL_EXTRA; i++) {
		if (!s_wextraOn[i])
			continue;
		if (w + QUAD_VERTS > end) {
			*full = true;
			return w;
		}
		used[0]++;
		static const float none[3] = { 0.0f, 0.0f, 0.0f };
		const FlareParams* const rows[1] = { &kPool[s_wextra[i].type - WPOOL_TYPE] };
		Quad q;
		if (flare(&q, rows, 1, s_wextra[i].pos, none, 1.0f, NULL, v, H, FLARE_POOL, 2, 1.0f)) {
			memcpy(w, q.v, sizeof(q.v));
			w += QUAD_VERTS;
		}
	}
	for (int i = 0; i < n; i++) {
		const int sec = sections[i];
		if (sec >= s_wnumbers || !s_wiCount[sec])
			continue;
		const u32 last = (u32)s_wiFirst[sec] + s_wiCount[sec];
		for (u32 j = s_wiFirst[sec]; j < last; j++) {
			const WorldInst* in = &s_winsts[j];
			const int lod = s_instLod(sec, in->pos, &s_wrefs[j], s_instLodUd);
			if (lod < 0) {
				s_wmissing++;
				if (s_wmissing <= 8) {
					char b[140];
					snprintf(b, sizeof(b), "lights: no instance at (%.1f, %.1f, %.1f) in section %d for its flares (%u so far)",
						in->pos[0], in->pos[1], in->pos[2], sec, (unsigned)s_wmissing);
					llog(b);
				}
				continue;
			}
			if (lod == 0)
				continue;
			const u32 m0 = in->first + (lod == 2 ? in->nA : 0);
			const u32 m1 = m0 + (lod == 2 ? in->nB : in->nA);
			for (u32 k = m0; k < m1; k++) {
				const WorldMark* mk = &s_wmarks[k];
				const int pool = mk->type == WPOOL_SPLIT;
				if (used[pool] >= WPOOL_SIZE)
					continue;
				used[pool]++;
				const int type = WPOOL_TYPE + mk->type;
				if (type >= WPOOL_TYPE + WPOOL_TYPES)
					continue;
				if (pool && type == WPOOL_BLINKER && !s_blink)
					continue;
				float I = 1.0f;
				if (type < WPOOL_UNLIT) {
					if (!s_carSet)
						continue;
					const float d[3] = { mk->pos[0] - s_carPos[0], mk->pos[1] - s_carPos[1],
						mk->pos[2] - s_carPos[2] };
					const float d2 = dot3(d, d);
					if (d2 <= DIST_EPS)
						continue;
					const float c = dot3(d, s_carFwd) / sqrtf(d2);
					if (c < WPOOL_CONE)
						continue;
					I = (c - WPOOL_CONE) / (1.0f - WPOOL_CONE);
				}
				float dir[3] = { 0.0f, 0.0f, 0.0f };
				u32 fl = 2;
				if (mk->type == WPOOL_SPLIT) {
					fl = FLARE_SHAPED;
					const float dx = mk->pos[0] - in->pos[0], dy = mk->pos[1] - in->pos[1];
					const float len = sqrtf(dx * dx + dy * dy);
					if (len > 0.0f) {
						dir[0] = dx / len;
						dir[1] = dy / len;
					}
				}
				if (w + QUAD_VERTS > end) {
					*full = true;
					return w;
				}
				const FlareParams* const rows[1] = { &kPool[type - WPOOL_TYPE] };
				Quad q;
				if (flare(&q, rows, 1, mk->pos, dir, I, NULL, v, H, FLARE_POOL, fl, 1.0f)) {
					memcpy(w, q.v, sizeof(q.v));
					w += QUAD_VERTS;
				}
			}
		}
	}
	return w;
}

void carlights_world_draw(const C3D_Mtx* vp, const CarNeonView* v, const u16* sections,
		int n, u32 flags, const float gain[3]) {
	if (!s_wbuf || !s_shader || !s_texOk[kWorld.tex] || !v || !v->fov || !sections)
		return;
	const bool refl = (flags & CARLIGHTS_REFLECTION) != 0;
	const float H = H_HALF_WIDTH / tanf((float)v->fov * ((float)M_PI / 65536.0f));
	static const FlareParams* const rows[1] = { &kWorld };
	FlareVert* const base = s_wbuf + s_half * WORLD_VERTS + s_wused;
	FlareVert* const end = s_wbuf + s_half * WORLD_VERTS + WORLD_VERTS;
	FlareVert* w = base;
	bool full = false;
	for (int i = 0; i < n && !full; i++) {
		const int k = sections[i] < s_wnumbers ? s_wpackOf[sections[i]] : -1;
		if (k < 0)
			continue;
		const WorldPack* p = &s_wpacks[k];
		float d[3], half[3];
		for (int c = 0; c < 3; c++) {
			d[c] = 0.5f * (p->lo[c] + p->hi[c]) - v->eye[c];
			half[c] = 0.5f * (p->hi[c] - p->lo[c]);
		}
		// The pack's own box, the lamps' and not their reflections' --
		// reflexion or none, as retail asks it.
		if (!boxInView(d, half, v))
			continue;
		for (u32 j = 0; j < p->count; j++) {
			const u32 idx = p->first + j;
			const WorldFlare* fl = &s_wflares[idx];
			if (fl->type != WFLARE_TYPE)
				continue;
			if (w + QUAD_VERTS > end) {
				full = true;
				break;
			}
			Quad q;
			int got;
			if (refl) {
				float rz;
				if (!reflZ(idx, &rz) || rz == WREFL_NONE)
					continue;
				const float pos[3] = { fl->pos[0], fl->pos[1], rz };
				const float dir[3] = { fl->dir[0], fl->dir[1], -fl->dir[2] };
				const float stretch = fl->pos[2] - rz >= WREFL_LOW ? WREFL_STRETCH
					: WREFL_STRETCH * 0.5f;
				got = flare(&q, rows, 1, pos, dir, 1.0f, NULL, v, H, FLARE_REFL_WORLD,
					fl->flags, stretch);
			} else {
				got = flare(&q, rows, 1, fl->pos, fl->dir, 1.0f, NULL, v, H, FLARE_MAIN,
					fl->flags, 1.0f);
			}
			if (got) {
				memcpy(w, q.v, sizeof(q.v));
				w += QUAD_VERTS;
			}
		}
	}
	FlareVert* const lamps = w;
	if (!refl && !full && s_instLod)
		w = drawPool(w, end, v, H, sections, n, &full);
	const int props = (int)(w - lamps) / QUAD_VERTS;
	const int nv = (int)(w - base);
	int* const peak = &s_wpeak[refl];
	if (nv / QUAD_VERTS > *peak * 2 || (full && !s_wfullLogged)) {
		char b[140];
		snprintf(b, sizeof(b), "lights: %d world flares in one %s%s", nv / QUAD_VERTS,
			refl ? "reflection" : "view",
			full ? " -- the buffer is full, the rest are not drawn" : " (a new high)");
		llog(b);
		s_wfullLogged = s_wfullLogged || full;
	}
	if (props > s_wpoolPeak * 2) {
		char b[100];
		snprintf(b, sizeof(b), "lights: %d props' flares in one view (a new high)", props);
		llog(b);
	}
	if (props > s_wpoolPeak)
		s_wpoolPeak = props;
	if (nv / QUAD_VERTS > *peak)
		*peak = nv / QUAD_VERTS;
	if (!nv)
		return;
	s_wused += nv;
	int start[LIGHTS_NTEX] = { 0 }, count[LIGHTS_NTEX] = { 0 };
	count[kWorld.tex] = nv;
	drawQuads(base, nv, start, count, vp, gain);
}

// ------------------------------------------------- the projected headlight
//
// FUN_005c2ae0 (SPEED2.EXE; eUpdateHeadlightCamera, Speed.elf 0x801d7460, on
// the GameCube) puts view 9's camera on the player's car in a race: at the
// car's position (+0x50, the GameCube's +0x48) raised 0.75 m up its own up
// axis, looking along its forward tipped 0.1 down, with world up for up; near
// 1 and far 100 (no matter here: the beam takes x, y and w alone), and a FOV
// of 2.5 rad, 143 degrees. The GameCube's is the same code with its own
// numbers -- 2 m ahead, 4 m up, 0.8 down, 80 degrees -- which throw a short
// pool the car hides from its own chase camera; the PC's lies low and flat
// and reaches out past the bonnet to the clip's 50 m.
//
// The projection is the PC's (FUN_005c7ec0 for views 9 and 10): x' = cot(fov/2)
// x, y' = -cot(0.6002 fov/2) y and w = z, in EA's view space (x right, y down,
// z ahead); then FUN_005bdaa0's bias, 0.5 with
// no scale: s w = x' + w/2, t w = y' + w/2. The texture covers the middle half
// of the frustum each way, and HEADLIGHTS' top row, its bright notched edge,
// falls on the frustum's lower side -- the road just ahead, the way
// 2PLAYERHEADLIGHT1 lies on the road with its notch toward the car. The
// PICA's V runs up the texture, so t here is 1 minus the PC's.
#define HL_EYE_AHEAD    0.0f          // SPEED2.EXE 0x7a27b0
#define HL_EYE_UP       0.75f         // 0x7a27b4
#define HL_DROP         0.1f          // 0x7a27c0
#define HL_FOV          0x65db        // camera +0xc4: ftol(2.5 (0x7a27c4) x 65536/2pi (0x78423c))
#define HL_FOVY_SCALE   0.6002401f    // SPEED2.EXE 0x3f19a956
#define HL_BIAS         0.5f          // 0x80052c / 0x800530
#define HL_CLIP_SCALE   0.005f        // 0x79bfc0
#define HL_CLIP_OFF     0.5f          // 0x784260
// Where the clip ramp is above zero under linear filtering: from texel 32's
// centre to texel 48's, as distances along the beam.
#define HL_NEAR (((HL_CLIP_FIRST - 0.5f) / HL_CLIP_W - HL_CLIP_OFF) / HL_CLIP_SCALE)
#define HL_FAR  (((HL_CLIP_END + 0.5f) / HL_CLIP_W - HL_CLIP_OFF) / HL_CLIP_SCALE)

static void hlRow(float out[4], const float a[3], float ka, const float b[3], float kb,
		const float eye[3], float add) {
	float d = 0.0f;
	for (int c = 0; c < 3; c++) {
		out[c] = a[c] * ka + b[c] * kb;
		d += out[c] * eye[c];
	}
	out[3] = add - d;
}

bool carlights_headlight(const CarLights* l, const float rot[3][3], const float origin[3],
		Model3DHeadlight* out) {
	if (!out || !rot || !origin || !s_texOk[TEX_BEAM] || !s_clipOk)
		return false;
	float eye[3], z[3];
	for (int c = 0; c < 3; c++) {
		eye[c] = origin[c] + HL_EYE_AHEAD * rot[0][c] + HL_EYE_UP * rot[2][c];
		z[c] = rot[0][c];
	}
	z[2] -= HL_DROP;
	const float zl = sqrtf(dot3(z, z));
	if (zl < 1e-4f)
		return false;
	for (int c = 0; c < 3; c++)
		z[c] /= zl;
	// eCreateLookAtMatrix's axes: right = ahead x up, down = ahead x right.
	float x[3] = { z[1], -z[0], 0.0f };
	const float xl = sqrtf(dot3(x, x));
	if (xl < 1e-4f)
		return false;
	x[0] /= xl;
	x[1] /= xl;
	const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2],
	                     z[0] * x[1] - z[1] * x[0] };
	const float half = (float)HL_FOV * ((float)M_PI / 65536.0f);
	const float cx = 1.0f / tanf(half), cy = 1.0f / tanf(half * HL_FOVY_SCALE);
	static const float none[3] = { 0.0f, 0.0f, 0.0f };
	hlRow(out->rows[0], x, cx, z, HL_BIAS, eye, 0.0f);           // s w = x' + w/2
	hlRow(out->rows[1], y, cy, z, HL_BIAS, eye, 0.0f);           // t w = w/2 - y'
	hlRow(out->rows[2], z, HL_CLIP_SCALE, none, 0.0f, eye, HL_CLIP_OFF);
	hlRow(out->rows[3], z, 1.0f, none, 0.0f, eye, 0.0f);         // w
	// The textured frustum: |x| <= w / (2 cx), |y| <= w / (2 cy), and the
	// clip's reach along it. Outside all six the beam's texel is zero.
	const float ax = HL_BIAS / cx, ay = HL_BIAS / cy;
	hlRow(out->planes[0], z, ax, x, 1.0f, eye, 0.0f);
	hlRow(out->planes[1], z, ax, x, -1.0f, eye, 0.0f);
	hlRow(out->planes[2], z, ay, y, 1.0f, eye, 0.0f);
	hlRow(out->planes[3], z, ay, y, -1.0f, eye, 0.0f);
	hlRow(out->planes[4], z, 1.0f, none, 0.0f, eye, -HL_NEAR);
	hlRow(out->planes[5], z, -1.0f, none, 0.0f, eye, HL_FAR);
	// And the corners of the part of it HEADLIGHTS has colour in: |x'| =
	// ax d s_beamExtS, |y'| = ay d s_beamExtT at d = HL_NEAR and HL_FAR along
	// z. model3d.c scissors the beam's pass to their box on screen (#412).
	for (int i = 0; i < 8; i++) {
		const float d = (i & 4) ? HL_FAR : HL_NEAR;
		const float ex = ax * s_beamExtS, ey = ay * s_beamExtT;
		const float sx = (i & 1) ? ex : -ex, sy = (i & 2) ? ey : -ey;
		for (int c = 0; c < 3; c++)
			out->corners[i][c] = eye[c] + d * (z[c] + sx * x[c] + sy * y[c]);
	}
	// HeadlightColor is the bulb / 128 (0x787af0), the whole +0x380 as
	// carlights_dynamic() reads it; the TEV takes half of it, the bulb / 255
	// to within a step, and doubles back (model3d.c hlStages()).
	const u8* bulb = l && l->set ? l->beam : kDefaultBulb;
	out->colour = 0xFF000000u | ((u32)bulb[2] << 16) | ((u32)bulb[1] << 8) | bulb[0];
	out->beam = &s_tex[TEX_BEAM];
	out->clip = &s_clip;
	return true;
}
