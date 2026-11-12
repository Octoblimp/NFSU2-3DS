#include "opponent.h"
#include "difficulty.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// RaceStarter::AddCareerEventAIOpponents (Speed.elf 0x8011A22C) and the car
// picker it calls, PickRandomCarsOfSomeSort (0x80119F24). See opponent.h for
// what of the roster is ported and what is still missing behind it.

// -------------------------------------------------------------- the generator
//
// bRandom on the global state at r13-0x7D38 (0x8046D0A8), which the image
// ships holding OPPONENT_RNG_SEED: rideinfo_random(), the one generator the
// car picker, the names, the grid swap and the AI cars' parts all draw on, so
// the first draw of a run is OPPONENT_RNG_SEED % n.
static u32 bRandomNext(u32* state, u32 n) {
	return rideinfo_random(state, n);
}

// ------------------------------------------------------------- the car lists
//
// RandomlyChooseableCarConfigsNorthAmerica (0x8035B9E0) and ...RestOfWorld
// (0x8035B9FC), 26 car types each, identical but for the last two; and
// RandomlyChooseableSUVs (0x8046DC0C), the three the engine uses instead when
// the event is an SUV race. Copied byte for byte out of the image.
static const u8 kCarsNorthAmerica[26] = {
	27, 19, 15, 22, 13, 11, 16, 21, 2, 14, 30, 28, 26, 4, 29, 3,
	1, 0, 24, 23, 20, 17, 12, 25, 5, 18
};
static const u8 kCarsRestOfWorld[26] = {
	27, 19, 15, 22, 13, 11, 16, 21, 2, 14, 30, 28, 26, 4, 29, 3,
	1, 0, 24, 23, 20, 17, 12, 25, 7, 6
};
static const u8 kCarsSUV[3] = { 8, 9, 10 };
// PORT (#455): Region=all's, North America's then Rest of World's last two.
static const u8 kCarsAllRegions[28] = {
	27, 19, 15, 22, 13, 11, 16, 21, 2, 14, 30, 28, 26, 4, 29, 3,
	1, 0, 24, 23, 20, 17, 12, 25, 5, 18, 7, 6
};

// The SUV byte PsychoDriver::Drive asks of a car (FECarRecord +0x88A on the
// GameCube): one of these three.
static bool isSuv(int carType) {
	for (int k = 0; k < (int)(sizeof(kCarsSUV) / sizeof(kCarsSUV[0])); k++)
		if (carType == kCarsSUV[k]) return true;
	return false;
}

// DAT_80327F6C and DAT_80327F74, the two constants the picker runs on: the
// tolerance base, which the loop doubles before its first pass so the first
// window is 0.06 wide, and the sentinel below which a target measure means
// "match the player instead".
#define OPPONENT_TOL_BASE 0.03f
#define OPPONENT_TARGET_SENTINEL 0.0f
// The loop is unbounded in retail -- the tolerance doubles until something
// fits, and something always does by about the eighth pass. A cap costs
// nothing and keeps a car table with no usable record from hanging the race.
#define OPPONENT_TOL_PASSES 40

// ------------------------------------------------------- the performance axis
//
// RidePhysicsInfo::GetPerformanceMeasure's normalize path, which is all this
// file needs of it: cardb_perf_measure() puts the car's shipped stock measure
// on retail's 0..10 axis, the axis the event table's matchValue and the stage
// parameter are quoted on (MatchPerformance divides them by 10 to get its
// interpolant). -1 for a record the table has no measure for.
//
// A car with a measure but no art is still no car here: retail's own 26 all
// have both, so this only fires on an incomplete GLOBALB.BUN, and putting such
// a record on the grid would draw nothing.
static float carMeasure(const CarDb* db, int carType) {
	return cardb_perf_measure(db, carType);
}

// The db row a car type sits on. cardb_load keeps the array in record order so
// the two normally agree, but the type arrives from a shipped table, so it is
// looked up rather than trusted -- the same care main.c takes with the menu's.
static int dbRow(const CarDb* db, int carType) {
	for (int i = 0; i < db->count; i++)
		if (db->recs[i].carType == carType) return i;
	return -1;
}

// ------------------------------------------------------- the upgrade pass
//
// TASKS.md #288; opponent.h has the modes. Everything here is in the RAW
// measure, the integral RebuildPhysicsInfo leaves at RidePhysicsInfo +0x08,
// except where it says 0..10.

// MatchPerformance's handling target for a STAGE / FIXED entry, and the
// GetPerformanceMeasure(true, ...) axis, off Extrema.
#define EXTREMA_HANDLING_LO 28
#define EXTREMA_HANDLING_HI 29

static bool canMatch(const OpponentMatch* m) {
	return m && m->bunPath && m->pdb && m->player;
}

// GetPerformanceMeasure(true, -1) (0x801C7EF8): a raw measure on the 0..10
// axis, not clamped.
static float normMeasure(const CarDb* db, float raw) {
	return (raw - db->measureLo) / (db->measureHi - db->measureLo) * CARDB_MEASURE_SCALE;
}

// The parts database the field is dressed from (TASKS.md #296): read for one
// build and freed at its end, the field keeping only each car's part hashes.
static CarPartDb s_parts;
static bool s_partsOk;

static void partsOpen(const OpponentMatch* m, void (*log)(const char* msg)) {
	s_partsOk = canMatch(m) && carpartdb_load(m->bunPath, &s_parts);
	if (!s_partsOk && log)
		log("opponents: no car parts database -- the field is drawn as stock cars");
}

static void partsClose(void) {
	if (s_partsOk)
		carpartdb_free(&s_parts);
	s_partsOk = false;
}

// The car `row` of `db`'s spec built to `measure` (and `handling`, < 0 for
// none): MatchPerformance(float, false, handling) (0x801C91C0) on its own
// table. With `nitrous` >= 0 the rest of MatchPerformance(other, true, ...)
// (0x801C9108) follows: InstallAllPerfParts(the player's GetUpgradeLevel(path
// 2), 2) (0x801C45B8) -- UninstallAllPerfParts(2), then the parts of each
// level-1..L nitrous package in -- and RebuildPhysicsInfo, whose levels the
// spec keeps. False, leaving `spec` alone, when the record or the table
// cannot be read.
static bool matchSpec(const CarDb* db, int row, const OpponentMatch* m, float measure,
		float handling, int nitrous, RideSpec* spec) {
	static u8 compleat[RIDEPHYS_COMPLEAT_SIZE];
	static u8 rows[CARDB_PERF_MAX_ROWS * CARDB_PERF_ROW_SIZE];
	static RidePhys rp;
	const int carType = db->recs[row].carType;
	if (!canMatch(m) || !db->recs[row].phys.valid ||
			!cardb_read_compleat(m->bunPath, db, carType, compleat))
		return false;
	const int n = cardb_read_perf_rows(m->bunPath, db, carType, rows, CARDB_PERF_MAX_ROWS);
	if (n <= 0)
		return false;
	u8 inst[PERFDB_NUM_PARTS];
	memset(inst, 0, sizeof(inst));
	ridephys_match_performance(&rp, inst, compleat, m->pdb, rows, n, measure, handling,
		db->extremaOk ? db->extrema : NULL);
	if (nitrous >= 0) {
		perfdb_reset_to_stock(m->pdb, inst, PERF_PKG_NITROUS);
		for (int l = PERFDB_LEVEL_MIN; l <= nitrous && l <= PERFDB_LEVEL_MAX; l++) {
			const PerformancePackage* p = perfdb_package(m->pdb, PERF_PKG_NITROUS, l);
			for (u32 k = 0; p && k < p->numParts && k < PERFDB_MAX_PARTS; k++)
				perfdb_install_part(m->pdb, inst, (int)p->part[k].partId, true, false);
		}
		ridephys_build(&rp, compleat, m->pdb, inst, NULL);
	}
	memcpy(spec->inst, inst, sizeof(inst));
	for (int p = 0; p < RIDEINFO_PATHS; p++)
		spec->norm[p] = rp.level[p];
	return true;
}

// The car as AddRacingCar leaves it: RebuildPhysicsInfo of every part the
// spec has in -- the matched ones and the aero parts SyncVisualPartsWithPhysics
// added -- and Car's ctor on GetCarPhysicsInfo, the built block, which the
// gearing, rev range and torque the sim and the driver read come out of too.
// False, leaving the sim alone, when the record cannot be read.
static bool buildSim(Opponent* o, const CarDb* db, int row, const OpponentMatch* m,
		const RideSpec* spec) {
	static u8 compleat[RIDEPHYS_COMPLEAT_SIZE];
	static RidePhys rp;
	static CarPhysInfo phys;
	if (!canMatch(m) || !db->recs[row].phys.valid ||
			!cardb_read_compleat(m->bunPath, db, db->recs[row].carType, compleat))
		return false;
	ridephys_build(&rp, compleat, m->pdb, spec->inst, NULL);
	// RebuildPhysicsInfo's type-2 tail in a drag: TuneForDragRace. The class
	// the DriveTrain takes is the untuned car's (GetCarClass).
	const int cls = ridephys_car_class(&rp);
	if (m->dragRace)
		ridephys_tune_for_drag(&rp);
	else if (m->driftRace)
		ridephys_tune_for_drift(&rp);   // type 3, TuneForDrift
	else if (m->shortTrack)
		ridephys_tune_for_short_track(&rp);   // type 10, TuneForShortTrack
	o->rideClass = ridephys_car_class(&rp);
	o->rideTop = ridephys_top_speed(&rp);
	phys = db->recs[row].phys;
	memcpy(phys.raw, rp.cpi, CARPHYS_CPI_SIZE);
	CarPerf perf = db->recs[row].perf;
	perf.valid = false;
	const float zero[3] = { 0.0f, 0.0f, 0.0f };
	carsim_init(&o->sim, &perf, &db->recs[row].chassis, &phys, zero, 0.0f);
	o->simReady = true;
	// The PhysicsMover ctor (0x801BF960) for an AI racer in a drag: its shift
	// modifiers drawn from its skill's DifficultyInfo record ([13]..[14] up,
	// [11]..[12] down, [15]..[16] third's up). ShiftSpeedMultipliers (+0xE8)
	// is only taken outside a career session (FEDatabase+0x125F0 != 1): a
	// Quick Race drag's (#419), indexed by the race's AI skill (+0x78); the
	// career's clutch keeps its 1.
	if (m->dragRace) {
		static const float kShiftSpeedMultipliers[3] = { 0.4f, 0.8f, 1.0f };
		const int sk = o->skill;
		const int rs = m->aiSkill < 0 ? 0 : m->aiSkill > 2 ? 2 : m->aiSkill;
		const float lo[3] = { difficulty_field(sk, 13), difficulty_field(sk, 11),
			difficulty_field(sk, 15) };
		const float hi[3] = { difficulty_field(sk, 14), difficulty_field(sk, 12),
			difficulty_field(sk, 16) };
		carsim_set_shift_tuning(&o->sim, cls, 1.0f,
			m->quickRace ? kShiftSpeedMultipliers[rs] : 1.0f, lo, hi);
		carsim_set_drag(&o->sim, true);
	} else {
		carsim_set_shift_tuning(&o->sim, cls, 1.0f, 1.0f, NULL, NULL);
	}
	if (m->driftRace)
		carsim_set_drift(&o->sim, true, m->aiSkill);
	o->parts = 0;
	for (int k = 0; k < PERFDB_NUM_PARTS; k++)
		o->parts += spec->inst[k] != 0;
	o->sndEngLevel = spec->inst[0x18] ? 3 : spec->inst[0x13] ? 2 : spec->inst[0xf] ? 1 : 0;
	o->camber = (spec->inst[0x24] ? 0.5f : 0.0f) + (spec->inst[0x23] ? 0.5f : 0.0f);
	const int tl = (int)(rp.level[PERF_PKG_TRANSMISSION] + 0.5f);   // GetUpgradeLevel
	o->sndTransLevel = tl < 0 ? 0 : (tl > 3 ? 3 : tl);
	const int ul = (int)(rp.level[PERF_PKG_TURBO] + 0.5f);
	o->sndTurboLevel = ul < 0 ? 0 : (ul > 3 ? 3 : ul);
	if (db->measureOk)
		o->perf = normMeasure(db, rp.measure);
	return true;
}

// MatchPerformance(the player's ride, true, !drag) (0x801C9108), what a PLAYER
// entry and every Quick Race driver get: the player's raw measure and, but in
// a drag race, its handling rating; then its nitrous level. The class is
// GetCarClass of the player's ride. False when the car stays stock.
static bool matchToPlayer(Opponent* o, const CarDb* db, int row, const OpponentMatch* m,
		RideSpec* spec) {
	if (!canMatch(m))
		return false;
	const RidePhys* pl = m->player;
	const float handling = (m->dragRace || !db->extremaOk) ? -1.0f :
		ridephys_rating(pl, db->extrema, RIDEPHYS_RATING_HANDLING);
	// GetUpgradeLevel (0x801C41DC): the spec's level, rounded.
	const int nitrous = (int)(pl->level[PERF_PKG_NITROUS] + 0.5f);
	if (!matchSpec(db, row, m, pl->measure, handling, nitrous, spec))
		return false;
	o->carClass = aidriver_car_class_raw(pl->measure);
	o->classSet = true;
	return true;
}

// --------------------------------------------------- PickRandomCarsOfSomeSort
//
// The whole of 0x80119F24 bar the RideInfo it fills: the region's car list (or
// the SUV list), a shuffle, then passes over the shuffled order with a
// doubling tolerance until a car's measure is at most the target plus it.
//
// Two details of the original worth keeping in view. The shuffle draws its
// index from the FULL count every step rather than from the shrinking tail, so
// it is not a Fisher-Yates and does not produce a uniform permutation -- that
// is the engine's, and changing it would change which cars turn up. And the
// `allowPlayerCar` argument is passed true by AddCareerEventAIOpponents, so in
// career the player's own car is a legal pick; the exclusion only applies on
// the paths that pass false.
static int pickRandomCar(const CarDb* db, u32* rng, bool suv, float target,
		float playerMeasure, int playerCarType, bool allowPlayerCar) {
	const u8* list;
	int n;
	if (suv) {
		list = kCarsSUV;
		n = (int)(sizeof(kCarsSUV) / sizeof(kCarsSUV[0]));
	} else if (cardb_build_region() == 0) {
		list = kCarsNorthAmerica;
		n = (int)(sizeof(kCarsNorthAmerica) / sizeof(kCarsNorthAmerica[0]));
	} else if (cardb_build_region() == CARDB_REGION_ALL) {
		// PORT (#455): both lists, the North American one and the two cars only
		// the Rest of World one has.
		list = kCarsAllRegions;
		n = (int)(sizeof(kCarsAllRegions) / sizeof(kCarsAllRegions[0]));
	} else {
		list = kCarsRestOfWorld;
		n = (int)(sizeof(kCarsRestOfWorld) / sizeof(kCarsRestOfWorld[0]));
	}

	int order[28];
	for (int i = 0; i < n; i++) order[i] = (int)list[i];
	for (int i = 0; i < n; i++) {
		const int j = (int)bRandomNext(rng, (u32)n);
		const int t = order[i];
		order[i] = order[j];
		order[j] = t;
	}

	// The target a candidate is measured against: retail re-reads the player's
	// own measure whenever the one it was handed is at or below the sentinel,
	// which is the path a career stage whose matching parameter this port
	// cannot read takes (see opponent.h).
	const float t = target < OPPONENT_TARGET_SENTINEL ? playerMeasure : target;

	float tol = OPPONENT_TOL_BASE;
	for (int pass = 0; pass < OPPONENT_TOL_PASSES; pass++) {
		tol += tol;
		for (int k = 0; k < n; k++) {
			const int carType = order[k];
			if (!allowPlayerCar && carType == playerCarType) continue;
			const int row = dbRow(db, carType);
			if (row < 0 || !db->recs[row].hasGeometry) continue;
			const float m = carMeasure(db, carType);
			if (m < 0.0f) continue;
			if (m <= t + tol) return carType;
		}
	}
	return -1;
}

// ------------------------------------------------------------ the dressing
//
// The RideInfo every opponent is given (TASKS.md #296, rideinfo.h). The log
// lines carry what tools/randomride_check.py takes to print the same parts
// offline: the car, the bRandom state before the draws and the ten levels.

// The parts of `r` that SetStockParts would not have given it, as the twin
// lists them: GC slot, part name, a few to a line.
static void logParts(int i, const RideInfo* r, void (*log)(const char* msg)) {
	static RideInfo stock;
	rideinfo_init(&stock, r->db, r->car);
	rideinfo_set_stock_parts(&stock);
	char b[256];
	int n = 0;
	for (int s = 0; s < CARPARTDB_SLOTS; s++) {
		if (r->part[s] == stock.part[s])
			continue;
		const char* name = r->part[s] ? carpartdb_name(r->db, r->part[s]) : NULL;
		if (n > 160) {
			log(b);
			n = 0;
		}
		if (n == 0)
			n = snprintf(b, sizeof(b), "rideinfo: opponent %d parts", i);
		n += snprintf(b + n, sizeof(b) - (size_t)n, " %02x %s", s, name ? name : "-");
	}
	if (n)
		log(b);
}

static void logRide(int i, const char* what, const RideInfo* r, u32 rng,
		void (*log)(const char* msg)) {
	char b[200];
	int n = snprintf(b, sizeof(b), "rideinfo: opponent %d %s %s rng 0x%08lx levels ", i,
		r->car->name, what, (unsigned long)rng);
	for (int p = 0; p < RIDEINFO_PATHS; p++)
		n += snprintf(b + n, sizeof(b) - (size_t)n, p ? ",%d" : "%d",
			rideinfo_upgrade_level(&r->spec, p));
	log(b);
}

// SyncVisualPartsWithPhysics(ride, true, random): the aero parts matched to
// the tyre, weight and suspension levels and, for a car picked at random,
// BuildRandomRide; then the parts as the hashes the field keeps.
static void dressRide(Opponent* o, int i, RideInfo* r, const OpponentMatch* m, bool random,
		u32* rng, void (*log)(const char* msg)) {
	const u32 before = *rng;
	rideinfo_sync_visual(r, m->pdb, random, rng);
	rideinfo_slots(r, o->ride);
	if (log) {
		logRide(i, random ? "ride" : "kept", r, before, log);
		logParts(i, r, log);
	}
}

// BuildRandomSkin alone, for the car that keeps the one before it's ride.
static void reskinRide(int i, RideInfo* r, u32* rng, void (*log)(const char* msg)) {
	const u32 before = *rng;
	rideinfo_build_random_skin(r, rng);
	if (log) {
		logRide(i, "skin", r, before, log);
		logParts(i, r, log);
	}
}

// ------------------------------------------------ AddCareerEventAIOpponents

int opponent_build(OpponentField* f, const EventRec* ev, const CarDb* db,
		int playerCarType, int difficulty, u32* rng, const OpponentMatch* match,
		void (*log)(const char* msg)) {
	char b[320];
	if (!f) return 0;
	memset(f, 0, sizeof(*f));
	if (!ev || !db || db->count <= 0) return 0;

	f->difficulty = difficulty < 0 ? 0 : (difficulty > 2 ? 2 : difficulty);

	int count = (int)ev->opponentCount;
	if (count > OPPONENT_MAX) count = OPPONENT_MAX;
	if (count <= 0) {
		if (log) {
			snprintf(b, sizeof(b), "opponents: %s carries none -- the player races alone",
				ev->name);
			log(b);
		}
		return 0;
	}

	// GetPerformanceMeasure(true, -1) of driver 0's ride: the player's car as
	// built, its own parts fitted, when the caller has it.
	float pm = carMeasure(db, playerCarType);
	if (canMatch(match) && db->measureOk)
		pm = normMeasure(db, match->player->measure);
	const float playerMeasure = pm < 0.0f ? 0.0f : pm;
	// GetMinMaxPerformanceMeasure (0x801C9748) of the PLAYER's car type: its
	// table's first and last rows, the range a STAGE / FIXED target is a share
	// of.
	const int playerRow = dbRow(db, playerCarType);
	const bool playerRange = playerRow >= 0 && db->recs[playerRow].measureValid;

	u32 localRng = OPPONENT_RNG_SEED;
	if (!rng) rng = &localRng;
	f->rngSeed = *rng;
	int prevCarType = -1;
	// The one RideInfo the loop fills, as retail's: each iteration but a
	// reused one starts it again. It starts holding no car (Init(-1)), so the
	// first preset always builds its own.
	static RideInfo ride;
	memset(&ride, 0, sizeof(ride));
	static PresetCar preset;
	partsOpen(match, log);

	for (int i = 0; i < count; i++) {
		const EventOpponent* src = &ev->opponents[i];
		Opponent* o = &f->car[i];
		o->raceCar = -1;
		snprintf(o->name, sizeof(o->name), "%s", src->name);
		o->skill = src->skill[f->difficulty];
		o->slot = i + 2;

		// The target this opponent is built to. Retail starts every entry at
		// the player's own measure and overwrites it for the two modes that
		// name something else; mode 1 (PLAYER) and mode 2 (NONE) both leave it.
		float target = playerMeasure;
		if (src->match == EVENTOPP_MATCH_FIXED) {
			target = (float)src->matchValue;
		} else if (src->match == EVENTOPP_MATCH_STAGE) {
			// GetAIPerformanceMatchingParameter of the career's current stage,
			// from the caller. Without it, retail's own sentinel, which sends
			// the picker down its "match the player" path.
			target = match && match->stageParam >= 0.0f ? match->stageParam : -1.0f;
		}

		// The one quirk of the loop: with more than two opponents, a RANDOM
		// second-to-last keeps the RideInfo as the previous iteration left it
		// -- same car, same parts, a fresh BuildRandomSkin -- instead of
		// picking again. A preset there is filled like any other.
		const bool random = src->carHash == EVENTDB_OPPONENT_CAR_RANDOM;
		const bool reuse = random && count > 2 && i == count - 2 && prevCarType >= 0;
		int carType = -1;
		bool fromPreset = false;
		char how[64] = "random";
		if (reuse) {
			carType = prevCarType;
			snprintf(how, sizeof(how), "same as the car before it");
		} else if (!random) {
			// FindFEPresetCar, then GetCarTypeInfoFromHash of its car name.
			int prow = -1;
			if (match && match->bunPath && presetcar_load(match->bunPath, src->carHash, &preset))
				for (int k = 0; k < db->count && prow < 0; k++)
					if (db->recs[k].nameHash == preset.carHash)
						prow = k;
			if (prow >= 0) {
				carType = db->recs[prow].carType;
				fromPreset = true;
				snprintf(how, sizeof(how), "preset %s", preset.name);
			} else {
				// PORT: retail has no fallback; the shipped presets are all
				// there, so this is a card missing GLOBALB.BUN's table.
				snprintf(how, sizeof(how), "preset 0x%08lX not on this card, picked at random",
					(unsigned long)src->carHash);
			}
		}
		if (carType < 0)
			carType = pickRandomCar(db, rng, ev->isSUV != 0, target,
				playerMeasure, playerCarType, true);
		// Debug (DbgFieldCar): the random pick swapped for the named car, the
		// rng having moved as it would.
		if (random && !reuse && match && match->forceCarDir[0])
			for (int k = 0; k < db->count; k++)
				if (strcasecmp(db->recs[k].geomDir, match->forceCarDir) == 0) {
					carType = db->recs[k].carType;
					snprintf(how, sizeof(how), "DbgFieldCar %s", match->forceCarDir);
					break;
				}
		if (carType < 0) {
			if (log) {
				snprintf(b, sizeof(b), "opponents: %s slot %d -- no car in the table fits; "
					"skipped", o->name, o->slot);
				log(b);
			}
			continue;
		}

		const int row = dbRow(db, carType);
		o->carType = carType;
		snprintf(o->carDir, sizeof(o->carDir), "%s", row >= 0 ? db->recs[row].geomDir : "");
		const float om = carMeasure(db, carType);
		o->perf = om < 0.0f ? 0.0f : om;
		prevCarType = carType;
		// The picker's Init + SetStockParts(0), FillWithPreset, or the kept
		// ride's new skin.
		if (reuse) {
			if (s_partsOk)
				reskinRide(i, &ride, rng, log);
		} else if (fromPreset) {
			const int missing = rideinfo_fill_with_preset(&ride, s_partsOk ? &s_parts : NULL,
				&db->recs[row], &preset, canMatch(match) ? match->pdb : NULL);
			if (missing) {
				const size_t n = strlen(how);
				snprintf(how + n, sizeof(how) - n, " (%d part(s) not in the database)", missing);
			}
		} else if (row >= 0) {
			rideinfo_init(&ride, s_partsOk ? &s_parts : NULL, &db->recs[row]);
			if (s_partsOk)
				rideinfo_set_stock_parts(&ride);
		}

		// The upgrade pass, by the entry's mode (opponent.h). The sim is built
		// on the result, parked at the origin until opponent_place() puts it
		// on its grid slot.
		const char* built = "stock";
		if (row >= 0 && src->match == EVENTOPP_MATCH_PLAYER) {
			if (matchToPlayer(o, db, row, match, &ride.spec))
				built = "matched to the player";
		} else if (row >= 0 && src->match != EVENTOPP_MATCH_NONE && canMatch(match) &&
				playerRange && !(src->match == EVENTOPP_MATCH_STAGE && match->stageParam < 0.0f)) {
			// 0x8011A488: the stage parameter, the entry's byte for FIXED, 0
			// for anything else; clamped to 0..10.
			float p = src->match == EVENTOPP_MATCH_STAGE ? match->stageParam :
				src->match == EVENTOPP_MATCH_FIXED ? (float)src->matchValue : 0.0f;
			p = p < 0.0f ? 0.0f : (p > CARDB_MEASURE_SCALE ? CARDB_MEASURE_SCALE : p);
			const float lo = db->recs[playerRow].measureStock;
			const float hi = db->recs[playerRow].measureMax;
			const float raw = p / CARDB_MEASURE_SCALE * (hi - lo) + lo;
			const float handling = (match->dragRace || !db->extremaOk) ? -1.0f :
				p / CARDB_MEASURE_SCALE * (db->extrema[EXTREMA_HANDLING_HI] -
					db->extrema[EXTREMA_HANDLING_LO]) + db->extrema[EXTREMA_HANDLING_LO];
			if (matchSpec(db, row, match, raw, handling, -1, &ride.spec)) {
				built = src->match == EVENTOPP_MATCH_STAGE ? "matched to the stage" :
					"matched to its fixed measure";
				// CalculateCarClass(raw, true): the raw measure is clamped as
				// if it were 0..10, so this is class 0 for every car.
				o->carClass = aidriver_car_class(raw, db->measureLo, db->measureHi);
				o->classSet = true;
				// Then its nitrous to the player's: MatchUpgradeLevel(the
				// player's GetNormalizedLevel(2), path 2).
				const int nitrous = PERF_PKG_NITROUS;
				rideinfo_match_upgrade_level(&ride.spec, match->pdb,
					match->player->level[PERF_PKG_NITROUS] / 3.0f, &nitrous, 1);
			}
		}
		// SyncVisualPartsWithPhysics(ride, true, picked at random), then the
		// car built on every part it ended up with.
		if (row >= 0 && s_partsOk)
			dressRide(o, i, &ride, match, random && !reuse, rng, log);
		if (row >= 0 && canMatch(match))
			buildSim(o, db, row, match, &ride.spec);
		if (row >= 0 && !o->simReady) {
			const float zero[3] = { 0.0f, 0.0f, 0.0f };
			carsim_init(&o->sim, &db->recs[row].perf, &db->recs[row].chassis,
				&db->recs[row].phys, zero, 0.0f);
			o->simReady = true;
			// Stock (no ride to tune): still the drift's physics.
			if (match && match->driftRace)
				carsim_set_drift(&o->sim, true, match->aiSkill);
		}

		f->count = i + 1;
		if (log) {
			char cls[16] = "";
			if (o->classSet)
				snprintf(cls, sizeof(cls), ", class %d", o->carClass);
			snprintf(b, sizeof(b), "opponents: slot %d %s -- %s (type %d, perf %.2f vs "
				"player %.2f), skill %d, %s; %s, %d part(s)%s", o->slot, o->name,
				row >= 0 ? db->recs[row].name : "?", carType, o->perf, playerMeasure,
				(int)o->skill, how, built, o->parts, cls);
			log(b);
		}
	}
	partsClose();
	return f->count;
}

// ----------------------------------------------------- AddAIOpponentCars
//
// QuickRaceCharacterNames (0x8035B90C): the 53 names a Quick Race field is
// drawn from, in the image's order.
static const char* const kQuickRaceNames[53] = {
	"Phil", "Scott", "Kristian", "Carl", "Steve", "Stefan", "Jason", "Greg",
	"Marc", "Audrey", "Ben", "Mike", "Vlad", "Russ", "Neil", "Mario", "Al",
	"Carla", "Frank", "Carol", "Serena", "Bill", "Antonio", "Michelle",
	"Vince", "Amy", "Matt", "Bryce", "Clarence", "Ryan", "Eric", "Daryl",
	"Stan", "Brent", "Dana", "Hector", "Stacey", "Alex", "Simon", "Timmy",
	"Terry", "Jackie", "Alden", "Jared", "Paul", "Arnel", "Jamey", "Brian",
	"Gary", "Lola", "Kirby", "Don", "Marcel"
};
#define QR_NAME_COUNT 53
// GetRandomCharacterNames is always asked for eight, whatever the field.
#define QR_NAMES_DRAWN 8
// Its draws per name before it gives up on finding an unused one (0x63
// inclusive).
#define QR_NAME_TRIES 100

// GetRandomCharacterNames (0x80119BD0): for each name, up to a hundred
// bRandom(53) draws until one lands on a name not yet taken. A name whose
// hundred draws all collide is left unset; with 53 names and 8 asked for,
// that does not happen.
void opponent_random_character_names(const char** out, int n, u32* rng) {
	bool used[QR_NAME_COUNT];
	memset(used, 0, sizeof(used));
	for (int i = 0; i < n; i++) {
		out[i] = NULL;
		for (int t = 0; t < QR_NAME_TRIES; t++) {
			const int k = (int)bRandomNext(rng, QR_NAME_COUNT);
			if (used[k]) continue;
			used[k] = true;
			out[i] = kQuickRaceNames[k];
			break;
		}
	}
}

static void quickRaceNames(const char* out[QR_NAMES_DRAWN], u32* rng) {
	opponent_random_character_names(out, QR_NAMES_DRAWN, rng);
}

const char* opponent_random_character_name(u32* rng) {
	return kQuickRaceNames[bRandomNext(rng, QR_NAME_COUNT)];
}

int opponent_career_grid(OpponentField* f, int kind, int prevPlace, u32* rng) {
	const int n = f ? f->count : 0;
	int player = kind == 2 ? n + 1 : 1;
	for (int i = 0; i < n; i++)
		f->car[i].slot = kind == 2 ? i + 1 : i + 2;
	if (kind == 3 && n > 0) {
		const int d = (int)bRandomNext(rng, (u32)(1 + n));
		if (d > 0) {
			const int s = f->car[d - 1].slot;
			f->car[d - 1].slot = player;
			player = s;
		}
	}
	if (n > 0 && prevPlace > 0 && prevPlace != player) {
		for (int i = 0; i < n; i++) {
			if (f->car[i].slot == prevPlace) {
				f->car[i].slot = player;
				player = prevPlace;
				break;
			}
		}
	}
	return player;
}

int opponent_build_quickrace(OpponentField* f, int count, const CarDb* db,
		int playerCarType, int strength, u32* rng, int* playerSlot,
		const OpponentMatch* match, int startKind, void (*log)(const char* msg)) {
	char b[320];
	if (playerSlot) *playerSlot = 1;
	if (!f) return 0;
	memset(f, 0, sizeof(*f));
	if (!db || db->count <= 0) return 0;
	if (count > OPPONENT_MAX) count = OPPONENT_MAX;
	// 0x8011A694: no opponents, no field -- and no names drawn either.
	if (count <= 0) {
		if (log) log("opponents: quick race with no opponents -- the player races alone");
		return 0;
	}
	f->difficulty = strength < 0 ? 0 : (strength > 2 ? 2 : strength);

	float pm = carMeasure(db, playerCarType);
	if (canMatch(match) && db->measureOk)
		pm = normMeasure(db, match->player->measure);
	const float playerMeasure = pm < 0.0f ? 0.0f : pm;

	u32 localRng = OPPONENT_RNG_SEED;
	if (!rng) rng = &localRng;
	f->rngSeed = *rng;

	const char* names[QR_NAMES_DRAWN];
	quickRaceNames(names, rng);

	for (int i = 0; i < count; i++) {
		Opponent* o = &f->car[i];
		o->raceCar = -1;
		o->carType = -1;
		snprintf(o->name, sizeof(o->name), "%s", names[i] ? names[i] : "");
		o->slot = i + 2;
		// PickRandomCarsOfSomeSort(ride, 100 + i, i + 2, true, player, NULL,
		// -1.0): no event, so never the SUV list, and the -1.0 sends the
		// picker to the player's own measure.
		const int carType = pickRandomCar(db, rng, false, -1.0f, playerMeasure,
			playerCarType, true);
		// DecideAISkillLevel(TheRaceParameters+0x78, 100 + i): the Quick
		// Race's own difficulty is the opponent strength.
		o->skill = (s8)difficulty_pick_skill(f->difficulty, rng);
		if (carType < 0) {
			if (log) {
				snprintf(b, sizeof(b), "opponents: %s slot %d -- no car in the table fits; "
					"skipped", o->name, o->slot);
				log(b);
			}
			continue;
		}
		o->carType = carType;
		f->count = i + 1;
	}

	// MatchAICarPerformanceToPlayer(count > 1 ? count - 1 : 0) (0x8011A90C):
	// every AI driver is upgraded to the reference -- the quickest human
	// driver's ride, here the player's -- with MatchPerformance(ride, true,
	// !drag), the ForceAICar*ToPerfRating tweaks all shipping at -1, and takes
	// its class, then SyncVisualPartsWithPhysics(ride, true, true). From that
	// AI index on, each opponent instead takes the one before it's RideInfo,
	// matched physics, parts and all, with a fresh BuildRandomSkin -- so with
	// two or more, the last drives the same car as the second-to-last.
	const int sameFrom = count > 1 ? count - 1 : 0;
	static RideInfo ride;
	bool matched[OPPONENT_MAX] = { false };
	partsOpen(match, log);
	for (int i = 0; i < f->count; i++) {
		Opponent* o = &f->car[i];
		if (o->carType < 0)
			continue;
		const bool same = sameFrom != 0 && i >= sameFrom && i > 0 && f->car[i - 1].carType >= 0;
		if (same) {
			o->carType = f->car[i - 1].carType;
			matched[i] = matched[i - 1];
			o->carClass = f->car[i - 1].carClass;
			o->classSet = f->car[i - 1].classSet;
		}
		const int row = dbRow(db, o->carType);
		snprintf(o->carDir, sizeof(o->carDir), "%s", row >= 0 ? db->recs[row].geomDir : "");
		const float om = carMeasure(db, o->carType);
		o->perf = om < 0.0f ? 0.0f : om;
		if (row < 0)
			continue;
		if (same) {
			if (s_partsOk) {
				reskinRide(i, &ride, rng, log);
				rideinfo_slots(&ride, o->ride);
			}
		} else {
			// The picker's Init + SetStockParts(0), which draw nothing.
			rideinfo_init(&ride, s_partsOk ? &s_parts : NULL, &db->recs[row]);
			if (s_partsOk)
				rideinfo_set_stock_parts(&ride);
			matched[i] = matchToPlayer(o, db, row, match, &ride.spec);
			if (s_partsOk)
				dressRide(o, i, &ride, match, true, rng, log);
		}
		if (canMatch(match))
			buildSim(o, db, row, match, &ride.spec);
		if (!o->simReady) {
			const float zero[3] = { 0.0f, 0.0f, 0.0f };
			carsim_init(&o->sim, &db->recs[row].perf, &db->recs[row].chassis,
				&db->recs[row].phys, zero, 0.0f);
			o->simReady = true;
		}
	}
	partsClose();

	// DetermineStartingPositions(3) (0x8011ADBC): the player first, the AI
	// behind in roster order, then RandomizePlayerStartPositions (0x8011B07C)
	// swaps the player's position with that of driver bRandom(players +
	// opponents) -- driver 0 being the player, so it may stay on pole.
	int pSlot = 1;
	int swap = 0;
	if (startKind == 2) {
		// DetermineStartingPositions(2): the AI first in roster order, the
		// player behind them; no RandomizePlayerStartPositions.
		int n = 0;
		for (int i = 0; i < f->count; i++)
			if (f->car[i].carType >= 0)
				f->car[i].slot = ++n;
		pSlot = n + 1;
	} else {
		swap = (int)bRandomNext(rng, (u32)(1 + count));
		if (swap > 0 && swap - 1 < f->count) {
			pSlot = f->car[swap - 1].slot;
			f->car[swap - 1].slot = 1;
		}
	}
	if (playerSlot) *playerSlot = pSlot;

	for (int i = 0; log && i < f->count; i++) {
		const Opponent* o = &f->car[i];
		if (o->carType < 0)
			continue;
		const int row = dbRow(db, o->carType);
		snprintf(b, sizeof(b), "opponents: slot %d %s -- %s (type %d, perf %.2f vs "
			"player %.2f), skill %d, %s; %s, %d part(s)", o->slot, o->name,
			row >= 0 ? db->recs[row].name : "?", o->carType, o->perf, playerMeasure,
			(int)o->skill, (sameFrom != 0 && i >= sameFrom && i > 0)
				? "same car as the one before it" : "random",
			matched[i] ? "matched to the player" : "stock", o->parts);
		log(b);
	}
	if (log) {
		if (startKind == 2)
			snprintf(b, sizeof(b), "opponents: quick race field of %d, strength %d -- "
				"DetermineStartingPositions(2), the player starts in position %d",
				f->count, f->difficulty, pSlot);
		else
			snprintf(b, sizeof(b), "opponents: quick race field of %d, strength %d -- the "
				"player starts in position %d (RandomizePlayerStartPositions drew driver %d)",
				f->count, f->difficulty, pSlot, swap);
		log(b);
	}
	return f->count;
}

// ------------------------------------------------------------ LAN race field

int opponent_build_remote(OpponentField* f, const OpponentRemote* r, int n,
		const CarDb* db, void (*log)(const char* msg)) {
	char b[192];
	if (!f) return 0;
	memset(f, 0, sizeof(*f));
	if (!db || db->count <= 0 || !r) return 0;
	if (n > OPPONENT_MAX) n = OPPONENT_MAX;
	f->difficulty = 1;
	f->rngSeed = OPPONENT_RNG_SEED;
	for (int i = 0; i < n; i++) {
		const int row = dbRow(db, r[i].carType);
		if (row < 0) {
			if (log) {
				snprintf(b, sizeof(b), "opponents: LAN racer %s (console %d) -- car type %d "
					"is not in the table; left out", r[i].name ? r[i].name : "?",
					r[i].console, r[i].carType);
				log(b);
			}
			continue;
		}
		Opponent* o = &f->car[f->count];
		o->raceCar = -1;
		o->carType = r[i].carType;
		snprintf(o->name, sizeof(o->name), "%s", r[i].name ? r[i].name : "");
		snprintf(o->carDir, sizeof(o->carDir), "%s", db->recs[row].geomDir);
		o->slot = r[i].slot;
		o->skill = 5;
		o->camber = r[i].camber;
		o->sndEngLevel = r[i].sndEng;
		o->sndTransLevel = r[i].sndTrans;
		o->sndTurboLevel = r[i].sndTurbo;
		if (r[i].ride)
			memcpy(o->ride, r[i].ride, sizeof(o->ride));
		const float om = carMeasure(db, o->carType);
		o->perf = om < 0.0f ? 0.0f : om;
		const float zero[3] = { 0.0f, 0.0f, 0.0f };
		carsim_init(&o->sim, &db->recs[row].perf, &db->recs[row].chassis,
			&db->recs[row].phys, zero, 0.0f);
		o->simReady = true;
		o->remote = true;
		o->console = r[i].console;
		o->netIn.aiDriver = true;
		o->netIn.steerIsAngle = true;
		o->netIn.gear = -1;
		f->count++;
		if (log) {
			snprintf(b, sizeof(b), "opponents: LAN racer %s (console %d) -- %s (type %d), "
				"grid position %d%s", o->name, o->console, db->recs[row].name, o->carType,
				o->slot, o->ride[0] ? ", in its own ride" : ", stock");
			log(b);
		}
	}
	return f->count;
}

// ---------------------------------------------------- AddRandomEncounterCars

int opponent_build_encounter(OpponentField* f, int count, const CarDb* db,
		int playerCarType, int strength, u32* rng, const OpponentMatch* match,
		void (*log)(const char* msg)) {
	char b[320];
	if (!f) return 0;
	memset(f, 0, sizeof(*f));
	if (!db || db->count <= 0) return 0;
	if (count > OPPONENT_MAX) count = OPPONENT_MAX;
	if (count <= 0) return 0;
	f->difficulty = strength < 0 ? 0 : (strength > 2 ? 2 : strength);

	float pm = carMeasure(db, playerCarType);
	if (canMatch(match) && db->measureOk)
		pm = normMeasure(db, match->player->measure);
	const float playerMeasure = pm < 0.0f ? 0.0f : pm;

	u32 localRng = OPPONENT_RNG_SEED;
	if (!rng) rng = &localRng;
	f->rngSeed = *rng;

	// GetRandomCharacterNames(names, 8): eight drawn, the first `count` used.
	const char* names[8];
	opponent_random_character_names(names, 8, rng);

	// RandomEncounterState's own RideInfo per car (GetRideInfo, +0x0 stride
	// 0x368): filled by the first loop, matched and dressed by the second.
	static RideInfo rides[OPPONENT_MAX];
	bool matched[OPPONENT_MAX] = { false };
	partsOpen(match, log);
	const bool suv = isSuv(playerCarType);
	for (int i = 0; i < count; i++) {
		Opponent* o = &f->car[i];
		o->raceCar = -1;
		o->carType = -1;
		o->inactive = true;
		snprintf(o->name, sizeof(o->name), "%s", names[i] ? names[i] : "");
		o->slot = i + 2;
		// PickRandomCarsOfSomeSort(ride, 100 + i, i + 2, true, NULL, NULL,
		// -1.0): the player's measure, and the SUV list when the player's car
		// is an SUV.
		const int carType = pickRandomCar(db, rng, suv, -1.0f, playerMeasure,
			playerCarType, true);
		const int row = carType >= 0 ? dbRow(db, carType) : -1;
		if (row >= 0) {
			// Init + SetStockParts(0), SetRandomParts (SetStockParts(0) again),
			// SetRandomPaint and SetRandomPart(0x9A, 2), each drawing on bRandom
			// in that order.
			rideinfo_init(&rides[i], s_partsOk ? &s_parts : NULL, &db->recs[row]);
			if (s_partsOk) {
				rideinfo_set_stock_parts(&rides[i]);
				rideinfo_set_random_paint(&rides[i], rng);
				rideinfo_set_random_part(&rides[i], 0x9A, 2, rng);
			}
		}
		// DecideAISkillLevel(DAT_803FCAB8, 100 + i): the career's difficulty.
		o->skill = (s8)difficulty_pick_skill(f->difficulty, rng);
		if (row < 0) {
			if (log) {
				snprintf(b, sizeof(b), "encounter: %s slot %d -- no car in the table fits; "
					"skipped", o->name, o->slot);
				log(b);
			}
			continue;
		}
		o->carType = carType;
		f->count = i + 1;
	}

	// MatchAICarPerformanceToPlayer(0): every AI driver matched to the player
	// with SyncVisualPartsWithPhysics(ride, true, true); with 0 no car takes
	// the one before it's ride.
	for (int i = 0; i < f->count; i++) {
		Opponent* o = &f->car[i];
		if (o->carType < 0)
			continue;
		const int row = dbRow(db, o->carType);
		snprintf(o->carDir, sizeof(o->carDir), "%s", row >= 0 ? db->recs[row].geomDir : "");
		const float om = carMeasure(db, o->carType);
		o->perf = om < 0.0f ? 0.0f : om;
		if (row < 0)
			continue;
		matched[i] = matchToPlayer(o, db, row, match, &rides[i].spec);
		if (s_partsOk)
			dressRide(o, i, &rides[i], match, true, rng, log);
		if (canMatch(match))
			buildSim(o, db, row, match, &rides[i].spec);
		if (!o->simReady) {
			const float zero[3] = { 0.0f, 0.0f, 0.0f };
			carsim_init(&o->sim, &db->recs[row].perf, &db->recs[row].chassis,
				&db->recs[row].phys, zero, 0.0f);
			o->simReady = true;
		}
	}
	partsClose();

	for (int i = 0; log && i < f->count; i++) {
		const Opponent* o = &f->car[i];
		if (o->carType < 0)
			continue;
		const int row = dbRow(db, o->carType);
		snprintf(b, sizeof(b), "encounter: racer %d %s -- %s (type %d, perf %.2f vs "
			"player %.2f), skill %d, %s, %d part(s)", i, o->name,
			row >= 0 ? db->recs[row].name : "?", o->carType, o->perf, playerMeasure,
			(int)o->skill, matched[i] ? "matched to the player" : "stock", o->parts);
		log(b);
	}
	return f->count;
}

int opponent_load_models(OpponentField* f, const char* dataRoot,
		void (*log)(const char* msg)) {
	char path[600], b[224];
	int resident = 0;
	if (!f || !dataRoot) return 0;
	for (int i = 0; i < f->count; i++) {
		Opponent* o = &f->car[i];
		if (o->model || o->carDir[0] == '\0') continue;
		// Drawn as the stock car, so its lights are the stock car's too.
		memset(o->ride, 0, sizeof(o->ride));
		// One load per distinct stock car: an assembled one wears its own
		// paint and is nobody else's.
		for (int k = 0; k < i; k++) {
			if (f->car[k].model && !f->car[k].ride[0] &&
					strcmp(f->car[k].carDir, o->carDir) == 0) {
				o->model = f->car[k].model;
				o->ownsModel = false;
				break;
			}
		}
		if (o->model) continue;
		snprintf(path, sizeof(path), "%s/CARS/%s.nfsc", dataRoot, o->carDir);
		o->model = model3d_load(path);
		o->ownsModel = o->model != NULL;
		if (o->model) resident++;
		if (log) {
			snprintf(b, sizeof(b), "opponents: slot %d car CARS/%s.nfsc %s", o->slot,
				o->carDir, o->model ? "loaded" : "MISSING or invalid");
			log(b);
		}
	}
	// An opponent whose own car would not load still races: it borrows the
	// first model that did, so the grid is not short a car. One with nothing
	// to borrow is simply not drawn.
	Model3D* fallback = NULL;
	for (int i = 0; i < f->count; i++)
		if (f->car[i].model) { fallback = f->car[i].model; break; }
	for (int i = 0; i < f->count; i++) {
		if (f->car[i].model || !fallback) continue;
		f->car[i].model = fallback;
		f->car[i].ownsModel = false;
	}
	return resident;
}

// The car's own world when it has one (TASKS.md #280), else the caller's.
static const CarSimWorld* carWorld(const Opponent* o, const CarSimWorld* w) {
	return o->world.groundProbe ? &o->world : w;
}

void opponent_place(OpponentField* f, int i, const CarSimWorld* w,
		const float pos[3], float yaw) {
	if (!f || i < 0 || i >= f->count || !f->car[i].simReady) return;
	carsim_place(&f->car[i].sim, carWorld(&f->car[i], w), pos, yaw);
}

// ------------------------------------------------------------- the driving
//
// Retail hands each AI car a RaceCarController, which decides a goal, turns it
// into a DrivingChoice, builds a DriveTarget out of the FuturePoints window
// and hands that to a PsychoDriver. aidriver.h says what of that is ported and
// what the race route stands in for; this is the part that joins the two.

// The racer's DrivingChoice (TASKS.md #280). Retail keeps every racer on line
// 0 and moves it off the line only to get round what is in front of it.
// AIActionAvoidAvoidable takes over from AIActionJustDrive once the current
// choice's drive target hits a car within AVOID_HORIZON (CalculateScore,
// 0x80012724), and its CalculatePointsToDrive (0x80012A9C) asks DrivingChoice::
// ChooseOffsetFromDrivingLine (0x8001E09C) for an offset that does not;
// JustDrive's TryAndDriveBestLine (0x8000EF34) takes the car back onto the
// line once no car is near and the way back is clear. The figures are the
// GameCube image's own, at the addresses given.
#define AVOID_HORIZON        OPPONENT_AVOID_HORIZON
#define AVOID_HOLD_SEC       OPPONENT_AVOID_HOLD_SEC
#define AVOID_STEPS          OPPONENT_AVOID_STEPS
#define AVOID_STEP_MIN       OPPONENT_AVOID_STEP_MIN
#define AVOID_RATE_MIN       OPPONENT_AVOID_RATE_MIN
#define AVOID_RATE_MAX       OPPONENT_AVOID_RATE_MAX
#define AVOID_RATE_STEP      OPPONENT_AVOID_RATE_STEP
#define AVOID_BACK_SEC       OPPONENT_AVOID_BACK_SEC
#define AVOID_BACK_RELV      OPPONENT_AVOID_BACK_RELV
#define AVOID_PLAYER_PAD    OPPONENT_AVOID_PLAYER_PAD
// Traffic (TASKS.md #316). HasBeenAccomplished (0x800127B4) ends an action
// against a traffic car only once no drive target hits and the car is
// AVOID_TRAFFIC_PAST down the line behind; against any other car, once none
// hits. CalculatePointsToDrive's hold and push (0x80012BC0) skip a traffic
// car (IsTrafficCar), so a racer with no offset clear of one asks no speed.
#define AVOID_TRAFFIC_PAST  OPPONENT_AVOID_TRAFFIC_PAST
// Build keeps at most 16 cars, the racer itself among them (0x8022F998).
// PORT: here the field and the player's car come first, then the traffic,
// and the cap counts the cars DoWeCare keeps.
#define AVOID_LIST_MAX       OPPONENT_AVOID_LIST_MAX
// AIGoalWinDragRace's lanes (TASKS.md #268 D step 6). DrivingChoice::
// SetChoice(lane) (0x8001CD44) moves the drive target across at this many
// metres a metre (0x803172C4), whichever of the goal's actions chose it.
#define DRAG_LANE_RATE       0.151f
// PORT: CollisionPredictor::WhoWillThisDriveTargetHit sweeps each car's box
// down its drive target, which is FuturePoints the port has not got. Here a
// drive target is the car's path down the line -- `curL` walking to `goalL`
// -- at its speed now, sampled AVOID_SAMPLES times over the horizon, and the
// box is the car's footprint along and across the line. A drive target runs
// up the road however slowly the car is going, so the racer's own is run at
// AVOID_PLAN_SPEED at the least: from a standing start every path is
// otherwise clear, and the whole grid made for the line at once. The port's
// driver holds its line less tightly than PsychoDriver holds FuturePoints --
// a metre either way through a change of offset -- so two paths must clear
// by AVOID_SIDE_PAD as well. Two cars already that close are a hit only
// where a path brings them closer still -- otherwise no choice would ever be
// clear of a car alongside, and neither would move away from the other.
// NearbyAvoidableList::Build is not decoded; a car is nearby within
// AVOID_NEAR_M up or down the line.
#define AVOID_SAMPLES        OPPONENT_AVOID_SAMPLES
#define AVOID_PLAN_SPEED     OPPONENT_AVOID_PLAN_SPEED
#define AVOID_SIDE_PAD       OPPONENT_AVOID_SIDE_PAD
#define AVOID_NEAR_M         OPPONENT_AVOID_NEAR_M
// With no offset clear, AvoidAvoidable's CalculatePointsToDrive (0x80012BC0 ..
// 0x80012E68) never slows a racer for a race car it cannot get round. With
// the time to the hit the action began with (+0x208) within AVOID_PACE_EPS of
// none, it holds if the car is ahead; otherwise it holds if the car will be
// over AVOID_PACE_GAP ahead by then -- relD + relV t + a t^2 / 2, relV the
// other's speed down the line less its own (NearbyAvoidable +0x04: Build
// takes the racer's line-frame velocity from the other's) and a its own
// GetForwardAcceleration -- and otherwise it pushes on. Holding asks for its
// own speed (Car +0x440) and AVOID_PACE_CLOSE of any closing speed, never
// under AVOID_PACE_MIN, which SetDesiredSpeedAsRacer (0x8000BA20) takes only
// where it is under the line's speed. Pushing asks for twice
// RaceCarController::GetMaxSpeed (1000 m/s), no limit, and MaybeUseNitrous.
// An online race's flags (0x8040CA84, 0x8040CA98) skip the hold. The seconds
// the speed is asked over (+0x1DC) go to PsychoDriver +0x214 (SetDesiredSpeed,
// 0x8001F5F0), which Drive counts down and only the nitrous calls read
// (GetDesiredTimeToSpeed). The pushing arm's MaybeUseNitrous is
// updateNitrous()'s (#281, AI nitrous below); +0x1E0 is not decoded.
#define AVOID_PACE_EPS       OPPONENT_AVOID_PACE_EPS
#define AVOID_PACE_GAP       OPPONENT_AVOID_PACE_GAP
#define AVOID_PACE_CLOSE     OPPONENT_AVOID_PACE_CLOSE
// HasBeenAccomplished (0x800127B4) does not end the action, its car still
// nearby, before it is AVOID_DONE_PAD_SEC older than the time to the hit
// it began with (+0x218, set by the ctor 0x800125B0; the age is +0x214's).
#define AVOID_DONE_PAD_SEC   OPPONENT_AVOID_DONE_PAD_SEC
#define AVOID_PACE_MIN       OPPONENT_AVOID_PACE_MIN
// AIActionIJustGotHit (0x800118D0): ChooseNewActionAsRacer (0x8000BF28) puts
// it ahead of AvoidAvoidable -- score 400 to 300 -- for a racer whose body
// touched a race car in the last step, outside the grace period
// (CalculateScore 0x800119B0, GetMostImportantNearbyCollidingAvoidable
// 0x8000E61C), and it is accomplished the first think it touches none
// (HasBeenAccomplished 0x80011A44; its lifetime is -1, no expiry). It makes
// for HIT_CLEAR past the side of the car it touched, both cars' half widths
// as well, at HIT_RATE, or no further than its choice already is; with the
// other car behind it by all but HIT_OVERLAP of their half lengths it holds
// its offset at the slowest lateral speed. It slows for nothing but the
// player's car ahead of it, to the player's speed
// (SetDesiredSpeedSlowerThanThisAvoidable 0x8000DDE8), never under HIT_FLOOR
// once the race (pCurrentRace +0x04 on the WorldTimer) is HIT_FLOOR_SEC old.
// A racer whose GetHeadingClassification (headingClass) is 1 with the other
// car ahead brakes as hard as it can instead (SetDesiredSpeedToMaxDeceleration
// 0x8000DF34: its speed and HIT_BRAKE_SEC of CalculateMaxDeceleration). PORT
// (TASKS.md #280): in the grace period as well, as the port's AvoidAvoidable
// is (see driveChoice). Without it the grid's first touches held: 4126F's
// field came off the line door to door into the first bend, and Ryan was
// turned round there and into the wall (6 s).
#define HIT_CLEAR            5.0f    // 0x803167AC
#define HIT_RATE             0.25f   // 0x803167A8
#define HIT_OVERLAP          2.8f    // 0x803167B0
#define HIT_FLOOR            20.0f   // 0x803167B8
#define HIT_FLOOR_SEC        OPPONENT_HIT_FLOOR_SEC
#define HIT_BRAKE_SEC        0.2f    // 0x803167B4
// AIAction::GetHeadingClassification (0x8000DF8C) and IsPointAheadOfCar
// (0x80016388), the figures at 0x803162C4..0x803162F4.
#define HEAD_SLOW            1.0f    // |speed| at or under it: the nose test
#define HEAD_PREDICT         0.1f    // seconds of velocity ahead
#define HEAD_RATE_MIN        0.001f  // m/s across, the least that times an edge
#define HEAD_NEVER           99999.0f
#define HEAD_SOON            1.0f    // seconds to an edge that count as out
#define HEAD_NOSE_M          2.0f    // the nose point, metres ahead
#define HEAD_STEER_COS       0.5f    // the steer point "ahead" (60 degrees)
#define HEAD_ALONG_COS       0.342f  // nose "along the road" (70 degrees)
#define AHEAD_SLOW           1.0f    // 0x80316F24: under it, the nose not the velocity
// GetUnstuck's two road triggers (CalculateScore 0x80010D58): further than
// UNSTUCK_OUT outside the road, or a MellowMover racer at UNSTUCK_HEAD_SPEED or
// under heading out of it (20 for movement mode 4, which the port has no car on).
#define UNSTUCK_OUT          20.0f   // 0x80316684
#define UNSTUCK_HEAD_SPEED   25.0f   // 0x80316688
// How near a road edge an offset may take a car: its own half width across
// the line (NearbyAvoidable +0x08) and LINE_EDGE_MARGIN more, BuildDriveTarget
// handing FillUpDriveTarget's ChangeCurrentL 0x803172F4 (0 for an emergency
// choice, which the port never makes; 5 online).
#define LINE_EDGE_MARGIN     1.0f
// Where a car finds itself on the line (ailine_track, TASKS.md #280): each
// frame within LINE_TRACK_BACK behind and LINE_TRACK_AHEAD beyond its last
// answer; within LINE_FIND_M either side of the race route's answer when it
// is placed, or when the last answer leaves it more than LINE_LOST_M off the
// line and the race route's finds it nearer.
#define LINE_TRACK_BACK      15.0f
#define LINE_TRACK_AHEAD     30.0f
#define LINE_FIND_M          60.0f
#define LINE_LOST_M          20.0f

// AIActionGetUnstuck and the RaceCarController around it. The figures are
// the GameCube image's own, at the addresses given.
#define UNSTUCK_SINCE_RESET  2.0f    // 0x80316670 / 0x803166C4
#define UNSTUCK_COOLDOWN     6.0f    // AllowedToCallGetUnstuckAction, 0x80317BEC
#define UNSTUCK_TOUCH_AGE    0.4f    // 0x80316690 / 0x803166D8 / 0x803166FC
#define UNSTUCK_SLOW         5.0f    // 0x80316698 / 0x803166DC / 0x80316700
#define UNSTUCK_SLOW_EPS     0.001f  // 0x8031669C / 0x80316704
#define UNSTUCK_FACING       0.9f    // HasBeenAccomplished, 0x803166D4
#define UNSTUCK_MIN_LIFE     1.3f    // GetMinLifetimeForGetUnstuck, 0x80317BF0
#define UNSTUCK_LIFETIME     OPPONENT_UNSTUCK_LIFETIME
#define UNSTUCK_DESIRED      OPPONENT_UNSTUCK_DESIRED
// GracePeriodIsActive (0x8022B510): no GetUnstuck for StartOfRaceGracePeriod
// seconds after the start, each later driver id leaving it
// TimeBetweenCarsForGracePeriodExit sooner.
#define UNSTUCK_GRACE        12.0f
#define UNSTUCK_GRACE_STEP   0.5f
// GetGoalFuturePoint (0x80011160) aims three FuturePoints ahead, one per
// TrackRoutePoint. PORT: the port does not walk TrackRoutePoints, so it aims
// that far along the racing line at the Bayview routes' median point spacing
// (25 m, Routes4001F.bin).
#define UNSTUCK_GOAL_M       OPPONENT_UNSTUCK_GOAL_M

// Think's very-stuck test (0x80027EDC) and Car::StartBlinking's length.
#define STUCK_SAMPLE_SEC     OPPONENT_STUCK_SAMPLE_SEC
#define STUCK_AVG_SPEED      OPPONENT_STUCK_AVG_SPEED
#define STUCK_PLAYER_DIST    OPPONENT_STUCK_PLAYER_DIST
#define BLINK_SEC            OPPONENT_BLINK_SEC
// Move (0x8021F3F4) keeps a car blinking while it still overlaps another in
// the last half second of the blink (r13-0x60D8).
#define BLINK_HOLD_SEC       0.5f
// Think's mover timer at +0x73C (opponent.h), in Timer ticks of 4000 a second
// (CountDown, 0x801A6BDC, scales by 0x803313F4): 20000 from GO and from a
// knock off the player's car, 4000 again while a wheel is off the ground.
#define PHYS_HOLD_SEC        5.0f
#define PHYS_REARM_SEC       1.0f
// PORT: what stands in for the route point's z when the field has no
// TrackRoute line. A height is taken when the car is on all four wheels within
// this far of the route point, and is trusted for a reset this far along the
// road from where it was taken.
#define ROAD_Z_LATERAL       3.0f
#define ROAD_Z_REACH         50.0f


// `from` moved `step` toward `to`, and no farther.
static float toward(float from, float to, float step) {
	if (from < to) return from + step < to ? from + step : to;
	return from - step > to ? from - step : to;
}

// The offsets the middle of a car `halfAcross` wide either side may take at
// `dist`, metres right of the line: ChangeCurrentL's GetMinL / GetMaxL
// (0x8001DADC), each LINE_EDGE_MARGIN and the half width in from its edge,
// and never so far in as to shut out the line itself -- an edge that near
// the line, where the wall pass took the line off the road it matched, had
// pushed the field off it toward the other edge, and at 4005F's island
// (1,904 m) into the nose (TASKS.md #280). False where the field has no line.
static bool roadSpan(const OpponentField* f, float dist, float halfAcross,
		float* lo, float* hi) {
	AiLineSample s;
	if (!ailine_at(f->line, dist, &s)) return false;
	*lo = s.left - s.line + LINE_EDGE_MARGIN + halfAcross;
	*hi = s.right - s.line - LINE_EDGE_MARGIN - halfAcross;
	if (*lo > 0.0f) *lo = 0.0f;
	if (*hi < 0.0f) *hi = 0.0f;
	return true;
}

// Half `o`'s width across the line: its last footprint there (linePose), or
// its chassis's before it has one.
static float halfWidth(const Opponent* o) {
	if (o->halfAcross > 0.0f) return o->halfAcross;
	return 0.5f * (o->sim.chassis.width > 0.5f ? o->sim.chassis.width : 1.8f);
}

static float keepOnRoad(float l, float lo, float hi) {
	return l < lo ? lo : (l > hi ? hi : l);
}

// The race's forks (TASKS.md #409). DecideSignpostAsRacer (0x8000A914) once
// a sign reaches the racer's FuturePoints: a fork is decided FORK_DECIDE_M
// before its sign (PORT: how far ahead retail's FuturePoints run is not
// decoded) and forgotten FORK_FORGET_M past it, so a circuit decides it anew
// each lap.
#define FORK_DECIDE_M 60.0f
#define FORK_FORGET_M 50.0f
// How far outside a branch's road a car put back on the road may be and keep
// to the branch (chooseHere).
#define FORK_KEEP_M   2.0f

// The branch `o` is taking at `dist`, false where it is on the line there.
static bool forkRoad(const OpponentField* f, const Opponent* o, float dist, TrackLaneRoad* r) {
	return o->fork >= 0 && trackroute_fork_road(f->forks, o->fork, dist, r);
}

// How far the branch `o` is taking moves its offsets between where the car
// was stepped and `dist`: every offset it holds is carried along the branch.
// PORT (TASKS.md #409): the speed limit on a fork. Retail's racer on a
// branch reads the branch route's limits; the port's profile is the line's,
// so a branch's own bends are planned here: each one's corner speed within
// FORK_SPEED_M ahead, braked back to the car at the profile's braking, as
// ScrubSpeeds does.
#define FORK_SPEED_M  150.0f
static float forkLimit(const OpponentField* f, const Opponent* o, const AiSpeedProfile* prof,
		float limit) {
	if (o->fork < 0 || !prof) return limit;
	const float step = 2.0f;   // metres a sample
	const float brake = aispeed_braking(prof);
	for (float s = 0.0f; s <= FORK_SPEED_M; s += step) {
		float k;
		if (!trackroute_fork_curv(f->forks, o->fork, o->lineDist + s, &k)) {
			if (s > 0.0f) break;   // past its end
			continue;
		}
		const float v = aispeed_corner(prof, k);
		const float cap = sqrtf(v * v + 2.0f * brake * s);
		if (cap < limit) limit = cap;
	}
	return limit;
}

static float forkShift(const OpponentField* f, const Opponent* o, float dist) {
	TrackLaneRoad r;
	if (o->fork < 0) return 0.0f;
	return (forkRoad(f, o, dist, &r) ? r.lat : 0.0f) - o->forkLat;
}

// The edges `o`'s offsets keep between at `dist`: in a drag, the road its
// lane is on there -- where a race splits, each pair of lanes has a road of
// its own beside the line's (trackroute.h, TASKS.md #268 D) -- and otherwise,
// or with no lane there, the line's (roadSpan).
static bool ownRoadSpan(const OpponentField* f, const Opponent* o, float dist, float* lo,
		float* hi) {
	TrackLaneRoad r;
	const float half = halfWidth(o);
	// On a fork, the branch's road (TASKS.md #409).
	if (!forkRoad(f, o, dist, &r) &&
			(o->lane < 0 || !trackroute_lane_road(f->lanes.lanes, o->lane, dist, &r)))
		return roadSpan(f, dist, half, lo, hi);
	*lo = r.lo + LINE_EDGE_MARGIN + half;
	*hi = r.hi - LINE_EDGE_MARGIN - half;
	if (*lo > r.lat) *lo = r.lat;
	if (*hi < r.lat) *hi = r.lat;
	return true;
}

// The road's own edges at `dist`, metres right of the line, for what retail
// measures off the car's route point (GetUnstuck, the heading class): in a
// drag the road `o`'s lane is on -- past 4201F's post the two pairs of lanes
// are on roads up to 24 m apart, and one measured off the line's had a racer
// "20 m off the road" in its own lane, backing out at 50 m/s (#268 D run 4)
// -- otherwise the line's.
static bool roadEdges(const OpponentField* f, const Opponent* o, float dist, float* l, float* r) {
	TrackLaneRoad lr;
	if (forkRoad(f, o, dist, &lr) ||
			(o->lane >= 0 && trackroute_lane_road(f->lanes.lanes, o->lane, dist, &lr))) {
		*l = lr.lo;
		*r = lr.hi;
		return true;
	}
	AiLineSample s;
	if (!ailine_at(f->line, dist, &s)) return false;
	*l = s.left - s.line;
	*r = s.right - s.line;
	return true;
}

// The offset `o`'s choice holds at `dist`: in a drag, its traffic lane's
// lateral there (TASKS.md #268 D step 6); otherwise, or where the lane is not,
// `goalL`, carried along the branch on a fork (TASKS.md #409).
static float choiceL(const OpponentField* f, const Opponent* o, float dist) {
	float at;
	if (o->lane >= 0 && trackroute_lane_at(f->lanes.lanes, o->lane, dist, &at)) return at;
	return o->goalL + forkShift(f, o, dist);
}

// DrivingChoice::SetChoice(lane): a drag racer's choice is `lane`.
static void setLane(const OpponentField* f, Opponent* o, int lane) {
	o->lane = lane;
	o->latRate = DRAG_LANE_RATE;
	o->choiceSec = 0.0f;
	o->goalL = choiceL(f, o, o->lineDist);
}

// `gap` along the route, the short way round a lap.
static float wrapGap(const RaceRoute* route, float gap) {
	if (race_route_loops(route)) {
		const float len = race_route_length(route);
		if (gap < -0.5f * len) gap += len;
		else if (gap > 0.5f * len) gap -= len;
	}
	return gap;
}

// Finds a car on the line from where it was last frame, and returns how far
// right of the line it is. Without a line, both are the race route's.
static float trackLine(const OpponentField* f, const RaceCoord* c, float* lineDist,
		const float xy[2]) {
	if (!f->line) {
		*lineDist = c->dist;
		const float dx = xy[0] - c->onPath[0], dy = xy[1] - c->onPath[1];
		return dx * c->dir[1] - dy * c->dir[0];
	}
	float lat;
	*lineDist = ailine_track(f->line, *lineDist, LINE_TRACK_BACK, LINE_TRACK_AHEAD,
		xy, &lat, NULL);
	if (fabsf(lat) > LINE_LOST_M) {
		float again;
		const float d = ailine_track(f->line, c->dist, LINE_FIND_M, LINE_FIND_M,
			xy, &again, NULL);
		if (fabsf(again) < fabsf(lat)) {
			*lineDist = d;
			lat = again;
		}
	}
	return lat;
}

// How `s` sits on the line at `dist`: its speed down the line, and the half
// extents of its body's footprint along and across the line -- a car spun
// across the road blocks its length of it.
static void linePose(const OpponentField* f, const RaceCoord* c, float dist,
		const CarSim* s, float* alongV, float* halfAlong, float* halfAcross) {
	float p[2], dir[2] = { c->dir[0], c->dir[1] };
	if (f->line) ailine_pose(f->line, dist, 0.0f, p, dir);
	*alongV = s->vel[0] * dir[0] + s->vel[1] * dir[1];
	const float len = s->chassis.length > 1.0f ? s->chassis.length : 4.5f;
	const float wid = s->chassis.width > 0.5f ? s->chassis.width : 1.8f;
	const float fx = s->rot[0][0], fy = s->rot[0][1];
	const float fl = sqrtf(fx * fx + fy * fy);
	float ca = 1.0f, sa = 0.0f;
	if (fl > 1e-4f) {
		ca = fabsf(fx * dir[0] + fy * dir[1]) / fl;
		sa = fabsf(fx * dir[1] - fy * dir[0]) / fl;
	}
	*halfAlong = 0.5f * (ca * len + sa * wid);
	*halfAcross = 0.5f * (sa * len + ca * wid);
}

// PORT (TASKS.md #280): how far a car is short of a sprint line's first
// sample, down that sample's heading. Retail's TrackRoute runs under the grid,
// so its drive target starts where the car is; the port's line starts at the
// race route's, and 4123F's and 4127F's grids are 35 - 80 m short of it. Aimed
// the full look-ahead past that start, a car on the grid's outside lane ran at
// it alongside a wall that closes on the start: 4127F's Bryce scraped it for
// three seconds before the line began. The metres to the start count toward
// the look-ahead instead, as they would down a line that reached the grid.
static float shortOfStart(const OpponentField* f, const RaceRoute* route,
		const Opponent* o) {
	float p[2], dir[2];
	if (!f->line || race_route_loops(route) || o->lineDist > 0.0f ||
			!ailine_pose(f->line, 0.0f, 0.0f, p, dir))
		return 0.0f;
	const float back = -((o->sim.pos[0] - p[0]) * dir[0] + (o->sim.pos[1] - p[1]) * dir[1]);
	return back > 0.0f ? back : 0.0f;
}

// A new DrivingChoice where the car is: that offset, kept off the edges. In
// a drag, ChooseNearestTrafficLane: the lane nearest it, the drive target
// starting where the car is.
static void chooseHere(const OpponentField* f, Opponent* o, float lat) {
	float lo, hi;
	// A car put back on the line, or backed out off the branch, has left its
	// fork (TASKS.md #409).
	TrackLaneRoad fr;
	if (o->fork >= 0 && !(forkRoad(f, o, o->lineDist, &fr) &&
			lat >= fr.lo - FORK_KEEP_M && lat <= fr.hi + FORK_KEEP_M))
		o->fork = -1;
	o->forkLat = forkRoad(f, o, o->lineDist, &fr) ? fr.lat : 0.0f;
	// The lane first: its road is the one the offset keeps to.
	o->lane = draglane_nearest(&f->lanes, o->lineDist, lat, o->sim.pos[2], DRAGLANE_NONE,
		DRAGLANE_NONE);
	o->curL = o->goalL = ownRoadSpan(f, o, o->lineDist, &lo, &hi) ?
		keepOnRoad(lat, lo, hi) : lat;
	o->latRate = AVOID_RATE_MIN;
	o->choiceSec = AVOID_HOLD_SEC;
	if (o->lane >= 0) {
		setLane(f, o, o->lane);
		o->choiceSec = AVOID_HOLD_SEC;
	}
}

// Car::SetMovementMode between the PhysicsMover (mode 1) and the AI's default,
// the MellowMover (mode 3, GetDefaultMovementMode 0x8021D7C8). The mellow
// tyres' friction is the car's own measure (opponent.h's `perf`).
static void setMover(const OpponentField* f, Opponent* o, bool mellow) {
	const float mu = f->mellowMu > 0.0f ? f->mellowMu : carsim_mellow_mu(false, 0u);
	carsim_set_mellow(&o->sim, mellow, o->perf, mu);
}

void opponent_player_contact(OpponentField* f, int i) {
	if (!f || i < 0 || i >= f->count) return;
	Opponent* o = &f->car[i];
	if (!o->simReady || !o->driving) return;
	o->hitBy |= OPPONENT_HIT_PLAYER;
	o->physSec = PHYS_HOLD_SEC;
	setMover(f, o, false);
}

void opponent_car_contact(OpponentField* f, int a, int b) {
	if (!f || a < 0 || b < 0 || a >= f->count || b >= f->count || a == b) return;
	f->car[a].hitBy |= 1u << b;
	f->car[b].hitBy |= 1u << a;
}

// ---- The race cars' Collisions (opponent.h's OpponentColl). Each contact
// impulse between two cars goes to the one Collision their bodies share
// (Collision::AddImpulse, 0x801AB990), so both cars' lists take its size.
// The field opponent_update last ran, and its player's car.
static OpponentField* s_collField;
static const CarSim* s_collPlayer;

static void addColl(OpponentColl* list, int who, float mag) {
	OpponentColl* spare = NULL;
	for (int k = 0; k < OPPONENT_COLL_MAX; k++) {
		OpponentColl* c = &list[k];
		if (c->used && c->who == who) {
			c->mag += mag;
			c->age = 0.0f;
			return;
		}
		if (!c->used && !spare) spare = c;
	}
	if (!spare) return;
	spare->used = true;
	spare->who = (s8)who;
	spare->mag = mag;
	spare->age = 0.0f;
}

// Collision::Update: done OPPONENT_COLL_LIFE past its last impulse.
static void ageColl(OpponentColl* list, float dt) {
	for (int k = 0; k < OPPONENT_COLL_MAX; k++) {
		OpponentColl* c = &list[k];
		if (c->used && (c->age += dt) > OPPONENT_COLL_LIFE) c->used = false;
	}
}

// The opponent driving `s`, -1 for none.
static int opponentOf(const OpponentField* f, const CarSim* s) {
	for (int k = 0; k < f->count; k++)
		if (&f->car[k].sim == s) return k;
	return -1;
}

// carsim's impulse hook. Only two race cars' collisions are kept: the one
// reader, IJustGotHitDragRace, lists race cars only.
static void onCollImpulse(const CarSim* car, const CarSim* other, CarSimProp* prop, bool ground,
		const float pos[3], const float imp[3], float slip) {
	(void)prop;
	(void)ground;
	(void)pos;
	(void)slip;
	OpponentField* f = s_collField;
	if (!f || !car || !other) return;
	const int a = opponentOf(f, car), b = opponentOf(f, other);
	const bool aPlayer = car == s_collPlayer, bPlayer = other == s_collPlayer;
	if ((a < 0 && !aPlayer) || (b < 0 && !bPlayer)) return;
	const float mag = sqrtf(imp[0] * imp[0] + imp[1] * imp[1] + imp[2] * imp[2]);
	addColl(a >= 0 ? f->car[a].coll : f->playerColl, b >= 0 ? b : OPPONENT_COLL_PLAYER, mag);
	addColl(b >= 0 ? f->car[b].coll : f->playerColl, a >= 0 ? a : OPPONENT_COLL_PLAYER, mag);
}

void opponent_start_driving(OpponentField* f, const CarDb* db,
		const RaceRoute* route, void (*log)(const char* msg)) {
	if (f) f->log = log;
	if (!f) return;
	if (!route) {
		// PORT (#488): a race with no route (the LAN Outrun, race.c's
		// raceLive()) has only LAN racers' cars, which their own consoles
		// drive (opponent_update()'s remote arm); no AI needs a road.
		f->raceSec = 0.0f;
		f->playerTracked = false;
		for (int i = 0; i < f->count; i++) {
			Opponent* o = &f->car[i];
			o->driving = o->simReady && o->remote;
			if (!o->driving) continue;
			o->blinkSec = 0.0f;
			o->finished = false;
			setMover(f, o, false);
		}
		return;
	}
	char b[224];
	f->raceSec = 0.0f;
	f->playerTracked = false;

	// Classify first: a profile's top speed is its class's quickest car's, and
	// it corners on its class's least grippy tyres (aidriver.h).
	float classTop[4] = { 0 }, classGrip[4] = { 0 };
	for (int i = 0; i < f->count; i++) {
		Opponent* o = &f->car[i];
		o->driving = false;
		o->prof = NULL;
		if (!o->simReady) continue;

		float lo = db ? db->measureLo : 0.0f;
		float hi = db ? db->measureHi : 10.0f;
		if (!o->classSet)
			o->carClass = aidriver_car_class(o->perf, lo, hi);
		o->topSpeed = o->sim.topSpeed > 5.0f ? o->sim.topSpeed : 90.0f;
		if (o->topSpeed > classTop[o->carClass]) classTop[o->carClass] = o->topSpeed;
		// TASKS.md #398: the cap is the tyres the car races on. On the
		// MellowMover that is its friction cap (4 g, 3.2 on a URL track, 2 in
		// Street X), which leaves retail's class friction (2.8-3.7 g) as it
		// is; only a field held on the PhysicsMover (drag, street drift)
		// corners on carsim's own tyres.
		const float g = f->physicsMover ? carsim_tyre_grip(&o->sim)
			: (f->mellowMu > 0.0f ? f->mellowMu : carsim_mellow_mu(false, 0u));
		if (classGrip[o->carClass] <= 0.0f || g < classGrip[o->carClass])
			classGrip[o->carClass] = g;
	}
	// Then one profile a car: retail scrubs each car's FuturePoints with that
	// car's own RidePhysicsInfo acceleration table (ScrubSpeeds, 0x80036CD8).
	for (int i = 0; i < OPPONENT_MAX; i++) {
		aispeed_free(f->prof[i]);
		f->prof[i] = NULL;
		if (i >= f->count || !f->car[i].simReady) continue;
		const Opponent* o = &f->car[i];
		float accel[CARSIM_ACCEL_N];
		const float top = carsim_accel_table(&o->sim, accel);
		f->prof[i] = aispeed_build(route, f->line, o->carClass, classTop[o->carClass],
			top > 0.0f ? accel : NULL, top, classGrip[o->carClass], log);
	}

	for (int i = 0; i < f->count; i++) {
		Opponent* o = &f->car[i];
		if (!o->simReady) continue;
		int cls = o->carClass;
		aidriver_init(&o->drv, cls);
		aidrift_init(&o->drift);
		if (o->rideTop <= 0.0f) {
			o->rideClass = o->carClass;
			o->rideTop = o->topSpeed;
		}
		o->drv.suv = isSuv(o->carType);
		o->drv.shortTrack = f->shortTrack;
		o->prof = f->prof[i];
		// AIGoal::SetDesiredSpeedAsRacer's own multiplier (0x8000BA14): the
		// shipped DifficultyInfo column for this driver's skill, which the
		// career difficulty has already chosen by picking the skill byte --
		// retail does not scale it a second time for the setting.
		// Street X takes its own column (+0x540) instead (TASKS.md #255).
		o->skillScale = difficulty_field(o->skill,
			f->shortTrack ? DIFF_STREETX_SPEED_SCALE : DIFF_RACER_SPEED_SCALE);
		o->lookahead = 0.0f;
		o->lookaheadSec = 0.0f;
		// The catch-up boost: 1 until the first DecideBoostFactor, which runs
		// at once (retail's stamp starts at 0, ages ago).
		o->boost = 1.0f;
		o->boostSec = 1e6f;
		o->sim.catchup = 1.0f;
		// The nitrous stamps as a new RaceCarController has them (none), and
		// the route sampled on the first racing frame.
		o->nosOnAt = o->nosOffAt = o->nosMonoAt = 0.0f;
		o->nosAction = o->nosMono = o->nosOn = false;
		o->nosWhy = OPP_NOS_STOP_NONE;
		o->nosLine = o->nosRouteSec = 0.0f;
		o->nosFillAt = o->nosFillPct = 0.0f;
		// One state per driver, drawn off the shared generator so a field is
		// reproducible but no two cars jitter in step.
		o->rng = f->rngSeed ^ (0x9E3779B9u * (u32)(i + 1));
		o->fork = -1;
		o->forkLat = 0.0f;
		o->forkSeen = 0;

		float xy[2] = { o->sim.pos[0], o->sim.pos[1] };
		race_coord_init(route, &o->coord, xy);
		float lat = 0.0f;
		o->lineDist = f->line ? ailine_track(f->line, o->coord.dist, LINE_FIND_M,
			LINE_FIND_M, xy, &lat, NULL) : o->coord.dist;
		if (!f->line) lat = trackLine(f, &o->coord, &o->lineDist, xy);
		// PORT: what DrivingChoice a racer leaves the grid with is not
		// decoded. Each starts on the offset its grid slot put it at (chosen
		// below, once the whole grid is placed), and TryAndDriveBestLine takes
		// it onto the line once the way is clear.
		o->lineLat = lat;
		linePose(f, &o->coord, o->lineDist, &o->sim, &o->alongV, &o->halfAlong,
			&o->halfAcross);
		o->desired = 0.0f;
		o->driving = true;

		// The grid placement is the car's last reset, and GetUnstuck has
		// never been called.
		o->unstuck = false;
		o->unstuckSec = 0.0f;
		o->sinceUnstuck = 1e6f;
		o->sinceReset = 0.0f;
		o->progressN = o->progressIdx = 0;
		o->progressSec = 0.0f;
		o->progressAt = o->lineDist;
		o->roadZ = o->sim.pos[2];
		o->roadDist = o->coord.dist;
		o->blinkSec = 0.0f;
		// The grid is on the PhysicsMover: before CanAICarsStartDriving
		// Think holds the timer full and keeps the car in mode 1.
		o->physSec = PHYS_HOLD_SEC;
		o->finished = false;
		setMover(f, o, false);
	}

	// PORT: a grid short of a sprint line's start (shortOfStart) keeps its
	// lanes. Its offsets are measured square off the line's first sample,
	// and where the road bends on the way there they are all out by about
	// the same: 4123F's five came out 8 - 19 m right of a line whose right
	// edge is 8 m out, each was held to that edge, and the whole field
	// funnelled into one lane and bumped down it to the first bend. So those
	// offsets move together, just as far as takes the farthest onto the road;
	// a grid wider than the road is still held to its edges car by car, as
	// is a grid beside the line, whose offsets are its own.
	float overR = 0.0f, overL = 0.0f;
	for (int i = 0; i < f->count; i++) {
		const Opponent* o = &f->car[i];
		float lo, hi;
		if (!o->driving || shortOfStart(f, route, o) <= 0.0f ||
				!roadSpan(f, o->lineDist, halfWidth(o), &lo, &hi))
			continue;
		if (o->lineLat - hi > overR) overR = o->lineLat - hi;
		if (lo - o->lineLat > overL) overL = lo - o->lineLat;
	}
	const float shift = overR > 0.0f && overL <= 0.0f ? -overR
		: (overL > 0.0f && overR <= 0.0f ? overL : 0.0f);
	for (int i = 0; i < f->count; i++) {
		Opponent* o = &f->car[i];
		if (!o->driving) continue;
		chooseHere(f, o, o->lineLat + (shortOfStart(f, route, o) > 0.0f ? shift : 0.0f));
		const int cls = o->carClass;
		if (log) {
			float lo = 0.0f, hi = 0.0f;
			roadSpan(f, o->lineDist, halfWidth(o), &lo, &hi);
			snprintf(b, sizeof(b), "ai: %s slot %d class %d (perf %.1f), skill %d "
				"-> %.0f%% of the limit +/-%.0f m, %+.1f m off the %s (slot %+.1f, "
				"road %+.1f..%+.1f), top %.0f km/h",
				o->name, o->slot, cls, o->perf, (int)o->skill,
				o->skillScale * 100.0f,
				difficulty_field(o->skill, DIFF_LOOKAHEAD_JITTER),
				o->goalL, f->line ? "racing line" : "race route", o->lineLat, lo, hi,
				o->topSpeed * 3.6f);
			log(b);
			if (o->lane >= 0) {
				snprintf(b, sizeof(b), "ai: %s drives traffic lane %d (%+.1f m)", o->name,
					o->lane, o->goalL);
				log(b);
			}
		}
	}
}

// Where `o` drives at `dist`, metres right of the racing line: its drive
// target's offset there -- ChangeCurrentL walking `curL` to the choice's
// `goalL` at `latRate` per metre down the road -- kept off the road's edges.
// Without a line it is that offset from the race route itself.
static float lineOffset(const OpponentField* f, const Opponent* o, float dist) {
	const float ahead = dist - o->lineDist;
	// On a fork the walk is the branch's way (TASKS.md #409).
	const float sh = forkShift(f, o, dist);
	const float l = sh + toward(o->curL, choiceL(f, o, dist) - sh,
		o->latRate * (ahead > 0.0f ? ahead : 0.0f));
	float lo, hi;
	return ownRoadSpan(f, o, dist, &lo, &hi) ? keepOnRoad(l, lo, hi) : l;
}

// The distance `m` metres up the line from the car. PORT (TASKS.md #280):
// retail measures along its own route's points; the port's distances are the
// race route's, which are not the line's metres where the line leaves the
// race route's road (ailine_along), so the metres are the line's own.
static float lineAhead(const OpponentField* f, const Opponent* o, float m) {
	return f->line ? ailine_along(f->line, o->lineDist, m) : o->lineDist + m;
}

// The world point `o` drives through at `dist`: its offset off the line, or
// off the race route when there is none. False past a sprint's end.
static bool drivePoint(const OpponentField* f, const RaceRoute* route,
		const Opponent* o, float dist, float out[2]) {
	if (!f->line)
		return race_route_sample(route, dist, lineOffset(f, o, dist), out, NULL);
	if (!race_route_loops(route) && dist > race_route_length(route)) {
		// PORT (TASKS.md #280): retail's TrackRoute is the whole road
		// network, so its drive target never runs out at a sprint's finish;
		// the port's line ends there. Past it the point runs straight on
		// down the line's last heading, at the plan's offset. Aimed down the
		// car's own heading instead, a route that ends on a bend -- 4101B's
		// last 25 m are a left-hander -- had every car home run straight
		// into the outside wall as the look-ahead passed the line.
		const float end = race_route_length(route) - 1.0f;
		float p[2], dir[2];
		if (!ailine_pose(f->line, end, lineOffset(f, o, end), p, dir)) return false;
		out[0] = p[0] + dir[0] * (dist - end);
		out[1] = p[1] + dir[1] * (dist - end);
		return true;
	}
	return ailine_pose(f->line, dist, lineOffset(f, o, dist), out, NULL);
}

// PORT (TASKS.md #280): how far up the line the steer-to point may be. Retail's
// PidSteerToTable puts it 25 m ahead even at a crawl, and Drive turns the
// wheel by the whole bearing error to it, so the car runs nearly straight at
// the point: through a hairpin that is a chord across the inside, and on
// 4021's right-hander at 690 m the field cut 2 - 3 m inside the line onto the
// traffic island at its apex. Retail's DriveTarget is FuturePoints with its
// own corner smoothing, which the port has not got; this keeps the point no
// farther than where the straight run at it stays within LINE_CHORD_SAG of
// the line -- the full distance on a straight or a sweeper, down to
// LINE_CHORD_MIN in the tightest bends.
#define LINE_CHORD_SAG       1.0f
#define LINE_CHORD_MIN       8.0f
#define LINE_CHORD_STEP      2.0f
#define LINE_CHORD_POINTS    40

static float chordAhead(const OpponentField* f, const RaceRoute* route,
		const Opponent* o, float ahead) {
	float a[2], p[LINE_CHORD_POINTS][2];
	if (!f->line || ahead <= LINE_CHORD_MIN || !drivePoint(f, route, o, o->lineDist, a))
		return ahead;
	int n = 0;
	for (float s = LINE_CHORD_STEP; s < ahead && n < LINE_CHORD_POINTS; s += LINE_CHORD_STEP) {
		if (!drivePoint(f, route, o, lineAhead(f, o, s), p[n])) return ahead;
		n++;
	}
	// The farthest end -- `ahead` itself, then each point back -- whose chord
	// from `a` passes every point before it within the sag.
	for (int e = n; e >= 0; e--) {
		const float s = e == n ? ahead : (float)(e + 1) * LINE_CHORD_STEP;
		if (s < LINE_CHORD_MIN) return LINE_CHORD_MIN;
		float b[2];
		if (e == n) {
			if (!drivePoint(f, route, o, lineAhead(f, o, ahead), b)) return ahead;
		} else {
			b[0] = p[e][0];
			b[1] = p[e][1];
		}
		const float dx = b[0] - a[0], dy = b[1] - a[1];
		const float l = sqrtf(dx * dx + dy * dy);
		if (l < 0.1f) return s;
		bool ok = true;
		for (int i = 0; i < e && ok; i++) {
			const float c = ((p[i][0] - a[0]) * dy - (p[i][1] - a[1]) * dx) / l;
			if (fabsf(c) > LINE_CHORD_SAG) ok = false;
		}
		if (ok) return s;
	}
	return LINE_CHORD_MIN;
}

// GracePeriodIsActive (0x8022B510), driver id 100 + i: no GetUnstuck, and no
// new DrivingChoice to get round a car, for StartOfRaceGracePeriod seconds
// after the start, each later driver id leaving it
// TimeBetweenCarsForGracePeriodExit sooner.
static bool graceActive(const OpponentField* f, int i) {
	return f->raceSec <= UNSTUCK_GRACE - UNSTUCK_GRACE_STEP * (float)i;
}

// Finds the player's car on the line, the way trackLine finds one of the
// field's, and measures it as linePose does.
static void trackPlayer(OpponentField* f, const RaceRoute* route, const CarSim* p) {
	const float xy[2] = { p->pos[0], p->pos[1] };
	if (!f->playerTracked) {
		race_coord_init(route, &f->playerCoord, xy);
		f->playerLineDist = f->line ? ailine_track(f->line, f->playerCoord.dist,
			LINE_FIND_M, LINE_FIND_M, xy, NULL, NULL) : f->playerCoord.dist;
		f->playerTracked = true;
	} else {
		race_coord_update(route, &f->playerCoord, xy);
	}
	f->playerLat = trackLine(f, &f->playerCoord, &f->playerLineDist, xy);
	linePose(f, &f->playerCoord, f->playerLineDist, p, &f->playerAlongV,
		&f->playerHalfAlong, &f->playerHalfAcross);
}

void opponent_set_traffic(OpponentField* f, int k, const CarSim* sim,
		const RaceCoord* coord, bool on) {
	if (!f || k < 0 || k >= OPPONENT_TRAFFIC_MAX) return;
	OpponentTraffic* t = &f->traffic[k];
	t->sim = sim;
	t->coord = coord;
	t->on = on && sim && coord;
	if (!t->on) t->tracked = false;
}

// Finds each traffic car on the line as trackPlayer finds the player's car.
// A teleport moves one round the race in a frame, and it is found afresh.
static void trackTraffic(OpponentField* f, const RaceRoute* route) {
	for (int k = 0; k < f->trafficCount && k < OPPONENT_TRAFFIC_MAX; k++) {
		OpponentTraffic* t = &f->traffic[k];
		if (!t->on) continue;
		const float xy[2] = { t->sim->pos[0], t->sim->pos[1] };
		if (t->tracked &&
				fabsf(wrapGap(route, t->coord->dist - t->coordDist)) > LINE_TRACK_AHEAD)
			t->tracked = false;
		if (!t->tracked) {
			t->lineDist = f->line ? ailine_track(f->line, t->coord->dist, LINE_FIND_M,
				LINE_FIND_M, xy, NULL, NULL) : t->coord->dist;
			t->tracked = true;
		}
		t->coordDist = t->coord->dist;
		t->lat = trackLine(f, t->coord, &t->lineDist, xy);
		linePose(f, t->coord, t->lineDist, t->sim, &t->alongV, &t->halfAlong,
			&t->halfAcross);
	}
}

// The cars round one racer as its CollisionPredictor sees them: opponent.h's
// OpponentAvoidCar / OpponentAvoidScene.
typedef OpponentAvoidCar AvoidCar;
typedef OpponentAvoidScene AvoidScene;

static float sampleSec(int k) {
	return (float)(k + 1) * (AVOID_HORIZON / (float)AVOID_SAMPLES);
}

// A car of the NearbyAvoidableList as `o` sees it: `dist` and `lat` where it
// is on the line, `v` its speed down it, and `goal` / `rate` the choice it is
// walking to -- its own offset when it has none. `pad` is DoWeCare's, `id`
// which car it is (AvoidCar.id).
static void addAvoidable(AvoidScene* sc, const OpponentField* f, const RaceRoute* route,
		const Opponent* o, int id, float dist, float lat, float v, float halfAlong,
		float halfAcross, float goal, float rate, float pad, const Opponent* q) {
	const float relD = wrapGap(route, dist - o->lineDist);
	if (fabsf(relD) > AVOID_NEAR_M) return;
	// TryAndDriveBestLine: relD over minus the relative speed is when the two
	// meet, for every nearby car closing on the racer or being closed on.
	const float relV = v - o->alongV;
	if (fabsf(relV) > AVOID_BACK_RELV) {
		const float t = relD / -relV;
		if (t > 0.0f && t < sc->minT) sc->minT = t;
	}
	// DoWeCareAboutThisAvoidable: not once it is wholly behind.
	if (relD <= -(sc->halfAlong + halfAlong + pad)) return;
	if (sc->n >= AVOID_LIST_MAX) return;
	AvoidCar* c = &sc->car[sc->n++];
	c->id = id;
	c->relD = relD;
	c->relL = lat - o->lineLat;
	c->speed = v;
	c->halfAlong = halfAlong;
	c->halfAcross = halfAcross;
	// From where the two paths start: the racer's at its drive target's
	// offset, the other's where it is. Measured from where the racer is, a
	// car whose drive target was 4 cm nearer the car alongside than the car
	// itself saw that car's path come 4 cm closer -- a hit -- and waited for
	// it to pull away: 4002F's grid, a row square to a race route the line
	// leaves at 20 degrees, each car the one to its right's follower, and the
	// front row's third car still standing 2 s after the start (TASKS.md #280).
	c->sep = fabsf(lat - o->curL);
	c->close = fabsf(relD) < sc->halfAlong + halfAlong &&
		c->sep < sc->halfAcross + halfAcross + AVOID_SIDE_PAD;
	// Its path keeps off the road's edges as the racer's own does, so two
	// offsets a narrowing road squeezes onto one edge meet there.
	const float fwd = v > 0.0f ? v : 0.0f;
	for (int k = 0; k < AVOID_SAMPLES; k++) {
		const float run = fwd * sampleSec(k);
		float lo, hi;
		c->d[k] = relD + run;
		c->l[k] = toward(lat, goal, rate * run);
		// A racer on a fork goes the branch's way, on the branch's road
		// (TASKS.md #409).
		if (q && q->fork >= 0) {
			c->l[k] += forkShift(f, q, dist + run);
			if (ownRoadSpan(f, q, dist + run, &lo, &hi)) c->l[k] = keepOnRoad(c->l[k], lo, hi);
		} else if (rate > 0.0f && !f->lanes.lanes && roadSpan(f, dist + run, halfAcross, &lo, &hi))
			c->l[k] = keepOnRoad(c->l[k], lo, hi);
	}
}

static void buildScene(AvoidScene* sc, const OpponentField* f, const RaceRoute* route,
		const Opponent* o, const CarSim* player) {
	sc->n = 0;
	sc->minT = 1e9f;
	const CarSim* s = &o->sim;
	sc->halfAlong = 0.5f * (s->chassis.length > 1.0f ? s->chassis.length : 4.5f);
	sc->halfAcross = 0.5f * (s->chassis.width > 0.5f ? s->chassis.width : 1.8f);
	const float fwd = o->alongV > AVOID_PLAN_SPEED ? o->alongV : AVOID_PLAN_SPEED;
	sc->road = f->line != NULL;
	for (int k = 0; k < AVOID_SAMPLES; k++) {
		sc->d[k] = fwd * sampleSec(k);
		sc->sh[k] = forkShift(f, o, o->lineDist + sc->d[k]);
		if (sc->road && !ownRoadSpan(f, o, o->lineDist + sc->d[k], &sc->lo[k], &sc->hi[k]))
			sc->road = false;
	}
	for (int k = 0; k < f->count; k++) {
		const Opponent* q = &f->car[k];
		// A blinking car is passed through, and passes through.
		if (q == o || !q->driving || !q->simReady || opponent_blinking(q)) continue;
		addAvoidable(sc, f, route, o, k, q->lineDist, q->lineLat, q->alongV, q->halfAlong,
			q->halfAcross, q->unstuck ? q->lineLat : q->goalL, q->latRate, 0.0f, q);
	}
	if (player && f->playerTracked)
		addAvoidable(sc, f, route, o, -1, f->playerLineDist, f->playerLat, f->playerAlongV,
			f->playerHalfAlong, f->playerHalfAcross, f->playerLat, 0.0f, AVOID_PLAYER_PAD, NULL);
	// The traffic, each down the line where it is, as the player's car is.
	for (int k = 0; k < f->trafficCount && k < OPPONENT_TRAFFIC_MAX; k++) {
		const OpponentTraffic* t = &f->traffic[k];
		if (!t->on || !t->tracked) continue;
		addAvoidable(sc, f, route, o, OPPONENT_AVOID_TRAFFIC + k, t->lineDist, t->lat,
			t->alongV, t->halfAlong, t->halfAcross, t->lat, 0.0f, 0.0f, NULL);
	}
}

// HasBeenAccomplished's traffic arm (AVOID_TRAFFIC_PAST): the traffic car the
// racer is avoiding is still in its list -- on the road, within AVOID_NEAR_M
// (PORT, as addAvoidable) -- and not yet far enough behind.
static bool trafficNotPassed(const OpponentField* f, const RaceRoute* route,
		const Opponent* o) {
	const int k = o->avoidWho - OPPONENT_AVOID_TRAFFIC;
	if (!o->avoiding || k < 0 || k >= f->trafficCount || k >= OPPONENT_TRAFFIC_MAX)
		return false;
	const OpponentTraffic* t = &f->traffic[k];
	if (!t->on || !t->tracked) return false;
	const float relD = wrapGap(route, t->lineDist - o->lineDist);
	return relD >= AVOID_TRAFFIC_PAST && relD <= AVOID_NEAR_M;
}

// WhoWillThisDriveTargetHit (PORT, see AVOID_SAMPLES): the first sample time
// at which the drive target from `from` walking to `goal` at `rate` meets one
// of the scene's cars, and which (`who`); past the horizon when none does.
// IsDriveable is this past AVOID_HORIZON.
static float hitTime(const AvoidScene* sc, float from, float goal, float rate, int* who) {
	for (int k = 0; k < AVOID_SAMPLES; k++) {
		float l = sc->sh[k] + toward(from, goal, rate * sc->d[k]);
		if (sc->road) l = keepOnRoad(l, sc->lo[k], sc->hi[k]);
		for (int i = 0; i < sc->n; i++) {
			const AvoidCar* c = &sc->car[i];
			float clear = sc->halfAcross + c->halfAcross + AVOID_SIDE_PAD;
			if (c->close && c->sep < clear) clear = c->sep;
			if (fabsf(c->d[k] - sc->d[k]) < sc->halfAlong + c->halfAlong &&
					fabsf(c->l[k] - l) < clear) {
				if (who) *who = i;
				return sampleSec(k);
			}
		}
	}
	return 1e9f;
}

float opponent_avoid_sample_sec(int k) {
	return sampleSec(k);
}

float opponent_avoid_hit_time(const OpponentAvoidScene* sc, float from, float goal, float rate,
		int* who) {
	return hitTime(sc, from, goal, rate, who);
}

// ChooseOffsetFromDrivingLine (0x8001E09C), past its IsDriveable of the
// current choice: nothing new within AVOID_HOLD_SEC of the last choice;
// otherwise the road's width in AVOID_STEPS steps, and for each lateral speed
// from the slowest up, each offset a step farther from the current goal --
// the side `preferLeft` names first -- until one's drive target is clear for
// `horizon` seconds, MakeNewChoice's own (AvoidAvoidable's AVOID_HORIZON,
// StopDriving's STOP_CHOICE_SEC). A racer does not drive round a race car any
// other way, so false is no offset.
bool opponent_avoid_offset(const OpponentAvoidScene* sc, float minL, float maxL, float curL,
		float* goalL, float* latRate, float* choiceSec, bool preferLeft, float horizon) {
	if (*choiceSec < AVOID_HOLD_SEC) return true;
	float step = (maxL - minL) / AVOID_STEPS;
	if (step < AVOID_STEP_MIN) step = AVOID_STEP_MIN;
	for (float rate = AVOID_RATE_MIN; rate <= AVOID_RATE_MAX; rate += AVOID_RATE_STEP) {
		for (float delta = step; ; delta += step) {
			const float left = *goalL - delta, right = *goalL + delta;
			const bool leftIn = left > minL, rightIn = right < maxL;
			const float a = preferLeft ? left : right, b = preferLeft ? right : left;
			const bool aIn = preferLeft ? leftIn : rightIn, bIn = preferLeft ? rightIn : leftIn;
			float pick = 0.0f;
			bool found = false;
			if (aIn && hitTime(sc, curL, a, rate, NULL) > horizon) {
				pick = a;
				found = true;
			} else if (bIn && hitTime(sc, curL, b, rate, NULL) > horizon) {
				pick = b;
				found = true;
			}
			if (found) {
				*goalL = pick;
				*latRate = rate;
				*choiceSec = 0.0f;
				return true;
			}
			if (!leftIn && !rightIn) break;
		}
	}
	return false;
}

// The above for racer `o`: FuturePoint::GetMinL / GetMaxL, the road either
// side of the line. PORT: without a line, five metres either side of the
// race route. On a fork, the branch's road (TASKS.md #409).
static bool chooseOffset(const OpponentField* f, Opponent* o, const AvoidScene* sc,
		bool preferLeft, float horizon) {
	AiLineSample s;
	TrackLaneRoad fr;
	float minL = -5.0f, maxL = 5.0f;
	if (forkRoad(f, o, o->lineDist, &fr)) {
		minL = fr.lo;
		maxL = fr.hi;
	} else if (ailine_at(f->line, o->lineDist, &s)) {
		minL = s.left - s.line;
		maxL = s.right - s.line;
	}
	return opponent_avoid_offset(sc, minL, maxL, o->curL, &o->goalL, &o->latRate, &o->choiceSec,
		preferLeft, horizon);
}

// AvoidAvoidable's speed when no offset or lane round `c` is clear
// (AVOID_PACE_*), and none asked round traffic (AVOID_TRAFFIC_PAST); a figure
// past any speed when it asks for none.
bool opponent_accident_step(Opponent* o, float dt) {
	const CarSim* s = &o->sim;
	const bool upright = s->rot[2][2] > OPPONENT_ACCIDENT_UPRIGHT;
	if (o->accident) {
		o->accidentSec += dt;
		if (o->accidentAsked || (s->wheelsDown == CARPHYS_WHEELS && upright))
			o->accident = false;
	} else if (s->wheelsDown < 2 && !upright) {
		o->accident = true;
		o->accidentSec = 0.0f;
		o->accidentAsked = false;
	}
	if (!o->accident) return false;
	if (o->accidentSec >= OPPONENT_ACCIDENT_RESET_SEC && !o->accidentAsked) {
		o->accidentAsked = true;
		o->accidentReset = true;
	}
	return true;
}

float opponent_avoid_pace(float avoidT, float alongV, float fwdAccel, float speed,
		const OpponentAvoidCar* c) {
	if (c->id >= OPPONENT_AVOID_TRAFFIC) return 1e9f;
	const float t = avoidT;
	const float relV = c->speed - alongV;
	const bool hold = fabsf(t) <= AVOID_PACE_EPS ? c->relD > 0.0f :
		c->relD + relV * t + 0.5f * fwdAccel * t * t > AVOID_PACE_GAP;
	if (!hold) return 1e9f;
	const float v = speed - AVOID_PACE_CLOSE * (relV < 0.0f ? relV : 0.0f);
	return v > AVOID_PACE_MIN ? v : AVOID_PACE_MIN;
}

static float avoidPace(const Opponent* o, const AvoidCar* c) {
	return opponent_avoid_pace(o->avoidT, o->alongV, o->fwdAccel, o->sim.speed, c);
}

// HasBeenAccomplished's GetNearbyAvoidable: the car the racer is avoiding
// still within AVOID_NEAR_M up or down the line (PORT, as addAvoidable).
static bool avoideeNear(const OpponentField* f, const RaceRoute* route, const Opponent* o,
		const CarSim* player) {
	float dist;
	const int k = o->avoidWho;
	if (k >= OPPONENT_AVOID_TRAFFIC) {
		const int t = k - OPPONENT_AVOID_TRAFFIC;
		if (t >= f->trafficCount || t >= OPPONENT_TRAFFIC_MAX) return false;
		if (!f->traffic[t].on || !f->traffic[t].tracked) return false;
		dist = f->traffic[t].lineDist;
	} else if (k < 0) {
		if (!player || !f->playerTracked) return false;
		dist = f->playerLineDist;
	} else {
		if (k >= f->count) return false;
		const Opponent* q = &f->car[k];
		if (!q->driving || !q->simReady || opponent_blinking(q)) return false;
		dist = q->lineDist;
	}
	return fabsf(wrapGap(route, dist - o->lineDist)) <= AVOID_NEAR_M;
}

// The racer's action for this frame between AIActionJustDrive and
// AIActionAvoidAvoidable, as far as its DrivingChoice goes. Returns the speed
// AvoidAvoidable holds it to when no offset is clear (AVOID_PACE_*), or a
// figure past any speed when it asks for none. AIActionIJustGotHit (gotHit)
// goes before it.
static float driveChoice(const OpponentField* f, const RaceRoute* route, int i,
		Opponent* o, const CarSim* player) {
	AvoidScene sc;
	buildScene(&sc, f, route, o, player);
	const bool grace = graceActive(f, i);
	int who = -1;
	const float hit = hitTime(&sc, o->curL, o->goalL, o->latRate, &who);
	if (hit > AVOID_HORIZON) {
		// Not accomplished yet: it keeps the choice it has.
		if (trafficNotPassed(f, route, o)) return 1e9f;
		if (o->avoiding && o->avoidSec < o->avoidDone && avoideeNear(f, route, o, player))
			return 1e9f;
		o->avoiding = false;
		// JustDrive's TryAndDriveBestLine: line 0 at the slowest lateral speed,
		// once no nearby car is within AVOID_BACK_SEC of meeting this one and
		// the way back is clear. PORT: not in the grace period. Retail lets
		// the whole grid make for the line from a standing start, where no
		// path yet runs into another, and then -- AvoidAvoidable taking no
		// new choice in the grace period -- the field closed up across the
		// road onto the line and raced into the first corners door to door.
		// The port's grid holds its lanes until the grace period is over, and
		// AvoidAvoidable chooses as it would after it, for where a narrowing
		// road squeezes two lanes into one.
		// On a fork, the branch's racing line (TASKS.md #409).
		const float best = o->fork >= 0 ? o->forkLat : 0.0f;
		if (!grace && o->goalL != best && sc.minT > AVOID_BACK_SEC &&
				hitTime(&sc, o->curL, best, AVOID_RATE_MIN, NULL) > AVOID_HORIZON) {
			o->goalL = best;
			o->latRate = AVOID_RATE_MIN;
			o->choiceSec = 0.0f;
		}
		return 1e9f;
	}
	// AvoidAvoidable's CalculatePointsToDrive (PORT: in the grace period as
	// well, see above). MakeNewChoice prefers the side away from the car hit.
	// A new action, for a car other than the last one's, keeps the time to
	// the hit it began with.
	const AvoidCar* c = &sc.car[who];
	if (!o->avoiding) {
		o->avoidSec = 0.0f;
		o->avoidDone = hit + AVOID_DONE_PAD_SEC;
	}
	if (!o->avoiding || o->avoidWho != c->id) {
		o->avoiding = true;
		o->avoidWho = c->id;
		o->avoidT = hit;
	}
	if (chooseOffset(f, o, &sc, c->relL > 0.0f, AVOID_HORIZON)) return 1e9f;
	// PORT: in the grace period as well, which retail's never reaches, as the
	// choice above is.
	return avoidPace(o, c);
}

// The lane a car is in: the one nearest its lateral, at its height.
static int laneOf(const OpponentField* f, float dist, float lat, float z) {
	return draglane_nearest(&f->lanes, dist, lat, z, DRAGLANE_NONE, DRAGLANE_NONE);
}

// CanChangeTrafficLane (0x8001DF24): every lane from the racer's to `to`
// one that CanChangeDrivingLine allows, and the drive target there clear
// (IsDriveable, for `horizon` seconds); then it is the choice.
static bool canChangeTrafficLane(const OpponentField* f, Opponent* o, const AvoidScene* sc,
		int to, float horizon) {
	if (o->lane < 0 || to == o->lane) return false;
	const int step = to > o->lane ? 1 : -1;
	for (int l = o->lane; l != to; l += step)
		if (!draglane_can_change(&f->lanes, o->lineDist, o->lineDist, l, l + step, true, false))
			return false;
	float at;
	if (!trackroute_lane_at(f->lanes.lanes, to, o->lineDist, &at) ||
			hitTime(sc, o->curL, at, DRAG_LANE_RATE, NULL) <= horizon)
		return false;
	setLane(f, o, to);
	return true;
}

// DrivingChoice::ChangeTrafficLane (0x8001DDEC), MakeNewChoice for a drag's
// lane: one lane over, then two, then three, the side `preferLeft` names
// first at each.
static bool changeTrafficLane(const OpponentField* f, Opponent* o, const AvoidScene* sc,
		bool preferLeft, float horizon) {
	for (int k = 1; k < 4; k++) {
		const int a = preferLeft ? o->lane - k : o->lane + k;
		const int b = preferLeft ? o->lane + k : o->lane - k;
		if (canChangeTrafficLane(f, o, sc, a, horizon) ||
				canChangeTrafficLane(f, o, sc, b, horizon))
			return true;
	}
	return false;
}

// ---- AIGoalPark (TASKS.md #268 E): Race::FinishCar (0x80229C88) engages it on
// every finished car in control mode 3 -- an AI racer, and the player's once
// FinishCar has put it in that mode (main.c's finish park) -- with
// AICarController::EngagePark (0x80009424, SetCurrentGoal(0xB)). Its only
// action is AIActionStopDriving (ctor 0x80013884, type 0x10): ChooseNewAction
// (0x8000D7F8) takes its CreateDefaultAction (vfunc +0x84) whatever it is
// asked, so a parked car neither backs out nor dodges, and StopDriving is
// never accomplished, never fails. Its CalculatePointsToDrive (0x800139AC):
// the drive target from the choice the car has, unless WhoWillThisDriveTarget
// Hit sees that meet a car within STOP_HIT_SEC; then MakeNewChoice, the side
// away from that car first, clear for STOP_CHOICE_SEC. Its speeds are all 0
// (0x80316A70), as is the goal's GetSpeedLimit (0x80318CE8): the car brakes to
// a stop. GetDesiredDrivingLineType (0x8000D78C) is the traffic lanes (2) for
// an AI car, and for the player's in a drag; else the racing line (1).
// PORT: off a drag the field has no traffic lanes, so an AI car makes its
// choice off the racing line as AvoidAvoidable does.
#define STOP_HIT_SEC    3.0f    // 0x80316A68
#define STOP_CHOICE_SEC 2.0f    // 0x80316A6C

static void stopChoice(const OpponentField* f, const RaceRoute* route, Opponent* o,
		const CarSim* player) {
	AvoidScene sc;
	buildScene(&sc, f, route, o, player);
	int who = -1;
	if (hitTime(&sc, o->curL, o->goalL, o->latRate, &who) > STOP_HIT_SEC || who < 0) return;
	const bool left = sc.car[who].relL > 0.0f;
	if (o->lane >= 0)
		changeTrafficLane(f, o, &sc, left, STOP_CHOICE_SEC);
	else
		chooseOffset(f, o, &sc, left, STOP_CHOICE_SEC);
}

// Against a wall on any side but the back now: AIActionHitWallDragRace's
// trigger. PORT: retail scores the wall's Collision; here it is a side of
// the body touching a wall (carsim.h's touchWith).
static bool onWall(const CarSim* s) {
	static const int side[3] = { CARSIM_TOUCH_FRONT, CARSIM_TOUCH_LEFT, CARSIM_TOUCH_RIGHT };
	for (int k = 0; k < 3; k++)
		if (carsim_touch_age(s, side[k]) >= 0.0f && s->touchWith[side[k]] == CARSIM_TOUCH_WALL)
			return true;
	return false;
}

// ---- AIActionIJustGotHitDragRace (ctor 0x80012EF8, AIAction type 0xC).
// CalculateScore (0x80012FC4) takes it on, at 400, for a collision with a race
// car whose impulses come to DRAGHIT_SCORE_IMPULSE; HasBeenAccomplished
// (0x80013040) ends it once the car has no collision left (a floor of -1),
// and moves its target to the most important one meanwhile. Its lifetime is
// -1 (AIActionIJustGotHitDragRaceLifetime): it does not expire.
#define DRAGHIT_SCORE_IMPULSE 8.3333f    // 0x80316968
#define DRAGHIT_DONE_IMPULSE  (-1.0f)    // 0x8031696C
#define COLL_NONE (-2)

// GetMostImportantNearbyCollidingAvoidable (0x8000E61C) over `coll`, the
// collisions of the car `dist` down the line (`self` its opponent index, -1
// the player's car): the other car of every collision whose impulses come to
// `floor` or more -- the player's car before any other; between two of a
// kind, one ahead before one behind, else the one less far along. COLL_NONE
// for none.
// PORT: retail also lets the car's controller pass a car over (its vfunc
// +0x114), which is not decoded: none is passed over.
static int mostImportantColliding(const OpponentField* f, const RaceRoute* route,
		const OpponentColl* coll, int self, float dist, const CarSim* player, float floor) {
	int who = COLL_NONE;
	float bestD = 0.0f;
	for (int k = 0; k < OPPONENT_COLL_MAX; k++) {
		const OpponentColl* c = &coll[k];
		if (!c->used || c->mag < floor) continue;
		float d;
		if (c->who == OPPONENT_COLL_PLAYER) {
			if (!player || !f->playerTracked) continue;
			d = wrapGap(route, f->playerLineDist - dist);
		} else {
			if (c->who == self || c->who < 0 || c->who >= f->count) continue;
			const Opponent* q = &f->car[c->who];
			if (!q->driving || !q->simReady) continue;
			d = wrapGap(route, q->lineDist - dist);
		}
		const bool isP = c->who == OPPONENT_COLL_PLAYER, bestP = who == OPPONENT_COLL_PLAYER;
		bool take = who == COLL_NONE || (isP && !bestP);
		if (!take && isP == bestP) take = (bestD < 0.0f && d > 0.0f) || d < bestD;
		if (take) {
			who = c->who;
			bestD = d;
		}
	}
	return who;
}

// HasBeenAccomplished while the action runs, CalculateScore while it does not.
// True while it runs; `*target` is the car it is about.
static bool dragHitRun(const OpponentField* f, const RaceRoute* route, const OpponentColl* coll,
		int self, float dist, const CarSim* player, bool* on, s8* target) {
	if (*on) {
		const int who = mostImportantColliding(f, route, coll, self, dist, player,
			DRAGHIT_DONE_IMPULSE);
		if (who == COLL_NONE) *on = false;
		else *target = (s8)who;
	}
	if (!*on) {
		const int who = mostImportantColliding(f, route, coll, self, dist, player,
			DRAGHIT_SCORE_IMPULSE);
		if (who == COLL_NONE) return false;
		*on = true;
		*target = (s8)who;
	}
	return true;
}

// A car as CalculatePointsToDrive reads it: where it is on the line, its
// height, its NearbyAvoidable half width (+0x08) and whether it is the
// player's. PORT: the half width is its footprint's across the line, as the
// port's avoidables take it.
typedef struct {
	float dist, lat, z, halfAcross;
	bool player;
} HitCar;

static bool hitCar(const OpponentField* f, int who, const CarSim* player, HitCar* h) {
	if (who == OPPONENT_COLL_PLAYER) {
		if (!player || !f->playerTracked) return false;
		h->dist = f->playerLineDist;
		h->lat = f->playerLat;
		h->z = player->pos[2];
		h->halfAcross = f->playerHalfAcross;
		h->player = true;
		return true;
	}
	if (who < 0 || who >= f->count) return false;
	const Opponent* q = &f->car[who];
	if (!q->driving || !q->simReady) return false;
	h->dist = q->lineDist;
	h->lat = q->lineLat;
	h->z = q->sim.pos[2];
	h->halfAcross = q->halfAcross;
	h->player = false;
	return true;
}

// CalculatePointsToDrive (0x8001311C): the lane `me` makes for, hit by `them`,
// -1 to leave the choice as it is (either car in no lane). `drag` is `me`
// under a DragController, for CanChangeDrivingLine.
//   * In different lanes it keeps its own.
//   * In one lane, it moves when either car is off the lane's middle by its
//     half width or more -- unless it is the nearer the middle and the other
//     is not the player's car -- and, both on the middle, when the other is
//     ahead. And it moves whenever the other's way out, the lane on the
//     other's side, cannot be changed to.
//   * Moving is one lane away from the other where CanChangeDrivingLine
//     allows; else it keeps its lane.
static int dragHitLane(const OpponentField* f, const RaceRoute* route, const HitCar* me,
		const HitCar* them, bool drag) {
	const int my = laneOf(f, me->dist, me->lat, me->z);
	const int their = laneOf(f, them->dist, them->lat, them->z);
	if (my < 0 || their < 0) return -1;
	// GetRelativeL: metres right of `me`.
	const float relL = them->lat - me->lat;
	bool move = false;
	if (my == their) {
		float atMe = me->lat, atThem = them->lat;
		trackroute_lane_at(f->lanes.lanes, my, me->dist, &atMe);
		trackroute_lane_at(f->lanes.lanes, my, them->dist, &atThem);
		const float myL = fabsf(me->lat - atMe), theirL = fabsf(them->lat - atThem);
		if (myL >= me->halfAcross || theirL >= them->halfAcross)
			move = !(myL < theirL) || them->player;
		else
			move = wrapGap(route, them->dist - me->dist) >= 0.0f;
		const int side = relL < 0.0f ? my - 1 : my + 1;
		if (!draglane_can_change(&f->lanes, me->dist, me->dist, my, side, !drag, drag))
			move = true;
	}
	if (!move) return my;
	const int away = relL >= 0.0f ? my - 1 : my + 1;
	return draglane_can_change(&f->lanes, me->dist, me->dist, my, away, !drag, drag) ? away : my;
}

// AIGoalWinDragRace (TASKS.md #268 D step 6): a drag racer keeps to a traffic
// lane and leaves it only to get round a car. ChooseNewAction after
// GetUnstuck: IJustGotHitDragRace (score 400), HitWallDragRace (300),
// AvoidAvoidable, and when none of them has anything to do,
// AIActionDriveTrafficLane, which holds the lane the choice has --
// ChooseNearestTrafficLane only when it has none, or the lane is not there.
// The circuits' way back to the racing line (TryAndDriveBestLine) is not in
// it. Returns the speed asked for, as driveChoice does.
static float dragChoice(const OpponentField* f, const RaceRoute* route, int i, Opponent* o,
		const CarSim* player) {
	if (o->lane < 0 || !trackroute_lane_at(f->lanes.lanes, o->lane, o->lineDist, NULL)) {
		const int lane = laneOf(f, o->lineDist, o->lineLat, o->sim.pos[2]);
		if (lane >= 0) setLane(f, o, lane);
	}
	if (o->lane < 0) {
		o->nosAction = true;   // JustDrive's
		return driveChoice(f, route, i, o, player);
	}
	// IJustGotHitDragRace (score 400), while it runs: CalculatePointsToDrive.
	if (dragHitRun(f, route, o->coll, i, o->lineDist, player, &o->dragHit, &o->dragHitWho)) {
		const HitCar me = { o->lineDist, o->lineLat, o->sim.pos[2], o->halfAcross, false };
		HitCar them;
		if (hitCar(f, o->dragHitWho, player, &them)) {
			const int lane = dragHitLane(f, route, &me, &them, false);
			if (lane >= 0) setLane(f, o, lane);
		}
		o->avoiding = false;
		return 1e9f;
	}
	// HitWallDragRace: the nearest lane.
	if (onWall(&o->sim)) {
		const int lane = laneOf(f, o->lineDist, o->lineLat, o->sim.pos[2]);
		if (lane >= 0 && lane != o->lane) setLane(f, o, lane);
		o->avoiding = false;
		return 1e9f;
	}
	// AvoidAvoidable: the lane's drive target hits a car within the horizon.
	// It and DriveTrafficLane both call MaybeUseNitrous.
	o->nosAction = true;
	AvoidScene sc;
	buildScene(&sc, f, route, o, player);
	int who = -1;
	const float hit = hitTime(&sc, o->curL, choiceL(f, o, o->lineDist), o->latRate, &who);
	if (hit > AVOID_HORIZON) {
		o->avoiding = false;
		return 1e9f;
	}
	const AvoidCar* c = &sc.car[who];
	if (!o->avoiding || o->avoidWho != c->id) {
		o->avoiding = true;
		o->avoidWho = c->id;
		o->avoidT = hit;
	}
	// No new choice in the grace period: the grid holds its lanes.
	if (!graceActive(f, i) && changeTrafficLane(f, o, &sc, c->relL > 0.0f, AVOID_HORIZON))
		return 1e9f;
	return avoidPace(o, c);
}

bool opponent_player_drag_hit(OpponentField* f, const RaceRoute* route, const CarSim* player,
		int* lane) {
	*lane = -1;
	if (!f || !route || !player || !f->lanes.lanes || !f->playerTracked) return false;
	if (!dragHitRun(f, route, f->playerColl, -1, f->playerLineDist, player, &f->playerDragHit,
			&f->playerDragHitWho))
		return false;
	const HitCar me = { f->playerLineDist, f->playerLat, player->pos[2], f->playerHalfAcross,
		true };
	HitCar them;
	if (hitCar(f, f->playerDragHitWho, player, &them))
		*lane = dragHitLane(f, route, &me, &them, true);
	return true;
}

// The player's car on AIGoalPark (opponent_park_player), kept as the field's
// racers are, from the field's own figures for it.
static Opponent s_park;

static void parkSync(const OpponentField* f, const CarSim* player) {
	Opponent* o = &s_park;
	o->lineDist = f->playerLineDist;
	o->lineLat = f->playerLat;
	o->alongV = f->playerAlongV;
	o->halfAlong = f->playerHalfAlong;
	o->halfAcross = f->playerHalfAcross;
	o->sim.chassis = player->chassis;
	memcpy(o->sim.pos, player->pos, sizeof(o->sim.pos));
	o->sim.speed = player->speed;
}

void opponent_park_player_begin(OpponentField* f, const CarSim* player) {
	memset(&s_park, 0, sizeof(s_park));
	if (!f || !player) return;
	parkSync(f, player);
	// PORT: the new AICarController's first DrivingChoice is not decoded. It
	// is chooseHere's: the offset the car is at, in a drag its nearest lane.
	chooseHere(f, &s_park, f->playerLat);
}

bool opponent_park_player(OpponentField* f, const RaceRoute* route, const CarSim* player,
		float ahead, float dt, float pt[2], float* lateral, float* choice) {
	if (!f || !route || !player || !f->playerTracked) return false;
	Opponent* o = &s_park;
	parkSync(f, player);
	// ChangeCurrentL, then StopDriving's CalculatePointsToDrive, as a racer's.
	const float run = o->alongV > 0.0f ? o->alongV * dt : 0.0f;
	float lo, hi;
	o->curL = toward(o->curL, choiceL(f, o, o->lineDist), o->latRate * run);
	if (ownRoadSpan(f, o, o->lineDist, &lo, &hi)) o->curL = keepOnRoad(o->curL, lo, hi);
	o->choiceSec += dt;
	stopChoice(f, route, o, NULL);
	o->goalL = choiceL(f, o, o->lineDist);
	// CalcSteerToPoint: the point on the drive target, that far ahead.
	const float a = chordAhead(f, route, o, ahead);
	if (!drivePoint(f, route, o, lineAhead(f, o, a), pt)) {
		pt[0] = player->pos[0] + player->rot[0][0] * a;
		pt[1] = player->pos[1] + player->rot[0][1] * a;
	}
	*lateral = o->lineLat - lineOffset(f, o, o->lineDist);
	if (choice) *choice = o->goalL;
	return true;
}

// IsPointAheadOfCar (0x80016388): `pt` within acos(cosMin) of the way the car
// is going -- its velocity, or its nose under AHEAD_SLOW.
static bool pointAhead(const CarSim* s, const float pt[2], float cosMin) {
	float dx = pt[0] - s->pos[0], dy = pt[1] - s->pos[1];
	float hx = s->vel[0], hy = s->vel[1];
	if (s->speed < AHEAD_SLOW) {
		hx = s->rot[0][0];
		hy = s->rot[0][1];
	}
	const float dl = sqrtf(dx * dx + dy * dy), hl = sqrtf(hx * hx + hy * hy);
	if (dl < 1e-6f || hl < 1e-6f) return false;
	return (dx * hx + dy * hy) / (dl * hl) > cosMin;
}

// AIAction::GetHeadingClassification (0x8000DF8C): 0 when the car is on the
// road or not leaving it, 1 when it is driving out across an edge, 2 when it
// is backing out across one.
//
// Moving (over HEAD_SLOW), the car is leaving when its velocity takes it to
// the right-hand edge within HEAD_SOON, or it is past either edge and still
// going further out; "driving" or "backing" is the sign of its speed along
// the nose (CarState +0x3FC). Retail times the left-hand edge too, but that
// arm tests the same three conditions as the right's (0x8000E1E0: rate over
// 0.001, both edges still ahead), so it never runs, and the left edge counts
// only once the car is past it. Near enough stopped, it is 1 when a point
// HEAD_NOSE_M ahead of the nose is past an edge and further out than the car.
//
// `checkSteer` (GetUnstuck's) lets a 1 go when the driver's steer point is
// ahead of the car; a 2 goes when the nose points within 70 degrees of the
// road's direction.
//
// PORT: retail measures from the car's TrackRoutePoint along its forward or
// backward direction, with its two half-widths. The port measures from the
// car's place on the racing line, along the line, with the road's edges off
// the line there -- the same edges, from a different origin. The 2's test
// takes the race's direction of travel where retail takes the route point's
// own forward (+0x18), which is the same wherever the line rides a route
// down its posted way.
static int headingClass(const OpponentField* f, const Opponent* o, bool checkSteer) {
	float p[2], d[2], eL, eR;
	if (!roadEdges(f, o, o->lineDist, &eL, &eR) ||
			!ailine_pose(f->line, o->lineDist, 0.0f, p, d))
		return 0;
	return opponent_heading_class(&o->sim, o->drv.steerTo, p, d, eL, eR, checkSteer);
}

// The classification itself, from the line's point `p` and way `d` beside the
// car and the road's edges there as metres right of it (`eL` negative). Shared
// with encounter.c's free-roam racers (TASKS.md #517).
int opponent_heading_class(const CarSim* c, const float steerTo[2], const float p[2],
		const float d[2], float eL, float eR, bool checkSteer) {
	const float wL = -eL, wR = eR;
	const int out = c->vfwd > 0.0f ? 1 : 2;
	// Metres right of the line, along it.
#define HEAD_LAT(x, y) (((x) - p[0]) * d[1] - ((y) - p[1]) * d[0])
	int cls = 0;
	if (fabsf(c->speed) > HEAD_SLOW) {
		const float now = HEAD_LAT(c->pos[0], c->pos[1]);
		const float fut = HEAD_LAT(c->pos[0] + c->vel[0] * HEAD_PREDICT,
			c->pos[1] + c->vel[1] * HEAD_PREDICT);
		const float inL = wL + now, inR = wR - now;       // inside each edge
		const float inLFut = wL + fut, inRFut = wR - fut;
		const float rate = (inLFut - inL) / HEAD_PREDICT;  // m/s to the right
		float t = HEAD_NEVER;
		if (rate > HEAD_RATE_MIN && inR > 0.0f && inL > 0.0f) t = inR / rate;
		if (t <= HEAD_SOON) cls = out;
		else if (inR < 0.0f && inRFut < inR) cls = out;
		else if (inL < 0.0f && inLFut < inL) cls = out;
	} else {
		float nx = c->rot[0][0], ny = c->rot[0][1];
		const float nl = sqrtf(nx * nx + ny * ny);
		if (nl > 1e-6f) {
			nx /= nl;
			ny /= nl;
		}
		const float now = HEAD_LAT(c->pos[0], c->pos[1]);
		const float nose = HEAD_LAT(c->pos[0] + nx * HEAD_NOSE_M, c->pos[1] + ny * HEAD_NOSE_M);
		const float inR = wR - nose, inL = wL + nose;
		if (inR < 0.0f && inR - (wR - now) < 0.0f) cls = 1;
		else if (inL < 0.0f && inL - (wL + now) < 0.0f) cls = 1;
	}
#undef HEAD_LAT
	if (cls == 1 && checkSteer && pointAhead(c, steerTo, HEAD_STEER_COS)) cls = 0;
	if (cls == 2) {
		float nx = c->rot[0][0], ny = c->rot[0][1];
		const float nl = sqrtf(nx * nx + ny * ny), dl = sqrtf(d[0] * d[0] + d[1] * d[1]);
		if (nl > 1e-6f && dl > 1e-6f && (nx * d[0] + ny * d[1]) / (nl * dl) > HEAD_ALONG_COS)
			cls = 0;
	}
	return cls;
}

// How far outside the road the car is, metres (negative inside): GetUnstuck's
// first trigger (0x80010E44..0x80010F90), off the edge on the car's side of the
// line. PORT: measured off the line, as headingClass is. Negative with no line.
static float outsideRoad(const OpponentField* f, const Opponent* o) {
	float l, r;
	if (!roadEdges(f, o, o->lineDist, &l, &r)) return -1.0f;
	const float lat = o->lineLat;
	return lat - r > l - lat ? lat - r : l - lat;
}

// AIActionIJustGotHit for this think (HIT_*): false when the racer has none.
// Otherwise it has set the choice and `*follow` to the speed it asks for, or a
// figure past any speed when it asks for none.
static bool gotHit(const OpponentField* f, const RaceRoute* route, int i, Opponent* o,
		u32 hits, const CarSim* player, float* follow) {
	if (!hits) return false;
	// GetMostImportantNearbyCollidingAvoidable: the player's car before any
	// other; between two of a kind, one ahead before one behind, and then the
	// one less far ahead.
	int who = -2;
	float bestD = 0.0f;
	for (int k = -1; k < f->count; k++) {
		float d;
		if (k < 0) {
			if (!(hits & OPPONENT_HIT_PLAYER) || !player || !f->playerTracked) continue;
			d = wrapGap(route, f->playerLineDist - o->lineDist);
		} else {
			const Opponent* q = &f->car[k];
			if (!(hits & (1u << k)) || k == i || !q->driving || !q->simReady) continue;
			d = wrapGap(route, q->lineDist - o->lineDist);
		}
		bool take = who == -2;
		if (!take && (k < 0) == (who < 0))
			take = (bestD < 0.0f && d > 0.0f) || d < bestD;
		if (take) {
			who = k;
			bestD = d;
		}
	}
	if (who == -2) return false;

	// CalculatePointsToDrive (0x80011B20).
	const Opponent* q = who >= 0 ? &f->car[who] : NULL;
	*follow = opponent_hit_step(&o->sim, o->lineLat, o->halfAcross, o->halfAlong, &o->goalL,
		&o->latRate, q ? q->lineLat : f->playerLat, q ? q->halfAcross : f->playerHalfAcross,
		q ? q->halfAlong : f->playerHalfAlong, bestD,
		bestD > 0.0f ? headingClass(f, o, false) : 0, q ? NULL : player,
		f->raceSec > HIT_FLOOR_SEC);
	return true;
}

float opponent_hit_step(const CarSim* s, float myL, float myHalfAcross, float myHalfAlong,
		float* goalL, float* latRate, float theirL, float theirHalfAcross, float theirHalfAlong,
		float relD, int headCls, const CarSim* player, bool floorOn) {
	float l, rate = HIT_RATE;
	if (theirL - myL < 0.0f) {
		l = theirL + theirHalfAcross + myHalfAcross + HIT_CLEAR;
		if (*goalL > l) l = *goalL;
	} else {
		l = theirL - theirHalfAcross - myHalfAcross - HIT_CLEAR;
		if (*goalL < l) l = *goalL;
	}
	const float overlap = myHalfAlong + theirHalfAlong - HIT_OVERLAP;
	const bool ahead = relD > overlap, behind = relD < -overlap;
	if (behind) {
		l = *goalL;
		rate = AVOID_RATE_MIN;
	}
	*goalL = l;       // DrivingChoice::SetChoice: the offset and the rate
	*latRate = rate;

	// The speed (0x80011D3C on). Leaving the road with the other car ahead:
	// as hard on the brakes as the car can be.
	if (headCls == 1 && relD > 0.0f)
		return s->speed + carsim_max_decel(s, s->speed) * HIT_BRAKE_SEC;
	if (player && ahead) {
		float v = sqrtf(player->vel[0] * player->vel[0] + player->vel[1] * player->vel[1] +
			player->vel[2] * player->vel[2]);
		if (v < HIT_FLOOR && floorOn) v = HIT_FLOOR;
		return v;
	}
	return 1e9f;
}

// A collision on `side` that began UNSTUCK_TOUCH_AGE or more ago and is still
// going -- the Collision +0x08 start time GetUnstuck reads, on the side
// GetCollisionClassification (0x8000E564) puts it.
static bool touchHeld(const CarSim* s, int side) {
	return carsim_touch_age(s, side) >= UNSTUCK_TOUCH_AGE;
}

// Retail's "less than, or within a hair of" for the 5 m/s tests.
static bool slowerThan(float v, float lim) {
	return v < lim || fabsf(v - lim) <= UNSTUCK_SLOW_EPS;
}

// AIActionGetUnstuck::CalculateScore (0x80010D58), with RaceCarController's
// AllowedToCallGetUnstuckAction in front of it. Any of: more than UNSTUCK_OUT
// outside the road; on the MellowMover at UNSTUCK_HEAD_SPEED or under and
// driving out of the road (headingClass 1, the steer point not ahead); or
// held up front or side on for 0.4 s under 5 m/s. An online race's flags
// (0x8040CA84, 0x8040CA98) skip the first two.
static bool unstuckWanted(const OpponentField* f, int i, const Opponent* o) {
	if (graceActive(f, i)) return false;
	return opponent_unstuck_score(&o->sim, outsideRoad(f, o), headingClass(f, o, true),
		o->sinceReset, o->sinceUnstuck);
}

// unstuckWanted's tests past the grace period, from the car's own figures:
// `outside` is outsideRoad's, `headCls` the checkSteer heading class. Shared
// with encounter.c (TASKS.md #517).
bool opponent_unstuck_score(const CarSim* s, float outside, int headCls, float sinceReset,
		float sinceUnstuck) {
	if (sinceReset <= UNSTUCK_SINCE_RESET || !s->onGround) return false;
	if (sinceUnstuck <= UNSTUCK_COOLDOWN) return false;
	if (outside >= UNSTUCK_OUT) return true;
	if (s->mellow && s->speed <= UNSTUCK_HEAD_SPEED && headCls == 1)
		return true;
	if (!slowerThan(s->speed, UNSTUCK_SLOW)) return false;
	return touchHeld(s, CARSIM_TOUCH_FRONT) ||
		touchHeld(s, CARSIM_TOUCH_LEFT) ||
		touchHeld(s, CARSIM_TOUCH_RIGHT);
}

// GetGoalPoint (0x80011218): the point the car backs round to face, on its
// own line.
static void unstuckGoal(const OpponentField* f, const RaceRoute* route,
		const Opponent* o, float out[2]) {
	if (!drivePoint(f, route, o, lineAhead(f, o, UNSTUCK_GOAL_M), out)) {
		out[0] = o->sim.pos[0] + o->sim.rot[0][0] * UNSTUCK_GOAL_M;
		out[1] = o->sim.pos[1] + o->sim.rot[0][1] * UNSTUCK_GOAL_M;
	}
}

// HasBeenAccomplished (0x800112A8): the nose points within about 26 degrees
// of the goal, and nothing is still pinning it from the front.
static bool unstuckDone(const Opponent* o, const float goal[2]) {
	return opponent_unstuck_done(&o->sim, goal, o->sinceReset, o->unstuckSec);
}

bool opponent_unstuck_done(const CarSim* s, const float goal[2], float sinceReset,
		float unstuckSec) {
	if (sinceReset <= UNSTUCK_SINCE_RESET || unstuckSec < UNSTUCK_MIN_LIFE)
		return false;
	float gx = goal[0] - s->pos[0], gy = goal[1] - s->pos[1];
	float fx = s->rot[0][0], fy = s->rot[0][1];
	float gl = sqrtf(gx * gx + gy * gy), fl = sqrtf(fx * fx + fy * fy);
	if (gl < 1e-4f || fl < 1e-4f) return false;
	bool facing = (gx * fx + gy * fy) / (gl * fl) >= UNSTUCK_FACING;
	if (touchHeld(s, CARSIM_TOUCH_FRONT) && s->speed < UNSTUCK_SLOW)
		return false;
	return facing;
}

// HasFailed (0x80011524): off the ground, backing out across the road's edge
// (headingClass 2), or backed into something.
static bool unstuckFailed(const OpponentField* f, const Opponent* o) {
	return opponent_unstuck_failed(&o->sim, headingClass(f, o, true), o->unstuckSec);
}

bool opponent_unstuck_failed(const CarSim* s, int headCls, float unstuckSec) {
	if (!s->onGround) return true;
	if (unstuckSec < UNSTUCK_MIN_LIFE) return false;
	if (headCls == 2) return true;
	return touchHeld(s, CARSIM_TOUCH_BEHIND) &&
		slowerThan(fabsf(s->speed), UNSTUCK_SLOW);
}

// Is `o` still inside any other car? Car::GetLastPenetrationTime's question,
// asked of the cars a blinking car passes through.
static bool overlapsAnother(const OpponentField* f, const Opponent* o,
		const CarSim* player) {
	if (player && carsim_cars_overlap(&o->sim, player)) return true;
	for (int k = 0; k < f->count; k++) {
		const Opponent* q = &f->car[k];
		if (q == o || !q->driving || !q->simReady) continue;
		if (carsim_cars_overlap(&o->sim, &q->sim)) return true;
	}
	return false;
}

// Car::Move's reset (0x8021F3F4 -> ResetToRoutePoint 0x80221314 ->
// ResetCore 0x80220F60): back on the route facing down it, still rolling at
// what the car had along the road, never more than RollingResetSpeed, and
// blinking.
//
// Retail puts the car on the TrackRoutePoint itself, lateral 0 (the driving
// line's offset only under a tweak), at the point's own z, facing down the
// route. PORT: this puts it on the racing line where the car is, at the
// matched TrackRoute's z, facing down the line. The line is the route point
// plus the line's offset wherever the posting is the road the race takes, but
// ailine_build moves it off the posting where that runs into a wall -- before
// the 4001 fork the posting is the other road's (TASKS.md #280) -- and a reset
// onto the posting there would put the car the wrong side of the gore.
// Without a line, the route point is the nearest point on the TrackPath
// spline, and its z is the last height the car stood level near it -- unless
// that was taken too far back along the road to trust, when it is the car's
// own.
static void resetToRoute(const OpponentField* f, const RaceRoute* route,
		Opponent* o, const CarSimWorld* w) {
	float p[3], dir[2];
	AiLineSample s;
	if (ailine_at(f->line, o->lineDist, &s) &&
			ailine_pose(f->line, o->lineDist, 0.0f, p, dir)) {
		p[2] = s.z;
	} else {
		float gap = fabsf(o->coord.dist - o->roadDist);
		float len = race_route_length(route);
		if (race_route_loops(route) && len - gap < gap) gap = len - gap;
		p[0] = o->coord.onPath[0];
		p[1] = o->coord.onPath[1];
		p[2] = gap <= ROAD_Z_REACH ? o->roadZ : o->sim.pos[2];
		dir[0] = o->coord.dir[0];
		dir[1] = o->coord.dir[1];
	}
	float along = o->sim.rot[0][0] * dir[0] + o->sim.rot[0][1] * dir[1];
	float roll = (o->sim.vfwd > 0.0f ? o->sim.vfwd : 0.0f) * (along > 0.0f ? along : 0.0f);
	if (roll > RACE_RESET_SPEED) roll = RACE_RESET_SPEED;
	carsim_place(&o->sim, w, p, atan2f(dir[1], dir[0]));
	carsim_set_rolling(&o->sim, roll);
	// ResetCore: ZeroSteeringMemory, the wheel straight, SetLowGear.
	aidriver_zero_memory(&o->drv);
	o->drv.dirState = 1;
	o->sim.resetRequest = false;
	o->sinceReset = 0.0f;
	o->unstuck = false;
	o->progressN = o->progressIdx = 0;
	o->progressSec = 0.0f;
	o->progressAt = o->lineDist;
	o->blinkSec = BLINK_SEC;
	o->lastThr = o->lastBrk = o->lastSteerDeg = 0.0f;
	// PORT: what a reset does to the DrivingChoice is not decoded. The car
	// is on the line now, so it drives on from there.
	o->lineLat = 0.0f;
	chooseHere(f, o, 0.0f);
}

// PORT (TASKS.md #280): retail's own race route for 4042 forwards climbs a
// 1.45 m vertical face at 2,674 m -- H13a -> H13c -> H1a, which only 4043 -
// 4045's gates close -- and a 1.5 m one at 3,316 m, which 4044 backwards
// climbs too. No car drives up either: the face is a wall to the body, and the
// road beyond is out of the wheels' reach (NewGetElevation looks no more than
// 1 m over the probe). Every car of the field stopped under the first, and the
// very-stuck reset put each back on the line where it was, under the face, for
// ever. So a car about to reach one is put on the line just past it, as a
// reset would put it and blinking, but keeping its speed: it has not crashed.
// The crossing is the sample past the wall, which can be up to a sample on
// from it, and the car's nose is half a car ahead of its middle.
#define HOP_REACH  6.0f   // how near the crossing the car's middle comes first
#define HOP_LAND   4.0f   // and how far past its last wall the car is put

// PORT (TASKS.md #280): where a car past a sprint's end steers as it brakes
// to a stop -- straight on down the road's last heading, along the right-hand
// edge of its last stretch -- and, in `outLat`, that edge as metres right of
// the line. Stopped on the line, the winner held the rest of the field short
// of it. False where the field has no line, and the car steers straight on.
static bool pullOver(const OpponentField* f, const RaceRoute* route, const Opponent* o,
		float ahead, float out[2], float* outLat) {
	const float end = race_route_length(route) - 1.0f;
	float p[2], dir[2], lo, hi;
	if (!roadSpan(f, end, halfWidth(o), &lo, &hi) || !ailine_pose(f->line, end, hi, p, dir))
		return false;
	const float run = (o->sim.pos[0] - p[0]) * dir[0] + (o->sim.pos[1] - p[1]) * dir[1];
	const float at = (run > 0.0f ? run : 0.0f) + ahead;
	out[0] = p[0] + dir[0] * at;
	out[1] = p[1] + dir[1] * at;
	*outLat = hi;
	return true;
}

static void hopFace(const OpponentField* f, const RaceRoute* route, Opponent* o,
		const CarSimWorld* w, const AiLineCrossing* face) {
	float d = face->end + HOP_LAND;
	if (race_route_loops(route)) d = fmodf(d, race_route_length(route));
	float p[3], dir[2];
	AiLineSample s;
	if (!ailine_at(f->line, d, &s) || !ailine_pose(f->line, d, 0.0f, p, dir)) return;
	p[2] = s.z;
	float along = o->sim.rot[0][0] * dir[0] + o->sim.rot[0][1] * dir[1];
	float roll = (o->sim.vfwd > 0.0f ? o->sim.vfwd : 0.0f) * (along > 0.0f ? along : 0.0f);
	carsim_place(&o->sim, w, p, atan2f(dir[1], dir[0]));
	carsim_set_rolling(&o->sim, roll);
	aidriver_zero_memory(&o->drv);
	o->drv.dirState = 1;
	o->sim.resetRequest = false;
	o->unstuck = false;
	o->lineDist = d;
	o->progressN = o->progressIdx = 0;
	o->progressSec = 0.0f;
	o->progressAt = d;
	o->blinkSec = BLINK_SEC;
	o->lineLat = 0.0f;
	chooseHere(f, o, 0.0f);
}

#define STEER_FLIP_DEG 5.0f

// ---- AI nitrous (TASKS.md #281 / #255; SpeedPS, tools/re/wip268/ainos*).
//
// Retail's chain, per racer (GetDriverInfo +4 == 2):
// - The four driving actions' CalculatePointsToDrive (JustDrive, Pass,
//   DriveTrafficLane, AvoidAvoidable) call AIAction::MaybeUseNitrous
//   (0x8000EEA8): with the bottle shut and CanUseNitrousRightNow, the
//   action's +0x1F0 is set, and ExecuteCurrentAction (0x80008714) stamps
//   JustStartedNitrous (+0x7B4) and StartNitrous.
// - RaceCarController::Think shuts it: StopNitrous + JustStoppedNitrous
//   (+0x7B8) when it burns, CanDisengageNitrousRightNow, and either
//   ShortTermCanUseNitrousRightNow (the in-between thinks) or
//   CanUseNitrousRightNow (the full ones) says no. The port thinks every
//   frame and asks both.
// The difficulty record's [17] / [18] (the cooldown, the last lap's), [19]
// (the least in the bottle to open it, the least burn before shutting it)
// and [20] (the longest burn). [1] has no reader in SpeedPS.

#define NOS_LAST_QUARTER   0.75f  // 0x80317c04: the last lap's last quarter
#define NOS_GETAWAY_BURN   2.0f   // 0x80317c0c: the getaway's longest burn
#define NOS_LINE_SPEED_MIN 0.001f // 0x80317c1c
#define NOS_BEHIND_LINE    0.8f   // 0x80317c20: under 80% of the line's speed
#define NOS_MONO_SEC       1.0f   // 0x80317c08: rising speeds held this long
#define NOS_SLIP           2.0f   // AreDriveWheelsSlipping (0x80315f10)
#define NOS_SHIFT_BAND     0.95f  // IsCloseToShiftingGears (0x80315ef8)
// AICarControllerGearThresholdForUsingNitrousInCircuitRace (r13-0x7FF0):
// first gear, in the DriveTrain's numbering (dtGear). A drag's is the record's
// byte at +0x588 plus 1: [25] is 0 in every record, so neutral.
#define NOS_GEAR_CIRCUIT   2
#define NOS_GEAR_DRAG      1
// PORT: retail reads the line speed off the FuturePoint the car is on, which
// it keeps up every think; the port finds the route point (trackroute_nearest,
// a search over every route) this often, and the speeds ahead from the next
// OPP_NOS_FUTURE points of that route.
#define OPP_NOS_ROUTE_SEC  0.5f
#define OPP_NOS_FUTURE     8

// Seconds since a stamp on the race clock; a stamp of 0 was never made, which
// retail's WorldTimer - 0 makes the whole time since the world began.
static float nosSince(const OpponentField* f, float at) {
	return at == 0.0f ? 1e9f : f->raceSec - at;
}

// The route point the car is on: its line speed (+0x2C), and whether the
// speeds from there on never fall (FuturePoints::
// AreDrivingLineSpeedsIncreasingMonotonically, 0x800391B0). No route point:
// speed 0 and not rising, so only the rest of CanUseNitrousRightNow decides.
static void nosRouteSample(const OpponentField* f, const RaceRoute* route, Opponent* o) {
	o->nosLine = 0.0f;
	o->nosMono = false;
	if (!f->routes) return;
	const TrackRouteSource src = trackroute_source(f->routes);
	if (src.nRoutes <= 0) return;
	TrackRouteQuery q;
	memset(&q, 0, sizeof(q));
	q.pos[0] = o->sim.pos[0];
	q.pos[1] = o->sim.pos[1];
	q.pos[2] = o->sim.pos[2];
	q.flags = TRN_RACE_DIST;
	q.racing = true;
	q.raceDist = o->coord.dist;
	q.loopLen = race_route_loops(route) ? race_route_length(route) : 0.0f;
	TrackRoutePick rp;
	if (!trackroute_nearest(&src, &q, &rp)) return;
	o->nosLine = (float)rp.pt.speed;
	int first, count;
	float bbMin[2], bbMax[2];
	u32 bits;
	src.route(src.ctx, rp.route, &first, &count, bbMin, bbMax, &bits);
	bool mono = true;
	float prev = o->nosLine;
	for (int k = 1; k <= OPP_NOS_FUTURE && rp.point + k < first + count; k++) {
		TrackRoutePt p;
		src.point(src.ctx, rp.point + k, &p);
		if ((float)p.speed < prev) {
			mono = false;
			break;
		}
		prev = (float)p.speed;
	}
	o->nosMono = mono;
}

// Something solid against the body now (CollisionBody's list, IsWallInvolved).
static bool nosWallTouch(const CarSim* s) {
	for (int k = 0; k < 4; k++)
		if (carsim_touch_age(s, k) >= 0.0f && s->touchWith[k] == CARSIM_TOUCH_WALL)
			return true;
	return false;
}

// AICarController::ShortTermCanUseNitrousRightNow (0x80008C70).
static bool nosShortTerm(const OpponentField* f, Opponent* o) {
	CarSim* s = &o->sim;
	const float minLeft = difficulty_field(o->skill < 0 ? 0 : o->skill, 19);
	if (!(carsim_nos_on(s) || carsim_nos_remaining(s) >= minLeft)) return false;
	if (s->wheelsDown != CARPHYS_WHEELS) return false;
	if (nosWallTouch(s)) return false;
	// AreDriveWheelsSlipping (0x80008F14): the drive wheels' mean |slip|.
	float slip = 0.0f;
	int n = 0;
	for (int k = 0; k < CARPHYS_WHEELS; k++)
		if (carsim_is_drive_wheel(s, k)) {
			slip += fabsf(s->wheel[k].s);
			n++;
		}
	if (n > 0) slip /= (float)n;
	if (slip > NOS_SLIP) return false;
	if (s->dtGear < (f->drag ? NOS_GEAR_DRAG : NOS_GEAR_CIRCUIT)) return false;
	// IsCloseToShiftingGears: out of [down, down + (up - down) * 0.95].
	float lo, hi;
	carsim_shift_band(s, &lo, &hi);
	hi = (hi - lo) * NOS_SHIFT_BAND + lo;
	return s->rpm >= lo && s->rpm <= hi;
}

// RaceCarController::CanUseNitrousRightNow (0x80028D48). The getaway (game
// mode 7) is not a field race, so its arms are left out.
static bool nosCanUse(const OpponentField* f, int i, Opponent* o) {
	CarSim* s = &o->sim;
	if (f->drag) return nosShortTerm(f, o);
	const int sk = o->skill < 0 ? 0 : o->skill;
	const OpponentCatchup* c = &f->catchup;
	// RacingCar +0x09 (the laps done) against the race's laps, and how far
	// round this lap it is (car +0x8E8 against the start's, wrapped).
	const float laps = c->valid ? c->lapsDone[i] : 0.0f;
	const int lap = laps > 0.0f ? (int)laps : 0;
	const bool lastLap = lap == (c->laps > 0 ? c->laps : 1) - 1;
	const bool lastQuarter = lastLap && laps - (float)lap > NOS_LAST_QUARTER;
	// The record's +0x589 byte arm: [25] is 0 everywhere, so never taken.
	const bool on = carsim_nos_on(s) != 0;
	if (on && nosSince(f, o->nosOnAt) >= difficulty_field(sk, 20)) return false;
	float cool = difficulty_field(sk, lastLap ? 18 : 17);
	if (lastQuarter) cool = 0.0f;
	if (!on && nosSince(f, o->nosOffAt) <= cool) return false;
	// Under 80% of the line's speed (a forwards traffic lane always passes),
	// or the line speeding up for a second.
	bool behind = false;
	if (o->lane >= 2 && o->lane < 6) behind = true;
	else if (o->nosLine > NOS_LINE_SPEED_MIN)
		behind = s->speed / o->nosLine < NOS_BEHIND_LINE;
	if (!o->nosMono) o->nosMonoAt = 0.0f;
	else if (o->nosMonoAt == 0.0f) o->nosMonoAt = f->raceSec;
	const bool rising = o->nosMonoAt != 0.0f && f->raceSec - o->nosMonoAt > NOS_MONO_SEC;
	if (!behind && !rising) return false;
	return nosShortTerm(f, o);
}

// CanDisengageNitrousRightNow (0x800291D4): a drag any time, else once it has
// burnt [19] seconds.
static bool nosCanStop(const OpponentField* f, const Opponent* o) {
	if (f->drag) return true;
	return difficulty_field(o->skill < 0 ? 0 : o->skill, 19) < nosSince(f, o->nosOnAt);
}

static void nosStop(OpponentField* f, Opponent* o, u8 why) {
	carsim_nos_stop(&o->sim);
	o->nosOffAt = f->raceSec;   // JustStoppedNitrous
	o->nosWhy = why;
}

// Think's two stop arms, then the action's MaybeUseNitrous and
// ExecuteCurrentAction's start. Racing, not drifting, before the step.
static void updateNitrous(OpponentField* f, const RaceRoute* route, int i, float dt) {
	Opponent* o = &f->car[i];
	CarSim* s = &o->sim;
	o->nosRouteSec -= dt;
	if (o->nosRouteSec <= 0.0f) {
		o->nosRouteSec = OPP_NOS_ROUTE_SEC;
		nosRouteSample(f, route, o);
	}
	if (carsim_nos_on(s) && !nosShortTerm(f, o) && nosCanStop(f, o))
		nosStop(f, o, OPP_NOS_STOP_SHORT);
	if (carsim_nos_on(s) && !nosCanUse(f, i, o) && nosCanStop(f, o))
		nosStop(f, o, OPP_NOS_STOP_CANT);
	if (o->nosAction && !carsim_nos_on(s) && nosCanUse(f, i, o) && carsim_nos_start(s)) {
		o->nosOnAt = f->raceSec;   // JustStartedNitrous
		o->nosWhy = OPP_NOS_STOP_NONE;
	}
}

// RacingCar::Update (0x80227770), the arm for a racer with no style points
// (GetDriverInfo +4 == 2): more than a second (0x8033aec4) after the last
// top-up, FillNitrous with the share of a lap covered since, times 0.5
// (0x8033aed0) -- half a bottle a lap -- or times 0 in a drag. The lap share
// is car +0x8E8 (the TrackPathCoordinate's fraction) * 100 against the last
// one, ComparePercentDistances wrapping +-50 on a loop, and never below 0.
// FakeStartlineCrossing (0x80227580) stamps both at GO; the port at the first
// racing frame.
#define NOS_FILL_SEC      1.0f
#define NOS_FILL_PER_LAP  0.5f

static void updateNosFill(OpponentField* f, const RaceRoute* route, Opponent* o) {
	const float len = race_route_length(route);
	if (len <= 0.0f) return;
	const bool loops = race_route_loops(route);
	float d = o->coord.dist;
	if (loops) {
		d = fmodf(d, len);
		if (d < 0.0f) d += len;
	}
	const float pct = d / len * 100.0f;
	if (o->nosFillAt == 0.0f) {
		o->nosFillAt = f->raceSec;
		o->nosFillPct = pct;
		return;
	}
	if (!(f->raceSec - o->nosFillAt > NOS_FILL_SEC)) return;
	float gain = pct - o->nosFillPct;
	if (loops) {
		if (gain < -50.0f) gain += 100.0f;
		else if (gain > 50.0f) gain -= 100.0f;
	}
	if (gain < 0.0f) gain = 0.0f;
	carsim_nos_fill(&o->sim, gain / 100.0f * (f->drag ? 0.0f : NOS_FILL_PER_LAP));
	o->nosFillPct = pct;
	o->nosFillAt = f->raceSec;
}

// RaceCarController::DecideBoostFactor for car `i` (difficulty.c), from the
// race inputs the caller left in f->catchup.
static void updateBoost(OpponentField* f, int i, float dt) {
	Opponent* o = &f->car[i];
	const OpponentCatchup* c = &f->catchup;
	if (!c->valid) {
		o->boost = 1.0f;
		o->sim.catchup = 1.0f;
		return;
	}
	o->boostSec += dt;
	DiffBoostIn in;
	memset(&in, 0, sizeof(in));
	in.gameMode = 1;
	in.racer = true;
	in.dragOrDrift = f->drag || f->drift;
	in.streetX = f->shortTrack;
	in.pointToPoint = c->pointToPoint;
	in.laps = c->laps;
	in.trafficLevel = c->trafficLevel;
	in.catchupOn = c->catchupOn;
	in.handicap = c->handicap;
	in.players = c->players;
	in.lapLength = c->lapLength;
	in.raceSec = c->raceSec;
	in.actionAllows = !o->unstuck;
	in.skill = o->skill;
	in.laps_done = c->lapsDone[i];
	in.place = c->place[i];
	in.speed = o->sim.speed;
	in.tLaps = c->pLaps;
	in.tPlace = c->pPlace;
	in.tSpeed = c->pSpeed;
	in.tRatio = c->pRatio;
	o->boost = difficulty_decide_boost(&in, &o->boostSec, o->boost);
	o->sim.catchup = o->boost > 1.0f ? o->boost : 1.0f;
}

// The race's forks as racer `i` reaches them (TASKS.md #409): off a branch
// once past its end, every offset carried along the branch the car is on, and
// DecideSignpostAsRacer (0x8000A914) at each sign ahead. A type-4 sign, not in
// a drag: TrackInfo +0xE4 -1 waits out Race::GracePeriodIsActive (graceActive,
// the same 12 s less 0.5 s a driver id), any other value that many seconds of
// the race; then bRandom(1) under the sign's odds takes it. A sign that comes
// while the car is on another branch is passed by.
static void forkStep(OpponentField* f, int i, Opponent* o) {
	if (!f->forks) return;
	TrackLaneRoad r;
	if (o->fork >= 0 && !forkRoad(f, o, o->lineDist, &r) &&
			trackroute_fork_ahead(f->forks, o->fork, o->lineDist) < 0.0f) {
		if (f->log) {
			char b[96];
			snprintf(b, sizeof(b), "ai: %s off fork %d at %.0f m", o->name, o->fork,
				(double)o->lineDist);
			f->log(b);
		}
		o->fork = -1;
	}
	const float lat = forkRoad(f, o, o->lineDist, &r) ? r.lat : 0.0f;
	const float d = lat - o->forkLat;
	o->curL += d;
	o->goalL += d;
	o->forkLat = lat;
	if (o->finished) return;
	const int n = trackroute_fork_count(f->forks);
	for (int j = 0; j < n && j < 64; j++) {
		const u64 bit = (u64)1 << j;
		const float a = trackroute_fork_ahead(f->forks, j, o->lineDist);
		if (a < -FORK_FORGET_M) {
			o->forkSeen &= ~bit;
			continue;
		}
		if (a < 0.0f || a > FORK_DECIDE_M || (o->forkSeen & bit)) continue;
		o->forkSeen |= bit;
		TrackFork k;
		if (!trackroute_fork_info(f->forks, j, &k)) continue;
		const bool open = f->forkAfterSec < 0.0f ? !graceActive(f, i)
			: f->forkAfterSec < f->raceSec;
		const float roll = (float)bRandomNext(&o->rng, 0x10000u) * (1.0f / 65536.0f);
		const bool take = open && o->fork < 0 && roll < k.p;
		if (take) o->fork = j;
		if (f->log) {
			char b[128];
			snprintf(b, sizeof(b), "ai: %s %s fork %d onto r%d at %.0f m (roll %.2f of %.2f%s)",
				o->name, take ? "takes" : "passes", j, k.branch, (double)k.sign,
				(double)roll, (double)k.p, !open ? ", not yet open" :
				(o->fork >= 0 && !take ? ", on another" : ""));
			f->log(b);
		}
	}
}

void opponent_update(OpponentField* f, const RaceRoute* route,
		const CarSimWorld* w, const CarSim* player, bool racing, float dt) {
	if (!f || dt <= 0.0f) return;
	if (!route) {
		// PORT (#488): a race with no route, the LAN racers' cars alone
		// (opponent_start_driving()): their owners' inputs and the contacts,
		// as the arm below has them.
		if (racing) f->raceSec += dt;
		s_collField = f;
		s_collPlayer = player;
		carsim_add_impulse_hook(onCollImpulse);
		ageColl(f->playerColl, dt);
		for (int i = 0; i < f->count; i++) ageColl(f->car[i].coll, dt);
		for (int i = 0; i < f->count; i++) {
			Opponent* o = &f->car[i];
			o->hitBy = 0;
			if (!o->driving || !o->simReady || !o->remote) continue;
			if (o->blinkSec > 0.0f) {
				o->blinkSec -= dt;
				if (o->blinkSec < 0.0f) o->blinkSec = 0.0f;
			}
			o->lastThr = o->netIn.throttle;
			o->lastBrk = o->netIn.brake;
			if (!racing || o->inactive) continue;
			o->netIn.racing = true;
			carsim_update(&o->sim, &o->netIn, carWorld(o, w), dt);
		}
		return;
	}
	if (racing) f->raceSec += dt;
	if (player) trackPlayer(f, route, player);
	// The collisions' listener, and Collision::Update on every car's.
	s_collField = f;
	s_collPlayer = player;
	carsim_add_impulse_hook(onCollImpulse);
	ageColl(f->playerColl, dt);
	for (int i = 0; i < f->count; i++) ageColl(f->car[i].coll, dt);
	trackTraffic(f, route);
	for (int i = 0; i < f->count; i++) {
		Opponent* o = &f->car[i];
		// The race cars its body touched since the last think.
		const u32 hits = o->hitBy;
		o->hitBy = 0;
		if (!o->driving || !o->simReady) continue;
		const CarSimWorld* cw = carWorld(o, w);

		float xy[2] = { o->sim.pos[0], o->sim.pos[1] };
		race_coord_update(route, &o->coord, xy);
		const float lineLat = trackLine(f, &o->coord, &o->lineDist, xy);
		o->lineLat = lineLat;
		linePose(f, &o->coord, o->lineDist, &o->sim, &o->alongV, &o->halfAlong,
			&o->halfAcross);
		// PORT (TASKS.md #280): past a sprint's end there is no road left to
		// race on -- the route stops, and the car ran on into whatever is
		// beyond it and was reset back there over and over. Retail's finished
		// racer is on AIGoalPark (stopChoice), which brakes it to a stop on its
		// choice; past the port's line it pulls over (pullOver) as it does.
		const bool home = !race_route_loops(route) && o->lineDist >= race_route_length(route);
		o->sinceReset += dt;
		o->sinceUnstuck += dt;

		// A LAN racer's car (#321c): its owner's inputs, no AI; on the grid
		// until GO, as the others' are. main.c pulls it onto its states.
		if (o->remote) {
			if (o->blinkSec > 0.0f) {
				o->blinkSec -= dt;
				if (o->blinkSec < 0.0f) o->blinkSec = 0.0f;
			}
			o->lastThr = o->netIn.throttle;
			o->lastBrk = o->netIn.brake;
			if (!racing || o->inactive)
				continue;
			o->netIn.racing = true;
			carsim_update(&o->sim, &o->netIn, cw, dt);
			continue;
		}

		// Car::IsBlinking runs out on its own clock, except that Move holds
		// the last half second while the car is still inside another one.
		if (o->blinkSec > 0.0f && racing) {
			if (!(o->blinkSec <= BLINK_HOLD_SEC && overlapsAnother(f, o, player)))
				o->blinkSec -= dt;
			if (o->blinkSec < 0.0f) o->blinkSec = 0.0f;
		}

		// The action. RaceCarController::Think asks the running GetUnstuck
		// whether it is done before anything else, and its CleanUp clears the
		// steering memory against the goal it was steering at.
		float goal[2];
		// AIGoalPark's every new action is StopDriving: a car backing out
		// when it finishes stops doing so, with GetUnstuck's CleanUp.
		if (o->finished && o->unstuck) {
			aidriver_clear_steering(&o->drv, &o->sim);
			o->unstuck = false;
		}
		if (o->unstuck) {
			o->unstuckSec += dt;
			unstuckGoal(f, route, o, goal);
			if (unstuckDone(o, goal) || unstuckFailed(f, o) ||
					o->unstuckSec > UNSTUCK_LIFETIME) {
				aidriver_clear_steering(&o->drv, &o->sim);
				o->unstuck = false;
				// PORT: and drives on from wherever it backed out to.
				chooseHere(f, o, lineLat);
			}
		} else if (racing && !home && !o->finished && unstuckWanted(f, i, o)) {
			o->unstuck = true;
			o->unstuckSec = 0.0f;
			o->sinceUnstuck = 0.0f;    // JustCalledGetUnstuckAction
			unstuckGoal(f, route, o, goal);
		}

		// ChangeCurrentL: the drive target walks toward the choice as the car
		// runs down the road, and is never off it -- retail builds its drive
		// target afresh each think from where the car is, keeping every point
		// on the road, so it holds nothing past an edge. Kept there, an offset
		// a car alongside had sent past the road's edge stood 7 m out still as
		// the road bent the other way, and steered 4104B's Eric (424 m) into
		// the wall when the choice came back (TASKS.md #280). Then the choice
		// itself, unless the car is backing out or home.
		float follow = 1e9f;
		if (racing && !o->unstuck && !home) forkStep(f, i, o);
		if (racing) {
			const float run = o->alongV > 0.0f ? o->alongV * dt : 0.0f;
			float lo, hi;
			o->curL = toward(o->curL, choiceL(f, o, o->lineDist), o->latRate * run);
			if (ownRoadSpan(f, o, o->lineDist, &lo, &hi))
				o->curL = keepOnRoad(o->curL, lo, hi);
			o->choiceSec += dt;
			o->avoidSec += dt;
			// ChooseNewActionAsRacer: GetUnstuck, then IJustGotHit, then
			// AvoidAvoidable. A drag's racers are AIGoalWinDragRace's, and a
			// finished one's AIGoalPark's.
			// The action's MaybeUseNitrous side (o->nosAction): only the
			// driving actions call it -- not GetUnstuck, AIGoalPark's,
			// IJustGotHit or the drag's two hit actions.
			o->nosAction = false;
			if (o->unstuck || home) {
				o->avoiding = false;
				o->accident = false;
			} else if (o->finished) {
				stopChoice(f, route, o, player);
				o->avoiding = false;
			} else if (f->lanes.lanes)
				follow = dragChoice(f, route, i, o, player);
			else if (opponent_accident_step(o, dt)) {
				// AccidentRecovery (opponent.h): every speed 0. PORT: asked
				// before IJustGotHit and AvoidAvoidable, which cannot take over
				// from it once it runs.
				o->avoiding = false;
				follow = 0.0f;
			} else if (gotHit(f, route, i, o, hits, player, &follow))
				o->avoiding = false;
			else {
				follow = driveChoice(f, route, i, o, player);
				o->nosAction = true;
			}
			// The lane's lateral here, which the cars round it plan on.
			o->goalL = choiceL(f, o, o->lineDist);
		}

		float pt[2];
		float planLat = lineOffset(f, o, o->lineDist);
		if (o->unstuck) {
			// CalculatePointsToDrive: the drive target is the goal alone.
			pt[0] = goal[0];
			pt[1] = goal[1];
		} else if (home && pullOver(f, route, o, aidriver_lookahead(&o->drv, &o->sim, o->sim.speed),
				pt, &planLat)) {
		} else {
			// CalcSteerToPoint: the point on the line, that far ahead.
			float ahead = aidriver_lookahead(&o->drv, &o->sim, o->sim.speed);
			const float shortBy = shortOfStart(f, route, o);
			ahead = chordAhead(f, route, o, ahead > shortBy ? ahead - shortBy : 0.0f);
			if (!drivePoint(f, route, o, lineAhead(f, o, ahead), pt)) {
				// Past the end of a sprint, or a gap in the splines: aim
				// straight on, which is SteerStraightVelocity3D's answer.
				pt[0] = o->sim.pos[0] + o->sim.rot[0][0] * ahead;
				pt[1] = o->sim.pos[1] + o->sim.rot[0][1] * ahead;
			}
		}
		o->drv.steerTo[0] = pt[0];
		o->drv.steerTo[1] = pt[1];

		// DriveTarget::GetLateralOffset: how far off its own line the car is,
		// positive to the right of travel.
		o->drv.lateral_m = lineLat - planLat;

		// SetDesiredSpeedAsRacer (0x8000B7D0): every switch interval the
		// driver re-rolls where up the road it reads its speed limit from,
		// uniformly within +/- its skill's own metres. That is the whole of
		// why a weak AI corners badly -- it is reading the right limit for
		// the wrong piece of road, so it brakes late for one bend and crawls
		// through the next. Only while racing: the roll is a race behaviour
		// and the grid should not drift during the countdown.
		if (racing) {
			o->lookaheadSec += dt;
			if (o->lookaheadSec > difficulty_get()->lookaheadSwitchSec) {
				o->lookaheadSec = 0.0f;
				float half = difficulty_field(o->skill, DIFF_LOOKAHEAD_JITTER);
				o->lookahead = difficulty_random(&o->rng, half + half) - half;
			}
		}

		// The route's limit there, as much of it as this driver's skill is
		// good for, and never past what the gearing gives. Backing out, it is
		// GetUnstuck's own speed instead, which Drive reads as reverse.
		// Street X's branch leaves the lookahead out (0x8000B8E8).
		const float ahead = f->shortTrack ? 0.0f : o->lookahead;
		float at = lineAhead(f, o, ahead);
		if (at < 0.0f) at = 0.0f;
		float limit = o->prof ? aispeed_at(o->prof, at) : o->topSpeed;
		// PORT (TASKS.md #280): read behind the car, no more than the limit
		// where it is. Retail keeps a racer's FuturePoints behind it
		// (RaceCarController::KeepFuturePointsBehindMe), so it does read the
		// road it has passed, but it plans its corners on its class's friction
		// (aidriver.c's kClassFriction, 2.8 g and up) and the port on what the
		// tyres hold, with no grip to spare for a late brake: a skill-6 racer
		// reading 14 m back took 4042B's 2,440 m left-hander, planned at
		// 16 m/s, at 21 - 24, ran 3.7 m wide into the nose of the island the
		// line passes, and its follower stopped behind it. Reading ahead still
		// crawls through the next bend as retail's does.
		if (o->prof && ahead < 0.0f) {
			const float here = aispeed_at(o->prof, o->lineDist > 0.0f ? o->lineDist : 0.0f);
			if (limit > here) limit = here;
		}
		limit = forkLimit(f, o, o->prof, limit);
		// DecideBoostFactor (TASKS.md #255), which ExecuteCurrentAction runs
		// before the action sets its speed: +0x778 on the speed wanted, and
		// Car +0x478 = max(boost, 1) on the physics.
		updateBoost(f, i, dt);
		float want = limit * o->skillScale * o->boost;
		if (want > o->topSpeed) want = o->topSpeed;
		if (want > follow) want = follow;
		if (o->unstuck) want = UNSTUCK_DESIRED;
		if (home || o->finished) want = 0.0f;   // AIGoalPark
		o->desired = want;
		o->drv.desired = want;
		o->drv.racing = racing;

		// Think's mover, which it sees to before the driving (0x80028198 on):
		// a finished racer goes onto the PhysicsMover; otherwise, the frame
		// the timer runs out the car goes onto its default mover if all four
		// wheels are down, or waits another second, and while it runs the
		// car is held in mode 1. The countdown keeps the timer full.
		if (racing) {
			if (o->finished || home) {
				setMover(f, o, false);
			} else if (o->physSec > 0.0f) {
				o->physSec -= dt;
				if (o->physSec > 0.0f)
					setMover(f, o, false);
				else if (o->sim.wheelsDown < CARPHYS_WHEELS)
					o->physSec = PHYS_REARM_SEC;
				else {
					o->physSec = 0.0f;
					setMover(f, o, !f->physicsMover);
				}
			}
		}

		CarSimInput in;
		if (f->drift) {
			// DriftDriver::Drive (#268 E step 5): the drive target's points
			// at its two distances up the line from the car -- the goal alone
			// while backing out, the pull-over point once home -- and the
			// speed capped at 70 mph (SetDesiredSpeed).
			float p1[2], p2[2];
			bool have = true;
			if (o->unstuck || home) {
				p1[0] = p2[0] = pt[0];
				p1[1] = p2[1] = pt[1];
			} else {
				float d1, d2;
				aidrift_ahead(&o->sim, o->drv.lateral_m, &d1, &d2);
				have = drivePoint(f, route, o, lineAhead(f, o, d1), p1) &&
					drivePoint(f, route, o, lineAhead(f, o, d2), p2);
			}
			aidrift_set_desired(&o->drift, want);
			o->drift.mode = racing ? 0 : 1;   // SetStartLineState
			aidrift_drive(&o->drift, &o->sim, dt, have ? p1 : NULL, have ? p2 : NULL,
				!o->sim.mellow, &in);
			o->drv.steerU16 = o->drift.steerU16;
		} else {
			aidriver_drive(&o->drv, &o->sim, dt, &in);
			// Its nitrous (TASKS.md #281), which Think and the action see to
			// before the car moves. A drift's DriftDriver has none.
			if (racing) updateNitrous(f, route, i, dt);
		}
		o->lastThr = in.throttle;
		o->lastBrk = in.brake;
		float deg = (float)in.steerAngleU16 * (360.0f / 65536.0f);
		{
			// The telemetry's chatter count (opponent.h steerFlips).
			const float was = o->lastSteerDeg;
			o->lastSteerDeg = deg > 180.0f ? deg - 360.0f : deg;
			if ((was > STEER_FLIP_DEG && o->lastSteerDeg < -STEER_FLIP_DEG) ||
					(was < -STEER_FLIP_DEG && o->lastSteerDeg > STEER_FLIP_DEG)) {
				if (o->steerFlips < 0xFFFF) o->steerFlips++;
				if (o->sim.mellow) o->flipMellow = true;
			}
		}
		// RaceStarter keeps the grid still until GO. The player's car is not
		// stepped during the countdown either (main.c's `held`), and an AI
		// car that is stepped there creeps off the line however hard its
		// driver holds the car, because the sim settles it onto its springs.
		//
		// A drag's is stepped, so its engine is revving at GO: PsychoDriver::
		// Drive at race stage 3 holds the brake at 1, the gear in neutral
		// (+0x1C8 = 1) and DoStartLineThrottlePlay (0x80021B04) puts the
		// throttle at 1. PORT: then back on its spot (carsim_hold_xy). #511:
		// the other races' grids rev the same way, through their countdown
		// (their cars are not stepped before it: the intro scene places them).
		if (!racing && (f->drag || f->countdown)) {
			in.throttle = 1.0f;
			in.brake = 1.0f;
			in.handbrake = false;
			in.gear = 1;
			// SetThrottle on the CarDriver itself (DAT_80317700 = 1): the
			// AI engine sound (CarDriver +0x1F0, raceAiSfxIn) hears the revs.
			o->lastThr = in.throttle;
			o->lastBrk = in.brake;
			carsim_update(&o->sim, &in, cw, dt);
			carsim_hold_xy(&o->sim, xy);
			continue;
		}
		if (!racing)
			continue;
		updateNosFill(f, route, o);   // RacingCar::Update's refill
		const float was[3] = { o->sim.vel[0], o->sim.vel[1], o->sim.vel[2] };
		carsim_update(&o->sim, &in, cw, dt);
		if (dt > 0.0f) {
			float a = 0.0f;
			for (int k = 0; k < 3; k++) a += (o->sim.vel[k] - was[k]) * o->sim.rot[0][k];
			o->fwdAccel = a / dt;
		}

		// The height a reset can trust without a line: all four wheels down,
		// near the route point.
		bool level = true;
		for (int k = 0; k < CARPHYS_WHEELS; k++)
			if (!o->sim.wheel[k].onGround) level = false;
		const float dx = xy[0] - o->coord.onPath[0], dy = xy[1] - o->coord.onPath[1];
		if (level && fabsf(dx * o->coord.dir[1] - dy * o->coord.dir[0]) <= ROAD_Z_LATERAL) {
			o->roadZ = o->sim.pos[2];
			o->roadDist = o->coord.dist;
		}

		// Think's very-stuck test: the speed along the route every 1.5 s,
		// and a full ten samples (15 s) averaging under 0.2 m/s with no
		// player within 50 m is PleaseResetMeLater. PORT: the ground made
		// along the line over the 1.5 s, see opponent.h.
		bool reset = o->sim.resetRequest || o->accidentReset;
		if (o->accidentReset && f->log) {
			char b[128];
			snprintf(b, sizeof(b), "opponent %d (%s): AccidentRecovery -- PleaseResetMeLater", i,
				o->name);
			f->log(b);
		}
		o->accidentReset = false;
		o->progressSec += dt;
		if (o->progressSec >= STUCK_SAMPLE_SEC) {
			const float gain = wrapGap(route, o->lineDist - o->progressAt);
			o->progressAt = o->lineDist;
			const float v = gain / o->progressSec;
			o->progressSec = 0.0f;
			o->progress[o->progressIdx] = v;
			o->progressIdx = (o->progressIdx + 1) % OPPONENT_PROGRESS_N;
			if (o->progressN < OPPONENT_PROGRESS_N) o->progressN++;
			float sum = 0.0f;
			for (int k = 0; k < o->progressN; k++) sum += o->progress[k];
			float near = 1e9f;
			if (player) {
				float px = player->pos[0] - o->sim.pos[0];
				float py = player->pos[1] - o->sim.pos[1];
				float pz = player->pos[2] - o->sim.pos[2];
				near = sqrtf(px * px + py * py + pz * pz);
			}
			// Not a finished car: it is parked on AIGoalPark, standing still on
			// purpose. Reset as stuck it blinked again each time its blink ran
			// out, and from the race's end on (the blink runs down only while
			// racing) for good -- the field flickering under the post-race
			// screens and the end NIS (2026-11-05, an AI-won drag).
			if (!home && !o->finished && !opponent_blinking(o) &&
					o->progressN == OPPONENT_PROGRESS_N &&
					sum / (float)o->progressN < STUCK_AVG_SPEED &&
					near >= STUCK_PLAYER_DIST)
				reset = true;
		}
		AiLineCrossing face;
		if (reset)
			resetToRoute(f, route, o, cw);
		else if (!home && ailine_face_ahead(f->line, o->lineDist, HOP_REACH, &face))
			hopFace(f, route, o, cw, &face);
	}
}

float opponent_distance(const OpponentField* f, int i) {
	if (!f || i < 0 || i >= f->count || !f->car[i].driving) return 0.0f;
	return f->car[i].coord.dist;
}

void opponent_free(OpponentField* f) {
	if (!f) return;
	if (s_collField == f) {
		carsim_remove_impulse_hook(onCollImpulse);
		s_collField = NULL;
		s_collPlayer = NULL;
	}
	for (int k = 0; k < OPPONENT_MAX; k++) {
		aispeed_free(f->prof[k]);
		f->prof[k] = NULL;
	}
	trackroute_lanes_free((TrackLanes*)f->lanes.lanes);
	f->lanes.lanes = NULL;
	trackroute_forks_free(f->forks);
	f->forks = NULL;
	ailine_free(f->line);
	f->line = NULL;
	trackroute_close(f->routes);
	f->routes = NULL;
	for (int i = 0; i < f->count; i++) {
		if (f->car[i].ownsModel)
			model3d_free(f->car[i].model);
		f->car[i].model = NULL;
		f->car[i].ownsModel = false;
	}
	memset(f, 0, sizeof(*f));
}
