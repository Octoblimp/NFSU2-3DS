// The world's planes, trains and fountains (#509). See envsfx.h.
#include "envsfx.h"

#include "aems.h"
#include "dbglog.h"
#include "mixmap.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void envLog(const char* fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	dbglog_line("ENVSFX ", b);
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------- tables

// An animated thing that can sound: the WorldAnimInstance's id (+0) and its
// entity's node-name hash.
typedef struct { uint32_t id, hash; } AnimRef;

// CSTATEMGR_Planes' (0x803565CC.., PC 0x7ED690, the same).
static const AnimRef kPlanesDrag[] = {
	{ 1, 0x09123e81u }, { 3, 0x09123ec8u }, { 5, 0x09123ec0u }, { 7, 0x09123ec3u },
	{ 10, 0x09123ec5u },
};
static const AnimRef kPlanesMain[] = {   // PlanesOrganicDrag is the same six
	{ 34, 0xa1672f31u }, { 36, 0xa1672f32u }, { 38, 0xa1672f33u }, { 40, 0xa1672f2du },
	{ 42, 0xa1672f2fu }, { 44, 0xa1672f30u },
};
static const AnimRef kPlanesURL[] = {
	{ 1, 0x09123e81u }, { 2, 0x09123e82u }, { 3, 0x09123e83u },
};

// uTrainIDs (0x80356684, PC 0x7ED798). With the PC L4RA only instance 106's
// three cars are found; the rest fail GetAnimPosition and are parked, as in
// retail.
static const AnimRef kTrains[] = {
	{ 132, 0x863f23f3u }, { 132, 0x863f23f2u }, { 132, 0x863f23f1u },
	{ 126, 0x863f23f0u }, { 126, 0x863f23efu }, { 126, 0x863f23cbu },
	{ 110, 0x863f23ecu }, { 114, 0x863f23edu }, { 118, 0x863f23eeu },
	{ 106, 0x863f23ebu }, { 106, 0x863f23eau }, { 106, 0x863f23d2u },
	{ 113, 0x863f23ccu }, { 113, 0x863f23cdu }, { 113, 0x863f23ceu },
	{ 121, 0x863f23d1u }, { 121, 0x863f23d0u }, { 121, 0x863f23cfu },
	{ 16, 0x5289c4c8u }, { 16, 0x5289c4c9u }, { 16, 0x863f236bu },
	{ 16, 0x863f236du }, { 16, 0x863f236au }, { 16, 0x863f2387u },
};
#define TRAIN_N ((int)(sizeof(kTrains) / sizeof(kTrains[0])))

// CSTATEMGR_Enviro's two passes: SmallFountain (0x803A5668, 7) then
// FountainList (0x803A5478, 31), both written by the static init 0x800B6D38
// (PC .data 0x7F0D70 / 0x7F0B80, the same). FountainList[3]'s signs are
// flipped against its neighbours in both builds; kept.
static const float kFountains[][3] = {
	{ -154, -615, 16 }, { -337, -702, 19 }, { -457, -1518, 13 }, { -133, -1388, 15 },
	{ -2662, 2237, 229 }, { -1641, 532, 8 }, { -883, 806, 40 },
	{ -106.65f, -575.58f, 17.5f }, { -113.417f, -571.36f, 18.9f }, { -97.8f, -571.66f, 18.86f },
	{ 102.52f, 581.0f, 17.0f }, { -93.45f, -575.83f, 17.0f }, { -100.22f, -587.22f, 15.5f },
	{ -92.1f, -579.93f, 15.5f }, { 137, -642, 17 }, { -104, -653, 15 }, { -161, -624, 16 },
	{ -343, -561, 20 }, { -326, -55, 20 }, { -312, -564, 20 }, { -309, -534, 20 },
	{ -339, -530, 20 }, { -785, -285, 23 }, { -432, -112, 15 }, { -233, -283, 21 },
	{ -202, -280, 21 }, { -571, -154, 20 }, { -580, -163, 20 }, { -571, -143, 19 },
	{ -564, -126, 16 }, { -526, -27, 13 }, { 603, -177, 15 }, { -2134, 1687, 131 },
	{ -1500, 1143, 64 }, { -1214, 498, 19 }, { -813, 725, 36 }, { -841, 717, 36 },
	{ -123, 1499, 40 },
};
#define FOUNTAIN_SMALL 7
#define FOUNTAIN_N ((int)(sizeof(kFountains) / sizeof(kFountains[0])))

// ---------------------------------------------------------------- states

enum { KIND_PLANE, KIND_TRAIN, KIND_FOUNTAIN, KIND_N };

// A CSTATE_Plane / _Train / _Fountain with its SFXObj (one per mix-map copy).
typedef struct {
	bool      attached;    // +0x3C
	int       entry;       // +0x40, the table entry it plays
	bool      big;         // a fountain's +0x44: FountainList's (pass 1)
	float     pos[3];      // SFXObj +0xC0: where its controller's object is
	float     prev[3];     // +0xB0
	float     speed;       // +0xD0, m/s (retail: m per 60 Hz frame)
	AemsInst  env;         // ENV_STATIC
	AemsInst  mem;         // a plane's FX_MAIN_MEM
	int32_t   envData[4];
	int32_t   memData[5];
} EnvState;

typedef struct {
	const char*    name;
	const AnimRef* table;  // NULL: the fountains' fixed points
	int            n;
	float          radius;
	int            copies;
	uint32_t       sfx, ctl;  // copy 0's ids
	EnvState       st[MIXMAP_TRAIN_COPIES];
	uint8_t        status[TRAIN_N];  // the trains': 1 found, 2 parked till EnterWorld
} Mgr;

static Mgr s_mgr[KIND_N] = {
	{ .name = "plane", .radius = 2000.0f, .copies = MIXMAP_PLANE_COPIES,
	  .sfx = MIXMAP_SFX_PLANE, .ctl = MIXMAP_CTL_3DENVIROPOS },
	{ .name = "train", .table = kTrains, .n = TRAIN_N, .radius = 400.0f,
	  .copies = MIXMAP_TRAIN_COPIES, .sfx = MIXMAP_SFX_TRAIN, .ctl = MIXMAP_CTL_3DTRAINPOS },
	{ .name = "fountain", .n = FOUNTAIN_N, .radius = 150.0f,
	  .copies = MIXMAP_FOUNTAIN_COPIES, .sfx = MIXMAP_SFX_FOUNTAIN,
	  .ctl = MIXMAP_CTL_3DFOUNTAINPOS },
};

static bool        s_entered;
static EnvSfxWorld s_world;
static const WorldAnims* s_anims;
static bool        s_fountainsOn;   // the update's gate, track - 4000 < 200
static AemsBank*   s_memBank;       // IG_GLOBAL/FX_MAIN_MEM_MB

static bool planeUsesMem(void) { return s_world.url && !s_world.lanOutrun; }

static float dist3(const float a[3], const float b[3])
{
	const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
	return sqrtf(x * x + y * y + z * z);
}

// GetAnimPosition (0x80055E80) for an animated entry, the table's point for
// a fountain (never delaying).
static bool entryPos(const Mgr* m, int e, float pos[3], bool* delaying)
{
	if (!m->table) {
		memcpy(pos, kFountains[e], sizeof(float) * 3);
		*delaying = false;
		return true;
	}
	*delaying = false;
	return worldanim_position(s_anims, m->table[e].id, m->table[e].hash, pos, delaying);
}

static void letGo(AemsInst* inst)
{
	if (*inst) aemsRelease(*inst);
	*inst = NULL;
}

static AemsInst makeInst(const char* cls, int32_t* data, int n, int32_t m0)
{
	memset(data, 0, sizeof(int32_t) * (size_t)n);
	data[0] = m0;
	return aemsCreate(aemsClass(cls), data);
}

// The plane's: ENV_STATIC sample 1, or FX_MAIN_MEM in a URL race that is not
// a LAN Outrun.
static void planeCreate(EnvState* s)
{
	if (!planeUsesMem()) s->env = makeInst("ENV_STATIC", s->envData, 4, 1);
	else s->mem = makeInst("FX_MAIN_MEM", s->memData, 5, 0);
}

// NewAttachment (Plane 0x800A6AC8, Train 0x800A7794, Fountain 0x800A8C38).
static void attach(Mgr* m, int k, int c, int e)
{
	EnvState* s = &m->st[c];
	bool delaying;
	s->attached = true;
	s->entry = e;
	s->big = k == KIND_FOUNTAIN && e >= FOUNTAIN_SMALL;
	entryPos(m, e, s->pos, &delaying);
	mixmapSetObjPos(MIXMAP_COPY_ID(m->ctl, c), s->pos);
	switch (k) {
	case KIND_PLANE:   // prevPos keeps its last attachment's
		planeCreate(s);
		envLog("plane %d/%08x on copy %d at (%.0f, %.0f, %.0f)", (int)m->table[e].id,
			(unsigned)m->table[e].hash, c, s->pos[0], s->pos[1], s->pos[2]);
		break;
	case KIND_TRAIN:
		memcpy(s->prev, s->pos, sizeof(s->prev));
		s->env = makeInst("ENV_STATIC", s->envData, 4, 0);
		envLog("train %d/%08x on copy %d at (%.0f, %.0f, %.0f)", (int)m->table[e].id,
			(unsigned)m->table[e].hash, c, s->pos[0], s->pos[1], s->pos[2]);
		break;
	default:
		s->env = makeInst("ENV_STATIC", s->envData, 4, 4);   // "SND: Fountain"
		envLog("fountain %d (%s) on copy %d at (%.0f, %.0f, %.0f)", e,
			s->big ? "big" : "small", c, s->pos[0], s->pos[1], s->pos[2]);
		break;
	}
}

// DetachOneShot: the sounds let go, AssignPositionVector(NULL).
static void detach(Mgr* m, int c)
{
	EnvState* s = &m->st[c];
	if (!s->attached) return;
	letGo(&s->env);
	letGo(&s->mem);
	s->attached = false;
	mixmapSetObjPos(MIXMAP_COPY_ID(m->ctl, c), NULL);
}

static void detachAll(void)
{
	for (int k = 0; k < KIND_N; k++)
		for (int c = 0; c < MIXMAP_TRAIN_COPIES; c++) detach(&s_mgr[k], c);
}

// The three EnterWorlds (0x800AAD5C, 0x800AB270, 0x800AB6B0).
static void enterWorld(const EnvSfxWorld* w, const WorldAnims* anims)
{
	detachAll();
	s_world = *w;
	s_anims = anims;
	s_entered = true;
	Mgr* p = &s_mgr[KIND_PLANE];
	const bool organic = w->track >= 4400 && w->track <= 4499;
	const char* which;
	if (w->drag && !organic) {
		p->table = kPlanesDrag, p->n = 5, which = "PlanesDrag";
	} else if (w->drag) {
		p->table = kPlanesMain, p->n = 6, which = "PlanesOrganicDrag";
	} else if (w->url) {
		p->table = kPlanesURL, p->n = 3, which = "PlanesURL";
	} else {
		p->table = kPlanesMain, p->n = 6, which = "PlanesMain";
	}
	memset(s_mgr[KIND_TRAIN].status, 0, sizeof(s_mgr[KIND_TRAIN].status));
	// Enviro makes its fountains only for track - 4000 < 199 and runs its
	// update only for < 200.
	s_mgr[KIND_FOUNTAIN].copies = w->track - 4000 < 199 ? MIXMAP_FOUNTAIN_COPIES : 0;
	s_fountainsOn = w->track - 4000 < 200;
	envLog("EnterWorld: track %d%s%s%s -- %s, %s, %d fountain states; %s", w->track,
		w->drag ? " drag" : "", w->url ? " URL" : "", w->lanOutrun ? " LAN Outrun" : "",
		which, planeUsesMem() ? "FX_MAIN_MEM" : "ENV_STATIC", s_mgr[KIND_FOUNTAIN].copies,
		anims ? "world anims loaded" : "no world anims (planes and trains silent)");
}

// CSTATEMGR_Planes / _Trains / _Enviro::UpdateParams (0x800AAEB0, 0x800AB304,
// 0x800AB734). The one player's car is every GetClosestPlayerCar.
static void mgrUpdate(Mgr* m, int k, const float car[3])
{
	for (int e = 0; e < m->n; e++) {
		if (k == KIND_TRAIN && m->status[e] == 2) continue;
		float pos[3];
		bool delaying;
		if (!entryPos(m, e, pos, &delaying)) {
			if (k == KIND_TRAIN) m->status[e] = 2;
			continue;
		}
		if (k == KIND_TRAIN) m->status[e] = 1;
		const float d = dist3(car, pos);
		const bool inR = d < m->radius;
		int c = -1;
		for (int i = 0; i < m->copies; i++)
			if (m->st[i].attached && m->st[i].entry == e) { c = i; break; }
		if (c >= 0) {
			if (!inR || delaying) detach(m, c);
			continue;
		}
		if (!inR || delaying) continue;
		// GetFreeState, else steal the first attached state that is farther
		// from the car than this entry.
		for (int i = 0; i < m->copies && c < 0; i++)
			if (!m->st[i].attached) c = i;
		for (int i = 0; i < m->copies && c < 0; i++) {
			float tpos[3];
			bool tdelay;
			if (!entryPos(m, m->st[i].entry, tpos, &tdelay)) continue;
			if (d < dist3(car, tpos)) {
				detach(m, i);
				c = i;
			}
		}
		if (c >= 0) attach(m, k, c, e);
	}
}

// Every 10 s, for the log: each attached state, its sound and its level.
static void statusLog(float dt)
{
	static float sec;
	sec += dt;
	if (sec < 10.0f) return;
	sec = 0.0f;
	char b[256];
	int n = 0;
	for (int k = 0; k < KIND_N; k++) {
		const Mgr* m = &s_mgr[k];
		for (int c = 0; c < m->copies; c++) {
			const EnvState* s = &m->st[c];
			if (!s->attached || n > (int)sizeof(b) - 48) continue;
			const int slot = k == KIND_FOUNTAIN && s->big ? 1 : (s->mem ? 1 : 0);
			n += snprintf(b + n, sizeof(b) - (size_t)n, " %s%d:%s q%d", m->name, c,
				s->env ? "env" : s->mem ? "mem" : "-",
				(int)mixmapQ15(MIXMAP_COPY_ID(m->sfx, c), slot));
		}
	}
	envLog("attached:%s", n ? b : " none");
}

// ---------------------------------------------------------------- public

void envsfxSetup(const char* audioDir, int bus, bool url)
{
	envsfxShutdown();
	if (!url) return;
	char path[512];
	snprintf(path, sizeof(path), "%sIG_GLOBAL/FX_MAIN_MEM_MB.aem", audioDir);
	s_memBank = aemsLoad(path, bus);
	envLog(s_memBank ? "FX_MAIN_MEM_MB loaded (the URL planes)"
		: "FX_MAIN_MEM_MB: no %s -- the URL planes silent (tools/abk3ds.py)", path);
}

void envsfxShutdown(void)
{
	detachAll();
	s_entered = false;
	s_anims = NULL;
	if (s_memBank) aemsUnload(s_memBank);
	s_memBank = NULL;
}

void envsfxPreMix(const EnvSfxWorld* w, const WorldAnims* anims, const float carPos[3],
	float dt)
{
	if (!w || !carPos) return;
	if (!s_entered || anims != s_anims || w->track != s_world.track ||
	    w->drag != s_world.drag || w->url != s_world.url || w->lanOutrun != s_world.lanOutrun)
		enterWorld(w, anims);
	mgrUpdate(&s_mgr[KIND_PLANE], KIND_PLANE, carPos);
	mgrUpdate(&s_mgr[KIND_TRAIN], KIND_TRAIN, carPos);
	if (s_fountainsOn) mgrUpdate(&s_mgr[KIND_FOUNTAIN], KIND_FOUNTAIN, carPos);
	statusLog(dt);
	// Each attached state's SFX UpdateParams: a plane's and a train's
	// position off the animation, and the plane's speed.
	for (int k = 0; k < KIND_FOUNTAIN; k++) {
		Mgr* m = &s_mgr[k];
		for (int c = 0; c < m->copies; c++) {
			EnvState* s = &m->st[c];
			if (!s->attached) continue;
			bool delaying;
			if (entryPos(m, s->entry, s->pos, &delaying) && dt > 0.0f) {
				s->speed = dist3(s->pos, s->prev) / dt;
				memcpy(s->prev, s->pos, sizeof(s->prev));
			}
			mixmapSetObjPos(MIXMAP_COPY_ID(m->ctl, c), s->pos);
		}
	}
}

// 0.3 m per 60 Hz frame (0x8031F2E0), as a speed.
#define PLANE_SILENT_SPEED (0.3f * 60.0f)

void envsfxUpdate(void)
{
	if (!s_entered) return;
	// SFXObj_Plane::ProcessUpdate (0x800A6F8C).
	Mgr* m = &s_mgr[KIND_PLANE];
	for (int c = 0; c < m->copies; c++) {
		EnvState* s = &m->st[c];
		if (!s->attached) continue;
		if (s->speed >= PLANE_SILENT_SPEED) {
			if (!s->env && !s->mem) planeCreate(s);
		} else {
			letGo(&s->env);
			letGo(&s->mem);
		}
		const uint32_t sfx = MIXMAP_COPY_ID(m->sfx, c), ctl = MIXMAP_COPY_ID(m->ctl, c);
		const float dop = mixmapDoppler(ctl, MIXMAP_DOPPLER_PLANE);
		if (s->mem) {
			const int32_t az = clampi(mixmapAzim(sfx, 1), 0, 0xffff);
			const int32_t* out = mixmapObjOut(ctl);
			float d = out ? (float)out[1] : 0.0f;
			if (d > 1000.0f) d = 1000.0f;
			int32_t sd = clampi((int32_t)(d * 2.6f), -1000, 1000);
			if (az > 0x4000 && az < 0xc000) sd = -sd;
			s->memData[0] = 0;
			s->memData[1] = clampi(mixmapQ15(sfx, 1), 0, 0x7fff);
			s->memData[2] = clampi((int32_t)(dop * 4096.0f), 0, 0x4000);
			s->memData[3] = az;
			s->memData[4] = sd;
			aemsSetMemberData(s->mem, s->memData);
		}
		if (s->env) {
			s->envData[0] = 1;
			s->envData[1] = clampi(mixmapQ15(sfx, 0), 0, 0x7fff);
			s->envData[2] = clampi((int32_t)(dop * 4096.0f), 0, 0x1fa0);
			s->envData[3] = clampi(mixmapAzim(sfx, 0), 0, 0xffff);
			aemsSetMemberData(s->env, s->envData);
		}
	}
	// SFXObj_Train::ProcessUpdate (0x800A7AC0): no Doppler.
	m = &s_mgr[KIND_TRAIN];
	for (int c = 0; c < m->copies; c++) {
		EnvState* s = &m->st[c];
		if (!s->attached || !s->env) continue;
		const uint32_t sfx = MIXMAP_COPY_ID(m->sfx, c);
		s->envData[0] = 0;
		s->envData[1] = clampi(mixmapQ15(sfx, 0), 0, 0x7fff);
		s->envData[2] = 0x1000;
		s->envData[3] = clampi(mixmapAzim(sfx, 0), 0, 0xffff);
		aemsSetMemberData(s->env, s->envData);
	}
	// SFXObj_Fountain::ProcessUpdate (0x800A8A58): a small one on slot 0 at
	// pitch 0x1450, a big one on slot 1 at 0xFA0.
	m = &s_mgr[KIND_FOUNTAIN];
	for (int c = 0; c < m->copies; c++) {
		EnvState* s = &m->st[c];
		if (!s->attached || !s->env) continue;
		const uint32_t sfx = MIXMAP_COPY_ID(m->sfx, c);
		const int slot = s->big ? 1 : 0;
		s->envData[0] = 4;
		s->envData[1] = clampi(mixmapQ15(sfx, slot), 0, 0x7fff);
		s->envData[2] = s->big ? 0xfa0 : 0x1450;
		s->envData[3] = clampi(mixmapAzim(sfx, slot), 0, 0xffff);
		aemsSetMemberData(s->env, s->envData);
	}
}
