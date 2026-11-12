#include "carhull.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// CARHULLS.nfsg, see tools/hull_pack.py: "NFSG", version 1, count, 0, then
// { u32 hash; u32 numVerts; f32 pos[numVerts][3]; } per hull.
#define CARHULL_MAGIC   0x4753464Eu   // "NFSG"
#define CARHULL_VERSION 1u
#define CARHULL_MAX     64

typedef struct {
	u32 hash;
	int n;
	const float* v;
} Hull;

static bool s_tried;
static float* s_data;
static Hull s_hull[CARHULL_MAX];
static int s_count;

static u32 rd32(const u8* p) {
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

// bStringHash, and its carry-on form bStringHash(s, h).
static u32 stringHash(const char* s, u32 h) {
	for (; *s; s++) h = h * 33u + (u8)*s;
	return h;
}

void carhull_load(const char* bunPath) {
	if (s_tried) return;
	s_tried = true;
	char path[256];
	const char* slash = strrchr(bunPath, '/');
	const int dir = slash ? (int)(slash - bunPath) + 1 : 0;
	if (dir + (int)sizeof("CARHULLS.nfsg") > (int)sizeof(path)) return;
	memcpy(path, bunPath, (size_t)dir);
	strcpy(path + dir, "CARHULLS.nfsg");

	FILE* f = fopen(path, "rb");
	if (!f) return;
	u8* all = NULL;
	long size = 0;
	if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 16 && fseek(f, 0, SEEK_SET) == 0 &&
			(all = (u8*)malloc((size_t)size)) && fread(all, 1, (size_t)size, f) != (size_t)size) {
		free(all);
		all = NULL;
	}
	fclose(f);
	if (!all) return;
	if (rd32(all) != CARHULL_MAGIC || rd32(all + 4) != CARHULL_VERSION) {
		free(all);
		return;
	}

	// The floats are copied out to an aligned block: the file's are only
	// 4-aligned by luck of the layout.
	const int count = (int)rd32(all + 8);
	s_data = (float*)malloc((size_t)size);
	if (!s_data) {
		free(all);
		return;
	}
	long at = 16;
	int used = 0;
	for (int i = 0; i < count && s_count < CARHULL_MAX && at + 8 <= size; i++) {
		const u32 hash = rd32(all + at);
		const int n = (int)rd32(all + at + 4);
		at += 8;
		if (n < 4 || n > CARHULL_MAX_VERTS || at + (long)n * 12 > size) break;
		float* v = s_data + used;
		memcpy(v, all + at, (size_t)n * 12);
		at += (long)n * 12;
		used += n * 3;
		s_hull[s_count].hash = hash;
		s_hull[s_count].n = n;
		s_hull[s_count].v = v;
		s_count++;
	}
	free(all);
}

int carhull_count(void) {
	return s_count;
}

int carhull_find(const char* name, const float** verts) {
	*verts = NULL;
	if (!name || !name[0]) return 0;
	const u32 want = stringHash("_CV", stringHash(name, 0xFFFFFFFFu));
	for (int i = 0; i < s_count; i++) {
		if (s_hull[i].hash != want) continue;
		*verts = s_hull[i].v;
		return s_hull[i].n;
	}
	return 0;
}
