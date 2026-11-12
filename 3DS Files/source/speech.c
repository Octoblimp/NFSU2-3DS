// Character speech -- see speech.h for the bank decode and the retail chain.

#include "speech.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "audio.h"
#include "dbglog.h"
#include "rideinfo.h"

static void dlog(const char* msg) { dbglog_line(NULL, msg); }
static void nisProbe(void);

// ---- the clump index: UG2_Speech.idx, copied to the card by
// tools/speech_build.py, which carries the decode. A group is a bank (or an
// Outrun voice's sub-bank); a record is one sound and its Csis parameters as
// bit numbers. The card's files are g<group>_<record>.dspm.
#define SPEECH_GROUPS_MAX  16
#define SPEECH_RECORDS_MAX 1024
#define SPEECH_PARMS_MAX   4
#define SPEECH_PICKS_MAX   200 // iSPCH_AddSampleToValidPicks' cap

typedef struct {
	uint16_t first; // its first record in s_rec
	uint8_t  count;
	uint8_t  nparm;
} SpeechGroup;

static SpeechGroup s_group[SPEECH_GROUPS_MAX];
static int         s_groups;
static uint8_t     s_rec[SPEECH_RECORDS_MAX][SPEECH_PARMS_MAX];
static uint8_t     s_recGroup[SPEECH_RECORDS_MAX];
static int         s_records;

static char s_dir[512];
static bool s_have;         // the idx parsed and the set's first file is there
static int  s_current = -1; // the record speaking, or -1

// The calls made while a line was playing, oldest first (speech.h).
#define SPEECH_QUEUE 4
static int16_t s_queue[SPEECH_QUEUE];
static int     s_queued;

// The NIS module's (the end of the file): each stream the world asks for,
// its alternates on the card (stem0.dspm, stem1.dspm, ...).
#define NIS_ALT_MAX 9

typedef struct {
	const char* stem;   // tools/nisaudio_build.py's file stem
	const char* name;   // for the log
	int  max;           // the alternates the bank has
	int  have;          // on the card, probed at boot (-1 not yet)
	bool on[NIS_ALT_MAX];
} NisStream;

enum { NIS_COLLISION, NIS_THUNDER, NIS_BLOWN, NIS_STREAMS };
static NisStream s_nis[NIS_STREAMS] = {
	{ "colstream",   "CollisionStream", 6, -1 },   // records 3..8, {2, 8}
	{ "thunder",     "THUNDERSTRM",     9, -1 },   // records 14..22, {4, 1}
	{ "blownengine", "BLOWNENGINE",     4, -1 },   // records 9..12, {8, 0x10} (#513)
};
static bool s_nisOn;          // a stream was started on AUDIO_MUSIC_B
static bool s_paused;
static bool  s_nisMap;        // the mix map gives the level (speechUpdate)
static float s_nisGain = 1.0f;

static int recGroup(int rec) { return s_recGroup[rec]; }

static void recName(int rec, char* out, size_t n)
{
	const int g = recGroup(rec);
	snprintf(out, n, "g%02d_%03d", g, rec - s_group[g].first);
}

static void recPath(int rec, char* out, size_t n)
{
	char name[16];
	recName(rec, name, sizeof(name));
	snprintf(out, n, "%s/%s.dspm", s_dir, name);
}

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// The idx's groups and records (speech_build.py's load_index, less the .big
// offsets the console has no use for).
static bool parseIndex(const uint8_t* d, size_t n)
{
	s_groups = s_records = 0;
	if (n < 0x58)
		return false;
	const uint32_t ng = rd32(d + 4);
	if (ng == 0 || ng > SPEECH_GROUPS_MAX || 0x58 + ng * 16 > n)
		return false;
	for (uint32_t i = 0; i < ng; i++) {
		const uint32_t off = rd32(d + 0x58 + i * 16 + 8);
		if (off + 12 > n)
			return false;
		const int nparm = d[off + 4] & 0x7F, count = d[off + 5];
		if (nparm < 1 || nparm > SPEECH_PARMS_MAX || s_records + count > SPEECH_RECORDS_MAX ||
				off + 12 + (size_t)count * (2 + nparm) > n)
			return false;
		s_group[i].first = (uint16_t)s_records;
		s_group[i].count = (uint8_t)count;
		s_group[i].nparm = (uint8_t)nparm;
		for (int r = 0; r < count; r++) {
			const uint8_t* p = d + off + 12 + r * (2 + nparm) + 2;
			memset(s_rec[s_records], 0xFF, SPEECH_PARMS_MAX);
			memcpy(s_rec[s_records], p, nparm);
			s_recGroup[s_records] = (uint8_t)i;
			s_records++;
		}
	}
	s_groups = (int)ng;
	return true;
}

bool speechInit(const char* dataDir)
{
	s_have = false;
	s_current = -1;
	s_queued = 0;
	for (int k = 0; k < NIS_STREAMS; k++) s_nis[k].have = -1;
	s_groups = s_records = 0;
	snprintf(s_dir, sizeof(s_dir), "%s/speech", dataDir ? dataDir : ".");
	// The NIS streams' opens, at boot and not at the first crash: each is a
	// lookup in a folder of ~880 files, and the first wild hit's frame paid
	// for all six collision ones (#342).
	nisProbe();

	char path[600], msg[700];
	snprintf(path, sizeof(path), "%s/UG2_Speech.idx", s_dir);
	FILE* f = fopen(path, "rb");
	uint8_t* buf = NULL;
	size_t len = 0;
	if (f) {
		fseek(f, 0, SEEK_END);
		const long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz > 0 && sz < 65536 && (buf = (uint8_t*)malloc((size_t)sz)) != NULL)
			len = fread(buf, 1, (size_t)sz, f);
		fclose(f);
	}
	const bool parsed = buf && parseIndex(buf, len);
	free(buf);
	if (!parsed) {
		snprintf(msg, sizeof(msg), "speech: no UG2_Speech.idx in %s -- silent "
			"(tools/speech_build.py)", s_dir);
		dlog(msg);
		return false;
	}
	// One open for the log, not 857: a card with the idx and none of the set
	// still plays nothing, safely, line by line.
	recPath(0, path, sizeof(path));
	f = fopen(path, "rb");
	if (f)
		fclose(f);
	s_have = f != NULL;
	snprintf(msg, sizeof(msg), "speech: UG2_Speech.idx -- %d groups, %d records in %s%s",
		s_groups, s_records, s_dir, s_have ? "" : " (g00_000 missing: silent)");
	dlog(msg);
	return s_have;
}

void speechExit(void)
{
	speechStop();
	speechNisStop();
	s_have = false;
	s_groups = s_records = 0;
}

bool speechAvailable(void)
{
	return s_have;
}

// ---- the pick. iSPCH_Rand (SpeedPS.elf 0x802F1620) draws over the matching
// samples and tries again (32 draws at most) while the draw is among the last
// min(n/2, 10) picks of the same bank, which a 32-entry ring keeps
// (iSPCH_FindRandInQueue / AddRandToQueue, 0x802F1488 / 0x802F1530). PORT:
// EACrandom is not ported (any LCG serves), nor are SPCH's cycle bits
// (iSPCH_SetCycleBits, a per-game window of samples) or its skip of a sample
// already queued.
#define SPEECH_RING 32
static uint32_t s_seed = 0x2545F491u;
static uint16_t s_ringVal[SPEECH_RING], s_ringBank[SPEECH_RING];
static int      s_ringIndex;
static bool     s_ringInit;

static uint32_t eacRandom(void)
{
	s_seed = s_seed * 1103515245u + 12345u;
	return s_seed;
}

static uint32_t drawOf(int n) { return (eacRandom() >> 16) * (uint32_t)n >> 16; }

static int findRandInQueue(uint32_t val, int n, uint16_t bank)
{
	if (n > SPEECH_RING)
		n = SPEECH_RING;
	int idx = s_ringIndex, seen = 0;
	for (int k = 0; k <= SPEECH_RING; k++) {
		if (seen >= n)
			break;
		if (s_ringBank[idx] == bank) {
			if (s_ringVal[idx] == val)
				return seen;
			seen++;
		}
		idx = idx - 1 < 0 ? idx + SPEECH_RING - 1 : idx - 1;
	}
	return -1;
}

static int speechRand(int n, uint16_t bank)
{
	if (!s_ringInit) {
		memset(s_ringBank, 0xFF, sizeof(s_ringBank));
		s_ringInit = true;
	}
	int lookback = n / 2;
	if (lookback > 10)
		lookback = 10;
	uint32_t draw = drawOf(n), pick = draw;
	int best = -1;
	for (int tries = 1;; tries++) {
		const int at = findRandInQueue(draw, lookback, bank);
		if (at == -1) {
			pick = draw;
			break;
		}
		if (best < at) {
			best = at;
			pick = draw;
		}
		if (tries > 31)
			break;
		draw = drawOf(n);
	}
	s_ringIndex = (s_ringIndex + 1) % SPEECH_RING;
	s_ringVal[s_ringIndex] = (uint16_t)pick;
	s_ringBank[s_ringIndex] = bank;
	// Retail returns the last draw, not the one it queued; the two differ
	// only when all 32 draws were recent.
	return (int)draw;
}

// ---- playing

// The line asked for last, logged once the streaming thread has opened it:
// audioMusicPlay only asks (#342), so its length and its steps come later.
static bool   s_startLog;
static char   s_startWhat[64], s_startName[16];
static double s_startAskMs;

static void startLogFlush(void)
{
	if (!s_startLog || audioMusicStarting(AUDIO_MUSIC_SPEECH))
		return;
	s_startLog = false;
	char msg[400];
	if (audioMusicFailed(AUDIO_MUSIC_SPEECH))
		snprintf(msg, sizeof(msg), "speech: %s -- %s missing or unreadable (%s)",
			s_startWhat, s_startName, audioMusicStartBreakdown(AUDIO_MUSIC_SPEECH));
	else
		snprintf(msg, sizeof(msg), "speech: %s -- %s (%.1fs, asked in %.2f ms; streaming "
			"thread: %s), music ducked", s_startWhat, s_startName,
			audioMusicDuration(AUDIO_MUSIC_SPEECH), s_startAskMs,
			audioMusicStartBreakdown(AUDIO_MUSIC_SPEECH));
	dlog(msg);
}

static bool startRecord(int rec, const char* what)
{
	char path[600], name[16], msg[400];
	recPath(rec, path, sizeof(path));
	recName(rec, name, sizeof(name));
	// A line asked for and replaced before it got going still gets its line.
	if (s_startLog && audioMusicStarting(AUDIO_MUSIC_SPEECH)) {
		snprintf(msg, sizeof(msg), "speech: %s -- %s replaced before it started",
			s_startWhat, s_startName);
		dlog(msg);
		s_startLog = false;
	}
	startLogFlush();
	// Replace, not mix: a SpeechModule plays one stream at a time.
	//
	// Nothing is asked of the music here. The duck is the mixer's: audio.c
	// watches this slot and holds both music buses down for as long as the
	// line runs (audio.h, audioSetMusicDuckLevel). That is where retail keeps
	// it too -- in the sound script and not in the speech code.
	// The ask's cost on the frame that makes it: the open in a folder of 870
	// files and the first blocks are the streaming thread's (#342).
	const u64 t0 = svcGetSystemTick();
	if (!audioMusicPlay(AUDIO_MUSIC_SPEECH, path, AUDIO_BUS_SPEECH, false)) {
		s_current = -1;
		snprintf(msg, sizeof(msg), "speech: %s -- %s not asked for: the DSP is down", what, name);
		dlog(msg);
		return false;
	}
	s_startAskMs = (double)(svcGetSystemTick() - t0) * 1000.0 / (double)SYSCLOCK_ARM11;
	s_current = rec;
	if (s_paused)
		audioMusicPause(AUDIO_MUSIC_SPEECH, true);
	snprintf(s_startWhat, sizeof(s_startWhat), "%s", what);
	snprintf(s_startName, sizeof(s_startName), "%s", name);
	s_startLog = true;
	return true;
}

// True while a line is on the slot; a finished one hands over to the next
// queued call here, as SpeechStreamQueued's PlayStream does. A line still
// being opened is on the slot (audioMusicIsPlaying).
static bool slotBusy(void)
{
	startLogFlush();
	if (s_current < 0)
		return false;
	if (!audioMusicFinished(AUDIO_MUSIC_SPEECH) && audioMusicIsPlaying(AUDIO_MUSIC_SPEECH))
		return true;
	s_current = -1;
	while (s_queued > 0) {
		const int rec = s_queue[0];
		memmove(s_queue, s_queue + 1, (size_t)(--s_queued) * sizeof(s_queue[0]));
		if (startRecord(rec, "queued"))
			return true;
	}
	return false;
}

static int bitOf(uint32_t v)
{
	return v && !(v & (v - 1)) ? __builtin_ctz(v) : -1;
}

// The SPCH half of a parser call: the records whose parameters match, one of
// them picked, and played (or queued).
static bool csisPlay(const uint32_t* v, int n, bool purge, const char* what)
{
	if (!s_have || n < 1 || n > SPEECH_PARMS_MAX)
		return false;
	uint8_t want[SPEECH_PARMS_MAX];
	for (int i = 0; i < n; i++) {
		const int b = bitOf(v[i]);
		if (b < 0)
			return false;
		want[i] = (uint8_t)b;
	}
	int16_t picks[SPEECH_PICKS_MAX];
	int np = 0, bank = -1;
	for (int g = 0; g < s_groups && np < SPEECH_PICKS_MAX; g++) {
		if (s_group[g].nparm != n || s_group[g].count == 0 ||
				s_rec[s_group[g].first][0] != want[0])
			continue;
		for (int r = 0; r < s_group[g].count && np < SPEECH_PICKS_MAX; r++) {
			const int rec = s_group[g].first + r;
			if (memcmp(s_rec[rec], want, (size_t)n) == 0) {
				if (bank < 0)
					bank = g;
				picks[np++] = (int16_t)rec;
			}
		}
	}
	char msg[160];
	if (np == 0) {
		snprintf(msg, sizeof(msg), "speech: %s -- no record matches", what);
		dlog(msg);
		return false;
	}
	const int rec = picks[speechRand(np, (uint16_t)bank)];
	if (purge)
		speechPurge();
	if (slotBusy()) {
		if (s_queued == SPEECH_QUEUE) {
			snprintf(msg, sizeof(msg), "speech: %s -- the queue is full, dropped", what);
			dlog(msg);
			return false;
		}
		s_queue[s_queued++] = (int16_t)rec;
		char name[16];
		recName(rec, name, sizeof(name));
		snprintf(msg, sizeof(msg), "speech: %s -- %s queued behind the line playing", what, name);
		dlog(msg);
		return true;
	}
	return startRecord(rec, what);
}

#define CSIS(what, purge, ...) do { \
		const uint32_t v_[] = { __VA_ARGS__ }; \
		return csisPlay(v_, (int)(sizeof(v_) / sizeof(v_[0])), purge, what); \
	} while (0)

// The parser calls (0x8008CB30..0x8008CFFC). Their first parameter is the
// DX_Mode the SpeechCmd call operator passes: 1 for Career, Mag and DVD (which
// are Career calls with CR_Event 0x200 and 0x400), 8 for Outrun, 4 for Timed,
// 2 for the frontend (FE_Mode 1 for a screen, 0x20 for the dyno shop), 0x10
// for IntroEvent and 0x20 for IntroArea.
bool speechPlayCareerID(uint32_t crEvent, uint32_t charId, bool purge)
{
	char what[64];
	snprintf(what, sizeof(what), "Career(0x%lx, 0x%lx)", (unsigned long)crEvent,
		(unsigned long)charId);
	CSIS(what, purge, 1, crEvent, charId);
}

bool speechPlayCareerMag(uint32_t magName, uint32_t status, bool purge)
{
	char what[64];
	snprintf(what, sizeof(what), "Mag(0x%lx, %lu)", (unsigned long)magName, (unsigned long)status);
	CSIS(what, purge, 1, 0x200, magName, status);
}

bool speechPlayCareerDVD(uint32_t dvdName, uint32_t status, bool purge)
{
	char what[64];
	snprintf(what, sizeof(what), "DVD(0x%lx, %lu)", (unsigned long)dvdName, (unsigned long)status);
	CSIS(what, purge, 1, 0x400, dvdName, status);
}

bool speechPlayOutrun(uint32_t orParms, uint32_t charId, bool purge)
{
	char what[64];
	snprintf(what, sizeof(what), "Outrun(0x%lx, 0x%lx)", (unsigned long)orParms,
		(unsigned long)charId);
	CSIS(what, purge, 8, orParms, charId);
}

bool speechPlayTimed(uint32_t timeProgress, bool purge)
{
	char what[64];
	snprintf(what, sizeof(what), "Timed(%lu)", (unsigned long)timeProgress);
	CSIS(what, purge, 4, timeProgress);
}

bool speechPlayFEScreen(uint32_t feScreen, uint32_t charId, bool purge)
{
	char what[64];
	snprintf(what, sizeof(what), "FEScreen(0x%lx, 0x%lx)", (unsigned long)feScreen,
		(unsigned long)charId);
	CSIS(what, purge, 2, 1, feScreen, charId);
}

bool speechPlayFEDynoshop(uint32_t dynoshop, uint32_t charId, bool purge)
{
	char what[64];
	snprintf(what, sizeof(what), "FEDynoshop(0x%lx, 0x%lx)", (unsigned long)dynoshop,
		(unsigned long)charId);
	CSIS(what, purge, 2, 0x20, dynoshop, charId);
}

bool speechPlayIntroArea(uint32_t locations, uint32_t charId, bool purge)
{
	char what[64];
	snprintf(what, sizeof(what), "IntroArea(0x%lx, 0x%lx)", (unsigned long)locations,
		(unsigned long)charId);
	CSIS(what, purge, 0x20, locations, charId);
}

bool speechPlayIntroEvent(uint32_t locations, uint32_t charId, bool purge)
{
	char what[64];
	snprintf(what, sizeof(what), "IntroEvent(0x%lx, 0x%lx)", (unsigned long)locations,
		(unsigned long)charId);
	CSIS(what, purge, 0x10, locations, charId);
}

// The prologue's four: CareerSpeech::Stage0Intro .. Stage0Carlot
// (0x8018A8B4..0x8018A960), each SpeechCmdCareer(CR_Event, 0x20) through
// DoSpeechCall(cmd, 0). PORT: a replace (speech.h) -- stage0.c's script waits
// for each line to end before the next, so nothing is cut off in practice.
bool speechPlay(SpeechLine line)
{
	static const uint32_t kEvent[SPEECH_LINE_COUNT] = { 0x8000, 0x800, 0x2, 0x10000000 };
	if (line < 0 || line >= SPEECH_LINE_COUNT)
		return false;
	return speechPlayCareerID(kEvent[line], 0x20, true);
}

void speechPurge(void)
{
	speechStop();
}

void speechStop(void)
{
	s_queued = 0;
	// A line that started gets its line; one called off while still being
	// opened goes unlogged, as it never played.
	startLogFlush();
	s_startLog = false;
	if (s_current >= 0)
		audioMusicStop(AUDIO_MUSIC_SPEECH);
	s_current = -1;
}

bool speechIsPlaying(void)
{
	return slotBusy();
}

float speechPosition(void)
{
	return s_current < 0 ? 0.0f : audioMusicPosition(AUDIO_MUSIC_SPEECH);
}

float speechDuration(void)
{
	return s_current < 0 ? 0.0f : audioMusicDuration(AUDIO_MUSIC_SPEECH);
}

// ------------------------------------------------------------ the NIS module

static void nisProbe(void)
{
	const u64 t0 = svcGetSystemTick();
	char msg[640];
	for (int k = 0; k < NIS_STREAMS; k++) {
		NisStream* n = &s_nis[k];
		n->have = 0;
		for (int i = 0; i < n->max; i++) {
			char path[600];
			snprintf(path, sizeof(path), "%s/%s%d.dspm", s_dir, n->stem, i);
			FILE* f = fopen(path, "rb");
			n->on[i] = f != NULL;
			if (f) {
				fclose(f);
				n->have++;
			}
		}
		snprintf(msg, sizeof(msg), "speech: %s -- %d of %d alternates in %s%s", n->name,
			n->have, n->max, s_dir, n->have ? "" : " (silent: tools/nisaudio_build.py)");
		dlog(msg);
	}
	snprintf(msg, sizeof(msg), "speech: NIS streams probed in %.1f ms",
		(double)(svcGetSystemTick() - t0) * 1000.0 / (double)SYSCLOCK_ARM11);
	dlog(msg);
}

// PlayStream on the module's slot (AUDIO_MUSIC_B). The mix map's level
// carries the master and speech sliders (its own bus applies neither);
// without the map, the speech bus does.
static bool nisPlayPath(const char* path)
{
	if (!audioMusicPlay(AUDIO_MUSIC_B, path, s_nisMap ? AUDIO_BUS_MIXMAP : AUDIO_BUS_SPEECH, false))
		return false;
	audioMusicSetGain(AUDIO_MUSIC_B, s_nisGain);
	s_nisOn = true;
	if (s_paused)
		audioMusicPause(AUDIO_MUSIC_B, true);
	return true;
}

// QueStream's callback's PlayStream: one of stream `k`'s alternates, picked
// with `rng`, on the module's slot.
static bool nisPlay(int k, uint32_t* rng)
{
	NisStream* n = &s_nis[k];
	if (s_dir[0] == '\0')
		return false;
	if (n->have < 0)
		nisProbe();
	// QueStream's DonePlaying: the module's stream goes first.
	speechNisStop();
	if (n->have == 0)
		return false;
	// The pick, over the alternates on the card.
	int r = (int)rideinfo_random(rng, (u32)n->have), pick = -1;
	for (int i = 0; i < n->max; i++)
		if (n->on[i] && r-- == 0) {
			pick = i;
			break;
		}
	if (pick < 0)
		return false;
	char path[600];
	snprintf(path, sizeof(path), "%s/%s%d.dspm", s_dir, n->stem, pick);
	if (!nisPlayPath(path))
		return false;
	char msg[160];
	// Asked for, not yet opened (audioMusicPlay, #342), so no length yet.
	snprintf(msg, sizeof(msg), "speech: %s -- %s%d asked for", n->name, n->stem, pick);
	dlog(msg);
	return true;
}

bool speechNisScene(const char* stem)
{
	if (s_dir[0] == '\0')
		return false;
	// QueStream's DonePlaying: the module's stream goes first.
	speechNisStop();
	char path[600], msg[700];
	snprintf(path, sizeof(path), "%s/%s.dspm", s_dir, stem);
	FILE* f = fopen(path, "rb");
	if (!f) {
		snprintf(msg, sizeof(msg), "speech: NIS scene stream %s -- not on the card (silent: "
			"tools/nisaudio_build.py)", stem);
		dlog(msg);
		return false;
	}
	fclose(f);
	const bool ok = nisPlayPath(path);
	snprintf(msg, sizeof(msg), "speech: NIS scene stream %s -- %s", stem, ok ? "asked for" : "did not start");
	dlog(msg);
	return ok;
}

bool speechCollisionStream(uint32_t* rng)
{
	return nisPlay(NIS_COLLISION, rng);
}

bool speechThunderStream(int variant, uint32_t* rng)
{
	// SoundFX_Select {4, variant}: the bank has variant 1's alone (records
	// 14..22, parameters (2, 0)); variant 2's (2, 1) is not in the retail
	// data, so the heavy rain's ask finds nothing and is silent.
	if (variant != 1) {
		char msg[96];
		snprintf(msg, sizeof(msg), "speech: THUNDERSTRM variant %d -- no such sound in the bank", variant);
		dlog(msg);
		return false;
	}
	return nisPlay(NIS_THUNDER, rng);
}

bool speechBlownEngineStream(uint32_t* rng)
{
	return nisPlay(NIS_BLOWN, rng);
}

void speechNisStop(void)
{
	if (s_nisOn)
		audioMusicStop(AUDIO_MUSIC_B);
	s_nisOn = false;
}

bool speechNisPlaying(void)
{
	if (!s_nisOn)
		return false;
	if (audioMusicFinished(AUDIO_MUSIC_B) || !audioMusicIsPlaying(AUDIO_MUSIC_B)) {
		speechNisStop();
		return false;
	}
	return true;
}

void speechUpdate(bool paused, int32_t nisQ15)
{
	if (paused != s_paused) {
		s_paused = paused;
		if (s_current >= 0)
			audioMusicPause(AUDIO_MUSIC_SPEECH, paused);
		if (s_nisOn)
			audioMusicPause(AUDIO_MUSIC_B, paused);
	}
	s_nisMap = nisQ15 >= 0;
	s_nisGain = s_nisMap ? (float)(nisQ15 > 0x7fff ? 0x7fff : nisQ15) / 32767.0f : 1.0f;
	if (speechNisPlaying())
		audioMusicSetGain(AUDIO_MUSIC_B, s_nisGain);
	// A finished line hands over to the next queued call.
	if (!paused)
		slotBusy();
}
