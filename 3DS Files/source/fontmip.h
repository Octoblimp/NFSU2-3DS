// Mipmapped glyph sheets for a citro2d font (a port accommodation, not engine
// behaviour).
//
// conduit.bcfnt is rasterised with 44-pixel cells, and nearly every menu string
// is drawn 13-20 pixels tall: the glyph sheet is minified 2-3x. citro2d binds
// its sheets with plain bilinear filtering and no mip chain, so each screen
// pixel reads 2x2 texels out of a 3x3 footprint -- whole strokes fall between
// samples, and small text comes out broken and smeared rather than small.
// fontmip_apply() rebuilds each sheet as a texture with a box-filtered mip
// chain and trilinear filtering, so the GPU reads glyphs from the level that
// matches the size they are drawn at. Nothing about where or how big text is
// drawn changes.
#pragma once

#include <citro2d.h>
#include <stdbool.h>
#include <stddef.h>

// Replaces `font`'s glyph sheets with mipmapped copies, down to `maxLevel`
// (1-7). Returns false, leaving the font exactly as it was, if the font's
// sheets are not laid out the way this expects or memory runs out. `log`
// receives one line either way.
bool fontmip_apply(C2D_Font font, int maxLevel, char* log, size_t logSize);

// The font's citro2d textScale: C2D_DrawText multiplies both of its scales by
// it (C2D_FontLoad sets 30 / the cell height). 0 for NULL or a record that
// does not check out.
float fontmip_text_scale(C2D_Font font);

// Sets it, for every later draw and C2D_TextGetDimensions of the font (the
// UI font's cap-height match, main.c). False, nothing written, if the record
// does not check out.
bool fontmip_set_text_scale(C2D_Font font, float scale);
