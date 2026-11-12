#include "carpartdb.h"
#include "bunidx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See carpartdb.h for the layout. Every rule here is tools/stockparts_pack.py's
// Db, which was checked against the stock parts the PC game fits.

#define CH_SLOT_TYPES 0x00034607u
#define CH_CONTAINER  0x80034602u
#define CH_HEADER     0x00034603u
#define CH_STRINGS    0x00034606u
#define CH_ATTRLISTS  0x0003460Cu
#define CH_ATTRS      0x00034605u
#define CH_CAR_TYPES  0x0003460Bu
#define CH_PARTS      0x00034604u

#define HDR_NUM_ATTRS 8   // u32 index into the header
#define HDR_NUM_PARTS 14

// CarPartSlotMap (Speed.elf 0x803713EC): CAR_SLOT_ID -> the part id a record
// carries in its byte +4. The identity up to 27.
static const u8 kSlotMap[CARPARTDB_SLOTS] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
	17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 27, 28, 28, 29, 30, 31,
	60, 61, 61, 62, 62, 63, 63, 61, 61, 61, 61, 62, 62, 32, 33, 34, 35,
	36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 50, 50, 50, 50,
	48, 48, 48, 48, 48, 48, 48, 48, 48, 49, 49, 49, 49, 49, 49, 49, 49,
	49, 49, 49, 49, 49, 49, 49, 49, 51, 51, 51, 51, 52, 52, 52, 52, 52,
	52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52,
	52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52,
	52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52,
	53, 54, 54, 54, 54, 55, 56, 57, 58, 59, 64, 65, 65, 65, 66, 67, 68,
};

// IsUG1 / IsUG2 / IsSUV (0x80243914 / 0x80243A40 / 0x80243B44): the car-type
// hashes GetCarPartDirect checks a part's EXCLUDE_* attributes against.
static const u32 kUG1[] = {
	0x0000DB7Cu, 0x0000DC00u, 0x000AC6D1u, 0x00163087u, 0x0150FB80u, 0x0290C04Du,
	0x02CA04DFu, 0x03455D6Bu, 0x03B8C48Au, 0x04FD0522u, 0x545AEC60u, 0x72B2E513u,
	0x7502DA84u, 0x79AEBFECu, 0xBE48375Eu,
};
static const u32 kUG2[] = {
	0x00000453u, 0x000006E7u, 0x0000A86Eu, 0x0000ACC9u, 0x0000DC01u, 0x0293FAD7u,
	0x030236EEu, 0x09D2EFF6u, 0x2D7C9B5Du, 0x35165819u, 0x4B10070Au, 0xCF4458F8u,
	0xF77C13EBu,
};
static const u32 kSUV[] = { 0x0219C94Au, 0x6126DD4Du, 0xCF704771u };
#define KEY_EXCLUDE_UG1 0xF7934315u
#define KEY_EXCLUDE_UG2 0xF7934316u
#define KEY_EXCLUDE_SUV 0xF7933C86u

static u32 rd32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u16 rd16(const u8* p) {
	return (u16)((u32)p[0] | ((u32)p[1] << 8));
}

static bool inSet(const u32* set, int n, u32 v) {
	for (int i = 0; i < n; i++)
		if (set[i] == v) return true;
	return false;
}
#define IN_SET(set, v) inSet(set, (int)(sizeof(set) / sizeof(set[0])), v)

u32 carpartdb_string_hash_from(const char* s, u32 h) {
	while (*s)
		h = h * 33u + (u8)*s++;
	return h;
}

u32 carpartdb_string_hash(const char* s) {
	return carpartdb_string_hash_from(s, 0xFFFFFFFFu);
}

static u8* readChunk(const char* path, u32 type, u32* size) {
	long hdrOff = 0;
	*size = 0;
	if (!bunidx_find(path, type, &hdrOff, size) || *size == 0)
		return NULL;
	u8* body = (u8*)malloc(*size);
	if (!body)
		return NULL;
	FILE* f = fopen(path, "rb");
	const bool ok = f && fseek(f, hdrOff + 8, SEEK_SET) == 0 &&
		fread(body, 1, *size, f) == *size;
	if (f)
		fclose(f);
	if (!ok) {
		free(body);
		return NULL;
	}
	return body;
}

bool carpartdb_load(const char* bunPath, CarPartDb* db) {
	memset(db, 0, sizeof(*db));
	u32 tsize = 0;
	u8* types = readChunk(bunPath, CH_SLOT_TYPES, &tsize);
	if (!types)
		return false;
	bool ok = tsize >= CARPARTDB_SLOTS * 8 && (tsize - CARPARTDB_SLOTS * 8) % 16 == 0;
	if (ok) {
		for (int s = 0; s < CARPARTDB_SLOTS; s++) {
			db->slotTypes[s][0] = rd32(types + s * 8);
			db->slotTypes[s][1] = rd32(types + s * 8 + 4);
		}
		db->numOverrides = (int)((tsize - CARPARTDB_SLOTS * 8) / 16);
		if (db->numOverrides > 0) {
			db->overrides = (u32(*)[4])malloc((size_t)db->numOverrides * sizeof(*db->overrides));
			ok = db->overrides != NULL;
			for (int i = 0; ok && i < db->numOverrides; i++)
				for (int k = 0; k < 4; k++)
					db->overrides[i][k] = rd32(types + CARPARTDB_SLOTS * 8 + i * 16 + k * 4);
		}
	}
	free(types);

	u32 size = 0;
	if (ok)
		db->buf = readChunk(bunPath, CH_CONTAINER, &size);
	ok = ok && db->buf;
	const u8* header = NULL;
	u32 headerSize = 0, partBytes = 0, attrBytes = 0, typeBytes = 0;
	for (u32 o = 0; ok && o + 8 <= size; ) {
		const u32 id = rd32(db->buf + o), sz = rd32(db->buf + o + 4);
		if (sz == 0 || sz > size - (o + 8))
			break;
		const u8* body = db->buf + o + 8;
		switch (id) {
		case CH_HEADER:    header = body; headerSize = sz; break;
		case CH_STRINGS:   db->strings = body; db->stringBytes = sz; break;
		case CH_ATTRLISTS: db->attrLists = body; db->attrListBytes = sz; break;
		case CH_ATTRS:     db->attrs = body; attrBytes = sz; break;
		case CH_CAR_TYPES: db->carTypes = body; typeBytes = sz; break;
		case CH_PARTS:     db->parts = body; partBytes = sz; break;
		default: break;
		}
		o += 8 + sz;
	}
	ok = ok && header && headerSize >= (HDR_NUM_PARTS + 1) * 4 && db->strings &&
		db->attrLists && db->attrs && db->carTypes && db->parts;
	if (ok) {
		db->numAttrs = rd32(header + HDR_NUM_ATTRS * 4);
		db->numParts = rd32(header + HDR_NUM_PARTS * 4);
		db->numCarTypes = typeBytes / 4;
		// The parts chunk is padded to four: 12167 records are 170338 bytes
		// of its 170340.
		ok = db->numParts * CARPARTDB_PART_SIZE <= partBytes &&
			db->numAttrs * 8 <= attrBytes && db->numCarTypes <= 256;
	}
	if (ok) {
		for (int t = 0; t < 256; t++)
			db->firstOfType[t] = db->numParts;
		u32 prev = 0;
		for (u32 i = 0; ok && i < db->numParts; i++) {
			const u32 t = db->parts[i * CARPARTDB_PART_SIZE + CARPARTDB_PART_TYPE];
			// The lookup leans on the sort, as retail's binary search does.
			ok = t >= prev;
			prev = t;
			if (db->firstOfType[t] == db->numParts)
				db->firstOfType[t] = i;
		}
	}
	if (!ok)
		carpartdb_free(db);
	return ok;
}

void carpartdb_free(CarPartDb* db) {
	if (!db)
		return;
	free(db->buf);
	free(db->overrides);
	memset(db, 0, sizeof(*db));
}

u32 carpartdb_hash(CarPart p) {
	return p ? rd32(p + CARPARTDB_PART_HASH) : 0;
}

int carpartdb_level(CarPart p) {
	return p ? p[CARPARTDB_PART_LEVEL] >> 5 : 0;
}

int carpartdb_kit(CarPart p) {
	return p ? p[CARPARTDB_PART_LEVEL] & 0x1F : 0;
}

const char* carpartdb_name(const CarPartDb* db, CarPart p) {
	if (!p)
		return "-";
	const u32 o = (u32)rd16(p + CARPARTDB_PART_NAME) * 4;
	if (o >= db->stringBytes || !memchr(db->strings + o, 0, db->stringBytes - o))
		return "?";
	return (const char*)db->strings + o;
}

u32 carpartdb_attr(const CarPartDb* db, CarPart p, u32 key, u32 def) {
	if (!p)
		return def;
	const u32 li = rd16(p + CARPARTDB_PART_ATTRS);
	if ((li + 1) * 2 > db->attrListBytes)
		return def;
	const u32 n = rd16(db->attrLists + li * 2);
	for (u32 k = 0; k < n && (li + 2 + k) * 2 <= db->attrListBytes; k++) {
		const u32 ai = rd16(db->attrLists + (li + 1 + k) * 2);
		if (ai < db->numAttrs && rd32(db->attrs + ai * 8) == key)
			return rd32(db->attrs + ai * 8 + 4);
	}
	return def;
}

static u32 typeHash(const CarPartDb* db, CarPart p) {
	const u32 t = p[CARPARTDB_PART_TYPE];
	return t < db->numCarTypes ? rd32(db->carTypes + t * 4) : 0;
}

// GetTypesFromSlot (0x80243C9C): an override for this car first.
static void typesFromSlot(const CarPartDb* db, int slot, u32 carHash, u32 out[2]) {
	for (int i = 0; i < db->numOverrides; i++) {
		if (db->overrides[i][0] == carHash && (int)db->overrides[i][1] == slot) {
			out[0] = db->overrides[i][2];
			out[1] = db->overrides[i][3];
			return;
		}
	}
	for (int k = 0; k < 2; k++) {
		const u32 t = db->slotTypes[slot][k];
		out[k] = t == 0xFFFFFFFFu ? carHash : t;
	}
}

// GetCarPartDirect (0x80243BEC).
static CarPart getDirect(const CarPartDb* db, u32 type, u32 pid, u32 partHash,
		CarPart prev, int level) {
	u32 idx = 0;
	while (idx < db->numCarTypes && rd32(db->carTypes + idx * 4) != type)
		idx++;
	if (idx >= db->numCarTypes || idx > 0xFE)
		return NULL;
	u32 i = prev ? (u32)(prev - db->parts) / CARPARTDB_PART_SIZE + 1 : db->firstOfType[idx];
	const bool ug1 = IN_SET(kUG1, type), ug2 = IN_SET(kUG2, type), suv = IN_SET(kSUV, type);
	for (; i < db->numParts; i++) {
		CarPart p = db->parts + i * CARPARTDB_PART_SIZE;
		if (p[CARPARTDB_PART_TYPE] != idx)
			break;   // the records are sorted, so its run is over
		if (p[CARPARTDB_PART_PID] != pid)
			continue;
		if (partHash && rd32(p) != partHash)
			continue;
		if (ug1 && carpartdb_attr(db, p, KEY_EXCLUDE_UG1, 0))
			continue;
		if (ug2 && carpartdb_attr(db, p, KEY_EXCLUDE_UG2, 0))
			continue;
		if (suv && carpartdb_attr(db, p, KEY_EXCLUDE_SUV, 0))
			continue;
		if (level == CARPARTDB_ANY_LEVEL || carpartdb_level(p) == level)
			return p;
	}
	return NULL;
}

CarPart carpartdb_get(const CarPartDb* db, u32 carHash, int slot, u32 partHash,
		CarPart prev, int level) {
	if (!db || !db->parts || slot < 0 || slot >= CARPARTDB_SLOTS)
		return NULL;
	u32 types[2];
	typesFromSlot(db, slot, carHash, types);
	int first = 0;
	if (prev) {
		while (first < 2 && typeHash(db, prev) != types[first])
			first++;
		if (first == 2)
			return NULL;
	}
	for (int i = first; i < 2; i++) {
		if (!types[i])
			continue;
		CarPart r = getDirect(db, types[i], kSlotMap[slot], partHash,
			i == first ? prev : NULL, level);
		if (r)
			return r;
	}
	return NULL;
}

CarPart carpartdb_find_with_level(const CarPartDb* db, u32 carHash, int slot, int level) {
	CarPart p = carpartdb_get(db, carHash, slot, 0, NULL, CARPARTDB_ANY_LEVEL);
	while (p && carpartdb_level(p) != level)
		p = carpartdb_get(db, carHash, slot, 0, p, CARPARTDB_ANY_LEVEL);
	return p;
}
