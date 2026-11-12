// NFSU2-3DS: the collision sounds (colsfx.h), TASKS.md #298.
#include "colsfx.h"

#include "aems.h"
#include "audio.h"
#include "dbglog.h"
#include "mixmap.h"
#include "rideinfo.h"
#include "sfxline.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void colLog(const char* fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	dbglog_line("COLSFX ", b);
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float len3(const float* a) { return sqrtf(dot3(a, a)); }
static void copy3(float* d, const float* s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }

static float dist3(const float* a, const float* b)
{
	const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
	return len3(d);
}

// bNormalize (0x8005D240): a zero vector stays zero.
static void norm3(float* d, const float* s)
{
	const float l = len3(s);
	if (l > 5e-11f) {
		d[0] = s[0] / l; d[1] = s[1] / l; d[2] = s[2] / l;
	} else {
		d[0] = d[1] = d[2] = 0.0f;
	}
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

// ---------------------------------------------------------------- tables

// cStichWrapper's volume per stitch, by the stitch's +6: STICH_TYPE 0's
// (collisions) and 1's (whooshes). SPEED2.EXE 0x3F34A0 and 0x3F36E8, the
// same values Speed.elf's static initialiser stores.
static const uint16_t kColVol[289] = {
	23197, 23197, 23197, 23197, 23197, 23197, 19518, 13045, 16422, 14636, 26028, 23197,
	26028, 23197, 32767, 32767, 32767, 32767, 32000, 32000, 32000, 32000, 23197, 23197,
	23197, 23197, 23197, 23197, 23197, 23197, 19518, 19518, 19518, 19518, 29204, 29204,
	29204, 29204, 16422, 13818, 16422, 16422, 26028, 23197, 23197, 23197, 26028, 23000,
	21000, 26028, 32767, 30934, 32767, 32767, 19518, 19518, 19518, 19518, 20675, 20675,
	20675, 20675, 24000, 24000, 25000, 27000, 23197, 26000, 32767, 22500, 20675, 20675,
	23197, 23197, 23197, 23197, 25000, 25000, 27570, 27570, 27570, 27570, 27570, 27570,
	27570, 27570, 32767, 32767, 32767, 32767, 29204, 29204, 29204, 29204, 26028, 26028,
	26028, 29204, 29204, 29204, 29204, 29204, 29204, 29204, 29204, 29204, 29204, 29204,
	29204, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 32767, 32767, 32767, 32767, 32767,
	32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 29204, 29204, 29204, 29204, 29204, 29204, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 32767, 32767,
	32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
	32767, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 29204, 29204, 29204, 24572, 24572,
	24572, 24572, 24572, 24572, 24572, 24572, 24572, 24572, 24572, 24572, 29204, 29204,
	29204, 29204, 29204, 29204, 29204, 29204, 29204, 29204, 29204, 29204, 29204, 29204,
	29204,
};
static const uint16_t kWooshVol[60] = {
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
	26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028, 26028,
};

// ImpactSlopeSensitivity (0x80321814): the impulse slope's input range, times
// this, by surface.
static const float kSens[9] = { 18.0f, 2.5f, 15.0f, 1.0f, 20.0f, 8.0f, 1.0f, 2.5f, 2.5f };

// IMPACT_INTENS_DISTRIBUTION (0x80355A3C): an impact's intensity, in twentieths,
// to which of a surface's four strengths of stitch it plays.
static const float kIntensDist[20] = {
	0.0f, 0.1691f, 0.2891f, 0.3821f, 0.4582f, 0.5224f, 0.5781f, 0.6272f, 0.6712f, 0.7109f,
	0.7472f, 0.7806f, 0.8115f, 0.8403f, 0.8672f, 0.8925f, 0.9163f, 0.9389f, 0.9603f, 0.9806f,
};

// Slope (Initialize 0x80085404, Update 0x800854BC, GetValue 0x8008544C): the
// input clamped to [inMin, inMax], mapped onto [outMin, outMax].
typedef struct { float outMin, outMax, inMin, inMax; } Slope;
static const Slope kWallImpulse = { 0.0f, 1.0f, 0.0f, 3.0f };   // WallImpulseScale 0x803A3E2C
static const Slope kWallScrape  = { 0.0f, 1.0f, 0.0f, 40.0f };  // WallScrapeScale 0x803A3E48
static const Slope kCarImpulse  = { 0.0f, 1.0f, 0.0f, 3.0f };   // CarImpulseScale 0x803A3E64
static const Slope kSmokeImpulse = { 0.0f, 1.0f, 0.0f, 3.0f };  // SmokeImpulseScale 0x803A3DF4
static const Slope kSmokeScrape  = { 0.0f, 1.0f, 5.0f, 15.0f }; // SmokeScrapeScale 0x803A3E10
// BarrelImpulseScale (0x803A3DD8): the car's mph to the barrel splash's level.
static const Slope kBarrelImpulse = { 16002.0f, 32767.0f, 15.0f, 60.0f };
#define BARREL_SPLASH_VOL 13000   // nBarrelSplashVol (0x8046D47C)
#define CATEGORY_WATER_DRUM 5     // SmokeableCategoryNameTable's

static float slopeValue(Slope s, float x)
{
	if (fabsf(s.inMax - s.inMin) < 1e-6f) s.inMax += 1e-6f;
	float t = (x - s.inMin) / (s.inMax - s.inMin);
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	return s.outMin + t * (s.outMax - s.outMin);
}

// ---------------------------------------------------------------- stitches

// LoaderSoundStichs (0x8008C7B4): InGameCommon.bun's two 0x8003B500 chunks.
// Each stitch is a 0x3B502 chunk (+0 its hash, +4 its volume, +6 its row in
// the volume table, +8 its STICH_TYPE, +9 how many samples) followed by a
// 0x3B503 one, the samples, 16 bytes each: +0 the sample, +1 its bank (0
// MB_Stich_Collision, 1 MB_Stitch_Whoosh), +2 volume, +4 pitch, +6 delay,
// +8 azimuth, +0xA and +0xC how much of the volume and pitch is random.
typedef struct {
	uint16_t vol, table;
	uint8_t  type, count;
	const uint8_t* rec;
} Stitch;

#define STICH_TYPES 8
static uint8_t* s_bun;                    // the chunks' payloads, end to end
static Stitch*  s_stich;                  // sorted by type, in file order
static int      s_stichN;
static int      s_typeAt[STICH_TYPES + 1];

// Counts the stitches in the payloads, into `out` when it is given; stops at
// one cut short.
static int stichWalk(const uint8_t* d, size_t len, Stitch* out)
{
	int n = 0;
	size_t o = 0;
	while (o + 8 <= len) {
		const uint32_t id = rd32(d + o), cs = rd32(d + o + 4);
		if (id != 0x3B502u) {
			o += 8 + (size_t)cs;
			continue;
		}
		const size_t r = o + 8 + cs;
		if (cs < 10 || r + 8 > len || rd32(d + r) != 0x3B503u) break;
		const uint32_t ds = rd32(d + r + 4);
		const uint8_t* st = d + o + 8;
		if (r + 8 + ds > len || (size_t)st[9] * 16 > ds) break;
		if (out) {
			out[n].vol = rd16(st + 4);
			out[n].table = rd16(st + 6);
			out[n].type = st[8];
			out[n].count = st[9];
			out[n].rec = d + r + 8;
		}
		n++;
		o = r + 8 + ds;
	}
	return n;
}

static bool stichLoad(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	size_t len = 0;
	uint8_t h[8];
	while (fread(h, 1, 8, f) == 8) {
		const uint32_t size = rd32(h + 4);
		if (rd32(h) != 0x8003B500u) {
			if (fseek(f, (long)size, SEEK_CUR) != 0) break;
			continue;
		}
		uint8_t* d = realloc(s_bun, len + size);
		if (!d) break;
		s_bun = d;
		if (fread(s_bun + len, 1, size, f) != size) break;
		len += size;
	}
	fclose(f);
	const int n = s_bun ? stichWalk(s_bun, len, NULL) : 0;
	Stitch* all = n ? malloc(sizeof(Stitch) * (size_t)n) : NULL;
	s_stich = n ? malloc(sizeof(Stitch) * (size_t)n) : NULL;
	if (!all || !s_stich) {
		free(all);
		return false;
	}
	stichWalk(s_bun, len, all);
	int k = 0;
	for (int t = 0; t < STICH_TYPES; t++) {
		s_typeAt[t] = k;
		for (int i = 0; i < n; i++)
			if (all[i].type == t) s_stich[k++] = all[i];
	}
	s_typeAt[STICH_TYPES] = k;
	s_stichN = k;
	free(all);
	return k > 0;
}

// cSTICH_PlayBack::GetStich: the idx-th stitch of a type, NULL past the end.
static const Stitch* getStich(int type, int idx)
{
	if (type < 0 || type >= STICH_TYPES || idx < 0) return NULL;
	const int at = s_typeAt[type] + idx;
	return at < s_typeAt[type + 1] ? &s_stich[at] : NULL;
}

// The volume table's entry for a stitch: type 0 kColVol, 1 kWooshVol, any
// other full.
static int32_t stichTableVol(const Stitch* st)
{
	if (st->type == 0) return st->table < 289 ? kColVol[st->table] : 0x7fff;
	if (st->type == 1) return st->table < 60 ? kWooshVol[st->table] : 0x7fff;
	return 0x7fff;
}

// ---------------------------------------------------------------- state

static bool          s_on;
static const CarSim* s_player;
static uint32_t*     s_rng;
static uint32_t      s_rngOwn;
static AemsBank*     s_bank[3];   // MB_Stich_Collision, MB_Stitch_Whoosh, FX_Misc_MB
static float         s_now;       // WorldTimer, seconds: stops with the pause
static bool          s_paused;
static float         s_pausedFor;
static float         s_reportIn;
static int           s_impacts, s_scrapes, s_impactLogs, s_doneLogs;

// SFX_Base::AdjustPitch, as carsfx.c's: a second into the pause, pitch 0.
static void adjustPitch(int32_t* pitch)
{
	if (s_paused && s_pausedFor > 1.0f)
		*pitch = 0;
}

// The physics' Collision (0x801AB990 on): two bodies in contact, alive 0.3 s
// past its last impulse (Collision::Update 0x801AC5EC). Retail makes one per
// pair of bodies; the port's world is one body, so a car's collisions with it
// are told apart by the ground and the walls, and two cars by the pair.
#define COLL_MAX     32
#define COLL_LIFE    0.3f
typedef struct {
	bool  used;
	const CarSim* a;   // GetCollidingCars (0x801AC744): b NULL against the world
	const CarSim* b;   // (and a NULL for a prop against the world)
	CarSimProp* prop;  // GetSmokeable (0x801AC834), with its serial
	uint32_t serial;
	bool  ground;
	float last;        // +0x0C, s_now at the last impulse
	float pos[3];      // +0x14, where the first one was
	float lastPos[3];  // +0x24, where the last one was
	float slip;        // +0x34, its sliding speed
	float sum[3];      // +0x38, the impulses summed
	float mag;         // +0x48, their lengths summed
	float dir[3];      // +0x54, the last one's direction
} Coll;
static Coll s_coll[COLL_MAX];

// cSampleWarpper: one AEMS sample of a stitch.
typedef struct {
	AemsInst inst;
	int32_t  data[6];
	int32_t  vol, pitch;   // the record's, less their random part
	const uint8_t* rec;
	bool     alive;
} Sample;

// cStichWrapper: a stitch playing.
#define SAMPLE_MAX 15
typedef struct {
	const Stitch* st;
	int32_t p[5];
	Sample  smp[SAMPLE_MAX];
	int     n;
	bool    playing;
} Wrapper;

// stSoundCollision, with stWorldCollision's, stCarOnCarCollision's and
// stSmokableCollision's parts.
#define SND_MAX 32
typedef struct {
	bool  used;
	bool  carOnCar;        // stCarOnCarCollision
	bool  smk;             // stSmokableCollision; neither, stWorldCollision
	bool  init;            // +0x08
	Coll* coll;            // +0x1C
	float pos[3];          // +0x20
	float n[3];            // +0x30
	int   impactInt;       // +0x40
	int   scrapeInt;       // +0x44
	const Slope* impSlope; // +0x90
	const Slope* scrSlope; // +0x94
	float dist;            // +0x98, to the player at Initialize
	int32_t p[5];          // +0xAC SND_Params of the impact: +0xB0 vol, pitch, azimuth
	int32_t scrVol, scrPitch, scrAz, scrInt;   // +0xC4..+0xD0, the scrape's
	float colPos[3];       // +0xD4
	float scrPos[3];       // +0xE4
	const CarSim* car;     // +0xFC
	float mph;             // +0x100
	int   type, desc;      // 1 two cars; 1 the player, 4 a wall, 8 two cars, 0x10 a car
	int   surface;         // +0x108, eSND_COL_SURFACE_TYPE
	bool  impactActive;    // +0x11C
	bool  impactBegun;     // +0x120
	bool  scraping, ending, disabled, scrapeQueued, fading, inQueue;
	InterpLine fade;
	int   state;           // the Collision state copy it holds, -1 none
	bool  hasWrap;
	Wrapper wrap;
	AemsInst scrape;       // FX_Scrape
	int32_t scrData[5];
	const CarSim* carA;    // +0x130, +0x134 (stCarOnCarCollision)
	const CarSim* carB;
	int   category;        // +0x130 (stSmokableCollision): the SmokeableInfo's
	CarSimProp* prop;      // +0x134, the Smokeable, while its serial matches
	uint32_t propSerial;
	AemsInst splash[3];    // +0x13C: SND_BarrelSplash
	int32_t splashData[3][6];
	// Debug: how long it lived and the levels it reached, logged at its end.
	float age;
	int32_t pkImp, pkSmp, pkScr, pkScrInt, pkScrMem;
} Snd;
static Snd s_snd[SND_MAX];

// EAXCollision: its list (+0x24), the queue of impacts to begin, the
// queue's count as last appended (+0x34; nothing else updates it), the
// impacts playing, and the five Collision state copies.
#define MAX_IMPACTS 8
static Snd* s_list[SND_MAX];
static int  s_listN;
static Snd* s_queue[SND_MAX];
static int  s_queueN;
static int  s_queuedCount;
static int  s_numPlaying;
static Snd* s_state[MIXMAP_COL_COPIES];
static bool s_newAttach[MIXMAP_COL_COPIES];   // SFXObj_Collision +0xBC
// stWorldCollision's LastImpactNormal (0x803A3D9C) and LastImpactPosition
// (0x803A3DAC), which BeginImpact sets.
static float s_lastNormal[3], s_lastPos[3];

static const float* carPos(const CarSim* c) { return c->rbPos; }   // Car +0x58
static float carMph(const CarSim* c) { return len3(c->vel) * 2.237f; }
static const float* playerPos(void) { return s_player ? carPos(s_player) : NULL; }

static void listRemove(Snd** l, int* n, const Snd* sc)
{
	for (int i = 0; i < *n; i++)
		if (l[i] == sc) {
			memmove(&l[i], &l[i + 1], sizeof(*l) * (size_t)(*n - i - 1));
			(*n)--;
			return;
		}
}
// ---------------------------------------------------------------- the stitch player

// cSampleWarpper::Destroy (0x8008C03C).
static void sampleDestroy(Sample* s)
{
	if (s->inst) aemsRelease(s->inst);
	s->inst = NULL;
	s->alive = false;
}

// cSampleWarpper::Play (0x8008C344): the record's volume and pitch less a
// random part, and an AEMS_StichCollision or AEMS_StichWoosh instance.
static void samplePlay(Sample* s, const uint8_t* rec, const int32_t* p)
{
	if (s->inst) return;
	const int16_t volRand = (int16_t)rd16(rec + 0xA), pitchRand = (int16_t)rd16(rec + 0xC);
	s->rec = rec;
	s->vol = rd16(rec + 2) - (volRand ? (int32_t)rideinfo_random(s_rng, (uint32_t)(int32_t)volRand) : 0);
	s->pitch = rd16(rec + 4) - (pitchRand ? (int32_t)rideinfo_random(s_rng, (uint32_t)abs(pitchRand)) : 0);
	int32_t* d = s->data;
	d[0] = clampi((int8_t)rec[1], 0, 16);
	d[1] = clampi(rec[0], 0, 0x7f);
	d[2] = clampi((int32_t)((int64_t)s->vol * p[1] >> 15), 0, 0x7fff);
	d[3] = clampi((int32_t)((float)s->pitch * (float)(0x1000 - p[2]) * (1.0f / 4096.0f)), 0, 0x2000);
	d[4] = clampi((p[3] + rd16(rec + 8) + 0x10000) % 0x10000, 0, 0x10000);
	d[5] = clampi(rd16(rec + 6), 0, 4000);
	s->inst = aemsCreate(aemsClass(d[0] ? "AEMS_StichWoosh" : "AEMS_StichCollision"), d);
	s->alive = true;
}

// cSampleWarpper::Update (0x8008C11C): the stored pitch, the wrapper's volume
// and azimuth; done when its graphs are.
static void sampleUpdate(Sample* s, const int32_t* p)
{
	int32_t* d = s->data;
	d[4] = clampi((p[3] + rd16(s->rec + 8) + 0x10000) % 0x10000, 0, 0x10000);
	d[2] = clampi((int32_t)((int64_t)s->vol * p[1] >> 15), 0, 0x7fff);
	d[3] = clampi(s->pitch, 0, 0x2000);
	aemsSetMemberData(s->inst, d);
	if (aemsRefCount(s->inst) < 2) sampleDestroy(s);
}

// cStichWrapper::Play (0x8008BC80): every sample of the stitch at once (each
// record's delay staggers them in its graph).
static void wrapPlay(Wrapper* w, const Stitch* st, const int32_t* p)
{
	memset(w, 0, sizeof(*w));
	w->st = st;
	memcpy(w->p, p, sizeof(w->p));
	const int32_t vol = p[1] * stichTableVol(st) >> 15;
	w->p[1] = vol;
	w->n = st->count < SAMPLE_MAX ? st->count : SAMPLE_MAX;
	for (int i = 0; i < w->n; i++) {
		const int32_t sp[5] = { p[0], vol * st->vol >> 15, p[2], p[3], p[4] };
		samplePlay(&w->smp[i], st->rec + i * 16, sp);
	}
	w->playing = true;
}

// cStichWrapper::Update (0x8008BDFC): playing while any sample is.
static void wrapUpdate(Wrapper* w, const int32_t* p)
{
	if (!w->playing) return;
	memcpy(w->p, p, sizeof(w->p));
	w->p[1] = p[1] * stichTableVol(w->st) >> 15;
	w->playing = false;
	for (int i = 0; i < w->n; i++) {
		if (!w->smp[i].alive) continue;
		sampleUpdate(&w->smp[i], w->p);
		w->playing = true;
	}
}

static void wrapDestroy(Snd* sc)
{
	if (!sc->hasWrap) return;
	for (int i = 0; i < sc->wrap.n; i++) sampleDestroy(&sc->wrap.smp[i]);
	sc->hasWrap = false;
}

// The cStichWrappers the other sounds new and delete themselves (road
// noise's transitions, the landings and bottom-outs, the tunnel drive-bys
// and the DriveBy state's wooshes: 2 + 2 + 4 + 4), out of a pool
// colsfxShutdown can empty before the banks go.
#define EXT_MAX 12
struct ColsfxStitch {
	bool    used;
	Wrapper w;
};
static ColsfxStitch s_ext[EXT_MAX];

static void extFree(ColsfxStitch* x)
{
	for (int i = 0; i < x->w.n; i++) sampleDestroy(&x->w.smp[i]);
	x->used = false;
}

// ---------------------------------------------------------------- the scrape

// CSISBeginScrape (0x80081494): FX_Scrape, sample 0 (SetScrapeSampleIndex is
// empty).
static void csisBeginScrape(Snd* sc)
{
	if (sc->scrape) return;
	int32_t* d = sc->scrData;
	d[0] = clampi(sc->scrVol, 0, 0x7fff);
	d[1] = clampi(sc->scrPitch, 0, 9000);
	d[2] = clampi(sc->scrAz, 0, 0xffff);
	d[3] = 0;
	d[4] = clampi(sc->scrInt, 0, 0x7f);
	sc->scrape = aemsCreate(aemsClass("FX_Scrape"), d);
}

// CSISUpdateScrape (0x80081624).
static void csisUpdateScrape(Snd* sc)
{
	if (!sc->scrape) return;
	int32_t* d = sc->scrData;
	d[0] = clampi(sc->scrVol, 0, 0x7fff);
	d[1] = clampi(sc->scrPitch, 0, 9000);
	d[2] = clampi(sc->scrAz, 0, 0xffff);
	d[4] = clampi(sc->scrInt, 0, 0x7f);
	aemsSetMemberData(sc->scrape, d);
}

// EndScrape (0x80081DD4), with CSISEndScrape.
static void endScrape(Snd* sc)
{
	if (sc->scrape) aemsRelease(sc->scrape);
	sc->scrape = NULL;
	sc->scraping = false;
}

// ---------------------------------------------------------------- stSoundCollision

// SFXObj_Collision::NewAttachment (0x800A2B44): the copy's 3DColPos and
// 3DScrapePos follow this collision.
static bool attach(Snd* sc)
{
	for (int k = 0; k < MIXMAP_COL_COPIES; k++) {
		if (s_state[k]) continue;
		s_state[k] = sc;
		sc->state = k;
		s_newAttach[k] = true;
		mixmapSetObjPos(MIXMAP_COPY_ID(MIXMAP_CTL_3DCOLPOS, k), sc->colPos);
		mixmapSetObjPos(MIXMAP_COPY_ID(MIXMAP_CTL_3DSCRAPEPOS, k), sc->scrPos);
		return true;
	}
	return false;
}

static int collDesc(const Coll* c)
{
	if (c->prop) {
		// GetCollisionObjectsDescription (0x8007FF90): 2 a smokeable, 0x10 with
		// a car, 4 against a wall of the track.
		int d = 2 | (c->a ? 0x10 : (c->ground ? 0 : 4));
		if (s_player && c->a == s_player) d |= 1;
		return d;
	}
	int d = 0x10 | (c->b ? 8 : 4);
	if (s_player && (c->a == s_player || c->b == s_player)) d |= 1;
	return d;
}

// stWorldCollision::GetSurfaceType (0x80083D3C): 2 on its roof, 3 on its
// floor, 4 a wall head on, 5 a wall along the side.
static int surfWorld(const Snd* sc)
{
	const CarSim* car = sc->coll->a ? sc->coll->a : sc->coll->b;
	if (!car) return 4;
	float n[3];
	norm3(n, sc->coll->dir);
	if (fabsf(n[2]) <= 0.707f)
		return fabsf(dot3(sc->n, car->rot[0])) > 0.707f ? 4 : 5;
	if (car->rot[2][2] < 0.1f) return 2;
	if (car->rot[2][2] > 0.707f) return 3;
	return 4;
}

// stCarOnCarCollision::GetSurfaceType (0x8008383C): 0 nose or tail on (or
// across), 1 side by side.
static int surfCarOnCar(const Snd* sc)
{
	const CarSim* a = sc->carA;
	const CarSim* b = sc->carB;
	if (!a || !b) return 0;
	float fa[3] = { a->rot[0][0], a->rot[0][1], 0.0f }, fb[3] = { b->rot[0][0], b->rot[0][1], 0.0f };
	norm3(fa, fa);
	norm3(fb, fb);
	if (fabsf(dot3(fa, fb)) <= 0.707f) return 0;
	norm3(fa, a->rot[0]);
	norm3(fb, b->rot[0]);
	const float* pa = carPos(a);
	const float* pb = carPos(b);
	float ra[3] = { sc->pos[0] - pa[0], sc->pos[1] - pa[1], sc->pos[2] - pa[2] };
	float rb[3] = { sc->pos[0] - pb[0], sc->pos[1] - pb[1], sc->pos[2] - pb[2] };
	norm3(ra, ra);
	norm3(rb, rb);
	return (fabsf(dot3(ra, fa)) > 0.8f && fabsf(dot3(rb, fb)) > 0.8f) ? 0 : 1;
}

// stSoundCollision::GenImpactIntensity (0x80081A30) and stWorldCollision's
// (0x80083F38): the summed impulse through the slope, its range scaled by the
// surface's sensitivity; a world hit on a car off its wheels and tipped is
// 50 louder.
static void genImpactIntensity(Snd* sc)
{
	Slope s = *sc->impSlope;
	s.inMax *= kSens[sc->surface];
	if (sc->smk) {
		// stSmokableCollision's (0x80083708): the impulses times the prop's
		// GetMassInverse, 0 while it is still anchored.
		const CarSimProp* p = sc->prop;
		const float im = p->anchor != 0.0f ? 0.0f : p->invMass;
		sc->impactInt = (int)(slopeValue(s, sc->coll->mag * im) * 127.0f);
		return;
	}
	sc->impactInt = (int)(slopeValue(s, sc->coll->mag) * 127.0f);
	if (!sc->carOnCar && sc->car && sc->car->rot[2][2] < 0.9f && sc->car->wheelsDown < 1)
		sc->impactInt = sc->impactInt + 50 > 127 ? 127 : sc->impactInt + 50;
}

// GenScrapeIntensity (0x80081B14).
static void genScrapeIntensity(Snd* sc)
{
	if (sc->ending || !sc->coll) return;
	sc->scrapeInt = (int)(slopeValue(*sc->scrSlope, sc->coll->slip) * 127.0f);
}

// stSoundCollision::Initialize (0x80081070), with stCarOnCarCollision's
// (0x80083CE8) around it. The car it plays for: the one, or of two the
// player's, else the faster.
static void scInitialize(Snd* sc, Coll* c)
{
	if (sc->carOnCar) {
		sc->carA = c->a;
		sc->carB = c->b;
	}
	if (sc->smk) {
		// stSmokableCollision::Initialize (0x800836A0): its prop and category.
		sc->prop = c->prop;
		sc->propSerial = c->serial;
		sc->category = c->prop->category;
	}
	copy3(sc->pos, c->pos);
	copy3(sc->n, c->dir);
	sc->type = sc->smk ? 2 : c->b ? 1 : 0;   // GetCollisionSoundType (0x8007FE9C)
	sc->desc = collDesc(c);
	// stSmokableCollision::GetSurfaceType (0x80083810): 7 a car on it, 8 the world.
	sc->surface = sc->smk ? ((sc->desc & 0x10) ? 7 : 8)
	                      : sc->carOnCar ? surfCarOnCar(sc) : surfWorld(sc);
	sc->dist = s_player ? dist3(sc->pos, playerPos()) : 0.0f;
	sc->scraping = sc->impactActive = false;
	sc->impSlope = &kWallImpulse;
	sc->scrSlope = &kWallScrape;
	sc->init = true;
	if (sc->desc & 0x10) {
		const CarSim* a = c->a;
		const CarSim* b = c->b;
		if (!a || !b) {
			sc->car = a ? a : b;
		} else {
			const bool pa = a == s_player, pb = b == s_player;
			if (!pa && !pb) sc->car = carMph(a) <= carMph(b) ? b : a;
			else if (!(pa && pb)) sc->car = pa ? a : b;
			else sc->car = carMph(b) < carMph(a) ? b : a;
		}
		sc->mph = sc->car ? carMph(sc->car) : 0.0f;
	}
	genImpactIntensity(sc);
	// stCarOnCarCollision's and stSmokableCollision's own slopes, set after (the
	// impulse ones are the same as the wall's).
	if (sc->carOnCar) sc->impSlope = &kCarImpulse;
	if (sc->smk) {
		sc->impSlope = &kSmokeImpulse;
		sc->scrSlope = &kSmokeScrape;
	}
}

// SetUpCollisionEnding (0x80081000): the collision is over; a scrape fades
// out over 0.1 s.
static void setUpEnding(Snd* sc)
{
	if (sc->ending) return;
	sc->coll = NULL;
	sc->ending = true;
	if (sc->scraping) {
		sc->fading = true;
		lineInit(&sc->fade, 1.0f, 0.0f, 100, 0);
	}
}

// stWorldCollision::GenStichData (0x80083FEC) and stCarOnCarCollision's
// (0x80083AFC): the surface's stitches come in fours (the floor's and roof's
// in twos), one per strength off the intensity, each played in turn. The jump
// camera's +60 (the car's +0x18 +0x58 == 6) has no port counterpart.
static int s_cWall, s_cSide, s_cFloor, s_cRoof, s_cNose, s_cBeside;

static float stichDist(int intensity)
{
	return kIntensDist[clampi((int)((float)intensity * 0.0078125f * 20.0f), 0, 19)];
}

static int stichStrength(int intensity)
{
	return (int)(stichDist(intensity) * 4.0f);
}

// stSmokableCollision::GenStichData (0x80082014): by the prop's category, three
// stitches a strength, two strengths against a car (surface 7) and three
// against the world (8), played in turn on the category's own counter. The
// categories with no stitches of their own share the last two counters.
static const uint8_t kSmkBranch[19] = { 0, 2, 0, 3, 4, 5, 5, 1, 1, 6, 7, 6, 8, 1, 1, 9, 10, 11, 12 };
static const uint16_t kSmkBase[13][2] = {
	{ 0x103, 0x109 }, { 0x103, 0x109 }, { 0xC7, 0xCD }, { 0x7C, 0x82 }, { 0x8B, 0x91 },
	{ 0x5E, 0x64 }, { 0xE5, 0xEB }, { 0xD6, 0xDC }, { 0xF4, 0xFA }, { 0x9A, 0xA0 },
	{ 0x112, 0x118 }, { 0xA9, 0xAF }, { 0x6D, 0x73 },
};
static int s_cSmk[2][13];

static const Stitch* genSmkStichData(Snd* sc)
{
	if (sc->surface != 7 && sc->surface != 8) return getStich(0, 0);
	const int w = sc->surface == 8;
	const int br = sc->category >= 0 && sc->category < 19 ? kSmkBranch[sc->category] : 0;
	const int d = (int)(stichDist(sc->impactInt) * (w ? 3.0f : 2.0f));
	int* c = &s_cSmk[w][br];
	*c %= 3;
	const int idx = d * 3 + kSmkBase[br][w] + *c;
	(*c)++;
	return getStich(0, idx);
}

static const Stitch* genStichData(Snd* sc)
{
	if (sc->smk) return genSmkStichData(sc);
	const int d = stichStrength(sc->impactInt);
	int idx, *c;
	if (sc->carOnCar) {
		if (sc->surface == 0) { c = &s_cNose; idx = 6; }
		else if (sc->surface == 1) { c = &s_cBeside; idx = 0x16; }
		else return getStich(0, 0);
		*c %= 4;
		idx += d * 4 + *c;
	} else {
		switch (sc->surface) {
		case 4: c = &s_cWall; idx = 0x26; break;
		case 5: c = &s_cSide; idx = 0x36; break;
		case 3: c = &s_cFloor; idx = 0x4E; break;
		case 2: case 6:
			s_cRoof %= 2;
			idx = d * 2 + s_cRoof + 0x46;
			s_cRoof++;
			return getStich(0, idx);
		default: return getStich(0, 0x26);
		}
		*c %= 4;
		idx += d * 4 + *c;
	}
	(*c)++;
	return getStich(0, idx);
}

// EndImpact (0x80081D80).
static void endImpact(Snd* sc)
{
	wrapDestroy(sc);
	sc->impactActive = false;
	s_numPlaying--;
}

// BeginImpact (0x80081E1C), with STICH_BeginImpact: a Collision state copy,
// the stitch, at volume 0 until the copy's first ProcessUpdate.
static void soundBeginImpact(Snd* sc)
{
	Coll* c = sc->coll;
	if (!c) {
		setUpEnding(sc);
		return;
	}
	copy3(s_lastNormal, c->dir);
	copy3(sc->pos, c->lastPos);
	copy3(sc->scrPos, sc->pos);
	copy3(s_lastPos, c->lastPos);
	copy3(sc->colPos, sc->pos);
	sc->impactActive = true;
	if (sc->state < 0 && !attach(sc)) {
		sc->impactActive = false;
		setUpEnding(sc);
		return;
	}
	const Stitch* st = genStichData(sc);
	sc->impactBegun = true;
	sc->impactActive = true;
	sc->p[1] = sc->p[2] = sc->p[3] = 0;
	s_numPlaying++;
	// PORT: a stitch the file does not have plays nothing (retail's are all there).
	if (!sc->hasWrap && st) {
		wrapPlay(&sc->wrap, st, sc->p);
		sc->hasWrap = true;
	}
	s_impacts++;
	if (s_impactLogs < 40) {
		s_impactLogs++;
		colLog("impact: %s surface %d, impulse %.2f -> intensity %d, stitch %d (%d sample(s)), copy %d, %.1f m%s",
		       sc->carOnCar ? "car-car" : sc->smk ? "smokable" : "world", sc->surface, c->mag, sc->impactInt,
		       st ? (int)(st - s_stich) - s_typeAt[0] : -1, st ? st->count : 0, sc->state, sc->dist,
		       (sc->desc & 1) ? ", player" : "");
	}
}

// stSmokableCollision::BeginImpact (0x80084B50): a water drum that has woken
// (Awaken has started its emitters, the water) splashes once:
// SND_BarrelSplash, three FX_ROADNOISE_TRANS of sample 2, silent until Update
// gives them their levels.
static void beginImpact(Snd* sc)
{
	soundBeginImpact(sc);
	CarSimProp* p = sc->prop;
	if (!sc->smk || sc->category != CATEGORY_WATER_DRUM || !p || p->serial != sc->propSerial ||
	    !p->emitted || p->splashed)
		return;
	p->splashed = true;
	for (int i = 0; i < 3; i++) {
		if (sc->splash[i]) continue;
		int32_t* d = sc->splashData[i];
		d[0] = 2;
		d[1] = d[2] = d[3] = 0;
		d[4] = 1;
		d[5] = 0;
		sc->splash[i] = aemsCreate(aemsClass("FX_ROADNOISE_TRANS"), d);
	}
	colLog("barrel splash: category %d, intensity %d, %.0f mph%s", sc->category, sc->impactInt,
	       sc->mph, (sc->desc & 1) ? ", player" : "");
}

// stSmokableCollision::Update's half (0x800847F4): each splash at the impact's
// level -- times BarrelImpulseScale of the speed when a car hit it -- and
// azimuth, pitched 2000, 4000 and 7000. The first one found done is let go
// and the rest wait for the next frame.
static void smkUpdateSplash(Snd* sc)
{
	for (int i = 0; i < 3; i++) {
		AemsInst inst = sc->splash[i];
		if (!inst) continue;
		if (aemsRefCount(inst) == 1) {
			aemsRelease(inst);
			sc->splash[i] = NULL;
			return;
		}
		int32_t vol = BARREL_SPLASH_VOL * sc->p[1] >> 15;
		if (sc->desc & 0x10) vol = vol * (int32_t)slopeValue(kBarrelImpulse, sc->mph) >> 15;
		int32_t* d = sc->splashData[i];
		d[1] = clampi(vol, 0, 0x7fff);
		d[3] = clampi(sc->p[3], 0, 0xffff);
		d[2] = clampi(i == 0 ? 2000 : i == 1 ? 4000 : 7000, 0, 0x1fff);
		aemsSetMemberData(inst, d);
	}
}

// BeginScrape (0x80081B84).
static void beginScrape(Snd* sc)
{
	if (!sc->coll) {
		setUpEnding(sc);
		return;
	}
	copy3(sc->scrPos, sc->pos);
	sc->scraping = true;
	if (sc->state < 0) {
		if (!attach(sc)) {
			sc->scraping = false;
			setUpEnding(sc);
			return;
		}
	} else {
		genScrapeIntensity(sc);
	}
	if (sc->type != 1) {
		sc->scrInt = sc->scrapeInt;
		sc->scrVol = sc->scrAz = sc->scrPitch = 0;
		csisBeginScrape(sc);
		sc->fading = false;
		s_scrapes++;
	}
}

// UpdateScrapeParams (0x800818F8). (Its Average of the scrape's azimuth has
// no reader, so is not kept.)
static void updateScrapeParams(Snd* sc, float dt)
{
	if (!sc->scraping) return;
	copy3(sc->scrPos, sc->pos);
	lineUpdate(&sc->fade, dt);
	genScrapeIntensity(sc);
	sc->scrInt = sc->scrapeInt;
	if (sc->fading) sc->scrVol = (int32_t)((float)sc->scrVol * sc->fade.cur);
	csisUpdateScrape(sc);
}

// stSoundCollision::Update (0x8008129C).
static void scUpdate(Snd* sc, float dt)
{
	if (!sc->init) return;
	if (sc->scrapeQueued) {
		beginScrape(sc);
		sc->scrapeQueued = false;
	}
	if (!sc->ending && (!sc->coll || s_now - sc->coll->last >= COLL_LIFE))
		setUpEnding(sc);
	// UpdateImpactParams.
	if (sc->impactActive) {
		sc->p[2] = 0x1000;
		if (sc->hasWrap) wrapUpdate(&sc->wrap, sc->p);
	}
	updateScrapeParams(sc, dt);
	sc->age += dt;
	if (sc->hasWrap)
		for (int i = 0; i < sc->wrap.n; i++)
			if (sc->wrap.smp[i].alive && sc->wrap.smp[i].data[2] > sc->pkSmp) sc->pkSmp = sc->wrap.smp[i].data[2];
	if (sc->scrape) {
		if (sc->scrInt > sc->pkScrInt) sc->pkScrInt = sc->scrInt;
		if (sc->scrData[0] > sc->pkScrMem) sc->pkScrMem = sc->scrData[0];
	}
	// UpdateCollisionEnding (0x800813B4).
	if (sc->ending) {
		if (!(sc->hasWrap && sc->wrap.playing) && sc->impactActive) endImpact(sc);
		if (sc->fading && sc->fade.done && sc->scraping) endScrape(sc);
	}
	if (sc->smk) smkUpdateSplash(sc);
}

// ~stSoundCollision: lets go of its state copy, its stitch (the count of
// impacts playing untouched) and its scrape.
static void scDelete(Snd* sc)
{
	if (sc->init && s_doneLogs < 40) {
		s_doneLogs++;
		colLog("done: %s surface %d after %.2f s; impact q15 peak %d -> sample vol peak %d; "
		       "scrape q15 peak %d, intensity peak %d -> FX_Scrape vol peak %d",
		       sc->carOnCar ? "car-car" : sc->smk ? "smokable" : "world", sc->surface, sc->age, sc->pkImp, sc->pkSmp,
		       sc->pkScr, sc->pkScrInt, sc->pkScrMem);
	}
	if (sc->state >= 0) {
		s_state[sc->state] = NULL;
		s_newAttach[sc->state] = false;
	}
	wrapDestroy(sc);
	endScrape(sc);
	// ~stSmokableCollision (0x80084A04): the splashes let go.
	for (int i = 0; i < 3; i++)
		if (sc->splash[i]) {
			aemsRelease(sc->splash[i]);
			sc->splash[i] = NULL;
		}
	listRemove(s_list, &s_listN, sc);
	if (sc->inQueue) listRemove(s_queue, &s_queueN, sc);
	sc->used = false;
}
// ---------------------------------------------------------------- EAXCollision

// stWorldCollision::ShouldBeginImpact (0x800843DC), stSoundCollision's
// (!disabled) first: not the same wall hit again (as the last impact's
// direction, within a metre of it, steep), nor a side scrape on this car
// along another's direction.
static bool shouldBeginImpact(Snd* sc)
{
	if (sc->smk) {
		// stSmokableCollision's (0x800835E4), which asks nothing of disabled:
		// the player's, or louder than 9, and not while an impact on the same
		// prop plays. Its walk ends, yes, at the list's first collision with
		// no prop.
		if (!(sc->desc & 1) && sc->impactInt <= 9) return false;
		for (int i = 0; i < s_listN; i++) {
			const Snd* o = s_list[i];
			if (!o->coll) continue;
			if (!o->coll->prop) return true;
			if (o->coll->prop == sc->prop && o->coll->serial == sc->propSerial && o->impactActive)
				return false;
		}
		return true;
	}
	if (sc->disabled) return false;
	if (sc->carOnCar || !sc->car) return true;
	const Coll* c = sc->coll;
	float ln[3], cn[3];
	norm3(ln, s_lastNormal);
	norm3(cn, c->dir);
	if (dot3(ln, cn) > 0.95f) {
		const float last = len3(s_lastNormal), dir = len3(c->dir);
		if (0.95f * dir < last && last < 1.05f * dir && dist3(c->lastPos, s_lastPos) < 1.0f &&
		    fabsf(c->dir[2]) > 0.707f)
			return false;
	}
	for (int i = 0; i < s_listN; i++) {
		const Snd* o = s_list[i];
		if (!o->init || o->surface != 5 || !o->car || o->car != sc->car) continue;
		if (o == sc) {
			if (o->impactBegun) return false;
		} else if (dot3(c->dir, o->n) > 0.8f) {
			return false;
		}
	}
	return true;
}

// CreatCollisionSample (0x800809C0): one stSoundCollision per Collision.
static void creatCollisionSample(Coll* c)
{
	for (int i = 0; i < s_listN; i++)
		if (s_list[i]->coll == c) return;
	if (s_queuedCount > 9) return;
	Snd* sc = NULL;
	for (int i = 0; i < SND_MAX && !sc; i++)
		if (!s_snd[i].used) sc = &s_snd[i];
	if (!sc) return;   // PORT: retail allocates; 32 at once is past anything seen
	memset(sc, 0, sizeof(*sc));
	sc->used = true;
	// GetCollisionSoundType (0x8007FE9C): a smokeable's first, then two cars'.
	// (ShouldPlaySmokable 0x800809B8 says yes.)
	sc->smk = c->prop != NULL;
	sc->carOnCar = !sc->smk && c->b != NULL;
	sc->coll = c;
	sc->state = -1;
	lineIdle(&sc->fade);
	s_list[s_listN++] = sc;
}

static Snd* findSnd(const Coll* c, bool live)
{
	for (int i = 0; i < s_listN; i++)
		if (s_list[i]->coll == c && !(live && s_list[i]->ending)) return s_list[i];
	return NULL;
}

// StartCollisionSample (0x80080B1C): the collision's impact, queued once.
static void startCollisionSample(Coll* c)
{
	Snd* sc = findSnd(c, true);
	if (!sc) return;
	if (!sc->init) scInitialize(sc, c);
	if (sc->impactActive || sc->ending || sc->impactBegun || sc->inQueue) return;
	if (shouldBeginImpact(sc)) {
		s_queue[s_queueN++] = sc;
		sc->inQueue = true;
		s_queuedCount = s_queueN;
	} else {
		sc->disabled = true;
	}
}

// PlayScrape (0x8008090C), and ShouldBeginScrape: a prop never scrapes
// (stSmokableCollision's 0x800835C0).
static void playScrape(Coll* c)
{
	Snd* sc = findSnd(c, false);
	if (!sc || !sc->scrSlope || sc->smk) return;
	if (!sc->scraping && !sc->ending && c->slip >= sc->scrSlope->inMin) sc->scrapeQueued = true;
}

// ReleaseCollisionSample (0x80080C54): a queued impact goes unplayed, a
// playing one ends.
static void releaseCollisionSample(Coll* c)
{
	for (int i = 0; i < s_queueN; i++)
		if (s_queue[i]->coll == c) {
			scDelete(s_queue[i]);
			return;
		}
	Snd* sc = findSnd(c, false);
	if (sc) setUpEnding(sc);
}

// UpdateQueueImpact (0x80080404): the queue's impacts begin if they fit in
// eight; if not, the player's and the nearest replace the playing ones from
// the list's end, as long as each pair is a step up.
static void updateQueueImpact(void)
{
	if (s_queuedCount == 0) return;
	if (s_queuedCount + s_numPlaying <= MAX_IMPACTS) {
		for (int i = 0; i < s_queueN; i++) beginImpact(s_queue[i]);
	} else {
		Snd* a[SND_MAX];
		Snd* b[SND_MAX];
		int na = 0, nb = 0;
		Snd* near = NULL;
		for (int i = 0; i < s_queueN; i++) {
			Snd* q = s_queue[i];
			if (q->desc & 1) a[na++] = q;
			else if (!near || q->dist < near->dist) near = q;
		}
		if (near) a[na++] = near;
		near = NULL;
		for (int i = 0; i < s_listN; i++) {
			Snd* l = s_list[i];
			if (l->desc & 1) b[nb++] = l;
			else if (!near || l->dist < near->dist) near = l;
		}
		if (near) b[nb++] = near;
		// PORT: retail's walk assumes neither is empty.
		int ia = 0, ib = nb - 1;
		bool replaced = false;
		while (na > 0 && nb > 0) {
			Snd* cand = a[ia];
			Snd* act = b[ib];
			bool swap;
			if (!(cand->desc & 1)) {
				if (act->desc & 1) break;
				swap = cand->dist < act->dist;
			} else {
				swap = !(act->desc & 1);
			}
			if (swap) {
				// Ends it whether or not it plays, and counts it off either way.
				endImpact(act);
				beginImpact(cand);
				replaced = true;
			}
			if (!replaced || ib == 0) break;
			ia++;
			ib--;
			if (ia >= na) break;
		}
	}
	for (int i = 0; i < s_queueN; i++) s_queue[i]->inQueue = false;
	s_queueN = 0;
	s_queuedCount = 0;
}

// ---------------------------------------------------------------- the hook

// Collision::AddImpulse (0x801AB990): summed into the pair's Collision. Within
// 50 m of the player (in the plane), the direction is the impulse's and, the
// collision's first hit within 100 m, its impact and its scrape are asked for.
static void onImpulse(const CarSim* car, const CarSim* other, CarSimProp* prop, bool ground,
		const float pos[3], const float imp[3], float slip)
{
	if (!s_on) return;
	Coll* c = NULL;
	Coll* spare = NULL;
	for (int i = 0; i < COLL_MAX; i++) {
		Coll* k = &s_coll[i];
		if (!k->used) {
			if (!spare) spare = k;
			continue;
		}
		const bool same = other ? ((k->a == car && k->b == other) || (k->a == other && k->b == car))
		                : prop ? (k->prop == prop && k->serial == prop->serial && k->a == car &&
		                          k->ground == ground)
		                       : (k->a == car && !k->b && !k->prop && k->ground == ground);
		if (same) {
			c = k;
			break;
		}
	}
	if (!c) {
		if (!spare) return;   // PORT: retail's pool is the physics'
		c = spare;
		memset(c, 0, sizeof(*c));
		c->used = true;
		c->a = car;
		c->b = other;
		c->prop = prop;
		c->serial = prop ? prop->serial : 0;
		c->ground = ground;
		copy3(c->pos, pos);
		creatCollisionSample(c);
	}
	for (int k = 0; k < 3; k++) c->sum[k] += imp[k];
	c->mag += len3(imp);
	c->last = s_now;
	copy3(c->lastPos, pos);
	c->slip = slip;
	if (!s_player) return;
	const float* pp = playerPos();
	const float dx = pos[0] - pp[0], dy = pos[1] - pp[1];
	if (sqrtf(dx * dx + dy * dy) >= 50.0f) return;
	norm3(c->dir, imp);
	if (dist3(c->pos, pp) < 100.0f) {
		startCollisionSample(c);
		playScrape(c);
	}
}

// ---------------------------------------------------------------- the camera

// SFX_Common's own sound, FX_Camera out of FX_Misc_MB: the photo shoot's
// flash. PlayCameraSnapShot (0x8007D0E8) raises EAXSound+8; UpdateParams
// (0x80098098) takes it down, lets the last shot go and makes a new one, its
// seven member words zero; ProcessUpdate (0x800981DC) lets it go once only
// the game holds it, and otherwise gives it member 0, the object's Q15
// output 0 clamped to 0..0x7FFF.
static bool     s_camShot;      // EAXSound+8
static AemsInst s_cam;          // SFX_Common+0x9C
static int32_t  s_camData[7];

static void camUpdateParams(void)
{
	if (!s_camShot) return;
	s_camShot = false;
	if (s_cam) aemsRelease(s_cam);
	memset(s_camData, 0, sizeof(s_camData));
	s_cam = aemsCreate(aemsClass("FX_Camera"), s_camData);
	colLog("camera: FX_Camera %s, q15 %d", s_cam ? "made" : "not made",
	       (int)mixmapQ15(MIXMAP_SFX_COMMON, 0));
}

static void camProcessUpdate(void)
{
	if (!s_cam) return;
	if (aemsRefCount(s_cam) <= 1) {
		aemsRelease(s_cam);
		s_cam = NULL;
		return;
	}
	int32_t v = mixmapQ15(MIXMAP_SFX_COMMON, 0);
	s_camData[0] = v < 0 ? 0 : (v > 0x7fff ? 0x7fff : v);
	aemsSetMemberData(s_cam, s_camData);
}

void colsfxCameraSnapShot(void)
{
	s_camShot = true;
}

// ---------------------------------------------------------------- API

void colsfxSetup(const char* audioDir, const char* bunPath, const CarSim* player, uint32_t* rng)
{
	colsfxShutdown();
	s_player = player;
	s_rngOwn = 0x12345678u;
	s_rng = rng ? rng : &s_rngOwn;
	if (!stichLoad(bunPath)) {
		colLog("no stitches in %s -- collisions silent", bunPath);
		colsfxShutdown();
		return;
	}
	// EAXSound::InitializeInGame's IG_GLOBAL banks, on the mix map's bus
	// when it is up (its outputs carry the sliders), else the effects one.
	static const char* const kBank[3] = { "MB_Stich_Collision", "MB_Stitch_Whoosh", "FX_Misc_MB" };
	const AudioBus bus = mixmapReady() ? AUDIO_BUS_MIXMAP : AUDIO_BUS_SFX;
	char path[512];
	for (int i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "%sIG_GLOBAL/%s.aem", audioDir, kBank[i]);
		s_bank[i] = aemsLoad(path, bus);
		if (!s_bank[i]) colLog("no %s -- its sounds silent (tools/abk3ds.py)", path);
	}
	colLog("%d stitches (type 0: %d, type 1: %d), banks %s %s %s, mix map %s", s_stichN,
	       s_typeAt[1] - s_typeAt[0], s_typeAt[2] - s_typeAt[1], s_bank[0] ? "yes" : "no",
	       s_bank[1] ? "yes" : "no", s_bank[2] ? "yes" : "no", mixmapReady() ? "up" : "down");
	s_on = true;
	s_reportIn = 5.0f;
	carsim_add_impulse_hook(onImpulse);
}

void colsfxPreMix(bool paused, float dt)
{
	s_pausedFor = (paused && s_paused) ? s_pausedFor + dt : 0.0f;
	s_paused = paused;
	camUpdateParams();
	if (!s_on) return;
	// Collision::Update: a collision 0.3 s past its last impulse is done.
	if (!paused)
		for (int i = 0; i < COLL_MAX; i++) {
			Coll* c = &s_coll[i];
			if (!c->used || s_now - c->last < COLL_LIFE) continue;
			releaseCollisionSample(c);
			for (int j = 0; j < s_listN; j++)
				if (s_list[j]->coll == c) s_list[j]->coll = NULL;
			c->used = false;
		}
	// SFXObj_Collision::UpdateParams (0x800A27F8), each copy: a new
	// attachment's impact triggers its surface's envelope at a level off its
	// intensity and how near the player it is.
	for (int k = 0; k < MIXMAP_COL_COPIES; k++) {
		int32_t* out = mixmapObjOut(MIXMAP_COPY_ID(MIXMAP_SFX_COLLISION, k));
		if (!out) continue;
		out[0] = out[1] = out[2] = 0;
		const Snd* sc = s_state[k];
		if (!sc) continue;
		if (s_newAttach[k]) {
			out[4] = sc->impactInt * 255;
			int32_t f = 0;
			if (s_player) {
				float d = dist3(playerPos(), sc->colPos);
				d = d < 0.0f ? 0.0f : (d > 40.0f ? 40.0f : d);
				f = (int32_t)((1.0f - d / 40.0f) * 32767.0f);
			}
			const int32_t vol = (sc->impactInt * f * 255 >> 15) * mixmapMasterVolume(4) >> 15;
			if (sc->impactActive) {
				if (sc->surface >= 0 && sc->surface < 2) {
					out[5] = vol;
					out[0] = 1;
				} else if (sc->surface >= 4 && sc->surface < 6) {
					out[6] = vol;
					out[1] = 1;
				} else if (sc->surface == 7 || sc->surface == 8) {
					out[2] = 1;
				}
			}
		}
		mixmapSetObjPos(MIXMAP_COPY_ID(MIXMAP_CTL_3DSCRAPEPOS, k), sc->scrPos);
	}
	if (!paused) s_now += dt;
}

void colsfxUpdate(float dt)
{
	camProcessUpdate();
	if (!s_on) return;
	// SFXObj_Collision::ProcessUpdate (0x800A298C), each copy: the impact's
	// volume off its surface's slot, the scrape's off slot 9; their azimuths
	// once, at the attachment.
	for (int k = 0; k < MIXMAP_COL_COPIES; k++) {
		Snd* sc = s_state[k];
		if (!sc) continue;
		const uint32_t id = MIXMAP_COPY_ID(MIXMAP_SFX_COLLISION, k);
		const int slot = sc->surface >= 0 && sc->surface <= 8 ? sc->surface : 0;
		if (s_newAttach[k]) {
			sc->p[3] = mixmapAzim(id, slot);
			sc->scrAz = mixmapAzim(id, 9);
			s_newAttach[k] = false;
		}
		sc->p[1] = mixmapQ15(id, slot);
		sc->scrVol = mixmapQ15(id, 9);
		if (sc->p[1] > sc->pkImp) sc->pkImp = sc->p[1];
		if (sc->scrVol > sc->pkScr) sc->pkScr = sc->scrVol;
		int32_t pitch = 0x1000;
		adjustPitch(&pitch);
		sc->p[2] = sc->scrPitch = pitch;
	}
	// EAXCollision::Update (0x800807E4).
	updateQueueImpact();
	for (int i = 0; i < s_listN;) {
		Snd* sc = s_list[i];
		scUpdate(sc, dt);
		if (!sc->impactActive && !sc->scraping && sc->ending) {
			scDelete(sc);
			continue;
		}
		i++;
	}
	if ((s_reportIn -= dt) <= 0.0f) {
		s_reportIn = 5.0f;
		int colls = 0;
		for (int i = 0; i < COLL_MAX; i++) colls += s_coll[i].used;
		if (s_impacts || s_scrapes || colls || s_listN)
			colLog("%d collision(s), %d sound(s), %d impact(s) playing; %d impact(s), %d scrape(s) begun in 5 s",
			       colls, s_listN, s_numPlaying, s_impacts, s_scrapes);
		s_impacts = s_scrapes = 0;
	}
}

ColsfxStitch* colsfxStitchPlay(int type, int idx, const int32_t p[5])
{
	const Stitch* st = s_on ? getStich(type, idx) : NULL;
	if (!st) return NULL;
	for (int i = 0; i < EXT_MAX; i++) {
		if (s_ext[i].used) continue;
		s_ext[i].used = true;
		wrapPlay(&s_ext[i].w, st, p);
		return &s_ext[i];
	}
	return NULL;
}

bool colsfxStitchPlaying(const ColsfxStitch* x)
{
	return x && x->used && x->w.playing;
}

void colsfxStitchUpdate(ColsfxStitch* x, const int32_t p[5])
{
	if (x && x->used) wrapUpdate(&x->w, p);
}

void colsfxStitchFree(ColsfxStitch* x)
{
	if (x && x->used) extFree(x);
}

void colsfxShutdown(void)
{
	carsim_remove_impulse_hook(onImpulse);
	if (s_cam) aemsRelease(s_cam);
	s_cam = NULL;
	s_camShot = false;
	for (int i = 0; i < EXT_MAX; i++)
		if (s_ext[i].used) extFree(&s_ext[i]);
	while (s_listN > 0) scDelete(s_list[s_listN - 1]);
	for (int i = 0; i < 3; i++) {
		if (s_bank[i]) aemsUnload(s_bank[i]);
		s_bank[i] = NULL;
	}
	free(s_bun);
	free(s_stich);
	s_bun = NULL;
	s_stich = NULL;
	s_stichN = 0;
	memset(s_typeAt, 0, sizeof(s_typeAt));
	memset(s_coll, 0, sizeof(s_coll));
	memset(s_snd, 0, sizeof(s_snd));
	memset(s_state, 0, sizeof(s_state));
	memset(s_newAttach, 0, sizeof(s_newAttach));
	s_queueN = s_queuedCount = s_numPlaying = 0;
	s_cWall = s_cSide = s_cFloor = s_cRoof = s_cNose = s_cBeside = 0;
	memset(s_cSmk, 0, sizeof(s_cSmk));
	memset(s_lastNormal, 0, sizeof(s_lastNormal));
	memset(s_lastPos, 0, sizeof(s_lastPos));
	s_on = false;
	s_player = NULL;
	s_now = 0.0f;
	s_paused = false;
	s_pausedFor = 0.0f;
	s_impacts = s_scrapes = s_impactLogs = s_doneLogs = 0;
}
