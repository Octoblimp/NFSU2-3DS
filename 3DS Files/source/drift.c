#include "drift.h"
#include "style.h"
#include "dbglog.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// The SpeedPS.elf tables (0x8032D8E0..0x8032D97C), by the car class
// (RidePhysicsInfo::GetCarClass, 0 the fastest).
static const float kMinStartVel[4]   = { 13.8889f, 12.5f, 11.1111f, 8.33333f };
static const float kMinSustainVel[4] = { 11.1111f, 9.72222f, 8.33333f, 6.94444f };
// By the level: the speed under which a level is lost, and over which (less
// the margin) the next is gained.
static const float kTooSlowVel[5]    = { 0.0f, 8.33333f, 11.1111f, 13.8889f, 16.6667f };
#define TOO_SLOW_MARGIN 1.38889f
// The drift points that take a car up from each level, [class][level - 1].
static const int kMultPoints[4][4] = {
	{ 350, 1400, 4200, 11200 },
	{ 300, 1200, 3600, 9600 },
	{ 250, 1000, 3000, 8000 },
	{ 200, 800, 2400, 6400 },
};
// SP_KingDriftLevels: the style records whose +0x08 are the King drift levels.
static const int kKingLevels[7] = { 8, 9, 10, 11, 12, 13, 14 };
// SP_DriftAIHandicapMultiplier, by the race's AI skill (TheRaceParameters
// +0x78): what an AI racer's drift points are multiplied by.
static const float kAIHandicap[3] = { 1.0f, 1.25f, 1.5f };

// TheDownhillTrackScores (0x8035EA60): the downhill drift tracks (4174..
// 4179), each with the score a score-only opponent ends on, [car class]
// [AI skill]. GetDownhillDriftScore (0x8018FFE0) reads it; a track not in it
// scores 0.
static const struct {
	int track;
	float score[4][3];
} kDownhill[6] = {
	{ 4174, { { 40000, 60000, 80000 }, { 34000, 50000, 66000 },
	          { 27000, 40000, 53000 }, { 20000, 30000, 40000 } } },
	{ 4175, { { 60000, 150000, 250000 }, { 53000, 130000, 220000 },
	          { 46000, 110000, 185000 }, { 40000, 90000, 150000 } } },
	{ 4176, { { 75000, 112000, 150000 }, { 65000, 97000, 130000 },
	          { 55000, 82000, 110000 }, { 45000, 67000, 90000 } } },
	{ 4177, { { 100000, 150000, 200000 }, { 86000, 130000, 170000 },
	          { 73000, 110000, 145000 }, { 60000, 90000, 120000 } } },
	{ 4178, { { 120000, 260000, 400000 }, { 106000, 225000, 350000 },
	          { 93000, 195000, 300000 }, { 80000, 160000, 250000 } } },
	{ 4179, { { 100000, 200000, 300000 }, { 91000, 175000, 260000 },
	          { 83000, 150000, 220000 }, { 75000, 125000, 180000 } } },
};
// BuildLeaderBoard's test: TheRaceParameters (the track) - 0x104E < 6.
#define DOWNHILL_FIRST 0x104E
#define DOWNHILL_COUNT 6
#define DOWNHILL_FAKES 3       // when NumPlayers (r13-0x6144) is 1
#define DOWNHILL_FAKE_ID 100   // their node's car number, 100 + i

// The run-time pairs __static_initialization sets (0x80192238..0x801922A8).
#define DODGE_DIST_LO 1.0f
#define DODGE_DIST_HI 20.0f
#define DODGE_VEL_LO  2.77777791f
#define DODGE_VEL_HI  44.4444466f
#define DODGE_MULT_LO 0.0f
#define DODGE_MULT_HI 1.0f

#define DEG_TO_RAD 0.0174533f
#define LEVEL_HOLD_SEC 1.5f      // between level drops; a drop holds the gain off this long
#define WRONG_WAY_CYCLE 4.0f     // the wrong-way count runs from 3 and starts over

// DriftManager (0x54 bytes), its fields by retail's word index, with the
// RacingCar fields UpdateDriftScore keeps. The two RealTimer stamps are
// seconds on the race clock here (< 0 unset, retail's 0 or 0x7FFFFFFF); the
// frames the port does not step are its pause, which retail's RealTimer runs
// through.
typedef struct {
	bool  on;
	int   id;            // the board's car number: the race_car() index
	int   slot;          // the car's accumulator (style.c's slot)
	bool  human;
	int   carClass;      // [3]
	float topSpeed;      // the ride's GetTheoreticalTopSpeed
	bool  collFlag;      // [6]
	bool  losing;        // [7]
	bool  tooSlow;       // [8]
	float bonusZone;     // [9]
	int   level;         // [10]
	int   nextPoints;    // [0xB]
	float levelAt;       // [0xC]
	float wrongAt;       // [0xD]
	float wrongShow;     // [0xE]
	float lapHigh;       // [0xF], the furthest RacingCar +0x14 seen on the ground
	float intensity;     // [0x10]
	float handicap;      // [0x11]
	float dodge;         // [0x12]
	int   king;          // [0x13]
	float peakYaw;       // [0x14]
	float now;
	// RacingCar's: +0x13C, each lap's points, and +0x190, the score as the
	// lap began.
	float lapScore[21];
	float lapStart;
	int loggedLap; // the last lap the log line went out for
	bool  finished;
	u32   nifty;
} DriftMgr;

// Each RacingCar's [0x76], by its race_car() index: [0] the player's, then
// every AI racer's (UpdateDriftScore under g_tweakAIDriftOpponents).
#define DRIFT_CARS (1 + RACE_MAX_AI)
static DriftMgr s_mgr[DRIFT_CARS];
// The manager being worked on.
static DriftMgr* D = &s_mgr[0];

// DriftManager's static leaderboard (the bList at r13-0x4FF4 over
// LeaderBoardScoreArray): the nodes in list order, each its car number
// ([2]), score (+0x10) and whether it is a human's (+0x14). It outlives the
// race, as retail's does until the next BuildLeaderBoard, so the post-race
// screens and CalculateIfWon still read it.
static struct {
	bool  built;
	int   count;
	int   id[DRIFT_BOARD_MAX];
	float score[DRIFT_BOARD_MAX];
	bool  player[DRIFT_BOARD_MAX];
	// BuildLeaderBoard's inputs (drift_board_setup): the track, the race's
	// AI skill and the score-only opponents' names.
	int   track;
	int   aiSkill;
	char  fakeName[DOWNHILL_FAKES][24];
	int   fakeNames;
} s;

static void driftLog(const char* fmt, ...)
{
	char b[200];
	int n = 0;
	if (D != &s_mgr[0])
		n = snprintf(b, sizeof(b), "car %d: ", D->id);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b + n, sizeof(b) - (size_t)n, fmt, ap);
	va_end(ap);
	dbglog_line("drift: ", b);
}

static int multPoints(int level) {
	// (int)(SP_DriftMultPoints[class][level - 1] + 0.5)
	return level < 5 ? kMultPoints[D->carClass][level - 1] : 0;
}

// The DRIFT moment of the accumulator style.c has selected (D's).
static bool drifting(void) {
	StyleMomentView m;
	return style_moment(STYLE_DRIFT, &m) && m.state == STYLE_STATE_ACTIVE;
}

static float momentPoints(void) {
	StyleMomentView m;
	return style_moment(STYLE_DRIFT, &m) ? m.points : 0.0f;
}

static float momentBanked(void) {
	StyleMomentView m;
	return style_moment(STYLE_DRIFT, &m) ? m.banked : 0.0f;
}

static DriftMgr* mgrOf(int id) {
	return (id >= 0 && id < DRIFT_CARS && s_mgr[id].on) ? &s_mgr[id] : NULL;
}

// __12DriftManager (0x8018EB10) and Initialize (0x8018EB80).
static void mgrInit(DriftMgr* m, int id, int slot, bool human, int carClass, float topSpeed,
		float handicap) {
	memset(m, 0, sizeof(*m));
	m->id = id;
	m->slot = slot;
	m->human = human;
	m->carClass = carClass < 0 ? 0 : carClass > 3 ? 3 : carClass;
	m->topSpeed = topSpeed > 1.0f ? topSpeed : 1.0f;
	m->level = 1;
	m->wrongShow = -1.0f;
	m->bonusZone = 1.0f;
	m->dodge = 1.0f;
	m->handicap = handicap;
	m->levelAt = m->wrongAt = -1.0f;
	m->nextPoints = kMultPoints[m->carClass][0];
	m->on = true;
}

void drift_begin(int carClass, float topSpeed) {
	memset(s_mgr, 0, sizeof(s_mgr));
	memset(&s, 0, sizeof(s));
	s.track = -1;
	D = &s_mgr[0];
	// A human's handicap is 1.
	mgrInit(D, 0, 0, true, carClass, topSpeed, 1.0f);
	driftLog("on -- car class %d, top speed %.1f m/s, x1 needs %d", D->carClass, D->topSpeed,
		D->nextPoints);
}

bool drift_ai_add(int id, const CarSim* car, int carClass, float topSpeed, int aiSkill) {
	if (!s_mgr[0].on || id < 1 || id >= DRIFT_CARS || !car)
		return false;
	const int slot = style_ai_add(car);
	if (slot < 0)
		return false;
	const int sk = aiSkill < 0 ? 0 : aiSkill > 2 ? 2 : aiSkill;
	DriftMgr* const prev = D;
	D = &s_mgr[id];
	mgrInit(D, id, slot, false, carClass, topSpeed, kAIHandicap[sk]);
	driftLog("on -- car class %d, top speed %.1f m/s, handicap x%.2f (AI skill %d)", D->carClass,
		D->topSpeed, D->handicap, sk);
	D = prev;
	return true;
}

void drift_end(void) {
	D = &s_mgr[0];
	if (D->on)
		driftLog("off -- score %d", drift_score());
	for (int i = 0; i < DRIFT_CARS; i++)
		s_mgr[i].on = false;
}

bool drift_on(void) { return s_mgr[0].on; }

// DetectStylePoints (0x8018FC60): the King drift levels this drift passes,
// and the widest angle it has held one way.
static void detectStylePoints(const CarSim* car) {
	if (drifting()) {
		if (D->king < 7) {
			const int rec = kKingLevels[D->king];
			if (style_record_value(rec) < momentPoints()) {
				D->king++;
				// ShowNiftyDriftyMessage: the HUD's, the player's alone.
				if (D->human) {
					D->nifty = style_record_name(rec);
					driftLog("King level %d (%d points, 0x%08lX)", D->king, (int)momentPoints(),
						(unsigned long)D->nifty);
				}
			}
		}
		const float yaw = carsim_yaw(car);
		const float p = D->peakYaw;
		if (p == 0.0f || (yaw * p > 0.0f && ((yaw < 0.0f && yaw < p) || (yaw > 0.0f && p < yaw))))
			D->peakYaw = yaw;
	} else {
		D->king = 0;
		D->peakYaw = 0.0f;
		style_moment_clear_mult(STYLE_CHAINED_DRIFT);
	}
}

// DetectWrongWay (0x8018FAEC): a human's wrong-way cue (Player +0x234), or
// rolling backwards (+0x43C below 0), which is an AI's only test. After 1 s
// it counts down from 3 (WrongWay_CountDown) and starts over at 4.
static void detectWrongWay(const CarSim* car, const DriftFrame* f) {
	const bool wrong = (D->human && f->wrongWay) || car->vfwd < 0.0f;
	D->wrongShow = -1.0f;
	if (!wrong) {
		D->wrongAt = -1.0f;
		return;
	}
	if (D->wrongAt < 0.0f)
		D->wrongAt = D->now;
	const float el = D->now - D->wrongAt;
	if (el > WRONG_WAY_CYCLE)
		D->wrongAt = -1.0f;
	if (el > 1.0f)
		D->wrongShow = WRONG_WAY_CYCLE - el;
}

// CalcDriftIntensity (0x8018F338): (speed / 30 km/h)^2 x |slip|, once the
// slip passes 5 degrees and the speed the class's start (or, mid-drift, its
// sustain) speed -- not on the wrong way, and not where the car is behind the
// furthest it has been round the lap.
static float calcDriftIntensity(const CarSim* car, const DriftFrame* f) {
	float r = 0.0f;
	// PORT: retail asks Car+0x3C's +0x11C < 1, read here as a wheel down.
	if (car->wheelsDown <= 0)
		return r;
	float hi = D->lapHigh;
	if (hi <= f->lapExact) {
		const float yaw = carsim_yaw(car);
		const float speed = car->speedAbs;
		const float wz = car->w[2];
		const bool starting = fabsf(wz) > 0.1f && wz * yaw < 0.0f;
		const float* table = (!drifting() || starting) ? kMinStartVel : kMinSustainVel;
		if (D->wrongAt < 0.0f && fabsf(yaw) > 5.0f * DEG_TO_RAD && table[D->carClass] < speed) {
			const float k = speed / kMinStartVel[3];
			r = k * k * fabsf(yaw);
		}
	}
	if (hi < f->lapExact)
		hi = f->lapExact;
	D->lapHigh = hi;
	return r;
}

// CalcBonusZoneMult (0x8018F4DC): two or more wheels off the road (the
// eTerrainType ROAD, 1) multiply by how many are off, scaled by the speed up
// to a quarter of the top speed.
static float calcBonusZoneMult(const CarSim* car) {
	int off = 0;
	for (int i = 0; i < CARPHYS_WHEELS; i++)
		if (car->wheel[i].terrain != 1)
			off++;
	if (off < 2)
		return 1.0f;
	float k = car->speedAbs / (D->topSpeed * 0.25f);
	if (k < 0.0f) k = 0.0f;
	if (k > 1.0f) k = 1.0f;
	return (float)off * k;
}

// CalcDriftMultiplier (0x8018F5AC): the level. Too slow for it (or the
// wrong way) loses one at once and another every 1.5 s; after a loss, none
// is gained for 1.5 s; fast enough, a drift worth the level's points gains
// one, up to x5.
static int calcDriftMultiplier(const CarSim* car) {
	const float speed = car->speedAbs;
	int level = D->level;
	D->tooSlow = false;
	D->losing = false;
	if (D->level > 1 && (D->wrongAt >= 0.0f ||
	                     speed < kTooSlowVel[D->level - 1] + TOO_SLOW_MARGIN)) {
		D->losing = true;
		if (!(D->levelAt >= 0.0f && D->now - D->levelAt <= LEVEL_HOLD_SEC)) {
			D->levelAt = D->now;
			level = D->level - 1;
		}
	} else if (D->levelAt >= 0.0f) {
		D->tooSlow = true;
		if (D->now - D->levelAt > LEVEL_HOLD_SEC)
			D->levelAt = -1.0f;
	} else if (D->level > 3 || kTooSlowVel[D->level] - TOO_SLOW_MARGIN <= speed) {
		D->levelAt = -1.0f;
		if (drifting() && D->level < 5 && (float)D->nextPoints < momentPoints())
			level = D->level + 1;
	} else {
		D->levelAt = -1.0f;
		D->tooSlow = true;
	}
	D->nextPoints = multPoints(level);
	return level;
}

// GetCollisionIntensity (0x8018F8CC): the last hit's impulse in the half
// second after it, once; [6] says whether it was one that ends a drift (a
// wall or anything not a car, or traffic).
static float getCollisionIntensity(void) {
	float r = 0.0f;
	if (style_time_since_collision() >= 0.5f) {
		D->collFlag = false;
	} else if (!D->collFlag) {
		r = style_last_impulse();
		D->collFlag = style_last_collision_type() == 3 || style_traffic_hit();
	}
	return r;
}

// CalcDodgeTrafficMult (0x8018FE98): mid-drift, each traffic car within 20 m
// adds up to 1, by how near it is and how fast this car goes.
static float calcDodgeTrafficMult(const CarSim* car, const DriftFrame* f) {
	float r = 1.0f;
	if (!drifting() || !f->traffic)
		return r;
	float v = car->speedAbs;
	if (v < DODGE_VEL_LO) v = DODGE_VEL_LO;
	if (v > DODGE_VEL_HI) v = DODGE_VEL_HI;
	const float vk = (v - DODGE_VEL_LO) / (DODGE_VEL_HI - DODGE_VEL_LO) *
		(DODGE_MULT_HI - DODGE_MULT_LO) + DODGE_MULT_LO;
	for (int i = 0; i < f->trafficCount; i++) {
		const float dx = f->traffic[i][0] - car->rbPos[0];
		const float dy = f->traffic[i][1] - car->rbPos[1];
		const float dz = f->traffic[i][2] - car->rbPos[2];
		float d = sqrtf(dx * dx + dy * dy + dz * dz);
		if (d < DODGE_DIST_LO) d = DODGE_DIST_LO;
		if (d > DODGE_DIST_HI) d = DODGE_DIST_HI;
		r += (1.0f - (d - DODGE_DIST_LO) / (DODGE_DIST_HI - DODGE_DIST_LO)) * vk;
	}
	return r;
}

// DriftManager::Update (0x8018F150).
static void managerUpdate(const CarSim* car, const DriftFrame* f) {
	detectStylePoints(car);
	detectWrongWay(car, f);
	const float intensity = calcDriftIntensity(car, f);
	int level = calcDriftMultiplier(car);
	D->bonusZone = calcBonusZoneMult(car);
	const float coll = getCollisionIntensity();
	const float team = 1.0f;   // CalcTeamDriftMult: no team car in one-player
	const float dodge = calcDodgeTrafficMult(car, f);
	D->intensity = intensity;
	D->dodge = dodge;
	const float score = intensity * (D->bonusZone * team * D->handicap * dodge);
	if (coll <= 0.2f || !D->collFlag) {
		if (score > 0.0f) {
			style_moment_set_mult(STYLE_DRIFT, (float)level);
			style_moment_trigger(STYLE_DRIFT, score, 0.0f);
			const float yaw = carsim_yaw(car);
			if (drifting() && yaw * D->peakYaw < 0.0f && fabsf(yaw) > 0.266667f)
				D->peakYaw = yaw;
		}
	} else {
		level = 1;
		style_moment_abort(STYLE_DRIFT);
		if (coll > 5.0f)
			style_dirty_clean_run();
		driftLog("a crash (impulse %.1f) -- the drift is lost, back to x1", coll);
	}
	// The log names the new level's own threshold: after a crash [0xB] still
	// holds the old one until the next frame's CalcDriftMultiplier, as retail's.
	if (level != D->level)
		driftLog("x%d -> x%d at %.1f m/s (%d points to the next)", D->level, level,
			car->speedAbs, multPoints(level));
	D->level = level;
}

// RacingCar::UpdateDriftScore (0x802279D4) on D, its accumulator selected.
static void scoreUpdate(const CarSim* car, const DriftFrame* f) {
	D->now += f->dt;
	managerUpdate(car, f);
	if ((f->laps > 0 && f->maxLap == f->laps) || f->finished) {
		style_force_finish_all();
		if (!D->finished) {
			D->finished = true;
			driftLog("%s -- score %d", f->maxLap == f->laps ? "the last lap is done"
				: "finished short of the laps", (int)momentBanked());
		}
	}
	const float banked = (float)(int)momentBanked();
	int idx = f->maxLap;
	if (!f->newLap && f->maxLap + 1 < f->laps)
		idx = f->maxLap + 1;
	else if (!f->newLap)
		idx = f->laps;
	if (idx >= 0 && idx < 21 && D->lapScore[idx] < banked - D->lapStart)
		D->lapScore[idx] = banked - D->lapStart;
	if (f->newLap) {
		// A finished car's newLap stays up (race.c stops counting its laps),
		// so the line goes out once per lap.
		if (f->maxLap >= 1 && f->maxLap < 21 && f->maxLap != D->loggedLap) {
			D->loggedLap = f->maxLap;
			driftLog("lap %d -- %d points (score %d)", f->maxLap, (int)D->lapScore[f->maxLap],
				(int)banked);
		}
		D->lapStart = banked;
	}
}

static void runOn(DriftMgr* m, const CarSim* car, const DriftFrame* f) {
	DriftMgr* const prevMgr = D;
	D = m;
	const int prevSlot = style_select(m->slot);
	scoreUpdate(car, f);
	style_select(prevSlot);
	D = prevMgr;
}

void drift_update(const CarSim* car, const DriftFrame* f) {
	if (!s_mgr[0].on || !car || !f)
		return;
	runOn(&s_mgr[0], car, f);
}

void drift_ai_update(int id, const CarSim* car, const DriftFrame* f) {
	DriftMgr* const m = id >= 1 ? mgrOf(id) : NULL;
	if (!m || !car || !f)
		return;
	// RacingCar::Update's order: the accumulator's Update, then
	// UpdateDriftScore.
	style_ai_update(m->slot, f->dt);
	runOn(m, car, f);
}

// NotifyPlayerReset (0x8018FC18).
void drift_player_reset(void) {
	DriftMgr* const m = mgrOf(0);
	if (!m)
		return;
	DriftMgr* const prevMgr = D;
	D = m;
	const int prevSlot = style_select(0);
	D->level = 1;
	style_moment_abort(STYLE_DRIFT);
	style_moment_clear_mult(STYLE_DRIFT);
	driftLog("the car was reset -- back to x1");
	style_select(prevSlot);
	D = prevMgr;
}

// A car's DRIFT moment, banked (+0x2C): its score. 0 with no manager.
static bool carMoment(int id, StyleMomentView* out) {
	const DriftMgr* const m = mgrOf(id);
	if (!m)
		return false;
	const int prev = style_select(m->slot);
	const bool ok = style_moment(STYLE_DRIFT, out);
	style_select(prev);
	return ok;
}

int drift_score(void) {
	StyleMomentView m;
	return carMoment(0, &m) ? (int)m.banked : 0;
}

float drift_lap_score(int lap) {
	const DriftMgr* const m = mgrOf(0);
	return (m && lap >= 0 && lap < 21) ? m->lapScore[lap] : 0.0f;
}

void drift_view(DriftView* out) {
	memset(out, 0, sizeof(*out));
	const DriftMgr* const d = mgrOf(0);
	out->on = d != NULL;
	if (!d)
		return;
	StyleMomentView m;
	const bool have = carMoment(0, &m);
	out->level = d->level;
	out->nextPoints = d->nextPoints;
	out->losing = d->losing;
	out->tooSlow = d->tooSlow;
	out->bonusZone = d->bonusZone;
	out->dodge = d->dodge;
	out->intensity = d->intensity;
	out->wrongWaySec = d->wrongShow;
	out->kingLevel = d->king;
	out->peakYaw = d->peakYaw;
	out->drifting = have && m.state == STYLE_STATE_ACTIVE;
	out->points = have ? m.points : 0.0f;
	out->score = have ? (int)m.banked : 0;
}

u32 drift_take_nifty(void) {
	const u32 n = s_mgr[0].nifty;
	s_mgr[0].nifty = 0;
	return n;
}

void drift_board_setup(int track, int aiSkill, const char* const* names, int nameCount) {
	s.track = track;
	s.aiSkill = aiSkill < 0 ? 0 : aiSkill > 2 ? 2 : aiSkill;
	s.fakeNames = 0;
	for (int i = 0; i < DOWNHILL_FAKES; i++)
		s.fakeName[i][0] = '\0';
	for (int i = 0; names && i < nameCount && i < DOWNHILL_FAKES; i++) {
		snprintf(s.fakeName[i], sizeof(s.fakeName[i]), "%s", names[i] ? names[i] : "");
		s.fakeNames = i + 1;
	}
}

// GetDownhillDriftScore (0x8018FFE0): the table's score for the track, the
// class and the skill, less 5% for the second opponent and 10% for the third
// (strength 2 x0.95, 3 x0.9).
static float downhillScore(int track, int strength, int carClass, int aiSkill) {
	float r = 0.0f;
	for (int i = 0; i < DOWNHILL_COUNT; i++)
		if (kDownhill[i].track == track) {
			r = kDownhill[i].score[carClass][aiSkill];
			break;
		}
	if (strength == 2)
		return r * 0.95f;
	if (strength == 3)
		return r * 0.9f;
	return r;
}

static void boardAdd(int id, float score, bool player) {
	if (s.count >= DRIFT_BOARD_MAX)
		return;
	s.id[s.count] = id;
	s.score[s.count] = score;
	s.player[s.count] = player;
	s.count++;
}

// BuildLeaderBoard (0x8018EC70), run as the first manager is made: a node
// per human in driver order on a score of 0, the lowest car class among them
// kept; then on a downhill track (4174..4179) three score-only opponents,
// ids 100..102, each named by GetRandomCharacterNames and on the score
// GetDownhillDriftScore gives it -- the event's AI racers then have no node;
// anywhere else a node per AI racer on 0. PORT: PickRandomCarsOfSomeSort's
// car for a score-only opponent is not drawn; nothing shows it.
static void buildLeaderBoard(int cars) {
	s.count = 0;
	// The humans: the player, the only one (NumPlayers 1).
	int minClass = 999;
	boardAdd(0, 0.0f, true);
	if (s_mgr[0].carClass < minClass)
		minClass = s_mgr[0].carClass;
	if ((unsigned)(s.track - DOWNHILL_FIRST) < DOWNHILL_COUNT) {
		for (int i = 0; i < DOWNHILL_FAKES; i++) {
			const float sc = downhillScore(s.track, i + 1, minClass, s.aiSkill);
			boardAdd(DOWNHILL_FAKE_ID + i, sc, false);
			D = &s_mgr[0];
			driftLog("score-only opponent %d (%s): %d points", DOWNHILL_FAKE_ID + i,
				s.fakeName[i], (int)sc);
		}
	} else {
		for (int k = 1; k < cars; k++)
			boardAdd(k, 0.0f, false);
	}
	s.built = true;
	driftLog("the leaderboard -- %d car(s)%s", s.count,
		(unsigned)(s.track - DOWNHILL_FIRST) < DOWNHILL_COUNT ? ", a downhill" : "");
}

// UpdateLeaderBoard (0x8018F9E8), the end of Race::Update in a drift: each
// racer's score is its accumulator's DRIFT moment banked (+0x2C) -- a
// score-only opponent keeps the one it was given -- then bList::Sort with
// SortScoreList (0x80190084, `b <= a`): the highest first, ties left in the
// order the list already had.
void drift_board_update(int cars) {
	if (!s_mgr[0].on)
		return;
	if (!s.built)
		buildLeaderBoard(cars);
	const int lead = s.count > 0 ? s.id[0] : -1;
	for (int k = 0; k < s.count; k++) {
		if (s.id[k] >= DOWNHILL_FAKE_ID)
			continue;
		StyleMomentView m;
		s.score[k] = carMoment(s.id[k], &m) ? m.banked : 0.0f;
	}
	for (int a = 1; a < s.count; a++)
		for (int k = a; k > 0 && s.score[k - 1] < s.score[k]; k--) {
			const int id = s.id[k];
			const float sc = s.score[k];
			const bool pl = s.player[k];
			s.id[k] = s.id[k - 1];
			s.score[k] = s.score[k - 1];
			s.player[k] = s.player[k - 1];
			s.id[k - 1] = id;
			s.score[k - 1] = sc;
			s.player[k - 1] = pl;
		}
	if (s.count > 0 && s.id[0] != lead && lead >= 0) {
		D = &s_mgr[0];
		driftLog("car %d leads the board (%d points)", s.id[0], (int)s.score[0]);
	}
}

int drift_board_count(void) { return s.built ? s.count : 0; }

// GetLeaderBoardPos (0x8018F9A8): the car's place in the list, from 1; 0 off it.
int drift_board_pos(int id) {
	for (int k = 0; s.built && k < s.count; k++)
		if (s.id[k] == id)
			return k + 1;
	return 0;
}

bool drift_board_at(int k, int* id, float* score, bool* player) {
	if (!s.built || k < 0 || k >= s.count)
		return false;
	if (id) *id = s.id[k];
	if (score) *score = s.score[k];
	if (player) *player = s.player[k];
	return true;
}

const char* drift_board_name(int id) {
	const int i = id - DOWNHILL_FAKE_ID;
	return (i >= 0 && i < s.fakeNames) ? s.fakeName[i] : NULL;
}

bool drift_car_stats(int id, RaceDriftCar* out) {
	memset(out, 0, sizeof(*out));
	out->boardPos = drift_board_pos(id);
	const DriftMgr* const d = mgrOf(id);
	StyleMomentView m;
	if (!d || !carMoment(id, &m))
		return false;
	for (int lap = 0; lap < 21 && lap < RACE_MAX_LAPS; lap++)
		out->lapScore[lap] = d->lapScore[lap];
	out->banked = m.banked;
	out->maxPts = m.maxPts;
	out->maxDur = m.maxDur;
	out->has = true;
	return true;
}
