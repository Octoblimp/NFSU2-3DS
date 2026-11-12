#include "credits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The parsed roll: every line UTF-8 and NUL-terminated, back to back in one
// pool, with an offset per line. One allocation each, sized exactly, once the
// parse below has counted what it produced.
static char* g_pool;
static int* g_off;
static int g_count;

void credits_unload(void) {
	free(g_pool);
	free(g_off);
	g_pool = NULL;
	g_off = NULL;
	g_count = 0;
}

int credits_count(void) {
	return g_count;
}

const char* credits_line(int i) {
	if (i < 0 || i >= g_count)
		return NULL;
	return g_pool + g_off[i];
}

// One UTF-16 code unit out of the raw bytes. Read a byte at a time: the file
// is read straight into a malloc'd buffer and nothing guarantees the odd
// halves line up with the 3DS's alignment rules.
static unsigned unit(const unsigned char* p, int i) {
	return (unsigned)p[i * 2] | ((unsigned)p[i * 2 + 1] << 8);
}

// One code point as UTF-8. Returns the bytes written, and writes nothing when
// `out` is NULL, which is how the sizing pass counts the pool.
static int utf8Put(char* out, unsigned cp) {
	if (cp < 0x80) {
		if (out) out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		if (out) {
			out[0] = (char)(0xC0 | (cp >> 6));
			out[1] = (char)(0x80 | (cp & 0x3F));
		}
		return 2;
	}
	if (cp < 0x10000) {
		if (out) {
			out[0] = (char)(0xE0 | (cp >> 12));
			out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[2] = (char)(0x80 | (cp & 0x3F));
		}
		return 3;
	}
	if (out) {
		out[0] = (char)(0xF0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (char)(0x80 | (cp & 0x3F));
	}
	return 4;
}

// The code point starting at unit `i`, and how many units it took. A lone
// surrogate is passed through as itself rather than dropped -- the roll is
// text to look at, not text to round-trip.
static unsigned codePoint(const unsigned char* p, int n, int i, int* used) {
	const unsigned u = unit(p, i);
	if (u >= 0xD800 && u < 0xDC00 && i + 1 < n) {
		const unsigned lo = unit(p, i + 1);
		if (lo >= 0xDC00 && lo < 0xE000) {
			*used = 2;
			return 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
		}
	}
	*used = 1;
	return u;
}

// Where 0x5113F0 would cut [start, end) -- the unit index one past the last
// line that fits, or `end` when the whole run fits. Retail counts characters,
// takes the first space of a space-run as the candidate break (and only from
// the second character on, so a leading space is never one), and commits to
// the candidate the moment the count reaches the limit; the run of spaces
// itself becomes terminators, so it belongs to neither line.
//
// `skipTo` comes back as the first unit of the next line, past that run.
static int wrapCut(const unsigned char* p, int start, int end, int* skipTo) {
	int count = 0;
	int cand = -1;
	for (int i = start; i < end; i++) {
		const unsigned u = unit(p, i);
		// A surrogate pair is two units and one character; counting the low
		// half as nothing keeps the limit a character limit, as retail's is.
		if (!(u >= 0xDC00 && u < 0xE000))
			count++;
		if (count > 1 && u == ' ' && unit(p, i - 1) != ' ')
			cand = i;
		if (count >= CREDITS_WRAP_CHARS && cand >= 0) {
			int j = cand;
			while (j < end && unit(p, j) == ' ')
				j++;
			*skipTo = j;
			return cand;
		}
	}
	*skipTo = end;
	return end;
}

// Walks the file once, calling back with each finished line's unit range.
// Both passes below are the same walk, so neither can disagree with the other
// about how many lines there are or where they break.
typedef void (*LineFn)(const unsigned char* p, int from, int to, void* ctx);

static void walkLines(const unsigned char* p, int n, LineFn fn, void* ctx) {
	int i = 0;
	while (i < n) {
		// Retail turns CR and LF into terminators and then skips runs of them,
		// so an empty line never becomes a line.
		const unsigned u = unit(p, i);
		if (u == '\n' || u == '\r' || u == 0) {
			i++;
			continue;
		}
		int end = i;
		while (end < n) {
			const unsigned e = unit(p, end);
			if (e == '\n' || e == '\r' || e == 0)
				break;
			end++;
		}
		// The wrap pass splits this line in place, so it can yield more than
		// one; a cut lands on a space run that belongs to neither side.
		int from = i;
		while (from < end) {
			int skip = end;
			const int cut = wrapCut(p, from, end, &skip);
			if (cut > from)
				fn(p, from, cut, ctx);
			from = skip > from ? skip : end;
		}
		i = end;
	}
}

typedef struct {
	int lines;
	int bytes;
	char* pool;
	int* off;
} Build;

static void measure(const unsigned char* p, int from, int to, void* ctx) {
	Build* b = (Build*)ctx;
	b->lines++;
	for (int i = from; i < to;) {
		int used = 1;
		b->bytes += utf8Put(NULL, codePoint(p, to, i, &used));
		i += used;
	}
	b->bytes++; // terminator
}

static void emit(const unsigned char* p, int from, int to, void* ctx) {
	Build* b = (Build*)ctx;
	b->off[b->lines++] = b->bytes;
	for (int i = from; i < to;) {
		int used = 1;
		b->bytes += utf8Put(b->pool + b->bytes, codePoint(p, to, i, &used));
		i += used;
	}
	b->pool[b->bytes++] = '\0';
}

bool credits_load(const char* path) {
	credits_unload();

	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	// Two bytes is the BOM alone. Anything shorter, or not a whole number of
	// UTF-16 units, is not the file the reader at 0x525A30 expects.
	if (size < 4 || (size & 1) != 0 || size > (16 << 20)) {
		fclose(f);
		return false;
	}
	unsigned char* raw = (unsigned char*)malloc((size_t)size);
	if (!raw) {
		fclose(f);
		return false;
	}
	const size_t got = fread(raw, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		free(raw);
		return false;
	}
	// 0x525A30 steps over the BOM and reads UTF-16LE from there. A file
	// without one is not retail's, so it is refused rather than guessed at.
	if (raw[0] != 0xFF || raw[1] != 0xFE) {
		free(raw);
		return false;
	}
	const unsigned char* p = raw + 2;
	const int n = (int)((size - 2) / 2);

	Build b;
	memset(&b, 0, sizeof(b));
	walkLines(p, n, measure, &b);
	if (b.lines <= 0) {
		free(raw);
		return false;
	}
	char* pool = (char*)malloc((size_t)b.bytes);
	int* off = (int*)malloc(sizeof(int) * (size_t)b.lines);
	if (!pool || !off) {
		free(pool);
		free(off);
		free(raw);
		return false;
	}
	Build w;
	memset(&w, 0, sizeof(w));
	w.pool = pool;
	w.off = off;
	walkLines(p, n, emit, &w);
	free(raw);

	g_pool = pool;
	g_off = off;
	g_count = w.lines;
	return true;
}
