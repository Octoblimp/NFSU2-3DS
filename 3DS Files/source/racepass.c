#include "racepass.h"
#include "trackroute_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See racepass.h. Function by function this is retail's; where gps.c already
// carries the same function for free roam (#358) it is that code on this set.

#define RPS_TYPES 3      // ePostednessType: 0 racers, 1 traffic, 2 players
#define RPS_NUMS 2048    // route numbers, a point's bits 0-10
#define SIGN_DEAD_END 3

typedef struct {
	TrackRouteSet* s;
	const RaceRoute* rr;
	int track;
	bool loop;           // TrackPathManager +0x24 (0x803F448C)
	float len;           // +0x18 (0x803F4480)
	float rd;            // +0x20 (0x803F4488)
	s16 byNum[RPS_NUMS];
	u8* gone;            // signs sorted past the end of the list
	const float (*gates)[4];
	int nGates;
} Pass;

static Pass X;

// --- points ---------------------------------------------------------------

static TRPoint* P(int p) { return &X.s->pts[p]; }
static TRRoute* RT(int r) { return &X.s->routes[r]; }
static int ptRoute(int p) { return P(p)->route; }
static int ptIndex(int p) { return p - RT(ptRoute(p))->first; }
static int ptPrev(int p) { return ptIndex(p) > 0 ? p - 1 : -1; }
static int ptNext(int p) {
	const TRRoute* r = RT(ptRoute(p));
	return p + 1 < r->first + r->count ? p + 1 : -1;
}
// GetRelativePoint (0x801F7D88): the next point for true, the previous for false.
static int ptRel(int p, bool fwd) { return fwd ? ptNext(p) : ptPrev(p); }
static float ptLen(int p) { return P(p)->len; }
static float ptLenBack(int p) {
	const int q = ptPrev(p);
	return ptLen(q >= 0 ? q : p);
}
static bool ptBit(int p, int b) { return (P(p)->bits >> b) & 1u; }
static void ptSetBit(int p, int b, bool v) {
	P(p)->bits = (P(p)->bits & ~(1u << b)) | ((v ? 1u : 0u) << b);
}

// GetNumSignposts / GetSignpost: the point's slots, less any sorted past the end.
static int ptSigns(int p, int out[2]) {
	int n = 0;
	for (int i = 0; i < 2; i++) {
		const int si = P(p)->sign[i];
		if (si == 0xFFFF) break;
		if (si < X.s->nSigns && !X.gone[si]) out[n++] = si;
	}
	return n;
}

// --- routes ---------------------------------------------------------------

static bool rB(int r, int b) { return r >= 0 && ((RT(r)->bits >> b) & 1u); }
static void rSet(int r, int b, bool v) {
	if (r < 0) return;
	RT(r)->bits = (RT(r)->bits & ~(1u << b)) | ((v ? 1u : 0u) << b);
}
static int routeOfNum(int n) { return n >= 0 && n < RPS_NUMS ? X.byNum[n] : -1; }
// IsExcludedOnTrack (0x801F8648) / IsIncludedOnTrack (0x801F8678).
static bool excluded(int r, int track) {
	for (int k = 0; k < 4; k++) if (RT(r)->excl[k] == (u16)track) return true;
	return false;
}
static bool included(int r, int track) {
	for (int k = 0; k < 4; k++) if (RT(r)->incl[k] == (u16)track) return true;
	return false;
}

// --- signs ----------------------------------------------------------------

static TRSign* G(int s) { return &X.s->signs[s]; }
static bool spBit(int s, int b) { return (G(s)->bits >> b) & 1u; }
static int spType(int s) { return (int)(G(s)->bits & 15u); }
// GetSignType(ePostednessType): traffic's nibble is bits 4-7, racers' and
// players' share bits 0-3.
static int spTypeT(int s, int t) { return t == 1 ? (int)((G(s)->bits >> 4) & 15u) : spType(s); }
// ChangeSignType.
static void spSetT(int s, int t, int v) {
	u32* b = &G(s)->bits;
	if (t == 1) *b = (*b & ~0xF0u) | ((u32)v << 4);
	else *b = (*b & ~0x0Fu) | (u32)v;
}
// Ignore (0x801F4A20), IsAlways (0x801F4ABC), IsNeverTake (0x801F4BD0).
static bool spIgnore(int s, int t) {
	const int ty = spTypeT(s, t);
	return t == 0 ? (ty == 7 || ty == 8) : ty == 7;
}
static bool spAlways(int s, int t) {
	const int ty = spTypeT(s, t);
	return t == 2 ? (ty == 2 || ty == 8) : ty == 2;
}
static bool spNeverTake(int s, int t) { return spTypeT(s, t) == 10; }
static bool spApplies(int s, bool fwd) { return spBit(s, 18) != fwd; }
static bool spDontTake(int ty) { return ty == 5 || ty == 10; }
// The direction a walk carries on in once across: !bit 18, flipped by bit 23.
static bool spOnDir(int s) { return !spBit(s, 18) != spBit(s, 23); }

// GetOldRoute / GetOldRoutePoint (+0x0A / +0x06), GetNewRoute (0x801F4650) /
// GetNewRoutePoint (0x801F46C0): the new fields, or the old ones while the
// sign is a dead end.
static int spOldRn(int s) { return G(s)->oldRn; }
static int spNewRn(int s) { return spType(s) == SIGN_DEAD_END ? G(s)->oldRn : G(s)->newRn; }
static int spOldRoute(int s) { return routeOfNum(spOldRn(s)); }
static int spNewRoute(int s) { return routeOfNum(spNewRn(s)); }
static int ptOf(int r, int idx) {
	return r >= 0 && idx >= 0 && idx < RT(r)->count ? RT(r)->first + idx : -1;
}
static int spOldPt(int s) { return ptOf(spOldRoute(s), G(s)->oldPt); }
static int spNewPt(int s) {
	return ptOf(spNewRoute(s), spType(s) == SIGN_DEAD_END ? G(s)->oldPt : G(s)->newPt);
}

// GetTwin (0x801F4D14): the sign at the landing point leading back.
static int spTwin(int s) {
	const int o = spOldPt(s), n = spNewPt(s);
	if (n == o) return s;
	if (n < 0) return -1;
	const bool b18 = spBit(s, 18), b23 = spBit(s, 23);
	int us[2];
	const int ns = ptSigns(n, us);
	for (int i = 0; i < ns; i++) {
		const int u = us[i];
		const bool ok = !spBit(u, 18) ? (b18 != b23) : (b18 == b23);
		if (ok && spOldPt(u) == n && spNewPt(u) == o) return u;
	}
	return -1;
}

// --- the race's path ------------------------------------------------------

static void coordReset(RaceCoord* c, int p) {
	const float xy[2] = { P(p)->x, P(p)->y };
	race_coord_init(X.rr, c, xy);
}
static void coordUpdate(RaceCoord* c, int p) {
	const float xy[2] = { P(p)->x, P(p)->y };
	race_coord_update(X.rr, c, xy);
}
static float coordOff(const RaceCoord* c) {
	const float dx = c->last[0] - c->onPath[0], dy = c->last[1] - c->onPath[1];
	return sqrtf(dx * dx + dy * dy);
}
// GetRelevantDistance (0x801F5870): traffic's at least 150 m (0x80336800).
static float relevantDist(int t) { return t == 1 && X.rd < 150.0f ? 150.0f : X.rd; }
static bool inRange(float d, float rd) { return X.loop || (d >= -rd && d <= X.len + rd); }
// CloseToPath (0x801F5894).
static bool closeToPath(const RaceCoord* c, int t) {
	const float rd = relevantDist(t);
	return coordOff(c) <= rd && inRange(c->dist, rd);
}
static float cmpDist(float a, float b) { return race_route_compare(X.rr, a, b); }
static float cmpPct(float a, float b) { return race_route_compare_percent(X.rr, a, b); }

// --- barriers -------------------------------------------------------------

// DoLinesIntersect (0x801FFCF8).
static bool linesCross(const float a[2], const float b[2], const float c[2], const float d[2]) {
	const float f1 = b[1] - a[1], f4 = d[0] - c[0], f6 = b[0] - a[0], f3 = d[1] - c[1];
	const float f7 = f6 * f3 - f1 * f4;
	if (f7 == 0.0f) return false;
	const float f5 = a[0] - c[0], f2 = a[1] - c[1];
	const float u = (f2 * f4 - f5 * f3) / f7;
	if (u < 0.0f || u > 1.0f) return false;
	const float v = (f2 * f6 - f5 * f1) / f7;
	return v >= 0.0f && v <= 1.0f;
}

// CrossesBarrier (0x801F598C): EnableBarriers sets +0x12 on every gate it
// closes, so traffic crosses them all.
static bool crosses(int p, int q, int t) {
	if (t == 1) return false;
	const float a[2] = { P(p)->x, P(p)->y }, b[2] = { P(q)->x, P(q)->y };
	for (int i = 0; i < X.nGates; i++)
		if (linesCross(X.gates[i], X.gates[i] + 2, a, b)) return true;
	return false;
}

static const u8 kPostBit[RPS_TYPES][2] = { { 11, 18 }, { 25, 26 }, { 27, 28 } };
static bool posted(int p, int t) { return ptBit(p, kPostBit[t][0]); }
static bool postedDir(int p, int t, bool fwd) { return ptBit(p, kPostBit[t][fwd ? 0 : 1]); }

// GetNextPointOnSegment (0x801F7E28) with takeZero false.
static int nextOnSeg(int p, bool* fwd, int t, bool skipPosted) {
	if (!skipPosted && !posted(p, t)) return -1;
	int us[2];
	const int ns = ptSigns(p, us);
	for (int i = 0; i < ns; i++) {
		const int s = us[i];
		if (spApplies(s, *fwd) && spAlways(s, t)) {
			if (spBit(s, 23)) *fwd = !*fwd;
			const int to = spNewPt(s);
			if (to != p) return to;
			break;
		}
	}
	const int q = ptRel(p, *fwd);
	if (q < 0) return -1;
	return skipPosted || posted(q, t) ? q : -1;
}

// WillCrossBarrier (0x801F5A6C): a closed gate within 200 m (0x8033684C)
// before a sign onto another route.
static bool willCross(int p, int t, bool fwd, int sign, int prev) {
	if (prev >= 0 && p >= 0 && crosses(prev, p, t)) return true;
	if (p < 0 || !rB(ptRoute(p), 12)) return false;
	float dist = 0.0f;
	while (p >= 0) {
		int us[2];
		const int ns = ptSigns(p, us);
		for (int i = 0; i < ns; i++) {
			const int s = us[i];
			if (s != sign && !spIgnore(s, t) && spApplies(s, fwd) && spOldRn(s) != spNewRn(s))
				return false;
		}
		const int q = nextOnSeg(p, &fwd, t, true);
		if (q >= 0) {
			if (crosses(p, q, t)) return true;
			dist += hypotf(P(p)->x - P(q)->x, P(p)->y - P(q)->y);
		}
		p = q;
		if (dist >= 200.0f) return false;
	}
	return false;
}

// WillMakeUTurn (0x801F5540): within 100 m (0x803367F4) the walk goes back
// along the race twice, before a sign onto another route.
static bool willUTurn(int p, int t, bool fwd, int sign, int prev, bool prevFwd) {
	if (p < 0) return false;
	const int r = ptRoute(p);
	if (prev < 0) {
		prev = p;
		prevFwd = fwd;
	}
	bool back = !prevFwd;
	const int q = nextOnSeg(prev, &back, t, true);
	const bool skip = t == 1 ? rB(r, 9) : (rB(r, 7) && !ptBit(prev, 29));
	if (skip) return false;
	const float ref = q >= 0 ? cmpDist(P(prev)->raceDist, P(q)->raceDist) : 1.0f;
	int count = 0;
	float dist = 0.0f;
	bool bad = false;
	while (p >= 0) {
		int us[2];
		const int ns = ptSigns(p, us);
		for (int i = 0; i < ns; i++) {
			const int s = us[i];
			if (s != sign && !spIgnore(s, t) && !bad && spApplies(s, fwd) &&
					spOldRn(s) != spNewRn(s))
				return false;
		}
		const int n = nextOnSeg(p, &fwd, t, true);
		if (n >= 0) {
			dist += hypotf(P(n)->x - P(p)->x, P(n)->y - P(p)->y);
			if (ref * cmpDist(P(n)->raceDist, P(p)->raceDist) >= 0.0f) {
				bad = false;
			} else {
				bad = true;
				if (++count > 1) return true;
			}
		}
		if (dist >= 100.0f) return false;
		p = n;
	}
	return false;
}

// GetPointAfterDistance, as MostFruitfulExit asks it: `d` metres on.
static int pointAfter(int p, bool fwd, float d) {
	while (p >= 0 && d > 0.0f) {
		const int q = ptRel(p, fwd);
		if (q < 0) break;
		d -= fwd ? ptLen(p) : ptLen(q);
		p = q;
	}
	return p;
}

// MostFruitfulExit (0x801F4FA4): of the point's two signs that leave it the
// way `fwd` goes, the one whose road is further along the race 30 m on
// (0x8033641C).
static int mostFruitful(int p, bool fwd, int t) {
	int e[2] = { -1, -1 };
	for (int i = 0; i < 2; i++) {
		const int s = P(p)->sign[i];
		if (s == 0xFFFF || s >= X.s->nSigns || X.gone[s]) continue;
		if (!spIgnore(s, t) && spOldRn(s) != spNewRn(s) && spApplies(s, fwd)) e[i] = s;
	}
	if (e[0] < 0 || e[1] < 0) return e[0] >= 0 ? e[0] : e[1];
	const int a = pointAfter(spNewPt(e[0]), spOnDir(e[0]), 30.0f);
	const int b = pointAfter(spNewPt(e[1]), spOnDir(e[1]), 30.0f);
	if (a < 0 || b < 0) return e[0];
	return P(b)->raceDist <= P(a)->raceDist ? e[0] : e[1];
}

// MakeUnreachable (0x801F5218): close the way into the road behind `sign`.
static void makeUnreachable(int sign, int t, int depth) {
	if (depth >= 0x65) return;
	const int r0 = spOldRoute(sign);
	if (r0 < 0 || !rB(r0, 6)) return;
	const bool b18 = spBit(sign, 18);
	const bool fwd = !b18;
	int p = spOldPt(sign);
	while (p >= 0) {
		if (!ptBit(p, 29) && rB(ptRoute(p), 7) && !X.loop) return;
		int us[2];
		int ns = ptSigns(p, us);
		for (int i = 0; i < ns; i++) {
			int s = us[i];
			if (s == sign || !spApplies(s, fwd) || spIgnore(s, t) || spNeverTake(s, t)) continue;
			const int nr = spNewRoute(s);
			if (nr == r0) continue;
			if (nr >= 0 && rB(nr, 6)) {
				if (spAlways(s, t)) return;
				if (spBit(s, 26)) {
					const int m = mostFruitful(p, fwd, t);
					if (m >= 0) s = m;
				}
				spSetT(s, t, 2);
				return;
			}
			spSetT(s, t, 7);
		}
		ns = ptSigns(p, us);
		for (int i = 0; i < ns; i++) {
			const int s = us[i];
			if (s == sign || spApplies(s, fwd)) continue;
			const int tw = spTwin(s);
			if (tw < 0 || spIgnore(tw, t) || spNeverTake(tw, t) || spType(s) == SIGN_DEAD_END)
				continue;
			const int ro = spOldRoute(tw);
			if (ro < 0 || !rB(ro, 6)) continue;
			const bool always = spAlways(tw, t);
			spSetT(tw, t, 7);
			if (always) makeUnreachable(tw, t, depth + 1);
		}
		p = ptRel(p, b18);
	}
}

// The body EliminateBarrierViolations and EliminateUTurns share: a sign
// whose way on is bad is ignored, one whose way back is bad becomes always.
static int eliminate(bool uturns) {
	int changed = 0;
	for (int s = 0; s < X.s->nSigns; s++) {
		if (X.gone[s]) continue;
		for (int t = 0; t < RPS_TYPES; t++) {
			if ((uturns && t == 1) || spIgnore(s, t)) continue;
			const int o = spOldPt(s), n = spNewPt(s);
			const bool u7 = !spBit(s, 18);
			const bool b11 = u7 != spBit(s, 23);
			const int ty = spTypeT(s, t);
			bool b1 = false, b2 = false, b3 = false;
			if (ty == 2) {
				if (uturns ? willUTurn(n, t, b11, s, o, u7) : willCross(n, t, b11, s, o))
					b1 = b3 = true;
			} else if (ty == 0 || ty == 1 || ty == 4 || ty == 6 || ty == 9) {
				const bool a = uturns ? willUTurn(o, t, u7, s, -1, true) : willCross(o, t, u7, s, -1);
				const bool b = uturns ? willUTurn(n, t, b11, s, o, u7) : willCross(n, t, b11, s, o);
				if (a && b) b1 = b3 = true;
				else if (b) b1 = true;
				else if (a) b2 = true;
			}
			if (b1) spSetT(s, t, 7);
			if (b2) spSetT(s, t, 2);
			if (b3) makeUnreachable(s, t, 1);
			changed += b1 || b2 || b3;
		}
	}
	return changed;
}

// EliminateHTurns (0x801F6180): within 30 m (0x8033699C) of landing from a
// corner sign, another corner sign the same way that is not always goes.
static int hTurns(void) {
	int changed = 0;
	for (int s = 0; s < X.s->nSigns; s++) {
		if (X.gone[s]) continue;
		for (int t = 0; t < RPS_TYPES; t++) {
			if (t == 1 || spIgnore(s, t) || !spBit(s, 26)) continue;
			int p = spNewPt(s);
			const bool dir = spOnDir(s);
			float dist = 0.0f;
			while (p >= 0) {
				dist += dir ? ptLenBack(p) : ptLen(p);
				if (dist > 30.0f) break;
				bool stop = false;
				int us[2];
				const int ns = ptSigns(p, us);
				for (int i = 0; i < ns; i++) {
					const int u = us[i];
					if (!spApplies(u, dir) || spIgnore(u, t)) continue;
					if (!spBit(u, 26) || spAlways(u, t)) {
						stop = true;
					} else {
						spSetT(u, t, 7);
						changed++;
					}
				}
				p = stop ? -1 : ptRel(p, dir);
			}
		}
	}
	return changed;
}

// CanBeBypassed (0x801F643C): within 150 m (0x80336AD8) behind a corner sign
// a sign onto a route closed to free roam rejoins the corner's new route
// further on, with nothing to take between.
static bool canBypass(int s) {
	const bool b18 = spBit(s, 18), u19 = !b18;
	const bool b22 = u19 != spBit(s, 23);
	const int sgn = b22 ? 1 : -1;
	int p = spOldPt(s);
	if (p < 0) return false;
	const float pos[3] = { P(p)->x, P(p)->y, P(p)->z };
	int best = -1;
	float dist = 0.0f;
	while (p >= 0 && dist < 150.0f) {
		if (dist > 0.0f) {
			int us[2];
			const int ns = ptSigns(p, us);
			for (int i = 0; i < ns; i++) {
				const int u = us[i];
				if (spBit(u, 18) != b18) {
					const int tw = spTwin(u);
					if (tw >= 0 && spType(tw) != 7 && !spDontTake(spType(tw)) && spType(s) == 2)
						return false;
					continue;
				}
				if (spType(u) == 7 || spOldRn(u) == spNewRn(u)) continue;
				if (spDontTake(spType(u))) {
					if (spType(s) == 2) return false;
					continue;
				}
				const int nr = spNewRoute(u);
				if (nr < 0 || !excluded(nr, 4000)) continue;
				const bool dir2 = spOnDir(u);
				for (int q = spNewPt(u); q >= 0; q = ptRel(q, dir2)) {
					int vs[2];
					const int nv = ptSigns(q, vs);
					for (int j = 0; j < nv; j++) {
						const int v = vs[j];
						if (spType(v) == 7 || spDontTake(spType(v))) continue;
						if (dir2 == spBit(v, 18)) continue;
						if (spNewRn(v) != spNewRn(s)) continue;
						const int dv = spType(v) == SIGN_DEAD_END ? G(v)->oldPt : G(v)->newPt;
						const int ds = spType(s) == SIGN_DEAD_END ? G(s)->oldPt : G(s)->newPt;
						if ((dv - ds) * sgn <= 0 || spOnDir(v) != b22) continue;
						const int np = spNewPt(v);
						if (np < 0) continue;
						const float dx = P(np)->x - pos[0], dy = P(np)->y - pos[1];
						const float dz = P(np)->z - pos[2];
						if (sqrtf(dx * dx + dy * dy + dz * dz) < 150.0f) best = np;
					}
				}
			}
		}
		dist += u19 ? ptLen(p) : ptLenBack(p);
		p = ptRel(p, b18);
	}
	if (best < 0) return false;
	for (int q = spNewPt(s); q >= 0 && q != best; q = ptRel(q, b22)) {
		int us[2];
		const int ns = ptSigns(q, us);
		for (int i = 0; i < ns; i++) {
			const int u = us[i];
			if (b22 != spBit(u, 18)) {
				if (spType(u) != 7 && !spDontTake(spType(u))) return false;
			} else {
				const int tw = spTwin(u);
				if (tw >= 0 && tw != s && spType(tw) != 7 && !spDontTake(spType(tw))) break;
			}
		}
	}
	return true;
}

static void bypassCorners(void) {
	for (int s = 0; s < X.s->nSigns; s++) {
		if (X.gone[s] || !spBit(s, 26) || spType(s) == 7 || spType(s) == SIGN_DEAD_END) continue;
		if (!canBypass(s)) continue;
		const bool always = spAlways(s, 0);
		spSetT(s, 0, 7);
		if (always) makeUnreachable(s, 0, 1);
	}
}

// --- relevance ------------------------------------------------------------

// SetDistanceAroundTrackAndNativeDirection (0x801F8AE4): each point's race
// distance (+0x08) and whether it is close to the race (bit 29); route bit 1
// when it runs against the race. Returns the nearest the route comes.
// PORT: no gate counts against it here -- EnableBarriers sets +0x12 on every
// gate it closes, and only a closed gate with +0x12 clear would.
static float setDistance(int r, bool* nativeBoth) {
	const TRRoute* rt = RT(r);
	if (nativeBoth) *nativeBoth = false;
	if (rt->count < 1) return 1e30f;
	const float rd = relevantDist(0);
	RaceCoord c;
	int p = rt->first;
	coordReset(&c, p);
	float d = coordOff(&c), minD = d, prev = c.dist;
	bool close = d <= rd && inRange(prev, rd);
	bool corner = false;
	ptSetBit(p, 29, close);
	P(p)->raceDist = c.dist;
	int us[2];
	int ns = ptSigns(p, us);
	for (int i = 0; i < ns; i++) corner |= spBit(us[i], 26);
	int nF = 0, nB = 0;
	for (int i = 1; i < rt->count; i++) {
		p = rt->first + i;
		if (close) coordUpdate(&c, p);
		else coordReset(&c, p);
		d = coordOff(&c);
		if (d < minD) minD = d;
		close = d <= rd && inRange(prev, rd);
		ptSetBit(p, 29, close);
		const float dist = c.dist;
		P(p)->raceDist = dist;
		if (close) {
			if (cmpDist(dist, prev) < 0.0f) nB++;
			else nF++;
		}
		ns = ptSigns(p, us);
		for (int k = 0; k < ns; k++) corner |= spBit(us[k], 26);
		prev = dist;
	}
	if (rt->type == 0) {
		rSet(r, 1, nF < nB);
		if (nativeBoth) {
			const int mx = nF > nB ? nF : nB;
			*nativeBoth = mx * 2 <= nB * nF ? !corner : false;
			rSet(r, 11, *nativeBoth);
		}
	}
	return minD;
}

// FindNextDepartingSignpostOnRoute (0x801F9284): on along route `r` from
// `start` while close to the race, the next sign onto another route;
// `*flag` where the walk passes the race's start or finish.
static int nextDeparting(int r, int start, int t, bool dir, bool side, bool* flag) {
	RaceCoord c;
	coordReset(&c, start);
	float prevPct = c.fraction * 100.0f;
	const float k = 10000.0f / X.len;
	for (int p = ptRel(start, dir); p >= 0; p = ptRel(p, dir)) {
		coordUpdate(&c, p);
		const float pct = c.fraction * 100.0f;
		const float a = cmpPct(pct, 0.0f), b = cmpPct(prevPct, 0.0f);
		if (a * b < 0.0f) *flag = true;
		const float e = cmpPct(pct, 100.0f), f = cmpPct(prevPct, 100.0f);
		if (e * f < 0.0f) *flag = true;
		if (coordOff(&c) > relevantDist(t)) break;
		if (a >= -k && a <= 0.0f && !X.loop && !rB(r, 7) && !*flag) *flag = true;
		if (e >= 0.0f && e <= k && !X.loop && !rB(r, 7) && !*flag) *flag = true;
		int found = -1;
		int us[2];
		const int ns = ptSigns(p, us);
		for (int i = 0; i < ns; i++) {
			const int u = us[i];
			if (!spIgnore(u, t) && spBit(u, 18) != side && spNewRoute(u) != r) found = u;
		}
		if (found >= 0) return found;
		prevPct = pct;
	}
	return -1;
}

// DetermineRelevanceOfRoutesToTrackPath (0x801FE998), its race half.
static void relevance(void) {
	for (int r = 0; r < X.s->nRoutes; r++) {
		rSet(r, 11, false);
		rSet(r, 9, false);
		rSet(r, 7, false);
		rSet(r, 6, false);
	}
	static const int order[RPS_TYPES] = { 0, 2, 1 };
	for (int s = 0; s < X.s->nSigns; s++) {
		const int R = spNewRoute(s);
		if (R < 0 || RT(R)->type == 1 || excluded(R, X.track) || rB(R, 10)) continue;
		bool rel = false;
		for (int k = 0; k < RPS_TYPES; k++) {
			const int t = order[k];
			if (spIgnore(s, t)) continue;
			const int p = spNewPt(s);
			if (p < 0) continue;
			RaceCoord c;
			coordReset(&c, p);
			if (!closeToPath(&c, t)) continue;
			if (t == 1) {
				rSet(R, 9, true);
				continue;
			}
			bool flag = false;
			const bool dir = spTypeT(s, t) == SIGN_DEAD_END ? spBit(s, 18) : spOnDir(s);
			int q = p;
			for (int guard = 0; guard < RT(R)->count + 1; guard++) {
				const int u = nextDeparting(R, q, t, dir, dir, &flag);
				if (u < 0) break;
				q = spOldPt(u);
				if (q < 0) break;
				coordReset(&c, q);
				if (closeToPath(&c, t)) {
					rel = true;
					if (X.loop) break;
				}
			}
			if (flag && !X.loop) {
				rel = true;
				rSet(R, 7, true);
			}
		}
		if (rel || rB(R, 9)) {
			bool both = false;
			setDistance(R, &both);
			if (both && !rB(R, 7)) rel = false;
		}
		if (rel) rSet(R, 6, true);
	}
	for (int r = 0; r < X.s->nRoutes; r++) {
		if (!rB(r, 9)) continue;
		if (!rB(r, 6)) rSet(r, 6, true);
		else rSet(r, 9, false);
	}
	float mn[2] = { 0, 0 }, mx[2] = { 0, 0 };
	const bool box = race_route_paths_box(X.rr, mn, mx);
	const float rd1 = relevantDist(1);
	for (int r = 0; r < X.s->nRoutes; r++) {
		if (included(r, X.track) && !rB(r, 6)) {
			setDistance(r, NULL);
			rSet(r, 6, true);
		}
		const TRRoute* rt = RT(r);
		if (box && rt->type == 1 && !(rt->bbMax[0] < mn[0] - rd1 || rt->bbMin[0] > mx[0] + rd1 ||
				rt->bbMax[1] < mn[1] - rd1 || rt->bbMin[1] > mx[1] + rd1)) {
			if (setDistance(r, NULL) <= rd1) rSet(r, 6, true);
		}
	}
}

// SetDistanceAroundTrackAndFixupDanglers (0x801F6BDC), what it does to the
// sign types; the signs it counts are sorted past the end.
static void danglers(void) {
	for (int s = 0; s < X.s->nSigns; s++) {
		const int O = spOldRoute(s), N = spNewRoute(s);
		if (!rB(O, 6)) {
			spSetT(s, 0, 7);
			spSetT(s, 1, 7);
			X.gone[s] = 1;
			continue;
		}
		if (!rB(N, 6)) {
			bool all = true;
			for (int t = 0; t < RPS_TYPES; t++) {
				const int ty = spTypeT(s, t);
				if (ty == 2) {
					if (!rB(O, 7) && t != 1) {
						spSetT(s, t, 7);
						makeUnreachable(s, t, 1);
					} else {
						spSetT(s, 0, SIGN_DEAD_END);
						spSetT(s, 1, SIGN_DEAD_END);
					}
				} else if ((ty >= 0 && ty < 2) || (ty > 3 && ty < 7) || (ty >= 8 && ty <= 10)) {
					spSetT(s, t, 7);
				}
				if (spTypeT(s, t) != 7) all = false;
			}
			if (all) X.gone[s] = 1;
			continue;
		}
		const int ty = spType(s);
		if (!rB(N, 9)) {
			if (!rB(O, 9)) {
				const int o = spOldPt(s), n = spNewPt(s);
				if (o >= 0 && n >= 0 && !ptBit(o, 29) && !ptBit(n, 29) && ty != SIGN_DEAD_END) {
					spSetT(s, 0, 7);
					if (ty == 2) makeUnreachable(s, 0, 1);
				}
			} else if (ty != 7 && ty != SIGN_DEAD_END && ty != 10) {
				spSetT(s, 0, 7);
			}
		} else if (ty != 7 && ty != SIGN_DEAD_END && ty != 10) {
			if (rB(O, 9)) {
				spSetT(s, 0, 7);
			} else {
				const int o = spOldPt(s);
				const float z = o >= 0 ? P(o)->raceDist : 0.0f;
				const float zc = z < 0.0f ? 0.0f : (z > X.len ? X.len : z);
				if (!rB(O, 7) || z == zc) {
					spSetT(s, 0, 7);
					if (ty == 2) makeUnreachable(s, 0, 1);
				} else {
					spSetT(s, 0, SIGN_DEAD_END);
				}
			}
		}
	}
}

// --- postedness -----------------------------------------------------------

// MarkPostedness (0x801FDF40), literally, as gps.c's barMark.
static void mark(int p, bool dir, int t) {
	const bool b10 = rB(ptRoute(p), 1) ? !dir : dir;
	bool b1 = false, b2 = false;
	int q = p;
	for (;;) {
		q = ptRel(q, dir);
		if (q < 0) {
			b2 = true;
		} else if (!postedDir(q, t, b10)) {
			int us[2];
			const int ns = ptSigns(q, us);
			for (int i = 0; i < ns; i++) {
				const int s = us[i];
				if (spIgnore(s, t)) continue;
				const int ty = spType(s);
				const bool ap = !spBit(s, 18) ? dir : !dir;
				b2 = ap && (ty == SIGN_DEAD_END || spAlways(s, t));
				const bool b3 = ((G(s)->bits >> 8) & 15u) == SIGN_DEAD_END || t == 1;
				b1 = ty == SIGN_DEAD_END ? b3 : true;
			}
		} else {
			b1 = b2 = true;
		}
		if (b1 && q >= 0) {
			b1 = false;
			const int r = ptRoute(p);
			rSet(r, 8, true);
			int i0 = ptIndex(p), i1 = ptIndex(q);
			if (i0 > i1) {
				const int k = i0;
				i0 = i1;
				i1 = k;
			}
			const int bit = kPostBit[t][b10 ? 0 : 1];
			for (int i = i0; i <= i1; i++) ptSetBit(RT(r)->first + i, bit, true);
			p = q;
		}
		if (b2) return;
	}
}

// DeterminePostedness (0x801FE304).
static void determine(void) {
	for (int t = 0; t < RPS_TYPES; t++) {
		for (int s = 0; s < X.s->nSigns; s++) {
			if (X.gone[s] || spIgnore(s, t)) continue;
			const int to = spNewPt(s);
			if (to < 0) continue;
			const int ty = spTypeT(s, t);
			if (ty == 0 || ty == 1 || ty == 2 || ty == 4 || ty == 5 || ty == 8 || ty == 9 ||
					ty == 10) {
				mark(to, spOnDir(s), t);
			} else if (ty == SIGN_DEAD_END) {
				if (((G(s)->bits >> 8) & 15u) != SIGN_DEAD_END && t != 1 && !rB(spOldRoute(s), 7))
					continue;
				mark(to, spBit(s, 18), t);
			}
		}
	}
	// The bleeders (route bool 9) carry traffic only.
	for (int r = 0; r < X.s->nRoutes; r++) {
		if (!rB(r, 9)) continue;
		for (int i = 0; i < RT(r)->count; i++) {
			const int p = RT(r)->first + i;
			ptSetBit(p, 11, false);
			ptSetBit(p, 18, false);
			ptSetBit(p, 27, false);
			ptSetBit(p, 28, false);
		}
	}
}

// --- the pass -------------------------------------------------------------

bool racepass_run(TrackRouteSet* s, const RaceRoute* rr, int trackId, RacePassStats* st) {
	RacePassStats dummy;
	if (!st) st = &dummy;
	memset(st, 0, sizeof(*st));
	if (!s || !rr || s->nRoutes <= 0) return false;
	memset(&X, 0, sizeof(X));
	X.s = s;
	X.rr = rr;
	X.track = trackId;
	X.loop = race_route_loops(rr);
	X.len = race_route_length(rr);
	X.rd = race_route_relevant(rr);
	X.nGates = race_route_closed_gates(rr, &X.gates);
	if (X.len <= 0.0f) return false;
	for (int n = 0; n < RPS_NUMS; n++) X.byNum[n] = -1;
	for (int r = 0; r < s->nRoutes; r++)
		if (s->routes[r].num >= 0 && s->routes[r].num < RPS_NUMS) X.byNum[s->routes[r].num] = (s16)r;
	X.gone = (u8*)calloc((size_t)(s->nSigns > 0 ? s->nSigns : 1), 1);
	u32* was = (u32*)malloc(sizeof(u32) * (size_t)s->nPts);
	float* wasDist = (float*)malloc(sizeof(float) * (size_t)s->nPts);
	u32* wasRoute = (u32*)malloc(sizeof(u32) * (size_t)s->nRoutes);
	u32* wasSign = (u32*)malloc(sizeof(u32) * (size_t)(s->nSigns > 0 ? s->nSigns : 1));
	if (!X.gone || !was || !wasDist || !wasRoute || !wasSign) {
		free(X.gone);
		free(was);
		free(wasDist);
		free(wasRoute);
		free(wasSign);
		return false;
	}
	for (int p = 0; p < s->nPts; p++) {
		was[p] = s->pts[p].bits;
		wasDist[p] = s->pts[p].raceDist;
	}
	for (int r = 0; r < s->nRoutes; r++) wasRoute[r] = s->routes[r].bits;
	for (int i = 0; i < s->nSigns; i++) wasSign[i] = s->signs[i].bits;

	// RestoreSignposts.
	for (int i = 0; i < s->nSigns; i++) {
		u32* b = &s->signs[i].bits;
		*b = (*b & ~0xFFu) | ((*b >> 8) & 0xFFu);
	}
	for (int r = 0; r < s->nRoutes; r++) rSet(r, 1, false);

	relevance();
	hTurns();
	danglers();
	// CheckForBarrierOverlap.
	for (int r = 0; r < s->nRoutes; r++) rSet(r, 12, false);
	for (int i = 0; i < X.nGates; i++) {
		const float* g = X.gates[i];
		const float mn[2] = { fminf(g[0], g[2]), fminf(g[1], g[3]) };
		const float mx[2] = { fmaxf(g[0], g[2]), fmaxf(g[1], g[3]) };
		for (int r = 0; r < s->nRoutes; r++) {
			const TRRoute* rt = RT(r);
			if (!rB(r, 6) || rB(r, 12)) continue;
			rSet(r, 12, !(mx[0] < rt->bbMin[0] || mn[0] > rt->bbMax[0] ||
				mx[1] < rt->bbMin[1] || mn[1] > rt->bbMax[1]));
		}
	}
	int c;
	do {
		const int h = hTurns();
		const int u = eliminate(true);
		const int b = eliminate(false);
		c = h + u + b;
		st->changes += c;
		st->passes++;
	} while (c != 0 && st->passes < 64);
	bypassCorners();
	// ResetPostedness.
	for (int r = 0; r < s->nRoutes; r++) rSet(r, 8, false);
	for (int p = 0; p < s->nPts; p++) {
		static const int bits[6] = { 11, 18, 25, 26, 27, 28 };
		for (int k = 0; k < 6; k++) ptSetBit(p, bits[k], false);
	}
	determine();
	// MakeUnpostedRoutesIrrelevant.
	for (int r = 0; r < s->nRoutes; r++) {
		if (rB(r, 8)) continue;
		rSet(r, 9, false);
		rSet(r, 7, false);
		rSet(r, 6, false);
	}
	// FindIrrelevantSignposts.
	for (int i = 0; i < s->nSigns; i++)
		if (!rB(spOldRoute(i), 6) || !rB(spNewRoute(i), 6)) X.gone[i] = 1;

	// The racers' results go to the fields only FuturePoints reads; the
	// file's own values go back for the port's line, forks and race
	// distances (trackroute.c), which were made for them.
	for (int p = 0; p < s->nPts; p++) {
		TRPoint* q = &s->pts[p];
		q->fpPostF = (u8)((q->bits >> 11) & 1u);
		q->fpPostB = (u8)((q->bits >> 18) & 1u);
		st->diffRacer += ((was[p] ^ q->bits) >> 11 & 1u) || ((was[p] ^ q->bits) >> 18 & 1u);
		st->diffPlayer += ((was[p] ^ q->bits) >> 27 & 1u) || ((was[p] ^ q->bits) >> 28 & 1u);
		q->bits = was[p];
		q->raceDist = wasDist[p];
	}
	for (int r = 0; r < s->nRoutes; r++) {
		st->relevant += rB(r, 6);
		s->routes[r].bits = wasRoute[r];
	}
	for (int i = 0; i < s->nSigns; i++) {
		TRSign* g = &s->signs[i];
		g->fpType = (u8)(g->bits & 15u);
		g->fpGone = X.gone[i] != 0;
		st->gone += X.gone[i];
		if (!X.gone[i]) st->racerSigns[g->fpType]++;
		g->bits = wasSign[i];
	}
	free(was);
	free(wasDist);
	free(wasRoute);
	free(wasSign);
	free(X.gone);
	X.gone = NULL;
	return true;
}
