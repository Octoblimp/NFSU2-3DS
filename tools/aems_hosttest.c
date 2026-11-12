// Host test for 3DS Files/source/aems.c (TASKS.md #298): runs a converted
// AEMS bank's graphs on the PC, with stand-in voices that log every call the
// player nodes make, so a graph can be checked without the 3DS.
//
// Build (devkitPro's msys2 gcc; any 32- or 64-bit gcc works):
//   gcc -O1 -DAEMS_HOST -I"3DS Files/source" -o aems_hosttest
//       tools/aems_hosttest.c "3DS Files/source/aems.c" -lm
// Run:
//   aems_hosttest <bank.aem> <class> <stop field> <field0> [field1 ...]
//
// Creates one instance of <class> with the given member data and feeds it the
// same data every 1/60 s frame, as CARSFX's ProcessUpdate does. After 2 s it
// sets <stop field> to 1 (the CARSFX stop flag), then runs until the class's
// reference count falls below 2 -- every module instance gone -- and releases
// it, again as ProcessUpdate does. Exit status 0 = the graph started at least
// one voice and wound itself down with no voice or instance left behind.
#include "aems.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define VOICES 16

typedef struct {
	bool on, paused;
	int gen;
	const AemsClip* clip;
	float pitch, gain, l, r, pos;
	float lastPitch, lastGain;
} Voice;

static Voice s_v[VOICES];
static int   s_gen;
static float s_now;
static int   s_plays;

static Voice* get(int h)
{
	if (h < 0) return NULL;
	int i = h & 0xff;
	if (i >= VOICES || !s_v[i].on || s_v[i].gen != (h >> 8)) return NULL;
	return &s_v[i];
}

int aemsVoicePlay(const AemsClip* c, int bus, float pitch, float gain, float l, float r)
{
	(void)bus;
	for (int i = 0; i < VOICES; i++) {
		if (s_v[i].on) continue;
		Voice* v = &s_v[i];
		*v = (Voice){ true, false, ++s_gen, c, pitch, gain, l, r, 0.0f, pitch, gain };
		s_plays++;
		printf("%6.2f  play  v%-2d %5u Hz %6u smp %s  pitch %.3f gain %.3f pan %.3f/%.3f\n",
		       s_now, i, (unsigned)c->rate, (unsigned)c->samples, c->loop ? "loop" : "once",
		       pitch, gain, l, r);
		return (v->gen << 8) | i;
	}
	printf("%6.2f  play  -- no voice free\n", s_now);
	return -1;
}

bool aemsVoiceAlive(int h) { return get(h) != NULL; }
const AemsClip* aemsVoiceClip(int h) { Voice* v = get(h); return v ? v->clip : NULL; }

// Ramps change pitch and gain every tick; log a step only when it has moved
// 5% from the last one logged.
void aemsVoiceSetPitch(int h, float p)
{
	Voice* v = get(h);
	if (!v) return;
	v->pitch = p;
	if (fabsf(p - v->lastPitch) > 0.05f * (v->lastPitch > 0.1f ? v->lastPitch : 0.1f)) {
		printf("%6.2f  pitch v%-2d %.3f\n", s_now, h & 0xff, p);
		v->lastPitch = p;
	}
}

void aemsVoiceSetGain(int h, float g)
{
	Voice* v = get(h);
	if (!v) return;
	v->gain = g;
	if (fabsf(g - v->lastGain) > 0.05f || (g == 0.0f && v->lastGain != 0.0f)) {
		printf("%6.2f  gain  v%-2d %.3f\n", s_now, h & 0xff, g);
		v->lastGain = g;
	}
}

void aemsVoiceSetPan(int h, float l, float r)
{
	Voice* v = get(h);
	if (!v) return;
	v->l = l; v->r = r;
	printf("%6.2f  pan   v%-2d %.3f/%.3f\n", s_now, h & 0xff, l, r);
}

void aemsVoicePause(int h, bool p)
{
	Voice* v = get(h);
	if (!v) return;
	v->paused = p;
	printf("%6.2f  %s v%d\n", s_now, p ? "pause" : "resume", h & 0xff);
}

void aemsVoiceStop(int h)
{
	Voice* v = get(h);
	if (!v) return;
	v->on = false;
	printf("%6.2f  stop  v%-2d at %u of %u\n", s_now, h & 0xff, (unsigned)v->pos,
	       (unsigned)v->clip->samples);
}

void aemsVoiceCutFades(void) {}

int aemsVoiceDropClips(const AemsClip* clips, uint32_t count)
{
	int cut = 0;
	for (int i = 0; i < VOICES; i++) {
		Voice* v = &s_v[i];
		if (!v->on || v->clip < clips || v->clip >= clips + count) continue;
		v->on = false;
		cut++;
		printf("%6.2f  LOST  v%-2d still on its bank at unload\n", s_now, i);
	}
	return cut;
}

uint32_t aemsVoiceSamplePos(int h) { Voice* v = get(h); return v ? (uint32_t)v->pos : 0; }

int aemsVoicesPlaying(void)
{
	int n = 0;
	for (int i = 0; i < VOICES; i++) n += s_v[i].on && !s_v[i].paused;
	return n;
}

static void advance(float dt)
{
	for (int i = 0; i < VOICES; i++) {
		Voice* v = &s_v[i];
		if (!v->on || v->paused) continue;
		v->pos += (float)v->clip->rate * v->pitch * dt;
		if (v->pos < (float)v->clip->samples) continue;
		if (v->clip->loop) {
			v->pos = fmodf(v->pos, (float)v->clip->samples);
		} else {
			v->on = false;
			printf("%6.2f  ends  v%d\n", s_now, i);
		}
	}
}

int main(int argc, char** argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s <bank.aem> <class> <stop field> <field0> [field1 ...]\n", argv[0]);
		return 2;
	}
	AemsBank* bank = aemsLoad(argv[1], 0);
	if (!bank) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
	int stopField = atoi(argv[3]);
	int32_t f[16] = { 0 };
	int nf = 0;
	for (int i = 4; i < argc && nf < 16; i++) f[nf++] = (int32_t)strtol(argv[i], NULL, 0);

	AemsInst inst = aemsCreate(aemsClass(argv[2]), f);
	printf("created %s: refcount %d, %d module instance(s)\n", argv[2], aemsRefCount(inst),
	       aemsLiveInstances());

	const float dt = 1.0f / 60.0f;
	bool stopped = false, released = false;
	for (int frame = 0; frame < 60 * 30; frame++) {
		s_now = frame * dt;
		// <stop field> -1: let go at 2 s with the voice still sounding, as
		// CARSFX_TrafficHorn::CSIS_EndHonk does, then run 5 s more.
		// (-1 lets go at 2 s; -N for N > 1, at N ms.)
		if (stopField < 0 && s_now >= (stopField == -1 ? 2.0f : -stopField / 1000.0f)) {
			aemsRelease(inst);
			released = true;
			printf("%6.2f  -- released while playing\n", s_now);
			for (int k = 0; k < 300; k++) {
				s_now += dt;
				aemsUpdate(dt);
				advance(dt);
			}
			break;
		}
		if (stopField >= 0 && !stopped && s_now >= 2.0f) {
			f[stopField] = 1;
			stopped = true;
			printf("%6.2f  -- stop flag set\n", s_now);
		}
		if (aemsRefCount(inst) < 2) {
			aemsRelease(inst);
			released = true;
			printf("%6.2f  -- refcount < 2: released\n", s_now);
			break;
		}
		aemsSetMemberData(inst, f);
		aemsUpdate(dt);
		advance(dt);
	}
	int on = 0;
	for (int i = 0; i < VOICES; i++) on += s_v[i].on;
	printf("result: %d play(s), released %s, %d module instance(s) left, %d voice(s) on\n",
	       s_plays, released ? "yes" : "NO", aemsLiveInstances(), on);
	aemsUnload(bank);
	return (s_plays > 0 && released && aemsLiveInstances() == 0 && on == 0) ? 0 : 1;
}
