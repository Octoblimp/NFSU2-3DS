// The car light materials (TASKS.md #166): what a car's surfaces are made of
// as far as its lighting goes.
//
// Every sub-mesh of a car names an eLightMaterial -- each paint colour
// (REGPAINT*, METPAINT*, PEARL*, CUSTOMPAINT_*), CHROME, CARBONFIBER, the
// glasses, the window tints, DEFAULT -- and the GameCube lights the car per
// vertex from it (epCalculateSphericalHarmonicsPOS16, Speed.elf 0x801e1c90)
// and combines the result with the reflection in ps_Model mode 7
// (0x801e6d3c). The 186 of them are chunk 0x135200 of GLOBALB.BUN, which
// tools/lightmat_pack.py copies to GLOBAL/LIGHTMATS.nfsl; this is the values
// UpdatePlatInfo (0x801e2a64) derives from each record, which are the ones
// those two read.
#pragma once

#include <3ds.h>
#include <stdbool.h>

// bStringHash("DEFAULT"): what elGetLightMaterial (0x800cd304) hands back for
// a name the table does not have. LoaderLights (0x800c9088) keeps it aside as
// the record of that name.
#define LIGHTMAT_DEFAULT 0x2c420d64u

typedef struct {
	u32 hash;
	char name[28];
	// UpdatePlatInfo's platinfo, by offset:
	float diffMin[3];    // +0x00 DiffMin rgb * its scale
	float minA;          // +0x0c
	float diffRange[3];  // +0x10 DiffMax * scale - DiffMin * scale
	float rangeA;        // +0x1c MaxA - MinA
	float specMin[3];    // +0x20
	float specRange[3];  // +0x2c
	float specPower;     // +0x38
	float envPower;      // +0x3c
	u8 k0[3];            // +0x48 the reflection's two colours, sat(scale * rgb * 128)
	u8 k1[3];            // +0x4c
} LightMat;

// Loads `<dataRoot>/GLOBAL/LIGHTMATS.nfsl` once; later calls are free.
// False, and every lookup NULL, without one.
bool lightmat_load(const char* dataRoot);

// The material named `hash`, or NULL.
const LightMat* lightmat_find(u32 hash);

// elGetLightMaterial: the material named `hash`, else DEFAULT; NULL only with
// no table at all.
const LightMat* lightmat_get(u32 hash);

// Its index in the table, for a batch to carry (0xff for NULL), and back.
u8 lightmat_index(const LightMat* m);
const LightMat* lightmat_at(u8 index);
