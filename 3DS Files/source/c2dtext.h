#pragma once

#include <citro2d.h>

#include "fontmip.h"

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
		const float ts = fontmip_text_scale(t->font);
		const FINF_s* fi = C2D_FontGetInfo(t->font);
		if (ts > 0.0f)
			w *= ts;
		else if (fi && fi->tglp && fi->tglp->cellHeight)
			w *= 30.0f / (float)fi->tglp->cellHeight;
	}
	if (outW)
		*outW = w;
	if (outH)
		*outH = h;
}
