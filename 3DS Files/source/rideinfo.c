#include "rideinfo.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// See rideinfo.h. The tables are Speed.elf's; SPEED2.EXE's RandomPartUpgrades
// (file offset 0x402C50) is byte-identical, and the paint and vinyl colour
// jobs are names both executables carry. tools/randomride_check.py holds the
// same tables and checks every name against the PC parts database.

// ------------------------------------------------------------------ tables

// RandomPartUpgrades (0x80371694, 14 entries): a slot, whether a draw of 4
// gives it the stock part (else nothing), and per upgrade level 0..3 the
// chances, in percent, of a level-0..3 part.
typedef struct {
	u8 slot;
	u8 stock;
	u8 chance[4][4];
} RandomPartUpgrade;

#define CHANCE_STD   { { 60, 30, 10, 0 }, { 20, 60, 20, 0 }, { 0, 20, 60, 20 }, { 0, 10, 20, 70 } }
#define CHANCE_PAINT { { 0, 70, 30, 0 }, { 0, 70, 20, 10 }, { 0, 20, 60, 20 }, { 0, 10, 20, 70 } }

static const RandomPartUpgrade kRandomPartUpgrades[] = {
	{ RIDE_SLOT_HOOD, 1, CHANCE_STD },
	{ RIDE_SLOT_ROOF, 1, CHANCE_STD },
	{ RIDE_SLOT_SPOILER, 1, CHANCE_STD },
	{ RIDE_SLOT_EXHAUST, 1, CHANCE_STD },
	{ 0x20, 1, CHANCE_STD },   // WING_MIRROR
	{ 0x0F, 1, CHANCE_STD },   // BRAKELIGHT
	{ 0x0E, 1, CHANCE_STD },   // HEADLIGHT
	{ 0x9F, 1, CHANCE_STD },   // HEADLIGHT_BULB
	{ 0x99, 0, CHANCE_STD },   // WINDOW_TINT
	{ RIDE_SLOT_BASE_PAINT, 1, CHANCE_PAINT },
	{ 0x49, 0, { { 70, 20, 10, 0 }, { 50, 30, 20, 0 }, { 20, 10, 60, 10 }, { 0, 10, 20, 70 } } },
	{ 0x4A, 0, CHANCE_PAINT }, // PAINT_SPINNER
	{ 0x9A, 0, { { 90, 10, 0, 0 }, { 40, 50, 10, 0 }, { 30, 20, 50, 0 }, { 30, 10, 30, 30 } } },
	{ RIDE_SLOT_FRONT_BUMPER, 0, CHANCE_STD },
};
#define NUM_RANDOM_PART_UPGRADES ((int)(sizeof(kRandomPartUpgrades) / sizeof(kRandomPartUpgrades[0])))
// BuildRandomSkin's base paint is the BASE_PAINT entry (0x80371790).
#define BASE_PAINT_ENTRY 9

// BuildRandomRide's wheel chances (0x8033CBB9), by the tyre level.
static const u8 kWheelLevels[4][4] = {
	{ 70, 20, 10, 0 }, { 50, 40, 10, 0 }, { 10, 20, 50, 20 }, { 0, 10, 20, 70 },
};

// BuildRandomSkin's tables (0x8033CBDC..0x8033CC64), by the mean level.
static const int kFullVinylChance[4] = { 0, 0, 5, 10 };
static const u8 kVinylL0Levels[4][4] = {
	{ 95, 0, 5, 0 }, { 90, 0, 10, 0 }, { 80, 0, 20, 0 }, { 65, 0, 35, 0 },
};
static const u8 kVinylL1Levels[4][4] = {
	{ 80, 10, 10, 0 }, { 70, 15, 10, 5 }, { 55, 25, 15, 5 }, { 40, 20, 20, 20 },
};
static const u8 kVinylL2Levels[4][4] = {
	{ 95, 0, 0, 5 }, { 90, 0, 0, 10 }, { 80, 0, 0, 20 }, { 65, 0, 0, 35 },
};
// The vinyl groups (a vinyl part's kit field) each layer draws from.
static const int kFullVinylGroups[] = { 0x16 };
static const int kVinylL0Groups[] = { 0x17 };
static const int kVinylL1Groups[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0x10, 0x11, 0x12, 0x13, 0x14,
};
static const int kVinylL2Groups[] = { 0x19 };
#define COUNT(a) ((int)(sizeof(a) / sizeof(a[0])))

// RandomCarPaintJobs (0x8040B154) and RandomVinylPaintJobs (0x8040B21C), built
// by the static initializer at 0x80254C6C out of these names.
typedef struct {
	const char* paint;
	const char* accent[5];
} RandomCarPaintJob;

static const RandomCarPaintJob kRandomCarPaintJobs[] = {
	{ "GLOSS_L1_COLOR02", { "GLOSS_L1_COLOR02", "GLOSS_L1_COLOR07", "GLOSS_L2_COLOR02",
		"GLOSS_L2_COLOR07", "GLOSS_L2_COLOR13" } },
	{ "GLOSS_L1_COLOR03", { "GLOSS_L1_COLOR03", "GLOSS_L1_COLOR02", "GLOSS_L2_COLOR01",
		"GLOSS_L2_COLOR06", "GLOSS_L3_TEST07" } },
	{ "GLOSS_L2_COLOR01", { "GLOSS_L2_COLOR01", "GLOSS_L2_COLOR02", "GLOSS_L2_COLOR07",
		"GLOSS_L2_COLOR11", "GLOSS_L2_COLOR03" } },
	{ "GLOSS_L2_COLOR17", { "GLOSS_L2_COLOR17", "GLOSS_L1_COLOR02", "GLOSS_L1_COLOR04",
		"GLOSS_L2_COLOR01", "GLOSS_L2_COLOR07" } },
	{ "METAL_L2_COLOR01", { "METAL_L2_COLOR01", "GLOSS_L1_COLOR02", "GLOSS_L1_COLOR07",
		"METAL_L2_COLOR05", "METAL_L3_TEST04" } },
};

static const char* const kRandomVinylPaintJobs[5][4] = {
	{ "VINYL_L1_COLOR01", "VINYL_L1_COLOR02", "VINYL_L2_COLOR11", "VINYL_L1_COLOR09" },
	{ "VINYL_L1_COLOR01", "VINYL_L1_COLOR02", "VINYL_L1_COLOR03", "VINYL_L1_COLOR04" },
	{ "VINYL_L1_COLOR02", "VINYL_L1_COLOR03", "VINYL_L1_COLOR04", "VINYL_L1_COLOR05" },
	{ "VINYL_L1_COLOR02", "VINYL_L3_COLOR12", "VINYL_L3_COLOR13", "VINYL_L3_COLOR14" },
	{ "VINYL_L1_COLOR02", "VINYL_L3_COLOR18", "VINYL_L3_COLOR19", "VINYL_L3_COLOR20" },
};

// SetStockParts' vinyl colours, the same four on every layer.
static const char* const kStockVinylColours[4] = {
	"VINYL_L1_COLOR09", "VINYL_L1_COLOR02", "VINYL_L2_COLOR11", "VINYL_L1_COLOR01",
};

// The widebody's door pieces, UpdatePartsEnabled's names after
// "<CAR>_KITW0<n>_DOOR_", and its four decal layouts after "<CAR>_WIDE<n>_".
static const char* const kWideDoors[6] = {
	"LEFT", "RIGHT", "PANEL_LEFT", "PANEL_RIGHT", "SILL_LEFT", "SILL_RIGHT",
};
static const char* const kWideDecals[4] = {
	"DECAL_LEFT_DOOR_RECT_MEDIUM", "DECAL_RIGHT_DOOR_RECT_MEDIUM",
	"DECAL_LEFT_QUARTER_RECT_MEDIUM", "DECAL_RIGHT_QUARTER_RECT_MEDIUM",
};

#define KEY_HOODUNDER    0x0D4B85C7u
#define KEY_OUTER_RADIUS 0xCE7D8DB5u
#define KEY_INNER_RADIUS 0xEB0101E2u

// MatchUpgradeLevel's tolerance (0x80332B24), and a bound on its search. The
// search ends in retail once no part is left to try, unless a part's
// requirement is missing, when GetNextAvailablePart hands the same part back
// and it never would; the bound only matters there.
#define MATCH_TOLERANCE 0.05f
#define MATCH_MAX_TRIES 256

// ------------------------------------------------------------------ bRandom
//
// bRandom(int) (0x80059160) is bRandom(int, u32*) (0x800590C4) on the global
// state at r13-0x7D38 (0x8046D0A8). The generator is a two-tap xorshift with a
// fixed pre-whitening constant; the value it RETURNS is the state it came in
// with, modulo n, and the state is advanced afterwards.
u32 rideinfo_random(u32* state, u32 n) {
	const u32 s = *state;
	u32 x = s ^ 0x1D872B41u;
	u32 y = x ^ (x >> 5);
	x ^= (y << 27);
	y ^= x;
	*state = y;
	return n ? s % n : 0;
}

// GetRandomLevel (0x80247040): 0..3, or 4 when the chances run out.
static int randomLevel(u32* rng, const u8 chance[4]) {
	const int r = (int)rideinfo_random(rng, 100);
	int i = 0, acc = chance[0];
	while (acc <= r && ++i < 4)
		acc += chance[i];
	return i;
}

// ------------------------------------------------------------------ parts

static CarPart get(const RideInfo* r, int slot, u32 hash, CarPart prev, int level) {
	return carpartdb_get(r->db, r->car->nameHash, slot, hash, prev, level);
}

// UpdatePartsEnabled (0x80244EE8), for the parts it sets: the doors after the
// WIDE_BODY part, and the hood's underside after the hood. The enable flags
// it also keeps are for the garage's menus.
static void updateDerived(RideInfo* r) {
	CarPart wide = r->part[RIDE_SLOT_WIDE_BODY];
	if (!wide || carpartdb_level(wide) == 0) {
		for (int k = 0; k < 6; k++)
			r->part[RIDE_SLOT_DOOR_FIRST + k] = get(r, RIDE_SLOT_DOOR_FIRST + k, 0, NULL,
				CARPARTDB_ANY_LEVEL);
	} else {
		const int kit = carpartdb_kit(wide);
		char name[64];
		snprintf(name, sizeof(name), "%s_WIDE%d_", r->car->name, kit);
		u32 seed = carpartdb_string_hash(name);
		for (int k = 0; k < 4; k++)
			r->part[RIDE_SLOT_WIDE_DECAL + k] = get(r, RIDE_SLOT_WIDE_DECAL + k,
				carpartdb_string_hash_from(kWideDecals[k], seed), NULL, CARPARTDB_ANY_LEVEL);
		snprintf(name, sizeof(name), "%s_KITW0%d_DOOR_", r->car->name, kit);
		seed = carpartdb_string_hash(name);
		for (int k = 0; k < 6; k++)
			r->part[RIDE_SLOT_DOOR_FIRST + k] = get(r, RIDE_SLOT_DOOR_FIRST + k,
				carpartdb_string_hash_from(kWideDoors[k], seed), NULL, CARPARTDB_ANY_LEVEL);
	}
	CarPart hood = r->part[RIDE_SLOT_HOOD];
	if (hood)
		r->part[RIDE_SLOT_HOOD_UNDER] = get(r, RIDE_SLOT_HOOD_UNDER,
			carpartdb_attr(r->db, hood, KEY_HOODUNDER, 0), NULL, CARPARTDB_ANY_LEVEL);
}

void rideinfo_init(RideInfo* r, const CarPartDb* db, const CarRec* car) {
	memset(r, 0, sizeof(*r));
	r->db = db;
	r->car = car;
}

void rideinfo_set_part(RideInfo* r, int slot, CarPart p) {
	if (slot < 0 || slot >= CARPARTDB_SLOTS || (slot >= 0x61 && slot <= 0x68) ||
			slot == RIDE_SLOT_HOOD_UNDER)
		return;
	r->part[slot] = p;
	updateDerived(r);
}

// SetUpgradePart (0x80244728): only when the car has a part of that level.
static void setUpgradePart(RideInfo* r, int slot, int level) {
	CarPart p = carpartdb_find_with_level(r->db, r->car->nameHash, slot, level);
	if (p)
		rideinfo_set_part(r, slot, p);
}

void rideinfo_set_stock_parts(RideInfo* r) {
	for (int i = 0; i < CARPARTDB_SLOTS; i++) {
		if ((i >= RIDE_SLOT_VINYL_LAYER0 && i <= RIDE_SLOT_VINYL_LAYER0 + 3) ||
				(i >= 0x61 && i <= 0x98))
			continue;
		if (i == RIDE_SLOT_BASE_PAINT) {
			CarPart p = get(r, i, r->car->stockPaint, NULL, CARPARTDB_ANY_LEVEL);
			if (p)
				rideinfo_set_part(r, i, p);
		} else {
			setUpgradePart(r, i, 0);
		}
	}
	for (int layer = 0; layer < 4; layer++) {
		for (int j = 0; j < 4; j++) {
			const int s = RIDE_SLOT_VINYL_COLOUR + layer * 4 + j;
			rideinfo_set_part(r, s, get(r, s, carpartdb_string_hash(kStockVinylColours[j]),
				NULL, CARPARTDB_ANY_LEVEL));
		}
	}
}

// GetRandomPart (0x80244994).
static CarPart randomPart(const RideInfo* r, int slot, int level, u32* rng) {
	int n = 0;
	for (CarPart p = get(r, slot, 0, NULL, level); p; p = get(r, slot, 0, p, level))
		n++;
	if (!n)
		return NULL;
	const int k = (int)rideinfo_random(rng, (u32)n);
	CarPart p = NULL;
	for (int i = 0; i <= k; i++)
		p = get(r, slot, 0, p, level);
	return p;
}

void rideinfo_set_random_paint(RideInfo* r, u32* rng) {
	CarPart p = randomPart(r, RIDE_SLOT_BASE_PAINT, CARPARTDB_ANY_LEVEL, rng);
	if (!p)
		return;
	rideinfo_set_part(r, RIDE_SLOT_BASE_PAINT, p);
	for (int k = 0; k < 4; k++)
		rideinfo_set_part(r, RIDE_SLOT_VINYL_LAYER0 + k, NULL);
}

void rideinfo_set_random_part(RideInfo* r, int slot, int level, u32* rng) {
	rideinfo_set_part(r, slot, randomPart(r, slot, level, rng));
}

// GetRandomWheel (0x80246D94): one of the first 128 rims of `level` that fit
// the car's hubs.
static CarPart randomWheel(const RideInfo* r, int level, u32* rng) {
	static CarPart fits[0x80];
	int n = 0;
	for (CarPart p = get(r, RIDE_SLOT_FRONT_WHEEL, 0, NULL, level); p && n < 0x80;
			p = get(r, RIDE_SLOT_FRONT_WHEEL, 0, p, level)) {
		const s8 inner = (s8)carpartdb_attr(r->db, p, KEY_INNER_RADIUS, 0);
		const s8 outer = (s8)carpartdb_attr(r->db, p, KEY_OUTER_RADIUS, 0);
		if (outer == r->car->wheelOuter && r->car->wheelInnerMin <= inner &&
				inner <= r->car->wheelInnerMax)
			fits[n++] = p;
	}
	return n ? fits[rideinfo_random(rng, (u32)n)] : NULL;
}

// GetRandomVinyl (0x80246ED4): one of the first 512 vinyls of `level` whose
// group (the kit field) is one of `groups`.
static CarPart randomVinyl(const RideInfo* r, int level, const int* groups, int numGroups,
		u32* rng) {
	static CarPart pool[0x200];
	int n = 0;
	for (CarPart p = get(r, RIDE_SLOT_VINYL_LAYER0, 0, NULL, level); p && n != 0x200;
			p = get(r, RIDE_SLOT_VINYL_LAYER0, 0, p, level)) {
		for (int g = 0; g < numGroups && n < 0x200; g++)
			if (carpartdb_kit(p) == groups[g])
				pool[n++] = p;
	}
	return n ? pool[rideinfo_random(rng, (u32)n)] : NULL;
}

// ----------------------------------------------------------------- levels

int rideinfo_upgrade_level(const RideSpec* s, int path) {
	return (int)(s->norm[path] + 0.5f);
}

float rideinfo_normalized_level(const RideSpec* s, int path) {
	return s->norm[path] / 3.0f;
}

void rideinfo_compute_levels(RideSpec* s, const PerfDb* pdb) {
	for (int p = 0; p < RIDEINFO_PATHS; p++)
		s->norm[p] = perfdb_upgrade_value(pdb, s->inst, p);
}

// ComputeUpgradeLevel (0x801C466C): stores the path's level in `s` and
// returns it on the 0..1 scale.
static float computeLevel(RideSpec* s, const PerfDb* pdb, int path) {
	float comp[PERFDB_UPGRADE_COMPONENTS];
	const float avg = perfdb_upgrade_components(pdb, s->inst, path, comp);
	s->norm[path] = perfdb_upgrade_value(pdb, s->inst, path);
	return avg;
}

static bool partInstalled(const RideSpec* s, const PerfPackagePart* p) {
	return p->partId < PERFDB_NUM_PARTS && s->inst[p->partId];
}

// What GetNextAvailablePart ranks a part by: the mean of its upgrade values,
// taken over as many leading entries as it has entries that are not -1.
static float partMean(const PerfPackagePart* p) {
	int n = 0;
	for (int k = 0; k < PERFDB_MAX_PART_UPGRADES; k++)
		if (p->upgrade[k] != -1.0f)
			n++;
	float sum = 0.0f;
	for (int k = 0; k < n; k++)
		sum += p->upgrade[k];
	return sum / (float)n;
}

// GetNextAvailablePart (0x801C3D38): of the parts not yet in, across the
// packages of `path` at `level` (1..3 when 0), the one with the lowest mean
// or the highest, the first on a tie.
static const PerfPackagePart* nextAvailablePart(const RideSpec* s, const PerfDb* pdb,
		int level, int path, bool preferLower) {
	const PerfPackagePart* best = NULL;
	const int lo = level ? level : PERFDB_LEVEL_MIN, hi = level ? level : PERFDB_LEVEL_MAX;
	for (int l = lo; l <= hi; l++) {
		const PerformancePackage* pkg = perfdb_package(pdb, path, l);
		for (u32 j = 0; pkg && j < pkg->numParts && j < PERFDB_MAX_PARTS; j++) {
			const PerfPackagePart* p = &pkg->part[j];
			if (partInstalled(s, p))
				continue;
			if (best) {
				const float a = partMean(p), b = partMean(best);
				if (preferLower ? !(a < b) : a <= b)
					continue;
			}
			best = p;
		}
	}
	return best;
}

// InstallAllPerfParts (0x801C45B8): the path's parts out, then every part of
// its level-1..`level` packages in.
static void installAllPerfParts(RideSpec* s, const PerfDb* pdb, int level, int path) {
	perfdb_reset_to_stock(pdb, s->inst, path);
	for (int l = PERFDB_LEVEL_MIN; l <= level; l++) {
		const PerformancePackage* pkg = perfdb_package(pdb, path, l);
		for (u32 j = 0; pkg && j < pkg->numParts && j < PERFDB_MAX_PARTS; j++)
			perfdb_install_part(pdb, s->inst, (int)pkg->part[j].partId, true, false);
	}
}

// IsPerfPackageInstalled (0x801C42B4): every part of it in, or no package.
static bool packageInstalled(const RideSpec* s, const PerfDb* pdb, int type, int level) {
	const PerformancePackage* pkg = perfdb_package(pdb, type, level);
	for (u32 j = 0; pkg && j < pkg->numParts && j < PERFDB_MAX_PARTS; j++)
		if (!partInstalled(s, &pkg->part[j]))
			return false;
	return true;
}

float rideinfo_match_upgrade_level(RideSpec* s, const PerfDb* pdb, float level,
		const int* paths, int numPaths) {
	if (!s || !pdb)
		return 0.0f;
	if (numPaths <= 0)
		numPaths = RIDEINFO_PATHS;
	float total = 0.0f;
	for (int i = 0; i < numPaths; i++) {
		const int path = paths ? paths[i] : i;
		perfdb_reset_to_stock(pdb, s->inst, path);
		// `work` is where parts are tried, `best` the closest so far. Both are
		// copied before the stock level is stored, so a path that is already
		// close enough keeps the level it had -- retail's order.
		RideSpec work = *s, best = *s;
		float cur = computeLevel(s, pdb, path);
		bool done = fabsf(cur - level) < MATCH_TOLERANCE;
		int maxLevel = (int)((double)(level * 3.0f) + 0.5) + 1;
		if (maxLevel > PERFDB_LEVEL_MAX)
			maxLevel = PERFDB_LEVEL_MAX;
		int tries = 0;
		for (int l = PERFDB_LEVEL_MIN; l <= maxLevel && !done; l++) {
			// Three passes: the highest-ranked parts first, then the lowest,
			// then the two alternately.
			for (int pass = 0; pass < 3 && !done; pass++) {
				bool lower = pass != 0;
				const PerfPackagePart* p = nextAvailablePart(&work, pdb, l, path, lower);
				while (p && !done && tries++ < MATCH_MAX_TRIES) {
					if (perfdb_install_part(pdb, work.inst, (int)p->partId, true, false)) {
						const float d = computeLevel(&work, pdb, path);
						if (fabsf(d - level) < fabsf(cur - level)) {
							best = work;
							done = fabsf(d - level) < MATCH_TOLERANCE;
							cur = d;
						}
					}
					if (pass == 2)
						lower = !lower;
					p = nextAvailablePart(&work, pdb, l, path, lower);
				}
				work = *s;
			}
			if (!done && l < maxLevel) {
				installAllPerfParts(s, pdb, l, path);
				work = *s;
			}
		}
		*s = best;
		total += computeLevel(&best, pdb, path);
	}
	return total / (float)numPaths;
}

int rideinfo_fill_with_preset(RideInfo* r, const CarPartDb* db, const CarRec* car,
		const PresetCar* p, const PerfDb* pdb) {
	if (!r || !car || !p)
		return 0;
	int missing = 0;
	if (r->car != car) {
		rideinfo_init(r, db, car);
		if (db)
			rideinfo_set_stock_parts(r);
	}
	for (int slot = 0; db && slot < CARPARTDB_SLOTS; slot++) {
		const u32 h = p->parts[slot + 1];
		if (h == 0) {
			rideinfo_set_part(r, slot, NULL);
		} else if (h != 1) {
			CarPart part = get(r, slot, h, NULL, CARPARTDB_ANY_LEVEL);
			missing += !part;
			rideinfo_set_part(r, slot, part);
		}
		// Whatever the preset names, the convertible top is the car's own.
		if (slot == RIDE_SLOT_TOP)
			rideinfo_set_part(r, slot, get(r, slot, 0, NULL, CARPARTDB_ANY_LEVEL));
	}
	if (!pdb)
		return missing;
	RideSpec* s = &r->spec;
	if (p->perfPackage == -1) {
		// InstallPerfPart(part, byte != 0, false) for every byte but -1.
		for (int i = 0; i < PRESETCAR_PERF_PARTS && i < PERFDB_NUM_PARTS; i++)
			if (p->perfParts[i] != -1)
				perfdb_install_part(pdb, s->inst, i, p->perfParts[i] != 0, false);
	} else {
		// InstallAllPerfParts(package, -1) (0x801C45B8): every path out, then
		// each level 1..package, every path's package of that level in.
		for (int path = 0; path < RIDEINFO_PATHS; path++)
			perfdb_reset_to_stock(pdb, s->inst, path);
		for (int l = PERFDB_LEVEL_MIN; l <= p->perfPackage && l <= PERFDB_LEVEL_MAX; l++)
			for (int path = 0; path < RIDEINFO_PATHS; path++) {
				const PerformancePackage* pkg = perfdb_package(pdb, path, l);
				for (u32 j = 0; pkg && j < pkg->numParts && j < PERFDB_MAX_PARTS; j++)
					perfdb_install_part(pdb, s->inst, (int)pkg->part[j].partId, true, false);
			}
	}
	rideinfo_compute_levels(s, pdb);
	return missing;
}

// ------------------------------------------------------------- the dressing

void rideinfo_build_random_ride(RideInfo* r, u32* rng) {
	int lv[RIDEINFO_PATHS];
	float sum = 0.0f;
	for (int p = 0; p < RIDEINFO_PATHS; p++) {
		lv[p] = rideinfo_upgrade_level(&r->spec, p);
		sum += (float)lv[p];
	}
	const int mean = (int)(sum / 10.0f);
	for (int i = 0; i < NUM_RANDOM_PART_UPGRADES; i++) {
		const RandomPartUpgrade* e = &kRandomPartUpgrades[i];
		int l;
		switch (e->slot) {
		case RIDE_SLOT_SPOILER:
		case RIDE_SLOT_FRONT_BUMPER:
		case RIDE_SLOT_ROOF: l = lv[9]; break;
		case RIDE_SLOT_EXHAUST: l = lv[3]; break;
		case RIDE_SLOT_FRONT_BRAKE: l = lv[6]; break;
		default: l = mean; break;
		}
		const int got = randomLevel(rng, e->chance[l]);
		if (got == 4)
			rideinfo_set_part(r, e->slot, e->stock ? get(r, e->slot, 0, NULL, 0) : NULL);
		else
			rideinfo_set_part(r, e->slot, randomPart(r, e->slot, got, rng));
	}

	// The rear bumper and skirt follow the front's kit number: the kit field
	// of the front part, so a front whose field is not its own kit's number
	// brings another kit's rear -- retail's rule, the PC's too (0x637654).
	CarPart front = r->part[RIDE_SLOT_FRONT_BUMPER];
	if (!front) {
		CarPart f = get(r, RIDE_SLOT_FRONT_BUMPER, 0, NULL, 0);
		CarPart b = get(r, RIDE_SLOT_REAR_BUMPER, 0, NULL, 0);
		CarPart k = get(r, RIDE_SLOT_SKIRT, 0, NULL, 0);
		rideinfo_set_part(r, RIDE_SLOT_FRONT_BUMPER, f);
		rideinfo_set_part(r, RIDE_SLOT_REAR_BUMPER, b);
		rideinfo_set_part(r, RIDE_SLOT_SKIRT, k);
	} else {
		char name[64];
		const int kit = carpartdb_kit(front);
		snprintf(name, sizeof(name), "%s_KIT%.2d_REAR_BUMPER", r->car->name, kit);
		CarPart rear = get(r, RIDE_SLOT_REAR_BUMPER, carpartdb_string_hash(name), NULL,
			CARPARTDB_ANY_LEVEL);
		snprintf(name, sizeof(name), "%s_KIT%.2d_SKIRT", r->car->name, kit);
		CarPart skirt = get(r, RIDE_SLOT_SKIRT, carpartdb_string_hash(name), NULL,
			CARPARTDB_ANY_LEVEL);
		if (rear && skirt) {
			rideinfo_set_part(r, RIDE_SLOT_FRONT_BUMPER, front);
			rideinfo_set_part(r, RIDE_SLOT_REAR_BUMPER, rear);
			rideinfo_set_part(r, RIDE_SLOT_SKIRT, skirt);
		}
	}

	CarPart w = randomWheel(r, randomLevel(rng, kWheelLevels[lv[8]]), rng);
	if (w) {
		rideinfo_set_part(r, RIDE_SLOT_FRONT_WHEEL, w);
		rideinfo_set_part(r, RIDE_SLOT_REAR_WHEEL, w);
	}
	rideinfo_build_random_skin(r, rng);
}

// GetRandomPermutation (0x80246FC8): 0..3 with each place swapped with a
// random one.
static void randomPermutation(int perm[4], u32* rng) {
	for (int i = 0; i < 4; i++)
		perm[i] = i;
	for (int i = 0; i < 4; i++) {
		const int j = (int)rideinfo_random(rng, 4);
		const int t = perm[j];
		perm[j] = perm[i];
		perm[i] = t;
	}
}

void rideinfo_build_random_skin(RideInfo* r, u32* rng) {
	float sum = 0.0f;
	for (int p = 0; p < RIDEINFO_PATHS; p++)
		sum += (float)rideinfo_upgrade_level(&r->spec, p);
	const int mean = (int)(sum / 10.0f);

	if ((int)rideinfo_random(rng, 100) < kFullVinylChance[mean]) {
		CarPart v = randomVinyl(r, CARPARTDB_ANY_LEVEL, kFullVinylGroups,
			COUNT(kFullVinylGroups), rng);
		if (v) {
			rideinfo_set_part(r, RIDE_SLOT_VINYL_LAYER0 + 1, NULL);
			rideinfo_set_part(r, RIDE_SLOT_VINYL_LAYER0 + 2, NULL);
			rideinfo_set_part(r, RIDE_SLOT_VINYL_LAYER0 + 3, NULL);
			rideinfo_set_part(r, RIDE_SLOT_VINYL_LAYER0, v);
		}
	} else {
		CarPart v0 = randomVinyl(r, randomLevel(rng, kVinylL0Levels[mean]), kVinylL0Groups,
			COUNT(kVinylL0Groups), rng);
		CarPart v1 = randomVinyl(r, randomLevel(rng, kVinylL1Levels[mean]), kVinylL1Groups,
			COUNT(kVinylL1Groups), rng);
		CarPart v2 = randomVinyl(r, randomLevel(rng, kVinylL2Levels[mean]), kVinylL2Groups,
			COUNT(kVinylL2Groups), rng);
		rideinfo_set_part(r, RIDE_SLOT_VINYL_LAYER0, v0);
		rideinfo_set_part(r, RIDE_SLOT_VINYL_LAYER0 + 1, v1);
		rideinfo_set_part(r, RIDE_SLOT_VINYL_LAYER0 + 2, v2);
		// Three layers' colours; the fourth keeps SetStockParts'.
		for (int layer = 0; layer < 3; layer++) {
			int perm[4];
			randomPermutation(perm, rng);
			const int job = (int)rideinfo_random(rng, COUNT(kRandomVinylPaintJobs));
			for (int k = 0; k < 4; k++) {
				const int s = RIDE_SLOT_VINYL_COLOUR + layer * 4 + k;
				rideinfo_set_part(r, s, get(r, s,
					carpartdb_string_hash(kRandomVinylPaintJobs[job][perm[k]]), NULL,
					CARPARTDB_ANY_LEVEL));
			}
		}
	}

	const RandomPartUpgrade* bp = &kRandomPartUpgrades[BASE_PAINT_ENTRY];
	const int got = randomLevel(rng, bp->chance[mean]);
	if (got == 4)
		rideinfo_set_part(r, bp->slot, bp->stock ? get(r, bp->slot, 0, NULL, 0) : NULL);
	else
		rideinfo_set_part(r, bp->slot, randomPart(r, bp->slot, got, rng));

	CarPart base = r->part[RIDE_SLOT_BASE_PAINT];
	for (int i = 0; base && i < COUNT(kRandomCarPaintJobs); i++) {
		const RandomCarPaintJob* j = &kRandomCarPaintJobs[i];
		if (carpartdb_string_hash(j->paint) != carpartdb_hash(base))
			continue;
		const int a = (int)rideinfo_random(rng, COUNT(j->accent));
		const int b = (int)rideinfo_random(rng, COUNT(j->accent));
		CarPart spoiler = get(r, RIDE_SLOT_PAINT_SPOILER, carpartdb_string_hash(j->accent[a]),
			NULL, CARPARTDB_ANY_LEVEL);
		CarPart exhaust = get(r, RIDE_SLOT_PAINT_EXHAUST, carpartdb_string_hash(j->accent[b]),
			NULL, CARPARTDB_ANY_LEVEL);
		rideinfo_set_part(r, RIDE_SLOT_PAINT_SPOILER, spoiler);
		rideinfo_set_part(r, RIDE_SLOT_PAINT_EXHAUST, exhaust);
	}
}

void rideinfo_sync_visual(RideInfo* r, const PerfDb* pdb, bool random, u32* rng) {
	RideSpec* s = &r->spec;
	float t = (rideinfo_normalized_level(s, 8) + rideinfo_normalized_level(s, 0) +
		rideinfo_normalized_level(s, 5)) / 3.0f;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	const int aero = 9;
	rideinfo_match_upgrade_level(s, pdb, t, &aero, 1);
	if (random)
		rideinfo_build_random_ride(r, rng);

	// The engine dress-up by the engine, ECU and turbo packages, the brakes by
	// the brake parts (ePhysicsPerformanceParts 0x28, 0x29, 0x2B).
	const bool l2 = packageInstalled(s, pdb, PERF_PKG_ENGINE, 2) &&
		packageInstalled(s, pdb, PERF_PKG_ECU, 2) && packageInstalled(s, pdb, PERF_PKG_TURBO, 2);
	const bool l3 = packageInstalled(s, pdb, PERF_PKG_ENGINE, 3) &&
		packageInstalled(s, pdb, PERF_PKG_ECU, 3) && packageInstalled(s, pdb, PERF_PKG_TURBO, 3);
	setUpgradePart(r, RIDE_SLOT_ENGINE, l3 ? 3 : l2 ? 2 : 0);
	const int brake = s->inst[0x28] ? 1 : s->inst[0x29] ? 2 : s->inst[0x2B] ? 3 : 0;
	setUpgradePart(r, RIDE_SLOT_FRONT_BRAKE, brake);
}

void rideinfo_slots(const RideInfo* r, unsigned int out[CARPARTDB_SLOTS + 1]) {
	out[0] = r->car ? r->car->nameHash : 0;
	for (int s = 0; s < CARPARTDB_SLOTS; s++)
		out[s + 1] = carpartdb_hash(r->part[s]);
}
