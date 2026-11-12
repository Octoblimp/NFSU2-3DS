// See lightdb.h. The file layout is documented at the top of
// tools/lightdb_pack.py.
#include "lightdb.h"
#include "vaabb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFLD_MAGIC   0x444C464Eu  // 'NFLD' little-endian
#define NFLD_VERSION 1
#define NFLD_HDR     0x20
#define NFLD_PACK    16
#define NFLD_NODE    VAABB_NODE
#define NFLD_LIGHT   48
#define NFLD_MAX     (2u * 1024 * 1024)

// QueryLightDatabase stops taking static lights once ten are in (it loops
// while the count is at most 9).
#define STATIC_MAX 10

typedef struct {
	u16 section;
	u16 nodes;
	u32 firstNode;
	u32 firstLight;
	u32 lights;
} LightPack;

struct LightDb {
	u8*           blob;       // the whole file; the tables point into it
	LightPack*    packs;
	int           packCount;
	const u8*     nodes;      // retail's 0x30-byte vAABB nodes
	u32           nodeCount;
	LightDbLight* lights;
	u32           lightCount;
};

static u32 rd32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd16(const u8* p) {
	return (u16)(p[0] | (p[1] << 8));
}

static void say(void (*log)(const char*), const char* path, const char* why) {
	if (!log)
		return;
	char b[600];
	snprintf(b, sizeof(b), "lightdb: %s %s", path, why);
	log(b);
}

LightDb* lightdb_open(const char* path, void (*log)(const char* msg)) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		say(log, path, "missing (run tools/lightdb_pack.py) -- cars lit by the rig alone");
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* blob = (size >= NFLD_HDR && (u32)size <= NFLD_MAX) ? (u8*)malloc((size_t)size) : NULL;
	const bool read = blob && fread(blob, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	if (!read) {
		free(blob);
		say(log, path, "unreadable");
		return NULL;
	}
	const u32 packs = rd32(blob + 0x08), nodes = rd32(blob + 0x0c), lights = rd32(blob + 0x10);
	const u32 po = rd32(blob + 0x14), no = rd32(blob + 0x18), lo = rd32(blob + 0x1c);
	bool ok = rd32(blob) == NFLD_MAGIC && rd32(blob + 4) == NFLD_VERSION &&
		packs && packs < 4096 && nodes < 65536 && lights < 65536 &&
		(u64)po + (u64)packs * NFLD_PACK <= (u64)size &&
		(u64)no + (u64)nodes * NFLD_NODE <= (u64)size &&
		(u64)lo + (u64)lights * NFLD_LIGHT <= (u64)size && (lo & 3) == 0;
	LightDb* db = ok ? (LightDb*)calloc(1, sizeof(LightDb)) : NULL;
	LightPack* pk = ok ? (LightPack*)malloc(packs * sizeof(LightPack)) : NULL;
	if (!db || !pk) {
		free(db);
		free(pk);
		free(blob);
		say(log, path, ok ? "out of memory" : "malformed or the wrong version");
		return NULL;
	}
	for (u32 i = 0; i < packs && ok; i++) {
		const u8* r = blob + po + i * NFLD_PACK;
		LightPack* p = &pk[i];
		p->section = rd16(r);
		p->nodes = rd16(r + 2);
		p->firstNode = rd32(r + 4);
		p->firstLight = rd32(r + 8);
		p->lights = rd32(r + 12);
		ok = p->nodes && (u64)p->firstNode + p->nodes <= nodes &&
			(u64)p->firstLight + p->lights <= lights;
		// Every index a node holds stays inside its own pack, so the query
		// never has to check one again.
		for (u32 k = 0; k < p->nodes && ok; k++) {
			const u8* n = blob + no + (p->firstNode + k) * NFLD_NODE;
			const s16 cnt = (s16)rd16(n + 0x0e);
			const int m = cnt < 0 ? -cnt : cnt;
			ok = m <= 10;
			for (int j = 0; j < m && ok; j++) {
				const s16 x = (s16)rd16(n + 0x1c + j * 2);
				ok = x >= 0 && (u32)x < (cnt > 0 ? (u32)p->nodes : p->lights);
			}
		}
	}
	if (!ok) {
		free(pk);
		free(db);
		free(blob);
		say(log, path, "has an index out of range");
		return NULL;
	}
	db->blob = blob;
	db->packs = pk;
	db->packCount = (int)packs;
	db->nodes = blob + no;
	db->nodeCount = nodes;
	// Little-endian floats on a little-endian CPU, 4-aligned in a malloc'd
	// blob: the records are read in place.
	db->lights = (LightDbLight*)(blob + lo);
	db->lightCount = lights;
	if (log) {
		char b[600];
		snprintf(b, sizeof(b), "lightdb: %s -- %d packs, %lu nodes, %lu lights", path,
			db->packCount, (unsigned long)nodes, (unsigned long)lights);
		log(b);
	}
	return db;
}

void lightdb_close(LightDb* db) {
	if (!db)
		return;
	free(db->packs);
	free(db->blob);
	free(db);
}

void lightdb_query_reset(LightDbQuery* q) {
	q->n = 0;
}

// QueryLightHelper (0x800c9d00) for an omni or a spot. Its type-0 branch
// keeps the ambient lights aside in the query, which elSetupLights never
// reads (and L4RA ships none), so they are not kept here.
void lightdb_query_light(LightDbQuery* q, const LightDbLight* L, const float pos[3]) {
	if (L->state != 1)
		return;
	const float dx = pos[0] - L->pos[0], dy = pos[1] - L->pos[1], dz = pos[2] - L->pos[2];
	const float d2 = dx * dx + dy * dy + dz * dz;
	const float r2 = L->radius * L->radius;
	if (d2 >= r2)
		return;
	const float i2 = L->inner * L->inner;
	// Full strength inside the inner radius, then falling off linearly in the
	// squared distance to nothing at the outer one.
	const float w = d2 > i2 ? 1.0f - (d2 - i2) / (r2 - i2) : 1.0f;
	if (L->type != LIGHTDB_TYPE_OMNI && L->type != LIGHTDB_TYPE_SPOT)
		return;
	if (q->n >= LIGHTDB_QUERY_MAX)
		return;
	q->light[q->n] = L;
	q->weight[q->n] = w;
	q->n++;
}

void lightdb_query_static(LightDbQuery* q, const LightDb* db, const float pos[3],
		bool (*loaded)(int section, void* ud), void* ud) {
	if (!db)
		return;
	// PORT: retail walks LightPackList, which holds the packs in the order
	// their sections loaded; this walks them in STREAM file order. The order
	// only matters past the tenth light, which a street lamp's 30 m reach
	// does not get to.
	for (int k = 0; k < db->packCount && q->n < STATIC_MAX; k++) {
		const LightPack* p = &db->packs[k];
		if (loaded && !loaded(p->section, ud))
			continue;
		const u8* nodes = db->nodes + (size_t)p->firstNode * NFLD_NODE;
		const u8* leaf = vaabb_query_leaf(nodes, pos);
		if (!leaf)
			continue;
		const int m = vaabb_leaf_count(leaf);
		for (int i = 0; i < m && q->n < STATIC_MAX; i++)
			lightdb_query_light(q, &db->lights[p->firstLight + vaabb_leaf_item(leaf, i)], pos);
	}
}

void lightdb_hit_colour(const LightDbQuery* q, int i, const float pos[3], float rgb[3]) {
	const LightDbLight* L = q->light[i];
	float s = 1.0f;
	if (L->type == LIGHTDB_TYPE_SPOT) {
		// bSin(cone / 2 + pi/2), the cosine of the half-angle, through bSin's
		// 16-bit angle (Speed.elf 0x8005930c: (int)(rad * 10430.378)).
		const float rad = L->cone * 0.5f + 1.5707964f;
		const u16 a = (u16)(int)(rad * 10430.378f);
		const float cosCut = sinf((float)a * (2.0f * (float)M_PI / 65536.0f));
		float t[3] = { pos[0] - L->pos[0], pos[1] - L->pos[1], pos[2] - L->pos[2] };
		const float tl = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
		if (tl > 0.0f)
			for (int c = 0; c < 3; c++)
				t[c] /= tl;
		const float dd = L->dir[0] * t[0] + L->dir[1] * t[1] + L->dir[2] * t[2];
		s = dd > cosCut ? (dd - cosCut) / (1.0f - cosCut) : 0.0f;
	}
	// The light's intensity (+0x2c) is not read here, as in retail.
	const float k = q->weight[i] * 0.75f * s;
	rgb[0] = (float)(L->colour & 0xffu) * k;
	rgb[1] = (float)((L->colour >> 8) & 0xffu) * k;
	rgb[2] = (float)((L->colour >> 16) & 0xffu) * k;
}
