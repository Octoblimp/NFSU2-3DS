#pragma once
#include <3ds.h>
#include <stddef.h>

// The performance package database: what a performance part is, what it costs,
// which brands sell it, and when the career lets you have it.
//
// Three sub-chunks of the same top-level 0x80034A10 container eventdb.h reads
// carry all of it, and LoaderMostWantedCareerChunk (Speed.elf 0x80123520) is
// the loader that parks each one in a global:
//
//   0x34A14  35 records of 0x44 -- the brand list (CreateUndergroundBrandList)
//   0x34A15  30 records of 0x17C -- the packages  (CreatePerfPackageList)
//   0x34A1C  86 records of 0x28  -- the unlock table (LoadUnlockData)
//
// The GameCube byte-swaps all three at load because they are authored
// little-endian on PC; the 3DS is little-endian too, so it reads them as they
// lie. Every offset below was confirmed against the shipped PC GLOBALB.BUN and
// then against the user's own retail save (see perfdb.c).

// ---------------------------------------------------------------------------
// Chunk ids and strides
// ---------------------------------------------------------------------------
#define PERFDB_CONTAINER_TYPE 0x80034A10u
#define PERFDB_BRAND_CHUNK    0x00034A14u
#define PERFDB_PACKAGE_CHUNK  0x00034A15u
#define PERFDB_UNLOCK_CHUNK   0x00034A1Cu

#define PERFDB_BRAND_SIZE   0x44
#define PERFDB_PACKAGE_SIZE 0x17C
#define PERFDB_PART_SIZE    0x5C
#define PERFDB_UNLOCK_SIZE  0x28

// The shipped table's own counts, used as the array bounds. A file carrying
// more records than these is truncated at load rather than rejected -- the same
// shape eventdb.h uses for its own tables.
#define PERFDB_MAX_BRANDS   48
#define PERFDB_MAX_PACKAGES 48
#define PERFDB_MAX_UNLOCKS  128

// PerformancePackage's own loop bound (+0x08) never exceeds 4 in the shipped
// table, and the 0x5C-byte part array at +0x0C has room for exactly four
// before the record ends at 0x17C.
#define PERFDB_MAX_PARTS 4
// PerfPackagePart's brand array, +0x10..+0x30.
#define PERFDB_MAX_PART_BRANDS 8
// Its upgrade-path array, +0x30..+0x40, and the two id lists after it.
#define PERFDB_MAX_PART_UPGRADES 4
#define PERFDB_MAX_PART_REPLACES 5
#define PERFDB_MAX_PART_REQUIRES 2

// ePhysicsPerformanceParts runs 0..67 and PhysicsUpgradeSpecification keeps one
// installed byte per id (Speed.elf 0x801C4270 indexes it with the enum), so 68
// is both the part count and the width of every per-part array here.
#define PERFDB_NUM_PARTS 68

// ---------------------------------------------------------------------------
// ePerfPackageTypes
// ---------------------------------------------------------------------------
// The order is the data's, not the shop screen's: UI_ChoosePerformanceCategory
// lists Engine first (MENU_ITEMS_PERFCATEGORY in main.c), which is type 3.
typedef enum {
	PERF_PKG_WEIGHT_REDUCTION = 0,
	PERF_PKG_TRANSMISSION     = 1,
	PERF_PKG_NITROUS          = 2,
	PERF_PKG_ENGINE           = 3,
	PERF_PKG_TURBO            = 4,
	PERF_PKG_SUSPENSION       = 5,
	PERF_PKG_BRAKES           = 6,
	PERF_PKG_ECU              = 7,
	PERF_PKG_TIRES            = 8,
	// Type 9 is the hidden junkman tier: twelve parts (56..67) in four groups
	// of three, every one priced $0 with no brands and a name hash that is not
	// in English.bin. ChoosePerfCategory::Setup emits nine rows, not ten,
	// which is why it never shows.
	PERF_PKG_JUNKMAN          = 9,
	PERF_NUM_PKG_TYPES        = 10,
} ePerfPackageTypes;

// eCareerUpgradeLevels. 0 is "stock" and never names a package;
// GetPerformancePackage matches on the record's own +0x04.
#define PERFDB_LEVEL_MIN 1
#define PERFDB_LEVEL_MAX 3

// ---------------------------------------------------------------------------
// The records
// ---------------------------------------------------------------------------

// One 0x5C-byte PerfPackagePart. Every field below is read by name somewhere in
// Speed.elf:
//   +0x00 the ePhysicsPerformanceParts id -- IsPerfPartInstalled (0x801714D4)
//         loads it straight out of the part and hands it to
//         PhysicsUpgradeSpecification::IsPerfPartInstalled.
//   +0x04 a language-bin hash: the part's display name.
//   +0x08 the price -- GetPackagePriceOfNotOwned (0x801229D0) sums part+0x14,
//         which is this field one part-stride in.
//   +0x0C the brand count -- NumBrands__18PerformancePackage reads package+0x18,
//         which is part0 + 0x0C.
//   +0x10 the brand name hashes, `numBrands` of them;
//         is_brand_in_any_perf_package walks exactly that many.
//   +0x30 the upgrade-path values this part contributes, -1.0 in an unused slot.
//   +0x40 the parts this one replaces, -1 for none.
//   +0x54 the parts this one needs first, -1 for none.
typedef struct {
	u32   partId;
	u32   nameHash;
	u32   price;
	u32   numBrands;
	u32   brand[PERFDB_MAX_PART_BRANDS];
	float upgrade[PERFDB_MAX_PART_UPGRADES];
	s32   replaces[PERFDB_MAX_PART_REPLACES];
	s32   requires[PERFDB_MAX_PART_REQUIRES];
} PerfPackagePart;

// One 0x17C-byte PerformancePackage. GetPerformancePackage
// (Speed.elf 0x80122970) is a linear scan of the array matching +0x00 against
// the type and +0x04 against the level, which is what fixes both fields.
typedef struct {
	u32 type;  // an ePerfPackageTypes
	u32 level; // 1..3
	u32 numParts;
	PerfPackagePart part[PERFDB_MAX_PARTS];
} PerformancePackage;

// One 0x44-byte brand record. The display name is what the brand-select screen
// shows; the hash is what a part's brand[] holds and what
// OwnedPartInventory::OwnsBrand compares against.
typedef struct {
	char name[32];    // +0x00, e.g. "DC"
	char display[32]; // +0x20, the shown form
	u32  hash;        // +0x40
} PerfBrand;

// How one upgrade level of one unlockable entity is gated. The record is
// 0x0C bytes and UnlockablePartData::GetLevel (Speed.elf 0x8012DED8) returns
// data+4, data+0x10 or data+0x1C for levels 1, 2 and 3 -- base +0x04,
// stride 0x0C.
typedef struct {
	u16 ratingMin;
	u16 ratingMax;
	u8  kind; // see PERFDB_UNLOCK_*
	u8  level;
	u16 pad;
	u32 arg;
} PerfUnlockLevel;

// CalculateIfUnlocked (Speed.elf 0x8012DFB0) switches on the kind byte.
// Everything it does not name falls through to "always unlocked", which is
// what 5 and 6 and anything higher get.
#define PERFDB_UNLOCK_ALWAYS   0
// `arg` is a shop handle: IsShopEnterable(FindShopDataDescByHandle(arg)).
#define PERFDB_UNLOCK_SHOP     1
// `arg` packs a stage in its high half and a race count in its low half:
// GetNumWorldRacesCompleted(stage) >= count.
#define PERFDB_UNLOCK_WORLD    2
// The same packing against GetNumURLRacesCompleted.
#define PERFDB_UNLOCK_URL      3
// The same packing against GetNumSponsorRacesCompleted.
#define PERFDB_UNLOCK_SPONSOR  4

#define PERFDB_UNLOCK_STAGE(arg) ((int)((s32)(arg) >> 16))
#define PERFDB_UNLOCK_COUNT(arg) ((int)((arg) & 0xFFFFu))

// One 0x28-byte UnlockablePartData: the entity id and its three levels.
// FindUnlockablePartData (0x8012DF30) indexes the array by entity - 1 for
// entities 1..0x56, so the record's own +0x00 is redundant with its position
// -- it is kept because the shipped table agrees with it everywhere, and a
// mismatch is worth seeing in the log rather than silently indexing past it.
typedef struct {
	u32             entity;
	PerfUnlockLevel level[3];
} PerfUnlockRec;

// eUnlockablePartEntity values the perf types map to, so the three shop-gated
// categories can be named in the log.
#define PERFDB_NUM_UNLOCK_ENTITIES 0x56

typedef struct {
	int         numBrands;
	PerfBrand   brands[PERFDB_MAX_BRANDS];
	int         numPackages;
	PerformancePackage packages[PERFDB_MAX_PACKAGES];
	int         numUnlocks;
	PerfUnlockRec unlocks[PERFDB_MAX_UNLOCKS];
} PerfDb;

// ---------------------------------------------------------------------------
// Loading and lookup
// ---------------------------------------------------------------------------

// Reads the three sub-chunks out of `bunPath`'s first 0x80034A10 container --
// the same container and the same one-shot "first one wins" rule eventdb_load
// follows, for the same reason (see eventdb.h). Returns false only when the
// container is missing or unreadable; a container carrying some of the three
// still fills what it has, and each table reports its own count.
bool perfdb_load(const char* bunPath, PerfDb* out);

// GetPerformancePackage (Speed.elf 0x80122970): the first record whose type and
// level both match, or NULL.
const PerformancePackage* perfdb_package(const PerfDb* db, int type, int level);

// FindPerfPackageForPerfPart (0x801228FC): the package that contains this part.
const PerformancePackage* perfdb_package_for_part(const PerfDb* db, int partId);

// The brand record for a hash a part's brand[] holds, or NULL.
const PerfBrand* perfdb_brand(const PerfDb* db, u32 hash);

// MapPerfPackageToUnlockable (0x8012ED7C), then FindUnlockablePartData
// (0x8012DF30). Returns NULL when the type has no record.
int perfdb_type_to_entity(int type);
const PerfUnlockRec* perfdb_unlock(const PerfDb* db, int entity);

// GetEndPerfPackageEnum (Speed.elf 0x80171D8C): the exclusive bound the package
// list and the category greying both loop to. 4 everywhere except the shop
// CC_MEGALOW, which caps itself at level 2. CC_MEGALOW is a performance shop
// (ShopDataDesc record 23, type 3, PERFSHOP01.BIN), and what the function
// compares is the +0x38 handle of the shop the player is in --
// PlayerCareerState +0x7DF8, valid while +0x7DF0 is set -- against this.
#define PERFDB_CC_MEGALOW_HASH 0x7FC8D0D0u
int perfdb_end_package_level(u32 currentShopHandle);

// ---------------------------------------------------------------------------
// Availability
// ---------------------------------------------------------------------------
// eUnlockFilters, as CarCustomizeManager::GetPartUnlockFilter (0x8017107C)
// picks one: entry point 1 -- the crib's CHANGE PARTS -- gets CAREER alone, so
// only parts already bought are offered; any other entry point into a career
// customize session gets CAREER|ONLINE, which is what lets a shop sell the
// levels the unlock table has opened.
#define PERF_FILTER_QUICKRACE 1
#define PERF_FILTER_CAREER    2
#define PERF_FILTER_ONLINE    4

// What the career state has to answer for the rules above. It is passed rather
// than reached for so that perfdb.c stays a reader of the shipped tables and
// does not pull in the profile or the event database.
//
// `owned` is the 16-byte OwnedPartInventory bitmask (record +0x432, +0x15C in),
// bit `id & 7` of byte `id >> 3`, which is exactly what OwnsPerfPart
// (0x801359A8) indexes. `installed` is PhysicsUpgradeSpecification's 68
// installed bytes (record +0x318). Either may be NULL, which reads as "none".
//
// The three race counts are per stage, the non-zero slots of the career's own
// RuntimeCareerStageInfo arrays (see ProfileStageInfo in profile_save.h).
//
// `shopEnterable` is PlayerCareerState::IsShopEnterable (0x801243B8) for a shop
// handle; a NULL function reads as "not enterable", which is what a port with
// no shop walk-in yet should answer.
#define PERFDB_NUM_STAGES 8

typedef bool (*PerfShopEnterableFn)(void* user, u32 shopHandle);

typedef struct {
	u32  filter;
	const u8* owned;     // 16 bytes
	const u8* installed; // PERFDB_NUM_PARTS bytes
	int  worldRaces[PERFDB_NUM_STAGES];
	int  urlRaces[PERFDB_NUM_STAGES];
	int  sponsorRaces[PERFDB_NUM_STAGES];
	u32  currentShopHandle; // the shop walked into, 0 for none (the crib)
	PerfShopEnterableFn shopEnterable;
	void* user;
} PerfUnlockCtx;

// OwnedPartInventory::OwnsPerfPart (0x801359A8) and
// PhysicsUpgradeSpecification::IsPerfPartInstalled (0x801C4270).
bool perfdb_owns_part(const PerfUnlockCtx* ctx, int partId);
bool perfdb_part_installed(const PerfUnlockCtx* ctx, int partId);

// CalculateIfUnlocked (0x8012DFB0) for one entity and level.
bool perfdb_entity_unlocked(const PerfDb* db, const PerfUnlockCtx* ctx,
	int entity, int level);

// UnlockSystem::IsPerfPackageUnlocked (0x8012FE14), reduced to the two filters
// a career customize session can carry. CAREER alone is "you already own one of
// its parts"; CAREER|ONLINE adds CalculateIfUnlocked on the mapped entity.
// This is CarCustomizeManager::IsPerfPackageAvailable (0x801711B0).
bool perfdb_package_available(const PerfDb* db, const PerfUnlockCtx* ctx,
	int type, int level);

// UnlockSystem::IsPerfPartUnlocked (0x8012FD2C), the same two filters.
bool perfdb_part_available(const PerfDb* db, const PerfUnlockCtx* ctx, int partId);

// AddCategory (0x80171DEC) greys a category when no level of it is available.
// Returns true when the row should be drawn locked.
bool perfdb_category_locked(const PerfDb* db, const PerfUnlockCtx* ctx, int type);

// ---------------------------------------------------------------------------
// Price
// ---------------------------------------------------------------------------
// PlayerCareerState::GetPerfPackageCost (Speed.elf 0x80129648).
//
// The full price is the sum of the parts that are neither installed nor already
// bought (IsPartNotInstalledOrPurchased, 0x80171478). When more than one such
// part is left the package is a bundle and the player pays a percentage of it:
// 90 normally, 80 in the CC_MEGALOW shop -- so the screen's "10%% Discount:"
// becomes "20%% Discount:" in that one shop. A package down to its last unowned part
// is charged in full and the saving is zero.
typedef struct {
	int full;    // before any discount
	int cost;    // what the player pays
	int savings; // full - cost
	int numNotOwned;
} PerfPackageCost;

void perfdb_package_cost(const PerfDb* db, const PerfUnlockCtx* ctx,
	int type, int level, PerfPackageCost* out);

// The language-bin hash of the discount label, which is the one string on the
// screen that changes with the event.
#define PERFDB_STR_DISCOUNT_10 0x6DA17A25u // "10%% Discount:"
#define PERFDB_STR_DISCOUNT_20 0x66559034u // "20%% Discount:"
// What RefreshHeader prints in the cost and savings fields when the value is 0.
#define PERFDB_STR_NA          0xA4C02484u // "n/a"

// ChoosePerformancePackages::GetLangHashForPackageType (0x801736C0): the
// header string for a category, the same hash MENU_ITEMS_PERFCATEGORY's row
// carries. Returns 0 for the junkman type, which has no header.
u32 perfdb_type_name_hash(int type);

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------
// These write PhysicsUpgradeSpecification's installed bytes, the same 68 the
// PerfUnlockCtx reads, so the caller passes the car's own array.

// The part record for an ePhysicsPerformanceParts id, or NULL.
const PerfPackagePart* perfdb_part(const PerfDb* db, int partId);

// PhysicsUpgradeSpecification::InstallPerfPart (Speed.elf 0x801C4338). With
// `install`, a part not yet in goes in once every part it requires is in --
// installing each first when `autoRequires` is set, refusing otherwise -- and
// takes out each part it replaces. Without it, an installed part comes out.
// Returns how many parts went in or came out, 0 when nothing changed.
int perfdb_install_part(const PerfDb* db, u8* installed, int partId,
	bool install, bool autoRequires);

// CarCustomizeManager::ResetToStockPerf (0x80170E70): every part of every
// package of `type` out.
void perfdb_reset_to_stock(const PerfDb* db, u8* installed, int type);

// PhysicsUpgradeSpecification::ComputeUpgradeLevel (0x801C466C): per
// component, the sum of the installed parts' upgrade values across levels
// 1..3, the components averaged (each clamped to 0..1 when the average falls
// outside it) and scaled to 0..3 -- the float SetNormalizedLevel (0x801C4220)
// stores at the spec's +0x00 + type * 4. perfdb_upgrade_level() rounds it the
// way GetUpgradeLevel (0x801C41DC) does.
// Transmission and suspension carry three components, the junkman tier two,
// every other type one. 0 is what CarCustomizeManager::IsPerfStock (0x801715D8)
// calls stock.
float perfdb_upgrade_value(const PerfDb* db, const u8* installed, int type);
int perfdb_upgrade_level(const PerfDb* db, const u8* installed, int type);

// ComputeUpgradeLevel itself: the per-component sums into `comp` (clamped to
// 0..1 in place when their average is out of range, as retail does), and that
// average, 0..1, returned -- what every BuildXxxPackage (ridephys.c) lerps by.
// Components past the type's count are zero.
#define PERFDB_UPGRADE_COMPONENTS 3
float perfdb_upgrade_components(const PerfDb* db, const u8* installed, int type,
	float comp[PERFDB_UPGRADE_COMPONENTS]);

// CarCustomizeManager::IsDowngradedFromInstalled (0x80171688): the part
// replaces one that is installed and comes from a higher upgrade level.
bool perfdb_part_downgrades(const PerfDb* db, const u8* installed, int partId);
