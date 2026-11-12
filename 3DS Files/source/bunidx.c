#include "bunidx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See bunidx.h.
#define BUNIDX_FILES  8
#define BUNIDX_WINDOW (16 * 1024)

typedef struct {
	u32 type;
	u32 off;    // of the header
	u32 size;   // of the payload
} BunChunk;

typedef struct {
	char*     path;
	BunChunk* chunks;
	int       count;
	bool      ok;     // walked; false until then, or while the file will not open
} BunIndex;

static BunIndex s_idx[BUNIDX_FILES];
static int s_idxCount;

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool walk(const char* path, BunIndex* out) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	long fileSize = -1;
	if (fseek(f, 0, SEEK_END) == 0)
		fileSize = ftell(f);
	u8* win = (u8*)malloc(BUNIDX_WINDOW);
	int cap = 64;
	BunChunk* list = (BunChunk*)malloc((size_t)cap * sizeof(BunChunk));
	if (fileSize <= 0 || !win || !list) {
		free(win);
		free(list);
		fclose(f);
		return false;
	}

	int n = 0;
	long winOff = 0, winLen = 0;   // what `win` holds
	long off = 0;
	while (off + 8 <= fileSize) {
		if (off < winOff || off + 8 > winOff + winLen) {
			const long want = fileSize - off < BUNIDX_WINDOW ? fileSize - off : BUNIDX_WINDOW;
			if (fseek(f, off, SEEK_SET) != 0 || (long)fread(win, 1, (size_t)want, f) != want)
				break;
			winOff = off;
			winLen = want;
		}
		const u8* h = win + (off - winOff);
		const u32 type = rd32le(h), size = rd32le(h + 4);
		if (type == 0 && size == 0)
			break;
		if ((long)size > fileSize - (off + 8))
			break;
		if (n == cap) {
			BunChunk* grown = (BunChunk*)realloc(list, (size_t)cap * 2 * sizeof(BunChunk));
			if (!grown)
				break;
			list = grown;
			cap *= 2;
		}
		list[n].type = type;
		list[n].off = (u32)off;
		list[n].size = size;
		n++;
		off += 8 + (long)size;
	}
	free(win);
	fclose(f);
	out->chunks = list;
	out->count = n;
	return true;
}

static BunIndex* indexFor(const char* path) {
	for (int i = 0; i < s_idxCount; i++)
		if (strcmp(s_idx[i].path, path) == 0)
			return &s_idx[i];
	BunIndex* x;
	if (s_idxCount < BUNIDX_FILES) {
		x = &s_idx[s_idxCount++];
	} else {
		// Full: the last slot is recycled. Only a handful of files are ever
		// asked about, so this is a guard rather than a policy.
		x = &s_idx[BUNIDX_FILES - 1];
		free(x->path);
		free(x->chunks);
	}
	memset(x, 0, sizeof(*x));
	x->path = strdup(path);
	if (!x->path) {
		s_idxCount--;
		return NULL;
	}
	return x;
}

bool bunidx_find(const char* path, u32 type, long* hdrOff, u32* size) {
	if (!path)
		return false;
	BunIndex* x = indexFor(path);
	if (!x)
		return false;
	// Walked on first use. A file that would not open is asked about again
	// next time rather than remembered as missing.
	if (!x->ok && !(x->ok = walk(path, x)))
		return false;
	for (int i = 0; i < x->count; i++)
		if (x->chunks[i].type == type) {
			*hdrOff = (long)x->chunks[i].off;
			*size = x->chunks[i].size;
			return true;
		}
	return false;
}
