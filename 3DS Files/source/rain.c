// The weather (TASKS.md #315). See rain.h for the retail functions; the
// comments here name the Rain fields (GameCube offsets) each piece keeps.
#include "rain.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shaders.h"
#include "texpack.h"

#define RAIN_POINTS 400        // Init: +0x4080
#define RAIN_TYPES  6          // 0 rain, 1-3 other kinds, 4, 5 idle
#define RAIN_JITTER 10

// The r13 tunables, as shipped.
static const float kBoundX = 20.0f, kBoundY = 7.0f, kBoundZ = 6.0f;   // precipBound
static const float kAheadX = 8.0f, kAheadY = 0.0f, kAheadZ = 0.0f;    // precipAhead
static const float kRainX = 0.02f, kRainY = 0.02f, kRainZ = 0.03f;    // RAINX/Y/Z
static const float kRainZConst = 0.45f;                               // RAINZconstant
static const float kWindEffect = 0.3f;                                // RAINwindEffect
static const float kRadiusX = 0.03f, kRadiusY = 0.7f;                 // RAINRadiusX/Y
static const float kDriveFactor = -0.02f;                             // driveFactor
static const float kHeadlights = 0.96f;                               // RainInTheHeadlights
static const float kWeatherChange = 1.0f;                             // WEATHERchange
static const float kBaseDampness = 1.0f;                              // BaseDampness
static const float kMaxWind = 25.0f;                                  // maxWindEffect
static const float kPrevailing = 0.01f;                               // PrevailingMult
static const float kCoverAhead = 9.5f;                                // 0x8033E870
static const float kYawRotate = 5e-5f;                                // 0x8033E874

typedef struct {
	float pos[2][3];   // +0x200, the two buffers +0x1F8 / +0x1FC pick
	u8 jitter;         // +0x220, one of the ten fall vectors
	u8 state;          // +0x224: 0 falling, 1 landed, 2 killed under cover
	u8 type;           // +0x225
	u8 variant;        // +0x226: the wind table's column
	u8 lit;            // +0x227: in the headlights
} Drop;

typedef struct {
	float pos[3];
	u8 rgba[4];
	float uv[2];
} RainVert;

// OnScreenRain (#406): the drops on the lens, in screen fractions (x right,
// y down), GameCube ctor 0x80252F78 / Update 0x8025303C.
#define LENS_DROPS 20
#define LENS_GRID  8                                   // cells a side (PORT)
#define LENS_GV    ((LENS_GRID + 1) * (LENS_GRID + 1)) // vertices a drop
#define LENS_GI    (LENS_GRID * LENS_GRID * 6)         // indices a drop
static const float kDripSpeed = 0.2f;    // DripSpeed
static const float kSpeedMod = 1e-4f;    // SpeedMod
static const float kDropSize = 0.12f;    // FUN_005C7AB0: r4 * 0.12
static const float kDropAspect = 0.8f;   // PORT: PC's 4:3 drop on the 5:3 panel

typedef struct {
	float x, y, life, lifeMax, r4, drip;
} LensDrop;

static struct {
	void (*log)(const char*);
	bool open;
	const ParamLayer* rainLayer;
	const ParamLayer* windLayer;
	const ParamLayer* fogLayer;
	const ParamLayer* normalFogLayer;
	const ParamLayer* blendDistLayer;
	// CalculateFogInfo's accessors (#407): FogAccessor (Normal Fog) and
	// NightRainFogAccessor, both ParameterAccessorBlendByDistance.
	ParamAccessor normalFog, rainFog;
	const TrackPaths* paths;

	Drop drop[RAIN_POINTS];
	int cur, prev;                 // +0x1F8, +0x1FC
	int count[RAIN_TYPES];         // +0x4084
	int target[RAIN_TYPES];        // +0x409C
	int active;                    // +0x1F4
	float jitter[RAIN_JITTER][3];  // +0x4458

	float intensity;               // +0x1F0
	float desired;                 // +0x43E0
	float dampTarget;              // +0x43E4
	float damp;                    // +0x43E8
	float wave;                    // IsRainingAt's last, for the log
	int   rainSet;

	float inv[3][3];               // +0x439C: world to the car's frame
	u16   yaw;                     // the render matrix's RotZ
	s16   prevYaw;                 // r13-0x5CC4
	bool  haveYaw;
	float carPos[3];
	float vel[3];                  // this tick's camera term, car frame
	float stretch;                 // +0x43DC
	bool  coverCar, coverAhead;    // +0x1E4, +0x1E8
	float camLocal[3];             // Render's camera direction, car frame
	float prevEye[3];
	bool  haveEye;

	float prevailing[3];           // +0x4440
	float wind[3];                 // +0x421C
	float windTarget[3];           // +0x422C
	float windWait, windChange;    // +0x4240, +0x423C
	int   windState;               // r13-0x5CA4
	float windLerp;                // r13-0x5CA0

	C3D_Tex tex;
	Tex3DS_SubTexture sub;
	bool texOk;
	RainVert* vbuf[2];
	u16* ibuf;
	int frame;
	int lastQuads;
	u32 seed;

	// The lens drops (#406).
	LensDrop lens[LENS_DROPS];
	int lensCount;                 // 20 while it rains on the car, else 0
	float camSpeed;                // |camera velocity|, cam +0x1E0
	bool lensOk;                   // RAINDROPPC sampled and buffers up
	float lensTab[LENS_GV][3];     // RAINDROPPC's G, B, A at each grid vertex
	RainVert* lensVb[2][2];        // [frame][eye]
	u16* lensIb;
	int lensFrame;
	int lensDrawn;
	bool lensHeld;                 // DbgLensHold: OverRide's freeze
} s;

static DVLE_s*         s_dvle;
static shaderProgram_s s_prog;
static int             s_uMvp = -1;
static bool            s_progReady;

void rain_set_log(void (*sink)(const char* msg)) { s.log = sink; }

static void rlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void rlog(const char* fmt, ...) {
	if (!s.log)
		return;
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	s.log(b);
}

// bRandom(f): [0, f).
static float rnd(float f) {
	s.seed = s.seed * 1664525u + 1013904223u;
	return (float)(s.seed >> 8) * (1.0f / 16777216.0f) * f;
}

static bool initProg(void) {
	if (s_progReady)
		return true;
	s_dvle = shaders_get(SHADER_PARTICLE);
	if (!s_dvle)
		return false;
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, s_dvle);
	s_uMvp = shaderInstanceGetUniformLocation(s_prog.vertexShader, "mvp");
	if (s_uMvp < 0) {
		shaderProgramFree(&s_prog);
		return false;
	}
	s_progReady = true;
	return true;
}

// Rain::Init(type): every drop of `type`, spread through the box.
static void rainInit(int type) {
	memset(s.count, 0, sizeof(s.count));
	memset(s.target, 0, sizeof(s.target));
	s.cur = 0;
	s.prev = 1;
	s.count[type] = RAIN_POINTS;
	for (int i = 0; i < RAIN_POINTS; i++) {
		Drop* d = &s.drop[i];
		d->type = (u8)type;
		d->state = 0;
		d->variant = (u8)(rnd(2.0f) >= 1.0f);
		d->lit = 0;
		d->pos[0][0] = kAheadX + (rnd(kBoundX) - kBoundX * 0.5f);
		d->pos[0][1] = kAheadY + (rnd(kBoundY) - kBoundY * 0.5f);
		d->pos[0][2] = kAheadZ + rnd(kBoundZ);
		memcpy(d->pos[1], d->pos[0], sizeof(d->pos[0]));
		d->jitter = (u8)(i % RAIN_JITTER);
	}
	for (int j = 0; j < RAIN_JITTER; j++) {
		s.jitter[j][0] = rnd(kRainX) - kRainX * 0.5f;
		s.jitter[j][1] = rnd(kRainY) - kRainY * 0.5f;
		s.jitter[j][2] = -rnd(kRainZ) - kRainZConst;
	}
	memset(s.wind, 0, sizeof(s.wind));
	memset(s.windTarget, 0, sizeof(s.windTarget));
	s.windWait = s.windChange = 0.0f;
	s.windState = 0;
	s.windLerp = 0.0f;
}

// OnScreenRain's ctor.
static void lensInit(void) {
	for (int i = 0; i < LENS_DROPS; i++) {
		LensDrop* d = &s.lens[i];
		d->x = d->y = d->life = 0.0f;
		d->lifeMax = 0.3f + rnd(1.5f);
		d->r4 = rnd(1.0f);
		d->drip = 0.02f + rnd(0.1f);
	}
	s.lensCount = 0;
	s.camSpeed = 0.0f;
}

// RAINDROPPC (bStringHash 0xD13A0471, GlobalTexturesPC.tpk), 16x16: G and B
// the refracted screen position across the drop, A its round mask. The PICA
// has no dependent reads, so rain_drop.efx's lookup is done here once, at
// each vertex of the drop's grid: bilinear, clamped, D3D's v (0 = the image's
// top row) running up the quad from its bottom edge.
static void lensLoad(const TexPack* pack) {
	s.lensOk = false;
	const TexPackEntry* ent = texpack_find(pack, 0xD13A0471u);
	u16 w = 0, h = 0;
	u8* px = ent ? texpack_decode_rgba(ent, &w, &h) : NULL;
	if (!px)
		return;
	for (int j = 0; j <= LENS_GRID; j++) {
		for (int i = 0; i <= LENS_GRID; i++) {
			float fx = (float)i / LENS_GRID * w - 0.5f;
			float fy = (float)j / LENS_GRID * h - 0.5f;
			if (fx < 0.0f) fx = 0.0f;
			if (fy < 0.0f) fy = 0.0f;
			if (fx > w - 1) fx = (float)(w - 1);
			if (fy > h - 1) fy = (float)(h - 1);
			const int x0 = (int)fx, y0 = (int)fy;
			const int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
			const float ax = fx - x0, ay = fy - y0;
			float* out = s.lensTab[j * (LENS_GRID + 1) + i];
			for (int c = 0; c < 3; c++) {
				const int ch = c + 1;   // G, B, A
				const float a = px[(y0 * w + x0) * 4 + ch], b = px[(y0 * w + x1) * 4 + ch];
				const float cc = px[(y1 * w + x0) * 4 + ch], dd = px[(y1 * w + x1) * 4 + ch];
				const float top = a + (b - a) * ax, bot = cc + (dd - cc) * ax;
				out[c] = (top + (bot - top) * ay) * (1.0f / 255.0f);
			}
		}
	}
	free(px);
	s.lensOk = true;
}

static bool lensBuffers(void) {
	for (int f = 0; f < 2; f++)
		for (int e = 0; e < 2; e++) {
			s.lensVb[f][e] = (RainVert*)linearAlloc(LENS_DROPS * LENS_GV * sizeof(RainVert));
			if (!s.lensVb[f][e])
				return false;
		}
	s.lensIb = (u16*)linearAlloc(LENS_DROPS * LENS_GI * sizeof(u16));
	if (!s.lensIb)
		return false;
	for (int d = 0; d < LENS_DROPS; d++) {
		u16* ix = s.lensIb + d * LENS_GI;
		const int base = d * LENS_GV;
		for (int j = 0; j < LENS_GRID; j++)
			for (int i = 0; i < LENS_GRID; i++) {
				const u16 a = (u16)(base + j * (LENS_GRID + 1) + i);
				const u16 b = (u16)(a + 1), c = (u16)(a + LENS_GRID + 1), e = (u16)(c + 1);
				*ix++ = a; *ix++ = b; *ix++ = e;
				*ix++ = a; *ix++ = e; *ix++ = c;
			}
	}
	GSPGPU_FlushDataCache(s.lensIb, LENS_DROPS * LENS_GI * sizeof(u16));
	return true;
}

static void lensFree(void) {
	for (int f = 0; f < 2; f++)
		for (int e = 0; e < 2; e++) {
			if (s.lensVb[f][e])
				linearFree(s.lensVb[f][e]);
			s.lensVb[f][e] = NULL;
		}
	if (s.lensIb)
		linearFree(s.lensIb);
	s.lensIb = NULL;
	s.lensOk = false;
}

bool rain_open(const char* globalBunPath, const ParamMaps* pm, const TrackPaths* paths) {
	rain_close();
	s.seed = 0x5EED1234u;
	s.rainLayer = pm ? parammaps_layer(pm, "Rain") : NULL;
	s.windLayer = pm ? parammaps_layer(pm, "Wind") : NULL;
	s.fogLayer = pm ? parammaps_layer(pm, "Night Raining Fog") : NULL;
	s.normalFogLayer = pm ? parammaps_layer(pm, "Normal Fog") : NULL;
	s.blendDistLayer = pm ? parammaps_layer(pm, "Fog Blend Distance") : NULL;
	paramaccessor_init(&s.normalFog, s.normalFogLayer);
	paramaccessor_init(&s.rainFog, s.fogLayer);
	s.paths = paths;
	// SetRainBase: the road starts wet, every drop idle.
	s.damp = 1.0f;
	s.dampTarget = 0.0f;
	s.intensity = s.desired = 0.0f;
	s.haveYaw = s.haveEye = false;
	s.rainSet = -1;
	rainInit(5);
	lensInit();
	s.open = true;

	s.texOk = false;
	s.lensOk = false;
	TexPack pack;
	if (globalBunPath && texpack_load_shared(globalBunPath, &pack)) {
		lensLoad(&pack);
		// bStringHash("RAINDROP"), in Global\GlobalTextures.tpk.
		const TexPackEntry* ent = texpack_find(&pack, 0x7415EA5Eu);
		if (ent && texpack_decode_entry(ent, 0, false, &s.tex, &s.sub)) {
			C3D_TexSetFilter(&s.tex, GPU_LINEAR, GPU_LINEAR);
			C3D_TexSetWrap(&s.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			s.texOk = true;
		}
		texpack_release(&pack);
	}
	if (s.texOk && initProg()) {
		s.vbuf[0] = (RainVert*)linearAlloc(RAIN_POINTS * 4 * sizeof(RainVert));
		s.vbuf[1] = (RainVert*)linearAlloc(RAIN_POINTS * 4 * sizeof(RainVert));
		s.ibuf = (u16*)linearAlloc(RAIN_POINTS * 6 * sizeof(u16));
		if (s.vbuf[0] && s.vbuf[1] && s.ibuf) {
			for (int q = 0; q < RAIN_POINTS; q++) {
				u16* ix = s.ibuf + q * 6;
				const u16 b = (u16)(q * 4);
				ix[0] = b; ix[1] = (u16)(b + 1); ix[2] = (u16)(b + 2);
				ix[3] = b; ix[4] = (u16)(b + 2); ix[5] = (u16)(b + 3);
			}
			GSPGPU_FlushDataCache(s.ibuf, RAIN_POINTS * 6 * sizeof(u16));
		} else {
			rain_close();
			s.open = true;   // still simulated, never drawn
		}
	} else if (s.texOk) {
		C3D_TexDelete(&s.tex);
		s.texOk = false;
	}
	if (s.lensOk && (!initProg() || !lensBuffers()))
		lensFree();
	rlog("rain: open -- RAINDROP %s, RAINDROPPC %s, Rain layer %s (%d sets), Wind %s, rain fog %s, cover %s",
		s.texOk ? "loaded" : "MISSING (not drawn)", s.lensOk ? "loaded" : "MISSING (no lens drops)",
		s.rainLayer ? "yes" : "NO", s.rainLayer ? parammaps_set_count(s.rainLayer) : 0,
		s.windLayer ? "yes" : "no", s.fogLayer ? "yes" : "no", paths ? "yes" : "no");
	return s.texOk;
}

void rain_close(void) {
	if (s.texOk)
		C3D_TexDelete(&s.tex);
	s.texOk = false;
	for (int i = 0; i < 2; i++) {
		if (s.vbuf[i])
			linearFree(s.vbuf[i]);
		s.vbuf[i] = NULL;
	}
	if (s.ibuf)
		linearFree(s.ibuf);
	s.ibuf = NULL;
	lensFree();
	s.open = false;
}

// IsRainingAt: y is not used.
static float isRainingAt(float x, float worldTime) {
	const float deg = x / 50.0f + worldTime;
	const u16 a = (u16)(((int)(deg * 65536.0f)) / 360);
	const float c = cosf((float)a * (6.28318531f / 65536.0f));
	return fabsf(c * 0.5f + 0.5f);
}

static float desiredAt(float x, float y, float worldTime) {
	s.wave = isRainingAt(x, worldTime);
	if (!s.rainLayer)
		return 0.0f;
	const int set = parammaps_set_at(s.rainLayer, x, y);
	s.rainSet = set;
	if (s.wave <= parammaps_field(s.rainLayer, set, 1))
		return parammaps_field(s.rainLayer, set, 0);
	return 0.0f;
}

// SetIntensity and Update's Change calls: idle drops to type 5, the rest to
// type 0 (Change(1, 0), Change(0, 1.0), Change(4, 0)).
static void setIntensity(float I) {
	s.intensity = I;
	int idle = (int)((1.0f - I) * 400.0f);
	if (idle < 0) idle = 0;
	if (idle > RAIN_POINTS) idle = RAIN_POINTS;
	s.active = RAIN_POINTS - idle;
	memset(s.target, 0, sizeof(s.target));
	s.target[5] = idle;
	s.target[0] = s.active;
}

static inline void toLocal(float out[3], const float v[3]) {
	float t[3];
	for (int r = 0; r < 3; r++)
		t[r] = s.inv[r][0] * v[0] + s.inv[r][1] * v[1] + s.inv[r][2] * v[2];
	memcpy(out, t, sizeof(t));
}

// Rain::Wind: the prevailing wind from the Wind layer (field 1 its heading,
// degrees), into the car's frame; then a gust every 1-6 s, eased in for 1 s.
static void wind(float dt) {
	if (s.windLayer) {
		const int set = parammaps_set_at(s.windLayer, s.carPos[0], s.carPos[1]);
		const float ang = parammaps_field(s.windLayer, set, 1);
		const u16 a = (u16)(((int)(ang * 65536.0f)) / 360);
		const float r = (float)a * (6.28318531f / 65536.0f);
		s.prevailing[0] = sinf(r);
		s.prevailing[1] = cosf(r);
		s.prevailing[2] = 0.0f;
	}
	for (int k = 0; k < 3; k++)
		s.prevailing[k] *= kPrevailing;
	toLocal(s.prevailing, s.prevailing);
	if (s.windState == 1) {
		s.wind[1] += (s.windTarget[1] - s.wind[1]) * dt;
		s.wind[0] += (s.windTarget[0] - s.wind[0]) * dt;
		s.windLerp += dt;
		if (s.windLerp >= 1.0f) {
			s.windState = 0;
			s.windLerp = 0.0f;
		}
	} else if (s.windState == 0) {
		s.windWait += dt;
		if (s.windChange <= s.windWait)
			s.windState = 2;
	} else if (s.windState == 2) {
		s.windChange = rnd(5.0f) + 1.0f;
		s.windWait = 0.0f;
		s.windState = 1;
		s.windTarget[0] = (rnd(0.05f) - 0.025f) * kMaxWind + s.prevailing[0];
		s.windTarget[1] = (rnd(0.05f) - 0.025f) * kMaxWind + s.prevailing[1];
		s.windTarget[2] = s.prevailing[2];
	}
}

// FindZone's bitmask overload goes on from `after` through the one list,
// which is sorted by type: a later type's zone leaves no earlier type to find.
static const TrackPathZone* coverFind(float x, float y, u32 type, const TrackPathZone* after) {
	if (after && after->type > type)
		return NULL;
	return trackpath_find(s.paths, x, y, type, after && after->type == type ? after : NULL);
}

static void seed(Drop* d) {
	d->pos[s.cur][2] = kAheadZ + kBoundZ;
	memcpy(d->pos[s.prev], d->pos[s.cur], sizeof(d->pos[0]));
}

// Rain::Update.
static void rainStep(float dt, const RainInput* in, float k) {
	// The car's matrix, translation zeroed and inverted: world to car frame.
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			s.inv[r][c] = in->carRot[r][c];
	memcpy(s.carPos, in->carPos, sizeof(s.carPos));
	wind(dt);

	// The camera's motion through the car's frame, the streaks' stretch.
	float camVel[3] = { 0.0f, 0.0f, 0.0f };
	if (s.haveEye && dt > 0.0f) {
		for (int i = 0; i < 3; i++)
			camVel[i] = (in->camEye[i] - s.prevEye[i]) / dt;
		const float v2 = camVel[0] * camVel[0] + camVel[1] * camVel[1] + camVel[2] * camVel[2];
		if (v2 > 150.0f * 150.0f)   // a teleport, not motion
			camVel[0] = camVel[1] = camVel[2] = 0.0f;
	}
	memcpy(s.prevEye, in->camEye, sizeof(s.prevEye));
	s.haveEye = true;
	s.camSpeed = sqrtf(camVel[0] * camVel[0] + camVel[1] * camVel[1] + camVel[2] * camVel[2]);
	if (in->playerDriven) {
		toLocal(s.vel, camVel);
		s.vel[0] *= kDriveFactor;
		s.vel[2] *= kDriveFactor;
		s.vel[1] = 0.0f;
		const float l2 = s.vel[2] * s.vel[2] + s.vel[0] * s.vel[0];
		s.stretch = l2 > 0.0f ? sqrtf(l2) : 0.0f;
		if (s.stretch > 1.0f) {
			s.stretch = 1.0f;
			const float n = 1.0f / sqrtf(l2);
			for (int i = 0; i < 3; i++)
				s.vel[i] *= n;
		}
	} else {
		s.vel[0] = s.vel[1] = s.vel[2] = 0.0f;
	}
	toLocal(s.camLocal, in->camFwd);

	// The render frame: the car's heading alone, as retail works it out.
	float fwd[3] = { in->carRot[0][0], in->carRot[0][1], in->carRot[0][2] };
	float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
	if (fl < 1e-6f) fl = 1.0f;
	for (int i = 0; i < 3; i++)
		fwd[i] /= fl;
	float sx = fwd[0] > 1.0f ? 1.0f : fwd[0] < -1.0f ? -1.0f : fwd[0];
	s16 yaw = (s16)(0x4000 - (s16)(int)(asinf(sx) * (32768.0f / 3.14159265f)));
	if (fwd[1] < 0.0f)
		yaw = (s16)(-0x44C - yaw);   // retail's own, not the mirror image
	s.yaw = (u16)yaw;
	float dyaw = 0.0f;
	if (s.haveYaw) {
		dyaw = (float)(u16)(s.prevYaw - yaw);
		if (dyaw >= 32768.0f)
			dyaw -= 65536.0f;
		const float lim = 400.0f * k;
		if (dyaw > lim) dyaw = lim;
		if (dyaw < -lim) dyaw = -lim;
	}
	s.prevYaw = yaw;
	s.haveYaw = true;

	const int t = s.prev;
	s.prev = s.cur;
	s.cur = t;
	const float maxX = kAheadX + kBoundX * 0.5f, minX = kAheadX - kBoundX * 0.5f;
	const float maxY = kAheadY + kBoundY * 0.5f, minY = kAheadY - kBoundY * 0.5f;

	bool match = true;
	for (int i = 0; i < RAIN_TYPES; i++)
		match = match && s.count[i] == s.target[i];

	// Cover: the tunnel and overpass zones over the car, and 9.5 m ahead.
	s.coverCar = s.coverAhead = false;
	const float* p = in->carPos;
	const float ahead[2] = { fwd[0] * kCoverAhead + p[0], fwd[1] * kCoverAhead + p[1] };
	const TrackPathZone* z = NULL;
	if (s.paths) {
		z = coverFind(p[0], p[1], TPZ_TUNNEL, NULL);
		if (z && p[2] < z->entryZ) s.coverCar = true;
		z = coverFind(p[0], p[1], TPZ_OVERPASS, z);
		if (z && p[2] < z->entryZ) s.coverCar = true;
		z = coverFind(ahead[0], ahead[1], TPZ_TUNNEL, z);
		if (z && p[2] < z->entryZ) s.coverAhead = true;
		z = coverFind(ahead[0], ahead[1], TPZ_OVERPASS, z);
		if (z && p[2] < z->entryZ) s.coverAhead = true;
	}

	const float add[3] = { s.vel[0] * k, s.vel[1] * k, s.vel[2] * k };
	for (int i = 0; i < RAIN_POINTS; i++) {
		Drop* d = &s.drop[i];
		float* cur = d->pos[s.cur];
		float* prv = d->pos[s.prev];
		if (d->state == 0) {
			// The turn since last tick, then the motion, wind and fall.
			prv[1] += prv[0] * dyaw * kYawRotate;
			const float we = (d->type == 0 ? kWindEffect : 0.0f) * (d->variant ? 0.5f : 1.0f);
			const float* jt = s.jitter[d->jitter];
			for (int c = 0; c < 3; c++)
				cur[c] = prv[c] + add[c] + (s.wind[c] * we + jt[c]) * k;
			if (cur[2] >= 0.0f) {
				if (cur[0] > maxX) {
					cur[0] -= kBoundX;
					prv[0] -= kBoundX;
				} else if (cur[0] < minX) {
					cur[0] += kBoundX;
					prv[0] += kBoundX;
				} else if (cur[1] > maxY) {
					cur[1] -= kBoundY;
					prv[1] -= kBoundY;
				} else if (cur[1] < minY) {
					cur[1] += kBoundY;
					prv[1] += kBoundY;
				}
			} else {
				d->state = 1;
			}
			d->lit = 0;
			if (cur[0] >= 0.0f) {
				const float l = sqrtf(cur[0] * cur[0] + cur[1] * cur[1] + cur[2] * cur[2]);
				if (l > 1e-6f && cur[0] / l >= kHeadlights)
					d->lit = 1;
			}
		} else if (d->state < 3) {
			if (!match) {
				for (int ty = 0; ty < RAIN_TYPES; ty++) {
					if (ty != d->type && s.count[ty] < s.target[ty]) {
						s.count[d->type]--;
						d->type = (u8)ty;
						s.count[ty]++;
						break;
					}
				}
			}
			if ((unsigned)(d->type - 4) > 1u) {
				d->lit = 0;
				if (d->state == 1)
					seed(d);
				d->state = 0;
			}
		}
		if ((s.coverAhead && d->pos[s.cur][0] > kCoverAhead) ||
		    (s.coverCar && d->pos[s.cur][0] <= kCoverAhead)) {
			s.count[d->type]--;
			d->type = 5;
			s.count[5]++;
			d->state = 2;
		}
	}
}

// CalculateFogInfo's captures (PC 0x614300; GameCube 0x80253660), at the
// view's car (GetXYviewCar). The fog layers are blended by distance over 50 m
// (0x42480000); the rain fog's over the Fog Blend Distance layer's field 0
// (50 everywhere in L4RA, and 50 without the layer), and only while it rains.
// PC weights Normal Fog by GetSunPercent (0x60FA80), which is 1 outside the
// horizon change (0x8A1C88 = 0, 0x8A1C90 = 0), so Night Normal Fog takes no
// part in play. (The GameCube's GetSunPercent is 0 and takes Night Normal Fog,
// but there the result only reaches eClearHorizon, never the world.)
static void fogCapture(float x, float y) {
	paramaccessor_capture(&s.normalFog, x, y, 50.0f);
	if (s.intensity > 0.0f) {
		float dist = 50.0f;
		if (s.blendDistLayer)
			dist = parammaps_field(s.blendDistLayer, parammaps_set_at(s.blendDistLayer, x, y), 0);
		paramaccessor_capture(&s.rainFog, x, y, dist);
	}
}

// OnScreenRain::Update, at the head of UpdateAndRender: last frame's cover and
// intensity, the world clock's step. A drop out of life starts again
// somewhere new; a live one drips down and slides out from the middle with the
// camera's speed (per tick, as the streaks).
static void lensUpdate(float dt, float k) {
	s.lensCount = (!s.coverCar && s.intensity != 0.0f) ? LENS_DROPS : 0;
	for (int i = 0; i < s.lensCount && !s.lensHeld; i++) {
		LensDrop* d = &s.lens[i];
		d->life -= dt;
		if (d->life > 0.0f) {
			d->y += d->drip * dt * kDripSpeed;
			if (d->y > 1.0f || d->x > 1.0f || d->x < 0.0f) {
				d->life = 0.0f;
			} else {
				const float nx = d->x - 0.5f, ny = d->y - 0.1f;
				const float l = sqrtf(nx * nx + ny * ny);
				if (l > 1e-6f)
					d->x += nx / l * s.camSpeed * kSpeedMod * k;
			}
		} else {
			d->life = d->lifeMax;
			d->x = rnd(1.0f);
			d->y = rnd(1.0f);
		}
	}
	s.lensFrame ^= 1;
}

void rain_update(float dt, const RainInput* in, float forceIntensity) {
	if (!s.open || !in || !(fabsf(dt) > 0.01f))
		return;
	float k = dt * 60.0f;
	if (k > 4.0f)
		k = 4.0f;
	lensUpdate(dt, k);
	// The dampness: wet while it rains, drying slowly after.
	s.dampTarget = s.intensity != 0.0f ? kBaseDampness : 0.0f;
	const float rate = s.damp < s.dampTarget ? 0.05f : 0.0015f;
	s.damp += (s.dampTarget - s.damp) * (1.0f - powf(1.0f - rate, k));
	s.desired = forceIntensity >= 0.0f ? forceIntensity
		: desiredAt(in->carPos[0], in->carPos[1], in->worldTime);
	float I = s.intensity + (s.desired - s.intensity) / ((1.0f / dt) * kWeatherChange);
	if (s.desired == 0.0f && I < 0.01f)
		I = 0.0f;
	if (I > 1.0f)
		I = 1.0f;
	setIntensity(I);
	fogCapture(in->carPos[0], in->carPos[1]);
	rainStep(dt, in, k);
}

void rain_draw(const C3D_Mtx* vp) {
	s.lastQuads = 0;
	if (!s.open || !s.texOk || !s_progReady || !s.vbuf[0] || s.count[0] == 0)
		return;
	RainVert* vb = s.vbuf[s.frame];
	const float c = cosf((float)s.yaw * (6.28318531f / 65536.0f));
	const float sn = sinf((float)s.yaw * (6.28318531f / 65536.0f));
	const float u0 = s.sub.left, u1 = s.sub.right, v0 = s.sub.top, v1 = s.sub.bottom;
	int quads = 0;
	for (int i = 0; i < RAIN_POINTS; i++) {
		const Drop* d = &s.drop[i];
		if (d->state != 0 || d->type > 3)
			continue;
		const float* cur = d->pos[s.cur];
		const float* prv = d->pos[s.prev];
		float dir[3] = { prv[0] - cur[0], prv[1] - cur[1], prv[2] - cur[2] };
		const float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		if (dl < 1e-6f)
			continue;
		for (int k = 0; k < 3; k++)
			dir[k] /= dl;
		const float* cl = s.camLocal;
		float side[3] = { cl[1] * dir[2] - cl[2] * dir[1], cl[2] * dir[0] - cl[0] * dir[2],
		                  cl[0] * dir[1] - cl[1] * dir[0] };
		const float along = kRadiusY + s.stretch;
		float corner[4][3];
		for (int k = 0; k < 3; k++) {
			side[k] *= kRadiusX;
			const float a = dir[k] * along;
			corner[0][k] = cur[k] - (side[k] + a);
			corner[1][k] = cur[k] + (side[k] - a);
			corner[2][k] = cur[k] + (side[k] + a);
			corner[3][k] = cur[k] + (a - side[k]);
		}
		// The two ePolys of the Rain: lit 0x80 grey at 0x80, the rest
		// 100 grey at 0x1C.
		const u8 g = d->lit ? 0x80 : 100, al = d->lit ? 0x80 : 0x1C;
		static const float kU[4] = { 0, 1, 1, 0 }, kV[4] = { 0, 0, 1, 1 };
		RainVert* q = vb + quads * 4;
		for (int v = 0; v < 4; v++) {
			const float* l = corner[v];
			q[v].pos[0] = s.carPos[0] + l[0] * c - l[1] * sn;
			q[v].pos[1] = s.carPos[1] + l[0] * sn + l[1] * c;
			q[v].pos[2] = s.carPos[2] + l[2];
			q[v].rgba[0] = q[v].rgba[1] = q[v].rgba[2] = g;
			q[v].rgba[3] = al;
			q[v].uv[0] = kU[v] ? u1 : u0;
			q[v].uv[1] = kV[v] ? v1 : v0;
		}
		quads++;
	}
	s.lastQuads = quads;
	if (!quads)
		return;
	GSPGPU_FlushDataCache(vb, (u32)quads * 4 * sizeof(RainVert));

	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, vb, sizeof(RainVert), 3, 0x210);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uMvp, vp);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	// As afx.c: the world's "colour twice texture times vertex, alpha not".
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	// RAINDROP's alphaBlendType is 1: source over.
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_TexBind(0, &s.tex);
	C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, s.ibuf);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_1);
	s.frame ^= 1;
}

void rain_lens_hold(bool on) { s.lensHeld = on; }
bool rain_lens_held(void) { return s.lensHeld; }

bool rain_lens_active(void) {
	if (!s.open || !s.lensOk)
		return false;
	for (int i = 0; i < s.lensCount; i++)
		if (s.lens[i].life > 0.0f && s.lens[i].r4 * kDropSize > 1e-3f)
			return true;
	return false;
}

// Screen pixel (400x240, y down) to the top target's clip space, as smear.c's
// toClip (Mtx_OrthoTilt's map).
static inline void lensClip(float sx, float sy, float* cx, float* cy) {
	*cx = 1.0f - sy / 120.0f;
	*cy = 1.0f - sx / 200.0f;
}

// FUN_005C7AB0 with rain_drop.efx. Per drop: alpha 3 * life / lifeMax, size
// s = r4 * 0.12; the quad from (x, y) right s / 2 and up s / 2 of the screen;
// at each texel d of RAINDROPPC it shows the frame at (x - s / 4 + d.g s,
// y + s / 4 - d.b s), times 1.2, at alpha d.a times the drop's.
void rain_lens_draw(const C3D_Tex* frame, int eye) {
	s.lensDrawn = 0;
	if (!frame || !rain_lens_active() || !s_progReady)
		return;
	RainVert* vb = s.lensVb[s.lensFrame][eye ? 1 : 0];
	// smear.c's map0: the eye's 240 columns of 256, its 400 rows at the top
	// of 512.
	const float mu = 240.0f / 256.0f, mv = 400.0f / 512.0f, ov = 1.0f - 400.0f / 512.0f;
	int n = 0;
	for (int i = 0; i < s.lensCount; i++) {
		const LensDrop* d = &s.lens[i];
		const float sz = d->r4 * kDropSize;
		if (!(d->life > 0.0f) || sz <= 1e-3f)
			continue;
		float A = 3.0f * d->life / d->lifeMax;
		if (A > 1.0f) A = 1.0f;
		RainVert* q = vb + n * LENS_GV;
		const float sw = sz * kDropAspect;
		for (int j = 0; j <= LENS_GRID; j++) {
			const float gv = (float)j / LENS_GRID;
			for (int k = 0; k <= LENS_GRID; k++) {
				const float gu = (float)k / LENS_GRID;
				const float* t = s.lensTab[j * (LENS_GRID + 1) + k];
				RainVert* v = &q[j * (LENS_GRID + 1) + k];
				lensClip((d->x + gu * sw * 0.5f) * 400.0f, (d->y - gv * sz * 0.5f) * 240.0f,
					&v->pos[0], &v->pos[1]);
				v->pos[2] = -0.5f;
				float cx, cy;
				lensClip((d->x + (t[0] * sz - sz * 0.25f) * kDropAspect) * 400.0f,
					(d->y + sz * 0.25f - t[1] * sz) * 240.0f, &cx, &cy);
				if (cx < -1.0f) cx = -1.0f;
				if (cx > 1.0f) cx = 1.0f;
				if (cy < -1.0f) cy = -1.0f;
				if (cy > 1.0f) cy = 1.0f;
				v->uv[0] = (cx + 1.0f) * 0.5f * mu;
				v->uv[1] = (cy + 1.0f) * 0.5f * mv + ov;
				float a = t[2] * A * 255.0f + 0.5f;
				v->rgba[0] = v->rgba[1] = v->rgba[2] = 255;
				v->rgba[3] = (u8)(a > 255.0f ? 255.0f : a);
			}
		}
		n++;
	}
	if (!n)
		return;
	s.lensDrawn = n;
	GSPGPU_FlushDataCache(vb, (u32)n * LENS_GV * sizeof(RainVert));

	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, vb, sizeof(RainVert), 3, 0x210);
	C3D_Mtx id;
	Mtx_Identity(&id);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uMvp, &id);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_ProcTexBind(0, NULL);
	C3D_TexBind(0, (C3D_Tex*)frame);
	// rgb = frame * 1.2 (t * 0.2 + t), alpha the vertex's.
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_TEXTURE0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MULTIPLY_ADD);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, 0xFF333333u);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	// Source over, the frame's alpha kept.
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_ZERO, GPU_ONE);
	C3D_DrawElements(GPU_TRIANGLES, n * LENS_GI, C3D_UNSIGNED_SHORT, s.lensIb);
	C3D_TexEnvInit(env);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

void rain_lens_describe(char* out, int size) {
	int n = snprintf(out, (size_t)size, "rain: lens drops (screen px x,y size alpha):");
	for (int i = 0; i < s.lensCount && n < size - 24; i++) {
		const LensDrop* d = &s.lens[i];
		if (!(d->life > 0.0f))
			continue;
		float A = 3.0f * d->life / d->lifeMax;
		n += snprintf(out + n, (size_t)(size - n), " %.0f,%.0f %.0f %.1f", d->x * 400.0f,
			d->y * 240.0f, d->r4 * kDropSize * 120.0f, A > 1.0f ? 1.0f : A);
	}
}

float rain_intensity(void) { return s.open ? s.intensity : 0.0f; }
float rain_dampness(void) { return s.open ? s.damp : 1.0f; }

bool rain_haze(u32 fogRgb, u8 rgb[3]) {
	rgb[0] = (u8)(fogRgb >> 16);
	rgb[1] = (u8)(fogRgb >> 8);
	rgb[2] = (u8)fogRgb;
	// ScreenEffectDB's slot is on while its intensity is at least 0.01
	// (AddScreenEffect, 0x0078425C on PC).
	if (!s.open || s.intensity < 0.01f)
		return false;
	// CalculateFogInfo: the Normal Fog colour (fields 2-4) moved toward the
	// Night Raining Fog's by the intensity, then the haze takes it (PC
	// FUN_00613020). Without the Normal Fog layer the frame's fog colour
	// stands in for it (#407).
	float fog[3] = { 128.0f, 128.0f, 128.0f };
	if (s.fogLayer)
		for (int i = 0; i < 3; i++)
			fog[i] = s.rainFog.values[2 + i];
	const float I = s.intensity;
	for (int i = 0; i < 3; i++) {
		const float dry = s.normalFogLayer ? s.normalFog.values[2 + i] : (float)rgb[i];
		// The effect's colour (Rain +0x4248.., an int per channel).
		float v = (1.0f - I) * dry + I * fog[i];
		const int c = (int)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v);
		// PC FUN_005C7860: ((c / 128 - 1) * I + 1) * 127.5, chopped by _ftol.
		// 128 is "no change", so the rain fog's grey leaves the frame as is.
		float m = (((float)c * 0.0078125f - 1.0f) * I + 1.0f) * 127.5f;
		rgb[i] = (u8)(m < 0.0f ? 0.0f : m > 255.0f ? 255.0f : m);
	}
	return true;
}

// FogValue's start and floor out of CalculateFogInfo's density (field 0) and
// start (field 1), blended toward the rain fog's by `I`.
static void fogInfo(float I, RainFog* f) {
	float d = s.normalFog.values[0], st = s.normalFog.values[1];
	if (I > 0.0f && s.fogLayer) {
		d = (1.0f - I) * d + I * s.rainFog.values[0];
		st = (1.0f - I) * st + I * s.rainFog.values[1];
	}
	f->start = st;
	// 0x5CB06C: 1 - FogInfo[4], no more than 0.6 (0x784564).
	const float fl = 1.0f - d;
	f->floor = fl > 0.6f ? 0.6f : fl;
}

bool rain_fog(RainFog* wet, RainFog* dry) {
	if (!s.open || !s.normalFogLayer || !(s.intensity > 0.0f))
		return false;
	fogInfo(s.intensity, wet);
	fogInfo(0.0f, dry);
	return true;
}

float rain_fog_visibility(const RainFog* f, float z) {
	float span = RAIN_FOG_END - f->start;
	if (span < 1.0f)
		span = 1.0f;
	float v = (RAIN_FOG_END - z) / span;
	if (v < f->floor)
		v = f->floor;
	return v > 1.0f ? 1.0f : v;
}

void rain_describe(char* out, int size) {
	RainFog wet = { 0.0f, 1.0f }, dry = { 0.0f, 1.0f };
	const bool fog = rain_fog(&wet, &dry);
	snprintf(out, (size_t)size,
		"rain: intensity %.2f (want %.2f, front %.2f, set %d) damp %.2f drops %d/%d "
		"drawn %d cover car %d ahead %d wind %.2f,%.2f fog %s start %.0f floor %.2f "
		"(dry %.0f %.2f) lens %d/%d drawn %d%s",
		s.intensity, s.desired, s.wave, s.rainSet, s.damp, s.count[0], s.active,
		s.lastQuads, s.coverCar, s.coverAhead, s.wind[0], s.wind[1], fog ? "on" : "off",
		wet.start, wet.floor, dry.start, dry.floor, s.lensCount, LENS_DROPS, s.lensDrawn,
		s.lensOk ? "" : " (no RAINDROPPC)");
}
