#pragma once
#include <3ds.h>
#include <stdbool.h>

// CarPartDatabase (Speed.elf), the visual parts every car can wear: bumpers,
// hoods, spoilers, rims, paints, vinyls and their colours, 12,167 records in
// the shipped PC GLOBALB.BUN. TASKS.md #296 reads it at race setup so the AI
// cars can be dressed the way RideInfo::BuildRandomRide dresses them
// (rideinfo.h), and frees it once the field has its parts.
//
// Where it lives, all little-endian and read as it lies:
//
//   0x00034607             CAR_SLOT_ID -> the two car types a slot's parts
//                          are filed under (GetTypesFromSlot): 0xAA pairs of
//                          u32, 0xFFFFFFFF meaning "the car's own", then
//                          {car, slot, type0, type1} overrides to the end.
//   0x80034602 container   the database proper, children back to back:
//     0x00034603           header; u32 [8] the attribute count, [14] the
//                          part count
//     0x00034606           the name strings
//     0x0003460C           attribute lists: u16 count, then u16 indices
//     0x00034605           attributes, 8 bytes {key, value}
//     0x0003460B           the car-type name hashes a part's +7 indexes
//     0x00034604           the parts, 14 bytes each (CARPARTDB_PART_*)
//
// The part records are sorted by their car-type byte, which is what lets
// ScanHashTableKey8 binary-search it; this keeps the first record of each
// type instead. tools/stockparts_pack.py's Db is the same reader in Python
// (and tools/randomride_check.py the RideInfo built on it), and the C was
// written against it.

// CAR_SLOT_ID, the GameCube numbering: a save's PresetCarSlot index is one
// more (profile_save.h PROFILE_CAR_SLOTS).
#define CARPARTDB_SLOTS 0xAA

// Any upgrade level, NewGetCarPart's -1.
#define CARPARTDB_ANY_LEVEL (-1)

// The 14-byte part record.
#define CARPARTDB_PART_SIZE  14
#define CARPARTDB_PART_HASH  0   // u32, bStringHash of the name
#define CARPARTDB_PART_PID   4   // u8, the part id GetCarPartFromSlot maps a slot to
#define CARPARTDB_PART_LEVEL 5   // u8: level << 5 | kit (or vinyl group)
#define CARPARTDB_PART_TYPE  7   // u8, index into the car-type hashes
#define CARPARTDB_PART_NAME  8   // u16, the name's offset in the strings / 4
#define CARPARTDB_PART_ATTRS 10  // u16, its attribute list

// A part is a pointer to its record, NULL for none -- what RideInfo keeps.
typedef const u8* CarPart;

typedef struct {
	u8* buf;               // the container's body, malloc'd
	CarPart parts;
	u32 numParts;
	const u8* strings;
	u32 stringBytes;
	const u8* attrLists;
	u32 attrListBytes;
	const u8* attrs;
	u32 numAttrs;
	const u8* carTypes;    // u32 each
	u32 numCarTypes;
	// The first record of each car-type index, numParts for none.
	u32 firstOfType[256];
	u32 slotTypes[CARPARTDB_SLOTS][2];
	u32 (*overrides)[4];   // {car, slot, type0, type1}, malloc'd
	int numOverrides;
} CarPartDb;

// Reads both chunks out of `bunPath` (GLOBALB.BUN). False, with `db` empty,
// when either is missing or does not hold together.
bool carpartdb_load(const char* bunPath, CarPartDb* db);
void carpartdb_free(CarPartDb* db);

// CarPartDatabase::NewGetCarPart (0x80243D60): the next part after `prev`
// (NULL for the first) that car `carHash` can fit in `slot`, named `partHash`
// (0 for any) and of upgrade `level` (CARPARTDB_ANY_LEVEL for any).
// GetCarPartDirect's EXCLUDE_UG1 / _UG2 / _SUV checks included.
CarPart carpartdb_get(const CarPartDb* db, u32 carHash, int slot, u32 partHash,
	CarPart prev, int level);

// FindPartWithLevel: the first part for the slot whose level is `level`.
CarPart carpartdb_find_with_level(const CarPartDb* db, u32 carHash, int slot, int level);

// GetAppliedAttribute*Param: the value of the first attribute keyed `key`,
// or `def`.
u32 carpartdb_attr(const CarPartDb* db, CarPart p, u32 key, u32 def);

u32 carpartdb_hash(CarPart p);
int carpartdb_level(CarPart p);   // byte +5 >> 5
int carpartdb_kit(CarPart p);     // byte +5 & 0x1F
const char* carpartdb_name(const CarPartDb* db, CarPart p);

// bStringHash (seed 0xFFFFFFFF) and its carry-on form bStringHash(s, h).
u32 carpartdb_string_hash(const char* s);
u32 carpartdb_string_hash_from(const char* s, u32 h);
