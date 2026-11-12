#pragma once
#include <3ds.h>
#include <stdbool.h>

// Retail's TrackPathManager zone list -- the polygons that make free roam
// react to where the car is (CONTINUE.MD E5, TASKS.md #214).
//
// This is the thing the Engage widget's tab and up arrow have been waiting on:
// engage.h says they are "not drawn, because the port has nothing to drive
// them with yet ... they need an engage event, which needs the free-roam
// triggers of CONTINUE.MD E5". A trigger is a zone of the right type with the
// car inside it.
//
// Where the data lives
// --------------------
// TRACKS/ROUTESL4RA/PathsFreeRoam.bin, copied as-is from the PC install -- the
// same arrangement world.c uses for TRACKS/L4RA.BUN. The file is one
// 0x80034147 container (TrackPathManager::Loader, Speed.elf 0x802016E8) with
// two sub-chunks:
//
//   0x0003414A  the zone list, variable-stride records  (69628 bytes, 671 zones)
//   0x0003414D  0x24-byte records                       (64368 bytes, 1788)
//
// Only 0x3414A is parsed here. 0x3414D goes to manager+0x4A4 and nothing on the
// trigger path reads it, so it is left alone rather than guessed at.
//
// The record, decoded from the loader's own endian swaps
// ------------------------------------------------------
// The GameCube loader byte-swaps this file field by field on load, which names
// every field's width for free -- a swap of a bVector2 is two floats, a `sth`
// is a short, and anything it does not touch is bytes. Walking 0x80201A88 ..
// 0x80201BB8 gives, at 0x44 bytes plus the polygon:
//
//   +0x00 u32       type, one of TrackPathZoneType below
//   +0x04 float[2]  entry point: where the car is put, and where the world-map
//                   marker goes. Not always inside the polygon.
//   +0x0C float[2]  entry facing, a unit vector
//   +0x14 float     entry point Z: the height UpdateTriggerEffects
//                   (SPEED2.EXE 0x605780) stands a zone's marker effects at;
//                   0 means ask the ground (SetEffectLocation, 0x6054b0)
//   +0x18 u8        a small enum, 0..2; +0x19 is always 0
//   +0x1A u16       unread here (the loader swaps it, so it is a short)
//   +0x20 float[2]  bbMin, the polygon's own XY extent
//   +0x28 float[2]  bbMax
//   +0x30 u32       data handle -- the key that names what this zone opens
//   +0x34 s32       entry point X, rounded to metres
//   +0x38 s32       entry point Y
//   +0x3C u32       unread here
//   +0x40 s16       numPoints
//   +0x42 s16       stride, the whole record's size in bytes
//   +0x44 float[2][numPoints]  the polygon, XY
//
// Checked against the shipped PC file rather than taken on trust: all 671
// records have stride == 0x44 + 8*numPoints, the walk ends on the chunk's last
// byte exactly, the +0x20/+0x28 box equals the polygon's own extent for every
// one of them, and the list is sorted ascending by +0x00 -- which is what
// TrackPathManager::FixupPointers (0x80201DC4) assumes when it builds the
// per-type index this file rebuilds below.
//
// What the handle at +0x30 names
// ------------------------------
// FindTrackPathZoneByDataHandle (0x801215BC) scans types 13/14/15 (the mask it
// passes FindZone is 0xE000) and matches zone+0x30 against the career tables.
// Which field of which table was settled by scoring every 4-byte offset of
// every chunk in the shipped GLOBALB.BUN against the shipped handles:
//
//   RACE_START_TRIGGER  196 zones -> chunk 0x34A11, CareerEventData, stride 0x88
//   SHOP_TRIGGER         23 zones -> chunk 0x34A12, stride 0xA0
//   SMS_TRIGGER          56 zones -> chunk 0x34A17, stride 0x14
//   BANK_TRIGGER         59 zones -> chunk 0x34A1E, stride 0x0C
//
// 123 of the 196 race handles are in 0x34A11 and 30 more in 0x34A16, so one
// event table does not cover them all; resolving a handle to an event is the
// next task's job (E4) and nothing here depends on it. NEIGHBOURHOOD handles
// are in none of the BUNs -- GetNeighbourhoodNameInfo (0x801F3F1C) resolves
// those, and that table has not been found yet.
//
// One handle outside those tables: the shipped data's single RESET_TO_POINT
// (zone 0 of every Paths file, a polygon over the south of Bayview) carries
// 0x728558D7, bStringHash("PCDEMO") -- the PC demo's fence. The PC build's
// MaybeResetToTrackPathZone (SPEED2.EXE 0x5FBE00) skips it; race.c does too.
//
// Note on the field at +0x08 of a career event record: eventdb.h calls it
// `eventId` and it is what FindCareerEventDataByHandle (0x80121634) compares,
// but it is NOT this handle -- none of the 196 zone handles appear at +0x08 in
// the shipped table, while 116 of them appear at +0x28. Two different handles
// over the same records, and the zone's is +0x28.

typedef enum {
	TPZ_NO_WRONG_WAY = 0,
	TPZ_WRONG_WAY,
	TPZ_RESET,
	TPZ_RESET_TO_POINT,
	TPZ_GUIDED_RESET,
	TPZ_TUNNEL,
	TPZ_OVERPASS,
	TPZ_OVERPASS_SMALL,
	TPZ_DRIFT_GATE,
	TPZ_DRIFT_HOT_SPOT,
	TPZ_TUTORIAL,
	TPZ_SHORTCUT,
	TPZ_STREAMER_PREDICTION,
	TPZ_RACE_START_TRIGGER,
	TPZ_SHOP_TRIGGER,
	TPZ_SHOWCASE_TRIGGER,
	TPZ_TRAFFIC_TYPE,
	TPZ_NEIGHBOURHOOD,
	TPZ_SMS_TRIGGER,
	TPZ_BANK_TRIGGER,
	TPZ_GARAGE,
	TPZ_TYPE_COUNT
} TrackPathZoneType;

// Speed.elf's own TrackPathZoneTypeNames (0x8036FD14), for the debug readout.
// NULL past TPZ_TYPE_COUNT.
const char* trackpath_type_name(u32 type);

// The file image, read in place. Packed because it is the shipped record, not
// a struct this project chose; the _Static_assert in trackpath.c is what keeps
// the compiler honest about that.
typedef struct __attribute__((packed)) {
	u32   type;
	float entry[2];
	float dir[2];
	float entryZ;       // +0x14, see above
	u8    kind;
	u8    pad19;
	u16   unk1A;
	u8    unk1C[4];
	float bbMin[2];
	float bbMax[2];
	u32   unk30[4];     // [0] is the data handle; [1],[2] the rounded entry
	s16   numPoints;
	s16   stride;
	// float points[numPoints][2] follows.
} TrackPathZone;

#define TPZ_HEADER_BYTES 0x44

// The zone's polygon, numPoints pairs of XY. Valid while the TrackPaths that
// owns the zone is open.
static inline const float* trackpath_points(const TrackPathZone* z) {
	return (const float*)((const u8*)z + TPZ_HEADER_BYTES);
}
static inline u32 trackpath_handle(const TrackPathZone* z) {
	return z->unk30[0];
}

typedef struct TrackPaths TrackPaths;

// Opens TRACKS/ROUTESL4RA/PathsFreeRoam.bin. Returns NULL when the file is
// absent or malformed, which is "no triggers", not fatal -- a card packed
// before this build has no ROUTESL4RA directory at all, and free roam has to
// keep working on it. `log` may be NULL.
TrackPaths* trackpath_open(const char* path, void (*log)(const char* msg));
void        trackpath_close(TrackPaths* tp);

// TrackPathManager::FindZone (0x802023C4): the first zone of `type` whose box
// contains (x,y) and whose polygon does too. NULL when the car is in none.
//
// `after` continues a previous search from the zone after it, which is how
// retail enumerates overlapping zones; pass NULL for the first.
const TrackPathZone* trackpath_find(const TrackPaths* tp, float x, float y,
	u32 type, const TrackPathZone* after);

// Every zone of one type, in file order, for the map layer and the debug
// readout. `*outCount` may be 0.
const TrackPathZone* trackpath_first(const TrackPaths* tp, u32 type,
	int* outCount);

// Walks from one zone to the next of the same type (the records are variable
// length, so pointer arithmetic will not do it). NULL past the last.
const TrackPathZone* trackpath_next(const TrackPaths* tp,
	const TrackPathZone* z);

int trackpath_zone_count(const TrackPaths* tp);
int trackpath_type_count(const TrackPaths* tp, u32 type);
