#include "ffn.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FFN_CHUNK_ID 0x30201u
#define FFN_HDR_OFS 0x200u
#define FFN_MAX_FONTS 8

struct FfnFont {
	u32 hash;
	char name[32];
	u8* hdr;      // the FNTF header and the tables after it
	u32 size;
	u16 count;
	u32 stride;   // 16 bytes with flag 0x40000, else 12
	u32 glyphOfs;
	u32 kernOfs;
	int lineH;
	float c2dScale;
};

static FfnFont s_fonts[FFN_MAX_FONTS];
static int s_count;

static u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

u32 ffn_name_hash(const char* name) {
	size_t n = strlen(name);
	if (n >= 4 && strcasecmp(name + n - 4, ".ffn") == 0)
		n -= 4;
	// FEHashUpper (Speed.elf 0x80141ac8), which FEngFont keys its fonts by.
	u32 h = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++)
		h = (u32)toupper((unsigned char)name[i]) + h * 0x21u;
	return h;
}

static bool add_font(const u8* chunk, u32 size) {
	if (s_count >= FFN_MAX_FONTS || size < FFN_HDR_OFS + 0x30)
		return false;
	const u8* hdr = chunk + FFN_HDR_OFS;
	if (memcmp(hdr, "FNTF", 4) != 0)
		return false;
	char name[32];
	size_t n = strnlen((const char*)chunk, 0x100);
	if (n >= sizeof(name))
		n = sizeof(name) - 1;
	memcpy(name, chunk, n);
	name[n] = '\0';
	const u32 hash = ffn_name_hash(name);
	if (ffn_find(hash))
		return false;
	const u32 hsize = size - FFN_HDR_OFS;
	FfnFont* f = &s_fonts[s_count];
	memset(f, 0, sizeof(*f));
	f->count = rd16(hdr + 0x0a);
	f->stride = (rd32(hdr + 0x0c) & 0x40000u) ? 16u : 12u;
	f->glyphOfs = rd32(hdr + 0x14);
	f->kernOfs = rd32(hdr + 0x18);
	f->lineH = (int)rd32(hdr + 0x24);
	if (f->glyphOfs + (u32)f->count * f->stride > hsize)
		return false;
	f->hdr = malloc(hsize);
	if (!f->hdr)
		return false;
	memcpy(f->hdr, hdr, hsize);
	f->size = hsize;
	f->hash = hash;
	memcpy(f->name, name, n + 1);
	s_count++;
	return true;
}

int ffn_load_file(const char* path) {
	FILE* fp = fopen(path, "rb");
	if (!fp)
		return 0;
	int added = 0;
	long off = 0;
	u8 head[8];
	while (fseek(fp, off, SEEK_SET) == 0 && fread(head, 1, 8, fp) == 8) {
		const u32 id = rd32(head), size = rd32(head + 4);
		if (id == FFN_CHUNK_ID && size < 0x10000u) {
			u8* buf = malloc(size);
			if (buf && fread(buf, 1, size, fp) == size && add_font(buf, size))
				added++;
			free(buf);
		}
		// A container's chunks follow its header directly.
		off += (id & 0x80000000u) ? 8 : 8 + (long)size;
	}
	fclose(fp);
	return added;
}

const FfnFont* ffn_find(u32 nameHash) {
	for (int i = 0; i < s_count; i++)
		if (s_fonts[i].hash == nameHash)
			return &s_fonts[i];
	return NULL;
}

int ffn_line_height(const FfnFont* f) { return f ? f->lineH : 0; }
float ffn_c2d_scale(const FfnFont* f) { return f ? f->c2dScale : 0.0f; }
void ffn_set_c2d_scale(const FfnFont* f, float s) {
	if (f)
		((FfnFont*)f)->c2dScale = s;
}
const char* ffn_name(const FfnFont* f) { return f ? f->name : ""; }
int ffn_count(void) { return s_count; }
const FfnFont* ffn_at(int i) { return (i >= 0 && i < s_count) ? &s_fonts[i] : NULL; }

// RealFontOld::BSearch (0x802c6e64) over the code-sorted glyph table, after
// FEngFont's direct-index shortcut (glyph code - 0x20) misses.
static const u8* find_glyph(const FfnFont* f, u32 code) {
	const u8* t = f->hdr + f->glyphOfs;
	if (code >= 0x20 && code - 0x20 < f->count && rd16(t + (code - 0x20) * f->stride) == code)
		return t + (code - 0x20) * f->stride;
	u32 lo = 0, hi = f->count;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		const u32 c = rd16(t + mid * f->stride);
		if (c == code)
			return t + mid * f->stride;
		if (c < code)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

int ffn_char_width(const FfnFont* f, u32 code, int prev) {
	if (!f)
		return 0;
	// GetLineWidth's own remaps: Windows-1252 TM and oe to their Unicode
	// glyphs, and anything past 0xff7f to its low byte.
	if (code > 0xff7fu)
		code &= 0xffu;
	if (code == 0x99u)
		code = 0x2122u;
	else if (code == 0x9cu)
		code = 0x153u;
	const u8* g = find_glyph(f, code);
	if (!g)
		return 0;
	int w = (s16)rd16(g + 0x0e);
	// RealFontOld::Font::GetKern (0x802c6dd0): the glyph's own run of
	// (previous character, adjustment) pairs. 16-byte fonts key it by a u16,
	// the 12-byte ones by a byte pair; every shipped font is the former.
	const u32 nk = g[0x0b];
	if (prev >= 0 && nk && f->stride == 16) {
		const u32 first = rd16(g + 0x0c);
		for (u32 k = 0; k < nk; k++) {
			const u32 o = f->kernOfs + 4 + (first + k) * 4;
			if (o + 4 > f->size)
				break;
			if (rd16(f->hdr + o) == (u32)prev) {
				w += (s8)f->hdr[o + 2];
				break;
			}
		}
	}
	return w;
}

int ffn_text_width(const FfnFont* f, const char* utf8, size_t len) {
	int w = 0, prev = -1;
	size_t i = 0;
	while (i < len) {
		u32 code = 0;
		ssize_t n = decode_utf8(&code, (const uint8_t*)utf8 + i);
		if (n <= 0) {
			code = (u8)utf8[i];
			n = 1;
		}
		w += ffn_char_width(f, code, prev);
		prev = (int)code;
		i += (size_t)n;
	}
	return w;
}
