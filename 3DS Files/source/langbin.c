#include "langbin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	u32 hash;
	s32 stringOffset;
} LangTableEntry;

// The strings blob is not ASCII and it is not UTF-8 either: it is stored one
// byte per character in the file's own private codepage, and the file carries
// that codepage with it -- a 256-entry byte -> Unicode table sitting at
// header[0] (`wcharTableOffset`, always 0x10), which is why the loader below
// has always read that field even though nothing used it. In the shipped
// English.bin only 16 entries are non-zero, and they are not Windows-1252:
// 0x81 -> U+2019, 0x82 -> U+2122, 0x83 -> U+2013, 0x84 -> U+201D,
// 0x86 -> U+2026, 0x87 -> U+201C, 0x88 -> U+201E, four Hangul syllables at
// 0x89-0x8C, plus 0xA9/0xAE/0xD1 mapped to themselves.
//
// citro2d's C2D_TextFontParse decodes UTF-8, so a raw 0x82 is an invalid lead
// byte and comes out as '?' -- which is exactly what the options ring's
// "EA GAMES<0x82> TRAX" was rendering (TASKS.md #128). The glyph itself was
// never missing: conduit.bcfnt's scan-type CMAP does carry U+2122, because
// mkbcfnt takes everything the .otf has. So the fix is a decode, not a font
// rebuild: run the blob through the file's own table once at load and hand
// citro2d real UTF-8.
//
// A zero entry is treated as identity (byte value = codepoint, i.e. Latin-1).
// The table listing 0xA9/0xAE/0xD1 explicitly hints that zero may instead mean
// "this character has no glyph in the game's own font pages", but identity is
// the strictly better of the two here: a wrong-but-plausible glyph beats the
// '?' that a dropped character would fall back to anyway.
#define LANGBIN_WCHAR_COUNT 256

static u32 langbin_utf8_len(u32 cp) {
	if (cp < 0x80) return 1;
	if (cp < 0x800) return 2;
	return 3; // the table is u16, so U+FFFF is the ceiling
}

static char* langbin_utf8_put(char* p, u32 cp) {
	if (cp < 0x80) {
		*p++ = (char)cp;
	} else if (cp < 0x800) {
		*p++ = (char)(0xC0 | (cp >> 6));
		*p++ = (char)(0x80 | (cp & 0x3F));
	} else {
		*p++ = (char)(0xE0 | (cp >> 12));
		*p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*p++ = (char)(0x80 | (cp & 0x3F));
	}
	return p;
}

// Rebuilds the string blob as UTF-8 and repoints every table entry at it.
// Copies per entry rather than walking the blob linearly, which needs no
// old-offset -> new-offset map: the shipped table's 3925 entries all have
// distinct string offsets, and an entry is the only way anything reads a
// string. Returns false only on allocation failure, in which case the table
// is left exactly as it was loaded (raw bytes, '?' for the 62 affected
// strings) rather than half-converted.
static bool langbin_to_utf8(LangTable* t, const u16* remap) {
	LangTableEntry* entries = (LangTableEntry*)((u8*)t->data + t->tableOffset);
	const char* blob = (const char*)t->data + t->stringsOffset;

	u32 need = 0;
	bool anyHigh = false;
	for (u32 i = 0; i < t->numStrings; i++) {
		const u8* s = (const u8*)blob + entries[i].stringOffset;
		for (; *s; s++) {
			if (*s < 0x80) {
				need++;
			} else {
				anyHigh = true;
				const u16 m = remap[*s];
				need += langbin_utf8_len(m ? m : *s);
			}
		}
		need++; // NUL
	}
	if (!anyHigh)
		return true; // already valid UTF-8; leave the blob alone

	char* out = (char*)malloc(need);
	if (!out)
		return false;

	char* w = out;
	for (u32 i = 0; i < t->numStrings; i++) {
		const u8* s = (const u8*)blob + entries[i].stringOffset;
		entries[i].stringOffset = (s32)(w - out);
		for (; *s; s++) {
			const u16 m = *s < 0x80 ? 0 : remap[*s];
			w = langbin_utf8_put(w, m ? m : *s);
		}
		*w++ = '\0';
	}
	t->utf8 = out;

	// The original blob is dead now, and it is the bulk of the file (208 KB of
	// the 244 KB payload in English.bin). The entry table sits below it, so
	// the payload can simply be trimmed at stringsOffset. Nothing caches a
	// pointer into `data` across this -- langbin_lookup() recomputes `entries`
	// from t->data on every call -- so a realloc that moves is fine, and one
	// that fails just leaves the larger buffer in place.
	void* shrunk = realloc(t->data, t->stringsOffset);
	if (shrunk)
		t->data = shrunk;
	return true;
}

bool langbin_load(const char* path, LangTable* out) {
	out->data = NULL;
	out->utf8 = NULL;

	FILE* f = fopen(path, "rb");
	if (!f) return false;

	u32 chunkHeader[2]; // {type, size}, per the shared bChunk container format
	if (fread(chunkHeader, 4, 2, f) != 2) { fclose(f); return false; }
	if (chunkHeader[0] != 0x00039000) { fclose(f); return false; } // not a language string table leaf

	u32 size = chunkHeader[1];
	void* buf = malloc(size);
	if (!buf) { fclose(f); return false; }
	if (fread(buf, 1, size, f) != size) { free(buf); fclose(f); return false; }
	fclose(f);

	// Header: {wcharTableOffset, numStrings, tableOffset, stringsOffset}, all u32 LE.
	u32* header = (u32*)buf;
	out->data = buf;
	out->numStrings = header[1];
	out->tableOffset = header[2];
	out->stringsOffset = header[3];

	// The wchar table is {s32 count; u16 entries[count];}. Only the 256-entry
	// form has ever been seen; anything else is left undecoded rather than
	// guessed at, which just restores the old raw-byte behaviour.
	const u32 wcharOffset = header[0];
	if (wcharOffset + 4 <= size) {
		const s32 count = *(const s32*)((u8*)buf + wcharOffset);
		if (count == LANGBIN_WCHAR_COUNT &&
				wcharOffset + 4 + LANGBIN_WCHAR_COUNT * 2 <= size) {
			const u16* remap = (const u16*)((u8*)buf + wcharOffset + 4);
			langbin_to_utf8(out, remap);
		}
	}
	return true;
}

const char* langbin_lookup(const LangTable* t, u32 hash) {
	if (!t->data) return NULL;

	LangTableEntry* entries = (LangTableEntry*)((u8*)t->data + t->tableOffset);
	// Entries are sorted low->high hash (confirmed via tools/language_bin.py
	// against the real English.bin), so a binary search is valid, matching
	// the game's own documented lookup strategy.
	s32 lo = 0, hi = (s32)t->numStrings - 1;
	while (lo <= hi) {
		s32 mid = (lo + hi) / 2;
		if (entries[mid].hash == hash) {
			// Offsets index the UTF-8 rebuild once langbin_to_utf8() has run.
			return t->utf8 ? t->utf8 + entries[mid].stringOffset
					: (const char*)t->data + t->stringsOffset + entries[mid].stringOffset;
		} else if (entries[mid].hash < hash) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return NULL;
}

void langbin_free(LangTable* t) {
	if (t->data) free(t->data);
	t->data = NULL;
	if (t->utf8) free(t->utf8);
	t->utf8 = NULL;
}
