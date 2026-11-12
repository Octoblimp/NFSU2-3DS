#include "race.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See race.h for the file formats and the function list this ports.
#define RP_CONTAINER 0x80034147u // TrackPathManager::Loader's magic
#define RP_POINTS    0x00034148u
#define RP_PATHS     0x00034149u
#define RP_GROUPS    0x0003414Cu
#define RP_BLOCKS    0x0003414Du
#define RP_MARKERS   0x00034146u

#define RP_POINT_BYTES 0x18
#define RP_PATH_BYTES  0xDC
#define RP_GROUP_BYTES 0x110
#define RP_BLOCK_BYTES 0x24
#define RP_MARKER_BYTES 0x30

#define RP_MAX_ROUTE   128 // manager+0x1BC: 0x200 bytes of path pointers
#define RP_MAX_GROUPS  16  // the branch loop's own bound
#define RP_MAX_BUILD   16  // TrackPathConnectionBuilder's list (2.24208e-44)
#define RP_MAX_TRACKS  7   // +0xAE up to the zone count at +0xBC
#define RP_MAX_ZONES   15  // +0xBE up to the end of the record
#define RP_MAX_WAYPOINTS 33 // (0x110 - 8) / 8

// Speed.elf .sdata2, by the functions that load them.
#define RP_BIG           9999999.0f   // 0x80338130 / 0x80338184
#define RP_NO_RADIUS     100000000.0f // FindClosestPoint's -1 (0x80338158)
#define RP_RESEARCH      128.0f       // UpdatePosition 0x8033819C
#define RP_NEIGHBOUR_PEN 40.0f        // 0x803381A8
#define RP_NEIGHBOUR_K   0.25f        // 0x803381AC
#define RP_PARAM_TOL     0.05f        // 0x803381B8
#define RP_REJOIN_RATIO  0.5f         // BuildPathConnectionsRecursive 0x8033811C
#define RP_EPS           5.00000006675716e-11f

typedef struct {
	float x, y;
	u16 path;
	u8 nearT, farT; // +0xA, +0xB
	s16 nb[3];      // +0xC
	float dist;     // +0x14
} RPoint;

// QuickSpline, endpoint type 2 (extrapolated), Overhauser basis. `cp` holds
// the n real control points; the ghosts FixupEndpoints writes before and after
// them are the same linear extrapolation GenerateExtrapolatedControlPoints
// uses further out, so cpAt() computes both on demand.
typedef struct {
	int n;
	float (*cp)[4];
	float length; // +0x14
} Spline;

typedef struct {
	char name[17];
	s16 first, end, count;   // +0x1C, +0x1E, +0x20
	u8 nConn[2];             // +0x22
	u16 connPath[2][4];      // +0x24 + side*0x20 + i*8
	u8 connSide[2][4];
	float length;            // +0x64
	float ep[2][2];          // +0x88, +0x90
	float bbMin[2], bbMax[2];// +0x98, +0xA0
	u8 allowCount, denyCount;// +0xAC, +0xAD
	u16 tracks[RP_MAX_TRACKS];
	s16 zoneCount;           // +0xBC
	u16 zones[RP_MAX_ZONES]; // +0xBE
	// Runtime, what CreateSplines/AssignDistances fill.
	bool splined;            // +0xA8 != NULL
	Spline spline;
	int side;                // +0x68
	float len6c;             // +0x6C spline length between the first and last point
	float w0;                // +0x70 the arc length at the first point
	float d74, d78, sign;    // +0x74 / +0x78 distance at end 0 / end 1, +0x7C
	float d80, d84;          // start / end distance
} RPath;

typedef struct {
	int start, count;   // into route[]
	float span;         // +0x08
	float startDist;    // +0x0C
	float endDist;      // +0x10
	float scale;        // +0x14
} RGroup;

struct RaceRoute {
	RPoint* pts;
	int nPts;
	RPath* paths;
	int nPaths;
	u8* blocked;
	int nBlocked;
	int nClosed; // how many of them this track's barriers close

	int trackId;
	int wpCount;
	float wp[RP_MAX_WAYPOINTS][2];
	bool loop;      // group +3 -> manager +0x24
	bool branches;  // group +4
	float f20;      // group +5 -> manager +0x20

	float total;    // +0x18
	float half;     // +0x1C
	int route[RP_MAX_ROUTE];
	u8 sides[RP_MAX_ROUTE];
	int routeCount; // +0x1B8
	RGroup groups[RP_MAX_GROUPS];
	int groupCount; // +0x34
};

typedef struct {
	float len;       // [0]
	float found;     // [1] the rejoin distance, branch search only
	float maxLen;    // [2]
	int maxDepth;    // [3]
	int target;      // [4] a path, -1 for a branch search
	int track;       // [5]
	float startDist; // [6]
	int count;       // [7]
	int list[RP_MAX_BUILD];
	u8 sides[RP_MAX_BUILD];
} Builder;

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u16 rd16le(const u8* p) {
	return (u16)(p[0] | (p[1] << 8));
}
static float rdf(const u8* p) {
	u32 v = rd32le(p);
	float f;
	memcpy(&f, &v, 4);
	return f;
}
// bStringHash (0x8005DBE0): seed 0xFFFFFFFF, h = h * 33 + c.
static u32 stringHash(const char* s) {
	u32 h = 0xFFFFFFFFu;
	while (*s)
		h = h * 33u + (u8)*s++;
	return h;
}

// ---------------------------------------------------------------- QuickSpline

static void cpAt(const Spline* s, int i, float out[4]) {
	const int n = s->n;
	if (i < 0) {
		const float k = (float)(-i);
		for (int j = 0; j < 4; j++)
			out[j] = s->cp[0][j] + k * (s->cp[0][j] - s->cp[1][j]);
	} else if (i > n - 1) {
		const float k = (float)(i - n + 1);
		for (int j = 0; j < 4; j++)
			out[j] = s->cp[n - 1][j] + k * (s->cp[n - 1][j] - s->cp[n - 2][j]);
	} else {
		for (int j = 0; j < 4; j++)
			out[j] = s->cp[i][j];
	}
}

// EvaluateSpline over Overhauser basis 0 (the point) or 1 (its derivative),
// the matrices InitBasisMatricies builds at 0x803DB968: rows are control
// points, columns powers of t.
static void splinePoint(const Spline* s, float t, int deriv, float out[4]) {
	int i = (int)t;
	if (t < (float)i)
		i--;
	const float f = t - (float)i;
	const float f2 = f * f, f3 = f2 * f;
	float w[4];
	if (!deriv) {
		w[0] = -0.5f * f + f2 - 0.5f * f3;
		w[1] = 1.0f - 2.5f * f2 + 1.5f * f3;
		w[2] = 0.5f * f + 2.0f * f2 - 1.5f * f3;
		w[3] = -0.5f * f2 + 0.5f * f3;
	} else {
		w[0] = -0.5f + 2.0f * f - 1.5f * f2;
		w[1] = -5.0f * f + 4.5f * f2;
		w[2] = 0.5f + 4.0f * f - 4.5f * f2;
		w[3] = -f + 1.5f * f2;
	}
	out[0] = out[1] = out[2] = out[3] = 0.0f;
	for (int k = 0; k < 4; k++) {
		float c[4];
		cpAt(s, i - 1 + k, c);
		for (int j = 0; j < 4; j++)
			out[j] += w[k] * c[j];
	}
}

// CalibrateLength(16): each control point's w is the arc length up to it,
// integrated with 16 chords a segment.
static void splineCalibrate(Spline* s, int steps) {
	float L = 0.0f;
	for (int i = 0; i < s->n; i++) {
		s->cp[i][3] = L;
		if (i < s->n - 1) {
			float prev[4], q[4];
			splinePoint(s, (float)i, 0, prev);
			for (int k = 1; k <= steps; k++) {
				splinePoint(s, (float)i + (float)k / (float)steps, 0, q);
				const float dx = q[0] - prev[0], dy = q[1] - prev[1], dz = q[2] - prev[2];
				L += sqrtf(dx * dx + dy * dy + dz * dz);
				prev[0] = q[0]; prev[1] = q[1]; prev[2] = q[2];
			}
		}
	}
	s->length = L;
}

// FindClosestParam: a secant-damped Newton walk on the projection of the
// target onto the tangent. Endpoint type 2 does not clamp the parameter.
static float splineClosestParam(const Spline* s, float t, float tol, const float tgt[2]) {
	float prevProj = 0.0f, prevStep = 0.0f, maxStep = 1.0f;
	for (int it = 0; it < 998; it++) {
		float P[4], D[4];
		splinePoint(s, t, 0, P);
		splinePoint(s, t, 1, D);
		const float l2 = D[0] * D[0] + D[1] * D[1] + D[2] * D[2];
		const float len = l2 > RP_EPS ? sqrtf(l2) : 0.0f;
		if (len == 0.0f)
			break;
		const float proj = ((0.0f - P[2]) * D[2] + (tgt[0] - P[0]) * D[0] +
			(tgt[1] - P[1]) * D[1]) / len;
		if (fabsf(proj) < tol)
			return t;
		float slope = len;
		if (prevStep != 0.0f) {
			slope = -(proj - prevProj) / prevStep;
			if (slope == 0.0f)
				return t;
		}
		float step = proj / slope;
		if (step < -maxStep)
			step = -maxStep;
		if (step > maxStep)
			step = maxStep;
		t += step;
		if (step * prevStep < 0.0f)
			maxStep *= 0.5f;
		prevProj = proj;
		prevStep = step;
	}
	return t;
}

// ---------------------------------------------------------------- the paths

static const RPoint* pt(const RaceRoute* r, int i) {
	return (i >= 0 && i < r->nPts) ? &r->pts[i] : NULL;
}

// TrackPath::GetDirection: into the path from end `side`, not normalised.
static void pathDirection(const RaceRoute* r, const RPath* p, int side, float out[2]) {
	int a, b;
	if (side == 0) {
		a = p->first;
		b = p->first + 1;
	} else {
		a = p->end - 1;
		b = p->end - 2;
	}
	const RPoint* pa = pt(r, a);
	const RPoint* pb = pt(r, b);
	if (!pa || !pb) {
		out[0] = out[1] = 0.0f;
		return;
	}
	out[0] = pb->x - pa->x;
	out[1] = pb->y - pa->y;
}

// TrackPath::GetDistance.
static float pathDistance(const RPath* p, float t) {
	float q[4];
	splinePoint(&p->spline, t, 0, q);
	return (q[3] - p->w0) * p->sign + p->d74;
}

static bool canUseOnTrack(const RPath* p, int track) {
	if (p->allowCount == 0 && p->denyCount == 0)
		return true;
	if (p->allowCount) {
		for (int i = 0; i < p->allowCount && i < RP_MAX_TRACKS; i++)
			if (p->tracks[i] == (u16)track)
				return true;
		return false;
	}
	for (int i = 0; i < p->denyCount && i < RP_MAX_TRACKS; i++)
		if (p->tracks[i] == (u16)track)
			return false;
	return true;
}

static bool boxHas(const RPath* p, float slack, const float pos[2]) {
	return p->bbMin[0] - slack <= pos[0] && pos[0] <= p->bbMax[0] + slack &&
	       p->bbMin[1] - slack <= pos[1] && pos[1] <= p->bbMax[1] + slack;
}

static void closestInPath(const RaceRoute* r, const RPath* p, const float pos[2],
		int* best, float* radius) {
	for (int j = p->first; j < p->end; j++) {
		const RPoint* q = pt(r, j);
		if (!q)
			break;
		const float dx = q->x - pos[0], dy = q->y - pos[1];
		const float d2 = dx * dx + dy * dy;
		const float d = d2 > RP_EPS ? sqrtf(d2) : 0.0f;
		// The first point looked at is taken whatever the radius, exactly as
		// retail's `pfVar8 == NULL ||` does.
		if (*best < 0 || d < *radius) {
			*best = j;
			*radius = d;
		}
	}
}

// FindClosestPoint(pos, splinedOnly, radius).
static int findClosestPoint(const RaceRoute* r, const float pos[2], bool splinedOnly,
		float radius) {
	if (radius == -1.0f)
		radius = RP_NO_RADIUS;
	int best = -1;
	if (!splinedOnly) {
		for (int i = 0; i < r->nPaths; i++)
			if (boxHas(&r->paths[i], radius, pos))
				closestInPath(r, &r->paths[i], pos, &best, &radius);
	} else {
		for (int g = 0; g < r->groupCount; g++) {
			const RGroup* G = &r->groups[g];
			for (int k = 0; k < G->count; k++) {
				const RPath* p = &r->paths[r->route[G->start + k]];
				if (p->splined && boxHas(p, radius, pos))
					closestInPath(r, p, pos, &best, &radius);
			}
		}
	}
	return best;
}

// GetDistanceFromLine: the distance from p to segment ab, and which end p is
// nearer (0 = a, 1 = b).
static float distFromLine(const RPoint* a, const RPoint* b, const float p[2], int* which) {
	const float d1x = b->x - a->x, d1y = b->y - a->y;
	float apx = p[0] - a->x, apy = p[1] - a->y;
	const float bpx = p[0] - b->x, bpy = p[1] - b->y;
	const float t1 = d1x * apx + d1y * apy;
	const float t2 = -(d1x * bpx + d1y * bpy);
	if (t1 > 0.0f) {
		if (t2 > 0.0f) {
			*which = t2 < t1 ? 1 : 0;
			const float l2 = d1x * d1x + d1y * d1y;
			const float l = l2 > RP_EPS ? sqrtf(l2) : 0.0f;
			return fabsf(apx * d1y - apy * d1x) / l;
		}
		*which = 1;
		apx = bpx;
		apy = bpy;
	} else {
		*which = 0;
	}
	const float l2 = apx * apx + apy * apy;
	return l2 > RP_EPS ? sqrtf(l2) : 0.0f;
}

// FindClosestAdjacentPoint: the nearer of the two segments either side of
// `*io` within its path; `*io` becomes that segment's nearer end (-1 if the
// path has neither).
static float closestAdjacent(const RaceRoute* r, const float pos[2], int* io) {
	const int i = *io;
	const RPath* p = &r->paths[r->pts[i].path];
	float best = RP_BIG;
	int found = -1;
	int which;
	if (i != p->first && i - 1 >= 0) {
		const float d = distFromLine(&r->pts[i - 1], &r->pts[i], pos, &which);
		if (d < best) {
			found = i - 1 + which;
			best = d;
		}
	}
	if (i != p->end - 1 && i + 1 < r->nPts) {
		const float d = distFromLine(&r->pts[i], &r->pts[i + 1], pos, &which);
		if (d < best) {
			found = i + which;
			best = d;
		}
	}
	*io = found;
	return best;
}

static float compareDistances(const RaceRoute* r, float a, float b) {
	const float d = a - b;
	if (r->loop) {
		if (d < -r->half)
			return d + r->total;
		if (r->half < d)
			return d - r->total;
	}
	return d;
}

float race_route_compare(const RaceRoute* r, float a, float b) {
	return r ? compareDistances(r, a, b) : a - b;
}

float race_route_compare_percent(const RaceRoute* r, float a, float b) {
	const float d = a - b;
	if (r && r->loop) {
		if (d < -50.0f)
			return d + 100.0f;
		if (50.0f < d)
			return d - 100.0f;
	}
	return d;
}

static float neighbourCost(const RaceRoute* r, int from, int to, float adj) {
	float pen = (fabsf(compareDistances(r, r->pts[from].dist, r->pts[to].dist)) -
		RP_NEIGHBOUR_PEN) * RP_NEIGHBOUR_K;
	if (pen < 0.0f)
		pen = 0.0f;
	return adj + 1.0f + pen;
}

// TrackPathManager::UpdatePosition(coord, pos, NULL).
static void managerUpdate(const RaceRoute* r, RaceCoord* c, const float pos[2]) {
	int p = c->point;
	if (p < 0) {
		p = findClosestPoint(r, pos, true, -1.0f);
	} else {
		const float dx = pos[0] - c->last[0], dy = pos[1] - c->last[1];
		const float d2 = dx * dx + dy * dy;
		const float d = d2 > RP_EPS ? sqrtf(d2) : 0.0f;
		if (RP_RESEARCH < d)
			p = findClosestPoint(r, pos, true, -1.0f);
	}
	if (p < 0) {
		c->point = -1;
		c->last[0] = pos[0];
		c->last[1] = pos[1];
		c->dir[0] = 1.0f;
		c->fraction = 0.0f;
		c->dir[1] = 0.0f;
		c->dist = 0.0f;
		c->onPath[0] = pos[0];
		c->onPath[1] = pos[1];
		return;
	}

	float d = closestAdjacent(r, pos, &p);
	if (p < 0)
		return; // a one-point path; retail would read through NULL here
	if (r->pts[p].nearT != 0xFF) {
		if (d + d < (float)r->pts[p].farT) {
			if ((float)r->pts[p].nearT <= d + d) {
				for (int k = 0; k < 3; k++) {
					// Re-read every pass: retail indexes the current best's
					// neighbour list, which changes when one wins.
					const int nb = r->pts[p].nb[k];
					if (nb < 0 || nb >= r->nPts)
						continue;
					if (!r->paths[r->pts[nb].path].splined)
						continue;
					int q = nb;
					const float d2 = closestAdjacent(r, pos, &q);
					if (q < 0 || r->pts[q].nearT == 0xFF)
						continue;
					const float cost = neighbourCost(r, p, q, d2);
					if (cost < d) {
						p = q;
						d = cost;
					}
				}
			}
		} else {
			int q = findClosestPoint(r, pos, true, RP_RESEARCH);
			if (q >= 0 && r->pts[q].nearT != 0xFF) {
				const float d2 = closestAdjacent(r, pos, &q);
				if (q >= 0 && neighbourCost(r, p, q, d2) < d)
					p = q;
			}
		}
	}

	const RPath* path = &r->paths[r->pts[p].path];
	if (!path->splined)
		return;
	float t = c->param;
	if (p != c->point || t == 0.0f) {
		c->point = p;
		t = (float)(p - path->first + 1);
	}
	t = splineClosestParam(&path->spline, t, RP_PARAM_TOL, pos);
	float P[4], D[4];
	splinePoint(&path->spline, t, 0, P);
	splinePoint(&path->spline, t, 1, D);
	float l = sqrtf(D[0] * D[0] + D[1] * D[1]);
	if (l > 0.0f) {
		D[0] /= l;
		D[1] /= l;
	}
	const float dist = pathDistance(path, t);
	if (path->sign < 0.0f) {
		D[0] = -D[0];
		D[1] = -D[1];
	}
	c->param = t;
	c->last[0] = pos[0];
	c->last[1] = pos[1];
	c->dir[0] = D[0];
	c->dir[1] = D[1];
	c->dist = dist;
	c->fraction = dist / r->total;
	c->onPath[0] = P[0];
	c->onPath[1] = P[1];
}

void race_coord_init(const RaceRoute* r, RaceCoord* c, const float pos[2]) {
	memset(c, 0, sizeof(*c));
	c->point = -1;
	c->dir[0] = 1.0f;
	race_coord_update(r, c, pos);
}

void race_coord_update(const RaceRoute* r, RaceCoord* c, const float pos[2]) {
	if (!r)
		return;
	if (c->threshold <= fabsf(pos[0] - c->last[0]) + fabsf(pos[1] - c->last[1]))
		managerUpdate(r, c, pos);
}

// ---------------------------------------------------------------- building

// CreateSpline: the path's points with one control point beyond each end.
// NULL ends extrapolate the path's own first/last segment.
static bool createSpline(RaceRoute* r, RPath* p, const float* sc, const float* ec) {
	const int n = p->count + 2;
	if (p->count < 1)
		return false;
	p->spline.cp = (float (*)[4])malloc(sizeof(float[4]) * (size_t)n);
	if (!p->spline.cp)
		return false;
	p->spline.n = n;
	for (int j = p->first; j < p->end; j++) {
		float* cp = p->spline.cp[j - p->first + 1];
		cp[0] = r->pts[j].x;
		cp[1] = r->pts[j].y;
		cp[2] = 0.0f;
		cp[3] = 0.0f;
	}
	float d[2];
	float* c0 = p->spline.cp[0];
	if (!sc) {
		pathDirection(r, p, 0, d);
		c0[0] = p->ep[0][0] - d[0];
		c0[1] = p->ep[0][1] - d[1];
	} else {
		c0[0] = sc[0];
		c0[1] = sc[1];
	}
	c0[2] = c0[3] = 0.0f;
	float* c1 = p->spline.cp[n - 1];
	if (!ec) {
		pathDirection(r, p, 1, d);
		c1[0] = p->ep[1][0] - d[0];
		c1[1] = p->ep[1][1] - d[1];
	} else {
		c1[0] = ec[0];
		c1[1] = ec[1];
	}
	c1[2] = c1[3] = 0.0f;
	splineCalibrate(&p->spline, 16);
	float q[4];
	splinePoint(&p->spline, 1.0f, 0, q);
	const float w0 = q[3];
	splinePoint(&p->spline, (float)p->count, 0, q);
	p->w0 = w0;
	p->len6c = q[3] - w0;
	p->splined = true;
	return true;
}

static void deleteSpline(RPath* p) {
	free(p->spline.cp);
	p->spline.cp = NULL;
	p->spline.n = 0;
	p->splined = false;
}

// AssignDistances.
static void assignDistances(RaceRoute* r, RPath* p, float d0, float scale, int side) {
	p->side = side;
	const float end = p->len6c * scale + d0;
	p->d80 = d0;
	p->d84 = end;
	if (side == 0) {
		p->d74 = d0;
		p->d78 = end;
		p->sign = scale;
	} else {
		p->d74 = end;
		p->d78 = d0;
		p->sign = -scale;
	}
	for (int j = p->first; j < p->end; j++)
		r->pts[j].dist = pathDistance(p, (float)(j - p->first + 1));
}

// CreateSplines(scale, group, loop). Returns the last path's end distance.
static float createSplines(RaceRoute* r, float scale, int g, bool loop) {
	const RGroup* G = &r->groups[g];
	const int n = G->count;
	// Retail wraps the neighbour index by group 0's count whatever the group
	// (`lwz r9, 0x3c(manager)`); only a looped group 0 ever wraps.
	const int n0 = r->groups[0].count > 0 ? r->groups[0].count : 1;
	float ret = 0.0f;
	for (int i = 0; i < n; i++) {
		const int idx = G->start + i;
		RPath* p = &r->paths[r->route[idx]];
		const int s = r->sides[idx];
		if (p->splined)
			deleteSpline(p);
		bool hasPrev = false, hasNext = false;
		float prevDir[2] = { 0, 0 }, nextDir[2] = { 0, 0 };
		if (i > 0 || loop) {
			hasPrev = true;
			const int j = (i + n0 - 1) % n0 + G->start;
			pathDirection(r, &r->paths[r->route[j]], r->sides[j] == 0 ? 1 : 0, prevDir);
		}
		if (i < n - 1 || loop) {
			hasNext = true;
			const int j = (i + 1) % n0 + G->start;
			pathDirection(r, &r->paths[r->route[j]], r->sides[j], nextDir);
		}
		float sc[2], ec[2];
		const float* scp = NULL;
		const float* ecp = NULL;
		if (hasPrev) {
			if (s == 0) {
				sc[0] = p->ep[0][0] + prevDir[0];
				sc[1] = p->ep[0][1] + prevDir[1];
				scp = sc;
			} else {
				ec[0] = p->ep[1][0] + prevDir[0];
				ec[1] = p->ep[1][1] + prevDir[1];
				ecp = ec;
			}
		}
		if (hasNext) {
			if (s == 0) {
				ec[0] = p->ep[1][0] + nextDir[0];
				ec[1] = p->ep[1][1] + nextDir[1];
				ecp = ec;
			} else {
				sc[0] = p->ep[0][0] + nextDir[0];
				sc[1] = p->ep[0][1] + nextDir[1];
				scp = sc;
			}
		}
		RaceCoord c;
		race_coord_init(r, &c, p->ep[s]);
		const float start = c.dist;
		if (!createSpline(r, p, scp, ecp))
			continue;
		assignDistances(r, p, start, scale, s);
		ret = p->d84;
	}
	return ret;
}

// BuildPathConnectionsRecursive: the shortest chain of paths from `pi` to the
// builder's target within maxDepth, or, with no target, a branch that rejoins
// the splined route further along.
static bool buildRecursive(RaceRoute* r, Builder* B, int pi, int side, int depth) {
	const RPath* p = &r->paths[pi];
	if (B->count == RP_MAX_BUILD || !canUseOnTrack(p, B->track))
		return false;
	for (int k = 0; k < B->count; k++)
		if (B->list[k] == pi && pi != B->target)
			return false;
	for (int z = 0; z < p->zoneCount && z < RP_MAX_ZONES; z++) {
		const int zi = p->zones[z];
		if (zi < r->nBlocked && r->blocked[zi])
			return false;
	}
	if (B->target < 0 && p->splined) {
		if (side == p->side) {
			float d = side == 1 ? p->d78 : p->d74;
			if (d == 0.0f) {
				if (!r->loop)
					return false;
				d = r->total;
			}
			if (B->startDist <= d && RP_REJOIN_RATIO <= (d - B->startDist) / B->len) {
				B->found = d;
				return true;
			}
		}
		return false;
	}
	B->list[B->count] = pi;
	B->sides[B->count] = (u8)side;
	B->count++;
	B->len += p->length;
	if (B->target >= 0 && pi == B->target)
		return true;
	if (depth + 1 <= B->maxDepth && B->len <= B->maxLen) {
		Builder best;
		bool have = false;
		const int other = side == 0 ? 1 : 0; // TrackPath::GetOtherSide
		for (int c = 0; c < p->nConn[other] && c < 4; c++) {
			Builder C = *B;
			if (buildRecursive(r, &C, p->connPath[other][c], p->connSide[other][c], depth + 1) &&
					(!have || C.len < best.len)) {
				best = C;
				have = true;
				B->maxLen = C.len;
				best.maxLen = C.len;
			}
		}
		if (have) {
			*B = best;
			return true;
		}
	}
	return false;
}

static bool appendRoute(RaceRoute* r, int pi, u8 side) {
	if (r->routeCount >= RP_MAX_ROUTE)
		return false;
	r->route[r->routeCount] = pi;
	r->sides[r->routeCount] = side;
	r->routeCount++;
	return true;
}

// BuildConnectionTree(group, reverse).
static bool buildConnectionTree(RaceRoute* r, bool reverse, void (*log)(const char*)) {
	char line[160];
	// ClearConnectionTree.
	r->total = r->half = 1.0f;
	r->routeCount = 0;
	r->groupCount = 0;
	memset(r->groups, 0, sizeof(r->groups));
	r->groups[0].start = 0;

	const int n = r->wpCount;
	bool first = true;
	for (int i = 0; i < n - 1; i++) {
		const float* a = reverse ? r->wp[n - 1 - i] : r->wp[i];
		const float* b = reverse ? r->wp[n - 2 - i] : r->wp[i + 1];
		const int pa = findClosestPoint(r, a, false, -1.0f);
		if (pa < 0)
			return false;
		const int startPath = r->pts[pa].path;
		int side;
		if (!first) {
			side = r->sides[r->routeCount - 1];
		} else {
			first = false;
			float wd[2] = { b[0] - a[0], b[1] - a[1] };
			float l = sqrtf(wd[0] * wd[0] + wd[1] * wd[1]);
			if (l > 0.0f) { wd[0] /= l; wd[1] /= l; }
			const RPath* sp = &r->paths[startPath];
			float pd[2];
			if (pa < sp->end - 1) {
				pd[0] = r->pts[pa + 1].x - r->pts[pa].x;
				pd[1] = r->pts[pa + 1].y - r->pts[pa].y;
			} else {
				pd[0] = r->pts[pa].x - r->pts[pa - 1].x;
				pd[1] = r->pts[pa].y - r->pts[pa - 1].y;
			}
			l = sqrtf(pd[0] * pd[0] + pd[1] * pd[1]);
			if (l > 0.0f) { pd[0] /= l; pd[1] /= l; }
			side = 0.0f < wd[0] * pd[0] + wd[1] * pd[1] ? 0 : 1;
		}
		const int pb = findClosestPoint(r, b, false, -1.0f);
		if (pb < 0)
			return false;
		Builder B;
		memset(&B, 0, sizeof(B));
		B.maxLen = RP_BIG;
		B.maxDepth = 10;
		B.target = r->pts[pb].path;
		B.track = r->trackId;
		if (!buildRecursive(r, &B, startPath, side, 0)) {
			if (log) {
				snprintf(line, sizeof(line), "race: route %d: no connection %s -> %s (waypoint %d)",
					r->trackId, r->paths[startPath].name, r->paths[B.target].name, i);
				log(line);
			}
			return false;
		}
		for (int k = 0; k < B.count; k++) {
			const int q = B.list[k];
			if (r->routeCount > 0 && q == r->route[r->routeCount - 1])
				continue;
			if (r->loop && r->routeCount > 0 && q == r->route[r->groups[0].start])
				continue;
			for (int m = r->groups[0].start; m < r->routeCount; m++)
				if (r->route[m] == q)
					return false;
			if (!appendRoute(r, q, B.sides[k]))
				return false;
		}
	}
	r->groups[0].count = r->routeCount - r->groups[0].start;
	r->groupCount = 1;
	const float total = createSplines(r, 1.0f, 0, r->loop);
	r->groups[0].scale = 1.0f;
	r->groups[0].startDist = 0.0f;
	r->groups[0].span = total;
	r->groups[0].endDist = total;
	r->half = total * 0.5f;
	r->total = total;

	if (!r->branches)
		return true;
	// The branch groups: every connection off the route that is not on it,
	// searched for a way back onto it further along. The group list grows
	// while it is walked, so branches off branches are found too.
	for (int g = 0; g < r->groupCount; g++) {
		const RGroup G = r->groups[g];
		for (int k = 0; k < G.count; k++) {
			const int idx = G.start + k;
			const RPath* p = &r->paths[r->route[idx]];
			const int other = r->sides[idx] == 0 ? 1 : 0;
			if (r->groupCount >= RP_MAX_GROUPS)
				continue;
			int c = 0;
			do {
				if (c < p->nConn[other]) {
					const int cpi = p->connPath[other][c];
					const int cside = p->connSide[other][c];
					if (!r->paths[cpi].splined) {
						Builder B;
						memset(&B, 0, sizeof(B));
						B.maxDepth = 15;
						B.maxLen = RP_BIG;
						B.track = r->trackId;
						B.target = -1;
						RaceCoord rc;
						race_coord_init(r, &rc, r->paths[cpi].ep[cside]);
						B.startDist = rc.dist;
						if (buildRecursive(r, &B, cpi, cside, 0)) {
							const int gi = r->groupCount;
							RGroup* NG = &r->groups[gi];
							NG->start = r->routeCount;
							for (int m = 0; m < B.count; m++) {
								if (gi == RP_MAX_GROUPS || r->routeCount == RP_MAX_ROUTE)
									break;
								appendRoute(r, B.list[m], B.sides[m]);
							}
							NG->count = r->routeCount - NG->start;
							r->groupCount = gi + 1;
							const float t = createSplines(r, 1.0f, gi, false);
							NG->span = t - B.startDist;
							NG->startDist = B.startDist;
							NG->endDist = B.found;
							NG->scale = (B.found - B.startDist) / NG->span;
							createSplines(r, NG->scale, gi, false);
						}
					}
				}
				c++;
			} while (c < 4 && r->groupCount < RP_MAX_GROUPS);
		}
	}
	return true;
}

// ---------------------------------------------------------------- loading

typedef struct {
	long off;
	u32 size;
} ChunkRef;

static bool readAt(FILE* f, long off, void* dst, size_t n) {
	return fseek(f, off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
}

static void* readChunk(FILE* f, ChunkRef c) {
	if (c.off < 0 || c.size == 0)
		return NULL;
	void* buf = malloc(c.size);
	if (buf && !readAt(f, c.off, buf, c.size)) {
		free(buf);
		buf = NULL;
	}
	return buf;
}

RaceRoute* race_route_open(const char* path, int trackId, bool reverse,
		void (*log)(const char* msg)) {
	char line[200];
	FILE* f = fopen(path, "rb");
	if (!f) {
		if (log) {
			snprintf(line, sizeof(line), "race: no %s -- no route for track %d", path, trackId);
			log(line);
		}
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long fileSize = ftell(f);

	ChunkRef cPts = { -1, 0 }, cPaths = { -1, 0 }, cGroups = { -1, 0 }, cBlocks = { -1, 0 };
	long off = 0;
	while (off + 8 <= fileSize) {
		u8 hdr[8];
		if (!readAt(f, off, hdr, 8))
			break;
		const u32 type = rd32le(hdr), size = rd32le(hdr + 4);
		if ((long)size > fileSize - (off + 8))
			break;
		if (type == RP_CONTAINER) {
			long sub = off + 8;
			const long end = sub + (long)size;
			while (sub + 8 <= end) {
				u8 sh[8];
				if (!readAt(f, sub, sh, 8))
					break;
				const u32 st = rd32le(sh), ss = rd32le(sh + 4);
				if ((long)ss > end - (sub + 8))
					break;
				const ChunkRef c = { sub + 8, ss };
				if (st == RP_POINTS) cPts = c;
				else if (st == RP_PATHS) cPaths = c;
				else if (st == RP_GROUPS) cGroups = c;
				else if (st == RP_BLOCKS) cBlocks = c;
				sub += 8 + (long)ss;
			}
		}
		off += 8 + (long)size;
	}

	u8* pts = (u8*)readChunk(f, cPts);
	u8* paths = (u8*)readChunk(f, cPaths);
	u8* groups = (u8*)readChunk(f, cGroups);
	u8* blocks = (u8*)readChunk(f, cBlocks);
	fclose(f);

	RaceRoute* r = NULL;
	const char* why = NULL;
	if (!pts || !paths || !groups) {
		why = "missing a points/paths/groups chunk";
		goto done;
	}
	r = (RaceRoute*)calloc(1, sizeof(RaceRoute));
	if (!r) {
		why = "out of memory";
		goto done;
	}
	r->trackId = trackId;
	r->nPts = (int)(cPts.size / RP_POINT_BYTES);
	r->nPaths = (int)(cPaths.size / RP_PATH_BYTES);
	r->pts = (RPoint*)calloc((size_t)r->nPts, sizeof(RPoint));
	r->paths = (RPath*)calloc((size_t)r->nPaths, sizeof(RPath));
	if (!r->pts || !r->paths) {
		why = "out of memory";
		goto done;
	}
	for (int i = 0; i < r->nPts; i++) {
		const u8* q = pts + i * RP_POINT_BYTES;
		RPoint* o = &r->pts[i];
		o->x = rdf(q);
		o->y = rdf(q + 4);
		o->path = rd16le(q + 8);
		o->nearT = q[0xA];
		o->farT = q[0xB];
		for (int k = 0; k < 3; k++)
			o->nb[k] = (s16)rd16le(q + 0xC + k * 2);
		if (o->path >= r->nPaths) {
			why = "a point names a path past the table";
			goto done;
		}
	}
	for (int i = 0; i < r->nPaths; i++) {
		const u8* q = paths + i * RP_PATH_BYTES;
		RPath* o = &r->paths[i];
		memcpy(o->name, q, 16);
		o->name[16] = 0;
		o->first = (s16)rd16le(q + 0x1C);
		o->end = (s16)rd16le(q + 0x1E);
		o->count = (s16)rd16le(q + 0x20);
		if (o->first < 0 || o->end > r->nPts || o->first >= o->end) {
			why = "a path's point range is outside the point table";
			goto done;
		}
		for (int s = 0; s < 2; s++) {
			o->nConn[s] = q[0x22 + s] > 4 ? 4 : q[0x22 + s];
			for (int k = 0; k < o->nConn[s]; k++) {
				o->connPath[s][k] = rd16le(q + 0x24 + s * 0x20 + k * 8);
				o->connSide[s][k] = q[0x24 + s * 0x20 + k * 8 + 2];
				if (o->connPath[s][k] >= r->nPaths || o->connSide[s][k] > 1) {
					why = "a connection names a path past the table";
					goto done;
				}
			}
		}
		o->length = rdf(q + 0x64);
		for (int k = 0; k < 2; k++) {
			o->ep[0][k] = rdf(q + 0x88 + k * 4);
			o->ep[1][k] = rdf(q + 0x90 + k * 4);
			o->bbMin[k] = rdf(q + 0x98 + k * 4);
			o->bbMax[k] = rdf(q + 0xA0 + k * 4);
		}
		o->allowCount = q[0xAC];
		o->denyCount = q[0xAD];
		for (int k = 0; k < RP_MAX_TRACKS; k++)
			o->tracks[k] = rd16le(q + 0xAE + k * 2);
		o->zoneCount = (s16)rd16le(q + 0xBC);
		for (int k = 0; k < RP_MAX_ZONES; k++)
			o->zones[k] = rd16le(q + 0xBE + k * 2);
	}
	if (blocks) {
		r->nBlocked = (int)(cBlocks.size / RP_BLOCK_BYTES);
		r->blocked = (u8*)malloc((size_t)(r->nBlocked > 0 ? r->nBlocked : 1));
		if (!r->blocked) {
			why = "out of memory";
			goto done;
		}
		// TrackLoader::InitTopologyAndSceneryGroups (0x80196CB8), the
		// TrackPathManager half, which FinishedLoadingPart1 runs before
		// BuildConnectionTree: DisableAllBarriers (0x80200260) clears every
		// gate, then EnableBarriers (0x8020029C) closes each one whose ids hold
		// bStringHash("BARRIERS_<track>") or ("PLAYER_BARRIERS_<track>"). The
		// file's own byte is 0 everywhere, so without this the builder routes
		// down the side roads the race closes off (TASKS.md #261).
		char name[32];
		u32 want[2];
		snprintf(name, sizeof(name), "BARRIERS_%d", trackId);
		want[0] = stringHash(name);
		snprintf(name, sizeof(name), "PLAYER_BARRIERS_%d", trackId);
		want[1] = stringHash(name);
		int closed = 0;
		for (int i = 0; i < r->nBlocked; i++) {
			const u8* g = blocks + i * RP_BLOCK_BYTES;
			const int n = (s8)g[0x11];
			r->blocked[i] = 0;
			for (int k = 0; k < n && k < 4 && !r->blocked[i]; k++) {
				const u32 id = rd32le(g + 0x14 + k * 4);
				if (id == want[0] || id == want[1])
					r->blocked[i] = 1;
			}
			closed += r->blocked[i];
		}
		r->nClosed = closed;
	}
	// GetGroupByTrack.
	{
		const int ng = (int)(cGroups.size / RP_GROUP_BYTES);
		const u8* g = NULL;
		for (int i = 0; i < ng; i++)
			if (rd16le(groups + i * RP_GROUP_BYTES) == (u16)trackId) {
				g = groups + i * RP_GROUP_BYTES;
				break;
			}
		if (!g) {
			why = "no route group for the track";
			goto done;
		}
		r->wpCount = (s8)g[2];
		if (r->wpCount < 2 || r->wpCount > RP_MAX_WAYPOINTS) {
			why = "the route group has no usable waypoint list";
			goto done;
		}
		r->loop = g[3] != 0;
		r->branches = g[4] != 0;
		r->f20 = (float)g[5];
		for (int k = 0; k < r->wpCount; k++) {
			r->wp[k][0] = rdf(g + 8 + k * 8);
			r->wp[k][1] = rdf(g + 12 + k * 8);
		}
	}
	if (!buildConnectionTree(r, reverse, log)) {
		why = "BuildConnectionTree failed";
		goto done;
	}

done:
	free(pts);
	free(paths);
	free(groups);
	free(blocks);
	if (why) {
		if (log) {
			snprintf(line, sizeof(line), "race: %s: track %d -- %s", path, trackId, why);
			log(line);
		}
		race_route_close(r);
		return NULL;
	}
	if (log) {
		int nb = 0;
		for (int g = 1; g < r->groupCount; g++)
			nb += r->groups[g].count;
		snprintf(line, sizeof(line), "race: route %d%s: %d paths, %.1f m, %s, %d branch group(s) "
			"of %d paths, %d of %d gates closed", trackId, reverse ? " reversed" : "",
			r->groups[0].count, r->total, r->loop ? "loop" : "point to point",
			r->groupCount - 1, nb, r->nClosed, r->nBlocked);
		log(line);
	}
	return r;
}

void race_route_close(RaceRoute* r) {
	if (!r)
		return;
	if (r->paths)
		for (int i = 0; i < r->nPaths; i++)
			free(r->paths[i].spline.cp);
	free(r->paths);
	free(r->pts);
	free(r->blocked);
	free(r);
}

// ---------------------------------------------------------------------------
// Zones, and the off-course handling built on them (TASKS.md #247).

// DetectWrongWay's own numbers: a 110-degree cone (0x4E37), 10 degrees of
// hysteresis once the cue is already up (0x71C) so it does not flicker on its
// own edge, and the 10 mph (DAT_80339D44, with DAT_80339D48 = 2.237) below
// which only the car's heading counts and not where it is sliding.
#define RP_WW_CONE    0x4E37
#define RP_WW_HYST    0x071C
#define RP_WW_MIN_MPH 10.0f
#define RP_MPS_TO_MPH 2.237f

u16 race_bangle(float x, float y) {
	float turn = atan2f(y, x) * (65536.0f / (2.0f * (float)M_PI));
	turn = fmodf(turn, 65536.0f);
	if (turn < 0.0f)
		turn += 65536.0f;
	return (u16)(int)(turn + 0.5f);
}

const TrackPathZone* race_find_wrong_way_zone(const TrackPaths* tp,
		const float pos[2]) {
	if (!tp || !pos)
		return NULL;
	for (u32 t = TPZ_NO_WRONG_WAY; t <= TPZ_RESET_TO_POINT; t++) {
		const TrackPathZone* z = trackpath_find(tp, pos[0], pos[1], t, NULL);
		if (z)
			return z;
	}
	return NULL;
}

// Car::MaybeResetToTrackPathZone (0x8022120C). A zone whose entry Z is 0 takes
// the car at any height; one with a Z takes it only below that, and only while
// the car has topology under it (Car +0x5BC) -- which is how a road above the
// zone is let through. A RESET_TO_POINT puts the car on the zone's own entry
// point and facing; anything else asks the car for the reset it does itself.
//
// Plus the one test only the PC build has (SPEED2.EXE 0x5FBE00): a
// RESET_TO_POINT whose data handle is bStringHash("PCDEMO") is let go. That is
// the PC demo's fence -- the shipped data's only RESET_TO_POINT, a polygon over
// the whole south of Bayview that throws the car back to -1079.6,288.0 -- and
// the full game carries it in every Paths file but never acts on it. The GC
// code has no such test; this port reads the PC files, so it needs the PC's
// (TASKS.md #261: it was what wrecked every Quick Race on route 4014).
#define TPZ_HANDLE_PCDEMO 0x728558D7u // bStringHash("PCDEMO")

static bool maybeResetToZone(const TrackPathZone* z, const RaceCarPose* car,
		RaceWrongWay* out) {
	bool ok = z->entryZ == 0.0f;
	if (!ok) {
		if (car->topology)
			ok = car->pos[2] < z->entryZ;
		if (!ok)
			return false;
	}
	if (z->type == TPZ_RESET_TO_POINT && z->unk30[0] == TPZ_HANDLE_PCDEMO)
		return false;
	if (z->type == TPZ_RESET_TO_POINT) {
		out->reset = RACE_RESET_TO_POINT;
		out->resetPos[0] = z->entry[0];
		out->resetPos[1] = z->entry[1];
		out->resetAngle = race_bangle(z->dir[0], z->dir[1]);
	} else {
		out->reset = RACE_RESET_LATER;
	}
	return true;
}

bool race_detect_wrong_way(const RaceRoute* r, const TrackPaths* tp,
		const RaceCoord* c, const RaceCarPose* car, bool already,
		RaceWrongWay* out) {
	out->inZone = false;
	out->reset = RACE_RESET_NONE;
	out->resetPos[0] = out->resetPos[1] = 0.0f;
	out->resetAngle = 0;
	// Car +0x8E0 is the coordinate's point: no point, no route to be wrong on.
	if (!r || !c || !car || c->point < 0)
		return false;

	u16 routeAngle = race_bangle(c->dir[0], c->dir[1]);
	const TrackPathZone* z = race_find_wrong_way_zone(tp, c->last);
	if (z) {
		if (z->type == TPZ_NO_WRONG_WAY)
			return false;
		if (z->type == TPZ_WRONG_WAY) {
			// The zone names the direction instead of the route, which is how
			// a two-way street inside the course is told from going backwards.
			routeAngle = race_bangle(z->dir[0], z->dir[1]);
			out->inZone = true;
		} else if (maybeResetToZone(z, car, out)) {
			return false;
		}
	}

	u16 diff = (u16)(routeAngle - car->heading);
	if (diff > 0x8000)
		diff = (u16)(-(int)diff);
	if (already)
		diff = (u16)(diff + RP_WW_HYST);
	if (diff <= RP_WW_CONE)
		return false;

	// Under 10 mph the heading is the whole answer: a car being spun round has
	// no velocity worth reading, so the cue holds but does not start.
	if (fabsf(car->speed * RP_MPS_TO_MPH) <= RP_WW_MIN_MPH)
		return already;

	u16 vdiff = (u16)(routeAngle - race_bangle(car->vel[0], car->vel[1]));
	if (vdiff > 0x8000)
		vdiff = (u16)(-(int)vdiff);
	if (already)
		vdiff = (u16)(vdiff + RP_WW_HYST);
	if (vdiff <= RP_WW_CONE)
		return false;
	// Airborne, the cue does not start -- but once up it stays up.
	if (!already && car->wheelsDown < 4)
		return false;
	return true;
}

// QuickSpline::GetParam (0x80193214): the parameter whose arc length is
// `arc`, found by regula falsi -- at most 40 passes, stopping once the arc
// length is within 0.001 m, both the engine's own numbers (0x8032DA78).
//
// Retail brackets the search with the spline's stored parameter range, which
// for endpoint type 2 runs outside the real control points. Here the bracket
// is [1, count]: that is the stretch createSpline() calibrated and the only
// one a distance along the route can land in, and it keeps the search off the
// extrapolated ghosts whose arc length was never measured.
static float splineParam(const Spline* s, float lo, float hi, float arc) {
	float q[4];
	if (hi <= lo)
		return lo;
	splinePoint(s, lo, 0, q);
	float wLo = q[3];
	splinePoint(s, hi, 0, q);
	float wHi = q[3];
	if (wHi == wLo || arc <= wLo)
		return lo;
	if (arc >= wHi)
		return hi;
	float t = lo;
	for (int i = 0; i < 40; i++) {
		const float u = (arc - wLo) / (wHi - wLo);
		t = lo * (1.0f - u) + hi * u;
		splinePoint(s, t, 0, q);
		const float w = q[3];
		if (fabsf(arc - w) < 0.001f)
			break;
		if (arc < w) {
			hi = t;
			wHi = w;
		}
		if (arc > w) {
			lo = t;
			wLo = w;
		}
	}
	return t;
}

// TrackPathManager::FindClosestPath(distance, group) 0x80202D68, over group 0:
// the path whose [d80, d84] the distance falls in, else the nearest one to it.
static const RPath* pathAtDistance(const RaceRoute* r, float dist) {
	const RGroup* G = &r->groups[0];
	const RPath* best = NULL;
	float bestCost = 1.0e7f; // DAT_803381BC
	for (int i = 0; i < G->count; i++) {
		const RPath* p = &r->paths[r->route[G->start + i]];
		if (!p->splined)
			continue;
		float cost = p->d80 - dist;
		if (cost <= 0.0f)
			cost = dist - p->d84;
		if (!best || cost < bestCost) {
			best = p;
			bestCost = cost;
		}
	}
	return best;
}

// TrackPathManager::GetPosition (0x80202F94) with the path FindClosestPath
// picks and the parameter TrackPath::GetSplineParam (0x801FFFFC) inverts out
// of it: the route's own spline `dist` metres along, pushed `lateral` metres
// to the right of travel.
//
// Retail offsets by the raw spline derivative; this offsets by the travel
// direction, which is the same vector on a path driven forwards and its
// negation on one driven backwards -- the same flip managerUpdate() already
// applies to RaceCoord::dir, so "right" means the same thing on both.
bool race_route_sample(const RaceRoute* r, float dist, float lateral,
		float outPos[2], float outDir[2]) {
	if (!r || r->groupCount <= 0)
		return false;
	if (r->loop && r->total > 0.0f) {
		dist = fmodf(dist, r->total);
		if (dist < 0.0f)
			dist += r->total;
	}
	const RPath* p = pathAtDistance(r, dist);
	if (!p)
		return false;
	const float arc = (dist - p->d74) / p->sign + p->w0;
	const float t = splineParam(&p->spline, 1.0f, (float)p->count, arc);
	float P[4], D[4];
	splinePoint(&p->spline, t, 0, P);
	splinePoint(&p->spline, t, 1, D);
	float l = sqrtf(D[0] * D[0] + D[1] * D[1]);
	if (l > 0.0f) {
		D[0] /= l;
		D[1] /= l;
	}
	if (p->sign < 0.0f) {
		D[0] = -D[0];
		D[1] = -D[1];
	}
	if (outDir) {
		outDir[0] = D[0];
		outDir[1] = D[1];
	}
	if (outPos) {
		outPos[0] = P[0] + D[1] * lateral;
		outPos[1] = P[1] - D[0] * lateral;
	}
	return true;
}

float race_route_length(const RaceRoute* r) { return r ? r->total : 0.0f; }
bool race_route_loops(const RaceRoute* r) { return r && r->loop; }
int race_route_path_count(const RaceRoute* r) { return r ? r->groups[0].count : 0; }
int race_route_branch_count(const RaceRoute* r) { return r ? r->groupCount - 1 : 0; }

// ---------------------------------------------------------------- markers

struct RaceMarkers {
	RaceMarker* recs;
	int count;
};

u32 race_string_hash(const char* s) {
	u32 h = 0xFFFFFFFFu;
	while (*s)
		h = h * 33u + (u8)*s++;
	return h;
}

RaceMarkers* race_markers_open(const char* path, void (*log)(const char* msg)) {
	char line[200];
	FILE* f = fopen(path, "rb");
	if (!f) {
		if (log) {
			snprintf(line, sizeof(line), "race: no %s -- no grid or start lines", path);
			log(line);
		}
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long fileSize = ftell(f);
	u8* buf = (fileSize > 16) ? (u8*)malloc((size_t)fileSize) : NULL;
	if (!buf || !readAt(f, 0, buf, (size_t)fileSize)) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	RaceMarkers* m = NULL;
	if (rd32le(buf) == RP_MARKERS) {
		const u32 size = rd32le(buf + 4);
		// LoaderTrackPositionMarkers: the records start at the chunk's data
		// rounded up to 16, i.e. (chunk + 0x17) & ~0xF -- offset 0x10 here.
		const long start = 0x10;
		long n = ((long)size - (start - 8)) / RP_MARKER_BYTES;
		if (start + n * RP_MARKER_BYTES > fileSize)
			n = (fileSize - start) / RP_MARKER_BYTES;
		m = (RaceMarkers*)calloc(1, sizeof(RaceMarkers));
		if (m && n > 0) {
			m->recs = (RaceMarker*)calloc((size_t)n, sizeof(RaceMarker));
			if (m->recs) {
				for (long i = 0; i < n; i++) {
					const u8* q = buf + start + i * RP_MARKER_BYTES;
					RaceMarker* o = &m->recs[i];
					o->nameHash = rd32le(q + 8);
					o->slot = (s32)rd32le(q + 0xC);
					o->pos[0] = rdf(q + 0x10);
					o->pos[1] = rdf(q + 0x14);
					o->pos[2] = rdf(q + 0x18);
					o->angle = rd16le(q + 0x20);
					o->track = (s32)rd32le(q + 0x24);
				}
				m->count = (int)n;
			}
		}
	}
	free(buf);
	if (log) {
		snprintf(line, sizeof(line), "race: %d track position markers", m ? m->count : 0);
		log(line);
	}
	return m;
}

void race_markers_close(RaceMarkers* m) {
	if (!m)
		return;
	free(m->recs);
	free(m);
}

int race_markers_count(const RaceMarkers* m, int track, u32 nameHash) {
	int n = 0;
	if (!m)
		return 0;
	for (int i = 0; i < m->count; i++)
		if (m->recs[i].nameHash == nameHash && (track == 0 || m->recs[i].track == track))
			n++;
	return n;
}

const RaceMarker* race_markers_get(const RaceMarkers* m, int track, u32 nameHash, int index) {
	int n = 0;
	if (!m)
		return NULL;
	for (int i = 0; i < m->count; i++)
		if (m->recs[i].nameHash == nameHash && (track == 0 || m->recs[i].track == track)) {
			if (n == index)
				return &m->recs[i];
			n++;
		}
	return NULL;
}

bool race_grid_slot(const RaceMarkers* m, int track, bool reverse, int pos,
		float outPos[3], u16* outAngle) {
	const u32 h = race_string_hash(reverse ? "BackwardStartingPosition"
		: "ForwardStartingPosition");
	const int n = race_markers_count(m, track, h);
	for (int i = 0; i < n; i++) {
		const RaceMarker* k = race_markers_get(m, track, h, i);
		if (k && k->slot == pos - 1) {
			outPos[0] = k->pos[0];
			outPos[1] = k->pos[1];
			outPos[2] = k->pos[2];
			*outAngle = k->angle;
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------- the race

s32 race_round_time(s32 ticks, float step) {
	// Timer::GetSecondsRounded then RoundTime: to the nearest step, half up,
	// then back to ticks through a ceil and a +0.5 truncation.
	const float q = (float)ticks * 0.00025f / step;
	float fl = (float)(int)q;
	if (fl > q)
		fl -= 1.0f;
	if (q - fl >= 0.5f)
		fl += 1.0f;
	const float v = fl * step * 4000.0f;
	float c = (float)(int)v;
	if (c < v)
		c += 1.0f;
	const float sec = c / 4000.0f;
	return (s32)(sec * 4000.0f + 0.5f);
}

static bool timerSet(s32 t) {
	return t != 0 && t != 0x7FFFFFFF;
}

// AddBoundaryPosition: a named marker's route percent, or the default when the
// track has none (a negative default adds nothing). Returns its index or -1.
static int addBoundary(Race* race, const RaceMarkers* mk, float pct, const char* name,
		int slot, float outPos[3]) {
	outPos[0] = outPos[1] = outPos[2] = 0.0f;
	if (100.0f < pct)
		pct -= 100.0f;
	const u32 h = race_string_hash(name);
	const int track = race->params.track;
	const RaceMarker* m = race_markers_get(mk, track, h, 0);
	if (slot != 0) {
		const int n = race_markers_count(mk, track, h);
		for (int i = 0; i < n; i++) {
			const RaceMarker* k = race_markers_get(mk, track, h, i);
			if (k && k->slot == slot)
				m = k;
		}
	}
	if (race->numBoundaries >= RACE_MAX_BOUNDARIES)
		return -1;
	if (!m) {
		if (pct < 0.0f)
			return -1;
		race->boundary[race->numBoundaries] = pct;
		return race->numBoundaries++;
	}
	RaceCoord c;
	race_coord_init(race->route, &c, m->pos);
	outPos[0] = m->pos[0];
	outPos[1] = m->pos[1];
	outPos[2] = m->pos[2];
	race->boundary[race->numBoundaries] = c.fraction * 100.0f;
	return race->numBoundaries++;
}

// FigureOutBoundaries.
static void figureOutBoundaries(Race* race, const RaceMarkers* mk) {
	race->numBoundaries = 0;
	if (race->params.gameMode == 7 || race->params.gameMode == 8)
		return;
	const bool p2p = race->params.pointToPoint;
	float pos[3];
	int s1, s2;
	if (!race->params.reverse) {
		race->startIdx = (s8)addBoundary(race, mk, 5.0f, "ForwardStartLine", 0, pos);
		memcpy(race->startLine, pos, sizeof(pos));
		const float st = race->boundary[race->startIdx];
		s1 = addBoundary(race, mk, st + 33.0f, "SplitTimeBoundary1", 0, pos);
		s2 = addBoundary(race, mk, st + 66.0f, "SplitTimeBoundary2", 0, pos);
		addBoundary(race, mk, -1.0f, "SplitTimeBoundary3", 0, pos);
		if (p2p) {
			race->finishIdx = (s8)addBoundary(race, mk, st + 95.0f, "ForwardFinishLine",
				race->params.finishSlot, pos);
			memcpy(race->finishLine, pos, sizeof(pos));
		}
	} else {
		race->startIdx = (s8)addBoundary(race, mk, 5.0f, "BackwardStartLine",
			race->params.finishSlot, pos);
		memcpy(race->startLine, pos, sizeof(pos));
		const float st = race->boundary[race->startIdx];
		addBoundary(race, mk, -1.0f, "SplitTimeBoundary3", 0, pos);
		s1 = addBoundary(race, mk, st + 33.0f, "SplitTimeBoundary2", 0, pos);
		s2 = addBoundary(race, mk, st + 66.0f, "SplitTimeBoundary1", 0, pos);
		if (p2p) {
			race->finishIdx = (s8)addBoundary(race, mk, st + 95.0f, "BackwardFinishLine", 0, pos);
			memcpy(race->finishLine, pos, sizeof(pos));
		}
	}
	if (!p2p) {
		race->finishIdx = race->startIdx;
		memcpy(race->finishLine, race->startLine, sizeof(race->finishLine));
	}
	if (p2p) {
		// Splits still at their defaults are respread over the real start ->
		// finish stretch.
		const float st = race->boundary[race->startIdx];
		float d1 = st + 33.0f, d2 = st + 66.0f;
		if (100.0f < d1)
			d1 -= 100.0f;
		if (100.0f < d2)
			d2 -= 100.0f;
		const float fin = race->boundary[race->finishIdx];
		if (s1 >= 0 && race->boundary[s1] == d1)
			race->boundary[s1] = (fin - st) * 0.33f + st;
		if (s2 >= 0 && race->boundary[s2] == d2)
			race->boundary[s2] = (fin - race->boundary[race->startIdx]) * 0.66f +
				race->boundary[race->startIdx];
	}
}

// GetLapNumberExact.
static float lapNumberExact(const Race* race, float pct) {
	const float s = race->boundary[race->startIdx];
	const float f = race->boundary[race->finishIdx];
	if (f == s) {
		float d = race_route_compare_percent(race->route, pct, s);
		if (d < 0.0f)
			d += 100.0f;
		return d / 100.0f;
	}
	float d = (pct - s) / (f - s);
	if (1.0f < d)
		d -= 1.0f;
	return d;
}

// CheckBoundaryCrossing: crossed `b` (route percent) since last frame, which
// way, and how many ticks ago, interpolated from the speed.
static bool checkCrossing(const Race* race, const RacingCar* car, float cur,
		float b, float speed, bool* forward, s32* agoTicks) {
	const float dc = race_route_compare_percent(race->route, cur, b);
	const float dp = race_route_compare_percent(race->route, car->prevPct, b);
	const float metres = dc * race_route_length(race->route) / 100.0f;
	*forward = false;
	*agoTicks = 0;
	if (25.0f < fabsf(dc))
		return false;
	if (dp < 0.0f && 0.0f <= dc) {
		*forward = true;
	} else if (0.0f <= dp && dc < 0.0f) {
		*forward = false;
	} else {
		return false;
	}
	float secs = 0.0f;
	if (1.0f < speed)
		secs = metres / speed;
	*agoTicks = (s32)(secs * 4000.0f + 0.5f);
	return true;
}

// RacingCar::UpdateLapCounts.
static void updateLapCounts(Race* race, RacingCar* car, float speed) {
	car->newProgress = false;
	car->newLap = false;
	const float pct = car->coord.fraction * 100.0f;
	for (int i = 0; i < race->numBoundaries; i++) {
		bool forward;
		s32 ago;
		if (!checkCrossing(race, car, pct, race->boundary[i], speed, &forward, &ago))
			continue;
		if (!forward) {
			if (i == race->finishIdx) {
				car->lap--;
				if (car->lap < car->maxLap - 1)
					car->lap = (s8)(car->maxLap - 1);
			}
			continue;
		}
		if (i == race->finishIdx) {
			car->lap++;
			if (car->maxLap < car->lap) {
				car->maxLap = car->lap;
				car->newLap = true;
			}
		}
		car->progress = (s16)(car->lap * 5 + i);
		if (car->maxProgress < car->progress) {
			car->maxProgress = car->progress;
			car->newProgress = true;
			const s32 crossed = race_round_time(race->now - ago, 0.01f);
			if (timerSet(car->crossTime[0])) {
				car->prevSplit[i] = car->split[i];
				car->prevBest[i] = car->bestSplit[i];
				car->split[i] = crossed - car->crossTime[0];
				if (car->split[i] < car->bestSplit[i])
					car->bestSplit[i] = car->split[i];
				if (i == race->finishIdx && (u8)car->lap < RACE_MAX_LAPS) {
					car->lapTime[car->lap] = car->split[i];
					car->lapTotal[car->lap] = car->lapTime[car->lap];
					if (car->lap > 0)
						car->lapTotal[car->lap] += car->lapTotal[car->lap - 1];
				}
			}
			car->crossTime[i] = crossed;
		}
	}
	car->lapExact = (float)car->lap + lapNumberExact(race, pct);
	car->prevPct = pct;
}

// RacingCar::FakeStartlineCrossing, at GO.
static void fakeStartlineCrossing(Race* race) {
	// Every car on the grid, not just the player's: RaceCoordinator calls it
	// on each RacingCar it owns, and an AI car that never had one would count
	// its first finish-line crossing as a lap it had already done.
	for (int c = 0; c <= race->aiCount; c++) {
		RacingCar* car = c == 0 ? &race->car : &race->ai[c - 1];
		car->crossTime[0] = race_round_time(race->now, 0.01f);
		car->maxProgress = 0;
		if (race->params.pointToPoint) {
			car->progress = 0;
			car->lap = 0;
		}
	}
}

// PORT (#488): TheRaceParameters gameMode 7, an outrun, is a race with no
// route: the LAN Outrun's spawns (tracks 4091..4094) have no Paths file, and
// FigureOutBoundaries already gives mode 7 no lines. Its cars still have
// RacingCars, a clock and a finish (race_retire_at()); nothing measures them
// along a road.
static bool raceLive(const Race* race) {
	return race->route || race->params.gameMode == RACE_MODE_OUTRUN;
}

bool race_init(Race* race, const RaceParams* p, RaceRoute* route,
		const RaceMarkers* markers, const float carPos[2]) {
	memset(race, 0, sizeof(*race));
	if (!route && p->gameMode != RACE_MODE_OUTRUN)
		return false;
	race->params = *p;
	race->route = route;
	// Retail's world timer has been running since the world loaded, so by
	// any race it is seconds in -- and LapCounter's 3 s and 2 s holds are
	// measured from fields its constructor zeroes, which only reads right on
	// a clock already past them. Ten seconds in; 0 stays every timer's
	// "unset".
	race->nowSec = RACE_CLOCK_START;
	race->now = (s32)(race->nowSec * 4000.0f);
	// __4Race.
	race->laps = p->laps;
	if (race->laps != 0 && p->pointToPoint)
		race->laps = 1;
	race->bestLap = 0x7FFFFFFF;
	race->countdownStep = 0.9f; // 0x8033AFB4
	figureOutBoundaries(race, markers);
	race->nextRank = 1;

	// RacingCar::Init.
	RacingCar* car = &race->car;
	race_coord_init(route, &car->coord, carPos);
	car->prevPct = car->coord.fraction * 100.0f;
	for (int i = 0; i < RACE_MAX_BOUNDARIES; i++) {
		car->bestSplit[i] = 0x7FFFFFFF;
		car->prevBest[i] = 0x7FFFFFFF;
	}
	car->lap = -1;
	car->maxLap = 0;
	car->progress = -1;
	car->maxProgress = -1;
	updateLapCounts(race, car, 0.0f);
	return true;
}

void race_free(Race* race) {
	race_route_close(race->route);
	race->route = NULL;
	race->stage = RACE_STAGE_NONE;
}

void race_set_stage(Race* race, int stage) {
	race->stage = stage;
	if (stage == RACE_STAGE_COUNTDOWN) {
		race->countdownTime = race->now;
	} else if (stage == RACE_STAGE_RACING) {
		fakeStartlineCrossing(race);
		race->timerBase = race_round_time(race->now, 0.01f);
		race->countdownTime = 0;
		race->startTime = race->now;
	}
}

int race_countdown_number(const Race* race) {
	if (timerSet(race->startTime)) {
		const float t = (float)(race->now - race->startTime) * 0.00025f;
		return t <= race->countdownStep ? 4 : 0;
	}
	if (!timerSet(race->countdownTime))
		return 0;
	const float s = race->countdownStep;
	const float t = (float)(race->now - race->countdownTime) * 0.00025f;
	if (t < s)
		return 3;
	if (t < s + s)
		return 2;
	return t < s * 3.0f ? 1 : 4;
}

float race_countdown_ratio(const Race* race) {
	const float s = race->countdownStep;
	if (s == 0.0f)
		return 0.0f;
	if (timerSet(race->startTime)) {
		const float t = (float)(race->now - race->startTime) * 0.00025f;
		return s <= t ? 1.0f : t / s;
	}
	if (!timerSet(race->countdownTime))
		return 0.0f;
	const float t = (float)(race->now - race->countdownTime) * 0.00025f;
	if (t < s)
		return t / s;
	if (t < s + s)
		return (t - s) / s;
	// 0x8033B0D8 is 3.0.
	return t < s * 3.0f ? (t - (s + s)) / s : 0.0f;
}

s32 race_clock(const Race* race) {
	return race->raceTime;
}

// IntToString2 (0x801A65B0): `digits` characters ending at buf[digits-1],
// least significant first. Without `pad` a digit whose remaining value is 0
// is a space, so 5 in two places is " 5".
static void intToString2(char* buf, int v, int digits, bool pad) {
	for (char* p = buf + digits - 1; digits > 0; digits--, p--) {
		*p = (!pad && v % 10 == 0 && v < 10) ? ' ' : (char)('0' + v % 10);
		v /= 10;
	}
}

// PrintToString(char*, flags, h, m, s, cs) (0x801A662C) on the English
// branches (GetCurrentLanguage 0). `out` needs 16 bytes.
static void printFields(char* out, u32 flags, int h, int m, int s, int cs) {
	const bool zeros = (flags & 4) != 0;
	int lead;
	if (h != 0) {
		strcpy(out, zeros ? "00h:00:00" : "--h:--:--");
		intToString2(out, h, 2, false);
		intToString2(out + 4, m, 2, true);
		intToString2(out + 7, s, 2, true);
		lead = h;
	} else if (m != 0) {
		strcpy(out, zeros ? "00:00.00" : "--:--.--");
		intToString2(out, m, 2, false);
		intToString2(out + 3, s, 2, true);
		intToString2(out + 6, cs, 2, true);
		lead = m;
	} else if (s != 0) {
		strcpy(out, zeros ? "00.00" : "--.--");
		intToString2(out, s, 2, false);
		intToString2(out + 3, cs, 2, true);
		lead = s;
	} else {
		strcpy(out, zeros ? "0.00" : "0.--");
		intToString2(out + 2, cs, 2, true);
		lead = 10; // this branch always makes room for a sign
	}
	if ((flags & 3) == 0)
		return;
	if (lead > 9) {
		// Shift right one for the sign, dropping the last character unless
		// flag 8 asks to keep it.
		char tmp[16];
		const size_t n = strlen(out);
		memcpy(tmp, out, n + 1);
		if (!(flags & 8) && n > 0)
			tmp[n - 1] = '\0';
		memcpy(out + 1, tmp, strlen(tmp) + 1);
	}
	out[0] = (flags & 1) ? '-' : '+';
}

void race_print_time(s32 ticks, u32 flags, char* out) {
	if (!timerSet(ticks)) {
		printFields(out, flags, 0, 0, 0, 0);
		return;
	}
	// GetHoursMinsSeconds (0x801A6A2C) on the magnitude.
	const s32 t = ticks < 0 ? -ticks : ticks;
	const s32 sec = t / 4000, ms = (t % 4000) >> 2;
	if (ticks < 0)
		flags |= 1;
	printFields(out, flags, sec / 3600, sec % 3600 / 60, sec % 60, ms / 10);
}

static void stepCar(Race* race, RacingCar* car, const float pos[2], float speed,
	float dt, bool best);

void race_update(Race* race, float dt, const float pos[2], float speed) {
	if (!raceLive(race) || race->stage == RACE_STAGE_NONE)
		return;
	// The world timer: 4000 ticks a second, run off simulated time so a
	// paused world stops the clock with it.
	race->nowSec += dt;
	race->now = (s32)(race->nowSec * 4000.0f);
	// Retail never lets 0 mean "now"; 0 is its unset value.
	if (race->now == 0)
		race->now = 1;

	if (race->stage == RACE_STAGE_COUNTDOWN && race_countdown_number(race) == 4)
		race_set_stage(race, RACE_STAGE_RACING);
	if (race->stage == RACE_STAGE_RACING)
		race->raceTime = race_round_time(race->now - race->timerBase, 0.01f);

	stepCar(race, &race->car, pos, speed, dt, true);

	// Race::Update's Hurry! finish (0x8022A70C, drag3 761): once +0x20's
	// seconds (above 0, 0x8033B1A4) have passed since +0x1C, every car
	// still racing is finished in the last place free, reason 11, with no
	// race time. Retail walks its car list; the port's is the player, then
	// the field in grid order.
	if (race->hurrySec > 0.0f && timerSet(race->hurryStart) &&
			race->hurrySec <= (float)(race->now - race->hurryStart) * 0.00025f) {
		const int count = race_car_count(race);
		for (int i = 0; i < count; i++) {
			const RacingCar* c = race_car(race, i);
			if (c && !c->finished)
				race_retire(race, i, RACE_FINISH_HURRY);
		}
	}
}

// RacingCarStats::Update (0x802267D0), its top and average speed. Both wait
// for the car to be racing (Car +0x434 > 2, which is GO). The human's top
// speed is simply the most its speed has read; an AI car's is its speed
// held to the car's theoretical top and dropped outside 0..1000 m/s
// (0x8033ADF0 / F4). The average is the distance driven over the race
// clock (0x8033AE08, a tick in seconds), once the clock has started.
// PORT: the distance is summed here from the speed, where retail keeps it
// on the Car (+0x424); and the AI arm's GetTheoreticalTopSpeed cap is left
// out, the port's AI cars having no RidePhysicsInfo to ask.
static void updateStats(const Race* race, RacingCar* car, float speed, float dt,
		bool human) {
	if (race->stage != RACE_STAGE_RACING)
		return;
	car->odometer += fabsf(speed) * dt;
	if (human) {
		if (car->topSpeed < speed)
			car->topSpeed = speed;
	} else {
		const float v = fabsf(speed);
		if (car->topSpeed < v && v <= 1000.0f)
			car->topSpeed = v;
	}
	const float secs = (float)race->raceTime * 0.00025f;
	if (0.0f < secs)
		car->avgSpeed = car->odometer / secs;
}

// One RacingCar's share of Race::Update. `best` says whether its lap times
// feed the race's best lap, which only the player's do here -- retail reads
// the best lap off the player's car the same way.
static void stepCar(Race* race, RacingCar* car, const float pos[2], float speed,
		float dt, bool best) {
	race_coord_update(race->route, &car->coord, pos);
	// RacingCar::Update runs the stats after the lap counts and whether or
	// not the car is home; a finished car's lap counts no longer move, so
	// the order makes no difference here.
	updateStats(race, car, speed, dt, best);
	if (car->finished)
		return;
	updateLapCounts(race, car, speed);
	if (best && car->newLap && timerSet(car->lapTime[car->maxLap]) &&
			car->lapTime[car->maxLap] < race->bestLap)
		race->bestLap = car->lapTime[car->maxLap];

	// Race::Update's lap finish: a car whose best lap count reaches the
	// race's is finished with reason 1, ranked next, timed from its last
	// finish-line crossing.
	if (race->laps != 0 && car->maxLap == race->laps) {
		car->finished = true;
		car->finishReason = 1;
		car->finishRank = (u8)race->nextRank++;
		car->raceTime = car->crossTime[race->finishIdx] - race->timerBase;
		// FinishCar's drift arm (#464): an AI car (driver type 2) home first
		// with reason 1 starts the Hurry! timer, once -- from its finish, for
		// the parameters' +0x48 seconds.
		if (race->params.drift && car != &race->car && car->finishRank == 1 &&
				!timerSet(race->hurryStart)) {
			race->hurryStart = race->timerBase + car->raceTime;
			race->hurrySec = race->params.hurryLimit;
		}
	}
}

float race_hurry_left(const Race* race) {
	if (!race || race->hurrySec <= 0.0f || !timerSet(race->hurryStart))
		return -1.0f;
	const float left = race->hurrySec - (float)(race->now - race->hurryStart) * 0.00025f;
	return left > 0.0f ? left : 0.0f;
}

int race_add_ai(Race* race, const float pos[2]) {
	if (!raceLive(race) || race->aiCount >= RACE_MAX_AI)
		return -1;
	RacingCar* car = &race->ai[race->aiCount];
	memset(car, 0, sizeof(*car));
	race_coord_init(race->route, &car->coord, pos);
	car->prevPct = car->coord.fraction * 100.0f;
	for (int i = 0; i < RACE_MAX_BOUNDARIES; i++) {
		car->bestSplit[i] = 0x7FFFFFFF;
		car->prevBest[i] = 0x7FFFFFFF;
	}
	car->lap = -1;
	car->maxLap = 0;
	car->progress = -1;
	car->maxProgress = -1;
	updateLapCounts(race, car, 0.0f);
	return race->aiCount++;
}

void race_update_ai(Race* race, int i, const float pos[2], float speed, float dt) {
	if (!raceLive(race) || race->stage == RACE_STAGE_NONE)
		return;
	if (i < 0 || i >= race->aiCount)
		return;
	stepCar(race, &race->ai[i], pos, speed, dt, false);
}

void race_car_shifts(Race* race, int idx, int shifts, int perfect) {
	RacingCar* car = idx <= 0 ? &race->car : idx - 1 < race->aiCount ? &race->ai[idx - 1] : NULL;
	if (!car || car->finished)
		return;
	car->shifts = shifts;
	car->perfectShifts = perfect;
}

const RacingCar* race_car(const Race* race, int idx) {
	if (idx <= 0)
		return &race->car;
	if (idx - 1 >= race->aiCount)
		return NULL;
	return &race->ai[idx - 1];
}

int race_car_count(const Race* race) {
	return 1 + race->aiCount;
}

// Who is where, right now. Retail's standings come out of RaceCoordinator
// sorting the cars on their RacingCar progress; a car that has finished holds
// the place it finished in, and the rest are ordered on how far round they
// are -- whole boundaries first, then the fraction of the route between them,
// which is what breaks the tie between two cars inside the same sector.
int race_position(const Race* race, int idx) {
	const RacingCar* me = race_car(race, idx);
	if (!me)
		return 0;
	if (me->finished && me->finishRank)
		return me->finishRank;
	int ahead = 0;
	const int n = race_car_count(race);
	for (int k = 0; k < n; k++) {
		if (k == idx)
			continue;
		const RacingCar* o = race_car(race, k);
		if (!o)
			continue;
		// A car home ahead; one out of the race (retired, knocked out,
		// disconnected) holds a place from the back (lastAvailableRank), so
		// it is behind every car still running.
		if (o->finished) { if (o->finishReason == 1) ahead++; continue; }
		if (o->maxProgress != me->maxProgress) {
			if (o->maxProgress > me->maxProgress) ahead++;
			continue;
		}
		float a = o->coord.fraction, b = me->coord.fraction;
		// Inside the same sector the two fractions cannot be more than half
		// the route apart, so the wrap at the finish line reads correctly.
		float d = a - b;
		if (d > 0.5f) d -= 1.0f;
		else if (d < -0.5f) d += 1.0f;
		if (d > 0.0f) ahead++;
	}
	return ahead + 1;
}

// ExtrapolateRaceTime (0x80228394) for a car `laps` laps into a race it has
// not finished. Retail has two estimates. When the race leader finished
// clean (reason 1, a real time) it charges the car the leader's time plus
// what TimeAroundTrackTable -- built from the route's posted speeds -- says
// the rest of the distance takes. Otherwise it scales the time so far by
// how much of the race the car has covered.
// PORT: the port has no TimeAroundTrackTable (TrackRouteManager's posted
// speeds are not packed), so every car takes the second estimate.
static void extrapolateRaceTime(Race* race, RacingCar* car, int laps) {
	float frac = car->lapExact / (float)laps;
	if (frac < 0.01f) // 0x8033AF20
		frac = 0.01f;
	const float elapsed = (float)(race->now - race->timerBase);
	car->raceTime = (s32)(elapsed * 0.00025f / frac * 4000.0f + 0.5f);
	car->raceTime = race_round_time(car->raceTime, 0.01f);
	car->extrapolated = true;

	const int fin = race->finishIdx;
	// No lap done: the best lap is the average lap at the same pace.
	if (car->maxLap == 0 && laps > 0) {
		const float since = (float)(race->now - car->crossTime[race->startIdx]);
		car->bestSplit[fin] = race_round_time(
			(s32)(since * 0.00025f / frac / (float)laps * 4000.0f + 0.5f), 0.01f);
	}
	// The laps still to run share what is left of the race time evenly.
	const int from = car->maxLap;
	if (from + 1 <= laps && laps < RACE_MAX_LAPS) {
		const s32 each = (s32)((float)(car->raceTime - car->lapTotal[from]) *
			0.00025f / (float)(laps - from) * 4000.0f + 0.5f);
		for (int k = from + 1; k <= laps; k++) {
			car->lapTime[k] = each;
			car->lapTotal[k] = car->lapTotal[k - 1] + each;
			if (car->lapTime[k] < car->bestSplit[fin])
				car->bestSplit[fin] = car->lapTime[k];
		}
	}
	car->maxLap = (s8)laps;
	car->lapExact = (float)laps;
}

// FinishCar(car, 0, false, 1) (0x80229C88), the part a forced finish runs: the
// next free rank, and the car's time -- its real one if it is on its last
// crossing, an estimate otherwise.
static void forceFinish(Race* race, RacingCar* car) {
	car->finishReason = 1;
	car->finishRank = (u8)race->nextRank++;
	if (car->maxLap == race->laps)
		car->raceTime = car->crossTime[race->finishIdx] - race->timerBase;
	else
		extrapolateRaceTime(race, car, race->laps);
	car->finished = true;
}

// GetLastAvailableRank (0x8022B4A4): walking the leadership order from the
// back, the place of the last car with no finish rank -- the lowest place no
// car holds -- else GetNextFinishRank.
static int lastAvailableRank(Race* race) {
	const int count = race_car_count(race);
	for (int r = count; r > 0; r--) {
		bool taken = false;
		for (int i = 0; i < count && !taken; i++) {
			const RacingCar* c = race_car(race, i);
			taken = c && c->finished && c->finishRank == r;
		}
		if (!taken)
			return r;
	}
	return race->nextRank++;
}

void race_retire(Race* race, int idx, int reason) {
	RacingCar* car = (RacingCar*)race_car(race, idx);
	if (!car || car->finished)
		return;
	car->finishRank = (u8)lastAvailableRank(race);
	car->finishReason = (s16)reason;
	car->finished = true;
}

void race_retire_at(Race* race, int idx, int reason, s32 raceTime) {
	RacingCar* car = (RacingCar*)race_car(race, idx);
	if (!car || car->finished)
		return;
	race_retire(race, idx, reason);
	car->raceTime = raceTime > 0 ? raceTime : 0;
}

void race_set_finish_time(Race* race, int idx, s32 raceTime) {
	RacingCar* car = (RacingCar*)race_car(race, idx);
	if (!car || race->params.drift)
		return;
	if (!car->finished) {
		car->finished = true;
		car->finishReason = 1;
		car->finishRank = (u8)race->nextRank++;
	} else if (car->finishReason != 1) {
		return;
	}
	car->raceTime = raceTime;
	// The places the lap finishers hold, dealt again by time.
	const int count = race_car_count(race);
	RacingCar* fin[1 + RACE_MAX_AI];
	u8 rank[1 + RACE_MAX_AI];
	int n = 0;
	for (int i = 0; i < count && n < 1 + RACE_MAX_AI; i++) {
		RacingCar* c = (RacingCar*)race_car(race, i);
		if (!c || !c->finished || c->finishReason != 1)
			continue;
		int j = n++;
		while (j > 0 && fin[j - 1]->raceTime > c->raceTime) {
			fin[j] = fin[j - 1];
			j--;
		}
		fin[j] = c;
		int k = n - 1;
		while (k > 0 && rank[k - 1] > c->finishRank) {
			rank[k] = rank[k - 1];
			k--;
		}
		rank[k] = c->finishRank;
	}
	for (int i = 0; i < n; i++)
		fin[i]->finishRank = rank[i];
}

void race_simulate_finish(Race* race) {
	if (!race->route || race->laps <= 0)
		return;
	// The normal-mode branch. The other, for the lap knockout (game mode 3),
	// ranks the cars still running from the back; the port's races are mode 1.
	// Retail walks its leadership-sorted car list; the running order is the
	// same thing, read before any of them is finished.
	int order[1 + RACE_MAX_AI], pos[1 + RACE_MAX_AI], n = 0;
	const int count = race_car_count(race);
	for (int i = 0; i < count; i++) {
		const RacingCar* c = race_car(race, i);
		if (!c || c->finished)
			continue;
		pos[n] = race_position(race, i);
		order[n++] = i;
	}
	for (int a = 1; a < n; a++)
		for (int b = a; b > 0 && pos[b] < pos[b - 1]; b--) {
			int t = pos[b]; pos[b] = pos[b - 1]; pos[b - 1] = t;
			t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
		}
	for (int k = 0; k < n; k++)
		forceFinish(race, (RacingCar*)race_car(race, order[k]));
}

// FinishedRaceStatsEntry::Construct (0x80132A2C), the fields the stats
// screen reads. The lap tables are copied for laps 1..laps, at most 20.
static void statsEntry(const Race* race, int idx, const RaceDriftCar* drift,
		RaceStatsEntry* e) {
	const RacingCar* c = race_car(race, idx);
	memset(e, 0, sizeof(*e));
	// +0x00 is RacingCar +0x0A, the car's place, which for a car that is
	// home is the place it finished in.
	e->position = race_position(race, idx);
	e->car = idx;
	e->finishReason = c->finishReason;
	e->raceTime = c->finishReason == 3 ? 0 : c->raceTime;
	e->maxLap = c->maxLap;
	// GetBestLapTime, or the race time for a car with no lap done.
	e->bestLap = c->maxLap < 1 ? c->raceTime : c->bestSplit[race->finishIdx];
	for (int lap = 1; lap <= race->laps && lap < RACE_MAX_LAPS; lap++) {
		e->lapTime[lap] = c->lapTime[lap];
		e->lapTotal[lap] = c->lapTotal[lap];
	}
	e->topSpeed = c->topSpeed;
	e->avgSpeed = c->avgSpeed;
	e->shifts = c->shifts;
	e->perfectShifts = c->perfectShifts;
	if (!drift)
		return;
	// The drift's: GetBestLapScore (0x802287AC), the most of RacingCar
	// +0x13C's 21; each lap's points + 0.5 (DAT_80329864) truncated, and
	// their running sum; the accumulator's DRIFT moment when the car has one
	// (+0x1D4); then the place, GetLeaderBoardPos of the car number.
	const RaceDriftCar* d = &drift[idx];
	float best = 0.0f;
	for (int lap = 0; lap < RACE_MAX_LAPS; lap++)
		if (best - d->lapScore[lap] < 0.0f)
			best = d->lapScore[lap];
	e->bestLapScore = (int)best;
	for (int lap = 1; lap <= race->laps && lap < RACE_MAX_LAPS; lap++) {
		e->driftLap[lap] = (int)(d->lapScore[lap] + 0.5f);
		e->driftTotal[lap] = e->driftTotal[lap - 1] + e->driftLap[lap];
	}
	if (d->has) {
		e->bestDrift = (int)d->maxPts;
		e->longestDrift = d->maxDur;
		e->driftScore = d->banked;
	}
	e->position = d->boardPos;
}

int race_finished_stats(const Race* race, const RaceDriftCar* drift, RaceStatsEntry* out,
		int cap) {
	if (!raceLive(race) || cap <= 0)
		return 0;
	// The table is filled off Race +0xBD0, the car list RaceCoordinator
	// keeps in running order -- by now the finishing order.
	int order[1 + RACE_MAX_AI], pos[1 + RACE_MAX_AI], n = 0;
	const int count = race_car_count(race);
	for (int i = 0; i < count && n < 1 + RACE_MAX_AI; i++) {
		pos[n] = race_position(race, i);
		order[n++] = i;
	}
	for (int a = 1; a < n; a++)
		for (int b = a; b > 0 && pos[b] < pos[b - 1]; b--) {
			int t = pos[b]; pos[b] = pos[b - 1]; pos[b - 1] = t;
			t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
		}
	if (n > cap)
		n = cap;
	for (int k = 0; k < n; k++)
		statsEntry(race, order[k], drift, &out[k]);

	// CalculatePositionsForEachLap: for each lap, the cars sorted by their
	// time at the end of it (SortRacingCarByTime 0x80133530: a car with no
	// time for the lap behind every car with one, then the lower total
	// first), the list carried over from lap to lap, the first of each
	// sort leading that lap.
	int list[1 + RACE_MAX_AI];
	for (int k = 0; k < n; k++)
		list[k] = k;
	const int laps = race->laps < RACE_MAX_LAPS - 1 ? race->laps : RACE_MAX_LAPS - 1;
	for (int lap = 1; lap <= laps; lap++) {
		for (int a = 1; a < n; a++)
			for (int b = a; b > 0; b--) {
				const RaceStatsEntry* x = &out[list[b - 1]];
				const RaceStatsEntry* y = &out[list[b]];
				const bool xNone = x->lapTime[lap] == 0, yNone = y->lapTime[lap] == 0;
				const bool swap = xNone != yNone ? xNone
					: y->lapTotal[lap] < x->lapTotal[lap];
				if (!swap)
					break;
				const int t = list[b]; list[b] = list[b - 1]; list[b - 1] = t;
			}
		for (int k = 0; k < n; k++) {
			out[list[k]].lapPos[lap] = k + 1;
			if (k == 0)
				out[list[k]].lapsLed++;
		}
	}
	if (!drift)
		return n;

	// A drift's second pass: laps led start over, and each lap the same list
	// is sorted on the running score (SortRacingCarByScore 0x801335F4: a car
	// that scored nothing in the lap behind every car that did, then the
	// higher total first), its places going to +0x1E8 + lap * 4.
	// PORT: retail runs this to min(laps, 21) over a ten-lap array, so past
	// lap 10 it would write over +0x214 on; the port keeps to the ten.
	for (int k = 0; k < n; k++)
		out[k].lapsLed = 0;
	for (int lap = 1; lap <= laps; lap++) {
		for (int a = 1; a < n; a++)
			for (int b = a; b > 0; b--) {
				const RaceStatsEntry* x = &out[list[b - 1]];
				const RaceStatsEntry* y = &out[list[b]];
				const bool xNone = x->driftLap[lap] == 0, yNone = y->driftLap[lap] == 0;
				const bool swap = xNone != yNone ? xNone
					: x->driftTotal[lap] < y->driftTotal[lap];
				if (!swap)
					break;
				const int t = list[b]; list[b] = list[b - 1]; list[b - 1] = t;
			}
		for (int k = 0; k < n; k++) {
			if (lap < RACE_DRIFT_LAP_POS)
				out[list[k]].driftLapPos[lap] = k + 1;
			if (k == 0)
				out[list[k]].lapsLed++;
		}
	}

	// cFinishedRaceStats::Construct's table (+0xCD8) in a drift: bList::Sort
	// with SortResultsByDriftPoints, the higher +0x198[laps] first and ties
	// in entry order.
	const int at = race->laps < RACE_MAX_LAPS ? race->laps : RACE_MAX_LAPS - 1;
	for (int a = 1; a < n; a++)
		for (int b = a; b > 0 && out[b - 1].driftTotal[at] < out[b].driftTotal[at]; b--) {
			const RaceStatsEntry t = out[b];
			out[b] = out[b - 1];
			out[b - 1] = t;
		}
	return n;
}
