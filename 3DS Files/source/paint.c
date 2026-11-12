#include "paint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 0x00 magic 'NFSP', 0x04 version, 0x08 count, 0x0c reserved, then
// count * { u32 partHash, u8 r, u8 g, u8 b, u8 gloss } sorted by hash (a
// version 1 file has a zero there).
#define PAINT_HDR 0x10
#define PAINT_REC 8

static u32 rd32(const u8* p, size_t o) {
	return (u32)p[o] | ((u32)p[o + 1] << 8) | ((u32)p[o + 2] << 16) |
	       ((u32)p[o + 3] << 24);
}

bool paint_load(const char* path, PaintTable* out) {
	memset(out, 0, sizeof(*out));
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < PAINT_HDR) {
		fclose(f);
		return false;
	}
	u8* buf = (u8*)malloc((size_t)size);
	if (!buf) {
		fclose(f);
		return false;
	}
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return false;
	}
	fclose(f);

	const u32 version = rd32(buf, 4);
	if (rd32(buf, 0) != PAINT_MAGIC || (version != 1 && version != PAINT_VERSION)) {
		free(buf);
		return false;
	}
	u32 count = rd32(buf, 8);
	// The count has to fit the file, or the binary search below reads past it.
	if ((u64)PAINT_HDR + (u64)count * PAINT_REC > (u64)size) {
		free(buf);
		return false;
	}
	out->buf = buf;
	out->count = (int)count;
	return true;
}

void paint_free(PaintTable* t) {
	if (!t)
		return;
	free(t->buf);
	t->buf = NULL;
	t->count = 0;
}

static const u8* find(const PaintTable* t, u32 partHash) {
	if (!t || !t->buf || !partHash)
		return NULL;
	int lo = 0, hi = t->count - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		const u8* e = t->buf + PAINT_HDR + (size_t)mid * PAINT_REC;
		u32 h = rd32(e, 0);
		if (h == partHash)
			return e;
		if (h < partHash)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

u32 paint_lookup(const PaintTable* t, u32 partHash, u32 fallback) {
	const u8* e = find(t, partHash);
	return e ? ((u32)e[4] << 16) | ((u32)e[5] << 8) | (u32)e[6] : fallback;
}

u32 paint_lookup_argb(const PaintTable* t, u32 partHash, u32 fallback) {
	const u8* e = find(t, partHash);
	return e ? ((u32)e[7] << 24) | ((u32)e[4] << 16) | ((u32)e[5] << 8) | (u32)e[6]
	         : fallback;
}
