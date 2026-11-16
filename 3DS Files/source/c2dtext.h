#pragma once

#include <citro2d.h>
#include <math.h>

#include "fontmip.h"

// PORT (the user, 2026-11-14): citro2d 1.7.0 ("Fix text width calculate",
// text.c, 2024-12) applies the font's textScale to a C2D_Text's width at
// parse time for every font, not the system font alone, so the width
// C2D_TextGetDimensions reports is already the drawn one -- and multiplying it
// again below made every measured string about 0.6 of its real width: the
// bank's label and value ran into each other, a translated SMS wrapped past
// the screen's edge. Which citro2d the build links is read once, off one
// glyph: at scale 1 a parsed "M" is either its advance (1.6) or its advance
// times textScale (1.7).
static inline bool c2dWidthHasFontScale(C2D_Font font, float ts) {
	static int known = -1;
	if (known < 0 && font && ts > 0.0f && fabsf(ts - 1.0f) > 0.01f) {
		C2D_TextBuf b = C2D_TextBufNew(4);
		if (b) {
			C2D_Text t;
			C2D_TextFontParse(&t, font, b, "M");
			fontGlyphPos_s gp;
			C2D_FontCalcGlyphPos(font, &gp, C2D_FontGlyphIndexFromCodePoint(font, 'M'),
				0, 1.0f, 1.0f);
			const float a = gp.xAdvance;
			known = fabsf(t.width - a * ts) < fabsf(t.width - a) ? 1 : 0;
			C2D_TextBufDelete(b);
		}
	}
	return known == 1;
}

// C2D_TextGetDimensions as C2D_DrawText draws. citro2d 1.6.0 normalises every
// font to a 30-pixel cell: C2D_DrawText multiplies both scales by the font's
// textScale (30 / its cell height) before placing a glyph, but
// C2D_TextGetDimensions only applies it to the height -- a C2D_Text's width is
// rescaled at parse time for the system font alone (text.c, "text->width *=
// s_textScale"). conduit.bcfnt's cells are 44 pixels tall, so the raw call
// reports every Conduit string 1.47x wider than it is drawn. Text parsed with
// the system font (font NULL) comes back as it was.
static inline void textDims(const C2D_Text* t, float scaleX, float scaleY,
		float* outW, float* outH) {
	float w = 0.0f, h = 0.0f;
	C2D_TextGetDimensions(t, scaleX, scaleY, &w, &h);
	// The font's own textScale rather than 30 / its cell height: the UI font's
	// is set below that (main.c, the cap-height match, TASKS.md #425).
	if (t->font) {
		float ts = fontmip_text_scale(t->font);
		const FINF_s* fi = C2D_FontGetInfo(t->font);
		if (ts <= 0.0f && fi && fi->tglp && fi->tglp->cellHeight)
			ts = 30.0f / (float)fi->tglp->cellHeight;
		// Not twice: citro2d 1.7.0 has already applied it (above).
		if (ts > 0.0f && !c2dWidthHasFontScale(t->font, ts))
			w *= ts;
	}
	if (outW)
		*outW = w;
	if (outH)
		*outH = h;
}
