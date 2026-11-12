// The world's light database (TASKS.md #166): the street lamps that light a
// car, and the cars' own head- and tail-lights.
//
// Every streamed section of the city carries a light pack (STREAML4RA.BUN
// chunk 0x80135000): its lamps and an AABB tree over them. LoaderLights
// (Speed.elf 0x800c8700) links a pack into LightPackList while its section
// is loaded, and each frame elSetupLights (0x800cb290) asks
// QueryLightDatabase (0x800c9ee4) which lights reach a car and folds them
// into its harmonics beside the shaper rig (model3d.c, carLightContext()).
// tools/lightdb_pack.py copies L4RA's 249 packs to TRACKS/L4RA/WORLD.NFSL;
// without the file a car is lit by the rig alone.
#pragma once

#include <3ds.h>
#include <stdbool.h>

// retail's eLight, the fields the query and elSetupLights read (48 bytes,
// as WORLD.NFSL holds them; the 0x60-byte original is in lightdb_pack.py).
typedef struct {
	u32   hash;
	u8    type;       // 0 ambient, 2 omni, 3 spot
	u8    b5, b6;
	u8    state;      // 1 = on; QueryLightHelper skips any other
	u32   colour;     // R | G << 8 | B << 16, as elSetupLights reads it
	float pos[3];
	float radius;     // reaches nothing at or past this
	float dir[3];     // a spot's axis
	float inner;      // full strength inside this
	float cone;       // a spot's angle, radians
} LightDbLight;

#define LIGHTDB_TYPE_AMBIENT 0
#define LIGHTDB_TYPE_OMNI    2
#define LIGHTDB_TYPE_SPOT    3

// eLightQuery: the lights QueryLightHelper accepted and their weights.
// Twenty-four is retail's cap; the static packs stop at ten of them.
#define LIGHTDB_QUERY_MAX 24
typedef struct {
	int n;
	const LightDbLight* light[LIGHTDB_QUERY_MAX];
	float weight[LIGHTDB_QUERY_MAX];
} LightDbQuery;

typedef struct LightDb LightDb;

// Loads WORLD.NFSL. NULL (and a line to `log`, which may be NULL) when it is
// missing or malformed.
LightDb* lightdb_open(const char* path, void (*log)(const char* msg));
void lightdb_close(LightDb* db);

void lightdb_query_reset(LightDbQuery* q);

// QueryLightDatabase's static half: for each pack whose section `loaded`
// says is resident (by retail number, letter * 100 + n), the lights listed by
// the first leaf of its tree holding `pos`, until ten are taken.
void lightdb_query_static(LightDbQuery* q, const LightDb* db, const float pos[3],
	bool (*loaded)(int section, void* ud), void* ud);

// QueryLightHelper for one light, which is how the dynamic half -- the cars'
// head- and tail-lights -- goes in.
void lightdb_query_light(LightDbQuery* q, const LightDbLight* L, const float pos[3]);

// elSetupLights' colour for the query's light `i` seen from `pos`: its RGB
// times the weight, 0.75 and a spot's cone falloff, still in 0..255 units.
void lightdb_hit_colour(const LightDbQuery* q, int i, const float pos[3], float rgb[3]);
