#pragma once
#include <3ds.h>

// Runtime parser for NFSU2's LANGUAGES/*.bin string tables (bChunk leaf type
// 0x00039000). Format confirmed via tools/language_bin.py (see that file's
// docstring for the full struct writeup and cross-referencing notes) -- this
// is a straight C port of the same layout, reading the real shipped
// PC-format English.bin from GameDir at runtime rather than baking any text
// into the app.
typedef struct {
	void* data; // malloc'd payload of the 0x39000 leaf
	u32 numStrings;
	u32 tableOffset;
	u32 stringsOffset;
	// UTF-8 rebuild of the string blob, or NULL when the file's own byte ->
	// codepoint table turned out to be pure ASCII. See langbin.c (TASKS.md
	// #128); when it is set, the entry offsets have been rewritten to index
	// it and the original blob has been dropped.
	char* utf8;
} LangTable;

bool langbin_load(const char* path, LangTable* out);
const char* langbin_lookup(const LangTable* t, u32 hash);
void langbin_free(LangTable* t);
