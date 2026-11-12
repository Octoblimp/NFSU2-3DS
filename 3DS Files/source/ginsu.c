// Ginsu, EA's granular engine synth. See ginsu.h.
#include "ginsu.h"

#include "audio.h"
#include "dbglog.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ginLog(const char* fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	dbglog_line("GINSU ", b);
}

// (int)(x +- 0.5), retail's rounding everywhere here (0.5 at 0x8031F5F4 etc.).
static int32_t rnd(float x)
{
	return x <= 0.0f ? (int32_t)(x - 0.5f) : (int32_t)(x + 0.5f);
}

// ---------------------------------------------------------------- data

// xafilterf (0x803567F4): the predictor pairs, k0 [0..3] and k1 [4..7].
static const float kFilter[8] = { 0.0f, 0.9375f, 1.796875f, 1.53125f, 0.0f, 0.0f, -0.8125f, -0.859375f };

// xatablef (0x80356814): 16 rows of 16, the signed nibble << (12 - row), and 0
// past row 12.
static float s_nib[16][16];
static bool  s_nibReady;

static void nibInit(void)
{
	for (int row = 0; row < 16; row++)
		for (int n = 0; n < 16; n++)
			s_nib[row][n] = row > 12 ? 0.0f : (float)((n >= 8 ? n - 16 : n) * (1 << (12 - row)));
	s_nibReady = true;
}

// DecodeBlock (0x800AFC28): 32 samples from 19 bytes -- two 12-bit seeds in
// the header, then 30 nibbles through the block's predictor and row.
static void decodeBlock(GinsuData* g, int32_t b)
{
	const uint8_t* p = g->data + b * 19;
	g->blockIdx = b;
	g->block[0] = (float)((int32_t)(p[0] & 0xf0) + (int32_t)(int8_t)p[1] * 0x100);
	g->block[1] = (float)((int32_t)(p[2] & 0xf0) + (int32_t)(int8_t)p[3] * 0x100);
	// Port guard: retail indexes xafilterf with the whole nibble, which reads
	// past its 8 floats above 3; no shipped .gin uses more than 3.
	const int fi = (p[0] & 0xf) < 4 ? (p[0] & 0xf) : 0;
	const float k0 = kFilter[fi], k1 = kFilter[fi + 4];
	const float* t = s_nib[p[2] & 0xf];
	float* o = &g->block[2];
	for (int i = 0; i < 15; i++) {
		const uint8_t x = p[4 + i];
		const float a = k1 * o[-2] + k0 * o[-1] + t[x >> 4];
		o[0] = a;
		o[1] = k1 * o[-1] + k0 * a + t[x & 0xf];
		o += 2;
	}
}

// GetSamples (0x800B07E8). Nothing is written when the run is out of range.
static bool getSamples(GinsuData* g, int32_t pos, int32_t n, int16_t* out)
{
	if (pos < 0 || g->ns <= pos + n - 1) return false;
	if (pos >> 5 != g->blockIdx) decodeBlock(g, pos >> 5);
	int32_t k = pos & 0x1f;
	for (int32_t i = 0; i < n; i++) {
		int32_t v = (int32_t)g->block[k];
		if (v > 0x7fff) v = 0x7fff;
		if (v < -0x8000) v = -0x8000;
		out[i] = (int16_t)v;
		if (k++ == 0x1f) {
			// Retail decodes the next block here even after the run's last
			// sample; the load pads the file so the last block's next is zeros.
			decodeBlock(g, g->blockIdx + 1);
			k = 0;
		}
	}
	return true;
}

// FrequencyToSample (0x800AFED0).
static int32_t freqToSample(const GinsuData* g, float f)
{
	if (g->nf < 1) return 0;
	if (!(g->minFreq < f)) return g->freq[0];
	if (!(f < g->maxFreq)) return g->freq[g->nf];
	const float x = (float)g->nf * (f - g->minFreq) / (g->maxFreq - g->minFreq);
	const int32_t i = (int32_t)floorf(x);
	return rnd((x - (float)i) * (float)(g->freq[i + 1] - g->freq[i]) + (float)g->freq[i]);
}

// CycleToSample (0x800B0088).
static int32_t cycleToSample(const GinsuData* g, float c)
{
	if (g->nc < 1) return 0;
	if (!(0.0f < c)) return g->cyc[0];
	if (!(c < (float)g->nc)) return g->cyc[g->nc];
	const int32_t i = (int32_t)floorf(c);
	return rnd((c - (float)i) * (float)(g->cyc[i + 1] - g->cyc[i]) + (float)g->cyc[i]);
}

// CyclePeriod (0x800B0220): the cycle's length in samples, from the centred
// differences either side of it.
static float cyclePeriod(const GinsuData* g, float c)
{
	const int32_t nc = g->nc;
	const int32_t* y = g->cyc;
	if (nc < 1) return 0.0f;
	int32_t i = (int32_t)floorf(c);
	float p0, p1;
	if (i < 1) {
		if (i < 0) { i = 0; c = 0.0f; }
		p0 = (float)(y[1] - y[0]);
		p1 = (float)(y[2] - y[0]) * 0.5f;
	} else if (i < nc - 1) {
		p0 = (float)(y[i + 1] - y[i - 1]) * 0.5f;
		p1 = (float)(y[i + 2] - y[i]) * 0.5f;
	} else {
		if (nc <= i) { c = (float)nc; i = nc - 1; }
		p0 = (float)(y[nc] - y[nc - 2]) * 0.5f;
		p1 = (float)(y[nc] - y[nc - 1]);
	}
	return (c - (float)i) * (p1 - p0) + p0;
}

// ceil for x > 0, truncation otherwise -- retail's inline for a count.
static int32_t ceilCount(float x)
{
	return x <= 0.0f ? (int32_t)x : (int32_t)ceilf(x);
}

// SampleToCycle (0x800B0480): the fractional cycle a sample is in -- an
// interpolation search that narrows its bounds by the shortest cycle.
static float sampleToCycle(const GinsuData* g, int32_t s)
{
	int32_t hi = g->nc;
	const int32_t* y = g->cyc;
	if (hi < 1 || s <= y[0]) return 0.0f;
	if (s >= y[hi]) return (float)hi;
	int32_t lo = 0, i, yi;
	for (;;) {
		for (;;) {
			const float f = ((float)(s - y[lo]) / (float)(y[hi] - y[lo])) * (float)(hi - lo);
			i = lo + (int32_t)floorf(f);
			yi = y[i];
			if (yi <= s) break;
			const int32_t k = ceilCount((float)(yi - s) / g->minCycle);
			hi = i;
			if (lo < i - k) lo = i - k;
		}
		if (s < y[i + 1]) break;
		const int32_t k = ceilCount((float)(s - yi) / g->minCycle);
		lo = i + 1;
		if (i + k + 1 < hi) hi = i + k + 1;
	}
	return (float)i + ((float)s - (float)yi) / ((float)y[i + 1] - (float)yi);
}

bool ginsuLoad(GinsuData* g, const char* path)
{
	memset(g, 0, sizeof(*g));
	g->blockIdx = -1;
	if (!s_nibReady) nibInit();
	FILE* f = fopen(path, "rb");
	if (!f) {
		ginLog("no %s", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	// 32 zero bytes past the end: GetSamples decodes one block past a run that
	// ends a block (see getSamples).
	uint8_t* d = n >= 0x24 && n < (4 << 20) ? calloc((size_t)n + 32, 1) : NULL;
	const bool read = d && fread(d, 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	if (!read) {
		ginLog("%s: unreadable", path);
		free(d);
		return false;
	}
	// BindToData: "Gnsu" version '2'. The files are the PC's, little-endian
	// (+6 = 0); the GC swaps them in place on load (AdjustEndienness 0x800AFB94,
	// which then sets +6 = 1). The 3DS reads them in their own order.
	const int32_t* w = (const int32_t*)d;
	if (memcmp(d, "Gnsu", 4) != 0 || d[4] != '2' || d[6] != 0 || d[7] != 0) {
		ginLog("%s: not a little-endian Gnsu2 file", path);
		free(d);
		return false;
	}
	const int32_t nf = w[4], nc = w[5], ns = w[6];
	// Port guard: tables and blocks inside the file (retail trusts its data),
	// and the 2 cycles CyclePeriod's first case reads.
	const long tab = 0x20 + 4L * (nf + 1) + 4L * (nc + 1);
	if (nf < 1 || nc < 2 || ns < 1 || w[7] <= 0 || nf > 0x10000 || nc > 0x100000 ||
	    tab + ((long)ns + 31) / 32 * 19 > n) {
		ginLog("%s: malformed (nf %d nc %d ns %d)", path, (int)nf, (int)nc, (int)ns);
		free(d);
		return false;
	}
	g->file = d;
	memcpy(&g->minFreq, &w[2], 4);
	memcpy(&g->maxFreq, &w[3], 4);
	g->nf = nf;
	g->nc = nc;
	g->ns = ns;
	g->rate = w[7];
	g->freq = w + 8;
	g->cyc = w + 8 + nf + 1;
	g->data = d + tab;
	int32_t m = ns;
	for (int32_t i = 0; i < nc; i++)
		if (g->cyc[i + 1] - g->cyc[i] < m) m = g->cyc[i + 1] - g->cyc[i];
	g->minCycle = (float)m;
	return true;
}

void ginsuFree(GinsuData* g)
{
	free(g->file);
	memset(g, 0, sizeof(*g));
	g->blockIdx = -1;
}

// ---------------------------------------------------------------- synthesis

// GetMemblockSize (0x800B106C) asks for 0x8A4 bytes past SNDPKTPLAY's own,
// which the constructor splits into two packet buffers of this many samples.
#define GINSU_CAPACITY 552
#define GINSU_XFADE_MAX 64

// HandlePacketRelease (0x800B0908), from the DSP's frame callback once a
// packet has played: the next packet into `out`, its length returned.
//
// The packet carries on from `pos`. If, by the packet's end, the cycle playing
// would be a whole cycle or more off the one the RPM wants (gliding from s30
// to s34), it plays only up to where that happens, jumps the difference in
// cycles, crossfades 0.5 ms into the new place and fills the rest from there.
static int handlePacket(void* user, int16_t* out)
{
	GinsuSynth* s = user;
	LightLock_Lock(&s->lock);
	GinsuData* g = s->data;
	s->cyc = sampleToCycle(g, s->s30);
	const float step = (float)(s->s34 - s->s30) / (float)s->glide;
	int32_t jumps = 0;
	float frac = 0.0f;
	if (s->s3c < s->packetLen) {
		const float cycPos = sampleToCycle(g, s->pos);
		const float perPos = cyclePeriod(g, cycPos);   // the arg Ghidra lost
		const float perTgt = cyclePeriod(g, s->cyc);
		const float f5 = (float)s->s3c / (float)s->packetLen;
		const float f4 = step / perTgt - (float)s->packetLen / perPos;
		const float f2 = s->cyc - cycPos;
		const float f6 = f4 * f5 + f2;
		const float f7 = f2 + f4;
		if (floorf(f6) == floorf(f7)) {
			const int32_t thr = ceilCount(fabsf(f4 * 0.011f / 0.011f));
			if (fabsf(f6) > (float)thr) {
				frac = f5;
				jumps = (int32_t)f6;
			}
		} else {
			jumps = f6 < f7 ? (int32_t)ceilf(f6) : (int32_t)floorf(f6);
			frac = ((float)jumps - f2) / f4;
		}
	}
	if (jumps == 0) frac = 1.0f;
	int32_t n = rnd(frac * (float)s->packetLen);
	getSamples(g, s->pos, n, out);
	s->pos += n;
	s->s3c -= n;
	if (s->s3c < 0) s->s3c = 0;
	if (jumps != 0) {
		int16_t* x = out + n;
		int16_t to[GINSU_XFADE_MAX];
		getSamples(g, s->pos, s->xfade, x);
		s->pos = cycleToSample(g, sampleToCycle(g, s->pos) + (float)jumps);
		getSamples(g, s->pos, s->xfade, to);
		const float da = 1.0f / (float)s->xfade;
		float a = 0.0f;
		for (int32_t i = 0; i < s->xfade; i++) {
			x[i] = (int16_t)rnd(a * (float)(to[i] - x[i]) + (float)x[i]);
			a += da;
		}
		n += s->xfade;
		s->pos += s->xfade;
		s->s3c = s->jumpReset - s->xfade;
		const int32_t rem = s->packetLen - n;
		if (rem > 0) {
			getSamples(g, s->pos, rem, out + n);
			n = s->packetLen;
			s->pos += rem;
			s->s3c -= rem;
		}
	}
	s->s30 = rnd((float)s->s30 + step);
	if (s->glide > 1) s->glide--;
	for (int i = 0; i < n; i++) {
		const int32_t a = out[i] < 0 ? -out[i] : out[i];
		if (a > s->peak) s->peak = a;
	}
	LightLock_Unlock(&s->lock);
	return n;
}

void ginsuSynthInit(GinsuSynth* s, int voice)
{
	memset(s, 0, sizeof(*s));
	LightLock_Init(&s->lock);
	s->voice = voice;
	s->playing = -1;
	s->capacity = GINSU_CAPACITY;
}

bool ginsuSetSynthData(GinsuSynth* s, GinsuData* d)
{
	const int32_t rate = d->rate;
	if (rate == 0 || (s->rate >= 1 && rate != s->rate)) return false;
	const int32_t jump = rnd((float)rate * 0.011f);
	const int32_t pkt = rnd((float)rate * 0.011f);
	const int32_t xf = rnd((float)rate * 0.0005f);
	// Port guard: the crossfade's scratch (retail's sits in a large stack frame).
	if (pkt + xf > s->capacity || xf < 1 || xf > GINSU_XFADE_MAX) return false;
	LightLock_Lock(&s->lock);
	s->data = d;
	s->jumpReset = jump;
	s->packetLen = pkt;
	s->xfade = xf;
	LightLock_Unlock(&s->lock);
	return true;
}

void ginsuStop(GinsuSynth* s)
{
	if (s->rate != 0) {
		audioPacketStop(s->voice);
		s->rate = 0;
	}
}

int ginsuStart(GinsuSynth* s, float freq)
{
	if (s->voice < 0 || !s->data) return -1;
	ginsuStop(s);
	const int32_t p = freqToSample(s->data, freq);
	LightLock_Lock(&s->lock);
	s->glide = 1;
	s->s3c = s->jumpReset;
	s->pos = s->s30 = s->s34 = p;
	s->cyc = sampleToCycle(s->data, p);
	LightLock_Unlock(&s->lock);
	// SNDPKTPLAY_start, then two silent packets (the port keeps three in
	// flight -- see audioPacketStart).
	s->playing = audioPacketStart(s->voice, (uint32_t)s->data->rate, s->capacity, s->packetLen,
	                              handlePacket, s) ? s->voice : -1;
	if (s->playing >= 0) s->rate = s->data->rate;
	return s->playing;
}

void ginsuUpdateFrequency(GinsuSynth* s, float freq, float ms)
{
	if (!s->data) return;
	const int32_t p = freqToSample(s->data, freq);
	LightLock_Lock(&s->lock);
	s->s34 = p;
	s->glide = (int32_t)floorf(ms / 11.0f) + 1;
	LightLock_Unlock(&s->lock);
}

int32_t ginsuTakePeak(GinsuSynth* s)
{
	LightLock_Lock(&s->lock);
	const int32_t p = s->peak;
	s->peak = 0;
	LightLock_Unlock(&s->lock);
	return p;
}

float ginsuCurrentPitch(GinsuSynth* s)
{
	if (s->rate == 0) return 0.0f;
	LightLock_Lock(&s->lock);
	const float c = s->cyc;
	LightLock_Unlock(&s->lock);
	return (float)s->rate / cyclePeriod(s->data, c + 0.5f);
}
