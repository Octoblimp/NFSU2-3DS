#pragma once
#include <3ds.h>

// The car-paint colour table, out of GLOBAL/PAINT.nfsp (tools/paint_pack.py).
//
// NFSU2 does not store a colour in a save. A saved car's BASE_PAINT slot holds
// the bStringHash of a *part name* -- GLOSS_L3_TEST20, RIMS_L1_COLOR08 -- and
// the RGB lives in that part's attribute list in GLOBALB.BUN's car-parts
// database, keyed by the hashes of "RED", "GREEN" and "BLUE". Walking that
// database needs the whole 8MB bundle resident, so the packer does it offline
// and ships the 494 colour parts as a flat sorted table: about 4KB, and a
// binary search per lookup.
//
// This is the piece that lets a car wear the colour the player actually chose.
// tools/car_pack.py no longer bakes a paint colour into the body skin at all --
// it ships the two terms the composite is linear in -- so the colour this
// returns is the whole of what makes a car its own colour at runtime.

#define PAINT_MAGIC   0x5053464E  // 'NFSP' little-endian
// Version 2 carries each part's GLOSS in the byte version 1 left zero.
#define PAINT_VERSION 2

typedef struct {
	u8* buf;      // the whole file; entries point into it
	int count;
} PaintTable;

// Loads GLOBAL/PAINT.nfsp. Returns false if the file is missing or malformed,
// in which case `out` is zeroed and paint_lookup() on it simply finds nothing
// -- a car then keeps the fallback colour rather than failing to draw.
bool paint_load(const char* path, PaintTable* out);

void paint_free(PaintTable* t);

// The colour for a part hash as 0xRRGGBB, or `fallback` if this table has no
// such part. A zero hash is an empty slot and always takes the fallback.
u32 paint_lookup(const PaintTable* t, u32 partHash, u32 fallback);

// The same part the way the car-skin composite reads it (CompositeSkin,
// SPEED2.EXE 0x61D790, then FUN_005BE040): 0xAARRGGBB with GLOSS as the
// alpha -- 0 from a version 1 table, which has none. `fallback` as above.
u32 paint_lookup_argb(const PaintTable* t, u32 partHash, u32 fallback);
