#include "presetcar.h"
#include <stdio.h>
#include <string.h>

// See presetcar.h for where the table lives and how the engine reads it.
#define PRESETCAR_CHUNK_TYPE 0x00030220u
#define PR_OFF_CAR   0x008
#define PR_OFF_NAME  0x028
#define PR_OFF_PERFPKG 0x048
#define PR_OFF_PARTS 0x04C
#define PR_OFF_PERFPARTS 0x2F4

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

u32 presetcar_hash(const char* s) {
	u32 h = 0xFFFFFFFFu;
	for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
		unsigned char c = *p;
		if (c >= 'a' && c <= 'z')
			c = (unsigned char)(c - 0x20);
		h = h * 0x21u + (u32)c;
	}
	return h;
}

// The record's strings are fixed 32-byte fields; this keeps a copy terminated
// even if one were ever written full.
static void copyName(char* dst, const u8* src) {
	memcpy(dst, src, 31);
	dst[31] = '\0';
}

bool presetcar_load(const char* bunPath, u32 nameHash, PresetCar* out) {
	memset(out, 0, sizeof(*out));
	FILE* f = fopen(bunPath, "rb");
	if (!f)
		return false;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	const long fileSize = ftell(f);

	// The same top-level chunk walk cardb_load() does: one 8-byte read per
	// chunk rather than the whole ~8MB file.
	long off = 0, chunkOff = -1;
	u32 chunkSize = 0;
	while (off + 8 <= fileSize) {
		u8 hdr[8];
		if (fseek(f, off, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8)
			break;
		const u32 type = rd32le(hdr), size = rd32le(hdr + 4);
		if (type == PRESETCAR_CHUNK_TYPE) {
			chunkOff = off;
			chunkSize = size;
			break;
		}
		if ((type == 0 && size == 0) || (long)size > fileSize - (off + 8))
			break;
		off += 8 + (long)size;
	}
	if (chunkOff < 0 || (long)chunkSize > fileSize - (chunkOff + 8)) {
		fclose(f);
		return false;
	}

	// 0x61C3C0: records from chunk + 8, size / 0x338 of them.
	u8 rec[PRESETCAR_RECORD_SIZE];
	bool found = false;
	for (u32 i = 0; i < chunkSize / PRESETCAR_RECORD_SIZE && !found; i++) {
		if (fseek(f, chunkOff + 8 + (long)i * PRESETCAR_RECORD_SIZE, SEEK_SET) != 0 ||
		    fread(rec, 1, sizeof(rec), f) != sizeof(rec))
			break;
		char name[32];
		copyName(name, rec + PR_OFF_NAME);
		if (presetcar_hash(name) != nameHash)
			continue;
		copyName(out->car, rec + PR_OFF_CAR);
		memcpy(out->name, name, sizeof(out->name));
		out->carHash = presetcar_hash(out->car);
		out->parts[0] = out->carHash;
		for (int s = 0; s < PRESETCAR_GC_SLOTS; s++)
			out->parts[s + 1] = rd32le(rec + PR_OFF_PARTS + s * 4);
		out->perfPackage = (s32)rd32le(rec + PR_OFF_PERFPKG);
		memcpy(out->perfParts, rec + PR_OFF_PERFPARTS, sizeof(out->perfParts));
		found = true;
	}
	fclose(f);
	return found;
}
