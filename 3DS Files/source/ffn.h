#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// The game's own bitmap-font metrics: every `.ffn` a .fng String names is a
// RealFontOld font, shipped as a 0x30201 chunk -- GLOBAL/GLOBALA.BUN carries
// ConduitMdITC_TT21i, LANGUAGES/<language>.bin the language's set (TT14i,
// TT38bi, Arial, Impact, the LCD digits). Chunk layout, read off FEngFont's
// constructor (Speed.elf 0x8014bbfc) and RealFontOld (0x802c6d4c-0x802c6ea4):
//
//   +0x000  font name, NUL-padded to 0x100
//   +0x100  atlas texture name, NUL-padded to 0x100
//   +0x200  "FNTF" header (RealFontOld::Font):
//             +0x0a u16 glyph count   +0x0c u32 flags (0x40000: 16-byte glyphs,
//             16-bit kern keys; else 12-byte)   +0x14 u32 glyph table offset
//             +0x18 u32 kern table offset   +0x24 s32 line height
//   glyph:  +0x0 u16 code  +0x2 u8 w,h  +0x4 u16 atlas x,y  +0x9 s8 x,y offset
//           +0xb u8 kern count  +0xc u16 first kern  +0xe s16 advance
//   kern (at kern table +4): u16 previous character, s8 adjustment, pad
//
// The port draws text with citro2d and the Conduit .otf (conduit.bcfnt), not
// with these atlases; what it takes from here is everything about *layout*:
// how wide a line is, where it wraps, and how far apart lines sit, all in the
// font's own units -- the units a String's Sw width and the canvas share.

typedef struct FfnFont FfnFont;

// Adds every font chunk in a chunk file (a .BUN or a language .bin); a font
// already known by name is kept. Returns how many were added; a missing or
// unreadable file adds none.
int ffn_load_file(const char* path);

// The font a .fng resource names ("ConduitMdITC_TT21i.ffn"), matched on the
// name without its extension, case-insensitively, as FEngFont's hash is.
// NULL when no loaded chunk has that name.
const FfnFont* ffn_find(u32 nameHash);

// The key ffn_find() takes: FEHashUpper over the name minus any ".ffn".
u32 ffn_name_hash(const char* name);

// RealFontOld::Font +0x24.
int ffn_line_height(const FfnFont* f);

// One character's contribution to a line's width, FEngFont::GetLineWidth's
// way (0x8014ca0c): its advance, plus the kern against the character before
// it when there is one (prev < 0 at a line's start). Characters the font has
// no glyph for add nothing.
int ffn_char_width(const FfnFont* f, u32 code, int prev);

// GetLineWidth over a whole UTF-8 run (no line breaks inside).
int ffn_text_width(const FfnFont* f, const char* utf8, size_t len);

// Per-font calibration the caller owns: the citro2d scale at which
// conduit.bcfnt draws text as wide as this font's units say, so one font unit
// lands on one pixel. 0 until set.
float ffn_c2d_scale(const FfnFont* f);
void ffn_set_c2d_scale(const FfnFont* f, float s);

// Name of the font, for logs.
const char* ffn_name(const FfnFont* f);

// Every loaded font, in load order.
int ffn_count(void);
const FfnFont* ffn_at(int i);
