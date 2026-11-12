// The sound controllers' envelopes. See sfxline.h.
#include "sfxline.h"

#include "mixmap.h"

#include <stdint.h>
#include <string.h>

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// The curves cInterpLine and cPathLine share (eCURVETYPE): 2 eases in, 3 eases
// out, 6 is NFSMixShape's table 3, and the rest (0, 4, 5) are straight. Curve
// 1 the callers handle: the value holds until the line ends.
float ease(int curve, float e, float d, float a, float b)
{
	switch (curve) {
	case 2:
		return (b - a) * (e / d) * (e / d) + a;
	case 3: {
		const float x = e / d - 1.0f;
		return (b - a) * (1.0f - x * x) + a;
	}
	case 6: {
		const int32_t i = clampi((int32_t)(e * 32767.0f / d), 0, 0x7fff);
		return (b - a) * ((float)mixmapCurveOut(3, i, false) / 32767.0f) + a;
	}
	default:
		return (b - a) * (e / d) + a;
	}
}

void lineIdle(InterpLine* l)
{
	memset(l, 0, sizeof(*l));
	l->done = 1;
}

void lineInit(InterpLine* l, float a, float b, int ms, int curve)
{
	l->dur = (float)ms / 1000.0f;
	if (l->dur <= 0.0f) l->dur = 0.01f;
	l->elapsed = 0.0f;
	l->end = b;
	l->curve = curve;
	l->done = 0;
	l->cur = a;
	l->start = a;
}

void lineUpdate(InterpLine* l, float dt)
{
	if (l->done) return;
	l->elapsed += dt;
	if (l->elapsed <= l->dur) {
		if (l->curve == 1) return;
		l->cur = ease(l->curve, l->elapsed, l->dur, l->start, l->end);
	} else {
		l->cur = l->end;
		l->done = 1;
	}
}

// Update(dt, target): the end follows `target`, and a finished line sits on it.
void lineTrack(InterpLine* l, float dt, float target)
{
	l->end = target;
	lineUpdate(l, dt);
	if (l->done) l->cur = target;
}

void pathClear(PathLine* p)
{
	memset(p, 0, sizeof(*p));
}

void pathAdd(PathLine* p, float a, float b, int ms, int curve)
{
	const int n = p->count;
	if (n == 5) return;
	p->dur[n] = (float)ms / 1000.0f;
	if (p->dur[n] <= 0.0f) p->dur[n] = 0.01f;
	p->end[n] = b;
	p->start[n] = a;
	p->curve[n] = curve;
	p->link[n] = 0;
	p->done = 0;
	if (n == 0) p->cur = p->start[0];
	p->count = n + 1;
}

void pathInit(PathLine* p, float a, float b, int ms)
{
	pathClear(p);
	pathAdd(p, a, b, ms, 0);
	p->cur = a;
}

void pathUpdate(PathLine* p, float dt)
{
	if (p->done || p->count == 0) return;
	const int s = p->stage;
	p->elapsed += dt;
	if (p->dur[s] < p->elapsed) {
		p->cur = p->end[s];
		if (s >= p->count - 1) {
			p->done = 1;
			return;
		}
		p->stage = s + 1;
		p->elapsed -= p->dur[s];
		if (p->link[s + 1]) p->start[s + 1] = p->end[s];
		return;
	}
	if (p->curve[s] == 1) return;
	p->cur = ease(p->curve[s], p->elapsed, p->dur[s], p->start[s], p->end[s]);
}

void avgInit(Average* a, int size)
{
	memset(a, 0, sizeof(*a));
	a->size = size < 1 ? 1 : (size > AVERAGE_MAX ? AVERAGE_MAX : size);   // port guard
}

void avgRecord(Average* a, float v)
{
	if (a->count < a->size) a->count++;
	a->sum += v;
	a->sum -= a->data[a->idx];
	a->data[a->idx] = v;
	a->idx++;
	a->avg = a->sum / (float)a->count;
	if (a->idx >= a->size) a->idx = 0;
}

void avgFlush(Average* a, float v)
{
	for (int i = 0; i < a->size; i++) a->data[i] = v;
	a->avg = v;
	a->count = a->size;
	a->sum = v * (float)a->size;
}

void avgRecalc(Average* a)
{
	a->sum = 0.0f;
	for (int i = 0; i < a->size; i++) a->sum += a->data[i];
	a->avg = a->sum * (1.0f / (float)(a->count ? a->count : 1));
}
