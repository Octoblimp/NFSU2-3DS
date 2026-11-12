// One of retail's TrackPolygons (TASKS.md #223): a topology polygon as the
// collision bodies see it, decoded the way TrackPolygon::TrackPolygon
// (Speed.elf 0x801f0150) decodes the 32-byte record. topology.c decodes it,
// carsim.c collides the car's hull with it; it lives here so the physics
// need not include the world's headers.
#pragma once

#include <3ds.h>

typedef struct {
	// World positions, all four as the constructor decodes them (+0x24): an
	// unbounded wall's z replaced by -2048/+2048/+2048/-2048, and a
	// triangle's fourth the same as its third.
	float v[4][3];
	float n[3], d;   // normalize((v1 - v0) x (v2 - v0)) and v0 . n (+0x64, +0x70)
	float box[4];    // min x, min y, max x, max y, from the record's selectors (+0x14)
	int   nv;        // 3, or 4 for flags & 0x10 (+0x09)
	u8    flags;     // the record's flags byte (+0x0a): 2 wall, 4 unbounded, 8 not road
	u8    group;     // record byte +0, the topology group: 0 is single-sided
	                 // (TrackPolygonCollisionBody +0x64, FindWaferNormal)
	s8    terrain;   // record byte +2 (+0x0b, GetTerrainType)
} TrackPoly;
