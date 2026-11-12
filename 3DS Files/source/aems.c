// Retail EA AEMS sound modules -- see aems.h for what this is (TASKS.md #298).
//
// Every node below is ported from the PC build's own node table (SPEED2.EXE
// 0x817FE0, 40 entries in sndaemsfuncs order), not from the GameCube's: the
// banks the port converts are the PC's, and a GC node reads the low half of a
// big-endian int where the PC reads the whole little-endian one. Rounding is
// the x87's (fistp, round to nearest even), hence lrintf throughout.
//
// The graphs run on the 20 ms AEMS timer (SNDAEMSI_timerupdate, Speed.elf
// 0x802DA3AC): every live module instance, newest first, once per tick.

#include "aems.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AEMS_HOST
#include <3ds.h>
#include "dbglog.h"
#endif

static void dlogf(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
#ifdef AEMS_HOST
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
#else
	char b[256];
	vsnprintf(b, sizeof(b), fmt, ap);
	dbglog_line("AEMS ", b);
#endif
	va_end(ap);
}

// ---------------------------------------------------------------- pointers
//
// The module image keeps its pointers as 32-bit words, and so do the Csis
// objects and instances the graphs read through them. On the 3DS a word is
// the address. A 64-bit host cannot fit an address in a word, so the host
// build puts everything a word can point at in one arena and stores offsets
// into it; offset 0 stays NULL because the base sits 16 bytes before it.
#ifdef AEMS_HOST
#define ARENA_BYTES (64u << 20)
static uint8_t* s_arena;
static uint32_t s_arenaUsed;

static inline uint8_t* AP(uint32_t o) { return o ? s_arena - 16 + o : NULL; }
static inline uint32_t AO(const void* p)
{
	return p ? (uint32_t)((const uint8_t*)p - (s_arena - 16)) : 0;
}

static void* aemAlloc(uint32_t n)
{
	if (!s_arena) s_arena = (uint8_t*)calloc(1, ARENA_BYTES);
	n = (n + 15) & ~15u;
	if (!s_arena || s_arenaUsed + n > ARENA_BYTES) return NULL;
	void* p = s_arena + s_arenaUsed;
	s_arenaUsed += n;
	return p;
}
static void  aemFree(void* p)               { (void)p; }
static void* aemAllocLinear(uint32_t n)     { return aemAlloc(n); }
static void  aemFreeLinear(void* p)         { (void)p; }
static void  aemFlushDsp(void* p, uint32_t n) { (void)p; (void)n; }
#else
static inline uint8_t* AP(uint32_t o)       { return (uint8_t*)(uintptr_t)o; }
static inline uint32_t AO(const void* p)    { return (uint32_t)(uintptr_t)p; }
static void* aemAlloc(uint32_t n)           { return calloc(1, n); }
static void  aemFree(void* p)               { free(p); }
// The bank file goes to linear memory in one piece: the DSP reads the samples
// straight out of it.
static void* aemAllocLinear(uint32_t n)     { return linearAlloc(n); }
static void  aemFreeLinear(void* p)         { linearFree(p); }
static void  aemFlushDsp(void* p, uint32_t n) { DSP_FlushDataCache(p, n); }
#endif

// Unaligned-safe reads and writes into the image and the instances.
static inline int32_t  R32(const uint8_t* p, int32_t o) { int32_t v; memcpy(&v, p + o, 4); return v; }
static inline void     W32(uint8_t* p, int32_t o, int32_t v) { memcpy(p + o, &v, 4); }
static inline float    RF(const uint8_t* p, int32_t o) { float v; memcpy(&v, p + o, 4); return v; }
static inline void     WF(uint8_t* p, int32_t o, float v) { memcpy(p + o, &v, 4); }
static inline uint8_t  U8(const uint8_t* p, int32_t o) { return p[o]; }
static inline int8_t   S8(const uint8_t* p, int32_t o) { return (int8_t)p[o]; }
static inline void     W8(uint8_t* p, int32_t o, uint8_t v) { p[o] = v; }
static inline uint16_t U16(const uint8_t* p, int32_t o) { uint16_t v; memcpy(&v, p + o, 2); return v; }
static inline int16_t  S16(const uint8_t* p, int32_t o) { int16_t v; memcpy(&v, p + o, 2); return v; }
static inline void     W16(uint8_t* p, int32_t o, uint16_t v) { memcpy(p + o, &v, 2); }
static inline uint8_t* PTR(const uint8_t* p, int32_t o) { return AP((uint32_t)R32(p, o)); }
static inline void     WPTR(uint8_t* p, int32_t o, const void* q) { W32(p, o, (int32_t)AO(q)); }

static inline int32_t rnd(float f) { return (int32_t)lrintf(f); }
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- the .aem

// tools/abk3ds.py writes it; keep the two in lockstep.
typedef struct {
	char     magic[4];        // "AEM3"
	uint32_t version;         // 1
	uint32_t imageOff, imageSize;
	uint32_t relocOff, relocCount;
	uint32_t csisOff, csisCount;
	uint32_t modOff, modCount;
	uint32_t sampleOff, sampleCount;
	char     name[16];
} AemFileHeader;

typedef struct { uint32_t imageOff, kind; char name[24]; } AemCsisRec;
typedef struct { uint32_t hdrOff, codeOff, words, pad; } AemModRec;
typedef struct {
	uint32_t dataOff, bytes, samples, rate, loop, pad;
	uint16_t coefs[16];
	uint16_t ps;
	int16_t  hist0, hist1;
	uint16_t pad2;
} AemSampleRec;

enum { OP_END, OP_CALL, OP_LD, OP_LDC, OP_ST, OP_STB, OP_ADD, OP_SUB, OP_IMUL,
       OP_MOVAC, OP_CMP, OP_JG, OP_JL, OP_ESI, OP_COUNT };
#define NODE_COUNT 40

typedef struct AemsBank AemsBank;

// One graph. The module header's update-function slot (+0x28) holds this
// record's address, and every instance's timer node copies it, so the tick
// finds the bytecode from the node alone, as retail found the x86 function.
typedef struct {
	AemsBank*       bank;
	uint8_t*        hdr;
	const uint32_t* code;
	uint32_t        words;
} AemsMod;

struct AemsBank {
	uint8_t*  file;         // the whole .aem, linear memory on the 3DS
	uint8_t*  image;
	int       bus;
	char      name[16];
	AemsClip* clips;
	uint32_t  nclips;
	uint32_t  nmods;
	AemsMod   mods[];       // the bank itself lives where a word can point at it
};

// ---------------------------------------------------------------- PC SND

// iSNDsin (SPEED2.EXE 0x7254CF): a quarter wave of 257 u16s (0x8181D8), one
// full period over 0..1023, amplitude +-65535.
static const uint16_t s_sinTab[257] = {
	    0,   402,   804,  1206,  1608,  2010,  2412,  2814,  3215,  3617,  4018,  4420,
	 4821,  5222,  5622,  6023,  6423,  6823,  7223,  7623,  8022,  8421,  8819,  9218,
	 9616, 10013, 10410, 10807, 11204, 11600, 11995, 12390, 12785, 13179, 13573, 13966,
	14359, 14751, 15142, 15533, 15923, 16313, 16702, 17091, 17479, 17866, 18253, 18638,
	19024, 19408, 19792, 20175, 20557, 20938, 21319, 21699, 22078, 22456, 22833, 23210,
	23586, 23960, 24334, 24707, 25079, 25450, 25820, 26189, 26557, 26925, 27291, 27656,
	28020, 28383, 28745, 29105, 29465, 29824, 30181, 30538, 30893, 31247, 31600, 31952,
	32302, 32651, 32999, 33346, 33692, 34036, 34379, 34721, 35061, 35400, 35738, 36074,
	36409, 36743, 37075, 37406, 37736, 38064, 38390, 38716, 39039, 39362, 39682, 40002,
	40319, 40636, 40950, 41263, 41575, 41885, 42194, 42501, 42806, 43110, 43412, 43712,
	44011, 44308, 44603, 44897, 45189, 45480, 45768, 46055, 46340, 46624, 46906, 47186,
	47464, 47740, 48015, 48288, 48558, 48828, 49095, 49360, 49624, 49886, 50146, 50403,
	50660, 50914, 51166, 51416, 51665, 51911, 52155, 52398, 52639, 52877, 53114, 53348,
	53581, 53811, 54040, 54266, 54491, 54713, 54933, 55152, 55368, 55582, 55794, 56004,
	56212, 56417, 56621, 56822, 57022, 57219, 57414, 57606, 57797, 57986, 58172, 58356,
	58538, 58718, 58895, 59070, 59243, 59414, 59583, 59749, 59913, 60075, 60235, 60392,
	60547, 60700, 60850, 60998, 61144, 61288, 61429, 61568, 61705, 61839, 61971, 62100,
	62228, 62353, 62475, 62596, 62714, 62829, 62942, 63053, 63162, 63268, 63371, 63473,
	63571, 63668, 63762, 63854, 63943, 64030, 64115, 64197, 64276, 64353, 64428, 64501,
	64571, 64638, 64703, 64766, 64826, 64884, 64939, 64992, 65043, 65091, 65136, 65179,
	65220, 65258, 65294, 65327, 65358, 65386, 65412, 65436, 65457, 65475, 65491, 65505,
	65516, 65524, 65531, 65534, 65535,
};

static int32_t isin(int32_t i)
{
	int q = (i >> 8) & 3;
	i &= 0xff;
	switch (q) {
	case 0:  return s_sinTab[i];
	case 1:  return s_sinTab[256 - i];
	case 2:  return -(int32_t)s_sinTab[i];
	default: return -(int32_t)s_sinTab[256 - i];
	}
}

// iSNDrandom (SPEED2.EXE 0x724903): six words of state (0x8B46F8..0x8B470C),
// all zero at boot, carried through a chain of adds. Exactly the PC's, so a
// graph's random picks come out in the PC's order.
static uint32_t s_rnd[6];

static uint32_t isrand(void)
{
	uint32_t* s = s_rnd;
	uint32_t a = s[4] + s[5];
	uint32_t c = (a < s[5] || a < s[4]) ? 1u : 0u;
	uint32_t d = c + s[3];
	s[4] = a;
	a += d;
	d = (a < s[3] ? 1u : 0u) + s[2];
	s[3] = a;
	a += d;
	d = (a < s[2] ? 1u : 0u) + s[1];
	s[2] = a;
	a += d;
	c = a < s[1] ? 1u : 0u;
	s[1] = a;
	a += c;
	s[0] += a;
	s[5]++;
	return s[0];
}

// The stereo pan law. PC SND turns a mono voice's azimuth into speaker gains
// through a 256-step table per speaker (SPEED2.EXE 0x7255C4, read by
// 0x725878 with the azimuth's high byte), each step a constant-power pair
// pan between the two speakers either side of it (0x72554C: t =
// (a - s1) / (s0 - s1) * pi/2, gains sin t and cos t, stored * 255 as bytes).
// Stereo puts its speakers at 90 and 270 degrees (0x725909). EA azimuth runs
// clockwise -- the same setup places a stereo sample's own channels at 0xC000
// (left) and 0x4000 (right) and the quad speakers at 0xE000 / 0x2000 for
// front left / right (0x72599E-0x725A5C) -- so the 90-degree speaker is the
// right one.
static float s_panL[256], s_panR[256];
static bool  s_panReady;

static void panInit(void)
{
	const float k = 0.017453294f;            // degrees to radians, as 0x7C6918
	for (int b = 0; b < 256; b++) {
		float deg = (float)(b << 8) * 0.0054931640625f;
		float s0 = 90.0f, s1, a;
		if (deg <= 90.0f)        { s1 = -90.0f; a = deg; }
		else if (deg >= 270.0f)  { s1 = -90.0f; a = deg - 360.0f; }
		else                     { s0 = 270.0f; s1 = 90.0f; a = deg; }
		float t = (a * k - s1 * k) / (s0 * k - s1 * k) * 1.5707964f;
		float g0 = (float)rnd(sinf(t) * 255.0f) / 255.0f;   // the speaker at s0
		float g1 = (float)rnd(cosf(t) * 255.0f) / 255.0f;   // the one at s1
		if (s0 == 90.0f) { s_panR[b] = g0; s_panL[b] = g1; }
		else             { s_panL[b] = g0; s_panR[b] = g1; }
	}
	s_panReady = true;
}

// ---------------------------------------------------------------- lists

// Every AEMS and Csis list is {next, prev} words with a head word somewhere;
// the head's prev is 0 (SPEED2.EXE 0x71FEA4 is the unlink).
static void listPush(uint8_t* head, int32_t headOff, uint8_t* node)
{
	uint8_t* h = PTR(head, headOff);
	WPTR(node, 0, h);
	W32(node, 4, 0);
	if (h) WPTR(h, 4, node);
	WPTR(head, headOff, node);
}

static void listUnlink(uint8_t* head, int32_t headOff, uint8_t* node)
{
	int32_t next = R32(node, 0), prev = R32(node, 4);
	if (PTR(head, headOff) == node) W32(head, headOff, next);
	if (prev) W32(AP((uint32_t)prev), 0, next);
	if (next) W32(AP((uint32_t)next), 4, prev);
}

// ---------------------------------------------------------------- Csis
//
// A class descriptor is {constructor clients, name}. A class instance --
// what the game creates and holds -- is {descriptor, reference count,
// member-data clients, destructor clients}. A client is {next, prev,
// callback, user, ...}; the callbacks are small ids here, since the image
// cannot hold a host function pointer.
#define CB_CTOR   1   // SNDAEMSI_CreateModuleInstance  (Speed.elf 0x802D9A3C)
#define CB_DTOR   2   // SNDAEMSI_SetClassDestructor
#define CB_CDATA  3   // SNDAEMSI_SetClassData

#define DESC_NAME 28
#define DESC_MAX  512

static uint8_t* s_desc[DESC_MAX];
static int      s_descCount;

// The timer list: every live module instance's node, newest first.
static uint8_t  s_timerHead[4];

// Instances destroyed during a tick, freed once it is over (the node that
// destroys its own instance is still running).
static uint8_t* s_grave;
static bool     s_inTick;
static int      s_live;

static void moduleCreate(uint8_t* cls, uint8_t* hdr);

static uint8_t* descFind(const char* name)
{
	for (int i = 0; i < s_descCount; i++)
		if (strncmp((const char*)s_desc[i] + 4, name, DESC_NAME - 1) == 0) return s_desc[i];
	if (s_descCount >= DESC_MAX) return NULL;
	uint8_t* d = (uint8_t*)aemAlloc(4 + DESC_NAME);
	if (!d) return NULL;
	strncpy((char*)d + 4, name, DESC_NAME - 1);
	s_desc[s_descCount++] = d;
	return d;
}

static void classUnref(uint8_t* cls)
{
	int32_t r = R32(cls, 4) - 1;
	W32(cls, 4, r);
	if (r == 0) aemFree(cls);
}

// Csis::Class::SendParameters: every member-data client copies as many
// fields as it has.
static void classSend(uint8_t* cls, const void* params)
{
	if (!params) return;
	for (uint8_t* c = PTR(cls, 8); c; c = PTR(c, 0))
		if (R32(c, 8) == CB_CDATA) {
			uint8_t* u = PTR(c, 0xc);
			memcpy(u + 0x14, params, (size_t)U8(u, 0x10) * 4);
		}
}

// The game's own constructor and member-data clients (aemsWatch).
#define WATCH_MAX 4
static struct {
	uint8_t*    desc;
	AemsWatchFn fn;
	void*       user;
} s_watch[WATCH_MAX];

static void classTell(uint8_t* cls, const void* params, bool made)
{
	if (!params) return;
	for (int i = 0; i < WATCH_MAX; i++)
		if (s_watch[i].fn && s_watch[i].desc == PTR(cls, 0))
			s_watch[i].fn(cls, (const int32_t*)params, made, s_watch[i].user);
}

// Csis::Class::CreateInstance: one reference for the caller, then every
// constructor client (one per module subscribed to the class) makes its
// instance, and they all get the member data.
static uint8_t* classCreate(uint8_t* desc, const void* params)
{
	if (!desc) return NULL;
	uint8_t* cls = (uint8_t*)aemAlloc(16);
	if (!cls) return NULL;
	WPTR(cls, 0, desc);
	W32(cls, 4, 1);
	for (uint8_t* c = PTR(desc, 0); c; c = PTR(c, 0))
		if (R32(c, 8) == CB_CTOR) moduleCreate(cls, PTR(c, 0xc));
	classSend(cls, params);
	classTell(cls, params, true);
	return cls;
}

// Csis::Class::Release: every destructor client is told (the graph's
// ClassDestructor node reads the flag), and the caller's reference goes.
static void classRelease(uint8_t* cls)
{
	for (uint8_t* c = PTR(cls, 0xc); c; c = PTR(c, 0))
		if (R32(c, 8) == CB_DTOR) W32(PTR(c, 0xc), 0x10, 1);
	classUnref(cls);
}

// ---------------------------------------------------------------- players
//
// The bank player node's state (SPEED2.EXE 0x720D57 and the play / stop /
// pause / unpause / update tables at 0x817F90..0x817FD0, of which only
// entry 0, bank samples, exists):
//   +0x00 bank image   +0x04 sample table   +0x08 voice handle
//   +0x0c s8 state     +0x0d s8 previous    +0x0e u8 params  +0x0f u8 outputs
//   +0x10 s8 playing type (-1 none)         +0x14 table index  +0x18 state in
//   +0x1c params[n] {u8 type; s32 last @4; s32 input @8}
//   then, with outputs, {s32 ms left (one-shots), s32 ms played}.
// A sample table is {s32 count; {u8 type; s32 bank index; s32}[count]}.
//
// Param types: 0 pitch (0x1000 = the sample's own rate), 2 volume 0..0x7fff,
// 3 azimuth (mono voices). Type 1 is time-stretch, which SND applies only to
// streamed voices, so bank samples ignore it on PC as well. Types 4..8 feed
// SND's spatial-spread, effect-send, low-/high-pass and dry paths; the 3DS
// voice has no such stages yet, so they are kept but not applied.

static AemsBank* playerBank(const uint8_t* s)
{
	const uint8_t* image = PTR(s, 0);
	return image ? (AemsBank*)PTR(image, 0x40) : NULL;
}

// 0x720C5A: no voice; zero the outputs.
static int32_t playerReset(uint8_t* s)
{
	W8(s, 0x10, 0xff);
	if (U8(s, 0xf)) {
		uint8_t* out = s + 0x1c + U8(s, 0xe) * 12;
		W32(out, 4, 0);
		W32(out, 0, 0);
	}
	return 0;
}

static void azimPan(int32_t azim, float* l, float* r)
{
	if (!s_panReady) panInit();
	int b = (azim >> 8) & 0xff;
	*l = s_panL[b];
	*r = s_panR[b];
}

void aemsAzimPan(int32_t azim, float* l, float* r) { azimPan(azim, l, r); }

static float q15Gain(int32_t v) { return (float)clampi(v, 0, 0x7fff) * 3.0518509e-05f; }
static float q12Pitch(int32_t v) { return (float)clampi(v, 0, 0xffff) / 4096.0f; }

// 0x720B5B: SNDplaysetdef, every param applied, then SNDplay from the bank.
// The play-time volume is SNDplayoptions' byte (input >> 8, of 127).
static int32_t playerPlay(uint8_t* s, const uint8_t* ent)
{
	int32_t pitch = 0x1000, vol = 0x7f, azim = 0;
	int n = U8(s, 0xe);
	for (int i = 0; i < n; i++) {
		uint8_t* p = s + 0x1c + i * 12;
		int32_t v = R32(p, 8);
		switch (U8(p, 0)) {
		case 0: pitch = v; break;
		case 2: vol = clampi(v, 0, 0x7fff) >> 8; break;
		case 3: azim = (uint16_t)v; break;
		default: break;
		}
		W32(p, 4, v);
	}
	AemsBank* b = playerBank(s);
	int32_t idx = R32(ent, 4);
	if (!b || idx < 0 || (uint32_t)idx >= b->nclips || !b->clips[idx].data) return -1;
	float l, r;
	azimPan(azim, &l, &r);
	return aemsVoicePlay(&b->clips[idx], b->bus, q12Pitch(pitch), (float)vol / 127.0f, l, r);
}

// 0x720C7C: a live voice follows its params; the outputs report where it is,
// in ms of the clip's own rate.
static int32_t playerUpdate(uint8_t* s)
{
	int32_t h = R32(s, 8);
	if (!aemsVoiceAlive(h)) return playerReset(s);
	int n = U8(s, 0xe);
	uint8_t* p = s + 0x1c;
	for (int i = 0; i < n; i++, p += 12) {
		int32_t v = R32(p, 8);
		if (v == R32(p, 4)) continue;
		switch (U8(p, 0)) {
		case 0: aemsVoiceSetPitch(h, q12Pitch(v)); break;
		case 2: aemsVoiceSetGain(h, q15Gain(v)); break;
		case 3: { float l, r; azimPan((uint16_t)v, &l, &r); aemsVoiceSetPan(h, l, r); break; }
		default: break;
		}
		W32(p, 4, v);
	}
	const AemsClip* c = aemsVoiceClip(h);
	if (U8(s, 0xf) && c && c->rate) {
		// p is now the outputs.
		float msPer = 1000.0f / (float)c->rate;
		uint32_t pos = aemsVoiceSamplePos(h);
		W32(p, 4, rnd((float)pos * msPer));
		W32(p, 0, c->loop ? 0 : rnd((float)(uint32_t)(c->samples - pos) * msPer));
	}
	return 1;
}

// 0x720BE7: pitch and volume back from the params (1.0 and full if absent).
static void playerUnpause(uint8_t* s)
{
	int32_t h = R32(s, 8), pitch = 0x1000, vol = 0x7fff;
	int n = U8(s, 0xe);
	for (int i = 0; i < n; i++) {
		uint8_t* p = s + 0x1c + i * 12;
		if (U8(p, 0) == 0)      { pitch = R32(p, 8); W32(p, 4, pitch); }
		else if (U8(p, 0) == 2) { vol = R32(p, 8);   W32(p, 4, vol); }
	}
	aemsVoiceSetPitch(h, q12Pitch(pitch));
	aemsVoiceSetGain(h, q15Gain(vol));
	aemsVoicePause(h, false);
}

// ---------------------------------------------------------------- modules
//
// Module header (offsets from the header, SNDAEMSI_resolvemodulebank):
//   +0x04 class handle {descriptor, key}    +0x0c constructor client
//   +0x1c s16 instances   +0x1e s16 max     +0x20 u16 globals  +0x22 u16 functions
//   +0x24 u8 players  +0x25 u8 destructor  +0x26 u8 class data  +0x27 u8 controls
//   +0x28 update (here: the AemsMod)        +0x2c instance template
//   +0x30 instance size   +0x34 destroy-state offset   +0x38 instances
//   +0x3c player state offsets, then control-class state offsets.
// No PC bank has a global or a function (#298 survey), so those are not kept.

// SNDAEMSI_CreateModuleInstance (Speed.elf 0x802D9A3C).
static void moduleCreate(uint8_t* cls, uint8_t* hdr)
{
	if (S16(hdr, 0x1c) >= S16(hdr, 0x1e)) return;
	uint32_t isz = (uint32_t)R32(hdr, 0x30);
	uint8_t* inst = (uint8_t*)aemAlloc(isz);
	if (!inst) return;
	memcpy(inst, PTR(hdr, 0x2c), isz);
	int32_t link = R32(hdr, 0x34);
	WPTR(inst, link + 0, hdr);
	WPTR(inst, link + 4, inst);
	WPTR(inst, link + 8, cls);
	listPush(hdr, 0x38, inst);
	// Timer node {next, prev, update, state}; the graph's state starts at +0x18.
	W32(inst, 0x10, R32(hdr, 0x28));
	WPTR(inst, 0x14, inst + 0x18);
	listPush(s_timerHead, 0, inst + 8);
	uint8_t* c = inst + 0x18;
	if (U8(hdr, 0x25)) {
		W32(c, 8, CB_DTOR);
		WPTR(c, 0xc, c);
		listPush(cls, 0xc, c);
		W32(cls, 4, R32(cls, 4) + 1);
		c += 0x14;
	}
	if (U8(hdr, 0x26)) {
		W32(c, 8, CB_CDATA);
		WPTR(c, 0xc, c);
		listPush(cls, 8, c);
		W32(cls, 4, R32(cls, 4) + 1);
	}
	W16(hdr, 0x1c, (uint16_t)(S16(hdr, 0x1c) + 1));
	s_live++;
}

static void instFree(uint8_t* inst)
{
	if (s_inTick) { WPTR(inst, 0, s_grave); s_grave = inst; }
	else aemFree(inst);
}

// The destroy node's work (SPEED2.EXE 0x71FF15), on its state {header,
// instance, class, trigger}.
static void moduleDestroy(uint8_t* st)
{
	uint8_t* hdr = PTR(st, 0);
	uint8_t* inst = PTR(st, 4);
	uint8_t* cls = PTR(st, 8);
	listUnlink(hdr, 0x38, inst);
	listUnlink(s_timerHead, 0, inst + 8);
	uint8_t* c = inst + 0x18;
	if (U8(hdr, 0x25)) {
		listUnlink(cls, 0xc, c);
		classUnref(cls);
		c += 0x14;
	}
	if (U8(hdr, 0x26)) {
		listUnlink(cls, 8, c);
		classUnref(cls);
	}
	int npl = U8(hdr, 0x24), nctl = U8(hdr, 0x27);
	for (int i = 0; i < npl; i++) {
		uint8_t* ps = inst + R32(hdr, 0x3c + 4 * i);
		if (S8(ps, 0x10) >= 0) aemsVoiceStop(R32(ps, 8));
	}
	for (int i = 0; i < nctl; i++) {
		uint8_t* cs = inst + R32(hdr, 0x3c + 4 * (npl + i));
		if (R32(cs, 8)) classRelease(PTR(cs, 8));
	}
	W16(hdr, 0x1c, (uint16_t)(S16(hdr, 0x1c) - 1));
	s_live--;
	instFree(inst);
}

// ---------------------------------------------------------------- nodes
//
// Each takes the graph's state pointer (esi) and returns eax. Offsets are the
// PC node's own.

static int32_t nClassDestructor(uint8_t* s) { int32_t v = R32(s, 0x10); W32(s, 0x10, 0); return v; }
static int32_t nClassData(uint8_t* s)       { return R32(s, 0x14); }
static int32_t nCreate(uint8_t* s)          { int32_t v = R32(s, 0); W32(s, 0, 0); return v; }

static int32_t nDestroy(uint8_t* s)
{
	if (R32(s, 0xc)) moduleDestroy(s);
	return 0;
}

static int32_t nCounter(uint8_t* s)
{
	int32_t in = R32(s, 0x14), lo = R32(s, 0), hi = R32(s, 4);
	if (in >= lo && in <= hi) return in;
	if (R32(s, 0x10) > 0) {
		int32_t v = (int32_t)((uint32_t)R32(s, 8) + (uint32_t)(int32_t)S8(s, 0xc));
		if (v > hi) v = lo;
		else if (v < lo) v = hi;
		W32(s, 8, v);
	}
	return R32(s, 8);
}

static int32_t nRandom(uint8_t* s)
{
	if (R32(s, 0xc) == 0) return R32(s, 8);
	uint32_t r = isrand(), range = (uint32_t)R32(s, 4);
	int32_t v = (int32_t)((uint32_t)R32(s, 0) + (range ? r % range : 0));
	W32(s, 8, v);
	return v;
}

static int32_t nRandomShuffle(uint8_t* s)
{
	if (R32(s, U16(s, 0)) == 0) return R32(s, 0xc);
	uint32_t lo = U16(s, 8);
	uint32_t span = (uint32_t)U16(s, 0xa) - (uint32_t)(int32_t)S8(s, 3) - lo;
	uint32_t r = isrand();
	uint32_t j = (span ? r % span : 0) + lo;
	int32_t v;
	if (U8(s, 2) == 1) {
		v = U8(s, 0x10 + (int32_t)j);
		W8(s, 0x10 + (int32_t)j, U8(s, 0x10 + (int32_t)lo));
		W8(s, 0x10 + U16(s, 8), (uint8_t)v);
	} else {
		v = U16(s, 0x10 + 2 * (int32_t)j);
		W16(s, 0x10 + 2 * (int32_t)j, U16(s, 0x10 + 2 * (int32_t)lo));
		W16(s, 0x10 + 2 * U16(s, 8), (uint16_t)v);
	}
	v += R32(s, 4);
	W32(s, 0xc, v);
	uint16_t idx = (uint16_t)(U16(s, 8) + 1);
	W16(s, 8, idx);
	if (U16(s, 0xa) <= idx) { W16(s, 8, 0); W8(s, 3, 1); }
	else W8(s, 3, 0);
	return R32(s, 0xc);
}

static int32_t nRandomWeighted(uint8_t* s)
{
	if (R32(s, 0x10)) {
		uint32_t r = isrand() % 100, cum = 0;
		const uint8_t* w = PTR(s, 0);
		int32_t n = R32(s, 8);
		for (int32_t i = 0; i < n; i++) {
			cum += (uint32_t)(int32_t)S8(w, 0x10 + i);
			if (r < cum) { W32(s, 0xc, R32(s, 4) + i); break; }
		}
	}
	return R32(s, 0xc);
}

static int32_t nRangeTrig(uint8_t* s)
{
	int32_t in = R32(s, 0x14);
	if (in >= R32(s, 0) && in <= R32(s, 4)) {
		if (U8(s, 0x10) == 0) { W8(s, 0x10, 1); W8(s, 0x11, 1); return 1; }
	} else if (in >= R32(s, 8) && in <= R32(s, 0xc)) {
		W8(s, 0x10, 0);
	}
	W8(s, 0x11, 0);
	return 0;
}

#define AEMS_TICK_MS 20.0f   // the AEMS timer period (SPEED2.EXE 0x8B4054)

static int32_t nDelayTrig(uint8_t* s)
{
	if (R32(s, 8)) WF(s, 0, 0.0f);
	float t = RF(s, 0);
	if (!(t < 0.0f)) {
		if ((float)R32(s, 0xc) <= t) { WF(s, 0, -1.0f); W8(s, 4, 1); return 1; }
		WF(s, 0, t + AEMS_TICK_MS);
	}
	W8(s, 4, 0);
	return 0;
}

static int32_t nStateGen(uint8_t* s)
{
	const uint8_t* in = s + U16(s, 0);
	int n = U8(s, 2);
	for (int i = 0; i < n; i++)
		if (R32(in, 4 * i)) { W32(s, 4, R32(s, 8 + 4 * i)); break; }
	return R32(s, 4);
}

static int32_t nMerge(uint8_t* s)
{
	int n = U8(s, 0);
	for (int i = 0; i < n; i++)
		if (R32(s, 4 + 4 * i)) return 1;
	return 0;
}

// Envelope state: +0 u16 input offset, +2 s8 last input, +3 u8 segment,
// +4 f time left, +8 f step, +0xc f value, +0x10 u8 segments,
// +0x12 s16 sustain segment, +0x14 f start, +0x18 {f time, f level}[].
// Input 1 = attack/run, 3 = release, 2 = hold.
static void envProgram(uint8_t* s)
{
	int seg = U8(s, 3);
	float time = RF(s, 0x18 + seg * 8);
	WF(s, 4, time);
	WF(s, 8, (RF(s, 0x1c + seg * 8) - RF(s, 0xc)) / time * AEMS_TICK_MS);
}

static int32_t nEnvelope(uint8_t* s)
{
	int32_t in = R32(s, U16(s, 0));
	int8_t last = S8(s, 2);
	if (in == 1 && last == 0) {
		W8(s, 3, 0);
		WF(s, 0xc, RF(s, 0x14));
		envProgram(s);
	} else if (in == 3 && last != 3 && S16(s, 0x12) > (int16_t)U8(s, 3)) {
		W8(s, 3, (uint8_t)S16(s, 0x12));
		envProgram(s);
	} else if (in == 1 || in == 3) {
		uint8_t seg = U8(s, 3);
		if (seg >= U8(s, 0x10)) {
			WF(s, 0xc, 0.0f);
		} else {
			float left = RF(s, 4) - AEMS_TICK_MS;
			WF(s, 4, left);
			if (left <= 0.0f) {
				WF(s, 0xc, RF(s, 0x1c + seg * 8));
				W8(s, 3, (uint8_t)(seg + 1));
				if ((uint8_t)(seg + 1) < U8(s, 0x10)) envProgram(s);
				else WF(s, 0xc, 0.0f);
			} else {
				WF(s, 0xc, RF(s, 8) + RF(s, 0xc));
			}
		}
	} else if (in != 2) {
		WF(s, 0xc, 0.0f);
	}
	W8(s, 2, (uint8_t)in);
	return rnd(RF(s, 0xc));
}

// Table: state {table, last input, value, input}; the table is {u8 type
// (1 s8, 2 s16, else s32), u16 count @2, s32 min @4, s32 max @8,
// f step @0xc, data @0x10}.
static int32_t tblElem(const uint8_t* t, int32_t i)
{
	switch (U8(t, 0)) {
	case 2:  return S16(t, 0x10 + 2 * i);
	case 1:  return S8(t, 0x10 + i);
	default: return R32(t, 0x10 + 4 * i);
	}
}

static int32_t nTable(uint8_t* s)
{
	int32_t in = R32(s, 0xc);
	if (in != R32(s, 4)) {
		const uint8_t* t = PTR(s, 0);
		W32(s, 4, in);
		int32_t lo = R32(t, 4), hi = R32(t, 8);
		int32_t x = (in >= lo && in <= hi ? in : (in < lo ? lo : hi)) - lo;
		float step = RF(t, 0xc);
		int32_t v;
		if (step == 1.0f) {
			v = tblElem(t, x);
		} else {
			float f = (float)x * step;
			int32_t i0 = rnd(f - 0.5f), i1 = i0 + 1;
			if (i1 >= (int32_t)U16(t, 2)) i1 = U16(t, 2) - 1;
			float e0 = (float)tblElem(t, i0), e1 = (float)tblElem(t, i1);
			v = rnd((e1 - e0) * (f - (float)i0) + e0);
		}
		W32(s, 8, v);
	}
	return R32(s, 8);
}

// Delay line: +0 u16 input offset, +2 u16 length, +4 u16 write, +6 u16 read,
// +8 s32 last delay, +0xc s32 buffer[length]; inputs {value, delay ms}.
static int32_t nDelayLine(uint8_t* s)
{
	uint16_t io = U16(s, 0);
	int32_t d = R32(s, io + 4);
	if (d != R32(s, 8)) {
		W32(s, 8, d);
		if (d < 0) { d = 0; W32(s, io + 4, 0); }
		int32_t k = rnd((float)d / AEMS_TICK_MS);
		if ((int32_t)U16(s, 2) <= k) k = U16(s, 2) - 1;
		W16(s, 4, (uint16_t)(U16(s, 6) + k));
	}
	uint16_t len = U16(s, 2);
	if (len <= U16(s, 4)) W16(s, 4, (uint16_t)(U16(s, 4) - len));
	if (len <= U16(s, 6)) W16(s, 6, 0);
	W32(s, 0xc + 4 * U16(s, 4), R32(s, io));
	int32_t v = R32(s, 0xc + 4 * U16(s, 6));
	W16(s, 4, (uint16_t)(U16(s, 4) + 1));
	W16(s, 6, (uint16_t)(U16(s, 6) + 1));
	return v;
}

static int32_t nMux(uint8_t* s)
{
	int32_t sel = R32(s, 4);
	return (sel > 0 && sel <= U8(s, 0)) ? R32(s, 4 + 4 * sel) : 0;
}

static int32_t nDemux(uint8_t* s)
{
	W32(s, S16(s, 2) * 4 + 8, 0);
	int32_t sel = R32(s, 4);
	if (sel > 0 && sel <= U8(s, 0)) {
		W32(s, sel * 4 + 8, R32(s, 8));
		W16(s, 2, (uint16_t)sel);
	}
	return R32(s, 0xc);
}

static int32_t nMin(uint8_t* s)
{
	int n = U8(s, 0);
	int32_t v = R32(s, 4);
	for (int i = 1; i < n; i++) { int32_t x = R32(s, 4 + 4 * i); if (x < v) v = x; }
	return v;
}

static int32_t nMax(uint8_t* s)
{
	int n = U8(s, 0);
	int32_t v = R32(s, 4);
	for (int i = 1; i < n; i++) { int32_t x = R32(s, 4 + 4 * i); if (x > v) v = x; }
	return v;
}

static int32_t nScale(uint8_t* s)
{
	int n = U8(s, 0);
	float f = (float)R32(s, 8);
	for (int i = 1; i < n; i++) f *= (float)R32(s, 8 + 4 * i);
	return rnd(f * RF(s, 4));
}

static int32_t nAdd(uint8_t* s)
{
	int n = U8(s, 0);
	uint32_t v = (uint32_t)R32(s, 4);
	for (int i = 1; i < n; i++) v += (uint32_t)R32(s, 4 + 4 * i);
	return (int32_t)v;
}

static int32_t nSubtract(uint8_t* s) { return (int32_t)((uint32_t)R32(s, 0) - (uint32_t)R32(s, 4)); }
static int32_t nMultiply(uint8_t* s) { return (int32_t)((uint32_t)R32(s, 4) * (uint32_t)R32(s, 0)); }

static int32_t nDivide(uint8_t* s)
{
	int32_t a = R32(s, 0), b = R32(s, 4);
	if (b == 0) return 0;
	if (b == -1) return (int32_t)(0u - (uint32_t)a);
	return a / b;
}

static int32_t nModulo(uint8_t* s)
{
	int32_t a = R32(s, 0), b = R32(s, 4);
	if (b == 0 || b == -1) return 0;
	return a % b;
}

// Player: the state in +0x18 is clamped to 0 stop, 1 play, 2 pause; a
// change from the last one starts, stops, pauses or resumes the voice.
static int32_t nPlayer(uint8_t* s)
{
	int32_t st = clampi(R32(s, 0x18), 0, 2);
	int8_t cur = S8(s, 0xc);
	if (st != cur) {
		int8_t act = S8(s, 0x10);
		if (st == 0) {
			if (act >= 0) { aemsVoiceStop(R32(s, 8)); playerReset(s); }
		} else if (st == 1) {
			if (act >= 0) {
				playerUnpause(s);
			} else if (!(cur == 2 && S8(s, 0xd) == 1) && PTR(s, 4)) {
				const uint8_t* tbl = PTR(s, 4);
				int32_t cnt = R32(tbl, 0), i = R32(s, 0x14);
				if (i >= cnt) i = cnt - 1;
				else if (i < 0) i = 0;
				const uint8_t* ent = tbl + 4 + 12 * i;
				W8(s, 0x10, U8(ent, 0));
				int32_t h = S8(ent, 0) == 0 ? playerPlay(s, ent) : -1;
				W32(s, 8, h);
				if (h < 0) playerReset(s);
			}
		} else if (act >= 0) {
			aemsVoicePause(R32(s, 8), true);
		}
		W8(s, 0xd, U8(s, 0xc));
		W8(s, 0xc, (uint8_t)st);
	}
	int32_t ret = st;
	if (st == 1 && S8(s, 0x10) >= 0) ret = playerUpdate(s);
	if (S8(s, 0x10) < 0) ret = 0;
	return ret;
}

// Oscillator: +0 u8 wave (0 sine, 1 square, 2 saw, else triangle),
// +4 f phase 0..1, +8 s32 period ms, +0xc s32 amplitude.
static int32_t nOscillator(uint8_t* s)
{
	int32_t period = R32(s, 8);
	if (period < 1) return 0;
	float inc = AEMS_TICK_MS / (float)period;
	int32_t amp = R32(s, 0xc);
	float a = (float)amp, ph = RF(s, 4), v;
	if (ph >= 1.0f) {
		do ph -= 1.0f; while (ph >= 1.0f);
		WF(s, 4, ph);
	}
	switch (U8(s, 0)) {
	case 0:  v = (float)isin(rnd(ph * 1024.0f)) * a * (1.0f / 65536.0f); break;
	case 1:  v = ph < 0.5f ? 0.0f : a; break;
	case 2:  v = a * ph; break;
	default: v = ph < 0.5f ? a * ph + a * ph : ((1.0f - ph) * a) * 2.0f; break;
	}
	WF(s, 4, inc + ph);
	return rnd(v);
}

// Ramp: +0 f value, +4 f slope, +8 last target, +0xc last time, +0x10 time
// (ms to cover the distance at rate 0x1000), +0x14 rate, +0x18 target.
static int32_t nRamp(uint8_t* s)
{
	int32_t tgt = R32(s, 0x18);
	float tf = (float)tgt;
	if (tf == RF(s, 0)) return tgt;
	if (tgt != R32(s, 8) || R32(s, 0x10) != R32(s, 0xc)) {
		int32_t time = R32(s, 0x10);
		W32(s, 8, tgt);
		W32(s, 0xc, time);
		if (time <= 0) { WF(s, 0, tf); return tgt; }
		WF(s, 4, (tf - RF(s, 0)) / (float)time * AEMS_TICK_MS * (1.0f / 4096.0f));
	}
	float slope = RF(s, 4);
	float cur = (float)R32(s, 0x14) * slope + RF(s, 0);
	if (slope < 0.0f ? !(tf <= cur) : !(cur <= tf)) cur = tf;
	WF(s, 0, cur);
	return rnd(cur);
}

static int32_t nAddMax(uint8_t* s)
{
	int n = U8(s, 0);
	uint32_t v = (uint32_t)R32(s, 8);
	for (int i = 1; i < n; i++) v += (uint32_t)R32(s, 8 + 4 * i);
	int32_t m = R32(s, 4);
	return (int32_t)v > m ? m : (int32_t)v;
}

static int32_t nSubtractMin(uint8_t* s)
{
	int32_t v = (int32_t)((uint32_t)R32(s, 4) - (uint32_t)R32(s, 8)), m = R32(s, 0);
	return v < m ? m : v;
}

static int32_t nMultiplyMax(uint8_t* s)
{
	int32_t v = (int32_t)((uint32_t)R32(s, 8) * (uint32_t)R32(s, 4)), m = R32(s, 0);
	return v > m ? m : v;
}

static int32_t nMin2(uint8_t* s) { int32_t a = R32(s, 0), b = R32(s, 4); return a < b ? a : b; }
static int32_t nMax2(uint8_t* s) { int32_t a = R32(s, 0), b = R32(s, 4); return a > b ? a : b; }
static int32_t nScale2(uint8_t* s) { return rnd((float)R32(s, 8) * (float)R32(s, 4) * RF(s, 0)); }
static int32_t nAdd2(uint8_t* s) { return (int32_t)((uint32_t)R32(s, 4) + (uint32_t)R32(s, 0)); }
static int32_t nFunction(uint8_t* s) { int32_t v = U8(s, 0x19); W8(s, 0x19, 0); return v; }

// Control class: a graph that drives another class (a car's sputter
// outputs). +0 class handle, +8 instance, +0xc u8 clamped, +0xd u8 params,
// then {min, max}[params] if clamped, then inputs {create, release,
// params[]}.
static void ctlClamp(uint8_t* s, uint8_t* params)
{
	if (!U8(s, 0xc)) return;
	int n = U8(s, 0xd);
	for (int i = 0; i < n; i++) {
		int32_t lo = R32(s, 0x10 + 8 * i), hi = R32(s, 0x14 + 8 * i), v = R32(params, 4 * i);
		W32(params, 4 * i, lo <= v ? (hi < v ? hi : v) : lo);
	}
}

static int32_t nControlClass(uint8_t* s)
{
	uint8_t* in = s + 0x10 + (U8(s, 0xc) ? U8(s, 0xd) * 8 : 0);
	uint8_t* params = in + 8;
	uint8_t* inst = PTR(s, 8);
	if (R32(in, 4)) {
		if (inst) { classRelease(inst); W32(s, 8, 0); }
	} else if (R32(in, 0) == 0) {
		if (inst) {
			ctlClamp(s, params);
			classSend(inst, params);
			classTell(inst, params, false);
		}
	} else if (!inst) {
		ctlClamp(s, params);
		WPTR(s, 8, classCreate(PTR(s, 0), params));
	}
	inst = PTR(s, 8);
	return inst ? R32(inst, 4) : 0;
}

// GlobalVariable, CallFunction and SetGlobalVariable read or call things no
// PC bank declares (#298 survey); nothing calls them.
static int32_t nUnused(uint8_t* s) { (void)s; return 0; }

typedef int32_t (*AemsNode)(uint8_t*);

static const AemsNode s_nodes[NODE_COUNT] = {
	nClassDestructor, nClassData, nUnused, nCreate, nDestroy,
	nUnused, nCounter, nRandom, nRandomShuffle, nRandomWeighted,
	nRangeTrig, nDelayTrig, nStateGen, nMerge, nEnvelope,
	nTable, nDelayLine, nMux, nDemux, nMin,
	nMax, nScale, nAdd, nSubtract, nMultiply,
	nDivide, nModulo, nPlayer, nOscillator, nRamp,
	nAddMax, nSubtractMin, nMultiplyMax, nMin2, nMax2,
	nScale2, nAdd2, nFunction, nControlClass, nUnused,
};

// ---------------------------------------------------------------- the VM

static void vmRun(const AemsMod* m, uint8_t* esi)
{
	const uint32_t* w = m->code;
	uint32_t pc = 0, steps = 0;
	int32_t eax = 0, ecx = 0, cmp = 0;
	for (;;) {
		// Every PC graph is straight-line with forward branches; anything that
		// runs this long is a broken bank, not a long graph.
		if (pc >= m->words || ++steps > 1u << 16) {
			dlogf("%s: module at %p ran off its code", m->bank->name, (void*)m->hdr);
			return;
		}
		uint32_t op = w[pc];
		switch (op & 0xff) {
		case OP_END:   return;
		case OP_CALL:  eax = s_nodes[op >> 8](esi); pc += 1; break;
		case OP_LD:    eax = R32(esi, (int32_t)w[pc + 1]); pc += 2; break;
		case OP_LDC:   ecx = R32(esi, (int32_t)w[pc + 1]); pc += 2; break;
		case OP_ST:    W32(esi, (int32_t)w[pc + 1], eax); pc += 2; break;
		case OP_STB:   W8(esi, (int32_t)w[pc + 1], (uint8_t)w[pc + 2]); pc += 3; break;
		case OP_ADD:   eax = (int32_t)((uint32_t)eax + (uint32_t)R32(esi, (int32_t)w[pc + 1])); pc += 2; break;
		case OP_SUB:   eax = (int32_t)((uint32_t)eax - (uint32_t)R32(esi, (int32_t)w[pc + 1])); pc += 2; break;
		case OP_IMUL:  eax = (int32_t)((uint32_t)eax * (uint32_t)R32(esi, (int32_t)w[pc + 1])); pc += 2; break;
		case OP_MOVAC: eax = ecx; pc += 1; break;
		case OP_CMP:   cmp = (eax > ecx) - (eax < ecx); pc += 1; break;
		case OP_JG:    pc = cmp > 0 ? w[pc + 1] : pc + 2; break;
		case OP_JL:    pc = cmp < 0 ? w[pc + 1] : pc + 2; break;
		case OP_ESI:   esi += (int32_t)w[pc + 1]; pc += 2; break;
		default:       return;
		}
	}
}

// Checked once at load, so the tick can trust the code.
static bool vmVerify(const uint32_t* w, uint32_t words)
{
	static const uint8_t len[OP_COUNT] = { 1, 1, 2, 2, 2, 3, 2, 2, 2, 1, 1, 2, 2, 2 };
	bool ended = false;
	for (uint32_t pc = 0; pc < words; ) {
		uint32_t op = w[pc] & 0xff;
		if (op >= OP_COUNT || pc + len[op] > words) return false;
		if (op == OP_CALL && (w[pc] >> 8) >= NODE_COUNT) return false;
		if ((op == OP_JG || op == OP_JL) && w[pc + 1] >= words) return false;
		if (op == OP_END) ended = true;
		pc += len[op];
	}
	return ended;
}

// ---------------------------------------------------------------- API

static void tick(void)
{
	s_inTick = true;
	// The next node is read first: a graph's destroy node unlinks its own.
	for (uint8_t* n = PTR(s_timerHead, 0); n; ) {
		uint8_t* node = n;
		n = PTR(node, 0);
		vmRun((const AemsMod*)PTR(node, 8), PTR(node, 0xc));
	}
	s_inTick = false;
	while (s_grave) {
		uint8_t* g = s_grave;
		s_grave = PTR(g, 0);
		aemFree(g);
	}
}

static float s_accumMs;

void aemsUpdate(float seconds)
{
	s_accumMs += seconds * 1000.0f;
	// A long hitch catches up at most 100 ms, as the timer would have queued.
	if (s_accumMs > 5.0f * AEMS_TICK_MS) s_accumMs = 5.0f * AEMS_TICK_MS;
	while (s_accumMs >= AEMS_TICK_MS) {
		s_accumMs -= AEMS_TICK_MS;
		tick();
	}
}

int aemsLiveInstances(void) { return s_live; }

// #351: the voice of every player node of every graph still ticking. A voice
// no graph holds has nothing left to stop it.
int aemsPlayerHandles(int32_t* out, int max)
{
	int n = 0;
	for (uint8_t* nd = PTR(s_timerHead, 0); nd; nd = PTR(nd, 0)) {
		const AemsMod* m = (const AemsMod*)PTR(nd, 8);
		if (!m || !m->hdr) continue;
		const uint8_t* hdr = m->hdr;
		uint8_t* inst = nd - 8;
		int npl = U8(hdr, 0x24);
		for (int i = 0; i < npl && n < max; i++) {
			uint8_t* ps = inst + R32(hdr, 0x3c + 4 * i);
			if (S8(ps, 0x10) >= 0) out[n++] = R32(ps, 8);
		}
	}
	return n;
}

AemsClass aemsClass(const char* name) { return name ? descFind(name) : NULL; }

AemsInst aemsCreate(AemsClass cls, const int32_t* params)
{
	return classCreate((uint8_t*)cls, params);
}

void aemsSetMemberData(AemsInst inst, const int32_t* params)
{
	if (!inst) return;
	classSend((uint8_t*)inst, params);
	classTell((uint8_t*)inst, params, false);
}

void aemsWatch(AemsClass cls, AemsWatchFn fn, void* user)
{
	if (!cls || !fn) return;
	for (int i = 0; i < WATCH_MAX; i++) {
		if (!s_watch[i].fn) {
			s_watch[i].desc = (uint8_t*)cls;
			s_watch[i].fn = fn;
			s_watch[i].user = user;
			return;
		}
	}
}

void aemsUnwatch(AemsClass cls, AemsWatchFn fn)
{
	for (int i = 0; i < WATCH_MAX; i++)
		if (s_watch[i].fn == fn && s_watch[i].desc == (uint8_t*)cls) memset(&s_watch[i], 0, sizeof(s_watch[i]));
}

void aemsRelease(AemsInst inst)
{
	if (inst) classRelease((uint8_t*)inst);
}

int aemsRefCount(AemsInst inst) { return inst ? R32((uint8_t*)inst, 4) : 0; }

static uint8_t* readFile(const char* path, uint32_t* outSize)
{
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* buf = n > (long)sizeof(AemFileHeader) ? (uint8_t*)aemAllocLinear((uint32_t)n) : NULL;
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { aemFreeLinear(buf); buf = NULL; }
	fclose(f);
	*outSize = (uint32_t)n;
	return buf;
}

static bool inFile(uint32_t off, uint64_t bytes, uint32_t size)
{
	return off <= size && bytes <= size - off;
}

AemsBank* aemsLoad(const char* path, int bus)
{
	uint32_t size = 0;
	uint8_t* file = readFile(path, &size);
	if (!file) return NULL;
	AemFileHeader h;
	memcpy(&h, file, sizeof(h));
	bool ok = memcmp(h.magic, "AEM3", 4) == 0 && h.version == 1 &&
	          inFile(h.imageOff, h.imageSize, size) && h.imageSize >= 0x44 &&
	          inFile(h.relocOff, (uint64_t)h.relocCount * 4, size) &&
	          inFile(h.csisOff, (uint64_t)h.csisCount * sizeof(AemCsisRec), size) &&
	          inFile(h.modOff, (uint64_t)h.modCount * sizeof(AemModRec), size) &&
	          inFile(h.sampleOff, (uint64_t)h.sampleCount * sizeof(AemSampleRec), size);
	AemsBank* b = NULL;
	if (ok) b = (AemsBank*)aemAlloc((uint32_t)(sizeof(AemsBank) + h.modCount * sizeof(AemsMod)));
	if (b && h.sampleCount) b->clips = (AemsClip*)calloc(h.sampleCount, sizeof(AemsClip));
	if (!b || (h.sampleCount && !b->clips)) {
		if (!ok) dlogf("%s: not a version-1 .aem", path);
		if (b) { free(b->clips); aemFree(b); }
		aemFreeLinear(file);
		return NULL;
	}
	b->file = file;
	b->image = file + h.imageOff;
	b->bus = bus;
	memcpy(b->name, h.name, sizeof(b->name));
	b->name[sizeof(b->name) - 1] = 0;

	uint8_t* img = b->image;
	for (uint32_t i = 0; i < h.relocCount; i++) {
		uint32_t r = (uint32_t)R32(file, (int32_t)(h.relocOff + 4 * i));
		if (r + 4 <= h.imageSize) W32(img, (int32_t)r, (int32_t)((uint32_t)R32(img, (int32_t)r) + AO(img)));
	}
	WPTR(img, 0x40, b);
	for (uint32_t i = 0; i < h.csisCount; i++) {
		AemCsisRec c;
		memcpy(&c, file + h.csisOff + i * sizeof(c), sizeof(c));
		c.name[sizeof(c.name) - 1] = 0;
		if (c.kind != 1 || c.imageOff + 8 > h.imageSize) continue;   // every PC entry is a class
		WPTR(img, (int32_t)c.imageOff, descFind(c.name));
		W32(img, (int32_t)c.imageOff + 4, 0);
	}
	for (uint32_t i = 0; i < h.modCount; i++) {
		AemModRec mr;
		memcpy(&mr, file + h.modOff + i * sizeof(mr), sizeof(mr));
		AemsMod* m = &b->mods[b->nmods];
		if (!inFile(mr.codeOff, (uint64_t)mr.words * 4, size) || mr.hdrOff + 0x3c > h.imageSize ||
				!vmVerify((const uint32_t*)(file + mr.codeOff), mr.words)) {
			dlogf("%s: module %u is malformed, skipped", b->name, (unsigned)i);
			continue;
		}
		uint8_t* hdr = img + mr.hdrOff;
		m->bank = b;
		m->hdr = hdr;
		m->code = (const uint32_t*)(file + mr.codeOff);
		m->words = mr.words;
		b->nmods++;
		W32(hdr, 0x14, CB_CTOR);
		WPTR(hdr, 0x18, hdr);
		W32(hdr, 0x2c, (int32_t)((uint32_t)R32(hdr, 0x2c) + AO(img)));
		WPTR(hdr, 0x28, m);
		W32(hdr, 0x38, 0);
		W16(hdr, 0x1c, 0);
		uint8_t* desc = PTR(hdr, 4);
		if (desc) listPush(desc, 0, hdr + 0xc);
		uint8_t* tmpl = PTR(hdr, 0x2c);
		for (int p = 0; p < U8(hdr, 0x24); p++) WPTR(tmpl, R32(hdr, 0x3c + 4 * p), img);
	}
	b->nclips = h.sampleCount;
	for (uint32_t i = 0; i < h.sampleCount; i++) {
		const AemSampleRec* r = (const AemSampleRec*)(file + h.sampleOff + i * sizeof(AemSampleRec));
		if (!r->bytes || !inFile(r->dataOff, r->bytes, size)) continue;
		AemsClip* c = &b->clips[i];
		c->data = file + r->dataOff;
		c->bytes = r->bytes;
		c->samples = r->samples;
		c->rate = r->rate;
		c->loop = r->loop != 0;
		c->coefs = r->coefs;
		c->ps = r->ps;
		c->hist0 = r->hist0;
		c->hist1 = r->hist1;
	}
	aemFlushDsp(file, size);
	dlogf("%s: %u module(s), %u sample(s)", b->name, (unsigned)b->nmods, (unsigned)b->nclips);
	return b;
}

void aemsUnload(AemsBank* b)
{
	if (!b) return;
	for (uint32_t i = 0; i < b->nmods; i++) {
		uint8_t* hdr = b->mods[i].hdr;
		while (PTR(hdr, 0x38)) moduleDestroy(PTR(hdr, 0x38) + R32(hdr, 0x34));
		uint8_t* desc = PTR(hdr, 4);
		if (desc) listUnlink(desc, 0, hdr + 0xc);
	}
	// Any voice still on these clips lost its player (#330): named, then cut.
	const int lost = aemsVoiceDropClips(b->clips, b->nclips);
	if (lost) dlogf("%s: %d voice(s) still on its samples at unload -- cut (#330)", b->name, lost);
	aemsVoiceCutFades();   // the voices just stopped still read this file
	free(b->clips);
	aemFreeLinear(b->file);
	aemFree(b);
}
