#include "perfdb.h"
#include "bunidx.h"
#include "cheats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See perfdb.h for where the three tables live and what every field is. This
// file is the reader and the unlock rules; nothing here is guessed.
//
// How the layouts were pinned down, in case the next person wants to redo it:
//
//   * The package stride is GetPerformancePackage's own (Speed.elf 0x80122970
//     adds 0x17C a record) and its two match fields are the +0x00 and +0x04 it
//     compares. The part stride is the 0x5C every walker uses
//     (GetPackagePriceOfNotOwned 0x801229D0, BuyPerfPackage 0x80170FC4).
//   * The brand count at part +0x0C is NumBrands__18PerformancePackage reading
//     package+0x18 -- part0 + 0x0C -- and is_brand_in_any_perf_package walking
//     part+0x10 for exactly that many entries.
//   * The unlock record is 0x28 because FindUnlockablePartData (0x8012DF30)
//     indexes by that stride, and its three levels sit at +0x04/+0x10/+0x1C
//     because UnlockablePartData::GetLevel (0x8012DED8) returns those three.
//
// And then checked against the data the game ships: 30 packages covering
// ePhysicsPerformanceParts 0..67 with no id used twice, 35 brands whose hashes
// are the bStringHash of their own name field, and 86 unlock records whose
// +0x00 equals their index + 1 throughout. The prices were checked a second
// way, against the user's retail save: career slot 2 owns twelve parts and its
// OwnedPartInventory's running spend is $7000, which is their prices summed
// exactly.

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u16 rd16le(const u8* p) {
	return (u16)((u32)p[0] | ((u32)p[1] << 8));
}
static float rdf32le(const u8* p) {
	union { u32 u; float f; } v;
	v.u = rd32le(p);
	return v.f;
}

// Record field offsets.
#define PP_OFF_TYPE      0x00
#define PP_OFF_LEVEL     0x04
#define PP_OFF_NUMPARTS  0x08
#define PP_OFF_PART0     0x0C

#define PT_OFF_ID        0x00
#define PT_OFF_NAME      0x04
#define PT_OFF_PRICE     0x08
#define PT_OFF_NUMBRANDS 0x0C
#define PT_OFF_BRANDS    0x10
#define PT_OFF_UPGRADES  0x30
#define PT_OFF_REPLACES  0x40
#define PT_OFF_REQUIRES  0x54

#define BR_OFF_NAME    0x00
#define BR_OFF_DISPLAY 0x20
#define BR_OFF_HASH    0x40

#define UN_OFF_ENTITY 0x00
#define UN_OFF_LEVEL0 0x04

static void copyField(char* out, size_t outSize, const u8* src, size_t n) {
	if (n > outSize - 1)
		n = outSize - 1;
	memcpy(out, src, n);
	out[n] = '\0';
	out[outSize - 1] = '\0';
}

static void decodePart(PerfPackagePart* p, const u8* rec) {
	p->partId = rd32le(rec + PT_OFF_ID);
	p->nameHash = rd32le(rec + PT_OFF_NAME);
	p->price = rd32le(rec + PT_OFF_PRICE);
	p->numBrands = rd32le(rec + PT_OFF_NUMBRANDS);
	if (p->numBrands > PERFDB_MAX_PART_BRANDS)
		p->numBrands = PERFDB_MAX_PART_BRANDS;
	for (int b = 0; b < PERFDB_MAX_PART_BRANDS; b++)
		p->brand[b] = rd32le(rec + PT_OFF_BRANDS + b * 4);
	for (int u = 0; u < PERFDB_MAX_PART_UPGRADES; u++)
		p->upgrade[u] = rdf32le(rec + PT_OFF_UPGRADES + u * 4);
	for (int r = 0; r < PERFDB_MAX_PART_REPLACES; r++)
		p->replaces[r] = (s32)rd32le(rec + PT_OFF_REPLACES + r * 4);
	for (int r = 0; r < PERFDB_MAX_PART_REQUIRES; r++)
		p->requires[r] = (s32)rd32le(rec + PT_OFF_REQUIRES + r * 4);
}

bool perfdb_load(const char* bunPath, PerfDb* out) {
	memset(out, 0, sizeof(*out));

	// The same container eventdb_load reads, found the same way and for the
	// same reason -- 0x512C00's one-shot guard means the first one in the file
	// is the live one, so the walk stops there rather than letting the stripped
	// second copy win.
	long hdrOff = 0;
	u32 size = 0;
	if (!bunidx_find(bunPath, PERFDB_CONTAINER_TYPE, &hdrOff, &size) || size < 8)
		return false;
	u8* body = (u8*)malloc(size);
	if (!body)
		return false;
	FILE* f = fopen(bunPath, "rb");
	const bool read = f && fseek(f, hdrOff + 8, SEEK_SET) == 0 &&
		fread(body, 1, size, f) == size;
	if (f)
		fclose(f);
	if (!read) {
		free(body);
		return false;
	}

	const u8* brands = NULL;
	u32 brandSize = 0;
	const u8* pkgs = NULL;
	u32 pkgSize = 0;
	const u8* unl = NULL;
	u32 unlSize = 0;
	for (u32 sub = 0; sub + 8 <= size; ) {
		const u32 st = rd32le(body + sub);
		const u32 ss = rd32le(body + sub + 4);
		if (st == 0 && ss == 0)
			break;
		if (ss > size - (sub + 8))
			break;
		const u8* pay = body + sub + 8;
		if (st == PERFDB_BRAND_CHUNK) {
			brands = pay;
			brandSize = ss;
		} else if (st == PERFDB_PACKAGE_CHUNK) {
			pkgs = pay;
			pkgSize = ss;
		} else if (st == PERFDB_UNLOCK_CHUNK) {
			unl = pay;
			unlSize = ss;
		}
		sub += 8 + ss;
	}

	if (brands) {
		int n = (int)(brandSize / PERFDB_BRAND_SIZE);
		if (n > PERFDB_MAX_BRANDS)
			n = PERFDB_MAX_BRANDS;
		for (int i = 0; i < n; i++) {
			const u8* rec = brands + i * PERFDB_BRAND_SIZE;
			PerfBrand* b = &out->brands[i];
			copyField(b->name, sizeof(b->name), rec + BR_OFF_NAME, 0x20);
			copyField(b->display, sizeof(b->display), rec + BR_OFF_DISPLAY, 0x20);
			b->hash = rd32le(rec + BR_OFF_HASH);
		}
		out->numBrands = n;
	}

	if (pkgs) {
		int n = (int)(pkgSize / PERFDB_PACKAGE_SIZE);
		if (n > PERFDB_MAX_PACKAGES)
			n = PERFDB_MAX_PACKAGES;
		for (int i = 0; i < n; i++) {
			const u8* rec = pkgs + i * PERFDB_PACKAGE_SIZE;
			PerformancePackage* p = &out->packages[i];
			p->type = rd32le(rec + PP_OFF_TYPE);
			p->level = rd32le(rec + PP_OFF_LEVEL);
			p->numParts = rd32le(rec + PP_OFF_NUMPARTS);
			if (p->numParts > PERFDB_MAX_PARTS)
				p->numParts = PERFDB_MAX_PARTS;
			for (u32 j = 0; j < PERFDB_MAX_PARTS; j++)
				decodePart(&p->part[j], rec + PP_OFF_PART0 + j * PERFDB_PART_SIZE);
		}
		out->numPackages = n;
	}

	if (unl) {
		int n = (int)(unlSize / PERFDB_UNLOCK_SIZE);
		if (n > PERFDB_MAX_UNLOCKS)
			n = PERFDB_MAX_UNLOCKS;
		for (int i = 0; i < n; i++) {
			const u8* rec = unl + i * PERFDB_UNLOCK_SIZE;
			PerfUnlockRec* u = &out->unlocks[i];
			u->entity = rd32le(rec + UN_OFF_ENTITY);
			for (int l = 0; l < 3; l++) {
				const u8* lv = rec + UN_OFF_LEVEL0 + l * 0x0C;
				u->level[l].ratingMin = rd16le(lv + 0x00);
				u->level[l].ratingMax = rd16le(lv + 0x02);
				u->level[l].kind = lv[0x04];
				u->level[l].level = lv[0x05];
				u->level[l].pad = rd16le(lv + 0x06);
				u->level[l].arg = rd32le(lv + 0x08);
			}
		}
		out->numUnlocks = n;
	}

	free(body);
	return true;
}

const PerformancePackage* perfdb_package(const PerfDb* db, int type, int level) {
	if (!db)
		return NULL;
	for (int i = 0; i < db->numPackages; i++) {
		const PerformancePackage* p = &db->packages[i];
		if ((int)p->type == type && (int)p->level == level)
			return p;
	}
	return NULL;
}

const PerformancePackage* perfdb_package_for_part(const PerfDb* db, int partId) {
	if (!db)
		return NULL;
	for (int i = 0; i < db->numPackages; i++) {
		const PerformancePackage* p = &db->packages[i];
		for (u32 j = 0; j < p->numParts; j++)
			if ((int)p->part[j].partId == partId)
				return p;
	}
	return NULL;
}

const PerfBrand* perfdb_brand(const PerfDb* db, u32 hash) {
	if (!db || !hash || hash == 0xFFFFFFFFu)
		return NULL;
	for (int i = 0; i < db->numBrands; i++)
		if (db->brands[i].hash == hash)
			return &db->brands[i];
	return NULL;
}

// MapPerfPackageToUnlockable (Speed.elf 0x8012ED7C), arm for arm.
int perfdb_type_to_entity(int type) {
	switch (type) {
	case PERF_PKG_WEIGHT_REDUCTION: return 6;
	case PERF_PKG_TRANSMISSION:     return 5;
	case PERF_PKG_NITROUS:          return 4;
	case PERF_PKG_ENGINE:           return 1;
	case PERF_PKG_TURBO:            return 9;
	case PERF_PKG_SUSPENSION:       return 3;
	case PERF_PKG_BRAKES:           return 8;
	case PERF_PKG_ECU:              return 2;
	case PERF_PKG_TIRES:            return 7;
	case PERF_PKG_JUNKMAN:          return 10;
	default:                        return 0;
	}
}

const PerfUnlockRec* perfdb_unlock(const PerfDb* db, int entity) {
	if (!db || entity < 1 || entity > PERFDB_NUM_UNLOCK_ENTITIES)
		return NULL;
	// FindUnlockablePartData is `&table[entity - 1]`, and the shipped table
	// agrees with its own +0x00 everywhere. The scan is the fallback for a file
	// that does not, so a reordered table reads right instead of silently
	// returning the wrong row.
	if (entity - 1 < db->numUnlocks && (int)db->unlocks[entity - 1].entity == entity)
		return &db->unlocks[entity - 1];
	for (int i = 0; i < db->numUnlocks; i++)
		if (db->unlocks[i].entity == (u32)entity)
			return &db->unlocks[i];
	return NULL;
}

// GetEndPerfPackageEnum (Speed.elf 0x80171D8C): 4, unless the shop the player
// is in is CC_MEGALOW, in which case it stops at level 2.
int perfdb_end_package_level(u32 currentShopHandle) {
	return (currentShopHandle == PERFDB_CC_MEGALOW_HASH) ? 3 : 4;
}

bool perfdb_owns_part(const PerfUnlockCtx* ctx, int partId) {
	if (!ctx || !ctx->owned || partId < 0 || partId >= PERFDB_NUM_PARTS)
		return false;
	return (ctx->owned[partId >> 3] >> (partId & 7)) & 1;
}

bool perfdb_part_installed(const PerfUnlockCtx* ctx, int partId) {
	if (!ctx || !ctx->installed || partId < 0 || partId >= PERFDB_NUM_PARTS)
		return false;
	return ctx->installed[partId] != 0;
}

static int stageRaces(const int* table, int stage) {
	if (stage < 0 || stage >= PERFDB_NUM_STAGES)
		return 0;
	return table[stage];
}

// CalculateIfUnlocked (Speed.elf 0x8012DFB0). Its two early returns come
// first: level 4 is never unlocked and level 0 -- stock -- always is. Then
// the contest vinyls' easter egg (#446): entity 0x44 with the regmebaby code
// entered (EasterEggs::AreContestVinylsUnlocked, PC 0x579D60).
bool perfdb_entity_unlocked(const PerfDb* db, const PerfUnlockCtx* ctx,
		int entity, int level) {
	if (level >= 4)
		return false;
	if (level <= 0)
		return true;
	if (entity == 0x44 && cheats_contest_vinyls())
		return true;
	const PerfUnlockRec* rec = perfdb_unlock(db, entity);
	if (!rec)
		return true; // no record: CalculateIfUnlocked returns 1
	const PerfUnlockLevel* lv = &rec->level[level - 1];
	switch (lv->kind) {
	case PERFDB_UNLOCK_SHOP:
		if (!ctx || !ctx->shopEnterable)
			return false;
		return ctx->shopEnterable(ctx->user, lv->arg);
	case PERFDB_UNLOCK_WORLD:
		return ctx && stageRaces(ctx->worldRaces, PERFDB_UNLOCK_STAGE(lv->arg)) >=
			PERFDB_UNLOCK_COUNT(lv->arg);
	case PERFDB_UNLOCK_URL:
		return ctx && stageRaces(ctx->urlRaces, PERFDB_UNLOCK_STAGE(lv->arg)) >=
			PERFDB_UNLOCK_COUNT(lv->arg);
	case PERFDB_UNLOCK_SPONSOR:
		return ctx && stageRaces(ctx->sponsorRaces, PERFDB_UNLOCK_STAGE(lv->arg)) >=
			PERFDB_UNLOCK_COUNT(lv->arg);
	case PERFDB_UNLOCK_ALWAYS:
	default:
		// 0, 5, 6 and anything higher all fall through to "unlocked" in the
		// shipped switch.
		return true;
	}
}

// CareerUnlocker::IsPerfPackageUnlocked (0x8012F724) under UnlockSystem's
// dispatch (0x8012FE14). The CAREER bit asks whether any part of the package is
// already owned; the ONLINE bit -- which CarCustomizeManager adds for every
// entry point but the crib's -- adds the unlock table. They are OR'd.
bool perfdb_package_available(const PerfDb* db, const PerfUnlockCtx* ctx,
		int type, int level) {
	if (!ctx)
		return false;
	bool ok = false;
	if (ctx->filter & PERF_FILTER_CAREER) {
		const PerformancePackage* p = perfdb_package(db, type, level);
		if (p) {
			for (u32 i = 0; i < p->numParts; i++) {
				if (perfdb_owns_part(ctx, (int)p->part[i].partId)) {
					ok = true;
					break;
				}
			}
		}
	}
	if (ctx->filter & PERF_FILTER_ONLINE)
		ok = ok || perfdb_entity_unlocked(db, ctx, perfdb_type_to_entity(type), level);
	// QuickRaceUnlocker::IsPerfPackageUnlocked (0x8012F238; PC 0x52A110's
	// first arm): the performance code of this level (#446).
	if (ctx->filter & PERF_FILTER_QUICKRACE)
		ok = ok || cheats_perf_level(level);
	return ok;
}

// CareerUnlocker::IsPerfPartUnlocked (0x8012F650), the same two bits.
bool perfdb_part_available(const PerfDb* db, const PerfUnlockCtx* ctx, int partId) {
	if (!ctx)
		return false;
	bool ok = false;
	if (ctx->filter & PERF_FILTER_CAREER)
		ok = perfdb_owns_part(ctx, partId);
	if (ctx->filter & PERF_FILTER_ONLINE) {
		const PerformancePackage* p = perfdb_package_for_part(db, partId);
		if (p)
			ok = ok || perfdb_entity_unlocked(db, ctx,
				perfdb_type_to_entity((int)p->type), (int)p->level);
	}
	// QuickRaceUnlocker::IsPerfPartUnlocked (0x8012F1B8; PC 0x529E30): the
	// performance code of its package's level (#446).
	if (ctx->filter & PERF_FILTER_QUICKRACE) {
		const PerformancePackage* p = perfdb_package_for_part(db, partId);
		if (p)
			ok = ok || cheats_perf_level((int)p->level);
	}
	return ok;
}

// ChoosePerfCategory::AddCategory (0x80171DEC): the row is drawn locked unless
// some level in 1..GetEndPerfPackageEnum()-1 is available.
bool perfdb_category_locked(const PerfDb* db, const PerfUnlockCtx* ctx, int type) {
	const int end = perfdb_end_package_level(ctx ? ctx->currentShopHandle : 0);
	for (int level = PERFDB_LEVEL_MIN; level < end; level++)
		if (perfdb_package_available(db, ctx, type, level))
			return false;
	return true;
}

// IsPartNotInstalledOrPurchased (Speed.elf 0x80171478). "Purchased" there is
// IsPerfPartUnlocked under the career filter alone, which is ownership --
// CarCustomizeManager::IsPerfPartPurchased (0x801714FC) passes filter 2 and
// nothing else, whatever filter the rest of the session is running under.
static bool partNotInstalledOrPurchased(const PerfUnlockCtx* ctx, int partId) {
	if (perfdb_part_installed(ctx, partId))
		return false;
	return !perfdb_owns_part(ctx, partId);
}

// PlayerCareerState::GetPerfPackageCost (Speed.elf 0x80129648).
void perfdb_package_cost(const PerfDb* db, const PerfUnlockCtx* ctx,
		int type, int level, PerfPackageCost* out) {
	memset(out, 0, sizeof(*out));
	const PerformancePackage* p = perfdb_package(db, type, level);
	if (!p)
		return;
	int full = 0, n = 0;
	for (u32 i = 0; i < p->numParts; i++) {
		if (partNotInstalledOrPurchased(ctx, (int)p->part[i].partId)) {
			full += (int)p->part[i].price;
			n++;
		}
	}
	out->full = full;
	out->numNotOwned = n;
	if (n > 1) {
		const int pct = (ctx && ctx->currentShopHandle == PERFDB_CC_MEGALOW_HASH)
			? 80 : 90;
		out->cost = (full * pct) / 100;
		out->savings = full - out->cost;
	} else {
		out->cost = full;
	}
}

// ChoosePerformancePackages::GetLangHashForPackageType (0x801736C0).
u32 perfdb_type_name_hash(int type) {
	switch (type) {
	case PERF_PKG_WEIGHT_REDUCTION: return 0x758D5E3Cu;
	case PERF_PKG_TRANSMISSION:     return 0x73AD2F72u;
	case PERF_PKG_NITROUS:          return 0xA7886F38u;
	case PERF_PKG_ENGINE:           return 0x026EEA5Eu;
	case PERF_PKG_TURBO:            return 0xABD0FF14u;
	case PERF_PKG_SUSPENSION:       return 0x9C34C75Fu;
	case PERF_PKG_BRAKES:           return 0xAA8959ADu;
	case PERF_PKG_ECU:              return 0xA7884765u;
	case PERF_PKG_TIRES:            return 0xABCA6AEFu;
	default:                        return 0u;
	}
}

const PerfPackagePart* perfdb_part(const PerfDb* db, int partId) {
	const PerformancePackage* p = perfdb_package_for_part(db, partId);
	if (!p)
		return NULL;
	for (u32 j = 0; j < p->numParts; j++)
		if ((int)p->part[j].partId == partId)
			return &p->part[j];
	return NULL;
}

// PhysicsUpgradeSpecification::InstallPerfPart (Speed.elf 0x801C4338).
int perfdb_install_part(const PerfDb* db, u8* installed, int partId,
		bool install, bool autoRequires) {
	const PerfPackagePart* p = perfdb_part(db, partId);
	if (!p || !installed || partId < 0 || partId >= PERFDB_NUM_PARTS)
		return 0;
	if (installed[partId]) {
		if (install)
			return 0;
		installed[partId] = 0;
		return 1;
	}
	if (!install)
		return 0;
	int n = 1;
	for (int k = 0; k < PERFDB_MAX_PART_REQUIRES; k++) {
		const int req = (int)p->requires[k];
		if (req < 0 || req >= PERFDB_NUM_PARTS || installed[req])
			continue;
		if (!autoRequires)
			return 0;
		n += perfdb_install_part(db, installed, req, true, true);
	}
	for (int k = 0; k < PERFDB_MAX_PART_REPLACES; k++) {
		const int rep = (int)p->replaces[k];
		if (rep >= 0 && rep < PERFDB_NUM_PARTS)
			perfdb_install_part(db, installed, rep, false, false);
	}
	installed[partId] = 1;
	return n;
}

// CarCustomizeManager::ResetToStockPerf (0x80170E70). The RebuildPhysicsInfo
// and car-viewer refresh after it are the caller's.
void perfdb_reset_to_stock(const PerfDb* db, u8* installed, int type) {
	if (!db || !installed)
		return;
	for (int i = 0; i < db->numPackages; i++) {
		const PerformancePackage* p = &db->packages[i];
		if ((int)p->type != type)
			continue;
		for (u32 j = 0; j < p->numParts; j++)
			perfdb_install_part(db, installed, (int)p->part[j].partId, false, false);
	}
}

// PhysicsUpgradeSpecification::ComputeUpgradeLevel (0x801C466C). An upgrade
// value of -1.0 is "no effect" and is skipped, as the shipped loop skips it.
float perfdb_upgrade_components(const PerfDb* db, const u8* installed, int type,
		float comp[PERFDB_UPGRADE_COMPONENTS]) {
	for (int c = 0; c < PERFDB_UPGRADE_COMPONENTS; c++)
		comp[c] = 0.0f;
	if (!db || !installed)
		return 0.0f;
	int ncomp = 1;
	if (type == PERF_PKG_TRANSMISSION || type == PERF_PKG_SUSPENSION)
		ncomp = 3;
	else if (type == PERF_PKG_JUNKMAN)
		ncomp = 2;
	for (int level = PERFDB_LEVEL_MIN; level <= PERFDB_LEVEL_MAX; level++) {
		const PerformancePackage* p = perfdb_package(db, type, level);
		if (!p)
			continue;
		for (int c = 0; c < ncomp; c++) {
			for (u32 j = 0; j < p->numParts; j++) {
				const int id = (int)p->part[j].partId;
				if (id < 0 || id >= PERFDB_NUM_PARTS || !installed[id])
					continue;
				if (p->part[j].upgrade[c] != -1.0f)
					comp[c] += p->part[j].upgrade[c];
			}
		}
	}
	float sum = 0.0f;
	for (int c = 0; c < ncomp; c++)
		sum += comp[c];
	float avg = sum / (float)ncomp;
	if (avg < 0.0f || avg > 1.0f) {
		// Out of range, each component is clamped where it lies -- the
		// package builders read them back -- and the average taken again.
		sum = 0.0f;
		for (int c = 0; c < ncomp; c++) {
			float v = comp[c];
			if (v < 0.0f) v = 0.0f;
			if (v > 1.0f) v = 1.0f;
			comp[c] = v;
			sum += v;
		}
		avg = sum / (float)ncomp;
	}
	return avg;
}

// Then SetNormalizedLevel's clamp and scale (0x801C4220) and GetUpgradeLevel's
// +0.5 truncation (0x801C41DC).
float perfdb_upgrade_value(const PerfDb* db, const u8* installed, int type) {
	float comp[PERFDB_UPGRADE_COMPONENTS];
	float avg = perfdb_upgrade_components(db, installed, type, comp);
	if (avg < 0.0f) avg = 0.0f;
	if (avg > 1.0f) avg = 1.0f;
	return avg * 3.0f;
}

int perfdb_upgrade_level(const PerfDb* db, const u8* installed, int type) {
	return (int)(perfdb_upgrade_value(db, installed, type) + 0.5f);
}

// CarCustomizeManager::IsDowngradedFromInstalled (0x80171688). It walks the
// replaced parts only as far as the count of entries that are not -1 and
// answers on the first one installed.
bool perfdb_part_downgrades(const PerfDb* db, const u8* installed, int partId) {
	const PerfPackagePart* p = perfdb_part(db, partId);
	const PerformancePackage* own = perfdb_package_for_part(db, partId);
	if (!p || !own || !installed)
		return false;
	int count = 0;
	for (int k = 0; k < PERFDB_MAX_PART_REPLACES; k++)
		if (p->replaces[k] != -1)
			count++;
	for (int k = 0; k < count; k++) {
		const int rep = (int)p->replaces[k];
		if (rep < 0 || rep >= PERFDB_NUM_PARTS || !installed[rep])
			continue;
		const PerformancePackage* theirs = perfdb_package_for_part(db, rep);
		return theirs && own->level < theirs->level;
	}
	return false;
}
