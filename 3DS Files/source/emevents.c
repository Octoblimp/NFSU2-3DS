// See emevents.h. The file layout is documented at the top of
// tools/emevents_pack.py.
#include "emevents.h"
#include "vaabb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFSE_MAGIC   0x4553464Eu  // 'NFSE' little-endian
#define NFSE_VERSION 1
#define NFSE_HDR     0x20
#define NFSE_PACK    16
#define NFSE_EVENT   24
#define NFSE_MAX     (2u * 1024 * 1024)

typedef struct {
	u16 section;
	u16 nodes;
	u32 firstNode;
	u32 firstEvent;
	u32 events;
} EventPack;

struct EmEvents {
	u8*        blob;       // the whole file; the tables point into it
	EventPack* packs;
	int        packCount;
	const u8*  nodes;      // retail's 0x30-byte vAABB nodes
	EmEvent*   events;
	u32        eventCount;
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
	snprintf(b, sizeof(b), "emevents: %s %s", path, why);
	log(b);
}

EmEvents* emevents_open(const char* path, void (*log)(const char* msg)) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		say(log, path, "missing (run tools/emevents_pack.py) -- no roadside wooshes");
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* blob = (size >= NFSE_HDR && (u32)size <= NFSE_MAX) ? (u8*)malloc((size_t)size) : NULL;
	const bool read = blob && fread(blob, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	if (!read) {
		free(blob);
		say(log, path, "unreadable");
		return NULL;
	}
	const u32 packs = rd32(blob + 0x08), nodes = rd32(blob + 0x0c), events = rd32(blob + 0x10);
	const u32 po = rd32(blob + 0x14), no = rd32(blob + 0x18), eo = rd32(blob + 0x1c);
	bool ok = rd32(blob) == NFSE_MAGIC && rd32(blob + 4) == NFSE_VERSION &&
		packs && packs < 4096 && nodes < 65536 && events < 65536 &&
		(u64)po + (u64)packs * NFSE_PACK <= (u64)size &&
		(u64)no + (u64)nodes * VAABB_NODE <= (u64)size &&
		(u64)eo + (u64)events * NFSE_EVENT <= (u64)size && (eo & 3) == 0;
	EmEvents* db = ok ? (EmEvents*)calloc(1, sizeof(EmEvents)) : NULL;
	EventPack* pk = ok ? (EventPack*)malloc(packs * sizeof(EventPack)) : NULL;
	if (!db || !pk) {
		free(db);
		free(pk);
		free(blob);
		say(log, path, ok ? "out of memory" : "malformed or the wrong version");
		return NULL;
	}
	for (u32 i = 0; i < packs && ok; i++) {
		const u8* r = blob + po + i * NFSE_PACK;
		EventPack* p = &pk[i];
		p->section = rd16(r);
		p->nodes = rd16(r + 2);
		p->firstNode = rd32(r + 4);
		p->firstEvent = rd32(r + 8);
		p->events = rd32(r + 12);
		ok = p->nodes && (u64)p->firstNode + p->nodes <= nodes &&
			(u64)p->firstEvent + p->events <= events;
		// Every index a node holds stays inside its own pack, so the test
		// never has to check one again.
		for (u32 k = 0; k < p->nodes && ok; k++) {
			const u8* n = blob + no + (p->firstNode + k) * VAABB_NODE;
			const s16 cnt = (s16)rd16(n + 0x0e);
			const int m = cnt < 0 ? -cnt : cnt;
			ok = m <= 10;
			for (int j = 0; j < m && ok; j++) {
				const s16 x = (s16)rd16(n + 0x1c + j * 2);
				ok = x >= 0 && (u32)x < (cnt > 0 ? (u32)p->nodes : p->events);
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
	// Little-endian words on a little-endian CPU, 4-aligned in a malloc'd
	// blob: the triggers are read in place.
	db->events = (EmEvent*)(blob + eo);
	db->eventCount = events;
	if (log) {
		char b[600];
		snprintf(b, sizeof(b), "emevents: %s -- %d packs, %lu nodes, %lu triggers", path,
			db->packCount, (unsigned long)nodes, (unsigned long)events);
		log(b);
	}
	return db;
}

void emevents_close(EmEvents* db) {
	if (!db)
		return;
	free(db->packs);
	free(db->blob);
	free(db);
}

int emevents_trigger(const EmEvents* db, const float p[3], int section,
		bool (*loaded)(int section, void* ud), void* ud, const EmEvent** out, int max) {
	if (!db || section <= 0)
		return 0;
	// The section's user info (TheVisibleSectionManager +0x50 [section]) holds
	// its pack while the section is loaded.
	const EventPack* pk = NULL;
	for (int k = 0; k < db->packCount && !pk; k++)
		if (db->packs[k].section == section)
			pk = &db->packs[k];
	if (!pk || (loaded && !loaded(section, ud)))
		return 0;
	const u8* leaf = vaabb_query_leaf(db->nodes + (size_t)pk->firstNode * VAABB_NODE, p);
	if (!leaf)
		return 0;
	const int m = vaabb_leaf_count(leaf);
	int n = 0;
	for (int i = 0; i < m && n < max && n < EMEVENTS_MAX; i++) {
		const EmEvent* e = &db->events[pk->firstEvent + vaabb_leaf_item(leaf, i)];
		const float dx = p[0] - e->pos[0], dy = p[1] - e->pos[1], dz = p[2] - e->pos[2];
		if (dx * dx + dy * dy + dz * dz < e->radius * e->radius)
			out[n++] = e;
	}
	return n;
}
