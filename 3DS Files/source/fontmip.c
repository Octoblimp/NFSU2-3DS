#include "fontmip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// citro2d 1.6.0's private font record (source/internal.h). It is not in the
// public headers, so fontmip_apply() checks every field it relies on against
// the font's own FINF/TGLP before it writes anything.
struct FontMirror {
	CFNT_s* cfnt;
	C3D_Tex* glyphSheets;
	float textScale;
};

// A texel's offset inside a PICA200 texture: 8x8 tiles in rows, and inside a
// tile the texels in Morton order (x0 y0 x1 y1 x2 y2). Whether the first tile
// row is the image's top or bottom does not matter here -- every level is
// read and written the same way, and a 2x2 box lands on the same four texels
// either way up.
static u32 texelIndex(u32 x, u32 y, u32 w) {
	const u32 tile = (y >> 3) * (w >> 3) + (x >> 3);
	x &= 7;
	y &= 7;
	const u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
		((x & 4) << 2) | ((y & 4) << 3);
	return tile * 64 + m;
}

// 4-bit formats pack two texels a byte, the first in the low nibble.
static int bitsPerTexel(GPU_TEXCOLOR fmt) {
	switch (fmt) {
	case GPU_A4: case GPU_L4: return 4;
	case GPU_A8: case GPU_L8: return 8;
	default: return 0;
	}
}

static void decodeLevel(const u8* src, u8* out, u32 w, u32 h, int bpp) {
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++) {
			const u32 i = texelIndex(x, y, w);
			u8 v;
			if (bpp == 4) {
				const u8 b = src[i >> 1];
				v = (u8)(((i & 1) ? (b >> 4) : (b & 0xF)) * 17);
			} else {
				v = src[i];
			}
			out[y * w + x] = v;
		}
}

static void encodeLevel(const u8* in, u8* dst, u32 w, u32 h, int bpp) {
	if (bpp == 4)
		memset(dst, 0, (w * h) >> 1);
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++) {
			const u32 i = texelIndex(x, y, w);
			const u8 v = in[y * w + x];
			if (bpp == 4) {
				const u8 n = (u8)((v + 8) / 17);
				dst[i >> 1] |= (i & 1) ? (u8)(n << 4) : n;
			} else {
				dst[i] = v;
			}
		}
}

bool fontmip_apply(C2D_Font font, int maxLevel, char* log, size_t logSize) {
	if (log && logSize)
		log[0] = '\0';
	struct FontMirror* fm = (struct FontMirror*)font;
	FINF_s* fi = font ? C2D_FontGetInfo(font) : NULL;
	if (!fi || !fm->cfnt || fi != &fm->cfnt->finf || !fi->tglp || !fm->glyphSheets) {
		if (log)
			snprintf(log, logSize, "font mipmaps: font record not as expected, left alone");
		return false;
	}
	TGLP_s* g = fi->tglp;
	const GPU_TEXCOLOR fmt = (GPU_TEXCOLOR)g->sheetFmt;
	const int bpp = bitsPerTexel(fmt);
	const u32 w = g->sheetWidth, h = g->sheetHeight;
	const int cap = C3D_TexCalcMaxLevel(w, h);
	if (maxLevel > cap)
		maxLevel = cap;
	if (!bpp || maxLevel < 1 || w < 8 || h < 8) {
		if (log)
			snprintf(log, logSize, "font mipmaps: sheet format %u (%lux%lu) not handled, left alone",
				(unsigned)g->sheetFmt, (unsigned long)w, (unsigned long)h);
		return false;
	}
	for (int s = 0; s < g->nSheets; s++) {
		const C3D_Tex* t = &fm->glyphSheets[s];
		if (t->data != g->sheetData + g->sheetSize * s || t->width != w || t->height != h ||
				t->fmt != fmt) {
			if (log)
				snprintf(log, logSize, "font mipmaps: sheet %d does not match the TGLP, left alone", s);
			return false;
		}
	}

	u8* a = malloc(w * h);
	u8* b = malloc((w / 2) * (h / 2));
	C3D_Tex* made = calloc(g->nSheets, sizeof(C3D_Tex));
	bool ok = a && b && made;
	int built = 0;
	for (int s = 0; ok && s < g->nSheets; s++) {
		C3D_Tex* t = &made[s];
		ok = C3D_TexInitWithParams(t, NULL,
			(C3D_TexInitParams){ (u16)w, (u16)h, (u8)maxLevel, fmt, GPU_TEX_2D, false });
		if (!ok)
			break;
		built++;
		u32 sz = 0;
		u8* lv = C3D_Tex2DGetImagePtr(t, 0, &sz);
		memcpy(lv, g->sheetData + g->sheetSize * s, sz);
		decodeLevel(lv, a, w, h, bpp);
		u32 lw = w, lh = h;
		for (int l = 1; l <= maxLevel; l++) {
			const u32 nw = lw / 2, nh = lh / 2;
			for (u32 y = 0; y < nh; y++)
				for (u32 x = 0; x < nw; x++) {
					const u8* r0 = a + (2 * y) * lw + 2 * x;
					const u8* r1 = r0 + lw;
					b[y * nw + x] = (u8)((r0[0] + r0[1] + r1[0] + r1[1] + 2) >> 2);
				}
			encodeLevel(b, C3D_Tex2DGetImagePtr(t, l, NULL), nw, nh, bpp);
			memcpy(a, b, nw * nh);
			lw = nw;
			lh = nh;
		}
		C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetFilterMipmap(t, GPU_LINEAR);
		C3D_TexSetWrap(t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		C3D_TexFlush(t);
	}
	free(a);
	free(b);
	if (!ok) {
		for (int s = 0; s < built; s++)
			C3D_TexDelete(&made[s]);
		free(made);
		if (log)
			snprintf(log, logSize, "font mipmaps: out of memory at sheet %d of %d, left alone",
				built, (int)g->nSheets);
		return false;
	}
	// The originals point into the font's own block and are never freed on
	// their own, so swapping the records over is all citro2d needs. The new
	// textures live as long as the font does.
	for (int s = 0; s < g->nSheets; s++)
		fm->glyphSheets[s] = made[s];
	free(made);
	if (log)
		snprintf(log, logSize, "font mipmaps: %d sheet(s) of %lux%lu, levels 0..%d, %lu KB linear",
			(int)g->nSheets, (unsigned long)w, (unsigned long)h, maxLevel,
			(unsigned long)(g->nSheets * C3D_TexCalcTotalSize(g->sheetSize, maxLevel) >> 10));
	return true;
}

// The same check fontmip_apply() makes before it trusts the mirror.
static struct FontMirror* fontMirror(C2D_Font font) {
	struct FontMirror* fm = (struct FontMirror*)font;
	FINF_s* fi = font ? C2D_FontGetInfo(font) : NULL;
	if (!fi || !fm->cfnt || fi != &fm->cfnt->finf || !fi->tglp || !fi->tglp->cellHeight)
		return NULL;
	return fm;
}

float fontmip_text_scale(C2D_Font font) {
	struct FontMirror* fm = fontMirror(font);
	return fm ? fm->textScale : 0.0f;
}

bool fontmip_set_text_scale(C2D_Font font, float scale) {
	struct FontMirror* fm = fontMirror(font);
	if (!fm || !(scale > 0.0f))
		return false;
	fm->textScale = scale;
	return true;
}
