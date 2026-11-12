#pragma once
#include <stdbool.h>

// Credits.fng's rolling text -- TASKS.md #259.
//
// Where it comes from
// -------------------
// CreditsScreen's setup arm (SPEED2.EXE 0x4B87F8, message 0x35F8620B) builds
// the filename with snprintf("CREDITS\\%s%s.TXT", prefix, language): the
// language name comes from the current language id (0x4FF970 / 0x4FF640) and
// the prefix from two territory predicates -- 0x570A30 false gives "NA_",
// else 0x570A40 true gives "UK_", else the whole name is replaced by "GERMAN".
// So the shipped set is CREDITS/NA_ENGLISH.TXT, UK_ENGLISH.TXT, GERMAN.TXT and
// one per remaining language, and this port, which ships English, reads the
// first of those. They are retail's own files, copied onto the card beside
// LANGUAGES/English.bin with no conversion: the reader below is the shipped
// UTF-16 one.
//
// How it is read
// --------------
// The file object at CreditsScreen+0x5C (ctor 0x525A30) loads the whole file,
// steps over the UTF-16 BOM, and overwrites every CR and LF in the buffer with
// a NUL, so the buffer becomes its own line table. 0x5113F0 then walks it once
// with the limit 0x2D (45) and hard-wraps any line longer than that, breaking
// at the first space of the last space-run that still fits and turning the
// whole run into terminators. Reading a line is 0x511340 (first) / 0x511360
// (next), and both skip runs of NULs, so blank lines never reach the roll; a
// next past the end returns 0, which is what makes CreditsScreen rewind and
// start the roll again (0x4B88D3).
//
// This port does that parse once at load instead of in place, because the
// lines outlive the buffer here: the roll keeps a handful of them alive at a
// time and the screen can be left and re-entered.
#define CREDITS_WRAP_CHARS 45

// Reads and parses one credits file. False leaves the roll empty, which is
// what the screen is expected to cope with -- the file is optional on the
// card. A second call replaces whatever the first read.
bool credits_load(const char* path);
void credits_unload(void);

// The parsed roll, in file order. credits_line() returns UTF-8.
int credits_count(void);
const char* credits_line(int i);
