// Retail's FuturePoints racing line (TASKS.md #549); futurepoints.h has the
// overview, tools/re/wip549fp/SPEC.md the decode.
#include "futurepoints.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aidriver.h"
#include "trackroute_int.h"

#define FP_MAX       40     // FuturePoints holds 0x28
#define FP_CORNERS   5      // InterpolatedCornerInfo slots
#define FP_FILL_MIN  10     // FillUpArray: count > 9
#define FP_FILL_STEPS 100
#define FP_DT_MAX    20     // FillUpDriveTarget's 20 points

#define FP_REDUCE    0.04f  // ReduceFixup per metre
// PORT: across one of the race's hops (fp_set_race) the line carries on from
// where it was and eases onto the new route's at this rate, as the AiLine's
// own ramps do, instead of jumping sideways onto the new route's line.
#define FP_HOP_RATE  0.15f  // 0.4 jinked the line 22 degrees at each hop (4045B 1,260 m)
#define FP_SIGN_SKIP 3.0f   // 0x80318530
#define FP_FAR_ENOUGH 5.0f  // 0x80318558 (10 in drag: PORT, racers only)
#define FP_INTERP    8.0f   // 0x80318564, 1 / 0.125
#define FP_CORNER_AGAIN 30.0f // 0x803184FC
#define FP_APEX      3.536f // 2.5 * sqrt 2
#define FP_APEX_LANE 1.2f
#define FP_PAD       3.0f
#define FP_CORNER_RUN 75.0f // InterpLinesForCorner's -75 / +75
#define FP_CORNER_SPEED 60.0f
#define FP_NO_CORNER (-99999.0f)
// KeepFuturePointsBehindMe keeps 20 (10 in race types 0 / 7) behind the car.
// PORT: 4 -- the port fills to the 40 from the car on, not from 20 back.
#define FP_KEEP_BEHIND 4

typedef struct {
	float fixup, minL, maxL, speed;
	float D, p54;          // +0x50, +0x54: along from point 0, and along its route point
	int route, point;      // route index into the set, point within it
	int sign;              // the Signpost index that led here, -1 for none
	int hop;               // PORT: the race's next hop from here (fp_set_race)
	bool fwd, interp;
} FP;

typedef struct {
	float pos[5][2];       // far before, near before, apex, near after, far after
	float D[5];
	float cornerD;
	int idx;               // the FP before the corner's own
	int segS[4], segE[4];
	bool inside;           // InterpolatedCornerInfo's flag: the apex on the min side
} FPCorner;

typedef struct {
	float x, y, D, L;
} FPTarget;

struct FuturePoints {
	const TrackRouteSet* s;
	FPForkFn fork;
	void* forkCtx;
	FP fp[FP_MAX];
	int n;
	FPCorner c[FP_CORNERS];
	int nc;
	float lastCornerD;
	u8* decided;           // per Signpost: 0 not yet, 1 no, 2 yes
	bool seeded;
	int seedRoute, seedPoint;
	bool seedFwd;
	FPTarget dt[FP_DT_MAX];
	int ndt;
	const TrackRouteHop* hops;
	int nHops;
	bool hopLoop;
	int hopCur;            // the walk's head: the next hop it is to make
	int seedHop;
	bool hopEase;          // the walk's last point is still easing off a hop
	const u8* onRace;
	const FPCar* car;
	float carZ;            // the car's height at the last fp_update, for the 2D queries
};

// ---------------------------------------------------------------------------
// The route point under a FuturePoint, and its frame.

static const TRPoint* rpAt(const FuturePoints* f, int route, int point) {
	return &f->s->pts[f->s->routes[route].first + point];
}
static const TRPoint* rpOf(const FuturePoints* f, const FP* p) {
	return rpAt(f, p->route, p->point);
}

// Forwards: the route point's own direction. Backwards: the one before it,
// turned round (FuturePointBackwards; -own at the route's first point).
static void dirAt(const FuturePoints* f, int route, int point, bool fwd, float d[2]) {
	const TRPoint* q = rpAt(f, route, point);
	if (fwd) {
		d[0] = q->fwd[0];
		d[1] = q->fwd[1];
		return;
	}
	const TRPoint* b = point > 0 ? rpAt(f, route, point - 1) : q;
	d[0] = -b->fwd[0];
	d[1] = -b->fwd[1];
}
static void dirOf(const FuturePoints* f, const FP* p, float d[2]) {
	dirAt(f, p->route, p->point, p->fwd, d);
}

static float lineOff(const FuturePoints* f, const FP* p) {
	const TRPoint* q = rpOf(f, p);
	return p->fwd ? q->off[0] : q->off[1];
}
static float lineL(const FuturePoints* f, const FP* p) { return lineOff(f, p) + p->fixup; }
// The road's half widths in the FP's own frame (+ right).
static float halfRight(const FuturePoints* f, const FP* p) {
	const TRPoint* q = rpOf(f, p);
	return p->fwd ? q->halfR : q->halfL;
}
static float halfLeft(const FuturePoints* f, const FP* p) {
	const TRPoint* q = rpOf(f, p);
	return p->fwd ? q->halfL : q->halfR;
}

// The road's height at D: the route point's, on up its slope (forwards), or
// down the one before's (backwards).
static float fpZ(const FuturePoints* f, const FP* p, float D) {
	const TRPoint* q = rpOf(f, p);
	const float a = D - p->D + p->p54;
	if (p->fwd) return q->z + a * q->slope;
	const TRPoint* b = p->point > 0 ? rpAt(f, p->route, p->point - 1) : q;
	return q->z - a * b->slope;
}

// GetPosition: along D - fp.D + p54 from the route point, L right of the line.
static void fpPos(const FuturePoints* f, const FP* p, float D, float L, float out[2]) {
	const TRPoint* q = rpOf(f, p);
	float d[2];
	dirOf(f, p, d);
	const float a = D - p->D + p->p54, l = L + lineL(f, p);
	out[0] = q->x + a * d[0] + l * d[1];
	out[1] = q->y + a * d[1] - l * d[0];
}
static float fpGetD(const FuturePoints* f, const FP* p, const float pos[2]) {
	const TRPoint* q = rpOf(f, p);
	float d[2];
	dirOf(f, p, d);
	return d[0] * (pos[0] - q->x) + d[1] * (pos[1] - q->y) - p->p54 + p->D;
}
static float fpGetL(const FuturePoints* f, const FP* p, const float pos[2]) {
	const TRPoint* q = rpOf(f, p);
	float d[2];
	dirOf(f, p, d);
	return (pos[0] - q->x) * d[1] - (pos[1] - q->y) * d[0] - lineL(f, p);
}
// A route point's own position along an FP's frame, L 0 off its line
// (the line's own point at the FP's D).
static void fpLinePos(const FuturePoints* f, const FP* p, float out[2]) {
	fpPos(f, p, p->D, 0.0f, out);
}
// FuturePoint::GetLength: to the next FP, or the rest of the route point.
static float fpLength(const FuturePoints* f, int i) {
	const FP* p = &f->fp[i];
	if (i + 1 < f->n) return f->fp[i + 1].D - p->D;
	float len;
	if (p->fwd) len = rpOf(f, p)->len;
	else len = p->point > 0 ? rpAt(f, p->route, p->point - 1)->len : 0.0f;
	return len - p->p54;
}

static void minMax(const FuturePoints* f, FP* p) {
	const float l = lineL(f, p);
	p->maxL = halfRight(f, p) - l;
	p->minL = -halfLeft(f, p) - l;
}
// ClampFixups (0x803187EC: 1 m inside either edge).
static void clampFixup(const FuturePoints* f, FP* p) {
	const float o = lineOff(f, p);
	const float lo = -halfLeft(f, p) - o + 1.0f, hi = halfRight(f, p) - o - 1.0f;
	if (lo > hi) p->fixup = 0.5f * (lo + hi);
	else if (p->fixup < lo) p->fixup = lo;
	else if (p->fixup > hi) p->fixup = hi;
}

static void fpInit(FP* p, int route, int point, bool fwd) {
	memset(p, 0, sizeof(*p));
	p->route = route;
	p->point = point;
	p->fwd = fwd;
	p->sign = -1;
	p->speed = -1.0f;
}

// ---------------------------------------------------------------------------
// Lifetime.

FuturePoints* fp_new(const TrackRouteSet* s, FPForkFn fork, void* forkCtx) {
	if (!s || s->nRoutes < 1) return NULL;
	FuturePoints* f = (FuturePoints*)calloc(1, sizeof(FuturePoints));
	if (!f) return NULL;
	f->s = s;
	f->fork = fork;
	f->forkCtx = forkCtx;
	if (s->nSigns > 0) f->decided = (u8*)calloc((size_t)s->nSigns, 1);
	f->lastCornerD = FP_NO_CORNER;
	return f;
}

void fp_set_race(FuturePoints* f, const TrackRouteHop* hops, int nHops, const u8* onRace,
		bool loop) {
	if (!f) return;
	f->hopLoop = loop;
	f->hops = hops;
	f->nHops = hops ? nHops : 0;
	f->onRace = onRace;
}

void fp_free(FuturePoints* f) {
	if (!f) return;
	free(f->decided);
	free(f);
}

void fp_clear(FuturePoints* f) {
	if (!f) return;
	f->n = 0;
	f->nc = 0;
	f->ndt = 0;
	f->seeded = false;
	f->lastCornerD = FP_NO_CORNER;
	if (f->decided) memset(f->decided, 0, (size_t)f->s->nSigns);
}

int fp_count(const FuturePoints* f) { return f ? f->n : 0; }

bool fp_seed(FuturePoints* f, const float pos[3], const float heading[2], int preferRoute,
		int nextHop) {
	if (!f) return false;
	f->seedHop = nextHop;
	const TrackRouteSet* s = f->s;
	float best = 40.0f * 40.0f;
	bool got = false;
	for (int r = 0; r < s->nRoutes; r++) {
		const TRRoute* rt = &s->routes[r];
		if (rt->count < 2) continue;
		for (int j = 0; j < rt->count; j++) {
			const TRPoint* q = &s->pts[rt->first + j];
			const float dx = pos[0] - q->x, dy = pos[1] - q->y, dz = q->z - pos[2];
			if (fabsf(dz) > 6.0f) continue;
			for (int w = 0; w < 2; w++) {
				const bool fwd = w == 0;
				if (!(fwd ? q->postF : q->postB)) continue;
				if (fwd && j + 1 >= rt->count) continue;
				if (!fwd && j < 1) continue;
				float d[2];
				dirAt(f, r, j, fwd, d);
				if (d[0] * heading[0] + d[1] * heading[1] < 0.5f) continue;
				// The point the car has just passed: it is along the point's
				// own run, scored by how far off to the side.
				const float len = fwd ? q->len : s->pts[rt->first + j - 1].len;
				const float along = dx * d[0] + dy * d[1];
				if (along < -2.0f || along > len + 2.0f) continue;
				const float side = dx * d[1] - dy * d[0];
				float d2 = side * side + dz * dz * 4.0f;
				if (r == preferRoute) d2 *= 0.05f;
				if (d2 >= best) continue;
				best = d2;
				f->seedRoute = r;
				f->seedPoint = j;
				f->seedFwd = fwd;
				got = true;
			}
		}
	}
	f->n = 0;
	f->nc = 0;
	f->ndt = 0;
	f->lastCornerD = FP_NO_CORNER;
	f->seeded = got;
	return got;
}

// ---------------------------------------------------------------------------
// The walk.

// SignPostDecision (DecideSignpostAsRacer 0x8000A914), kept per sign.
static bool signDecision(FuturePoints* f, int si) {
	if (f->decided && f->decided[si]) return f->decided[si] == 2;
	const TRSign* g = &f->s->signs[si];
	const u32 type = g->bits & 15u;
	bool take = false;
	if (type == 3u) take = false;
	else if (type == 2u) take = true;
	else {
		const int nr = trackroute_route_by_num(f->s, g->newRn);
		if (nr < 0 || (f->s->routes[nr].rbits >> 10) & 1u) take = false;
		else if (type == 5u || type == 10u || type == 7u || (g->bits >> 20) & 1u) take = false;
		else if (type == 4u) {
			const float p = (g->bits >> 27) & 1u ? 0.333f : 0.5f;
			take = f->fork ? f->fork(f->forkCtx, si, g->newRn, p) : false;
		}
	}
	if (f->decided) f->decided[si] = take ? 2 : 1;
	return take;
}

#define FP_HOP (-2)

// The race's hops are made in order (PORT): the walk's next, or the one after
// it where that one's point was not walked over. -1 past the last.
static int hopIndex(const FuturePoints* f, int k) {
	const int h = f->hopCur + k;
	if (h < f->nHops) return h;
	return f->hopLoop && f->nHops > 0 ? h % f->nHops : -1;
}

// Whether the race's next hop leaves `route` (driven `fwd`) for `to`: its index, or -1.
static int hopTo(const FuturePoints* f, int route, bool fwd, int to) {
	for (int k = 0; k < 2; k++) {
		const int h = hopIndex(f, k);
		if (h < 0) break;
		if (f->hops[h].fromRoute == route && f->hops[h].fromFwd == fwd && f->hops[h].toRoute == to)
			return h;
	}
	return -1;
}   // advance's sign for one of the race's hops

// AdvancePoint: across a sign the racer takes, else one point on. 0 when the
// walk ends; *sign the sign taken (-1 for none, FP_HOP for a race hop).
static bool advance(FuturePoints* f, int* route, int* point, bool* fwd, int* sign) {
	const TrackRouteSet* s = f->s;
	const TRPoint* q = rpAt(f, *route, *point);
	*sign = -1;
	for (int k = 0; k < 2; k++) {
		const int si = q->sign[k];
		if (si == 0xFFFF || si >= s->nSigns) continue;
		const TRSign* g = &s->signs[si];
		const u32 type = g->bits & 15u;
		const bool side = (g->bits >> 18) & 1u;
		const bool corner = (g->bits >> 26) & 1u;
		if (side == *fwd) continue;
		if (type == 3u) return false;
		bool take;
		if (type == 2u) take = true;
		else {
			const FP* last = f->n > 0 ? &f->fp[f->n - 1] : NULL;
			take = false;
			if (!corner || (last && last->D - f->lastCornerD > FP_CORNER_AGAIN))
				take = signDecision(f, si);
		}
		if (!take) continue;
		const int nr = trackroute_route_by_num(s, g->newRn);
		if (nr < 0 || g->newPt < 0 || g->newPt >= s->routes[nr].count) continue;
		// PORT: on the race's routes, the race's own changes are its hops; a
		// sign is taken there only as one of its forks (fp_set_race), or onto
		// the route a hop from this one takes, so a corner sign the race
		// turns at keeps its corner (InterpolateFuturePointsForCorner).
		const int viaHop = f->onRace && f->onRace[*route] ? hopTo(f, *route, *fwd, nr) : -1;
		if (f->onRace && f->onRace[*route] && type != 4u && viaHop < 0) continue;
		if (viaHop >= 0) f->hopCur = viaHop + 1;
		*route = nr;
		*point = g->newPt;
		if ((g->bits >> 23) & 1u) *fwd = !*fwd;
		*sign = si;
		if (corner) f->lastCornerD = f->n > 0 ? f->fp[f->n - 1].D : 0.0f;
		return true;
	}
	// PORT: the race's hops where no sign is taken.
	for (int k = 0; k < 2; k++) {
		const int h = hopIndex(f, k);
		if (h < 0) break;
		const TrackRouteHop* hp = &f->hops[h];
		if (hp->fromRoute != *route || hp->fromPoint != *point || hp->fromFwd != *fwd) continue;
		if (hp->toRoute < 0 || hp->toPoint < 0 || hp->toPoint >= s->routes[hp->toRoute].count)
			break;
		f->hopCur = h + 1;
		*route = hp->toRoute;
		*point = hp->toPoint;
		*fwd = hp->toFwd;
		*sign = FP_HOP;
		return true;
	}
	// IncrementPointForwards / Backwards.
	if (*fwd) {
		if (*point + 1 >= s->routes[*route].count) return false;
		(*point)++;
	} else {
		if (*point < 1) return false;
		(*point)--;
	}
	return true;
}

static void addPoint(FuturePoints* f, const FP* p) {
	if (f->n < FP_MAX) f->fp[f->n++] = *p;
}

// AddInterpolatedPoints: copies of the last every 8 m up to the new one.
static void addInterpolated(FuturePoints* f, const FP* nw) {
	if (f->n < 1) return;
	const FP last = f->fp[f->n - 1];
	const int k = (int)((nw->D - last.D) * 0.125f) - 1;
	for (int i = 1; i <= k && f->n < FP_MAX - 1; i++) {
		FP p = last;
		p.D = last.D + FP_INTERP * (float)i;
		p.p54 = last.p54 + FP_INTERP * (float)i;
		p.fixup = 0.0f;
		p.interp = true;
		p.sign = -1;
		p.speed = -1.0f;
		addPoint(f, &p);
	}
}

// FarEnoughFromPrevPoint.
static bool farEnough(const FuturePoints* f, const FP* last, const FP* nw) {
	const TRPoint* q = rpOf(f, nw);
	if (q->sign[0] != 0xFFFF || q->tInt != 0x3F || nw->route == last->route) return true;
	float p[2];
	fpPos(f, nw, nw->D, 0.0f, p);
	return fpGetD(f, last, p) - last->D > FP_FAR_ENOUGH;
}

// The lane-0 road edges at an FP's own D (min side, max side).
static void edges(const FuturePoints* f, const FP* p, float mn[2], float mx[2]) {
	const TRPoint* q = rpOf(f, p);
	float d[2];
	dirOf(f, p, d);
	const float a = p->p54, l0 = -halfLeft(f, p), l1 = halfRight(f, p);
	mn[0] = q->x + a * d[0] + l0 * d[1];
	mn[1] = q->y + a * d[1] - l0 * d[0];
	mx[0] = q->x + a * d[0] + l1 * d[1];
	mx[1] = q->y + a * d[1] - l1 * d[0];
}

// InterpolateFuturePointsForCorner.
static void cornerPoints(FuturePoints* f, FP nw) {
	if (f->n < 1) {
		addPoint(f, &nw);
		return;
	}
	float c8[2], c0[2], b8[2], b0[2];
	{
		const FP* last = &f->fp[f->n - 1];
		edges(f, last, c8, c0);
		edges(f, &nw, b8, b0);
	}
	const FP* last = &f->fp[f->n - 1];
	const float d21 = fpGetD(f, &nw, c8), d22 = fpGetD(f, &nw, c0);
	const float e8 = fpGetD(f, last, b8), e0 = fpGetD(f, last, b0);
	const float d24 = e8 < e0 ? e8 : e0;
	// The cut: no more than 2.5 m each way, less where a lane runs nearer the edge.
	float inset = FP_APEX;
	{
		const TRPoint* q = rpOf(f, last);
		const float hr = halfRight(f, last), hl = halfLeft(f, last);
		for (int l = 0; l < 4; l++) {
			const float lo = last->fwd ? q->lane[l] : q->lane[4 + l];
			const float L = d21 <= d22 ? hr - lo : -hl - lo;
			const float v = sqrtf(L * L * 0.5f) - FP_APEX_LANE;
			if (v < inset) inset = v;
		}
	}
	const float apexD = d24 + inset;
	while (f->n > 0 && f->fp[f->n - 1].D > apexD - FP_PAD) f->n--;
	while (f->nc > 0 && f->c[f->nc - 1].idx + 1 >= f->n) f->nc--;
	if (f->n < 1) {
		addPoint(f, &nw);
		return;
	}
	while (f->fp[f->n - 1].D < apexD - 2.0f * FP_PAD && f->n < FP_MAX - 1) {
		FP p = f->fp[f->n - 1];
		p.D += FP_PAD;
		p.p54 += FP_PAD;
		p.interp = false;
		p.sign = -1;
		p.speed = -1.0f;
		addPoint(f, &p);
	}
	FP* lp = &f->fp[f->n - 1];
	minMax(f, lp);
	const bool inside = d22 >= d21;
	float apex[2];
	fpPos(f, lp, d24 + FP_APEX, inside ? lp->maxL - FP_APEX : lp->minL + FP_APEX, apex);
	// The corner's own point sits 3 m past the apex, on whichever route point
	// of the new route that is.
	float d[2];
	{
		const TRPoint* q = rpOf(f, &nw);
		dirOf(f, &nw, d);
		float ref = d[0] * (apex[0] - q->x) + d[1] * (apex[1] - q->y) + FP_PAD;
		for (int guard = 0; guard < 64; guard++) {
			float len;
			if (nw.fwd) len = rpOf(f, &nw)->len;
			else len = nw.point > 0 ? rpAt(f, nw.route, nw.point - 1)->len : 0.0f;
			if (!(len < ref)) break;
			int r = nw.route, pt = nw.point, sg;
			bool fw = nw.fwd;
			if (!advance(f, &r, &pt, &fw, &sg)) break;
			const int keepSign = nw.sign;
			fpInit(&nw, r, pt, fw);
			nw.hop = f->hopCur;
			nw.sign = sg >= 0 ? sg : keepSign;
			q = rpOf(f, &nw);
			dirOf(f, &nw, d);
			ref = d[0] * (apex[0] - q->x) + d[1] * (apex[1] - q->y) + FP_PAD;
		}
		nw.p54 = ref;
	}
	nw.D = apexD + FP_PAD;
	nw.interp = false;
	addPoint(f, &nw);
	if (f->nc < FP_CORNERS && f->n >= 2) {
		FPCorner* c = &f->c[f->nc++];
		memset(c, 0, sizeof(*c));
		c->idx = f->n - 2;
		c->cornerD = d24;
		c->inside = d22 < d21;
		for (int j = 0; j < 5; j++) c->D[j] = -1.0f;
		c->D[2] = d24 + FP_APEX;
		c->pos[2][0] = apex[0];
		c->pos[2][1] = apex[1];
		for (int j = 0; j < 4; j++) c->segS[j] = c->segE[j] = -1;
	}
}

// ReduceFixup: the fixup eases back onto the route's line, 4 cm a metre.
static float reduceFixup(float fx, float gap) {
	const float k = gap * FP_REDUCE;
	if (fx > 0.0f) return fx > k ? fx - k : 0.0f;
	return -fx > k ? fx + k : 0.0f;
}

// FillUpArray.
static void fillUp(FuturePoints* f, float maxD) {
	if (f->n < 1) return;
	const FP* last = &f->fp[f->n - 1];
	int route = last->route, point = last->point;
	bool fwd = last->fwd;
	float fixup = last->fixup;
	float D = last->D, cornerResetD = 0.0f;
	f->hopCur = last->hop;
	// Easing off a hop's carried fixup (FP_HOP_RATE) until it is gone.
	bool hopEase = last->fixup != 0.0f && f->hopEase;
	for (int step = 0; step < FP_FILL_STEPS; step++) {
		if ((D > maxD && f->n >= FP_FILL_MIN && D >= cornerResetD) || f->n >= FP_MAX) return;
		const int oRoute = route, oPoint = point;
		const bool oFwd = fwd;
		int sign;
		if (!advance(f, &route, &point, &fwd, &sign)) return;
		last = &f->fp[f->n - 1];
		const TRPoint* nq = rpAt(f, route, point);
		const float npos[2] = { nq->x, nq->y };
		float gap;
		bool corner = false;
		if (sign < 0) {
			gap = fpGetD(f, last, npos) - last->D;
			const TRRoute* rt = &f->s->routes[route];
			const int l = fwd ? 0 : 1;
			if (hopEase) {
				fixup = reduceFixup(fixup, gap * (FP_HOP_RATE / FP_REDUCE));
				hopEase = fixup != 0.0f;
			} else if (rt->lineHas[l][0] || rt->lineHas[l][1])
				fixup = reduceFixup(fixup, gap);
		} else if (sign == FP_HOP || !((f->s->signs[sign].bits >> 26) & 1u)) {
			const TRPoint* oq = rpAt(f, oRoute, oPoint);
			float od[2];
			dirAt(f, oRoute, oPoint, oFwd, od);
			gap = (nq->x - oq->x) * od[0] + (nq->y - oq->y) * od[1];
			bool lost = false;
			for (int k = 0; gap < FP_SIGN_SKIP && k < 64; k++) {
				if (fwd) {
					if (point + 1 >= f->s->routes[route].count) { lost = true; break; }
					point++;
				} else {
					if (point < 1) { lost = true; break; }
					point--;
				}
				nq = rpAt(f, route, point);
				gap = (nq->x - oq->x) * od[0] + (nq->y - oq->y) * od[1];
			}
			if (lost) return;
		} else {
			const TRPoint* oq = rpAt(f, oRoute, oPoint);
			gap = sqrtf((nq->x - oq->x) * (nq->x - oq->x) + (nq->y - oq->y) * (nq->y - oq->y));
			fixup = 0.0f;
			corner = true;
		}
		FP nw;
		fpInit(&nw, route, point, fwd);
		nw.hop = f->hopCur;
		nw.sign = sign >= 0 ? sign : -1;
		// Between signs the gap is from the last FP kept (FarEnough may drop one).
		nw.D = (sign < 0 ? last->D : D) + gap;
		nw.fixup = fixup;
		if (sign == FP_HOP) {
			// Where the line was, across the new route.
			float lp[2];
			fpLinePos(f, last, lp);
			nw.fixup = 0.0f;
			nw.fixup = fpGetL(f, &nw, lp);
			hopEase = true;
		}
		clampFixup(f, &nw);
		fixup = nw.fixup;
		if (!corner) {
			if (farEnough(f, last, &nw)) {
				addInterpolated(f, &nw);
				if (f->n >= FP_MAX) return;
				addPoint(f, &nw);
				D = nw.D;
			}
			f->hopEase = hopEase;
		} else {
			hopEase = false;
			cornerPoints(f, nw);
			last = &f->fp[f->n - 1];
			route = last->route;
			point = last->point;
			fwd = last->fwd;
			fixup = last->fixup;
			D = cornerResetD = last->D + gap;
			// PORT: the walk carries on from the corner's own point, whose
			// along is past its route point's start; D tracks the point.
			D = last->D;
		}
	}
}

// ---------------------------------------------------------------------------
// Keeping the array.

// ShiftLeft: drops the first k points and counts D from the new first.
static void shiftLeft(FuturePoints* f, int k) {
	if (k <= 0) return;
	if (k > f->n) k = f->n;
	for (int i = 0; i < k; i++) {
		const int si = f->fp[i].sign;
		if (si >= 0 && f->decided) f->decided[si] = 0;
	}
	memmove(f->fp, f->fp + k, sizeof(FP) * (size_t)(f->n - k));
	f->n -= k;
	const float d0 = f->n > 0 ? f->fp[0].D : 0.0f;
	for (int i = 0; i < f->n; i++) f->fp[i].D -= d0;
	if (f->lastCornerD > FP_NO_CORNER) {
		f->lastCornerD -= d0;
		if (f->lastCornerD < -9999.0f) f->lastCornerD = FP_NO_CORNER;
	}
	int m = 0;
	for (int c = 0; c < f->nc; c++) {
		FPCorner cc = f->c[c];
		cc.idx -= k;
		if (cc.idx < 0) continue;
		cc.cornerD -= d0;
		for (int j = 0; j < 5; j++)
			if (cc.D[j] != -1.0f) cc.D[j] -= d0;
		for (int j = 0; j < 4; j++) {
			cc.segS[j] = cc.segS[j] < 0 ? -1 : (cc.segS[j] - k < 0 ? 0 : cc.segS[j] - k);
			cc.segE[j] = cc.segE[j] < 0 ? -1 : (cc.segE[j] - k < 0 ? 0 : cc.segE[j] - k);
		}
		f->c[m++] = cc;
	}
	f->nc = m;
	for (int i = 0; i < f->ndt; i++) f->dt[i].D -= d0;
}

// RemoveInterpolatedPointsFromFront: the tail past the last plain point goes
// (a corner's own point and its padding are rebuilt).
static void removeTail(FuturePoints* f) {
	const int n0 = f->n;
	while (f->n > 1 && fabsf(f->fp[f->n - 1].p54) > 0.001f) f->n--;
	while (f->nc > 0 && f->c[f->nc - 1].idx + 1 >= f->n) f->nc--;
	// PORT: the corner sign that built the tail is walked again from the
	// point before it; the D it set must not hold that sign off for 30 m.
	if (f->n < n0 && f->n > 0 && f->lastCornerD >= f->fp[f->n - 1].D - 0.001f)
		f->lastCornerD = FP_NO_CORNER;
}

// GetClosestPoint: by distance off the FP's line point, plus ten times the
// height difference. flags 1: on to the first point `thresh` ahead of pos.
static int closest(const FuturePoints* f, const float pos[3], int flags, float thresh, int start) {
	if (f->n < 1) return -1;
	if (start < 0) start = 0;
	int best = -1;
	float bs = 1e30f;
	for (int i = start; i < f->n; i++) {
		const FP* p = &f->fp[i];
		float lp[2];
		fpLinePos(f, p, lp);
		const float dx = lp[0] - pos[0], dy = lp[1] - pos[1];
		float d = sqrtf(dx * dx + dy * dy);
		if (d > 99999.0f) d = 99999.0f;
		const float sc = d + fabsf(fpZ(f, p, p->D) - pos[2]) * 10.0f;
		if (sc < bs) {
			bs = sc;
			best = i;
		}
	}
	if (best < 0) return -1;
	if (flags & 1) {
		while (best < f->n && f->fp[best].D - fpGetD(f, &f->fp[best], pos) < thresh) best++;
		if (best >= f->n) return -1;
	}
	if (flags & 2) {
		while (best >= 0 && f->fp[best].D - fpGetD(f, &f->fp[best], pos) > thresh) best--;
		if (best < 0) return -1;
	}
	return best;
}

// ---------------------------------------------------------------------------
// Speeds.

static float accelOf(const FPCar* car, float v) {
	if (!car || !(car->accelTop > 0.0f)) return 4.0f;   // aidriver.c's AISPEED_ACCEL_NONE
	float x = v / car->accelTop * (float)(CARSIM_ACCEL_N - 1);
	if (x <= 0.0f) return car->accel[0];
	if (x >= (float)(CARSIM_ACCEL_N - 1)) return car->accel[CARSIM_ACCEL_N - 1];
	const int i = (int)x;
	const float t = x - (float)i;
	return car->accel[i] + (car->accel[i + 1] - car->accel[i]) * t;
}
static float sqrtPos(float v2) { return v2 > 5e-11f ? sqrtf(v2) : 0.0f; }

// ScrubSpeeds: no point faster than braking from the one after it (back) or
// accelerating from the one before it (forwards) allows.
static void scrub(FuturePoints* f, int i, bool fwd) {
	const FPCar* car = f->car;
	if (!car || f->n < 1) return;
	if (fwd) {
		if (i < 0) i = 0;
		for (int j = i; j < f->n; j++) {
			const int pj = j > 0 ? j - 1 : 0;
			if (pj == j) continue;
			const float vp = f->fp[pj].speed;
			if (vp < 0.0f) continue;
			const float v = sqrtPos(vp * vp + 2.0f * accelOf(car, vp) * (f->fp[j].D - f->fp[pj].D));
			if (f->fp[j].speed < 0.0f || v < f->fp[j].speed) f->fp[j].speed = v;
		}
	} else {
		if (i > f->n - 1) i = f->n - 1;
		for (int j = i; j >= 0; j--) {
			const int pj = j + 1 < f->n ? j + 1 : f->n - 1;
			if (pj == j) continue;
			const float vp = f->fp[pj].speed;
			if (vp < 0.0f) continue;
			const float v = sqrtPos(vp * vp + 2.0f * car->brake * (f->fp[pj].D - f->fp[j].D));
			if (f->fp[j].speed < 0.0f || v < f->fp[j].speed) f->fp[j].speed = v;
		}
	}
}

// FillInDrivingLineSpeeds: the route point's own line speed for the new ones.
static void fillSpeeds(FuturePoints* f) {
	int first = -1;
	for (int i = 0; i < f->n; i++) {
		FP* p = &f->fp[i];
		if (p->speed >= 0.0f) continue;
		if (first < 0) first = i;
		const TRPoint* q = rpOf(f, p);
		p->speed = (float)(p->fwd ? q->speed : q->speedB);
	}
	if (first >= 0) scrub(f, first, true);
}

// ---------------------------------------------------------------------------
// Splines.

static void hermite(const float p0[2], const float m0[2], const float p1[2], const float m1[2],
		float t, float out[2]) {
	const float t2 = t * t, t3 = t2 * t;
	const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f, h10 = t3 - 2.0f * t2 + t;
	const float h01 = -2.0f * t3 + 3.0f * t2, h11 = t3 - t2;
	for (int k = 0; k < 2; k++) out[k] = h00 * p0[k] + h10 * m0[k] + h01 * p1[k] + h11 * m1[k];
}
static void setLen(float v[2], float len) {
	const float l = sqrtf(v[0] * v[0] + v[1] * v[1]);
	if (l < 1e-6f) {
		v[0] = v[1] = 0.0f;
		return;
	}
	v[0] *= len / l;
	v[1] *= len / l;
}
// The FP's driving direction (forwards: its own; the route's first FP
// backwards: GetBackwardDirection turned round).
static void fpDir(const FuturePoints* f, const FP* p, float d[2]) { dirOf(f, p, d); }

// InterpolateDrivingLinesBetweenFuturePointsUsingSplines.
static void splineBetween(FuturePoints* f, int from) {
	if (f->n < 2) return;
	int a = from < 0 ? 0 : (from >= f->n ? f->n - 1 : from);
	for (int back = 0; back < 2 && a > 0;) {
		a--;
		if (!f->fp[a].interp) back++;
	}
	while (a < f->n && f->fp[a].interp) a++;
	while (a < f->n - 1) {
		int b = a + 1;
		while (b < f->n && f->fp[b].interp) b++;
		if (b >= f->n) break;
		if (b - a > 1) {
			const FP* pa = &f->fp[a];
			const FP* pb = &f->fp[b];
			float p0[2], p1[2], m0[2], m1[2];
			fpLinePos(f, pa, p0);
			fpLinePos(f, pb, p1);
			if (a > 0) {
				float q[2];
				fpLinePos(f, &f->fp[a - 1], q);
				m0[0] = p0[0] - q[0];
				m0[1] = p0[1] - q[1];
			} else fpDir(f, pa, m0);
			int c = b + 1;
			while (c < f->n && f->fp[c].interp) c++;
			if (c < f->n) {
				float q[2];
				fpLinePos(f, &f->fp[c], q);
				m1[0] = q[0] - p1[0];
				m1[1] = q[1] - p1[1];
			} else fpDir(f, pb, m1);
			const float len = pb->D - pa->D;
			setLen(m0, 0.75f * len);
			setLen(m1, 0.75f * len);
			for (int k = a + 1; k < b && len > 0.0f; k++) {
				float sp[2];
				hermite(p0, m0, p1, m1, (f->fp[k].D - pa->D) / len, sp);
				f->fp[k].fixup += fpGetL(f, &f->fp[k], sp);
			}
		}
		a = b;
	}
}

// ---------------------------------------------------------------------------
// Corners.

static int clampIdx(const FuturePoints* f, int i) {
	return i < 0 ? 0 : (i >= f->n ? f->n - 1 : i);
}

// InterpLinesForCorner: the points 37.5 m and 75 m before (p < 0) or after it.
static void cornerLines(FuturePoints* f, FPCorner* c, float p) {
	const bool before = p <= 0.0f;
	const int i8 = c->idx + (before ? 0 : 1);
	if (i8 < 0 || i8 > f->n - 1) return;
	const float nearD = c->cornerD + 0.5f * p, farD = c->cornerD + p;
	int iNear, iFar;
	if (before) {
		int i14 = i8;
		while (i14 > 0 && f->fp[i14].D > nearD) i14--;
		int i9 = i14;
		while (i9 > 0 && f->fp[i9].D > farD) i9--;
		const int i10 = i9 + 1 < i14 ? i9 + 1 : i14;
		const int i13 = i14 + 1 < i8 ? i14 + 1 : i8;
		c->segS[0] = clampIdx(f, i10);
		c->segE[0] = clampIdx(f, i14);
		c->segS[1] = clampIdx(f, i13);
		c->segE[1] = clampIdx(f, i8);
		c->D[0] = farD;
		c->D[1] = nearD;
		iNear = i14;
		iFar = i9;
	} else {
		int i13 = i8;
		while (i13 < f->n - 1 && f->fp[i13].D < nearD) i13++;
		int i9 = i13;
		while (i9 < f->n - 1 && f->fp[i9].D < farD) i9++;
		const int i14 = i8 < i13 - 1 ? i13 - 1 : i8;
		c->segS[2] = clampIdx(f, i8);
		c->segE[2] = clampIdx(f, i14);
		c->segS[3] = clampIdx(f, i13);
		c->segE[3] = clampIdx(f, i13 > i9 - 1 ? i13 : i9 - 1);
		c->D[3] = nearD;
		c->D[4] = farD;
		iNear = i13;
		iFar = i9;
	}
	FP* nf = &f->fp[clampIdx(f, iNear)];
	FP* ff = &f->fp[clampIdx(f, iFar)];
	minMax(f, nf);
	float L;
	if (!c->inside) {
		L = nf->minL + 5.0f;
		if (-8.0f - nf->fixup > L) L = -8.0f - nf->fixup;
	} else {
		L = nf->maxL - 5.0f;
		if (8.0f - nf->fixup < L) L = 8.0f - nf->fixup;
	}
	const int pn = before ? 1 : 3, pf = before ? 0 : 4;
	fpPos(f, nf, nearD, L, c->pos[pn]);
	fpPos(f, ff, farD, -ff->fixup, c->pos[pf]);
}

// InterpolateSplineDrivingLinesForCorner: far -> near -> apex -> near -> far.
static void cornerSplines(FuturePoints* f, const FPCorner* c) {
	for (int j = 0; j < 4; j++) {
		const int s = c->segS[j], e = c->segE[j];
		const float len = c->D[j + 1] - c->D[j];
		if (!(len > 0.1f) || s < 0 || e < 0 || s >= f->n || e >= f->n || c->D[j] == -1.0f ||
				c->D[j + 1] == -1.0f)
			continue;
		float m = 0.75f * len;
		if (m < 1.0f) m = 1.0f;
		float m0[2], m1[2];
		if (s >= 1) {
			float q[2];
			fpLinePos(f, &f->fp[s - 1], q);
			m0[0] = c->pos[j][0] - q[0];
			m0[1] = c->pos[j][1] - q[1];
		} else fpDir(f, &f->fp[0], m0);
		if (j < 3) {
			m1[0] = c->pos[j + 2][0] - c->pos[j + 1][0];
			m1[1] = c->pos[j + 2][1] - c->pos[j + 1][1];
		} else if (e <= f->n - 2) {
			float q[2];
			fpLinePos(f, &f->fp[e + 1], q);
			m1[0] = q[0] - c->pos[4][0];
			m1[1] = q[1] - c->pos[4][1];
		} else fpDir(f, &f->fp[f->n - 1], m1);
		setLen(m0, j == 0 ? 0.1f * m : m);
		setLen(m1, j == 3 ? 0.1f * m : m);
		for (int k = s; k <= e; k++) {
			const float t = (f->fp[k].D - c->D[j]) / len;
			float sp[2];
			hermite(c->pos[j], m0, c->pos[j + 1], m1, t, sp);
			f->fp[k].fixup += fpGetL(f, &f->fp[k], sp);
		}
	}
}

// FinishInterpolatingCorners: each corner's lines, then its speed.
static void finishCorners(FuturePoints* f) {
	const FPCar* car = f->car;
	for (int ci = 0; ci < f->nc; ci++) {
		FPCorner* c = &f->c[ci];
		for (int j = 0; j < 4; j++) c->segS[j] = c->segE[j] = -1;
		c->D[0] = c->D[1] = c->D[3] = c->D[4] = -1.0f;
		cornerLines(f, c, -FP_CORNER_RUN);
		cornerLines(f, c, FP_CORNER_RUN);
		cornerSplines(f, c);
		if (!car) continue;
		float v = FP_CORNER_SPEED;
		if (c->idx <= f->n - 2 && c->D[1] != -1.0f && c->D[3] != -1.0f)
			v = ai_speed_for_curvature(car->friction, ai_curvature(c->pos[1], c->pos[2], c->pos[3]),
				FP_CORNER_SPEED);
		const int i = c->idx;
		if (i >= 0 && i < f->n) {
			FP* p = &f->fp[i];
			const float w = sqrtPos(v * v - 2.0f * car->brake * (p->D - c->cornerD));
			if (p->speed < 0.0f || w < p->speed) p->speed = w;
		}
		if (i + 1 >= 0 && i + 1 < f->n) {
			FP* p = &f->fp[i + 1];
			const float w = sqrtPos(v * v + 2.0f * accelOf(car, v) * (p->D - c->cornerD));
			if (p->speed < 0.0f || w < p->speed) p->speed = w;
		}
		scrub(f, i - 1, false);
		scrub(f, i + 2, true);
	}
}

// ---------------------------------------------------------------------------
// Update.

static void buildArray(FuturePoints* f) {
	if (!f->seeded) return;
	FP p;
	fpInit(&p, f->seedRoute, f->seedPoint, f->seedFwd);
	p.hop = f->seedHop;
	f->n = 0;
	f->nc = 0;
	addPoint(f, &p);
}

bool fp_update(FuturePoints* f, const float pos[3], float maxD, const FPCar* car) {
	if (!f) return false;
	f->car = car;
	f->carZ = pos[2];
	if (f->n > 0) {
		const int c = closest(f, pos, 0, 0.0f, 0);
		if (c > FP_KEEP_BEHIND) shiftLeft(f, c - FP_KEEP_BEHIND);
	}
	removeTail(f);
	int oldCount = f->n;
	if (f->n < 1) {
		buildArray(f);
		oldCount = 0;
		if (f->n < 1) return false;
	}
	fillUp(f, f->fp[0].D + maxD);
	fillSpeeds(f);
	splineBetween(f, oldCount - 1);
	finishCorners(f);
	for (int i = 0; i < f->n; i++) minMax(f, &f->fp[i]);
	return f->n > 1;
}

// ---------------------------------------------------------------------------
// What the driver reads.

float fp_distance(const FuturePoints* f, const float pos[2]) {
	if (!f || f->n < 2) return 1e30f;
	const float p3[3] = { pos[0], pos[1], f->carZ };
	const int c = closest(f, p3, 0, 0.0f, 0);
	if (c < 0) return 1e30f;
	const FP* p = &f->fp[c];
	const float d = fpGetD(f, p, pos);
	const float far = f->fp[f->n - 1].D + 10.0f;
	if (d < f->fp[0].D - 10.0f || d > far) return 1e30f;
	return fabsf(fpGetL(f, p, pos));
}

float fp_lateral(const FuturePoints* f, const float pos[2]) {
	if (!f || f->n < 1) return 0.0f;
	const float p3[3] = { pos[0], pos[1], f->carZ };
	const int c = closest(f, p3, 0, 0.0f, 0);
	return c < 0 ? 0.0f : fpGetL(f, &f->fp[c], pos);
}

// The last FP at or before D.
static int fpAtD(const FuturePoints* f, float D) {
	int i = 0;
	while (i + 1 < f->n && f->fp[i + 1].D <= D) i++;
	return i;
}

static float carD(const FuturePoints* f, const float pos[2]) {
	const float p3[3] = { pos[0], pos[1], f->carZ };
	const int c = closest(f, p3, 0, 0.0f, 0);
	return c < 0 ? 0.0f : fpGetD(f, &f->fp[c], pos);
}

bool fp_point_ahead(const FuturePoints* f, const float pos[2], float ahead, float L, float out[2]) {
	if (!f || f->n < 2) return false;
	const float D = carD(f, pos) + ahead;
	fpPos(f, &f->fp[fpAtD(f, D)], D, L, out);
	return true;
}

// InterpolateDrivingLineSpeed: between this route point's FP and the next
// route point's.
float fp_speed_ahead(const FuturePoints* f, const float pos[2], float ahead) {
	if (!f || f->n < 2) return -1.0f;
	const float D = carD(f, pos) + ahead;
	if (D >= f->fp[f->n - 1].D) return f->fp[f->n - 1].speed;
	const int i = fpAtD(f, D);
	const FP* p = &f->fp[i];
	const float d0 = p->D - p->p54;
	for (int j = i + 1; j < f->n; j++) {
		const FP* q = &f->fp[j];
		if (q->route == p->route && q->point == p->point) continue;
		const float span = q->D - d0;
		if (!(span > 1e-3f)) break;
		float t = (D - d0) / span;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		return p->speed + (q->speed - p->speed) * t;
	}
	return p->speed;
}

void fp_drive_target_clear(FuturePoints* f) {
	if (f) f->ndt = 0;
}

// ChangeCurrentL: toward the target, at most `step`.
static float changeL(float L, float target, float step) {
	if (L < target) return L + step < target ? L + step : target;
	return L - step > target ? L - step : target;
}

int fp_drive_target(FuturePoints* f, const float pos[3], float speed, float targetL,
		float rate, float margin, float out[][2], int max) {
	if (!f || f->n < 2 || max < 1) return 0;
	const float cd = carD(f, pos);
	// Keep last frame's points still ahead, out to half a second's run (30 m).
	float keep = 0.5f * speed;
	if (keep < 30.0f) keep = 30.0f;
	int k0 = 0;
	while (k0 < f->ndt && f->dt[k0].D <= cd) k0++;
	int m = 0;
	for (int i = k0; i < f->ndt; i++) {
		if (f->dt[i].D - cd > keep) break;
		f->dt[m++] = f->dt[i];
	}
	f->ndt = m;
	int start;
	float L;
	if (f->ndt > 0) {
		const FPTarget* t = &f->dt[f->ndt - 1];
		const float tp[3] = { t->x, t->y, pos[2] };
		start = closest(f, tp, 1, 0.5f, 0);
		if (start < 0) start = f->n;
		const int c = closest(f, tp, 0, 0.0f, start - 1 > 0 ? start - 1 : 0);
		L = c < 0 ? 0.0f : fpGetL(f, &f->fp[c], tp);
	} else {
		const int c = closest(f, pos, 0, 0.0f, 0);
		start = c < 0 ? f->n : c + 1;
		// Not behind the car: on from the first FP ahead of it.
		while (start < f->n && f->fp[start].D <= cd) start++;
		L = c < 0 ? 0.0f : fpGetL(f, &f->fp[c], pos);
	}
	const float dir = targetL > L ? 1.0f : -1.0f;
	for (int i = start; i < f->n && f->ndt < FP_DT_MAX; i++) {
		FP* p = &f->fp[i];
		float L2 = changeL(L, targetL, rate * fpLength(f, i));
		float lo = p->minL + margin, hi = p->maxL - margin;
		if (lo > 0.0f) lo = 0.0f;
		if (hi < 0.0f) hi = 0.0f;
		if (L2 < lo) L2 = lo;
		if (L2 > hi) L2 = hi;
		if (fabsf(L2 - targetL) > 0.001f && (L2 - L) * dir < 0.0f) rate = 0.0f;
		L = L2;
		FPTarget* t = &f->dt[f->ndt++];
		float wp[2];
		fpPos(f, p, p->D, L, wp);
		t->x = wp[0];
		t->y = wp[1];
		t->D = p->D;
		t->L = L;
	}
	const int n = f->ndt < max ? f->ndt : max;
	for (int i = 0; i < n; i++) {
		out[i][0] = f->dt[i].x;
		out[i][1] = f->dt[i].y;
	}
	return n;
}

void fp_drive_target_move(FuturePoints* f, int i, const float to[2]) {
	if (!f || i < 0 || i >= f->ndt) return;
	FPTarget* t = &f->dt[i];
	t->x = to[0];
	t->y = to[1];
	const float p3[3] = { to[0], to[1], f->carZ };
	const int c = closest(f, p3, 0, 0.0f, 0);
	if (c >= 0) t->L = fpGetL(f, &f->fp[c], to);
}

float fp_drive_target_offset(const FuturePoints* f, const float pos[2]) {
	if (!f || f->ndt < 1 || f->n < 1) return 0.0f;
	return fp_lateral(f, pos) - f->dt[0].L;
}
