#include "stargazer.h"

// See stargazer.h.

// MapCarSlotToUnlockable (SpeedPS.elf .bss 0x803DA574), as InitUnlockablesMap
// (0x8012E544) fills it: the eUnlockablePartEntity each CAR_SLOT_ID's parts
// map to unless MapCarPartToUnlockable says otherwise.
static const u8 kSlotToUnlockable[CARPARTDB_SLOTS] = {
	0x00, 0x12, 0x11, 0x13, 0x13, 0x00, 0x16, 0x14, 0x00, 0x0B, 0x00, 0x10, 0x0C, 0x00, 0x0F, 0x0E,  // 0x00
	0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x0D, 0x21,  // 0x10
	0x13, 0x00, 0x1E, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x00,  // 0x20
	0x00, 0x00, 0x00, 0x00, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x30
	0x00, 0x00, 0x00, 0x00, 0x28, 0x50, 0x4F, 0x4E, 0x52, 0x4D, 0x51, 0x53, 0x54, 0x00, 0x00, 0x00,  // 0x40
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00,  // 0x50
	0x56, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C,  // 0x60
	0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C,  // 0x70
	0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C,  // 0x80
	0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x20, 0x19, 0x1A, 0x1B, 0x1C, 0x1B, 0x18,  // 0x90
	0x23, 0x24, 0x1D, 0x17, 0x17, 0x17, 0x17, 0x00, 0x00, 0x00,                                      // 0xA0
};

// MapCarPartToUnlockable's vinyl arm (slots 0x40..0x43): the entity by the
// vinyl's group, the part's kit bits. 0 past the table.
static const u8 kVinylGroupToUnlockable[] = {
	0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,  // 0x00
	0x38, 0x39, 0x3A, 0x3B, 0x41, 0x42, 0x43, 0x3C, 0x3D, 0x40, 0x3E, 0x37, 0x3F, 0x44,  // 0x0E
};

#define SLOT_FRONT_BUMPER 0x01
#define SLOT_REAR_BUMPER  0x02
#define SLOT_SKIRT        0x0B
#define SLOT_HOOD         0x09
#define SLOT_ROOF         0x07
#define SLOT_SPOILER      0x0C
#define SLOT_WING_MIRROR  0x20

// GetAppliedAttributeUParam(0xEBB03E66): the part's base-name hash -- a
// paint's finish ("METAL", "GLOSS"...), a rim's kind. GetAppliedAttributeIParam
// (bStringHash("CARBONFIBRE")): the carbon hoods, roofs, spoilers and mirrors.
#define KEY_BASE_NAME   0xEBB03E66u
#define KEY_CARBONFIBRE 0x721AFF7Cu
// bStringHash("SPINNER"), the spinner rims' base name.
#define WHEEL_SPINNER   0xC8D1A75Eu

// MapCarPartToUnlockable (0x8012E8BC).
static int mapCarPartToUnlockable(const CarPartDb* db, int slot, CarPart p) {
	if (slot == RIDE_SLOT_BASE_PAINT) {
		const u32 kind = carpartdb_attr(db, p, KEY_BASE_NAME, 0);
		if (kind == carpartdb_string_hash("METAL"))   return 0x26;
		if (kind == carpartdb_string_hash("GLOSS"))   return 0x25;
		if (kind == carpartdb_string_hash("PEARL"))   return 0x27;
		if (kind == carpartdb_string_hash("MUFFLER")) return 0x4E;
		if (kind == carpartdb_string_hash("RIM"))     return 0x4D;
		if (kind == carpartdb_string_hash("HOSE"))    return 0x28;
		return 0x26;
	}
	if (slot >= RIDE_SLOT_VINYL_LAYER0 && slot <= RIDE_SLOT_VINYL_LAYER0 + 3) {
		const int group = carpartdb_kit(p);
		return group < (int)sizeof(kVinylGroupToUnlockable) ? kVinylGroupToUnlockable[group] : 0;
	}
	if (carpartdb_attr(db, p, KEY_CARBONFIBRE, 0) != 0) {
		switch (slot) {
		case SLOT_HOOD:        return 0x4B;
		case SLOT_ROOF:        return 0x46;
		case SLOT_SPOILER:     return 0x4A;
		case SLOT_WING_MIRROR: return 0x45;
		default:               return 0;
		}
	}
	if (slot == RIDE_SLOT_FRONT_WHEEL && carpartdb_attr(db, p, KEY_BASE_NAME, 0) == WHEEL_SPINNER)
		return 0x21;
	if (slot == SLOT_HOOD && carpartdb_kit(p) == 5)
		return 0x22;
	return kSlotToUnlockable[slot];
}

int stargazer_unlockable(const CarPartDb* db, int slot, CarPart p) {
	return slot >= 0 && slot < CARPARTDB_SLOTS && p ? mapCarPartToUnlockable(db, slot, p) : 0;
}

// GetUnlockablePartRep (0x8012E294).
static int unlockablePartRep(const PerfDb* pdb, int entity, int level) {
	if (level == 0)
		return 0;
	const PerfUnlockRec* u = perfdb_unlock(pdb, entity);
	if (!u)
		return 0;
	if (level >= 1 && level <= 3)
		return (s16)u->level[level - 1].ratingMin;
	if (level != 4)
		return 0; // GetLevel's NULL; no shipped part carries a level past 4
	switch (entity) {
	case 0x16:
		return 1000;
	case 0x0B: case 0x0C: case 0x0D: case 0x22: case 0x4A: case 0x4B:
		return 400;
	case 0x3E:
		return 100;
	default:
		return 0;
	}
}

// GetCarPartRep (0x80129548).
static int carPartRep(const RideInfo* r, const PerfDb* pdb, int slot, CarPart p) {
	int rep = unlockablePartRep(pdb, mapCarPartToUnlockable(r->db, slot, p), carpartdb_level(p));
	if (slot == RIDE_SLOT_BASE_PAINT && r->car && carpartdb_hash(p) == r->car->stockPaint)
		rep = 0;
	return rep == -1 ? 0 : rep;
}

int stargazer_rep(const RideInfo* r, const PerfDb* pdb) {
	if (!r || !r->db || !pdb)
		return 0;
	const CarPart wide = r->part[RIDE_SLOT_WIDE_BODY];
	const bool wideBody = wide && carpartdb_level(wide) != 0;
	int rep = 0;
	for (int s = 0; s < CARPARTDB_SLOTS; s++) {
		if ((unsigned)(s - 0x4E) <= 0x0E || s == RIDE_SLOT_REAR_WHEEL)
			continue;
		if (wideBody && (s == SLOT_FRONT_BUMPER || s == SLOT_REAR_BUMPER || s == SLOT_SKIRT))
			continue;
		if (r->part[s])
			rep += carPartRep(r, pdb, s, r->part[s]);
	}
	return rep;
}

// StarValues (0x8035E83C): {stars, rep} rows, read linearly between them.
static const int kStarValues[11][2] = {
	{ 0, 0 }, { 1, 500 }, { 2, 1000 }, { 3, 1500 }, { 4, 2000 }, { 5, 2500 },
	{ 6, 3000 }, { 7, 3500 }, { 8, 4000 }, { 9, 4500 }, { 10, 5000 },
};

float stargazer_stars_for_rep(int rep) {
	if (rep >= kStarValues[10][1])
		return (float)kStarValues[10][0];
	for (int i = 0; i < 10; i++) {
		const int lo = kStarValues[i][1], hi = kStarValues[i + 1][1];
		if (lo <= rep && rep <= hi) {
			const float t = hi != lo ? (float)(rep - lo) / (float)(hi - lo) : 0.0f;
			return t * (float)(kStarValues[i + 1][0] - kStarValues[i][0]) + (float)kStarValues[i][0];
		}
	}
	return 0.0f; // a negative rep, which no row holds
}

float stargazer_stars(const RideInfo* r, const PerfDb* pdb) {
	return stargazer_stars_for_rep(stargazer_rep(r, pdb));
}
