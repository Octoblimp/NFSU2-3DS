#include "style.h"
#include "bunidx.h"
#include "dbglog.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// StyleMomentsInfo (LoaderStyleMomentsInfo 0x8018D960): 39 records of 0x30
// bytes from the chunk's 16-byte aligned payload. Little-endian on the PC.
#define STYLE_CHUNK_TYPE 0x00034A07u
#define STYLE_RECORD     0x30

#define TICKS_PER_SEC 4000.0f
#define SEC_PER_TICK  0.00025f

typedef struct {
	u32   nameHash;     // +0x00, the moment's name in English.bin
	u8    type;         // +0x04
	u8    group;        // +0x05
	u8    continuous;   // +0x06: points while it lasts, else one award
	u8    mask;         // +0x07, the race kinds it runs in
	float hold;         // +0x08, s before a continuous one counts
	float timeout;      // +0x0C, s without a trigger before it ends
	float threshold;    // +0x10, points it has to pass
	float cap;          // +0x14, 0 for none
	float maxMult;      // +0x18
	float multStep;     // +0x1C
	float rate;         // +0x28, points a second (or per award)
	s8    maxCount;     // +0x2C, -1 unlimited
	char  name[24];
} StyleInfo;

// eStyleMomentState.
enum { ST_ACTIVE, ST_ACTIVATING, ST_INACTIVE, ST_DONE, ST_ABORTED };

// StyleMoment (0x58 bytes), its fields at retail's offsets.
typedef struct {
	const StyleInfo* info;  // +0x08
	bool  made;             // the accumulator's slot is filled (and +0x10)
	int   state;            // +0x14
	int   counts;           // +0x18, banked points count to the total
	float mult;             // +0x1C
	int   count;            // +0x20
	float points;           // +0x24
	float intensity;        // +0x28
	float banked;           // +0x2C
	int   flag;             // +0x30
	s32   lastTrig;         // +0x34
	s32   startTime;        // +0x38
	float raw;              // +0x3C
	float maxDur, maxPts;   // +0x40 / +0x44
	float startVal, curVal; // +0x48 / +0x4C
	float maxDelta;         // +0x50
	float maxIntensity;     // +0x54
} StyleMoment;

// The collisions the style listens to, Collision's fields: +0x08 when it
// began, +0x0C its last impulse, +0x48 every impulse's size summed. Done
// 0.3 s past its last impulse (Collision::Update), as colsfx.c keeps them.
#define COLL_MAX 8
#define COLL_LIFE 0.3f
typedef struct {
	bool used;
	const CarSim* a;
	const CarSim* b;
	const CarSimProp* prop;   // a knocked prop's (smokeable.c)
	u32 serial;
	bool ground;
	s32 start, last;
	float mag;
} StyleColl;

#define SHORTCUT_MAX 64

static StyleInfo s_info[STYLE_TYPE_COUNT];
static bool s_loaded;

static bool s_on;
static const CarSim* s_player;
static s32 s_now;
static s32 s_begin;         // style_begin's WorldTimer, free roam's race clock
static float s_raceSec;     // this frame's Race +0x14, seconds
static float s_lastPts;     // RacingCar [0x65], the points the bottle has had
static float s_dist;        // Car +0x420, metres driven while racing
static float s_burn;        // RacingCarStats [10]: -1, armed 0, or the distance
static bool s_hadLap;       // LapCounter +0x68
static StyleColl s_coll[COLL_MAX];
// The last frame's traffic cars (StyleFrame.traffic): Car::GetDriverInfo's
// type 3 for RegisterCollisionImpulse, which runs inside the car's step.
#define STYLE_TRAFFIC_MAX 24
static const CarSim* s_traffic[STYLE_TRAFFIC_MAX];
static int s_trafficN;
static bool s_finished;     // the last frame's RacingCar::IsFinishedRacing
static StyleUp s_up[STYLE_TYPE_COUNT];   // Update's list for FEUpdateStylePoints
static int s_upN;
static const TrackPathZone* s_shortcut[SHORTCUT_MAX];
static int s_shortcutN;

// MiscStyleManager (0x4C bytes), its fields at retail's word index.
typedef struct {
	int   cleanSection;   // [2]
	int   cleanLap;       // [3]
	int   clean4;         // [4]
	bool  progEdge;       // [5]
	bool  lapEdge;        // [6]
	int   lastPos;        // [7]
	s8    spin;           // [8], the 360's half-turns
	float spinMark;       // [9], degrees
	s8    jturn;          // [10]
	s8    near;           // +0x29
	int   nearKind;       // [0xb]: 0 NEARMISS, 1 LEAPFROG
	int   nearWith;       // [0xc..0xe], the witness: the other car's index
	s32   lastColl;       // [0xf]
	float lastImpulse;    // [0x10]
	int   lastCollType;   // [0x11]
	bool  trafficHit;     // [0x12]
} MiscStyle;

// One RacingCar's StylePointsAccumulator ([0x75]) and MiscStyleManager
// ([0x77]). Slot 0 is the player's; in a drift every AI racer has one too
// (g_tweakAIDriftOpponents, 1 on the disc), made by style_ai_add.
#define STYLE_CARS 8
typedef struct {
	const CarSim* car;
	StyleMoment mom[STYLE_TYPE_COUNT];
	float total;          // StylePointsAccumulator [0x27]
	MiscStyle msm;
} StyleCar;
static StyleCar s_cars[STYLE_CARS];
static int s_carN;
static u8 s_kind;           // style_begin's race kind, the AI's moments too
// The car the moments and the collision fields below are read and driven on:
// the player's but while style_select has picked another.
static StyleCar* S = &s_cars[0];

static void styleLog(const char* fmt, ...)
{
	char b[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	dbglog_line("style: ", b);
}

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static float rdf32le(const u8* p) {
	u32 bits = rd32le(p);
	float out;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

static bool tickSet(s32 t) { return t != 0 && t != 0x7FFFFFFF; }
static float secSince(s32 t) { return (float)(s_now - t) * SEC_PER_TICK; }

bool style_load(const char* bunPath, const LangTable* lang)
{
	s_loaded = false;
	long hdr = -1;
	u32 size = 0;
	if (!bunPath || !bunidx_find(bunPath, STYLE_CHUNK_TYPE, &hdr, &size) || hdr < 0) {
		styleLog("no 0x34A07 chunk in GLOBALB.BUN -- no style points");
		return false;
	}
	const long base = (hdr + 0x17) & ~0xFL;
	static u8 buf[STYLE_TYPE_COUNT * STYLE_RECORD];
	FILE* f = fopen(bunPath, "rb");
	const bool ok = f && fseek(f, base, SEEK_SET) == 0 &&
		fread(buf, 1, sizeof(buf), f) == sizeof(buf) && (long)size + hdr + 8 >= base + (long)sizeof(buf);
	if (f)
		fclose(f);
	if (!ok) {
		styleLog("0x34A07 chunk short or unreadable -- no style points");
		return false;
	}
	for (int i = 0; i < STYLE_TYPE_COUNT; i++) {
		const u8* r = buf + i * STYLE_RECORD;
		StyleInfo* in = &s_info[i];
		in->nameHash = rd32le(r);
		in->type = r[4];
		in->group = r[5];
		in->continuous = r[6];
		in->mask = r[7];
		in->hold = rdf32le(r + 0x08);
		in->timeout = rdf32le(r + 0x0C);
		in->threshold = rdf32le(r + 0x10);
		in->cap = rdf32le(r + 0x14);
		in->maxMult = rdf32le(r + 0x18);
		in->multStep = rdf32le(r + 0x1C);
		in->rate = rdf32le(r + 0x28);
		in->maxCount = (s8)r[0x2C];
		const char* s = lang ? langbin_lookup(lang, in->nameHash) : NULL;
		if (s && *s)
			snprintf(in->name, sizeof(in->name), "%s", s);
		else
			snprintf(in->name, sizeof(in->name), "#%d", i);
	}
	s_loaded = true;
	styleLog("%d moments from GLOBALB.BUN (POWERSLIDE rate %.0f, hold %.2f s; HANGTIME rate %.0f)",
		STYLE_TYPE_COUNT, s_info[STYLE_POWERSLIDE].rate, s_info[STYLE_POWERSLIDE].hold,
		s_info[STYLE_HANGTIME].rate);
	return true;
}

// ---------------------------------------------------------------- StyleMoment

// IncMultiplier (0x8018E40C).
static void incMultiplier(StyleMoment* m) {
	if (m->mult < m->info->maxMult)
		m->mult += m->info->multStep;
}

// SetState (0x8018E000): leaving DONE counts it and steps the multiplier;
// entering DONE keeps its records.
static void setState(StyleMoment* m, int n) {
	if (m->state == n)
		return;
	if (m->state == ST_DONE) {
		m->count++;
		incMultiplier(m);
	}
	if (n == ST_DONE) {
		const float delta = m->curVal - m->startVal;
		const float dur = secSince(m->startTime);
		if (m->maxDur < dur) m->maxDur = dur;
		if (m->maxPts < m->points) m->maxPts = m->points;
		if (m->maxDelta < delta) m->maxDelta = delta;
	}
	m->state = n;
}

// CalculateNewPoints (0x8018E0D8): `dt` more of the rate, held to the cap
// (a ceiling when it is above 0, a floor below), and the raw count the
// threshold is read against.
static float calcNewPoints(StyleMoment* m, float dt) {
	const StyleInfo* in = m->info;
	const float step = in->rate * dt;
	const float p = step * m->mult * m->intensity + m->points;
	float r = p;
	if (in->cap > 0.0f) {
		if (in->cap - p < 0.0f) r = in->cap;
	} else if (in->cap < 0.0f) {
		r = in->cap;
		if (in->cap - p < 0.0f) r = p;
	}
	float inc = step * m->intensity;
	if (r - inc < 0.0f) inc = r;
	m->raw += inc;
	return r;
}

// Reset (0x8018E33C).
static void momentReset(StyleMoment* m) {
	m->maxIntensity = 0.0f;
	m->intensity = 1.0f;
	m->lastTrig = 0;
	m->mult = 1.0f;
	m->count = 0;
	m->points = 0.0f;
	m->raw = 0.0f;
	m->banked = 0.0f;
	m->maxDur = 0.0f;
	m->maxPts = 0.0f;
	m->flag = 0;
	m->startVal = m->curVal = m->maxDelta = 0.0f;
	m->startTime = 0;
	setState(m, ST_INACTIVE);
}

// ForceDone (0x8018E2CC): the points, whole, are banked.
static void forceDone(StyleMoment* m, int flag) {
	m->flag = flag;
	m->banked += (float)(int)m->points;
	setState(m, ST_DONE);
	if (S == &s_cars[0])
		styleLog("%s +%d (x%.1f, %d so far)", m->info->name, (int)m->points, m->mult,
			(int)m->banked);
	else
		styleLog("car slot %d: %s +%d (x%.1f, %d so far)", (int)(S - s_cars),
			m->info->name, (int)m->points, m->mult, (int)m->banked);
}

// Cancel (0x8018E26C): one still building up is dropped; one running is
// ABORTED, its points lost.
static void momentCancel(StyleMoment* m, int flag) {
	if (m->state == ST_ACTIVATING) {
		m->flag = 0;
		m->raw = 0.0f;
		m->points = 0.0f;
		setState(m, ST_INACTIVE);
	} else {
		m->flag = flag;
		setState(m, ST_ABORTED);
		if (S == &s_cars[0])
			styleLog("%s aborted (%d lost)", m->info->name, (int)m->points);
		else
			styleLog("car slot %d: %s aborted (%d lost)", (int)(S - s_cars),
				m->info->name, (int)m->points);
	}
}

// Deactivate (0x8018E3B4): back to INACTIVE, or a frame later with the flag.
static void deactivate(StyleMoment* m) {
	if (m->flag == 0) {
		m->startTime = 0;
		m->raw = 0.0f;
		m->points = 0.0f;
		setState(m, ST_INACTIVE);
	} else {
		m->flag = 0;
	}
}

// Update (0x8018DE74).
static void momentUpdate(StyleMoment* m, float dt) {
	const StyleInfo* in = m->info;
	const float el = secSince(m->lastTrig);
	if (in->timeout < el) {
		if (m->state == ST_ACTIVE) {
			forceDone(m, 0);
		} else if (m->state == ST_ACTIVATING) {
			momentCancel(m, 0);
			deactivate(m);
		}
	}
	if ((unsigned)m->state < 2u && el < dt + dt)
		m->points = calcNewPoints(m, dt);
	if (m->state == ST_ACTIVATING && in->hold < secSince(m->startTime) &&
	    ((in->rate < 0.0f && m->raw < in->threshold) ||
	     (in->rate > 0.0f && in->threshold < m->raw))) {
		m->count++;
		setState(m, ST_ACTIVE);
	}
}

// Trigger (0x8018E158): a one-off moment is awarded on the spot if it clears
// its threshold; a continuous one starts building up.
static void momentTrigger(StyleMoment* m, float intensity, float value) {
	const StyleInfo* in = m->info;
	if (!((in->maxCount < 0 || m->count < in->maxCount) && (unsigned)(m->state - ST_DONE) > 1u))
		return;
	m->intensity = intensity;
	m->curVal = value;
	m->lastTrig = s_now;
	if (m->maxIntensity < intensity)
		m->maxIntensity = intensity;
	if (!in->continuous) {
		const float p = calcNewPoints(m, 1.0f);
		if ((in->rate < 0.0f && p < in->threshold) || (in->rate > 0.0f && in->threshold < p)) {
			m->points = p;
			setState(m, ST_ACTIVE);
			forceDone(m, 0);
		}
	} else if (m->state == ST_INACTIVE) {
		m->startVal = value;
		m->startTime = s_now;
		setState(m, ST_ACTIVATING);
	}
}

// ---------------------------------------------------------------- the accumulator

static StyleMoment* moment(int type) {
	StyleMoment* m = &S->mom[type];
	return (s_on && m->made) ? m : NULL;
}

// TriggerMoment (0x8018E848).
static void trigger(int type, float intensity, float value) {
	StyleMoment* m = moment(type);
	if (m)
		momentTrigger(m, intensity, value);
}

// AbortMoment (0x8018E884).
static void abortMoment(int type) {
	StyleMoment* m = moment(type);
	if (m && (m->state == ST_ACTIVE || m->state == ST_ACTIVATING))
		momentCancel(m, 1);
}

// ClearMultiplier (0x8018E8D8).
static void clearMultiplier(int type) {
	StyleMoment* m = moment(type);
	if (m)
		m->mult = 1.0f;
}

// Update (0x8018E634): the total of what is banked and the moments that are
// up (ACTIVE, DONE or ABORTED, no flag) listed for FEUpdateStylePoints, then
// every moment that finished last frame put back and every moment stepped.
// PORT: the list is kept for the HUD to take (style_take_up) rather than
// handed to it here; the HUD draws later in the frame. Only the player's
// accumulator has one (retail's FEUpdateStylePoints is the player's HUD).
static void accumulatorUpdate(float dt) {
	const bool hud = S == &s_cars[0];
	S->total = 0.0f;
	if (hud)
		s_upN = 0;
	for (int i = 0; i < STYLE_TYPE_COUNT; i++) {
		const StyleMoment* m = &S->mom[i];
		if (!m->made)
			continue;
		if (hud && (m->state == ST_ACTIVE || m->state == ST_DONE || m->state == ST_ABORTED) &&
				m->flag == 0) {
			StyleUp* u = &s_up[s_upN++];
			u->type = i;
			u->state = m->state;
			u->mult = m->mult;
			u->points = m->points;
			u->nameHash = m->info->nameHash;
			u->name = m->info->name;
			u->continuous = m->info->continuous != 0;
		}
		if (m->counts)
			S->total += (float)(int)m->banked;
	}
	for (int i = 0; i < STYLE_TYPE_COUNT; i++) {
		StyleMoment* m = &S->mom[i];
		if (!m->made)
			continue;
		if (m->state == ST_DONE || m->state == ST_ABORTED)
			deactivate(m);
		momentUpdate(m, dt);
	}
}

// ---------------------------------------------------------------- collisions

// RegisterCollisionImpulse (0x80191398), which Collision::AddImpulse calls
// for a car of the collision that has a MiscStyleManager: a real knock in its
// first 0.1 s dirties the clean run and starts the time since the last hit.
// A two-car knock with traffic in it (driver type 3) is TRAFFIC_HIT, at
// min(impulse / 40, 1) unless the race is over, and clears NEARMISS's
// multiplier as any two-car knock does. On S, the car whose manager it is.
static bool isTraffic(const CarSim* car) {
	for (int i = 0; car && i < s_trafficN; i++)
		if (s_traffic[i] == car)
			return true;
	return false;
}

static void registerCollisionImpulse(const StyleColl* c) {
	if (c->mag < 3.0f || !(secSince(c->start) <= 0.1f))
		return;
	const CarSim* me = S->car;
	const bool twoCar = c->b != NULL;
	// IsWallInvolved (0x801AC890): the track's wallish polygons, so not a prop.
	const bool wall = !twoCar && !c->ground && !c->prop;
	if (!(wall || twoCar || me->wheelsDown < 1 || me->rot[2][2] <= 0.9f))
		return;
	if (c->mag > 10.0f)
		style_dirty_clean_run();
	S->msm.trafficHit = false;
	int type = 3;
	if (twoCar) {
		if (isTraffic(c->a) || isTraffic(c->b)) {
			float in = c->mag / 40.0f;
			if (1.0f - in < 0.0f)
				in = 1.0f;
			if (!s_finished)
				trigger(STYLE_TRAFFIC_HIT, in, 0.0f);
			styleLog("TRAFFIC_HIT, impulse %.1f", c->mag);
			S->msm.trafficHit = true;
		}
		clearMultiplier(STYLE_NEARMISS);
		type = 0;
	}
	S->msm.lastCollType = type;
	S->msm.lastColl = s_now;
	S->msm.lastImpulse = c->mag;
}

static StyleCar* carSlot(const CarSim* car) {
	for (int i = 0; car && i < s_carN; i++)
		if (s_cars[i].car == car)
			return &s_cars[i];
	return NULL;
}

// carsim's impulse hook: the collisions of the cars with a MiscStyleManager,
// summed as Collision keeps them, each registered with every such car in it
// (Collision::AddImpulse). Times are the player's WorldTimer, which has
// moved on this frame.
static void onImpulse(const CarSim* car, const CarSim* other, CarSimProp* prop, bool ground,
		const float pos[3], const float imp[3], float slip)
{
	(void)pos;
	(void)slip;
	StyleCar* const ca = carSlot(car);
	StyleCar* const cb = carSlot(other);
	if (!s_on || !car || (!ca && !cb))
		return;
	s_now = (s32)(s_player->clock * TICKS_PER_SEC);
	StyleColl* c = NULL;
	StyleColl* spare = NULL;
	for (int i = 0; i < COLL_MAX; i++) {
		StyleColl* k = &s_coll[i];
		if (!k->used) {
			if (!spare) spare = k;
			continue;
		}
		const bool same = other ? ((k->a == car && k->b == other) || (k->a == other && k->b == car))
		                : prop ? (k->a == car && k->prop == prop && k->serial == prop->serial)
		                       : (k->a == car && !k->b && !k->prop && k->ground == ground);
		if (same) {
			c = k;
			break;
		}
	}
	if (!c) {
		if (!spare)
			return;
		c = spare;
		memset(c, 0, sizeof(*c));
		c->used = true;
		c->a = car;
		c->b = other;
		c->prop = prop;
		c->serial = prop ? prop->serial : 0;
		c->ground = ground;
		c->start = s_now;
	}
	c->mag += sqrtf(imp[0] * imp[0] + imp[1] * imp[1] + imp[2] * imp[2]);
	c->last = s_now;
	StyleCar* const prev = S;
	if (ca) {
		S = ca;
		registerCollisionImpulse(c);
	}
	if (cb) {
		S = cb;
		registerCollisionImpulse(c);
	}
	S = prev;
}

// ---------------------------------------------------------------- MiscStyleManager

// GetTimeSinceLastCollision (0x80190378).
static float timeSinceCollision(void) {
	return tickSet(S->msm.lastColl) ? secSince(S->msm.lastColl) : 99999.9f;
}

// DetectCleanRun (0x80190408): each new section and lap without a hard knock
// is a clean one, and a lap ended in the lead is LEADLAP -- in a drift, a lap
// ended anywhere on DriftManager's leaderboard (GetLeaderBoardPos != 0), which
// is every lap once the board is built: retail's test, kept as it is.
static void detectCleanRun(const StyleFrame* f) {
	if (!f->newProgress || S->msm.progEdge) {
		S->msm.progEdge = f->newProgress;
	} else {
		S->msm.progEdge = true;
		if (!S->msm.cleanSection) {
			S->msm.cleanSection = 1;
			clearMultiplier(STYLE_CLEANSECTION);
		} else {
			trigger(STYLE_CLEANSECTION, 1.0f, 0.0f);
		}
	}
	if (!f->newLap || S->msm.lapEdge) {
		S->msm.lapEdge = f->newLap;
	} else {
		S->msm.lapEdge = true;
		if (!S->msm.cleanLap) {
			S->msm.cleanLap = 1;
			clearMultiplier(STYLE_CLEANLAP);
		} else {
			trigger(STYLE_CLEANLAP, 1.0f, 0.0f);
		}
		if (f->drift ? f->boardPos != 0 : f->position == 1)
			trigger(STYLE_LEADLAP, 1.0f, 0.0f);
		else
			clearMultiplier(STYLE_LEADLAP);
	}
}

// TopologyCoordinate::GetElevation under the car. PORT: nothing under it at
// all is read as no height rather than retail's -10000, so a section still
// paging in cannot hand out air time.
static bool groundUnder(const CarSim* s, const CarSimWorld* w, float* z) {
	if (!w || !w->groundProbe)
		return false;
	const float xy[2] = { s->rbPos[0], s->rbPos[1] };
	float n[3];
	bool hit = false;
	return w->groundProbe(w->ctx, xy, 1, s->rbPos[2], z, &hit, n, NULL) > 0 && hit;
}

// DetectAirTime (0x801905C8): racing with no wheel and nothing else touching
// and the car's origin 0.75 m clear of the road, and not bouncing on its
// hydraulics (AreHydraulicsBouncing, #313).
static void detectAirTime(const CarSim* s, const StyleFrame* f) {
	const StyleMoment* hang = moment(STYLE_HANGTIME);
	if (f->stage != 4 || f->finished || !hang || carsim_hyd_bouncing(s))
		return;
	if (s->wheelsDown != 0 || s->contacts != 0 || !(s->rot[2][2] > 0.8f || hang->state == ST_ACTIVE))
		return;
	if (!(timeSinceCollision() > 0.3f || hang->state == ST_ACTIVE))
		return;
	float ground;
	if (groundUnder(s, f->world, &ground) && (s->rbPos[2] - s->cg[2]) - ground > 0.75f)
		trigger(STYLE_HANGTIME, 1.0f, 0.0f);
}

// MiscStyleManager::DetectHydraulics (0x80191188, TASKS.md #313): two or more
// corners kicked by a bounce this frame are a Hydraulic Bounce; a corner
// rising or lowering while nothing bounces is a Hydraulic Extend worth a
// quarter a corner.
static void detectHydraulics(const CarSim* s) {
	int nb = 0, ne = 0;
	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		if (s->hydFired[i]) nb++;
		if (!s->mellow && (s->hydState[i] == 2 || s->hydState[i] == 3)) ne++;
	}
	if (nb >= 2)
		trigger(STYLE_HYDRAULIC_BOUNCE, 1.0f, 0.0f);
	if (ne > 0 && !carsim_hyd_bouncing(s))
		trigger(STYLE_HYDRAULIC_EXTEND, (float)ne * 0.25f, 0.0f);
}

// DetectPowerSlide (0x80190BF4): more than 5 degrees off the direction of
// travel at over 40 mph on four wheels, worth more the faster and wider.
static void detectPowerSlide(const CarSim* s) {
	const float yaw = fabsf(carsim_yaw(s));
	float intensity = 1.0f;
	bool valid = false;
	if (yaw > 5.0f * 0.0174533f && s->speedAbs > 17.88109f && s->wheelsDown == 4) {
		intensity = s->speedAbs * yaw / 20.0f;
		valid = s->contacts == 0;
		if (1.0f - intensity < 0.0f)
			intensity = 1.0f;
	}
	if (timeSinceCollision() >= 0.1f || S->msm.lastImpulse <= 5.0f) {
		if (valid)
			trigger(STYLE_POWERSLIDE, intensity, s_dist);
	} else {
		abortMoment(STYLE_POWERSLIDE);
	}
}

// Car +0x41C, the heading as bFastATan's u16, in degrees.
static float headingDeg(const CarSim* s) {
	const float a = atan2f(s->rot[0][1], s->rot[0][0]);
	const u16 h = (u16)(s32)(a * (65536.0f / (2.0f * (float)M_PI)));
	return (float)h * 0.0054931640625f;
}

// Detect360 (0x80190D04): three marks at least 170 degrees apart while the
// car spins on four wheels.
static void detect360(const CarSim* s) {
	const float wz = fabsf(s->w[2]);
	const float hdg = headingDeg(s);
	const bool spinning = s->wheelsDown == 4 &&
		((S->msm.spin == 0 && wz > 2.0f) || (S->msm.spin > 0 && wz > 1.0f));
	if (!spinning) {
		S->msm.spin = 0;
	} else {
		const s8 c = S->msm.spin;
		bool mark = true;
		if (c != 0) {
			if (c < 0 || c > 2) {
				mark = false;
			} else {
				const float d = fabsf(hdg - S->msm.spinMark);
				float w = 360.0f - d;
				if (d - w < 0.0f) w = d;
				if (fabsf(w) < 170.0f) mark = false;
			}
		}
		if (mark) {
			S->msm.spinMark = hdg;
			S->msm.spin = c + 1;
		}
	}
	if (S->msm.spin == 3) {
		trigger(STYLE_360, 1.0f, 0.0f);
		S->msm.spin = 0;
	}
}

// DetectNearMiss (0x80190760): a car passed within 1.5 m at a closing speed
// over 80 mph (NEARMISS), or cleared from over 1 m above it while airborne
// (LEAPFROG), awarded once the two are 1.875 m apart again. PORT: retail's
// CollisionBodyWitness list is the bodies whose boxes overlap, with the GJK
// distance between them; here a witness is another car within 1.875 m of
// plan gap (carsim_plan_gap), touching when the gap is 0. The witnesses are
// the race's cars and then the traffic (#459: a close call with a traffic car
// is the near miss the user means).
#define WITNESS_RANGE 1.875f
static const CarSim* witness(const StyleFrame* f, int k, bool* blinking) {
	*blinking = false;
	if (k < f->otherCount) {
		*blinking = f->othersBlinking && f->othersBlinking[k];
		return f->others[k];
	}
	k -= f->otherCount;
	return (f->traffic && k < f->trafficCount) ? f->traffic[k] : NULL;
}
static void nearReset(void) {
	S->msm.near = 0;
	S->msm.nearKind = 0;
	S->msm.nearWith = -1;
}

static void detectNearMiss(const CarSim* s, const StyleFrame* f) {
	if (f->blinking || !(s->speedAbs > 13.410818f) || !(timeSinceCollision() > 0.2f)) {
		nearReset();
		return;
	}
	bool step = false;
	if (S->msm.near == 0) {
		S->msm.nearKind = 0;
		S->msm.nearWith = -1;
		for (int k = 0; k < f->otherCount + f->trafficCount; k++) {
			bool blink;
			const CarSim* o = witness(f, k, &blink);
			if (!o || blink)
				continue;
			const float gap = carsim_plan_gap(s, o);
			if (gap <= 0.0f || gap > WITNESS_RANGE)
				continue;
			bool found = false;
			if (s->wheelsDown == 0) {
				const float dx = s->rbPos[0] - o->rbPos[0], dy = s->rbPos[1] - o->rbPos[1];
				if (sqrtf(dx * dx + dy * dy) < 3.0f && s->rbPos[2] - o->rbPos[2] > 1.0f) {
					S->msm.nearKind = 1;
					found = true;
				}
			} else if (gap < 1.5f) {
				const float vx = s->rbVel[0] - o->rbVel[0], vy = s->rbVel[1] - o->rbVel[1],
					vz = s->rbVel[2] - o->rbVel[2];
				if (sqrtf(vx * vx + vy * vy + vz * vz) > 35.76218f) {
					S->msm.nearKind = 0;
					found = true;
				}
			}
			if (found) {
				S->msm.nearWith = k;
				break;
			}
		}
		step = S->msm.nearWith >= 0;
	} else if (S->msm.near == 1) {
		step = true;
		const int k = S->msm.nearWith;
		bool blink;
		const CarSim* o = k >= 0 ? witness(f, k, &blink) : NULL;
		if (o) {
			const float gap = carsim_plan_gap(s, o);
			if (gap <= 0.0f) {
				S->msm.near = 0;
				step = false;
			} else if (gap <= WITNESS_RANGE) {
				step = false;
			}
		}
	}
	if (step)
		S->msm.near++;
	if (S->msm.near == 2) {
		styleLog("%s past %s %d at %.1f m/s", S->msm.nearKind == 0 ? "NEARMISS" : "LEAPFROG",
			S->msm.nearWith >= f->otherCount ? "traffic" : "racer",
			S->msm.nearWith >= f->otherCount ? S->msm.nearWith - f->otherCount : S->msm.nearWith,
			s->speedAbs);
		trigger(S->msm.nearKind == 0 ? STYLE_NEARMISS : STYLE_LEAPFROG, 1.0f, 0.0f);
		nearReset();
	}
}

// DetectMiscellaneous (0x80190E6C): the head start, the places gained,
// reversing at speed. PORT: no drafting (Car +0x4DC), so DRAFTING never fires.
static void detectMiscellaneous(const CarSim* s, const StyleFrame* f) {
	static const int kGained[4] = {
		STYLE_GAINED_1ST, STYLE_GAINED_2ND, STYLE_GAINED_3RD, STYLE_GAINED_4TH
	};
	if (f->gameMode != 0 && f->stage == 4 && f->totalCars > 1) {
		if (s_raceSec < 1.5f && s_raceSec > 1.0f && f->position == 1 && s->speedAbs > 2.2351363f)
			trigger(STYLE_HEADSTART, 1.0f, 0.0f);
	}
	if (s_raceSec > 1.5f) {
		if (S->msm.lastPos == 0)
			S->msm.lastPos = f->position;
		if (S->msm.lastPos > 1) {
			const int p = f->position;
			if (p >= 1 && p < S->msm.lastPos && p < 5) {
				S->msm.lastPos = p;
				trigger(kGained[p - 1], 1.0f, 0.0f);
			}
		}
	}
	if (s->vfwd < -17.88109f)
		trigger(STYLE_REVERSE, 1.0f, 0.0f);
	if (timeSinceCollision() < 0.3f && S->msm.lastImpulse > 3.0f)
		abortMoment(STYLE_REVERSE);
}

// DetectShortcut (0x80191108): the first time into each SHORTCUT zone.
// Retail marks the zone itself (+0x1A); the file is read-only here, so the
// zones taken are kept aside until the next style_begin.
static void detectShortcut(const CarSim* s, const StyleFrame* f) {
	if (!f->paths)
		return;
	const TrackPathZone* z = trackpath_find(f->paths, s->pos[0], s->pos[1], TPZ_SHORTCUT, NULL);
	if (!z || z->unk1A != 0)
		return;
	for (int i = 0; i < s_shortcutN; i++)
		if (s_shortcut[i] == z)
			return;
	if (s_shortcutN < SHORTCUT_MAX)
		s_shortcut[s_shortcutN++] = z;
	trigger(STYLE_SHORTCUT, 1.0f, 0.0f);
}

// DetectJTurn (0x80191294): reversing at over 30 mph, then forward at over
// 20 mph without dropping under 20 mph between.
static void detectJTurn(const CarSim* s) {
	if (!(s->wheelsDown == 4 && timeSinceCollision() > 0.1f)) {
		S->msm.jturn = 0;
	} else {
		const s8 c = S->msm.jturn;
		bool next = false;
		if (c == 0) {
			next = !(-13.410818f < s->vfwd);
		} else if (c == 1) {
			if (s->speedAbs < 8.940545f)
				S->msm.jturn = 0;
			else
				next = s->vfwd > 8.940545f;
		}
		if (next)
			S->msm.jturn = c + 1;
	}
	if (S->msm.jturn == 2) {
		trigger(STYLE_JTURN, 1.0f, 0.0f);
		S->msm.jturn = 0;
	}
}

// ---------------------------------------------------------------- the frame

// RacingCarStats::Update's burnout (0x802267D0): the driven wheels spinning
// (+0x100, the forward slip while a tyre slides) within the first 5 s of the
// race arm it, and they gripping again awards it, worth the distance driven.
static void burnout(const CarSim* s, const StyleFrame* f) {
	if (!((s_burn == 0.0f || s_burn == -1.0f) && f->stage == 4 && f->gameMode != 7))
		return;
	int n = 0;
	float sum = 0.0f;
	for (int i = 0; i < CARPHYS_WHEELS; i++)
		if (carsim_is_drive_wheel(s, i)) {
			n++;
			sum += s->wheel[i].sndSlip[0];
		}
	const float avg = n > 0 ? sum / (float)n : 0.0f;
	if (s_burn != -1.0f || s_raceSec >= 5.0f) {
		if (s_burn == 0.0f && avg < 0.5f) {
			s_burn = s_dist;
			trigger(STYLE_BURNOUT, s_dist, 0.0f);
		}
	} else if (avg > 0.5f) {
		s_burn = 0.0f;
	}
}

// A car's StylePointsAccumulator (0x8018E4E8: the moments race kind
// s_kind runs) and MiscStyleManager (0x801900A0), in the next slot. A drift
// race's own moment, DRIFT, is DriftManager's (drift.c). How many moments it
// made.
static int carInit(const CarSim* car)
{
	StyleCar* const c = &s_cars[s_carN++];
	memset(c, 0, sizeof(*c));
	c->car = car;
	int made = 0;
	for (int i = 0; i < STYLE_TYPE_COUNT; i++) {
		if (!(s_info[i].mask & s_kind))
			continue;
		StyleMoment* m = &c->mom[i];
		m->info = &s_info[i];
		m->made = true;
		m->counts = 1;
		m->state = ST_INACTIVE;
		momentReset(m);
		made++;
	}
	c->msm.cleanSection = c->msm.cleanLap = c->msm.clean4 = 1;
	c->msm.lastCollType = -1;
	c->msm.nearWith = -1;
	return made;
}

void style_begin(const CarSim* player, u8 kind)
{
	style_end();
	if (!s_loaded || !player)
		return;
	s_player = player;
	s_now = (s32)(player->clock * TICKS_PER_SEC);
	s_begin = s_now;
	s_kind = kind;
	s_carN = 0;
	S = &s_cars[0];
	const int made = carInit(player);
	memset(s_coll, 0, sizeof(s_coll));
	s_upN = 0;
	s_shortcutN = 0;
	s_lastPts = 0.0f;
	s_dist = 0.0f;
	s_burn = -1.0f;   // RacingCarStats::Reset (0x802265D4)
	s_hadLap = false;
	s_on = true;
	carsim_add_impulse_hook(onImpulse);
	styleLog("%d moments for race kind 0x%02x", made, kind);
}

void style_end(void)
{
	if (s_on)
		carsim_remove_impulse_hook(onImpulse);
	s_on = false;
	s_upN = 0;
	s_player = NULL;
	s_carN = 0;
	S = &s_cars[0];
}

int style_ai_add(const CarSim* car)
{
	if (!s_on || !car || s_carN >= STYLE_CARS)
		return -1;
	StyleCar* const had = carSlot(car);
	if (had)
		return (int)(had - s_cars);
	const int slot = s_carN;
	const int made = carInit(car);
	styleLog("car slot %d: %d moments", slot, made);
	return slot;
}

// StylePointsAccumulator::Update for an AI racer's accumulator.
// PORT: MiscStyleManager::Update's detectors (power slides, air, near misses,
// clean laps...) are not run for the AI: in a drift, the only race the AI
// have an accumulator in, the board and Results read the DRIFT moment alone,
// which DriftManager drives; the collision fields it reads are kept, by the
// impulse hook, for every car with a slot.
void style_ai_update(int slot, float dt)
{
	if (!s_on || slot < 1 || slot >= s_carN)
		return;
	StyleCar* const prev = S;
	S = &s_cars[slot];
	s_now = (s32)(s_player->clock * TICKS_PER_SEC);
	accumulatorUpdate(dt);
	S = prev;
}

int style_select(int slot)
{
	const int prev = (int)(S - s_cars);
	if (slot >= 0 && slot < (s_carN > 0 ? s_carN : 1))
		S = &s_cars[slot];
	return prev;
}

void style_update(CarSim* player, const StyleFrame* f)
{
	if (!s_on || player != s_player || !f)
		return;
	S = &s_cars[0];
	s_now = (s32)(player->clock * TICKS_PER_SEC);
	s_trafficN = 0;
	for (int i = 0; f->traffic && i < f->trafficCount && s_trafficN < STYLE_TRAFFIC_MAX; i++)
		s_traffic[s_trafficN++] = f->traffic[i];
	s_finished = f->finished;
	// PORT: free roam's Race clock is taken from when the world was entered.
	s_raceSec = f->raceSec >= 0.0f ? f->raceSec : secSince(s_begin);
	// Car::FinishMovement's distance (+0x420), while the race runs.
	if (f->stage == 4 && player->speedAbs <= 1000.0f)
		s_dist += player->speedAbs * f->dt;
	for (int i = 0; i < COLL_MAX; i++)
		if (s_coll[i].used && secSince(s_coll[i].last) > COLL_LIFE)
			s_coll[i].used = false;

	burnout(player, f);
	// MiscStyleManager::Update (0x80190308), DetectHydraulics last.
	detectCleanRun(f);
	detectAirTime(player, f);
	detectPowerSlide(player);
	detect360(player);
	detectNearMiss(player, f);
	detectMiscellaneous(player, f);
	detectShortcut(player, f);
	detectJTurn(player);
	detectHydraulics(player);
	// LapCounter::Update's best lap (+0x64), from the second lap on.
	if (f->newLap) {
		const bool best = !(f->bestLap < f->lastLap || !s_hadLap);
		s_hadLap = true;
		if (best)
			trigger(STYLE_BESTLAP, 1.0f, 0.0f);
	}
	accumulatorUpdate(f->dt);

	// RacingCar::Update (0x80227770): the points banked since last frame go
	// into the bottle, 1% of it per 15 points.
	const float pts = S->total;
	const float fill = (pts - s_lastPts) / 15.0f;
	if (pts != s_lastPts) {
		const float before = carsim_nos_fraction(player);
		carsim_nos_fill(player, fill * 0.01f);
		styleLog("%d points (+%d): nitrous %.1f%% -> %.1f%%", (int)pts, (int)(pts - s_lastPts),
			before * 100.0f, carsim_nos_fraction(player) * 100.0f);
	} else {
		carsim_nos_fill(player, fill * 0.01f);
	}
	s_lastPts = pts;
}

void style_nitrous_pressed(const CarSim* player, float throttle)
{
	// Car::IsBlowOff (0x802242A4): the bottle open with the throttle shut or
	// under 5 mph.
	if (!s_on || player != s_player)
		return;
	StyleCar* const prev = S;
	S = &s_cars[0];
	if ((fabsf(throttle) <= 0.001f || player->speedAbs * 2.237f < 5.0f) && tickSet(player->nosOnAt))
		trigger(STYLE_SPRAYSHOW, 1.0f, 0.0f);
	S = prev;
}

float style_points(void)
{
	return s_cars[0].total;
}

int style_take_up(StyleUp* out, int cap)
{
	int n = s_on ? s_upN : 0;
	if (n > cap)
		n = cap;
	if (n > 0)
		memcpy(out, s_up, (size_t)n * sizeof(*out));
	s_upN = 0;
	return n;
}

// ---------------------------------------------------------------- DriftManager's view

bool style_moment(int type, StyleMomentView* out)
{
	const StyleMoment* m = (type >= 0 && type < STYLE_TYPE_COUNT) ? moment(type) : NULL;
	if (!m)
		return false;
	out->state = m->state;
	out->mult = m->mult;
	out->points = m->points;
	out->banked = m->banked;
	out->maxDur = m->maxDur;
	out->maxPts = m->maxPts;
	return true;
}

void style_moment_trigger(int type, float intensity, float value)
{
	if (type >= 0 && type < STYLE_TYPE_COUNT)
		trigger(type, intensity, value);
}

// SetMultiplier (0x8018E430).
void style_moment_set_mult(int type, float mult)
{
	StyleMoment* m = (type >= 0 && type < STYLE_TYPE_COUNT) ? moment(type) : NULL;
	if (m)
		m->mult = mult;
}

void style_moment_abort(int type)
{
	if (type >= 0 && type < STYLE_TYPE_COUNT)
		abortMoment(type);
}

void style_moment_clear_mult(int type)
{
	if (type >= 0 && type < STYLE_TYPE_COUNT)
		clearMultiplier(type);
}

void style_force_finish_all(void)
{
	for (int i = 0; i < STYLE_TYPE_COUNT; i++) {
		StyleMoment* m = moment(i);
		if (!m)
			continue;
		if (m->state == ST_ACTIVE)
			forceDone(m, 1);
		else if (m->state == ST_ACTIVATING)
			momentCancel(m, 1);
	}
}

// The record whose name is English.bin key `hash`: that name as style_load
// read it, NULL for none.
const char* style_name_for_hash(u32 hash)
{
	if (!s_loaded || !hash)
		return NULL;
	for (int i = 0; i < STYLE_TYPE_COUNT; i++)
		if (s_info[i].nameHash == hash)
			return s_info[i].name;
	return NULL;
}

u32 style_record_name(int type)
{
	return (s_loaded && type >= 0 && type < STYLE_TYPE_COUNT) ? s_info[type].nameHash : 0u;
}

float style_record_value(int type)
{
	return (s_loaded && type >= 0 && type < STYLE_TYPE_COUNT) ? s_info[type].hold : 0.0f;
}

float style_time_since_collision(void)
{
	return s_on ? timeSinceCollision() : 99999.9f;
}

float style_last_impulse(void)
{
	return s_on ? S->msm.lastImpulse : 0.0f;
}

int style_last_collision_type(void)
{
	return s_on ? S->msm.lastCollType : -1;
}

bool style_traffic_hit(void)
{
	return s_on && S->msm.trafficHit;
}

// DirtyCleanRun (0x801903F4).
void style_dirty_clean_run(void)
{
	S->msm.cleanSection = S->msm.cleanLap = S->msm.clean4 = 0;
}
