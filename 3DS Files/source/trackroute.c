#include "trackroute.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See trackroute.h for the format and for what of retail's route following
// this stands in for.

#define TR_CHUNK_ROUTES 0x00034121u
#define TR_CHUNK_SIGNS  0x00034122u   // the Signposts (TASKS.md #409)
#define TR_SIGN_BYTES   0x20
#define TR_ROUTE_HDR    0x8C
#define TR_POINT_BYTES  0x38
// TrackRouteManager::Loader's stride is 0xE08C - (0x400 - n) * 0x38, so a
// route never holds more than 0x400 points.
#define TR_ROUTE_POINTS_MAX 0x400

typedef struct {
	float x, y, z;
	float halfL, halfR;   // +0x14 / +0x16
	float off[2];         // racing lines 0 and 1, metres
	float lane[8];        // the traffic lanes, lines 2..9, metres (draglanes)
	float fwd[2];         // +0x18 / +0x1A, GetForwardDirection (x 1/32767)
	float raceDist;       // +0x08, metres along the race (trackroute_nearest)
	u8 postF, postB;      // bits 11 / 18
	u8 postP;             // bit 27, GetPostedForPlayersForward (0x80312BC0)
	u8 speed;             // +0x2C, line 0's speed, m/s
	u16 route;            // which route of the set the point is on
} TRPoint;

// A route's header, as much of it as trackroute_nearest reads.
typedef struct {
	int first, count;           // its points in pts
	float bbMin[2], bbMax[2];   // +0x60 / +0x68
	u32 bits;                   // +0x70, TrackRoute::GetBits (0x80302548)
	u8 line[10];                // +0x82 + line: non-zero where it has the line
	int num;                    // its number, its points' bits 0-10 (GetRoute 0x801F7C60)
} TRRoute;

// A Signpost (chunk 0x34122: a u32, then 0x20 bytes each), what a car reads
// where one route meets another: from route `oldRn`'s point `oldPt` onto
// `newRn`'s `newPt` (indices within each route), and its bits (GetBits
// 0x80302508 on +0x0C): 0-3 the sign type for racers (GetSignType
// ePostednessType 0), 18 the side it stands on, 20 a sign never taken, 23 the
// new route driven the other way round, 27 the rarer of the two odds.
typedef struct {
	s16 newPt, oldPt, newRn, oldRn;
	u32 bits;
} TRSign;

struct TrackRouteSet {
	TRPoint* pts;
	int nPts;
	TRRoute* routes;
	int nRoutes;
	TRSign* signs;
	int nSigns;
};

static u32 rd32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
static float rdf(const u8* p) {
	const u32 v = rd32(p);
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

// Walks every route in every 0x34121 chunk of `buf`. With `out` NULL it only
// counts; the second pass fills, `outRoutes` too when it is not NULL.
static void walkRoutes(const u8* buf, long size, TRPoint* out, TRRoute* outRoutes,
		int* nPts, int* nRoutes) {
	int np = 0, nr = 0;
	long off = 0;
	while (off + 8 <= size) {
		const u32 type = rd32(buf + off), csz = rd32(buf + off + 4);
		if ((long)csz > size - (off + 8))
			break;
		if (type == TR_CHUNK_ROUTES) {
			long r = off + 8;
			const long end = r + (long)csz;
			while (r + TR_ROUTE_HDR <= end) {
				const int n = (s16)rd16(buf + r + 0x34);
				if (n < 0 || n > TR_ROUTE_POINTS_MAX ||
						r + TR_ROUTE_HDR + (long)n * TR_POINT_BYTES > end)
					break;
				for (int k = 0; out && k < n; k++) {
					const u8* p = buf + r + TR_ROUTE_HDR + k * TR_POINT_BYTES;
					TRPoint* q = &out[np + k];
					const u32 bits = rd32(p + 0x0C);
					q->x = rdf(p + 0x00);
					q->y = rdf(p + 0x04);
					q->z = (float)(s16)rd16(p + 0x12) * (8192.0f / 65536.0f);
					q->halfL = (float)rd16(p + 0x14) * (1.0f / 256.0f);
					q->halfR = (float)rd16(p + 0x16) * (1.0f / 256.0f);
					const float w = q->halfL > q->halfR ? q->halfL : q->halfR;
					q->off[0] = (float)(s8)p[0x22] * (1.0f / 128.0f) * w;
					q->off[1] = (float)(s8)p[0x23] * (1.0f / 128.0f) * w;
					for (int l = 0; l < 8; l++)
						q->lane[l] = (float)(s8)p[0x24 + l] * (1.0f / 128.0f) * w;
					q->fwd[0] = (float)(s16)rd16(p + 0x18) * (1.0f / 32767.0f);
					q->fwd[1] = (float)(s16)rd16(p + 0x1A) * (1.0f / 32767.0f);
					q->raceDist = rdf(p + 0x08);
					q->speed = p[0x2C];
					q->postF = (u8)((bits >> 11) & 1);
					q->postB = (u8)((bits >> 18) & 1);
					q->postP = (u8)((bits >> 27) & 1);
					q->route = (u16)nr;
				}
				if (out && outRoutes) {
					TRRoute* rt = &outRoutes[nr];
					rt->first = np;
					rt->count = n;
					rt->bbMin[0] = rdf(buf + r + 0x60);
					rt->bbMin[1] = rdf(buf + r + 0x64);
					rt->bbMax[0] = rdf(buf + r + 0x68);
					rt->bbMax[1] = rdf(buf + r + 0x6C);
					rt->bits = rd32(buf + r + 0x70);
					memcpy(rt->line, buf + r + 0x82, sizeof(rt->line));
					rt->num = n > 0 ? (int)(rd32(buf + r + TR_ROUTE_HDR + 0x0C) & 0x7FFu) : -1;
				}
				np += n;
				nr++;
				r += TR_ROUTE_HDR + (long)n * TR_POINT_BYTES;
			}
		}
		off += 8 + (long)csz;
	}
	*nPts = np;
	*nRoutes = nr;
}

// Every Signpost in every 0x34122 chunk of `buf`; counts them with `out` NULL.
static int walkSigns(const u8* buf, long size, TRSign* out) {
	int n = 0;
	long off = 0;
	while (off + 8 <= size) {
		const u32 type = rd32(buf + off), csz = rd32(buf + off + 4);
		if ((long)csz > size - (off + 8))
			break;
		if (type == TR_CHUNK_SIGNS && csz >= 4) {
			const int cnt = (int)((csz - 4) / TR_SIGN_BYTES);
			for (int i = 0; i < cnt; i++, n++) {
				if (!out) continue;
				const u8* p = buf + off + 8 + 4 + (long)i * TR_SIGN_BYTES;
				TRSign* g = &out[n];
				g->newPt = (s16)rd16(p + 0x04);
				g->oldPt = (s16)rd16(p + 0x06);
				g->newRn = (s16)rd16(p + 0x08);
				g->oldRn = (s16)rd16(p + 0x0A);
				g->bits = rd32(p + 0x0C);
			}
		}
		off += 8 + (long)csz;
	}
	return n;
}

TrackRouteSet* trackroute_open(const char* path, void (*log)(const char* msg)) {
	char line[200];
	FILE* f = fopen(path, "rb");
	if (!f) {
		if (log) {
			snprintf(line, sizeof(line), "trackroute: no %s", path);
			log(line);
		}
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* buf = size > 8 ? (u8*)malloc((size_t)size) : NULL;
	const bool read = buf && fread(buf, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	if (!read) {
		free(buf);
		if (log) {
			snprintf(line, sizeof(line), "trackroute: could not read %s", path);
			log(line);
		}
		return NULL;
	}

	int np = 0, nr = 0;
	walkRoutes(buf, size, NULL, NULL, &np, &nr);
	TrackRouteSet* s = NULL;
	if (np > 0) {
		s = (TrackRouteSet*)calloc(1, sizeof(TrackRouteSet));
		if (s) s->pts = (TRPoint*)malloc(sizeof(TRPoint) * (size_t)np);
		if (s) s->routes = (TRRoute*)malloc(sizeof(TRRoute) * (size_t)nr);
		if (s && s->pts && s->routes) {
			walkRoutes(buf, size, s->pts, s->routes, &s->nPts, &s->nRoutes);
			const int ns = walkSigns(buf, size, NULL);
			s->signs = ns > 0 ? (TRSign*)malloc(sizeof(TRSign) * (size_t)ns) : NULL;
			s->nSigns = s->signs ? walkSigns(buf, size, s->signs) : 0;
		} else {
			if (s) {
				free(s->pts);
				free(s->routes);
			}
			free(s);
			s = NULL;
		}
	}
	free(buf);
	if (log) {
		if (s)
			snprintf(line, sizeof(line), "trackroute: %d route(s), %d point(s), %d sign(s)",
				s->nRoutes, s->nPts, s->nSigns);
		else
			snprintf(line, sizeof(line), "trackroute: %s holds no routes", path);
		log(line);
	}
	return s;
}

void trackroute_close(TrackRouteSet* s) {
	if (!s) return;
	free(s->pts);
	free(s->routes);
	free(s->signs);
	free(s);
}

// ------------------------------------------------- the nearest route point
//
// See trackroute.h.

// TrackPathManager::CompareDistances (0x8020309C): a - b, wrapped half a lap
// either way on a loop (+0x24, with the length at +0x18).
static float compareDist(float a, float b, float loopLen) {
	float d = a - b;
	if (loopLen > 0.0f) {
		if (d < -0.5f * loopLen) d += loopLen;
		else if (d > 0.5f * loopLen) d -= loopLen;
	}
	return d;
}

// TrackRoutePoint::GetDistanceAway: straight-line (0x801F8078), or with the
// race's distance (0x801F80F0) the larger of that and how far apart the two
// are along the race, less 50 m (0x80336DB4).
static float distAway(const TrackRoutePt* p, const TrackRouteQuery* q, bool race) {
	const float dx = q->pos[0] - p->pos[0], dy = q->pos[1] - p->pos[1];
	const float dz = q->pos[2] - p->pos[2];
	float d = sqrtf(dx * dx + dy * dy + dz * dz);
	if (race) {
		const float a = fabsf(compareDist(p->raceDist, q->raceDist, q->loopLen)) - 50.0f;
		if (a > d) d = a;
	}
	return d;
}

// TrackRoute::FindClosestPoint (0x801F86A8): nothing unless the car is inside
// the route's box grown by `maxDist`; then the nearest point under `maxDist`,
// a far point skipping the next int(d / 50) - 2 (0x80337150), which no point
// that close could beat. The ahead/behind (1, 2) and racer/traffic postings
// (4, 0x200) it can also ask for have no caller here.
static int findClosest(const TrackRouteSource* s, int r, const TrackRouteQuery* q,
		float maxDist, bool race, float* outDist, TrackRoutePt* outPt, u32* outBits) {
	int first = 0, count = 0;
	float mn[2], mx[2];
	s->route(s->ctx, r, &first, &count, mn, mx, outBits);
	if (q->pos[0] < mn[0] - maxDist || q->pos[0] > mx[0] + maxDist ||
			q->pos[1] < mn[1] - maxDist || q->pos[1] > mx[1] + maxDist)
		return -1;
	int best = -1;
	float bestD = maxDist;
	for (int k = 0; k < count; k++) {
		TrackRoutePt p;
		s->point(s->ctx, first + k, &p);
		const float d = distAway(&p, q, race);
		if (d < bestD && (!(q->flags & TRN_PLAYERS) || p.posted)) {
			best = first + k;
			bestD = d;
			*outPt = p;
		}
		const int skip = (int)(d / 50.0f) - 2;
		if (skip > 0) k += skip;
	}
	*outDist = bestD;
	return best;
}

bool trackroute_nearest(const TrackRouteSource* s, const TrackRouteQuery* q,
		TrackRoutePick* out) {
	if (!s || !q || s->nRoutes <= 0 || !s->route || !s->point) return false;
	const bool race = (q->flags & TRN_RACE_DIST) && q->racing;
	float best = (q->flags & TRN_NEAR) ? 100.0f : 999999.0f;   // 0x80316FB0 / FAC
	bool found = false;
	for (int r = 0; r < s->nRoutes; r++) {
		TrackRoutePt p;
		float d = 0.0f;
		u32 bits = 0;
		const int k = findClosest(s, r, q, best, race, &d, &p, &bits);
		if (k < 0) continue;
		// Only the route's nearest point is tested: one that fails leaves the
		// route out, however near its next point is.
		if ((q->flags & TRN_LEVEL) && !(fabsf(q->pos[2] - p.pos[2]) < 4.0f))   // 0x80316FC4
			continue;
		if (!(d < best)) continue;
		if ((q->flags & TRN_WALLS) && d < 90.0f) {   // 0x80316FCC
			if (q->wallBetween && q->wallBetween(q->ctx, p.pos, q->pos)) continue;
			if (q->wallNear && q->wallNear(q->ctx, p.pos, 1.5f)) continue;   // 0x80316FD0
		}
		best = d;
		found = true;
		out->route = r;
		out->point = k;
		out->routeBits = bits;
		out->dist = d;
		out->pt = p;
	}
	return found;
}

static void setRoute(const void* ctx, int r, int* first, int* count, float bbMin[2],
		float bbMax[2], u32* bits) {
	const TRRoute* rt = &((const TrackRouteSet*)ctx)->routes[r];
	*first = rt->first;
	*count = rt->count;
	bbMin[0] = rt->bbMin[0];
	bbMin[1] = rt->bbMin[1];
	bbMax[0] = rt->bbMax[0];
	bbMax[1] = rt->bbMax[1];
	*bits = rt->bits;
}

static void setPoint(const void* ctx, int p, TrackRoutePt* out) {
	const TrackRouteSet* s = (const TrackRouteSet*)ctx;
	const TRPoint* q = &s->pts[p];
	out->pos[0] = q->x;
	out->pos[1] = q->y;
	out->pos[2] = q->z;
	out->fwd[0] = q->fwd[0];
	out->fwd[1] = q->fwd[1];
	const TRPoint* b = p > 0 && s->pts[p - 1].route == q->route ? &s->pts[p - 1] : q;
	out->back[0] = -b->fwd[0];
	out->back[1] = -b->fwd[1];
	out->raceDist = q->raceDist;
	out->posted = q->postP != 0;
	out->speed = q->speed;
}

TrackRouteSource trackroute_source(const TrackRouteSet* s) {
	TrackRouteSource src;
	memset(&src, 0, sizeof(src));
	if (s) {
		src.ctx = s;
		src.nRoutes = s->nRoutes;
		src.route = setRoute;
		src.point = setPoint;
	}
	return src;
}

// ------------------------------------------------------- the line table
//
// The tuning below is the port's own: it decides which retail route a stretch
// of the race route belongs to, which retail never has to ask.

#define AILINE_STEP_MAX     2.0f    // metres between samples
#define AILINE_SCAN_EVERY   16      // samples between candidate gathers
#define AILINE_SCAN_REACH   60.0f   // a gather keeps segments this near...
#define AILINE_REACH        20.0f   // ...for matches at most this far off a road
#define AILINE_ALIGN        0.5f    // cos 60 deg: a road further off the race's heading is a side road
#define AILINE_UP_COST      6.0f    // metres a route driven against its direction pays
#define AILINE_SAME_ROUTE   2.0f    // what staying on the last sample's route is worth
#define AILINE_NEAR_COST    0.1f    // per metre off a route's own line, between roads the sample is on
#define AILINE_Z_FREE       1.5f    // the height change from the last match that costs nothing
#define AILINE_Z_COST       4.0f    // and per metre beyond it, so a deck above or below loses
// A car is continuous, so the line it drives has to be. Nothing above says so:
// miss, lat, up and z are each about one sample on its own, so two neighbouring
// samples could be matched to routes whose driving lines are ten metres apart
// and the cost function would not notice. On route 4164 that happened 27 times,
// and one of them -- 2,618 m to 2,638 m, straight road -- threw the line 12 m
// right and back again over 20 m. The curvature of that V held the field to
// 30 m/s less than the road deserved, and it is a V the AI then steered at.
// So a candidate pays for where it puts the line relative to the last sample's,
// beyond what a car can actually move sideways in one step. Over AILINE_STEP_MAX
// (2 m), half a metre is already a 14 degree lane change.
#define AILINE_JUMP_FREE    0.5f    // metres of lateral change per sample that cost nothing
#define AILINE_JUMP_COST    3.0f    // and the cost per metre beyond it
// The roads are chosen for the whole race route at once (below); each sample
// keeps this many of its cheapest.
#define AILINE_MATCH_STATES 16
// And which side of a wall each road's line is on. A step from one road to the
// next costs only how far it moves the line, so a change of carriageway costs
// the same wherever it is made. 4006B has a crossover between two medians at
// 604 - 648 m: the match kept the left carriageway past it and changed at
// 650 - 690 m, through the second median, and the wall pass could only ramp the
// line across to the median's nose, where the field stopped. So each road's
// line is placed among the same gaps between walls the wall pass reads
// (sampleGaps), at the last sample's cheapest height: a sample costs
// AILINE_MATCH_WALL_OUT a metre its line is outside its nearest gap, and a step
// between two gaps that do not meet (gapsMeet) costs AILINE_MATCH_THROUGH.
#define AILINE_MATCH_WALL_OUT 1.0f
#define AILINE_MATCH_THROUGH  1000.0f

// One road a sample may be on: the segment from pts[seg], how far along it,
// whether driven against it, how far outside its surface the race route is,
// and which of the last sample's roads the cheapest way here came from.
typedef struct {
	int seg;
	float u, out;
	u8 up;
	s8 from;
} MatchState;
// How far outside a posted road's own surface the race route may be and the
// road still count as the road the race is on. Measured over the whole of
// route 4001, where the tester's field drove off: 94% of samples sit within
// 3 m of the posted surface -- the postings run a little narrow against the
// race route the whole way round -- and then there is a thin tail out to
// 18.8 m. That tail is not a narrow road. It is the race route crossing
// ground no posted road covers at all (a junction, a branch the postings do
// not follow), where the nearest road's own centreline is 16 to 26 m away
// across the route. Taking it anyway put the racing line 31.5 m off the race
// route between 1,690 m and 1,740 m, and that is what walked the field into
// the scenery there. Past this, the sample has no road, which the bridge
// below already knows how to answer.
//
// Unless the road is the one matched either side. A run past AILINE_OFF_ROAD
// whose every sample's road is the same route, driven the same way, as the
// matched samples just before and just after it is not ground with no road:
// it is the race route cutting the inside of that road's bend. 4042B at
// 2,417-2,487 m is route 32 all the way (segments 952-957), and the race route
// swings 10-17 m west of it, through a row of 38 m walls the road runs beside.
// Bridged along the race route, the line took the race route's own corner --
// a 6 m step at 2,433 m, 13 m inside it -- as a 45 degree turn in 6 m, which
// the speed profile's 15 m chords plan at 16 m/s, and three cars in five ran
// wide past the first wall's nose into the corridor beyond it. Such a run
// keeps the road: each sample where the road's racing line crosses its
// normal, as gridCross puts the grid's road in. Within AILINE_WALL_REACH of
// the race route, where the wall pass can still see.
#define AILINE_OFF_ROAD     6.0f
// The longest run of unmatched samples a straight line between the matches
// either side is allowed to answer, in samples. A chord is the road over a
// seam or a junction and wrong over a bend: on a 50 m radius it is a metre
// out over 20 m and nine metres out over 60. Past this the run is not a gap
// in a road, it is ground with no posted road on it at all, and the only
// thing still known there is the race route -- so the line follows that,
// blended to the matched line at each end, which is what the port steered
// before it had the postings at all.
#define AILINE_BRIDGE_CHORD 16      // 32 m at AILINE_STEP_MAX
#define AILINE_SMOOTH       3       // samples either side the two lines are averaged over
// What is left after both of those can still step: a route change the jump
// cost judged worth paying moves the line metres sideways inside one sample,
// and AILINE_SMOOTH spreads that over twelve metres -- a 25 degree swerve at
// race speed. So a step is ramped: no stretch of the line may move sideways
// faster than this, and a step is shared out evenly either side of where it
// was (TASKS.md #280).
#define AILINE_STEP_RATE    0.15f   // metres sideways per metre along, 8.5 degrees
// The walls (TASKS.md #280). The matched line knows the posted roads, not
// what is solid, and the two disagree in exactly the places a car gets hurt:
// at the 4001 fork (1,640 - 1,710 m) the posting before the split is the
// left-hand road's, the race takes the right-hand one, and the line drawn
// between them crossed the gore's barrier nose. So the line is checked against
// the race's own collision: across the race route at every sample, the gaps
// between the solid walls within AILINE_WALL_REACH are the places a car can
// be, less AILINE_WALL_MARGIN either side, and the line is kept in one gap
// after another that never goes through a wall, ramped in at
// AILINE_STEP_RATE, so the move is driven, not made at the wall. Which gaps is
// the cheapest way through the whole race: a sample costs how far the matched
// line is outside its gap, and AILINE_WALL_ASIDE more where the race route is
// plainly in another one -- the posting is the other road's at 4122B's split
// (1,880 m) and 4042F's crossroads (2,670 m), and the other side's at 4021B's
// island (1,430 m), and the race route is not. Following the line's own gap
// and repairing it where it crossed into another could not see far enough
// back to any of those, and every car of the field stopped at each; following
// the route's gap instead took the line off 4001's dual carriageway, whose
// route is down the median barrier. A wall there is no way round costs
// AILINE_WALL_THROUGH.
#define AILINE_WALL_REACH   30.0f   // m either side of the race route
#define AILINE_WALL_LINE_REACH 12.0f // and round the line's own point
#define AILINE_WALL_SIDE    18.0f   // and round a point this far either side
#define AILINE_WALL_MARGIN  2.0f    // half a car and a little, kept off a wall
// And where the gap has the room, this much more for the line itself. A car
// steering at a point up a curving line runs inside it, by up to opponent.c's
// LINE_CHORD_SAG, and 4006B's line ramped round the nose of a median at 650 m
// with the margin's 1.1 m to spare: the field ran 1.3 - 1.4 m inside it and
// into the nose. A gap narrower than twice this keeps the line in its middle.
#define AILINE_WALL_CLEAR   1.0f
#define AILINE_WALL_GATHER  96      // walls asked for per question
#define AILINE_WALL_GAPS    4       // gaps kept per sample, nearest the race route first
#define AILINE_WALL_SLACK   1.0f    // how much two samples' gaps may miss and still join
#define AILINE_WALL_ROAD    0.5f    // how far inside a gap the route has to be to be in it
#define AILINE_WALL_ASIDE   30.0f   // cost of a sample whose line is not in the route's gap
#define AILINE_WALL_THROUGH 1e5f    // and of a wall gone through
// Where there is no way round a wall -- the race goes off a ledge, as 4044F
// does at 3,450 m, and the ledge's face is a wall -- the line is left free
// this many samples either side of it, so it crosses as matched rather than
// being held to either side.
#define AILINE_WALL_CROSS   5
// Every wall the finished line goes through is kept, with the road's height
// either side of it, at most AILINE_WALL_KEEP of them: a race goes up a ledge
// there -- the road beyond more than AILINE_HOP_UP higher -- which no car can
// drive, and ailine_face_ahead tells the field where.
#define AILINE_WALL_KEEP    16
#define AILINE_HOP_UP       0.5f
#define AILINE_HOP_JOIN     8.0f
// How far along the route either side of a sample a wall counts at it: half a
// sample, and the half a car's length that reaches past its middle. A wall's
// whole footprint across that strip is in the way, not just the point where it
// crosses the sample's own normal -- the end of the traffic island at 4021's
// 690 m fell between two normals, so neither saw it, and the line took the
// field into it.
#define AILINE_WALL_STRIP   3.0f
// The band a wall has to reach into to count: main.c's WORLD_BODY_LO and
// WORLD_BODY_HI above the road, the same band a car's own body collides in.
// The road is the ground under the line, not the TrackRoute's z, which can sit
// most of a metre over the tarmac (4021's 680 - 690 m: 19.5 on a road at
// 18.6, and the island there was under the band). Ground is looked for no
// higher than AILINE_GROUND_UP over the route's z, and trusted within
// AILINE_GROUND_TRUST of it.
#define AILINE_BODY_LO      0.35f
#define AILINE_BODY_HI      3.0f
#define AILINE_GROUND_UP    1.5f
#define AILINE_GROUND_TRUST 3.0f
// And how near a road edge that is a wall a car's own offset may take it,
// beyond the wall margin above -- the edges the field reads (opponent.c's
// LINE_EDGE_MARGIN) are moved in this much from a wall. A car held at such an
// edge is LINE_EDGE_MARGIN in from it, and runs as much as a metre and a half
// inside its offset through a bend: at 0.8, 4123F's grid, all five on the
// road's right edge through the grace period, cut into a 2.8 m step in the
// wall at 92 m at 29 m/s, and the field piled up behind the first car.
#define AILINE_WALL_EDGE    1.5f
// The line faired (TASKS.md #280). Where the wall pass moved it, it is the
// ramps' and the gaps' line, and where those two disagree -- squeezed: the gaps
// move sideways faster than a ramp -- it is clamped sample by sample to a gap's
// edge, so it takes that wall's own angle and every corner of it. 4143F's
// docks, 3,014 - 3,064 m: 47 degrees along one wall, then a 2.3 m jog round a
// kerb island inside one sample, which the speed profile's 15 m chords never
// saw; the field took it at 26 m/s and into the island. So each stretch the
// pass touched, AILINE_FAIR_PAD samples either side, is refitted as a curve in
// the world: the least bending (the line's second difference, as world points)
// that stays near the line it was, at AILINE_FAIR_HOLD a sample -- and not at
// all where it was squeezed, which is no line to stay near -- inside every
// sample's own gap. AILINE_FAIR_HOLD is how short a wiggle is ironed out: 0.01
// is about 40 m. The samples a wall crossing leaves free are held still, and
// so are two either side of the stretch.
#define AILINE_FAIR_PAD     12
#define AILINE_FAIR_HOLD    0.01
#define AILINE_FAIR_ITER    64

// F_WX/F_WY are the line itself as a world point, which is what a car tracks
// itself along and steers at (ailine_pose, ailine_track).
enum { F_LINE, F_POINT, F_LEFT, F_RIGHT, F_Z, F_DX, F_DY, F_WX, F_WY, F_COUNT };

struct AiLine {
	int n;
	float step, len;
	bool loop;
	float* v;   // n * F_COUNT
	float* arc; // n + 1: world metres along the line to each sample, the last round to a loop's start
	int nWall;  // the walls with no way round, in order along the route
	AiLineCrossing wall[AILINE_WALL_KEEP];
};

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Sample k of a looped or a sprint table, wrapped or clamped.
static int wrapIndex(const AiLine* t, int k) {
	if (t->loop) {
		k %= t->n;
		return k < 0 ? k + t->n : k;
	}
	return k < 0 ? 0 : (k >= t->n ? t->n - 1 : k);
}

static void lineSpan(const AiLine* t, float dist, int* k0, int* k1, float* f);

// The Lipschitz envelopes of two bounds at `rate` metres per metre: out lo[k]
// is the greatest lo[j] - rate * |k - j| * step, hi[k] the least hi[j] + the
// same, so a bound at one sample becomes a ramp into its neighbours. Two laps
// round a loop carry a bound across the seam. In place.
static void envelope(const AiLine* t, float* lo, float* hi, float rate) {
	const float d = rate * t->step;
	const int laps = t->loop ? 2 : 1;
	for (int pass = 0; pass < laps * t->n; pass++) {
		const int k = pass % t->n, p = wrapIndex(t, k - 1);
		if (p == k) continue;
		if (lo[p] - d > lo[k]) lo[k] = lo[p] - d;
		if (hi[p] + d < hi[k]) hi[k] = hi[p] + d;
	}
	for (int pass = laps * t->n - 1; pass >= 0; pass--) {
		const int k = pass % t->n, p = wrapIndex(t, k + 1);
		if (p == k) continue;
		if (lo[p] - d > lo[k]) lo[k] = lo[p] - d;
		if (hi[p] + d < hi[k]) hi[k] = hi[p] + d;
	}
}

// AILINE_STEP_RATE: every sample's line is both a floor and a ceiling for its
// neighbours'; where a step makes the two cross, the line goes half way
// between them, which is a ramp at half the rate centred on the step.
static int rampSteps(AiLine* t, float* lo, float* hi) {
	for (int k = 0; k < t->n; k++)
		lo[k] = hi[k] = t->v[k * F_COUNT + F_LINE];
	envelope(t, lo, hi, 2.0f * AILINE_STEP_RATE);
	int moved = 0;
	for (int k = 0; k < t->n; k++) {
		float* o = t->v + k * F_COUNT;
		const float m = 0.5f * (lo[k] + hi[k]);
		if (fabsf(m - o[F_LINE]) > 0.05f) moved++;
		o[F_LINE] = m;
	}
	return moved;
}

// The road's edges, as offsets off the line, ramped the same way: a car keeps
// its own offset (opponent.c's kSlotLateral, +/-2.2 m) until an edge takes it
// away, and an edge that closes in one sample -- the nose of a traffic island,
// whose wall is only in the strips of the samples it reaches -- took it away
// at the nose. 4143F, 1,176 m: the line passes an island's nose at 2.0 m, the
// car in slot 4 was still 2.2 m right of the line one sample before it, and
// hit the nose at 26 m/s (TASKS.md #280). Now an edge closes in at
// AILINE_STEP_RATE ahead of where it has to be, and opens again as slowly.
static void rampEdges(AiLine* t, float* lo, float* hi) {
	for (int k = 0; k < t->n; k++) {
		const float* o = t->v + k * F_COUNT;
		lo[k] = o[F_LEFT] - o[F_LINE];
		hi[k] = o[F_RIGHT] - o[F_LINE];
	}
	envelope(t, lo, hi, AILINE_STEP_RATE);
	for (int k = 0; k < t->n; k++) {
		float* o = t->v + k * F_COUNT;
		o[F_LEFT] = o[F_LINE] + (lo[k] < 0.0f ? lo[k] : 0.0f);
		o[F_RIGHT] = o[F_LINE] + (hi[k] > 0.0f ? hi[k] : 0.0f);
	}
}

// One sample's gaps between walls, across the race route: [a, b] where a car
// may be, as laterals, the wall margin already taken off. An end that is not a
// wall is +/-AILINE_WALL_REACH and has no margin taken off it.
typedef struct {
	u8 n;
	float a[AILINE_WALL_GAPS], b[AILINE_WALL_GAPS];
} WallGaps;

static void sampleGaps(const AiLineWalls* walls, AiLineWall* buf, const float* at,
		float z, float line, WallGaps* g, float* roadZ) {
	// Each wall's lateral extent inside the sample's strip (AILINE_WALL_STRIP).
	float bs[4 * AILINE_WALL_GATHER], be[4 * AILINE_WALL_GATHER];
	int nb = 0;
	const float px = at[0], py = at[1], nx = at[2], ny = at[3];
	const float lx = px + nx * line, ly = py + ny * line;
	const float h = AILINE_WALL_STRIP;
	// The road under the race route, which is the road the gaps are for;
	// under the line where the route is over nothing.
	float gz;
	if (walls->ground && ((walls->ground(walls->ctx, px, py, z + AILINE_GROUND_UP, &gz) &&
			fabsf(gz - z) < AILINE_GROUND_TRUST) ||
			(walls->ground(walls->ctx, lx, ly, z + AILINE_GROUND_UP, &gz) &&
			fabsf(gz - z) < AILINE_GROUND_TRUST)))
		z = gz;
	*roadZ = z;
	// Asked about four times: round the route's point for the whole width,
	// round a point either side of it, and round the line's own point.
	// Topology answers from the one section the point asked about is in
	// (topo_walls_near), and where the line or the road's far side is in the
	// next section over -- 4106F's 4876 m, 4044B's 4462 m -- the route's
	// question alone never heard of the walls there.
	for (int q = 0; q < 4; q++) {
		if (q == 3 && fabsf(line) < 1.0f) continue;
		const float side = q == 1 ? -AILINE_WALL_SIDE : (q == 2 ? AILINE_WALL_SIDE : 0.0f);
		const float qx = q == 3 ? lx : px + nx * side, qy = q == 3 ? ly : py + ny * side;
		const float reach = q == 0 ? AILINE_WALL_REACH + h
			: (q == 3 ? AILINE_WALL_LINE_REACH : AILINE_WALL_REACH - AILINE_WALL_SIDE + h);
		const int nw = walls->near(walls->ctx, qx, qy, reach, z + AILINE_BODY_LO,
			z + AILINE_BODY_HI, buf, AILINE_WALL_GATHER);
		for (int i = 0; i < nw; i++) {
			// The wall's two ends as s along the route and t across it.
			const float ax = buf[i].x0 - px, ay = buf[i].y0 - py;
			const float bx = buf[i].x1 - px, by = buf[i].y1 - py;
			const float s0 = ax * ny - ay * nx, s1 = bx * ny - by * nx;
			float t0 = ax * nx + ay * ny, t1 = bx * nx + by * ny;
			if ((s0 > h && s1 > h) || (s0 < -h && s1 < -h)) continue;
			if (fabsf(s1 - s0) > 1e-6f) {
				float u0 = (-h - s0) / (s1 - s0), u1 = (h - s0) / (s1 - s0);
				if (u0 > u1) { const float u = u0; u0 = u1; u1 = u; }
				if (u0 < 0.0f) u0 = 0.0f;
				if (u1 > 1.0f) u1 = 1.0f;
				if (u0 > u1) continue;
				const float ta = t0 + u0 * (t1 - t0), tb = t0 + u1 * (t1 - t0);
				t0 = ta;
				t1 = tb;
			}
			const float lo = t0 < t1 ? t0 : t1, hi = t0 < t1 ? t1 : t0;
			if (hi <= -AILINE_WALL_REACH || lo >= AILINE_WALL_REACH) continue;
			int j = nb - 1;
			while (j >= 0 && bs[j] > lo) { bs[j + 1] = bs[j]; be[j + 1] = be[j]; j--; }
			bs[j + 1] = lo;
			be[j + 1] = hi;
			nb++;
		}
	}
	// Every gap between them, then the AILINE_WALL_GAPS nearest the race route.
	float ga[4 * AILINE_WALL_GATHER + 1], gb[4 * AILINE_WALL_GATHER + 1];
	int ng = 0;
	float edge = -AILINE_WALL_REACH;
	bool wall = false;
	for (int i = 0; i <= nb; i++) {
		const float next = i == nb ? AILINE_WALL_REACH : bs[i];
		if (next > edge) {
			const float lo = wall ? edge + AILINE_WALL_MARGIN : -AILINE_WALL_REACH;
			const float hi = i == nb ? AILINE_WALL_REACH : next - AILINE_WALL_MARGIN;
			if (hi > lo) { ga[ng] = lo; gb[ng] = hi; ng++; }
		}
		if (i < nb && be[i] > edge) {
			edge = be[i];
			wall = true;
		}
	}
	g->n = 0;
	while (g->n < AILINE_WALL_GAPS && ng > 0) {
		int best = 0;
		float bd = 1e30f;
		for (int i = 0; i < ng; i++) {
			const float d = ga[i] > 0.0f ? ga[i] : (gb[i] < 0.0f ? -gb[i] : 0.0f);
			if (d < bd) { bd = d; best = i; }
		}
		g->a[g->n] = ga[best];
		g->b[g->n] = gb[best];
		g->n++;
		ga[best] = ga[ng - 1];
		gb[best] = gb[ng - 1];
		ng--;
	}
	// In lateral order, so neighbouring samples compare like with like.
	for (int i = 1; i < g->n; i++) {
		const float a = g->a[i], b = g->b[i];
		int j = i - 1;
		while (j >= 0 && g->a[j] > a) { g->a[j + 1] = g->a[j]; g->b[j + 1] = g->b[j]; j--; }
		g->a[j + 1] = a;
		g->b[j + 1] = b;
	}
}

// Whether gap i of `g` and gap j of `h` (neighbouring samples) are one road:
// they overlap, give or take AILINE_WALL_SLACK.
static bool gapsMeet(const WallGaps* g, int i, const WallGaps* h, int j) {
	const float lo = g->a[i] > h->a[j] ? g->a[i] : h->a[j];
	const float hi = g->b[i] < h->b[j] ? g->b[i] : h->b[j];
	return hi - lo > -AILINE_WALL_SLACK;
}

// Gap i of `g` as the line is held to it: AILINE_WALL_CLEAR more off each end
// that is a wall, as far as the gap's middle.
static void gapClear(const WallGaps* g, int i, float* lo, float* hi) {
	const float half = 0.5f * (g->b[i] - g->a[i]);
	const float c = half < AILINE_WALL_CLEAR ? half : AILINE_WALL_CLEAR;
	*lo = g->a[i] > -AILINE_WALL_REACH ? g->a[i] + c : g->a[i];
	*hi = g->b[i] < AILINE_WALL_REACH ? g->b[i] - c : g->b[i];
}

// Whether the segment from (x0, y0) to (x1, y1) crosses wall `w`.
static bool segmentsCross(float x0, float y0, float x1, float y1, const AiLineWall* w) {
	const float dx = x1 - x0, dy = y1 - y0;
	const float ex = w->x1 - w->x0, ey = w->y1 - w->y0;
	const float den = dx * ey - dy * ex;
	if (fabsf(den) < 1e-9f) return false;
	const float fx = w->x0 - x0, fy = w->y0 - y0;
	const float s = (fx * ey - fy * ex) / den, u = (fx * dy - fy * dx) / den;
	return s >= 0.0f && s <= 1.0f && u >= 0.0f && u <= 1.0f;
}

// The road's height under a point of the line, looked for as sampleGaps
// looks under the route; `road` when there is none to trust.
static float lineGround(const AiLineWalls* walls, float x, float y, float z, float road) {
	float gz;
	if (walls->ground && walls->ground(walls->ctx, x, y, z + AILINE_GROUND_UP, &gz) &&
			fabsf(gz - z) < AILINE_GROUND_TRUST)
		return gz;
	return road;
}

// Sample u of the line, wrapped round a loop; -1 past a sprint's ends.
static int fairIndex(const AiLine* t, int u) {
	if (t->loop) return wrapIndex(t, u);
	return u >= 0 && u < t->n ? u : -1;
}

// One stretch faired (AILINE_FAIR_*): the m samples from s, unwrapped. `flags`
// is the wall pass's: bit 2 held still, bit 4 squeezed; lo/hi each sample's
// own gap. The bends' normal equations are banded five wide and solved
// directly; a sample the solution puts outside its gap is held at the edge and
// the rest solved again, and one held where the bending would sooner leave the
// edge is let go.
static void fairRun(AiLine* t, const float* at, const float* lo, const float* hi,
		const u8* flags, int s, int m) {
	static const double c[3] = { 1.0, -2.0, 1.0 };
	const int e = m + 4;
	double* mem = (double*)malloc(sizeof(double) * (size_t)(9 * m + 3 * e));
	u8* state = (u8*)calloc((size_t)m, 1);   // 1 held at lo, 2 at hi
	if (!mem || !state) {
		free(mem); free(state);
		return;
	}
	double *H0 = mem, *H1 = H0 + m, *H2 = H1 + m, *rhs = H2 + m, *L0 = rhs + m, *L1 = L0 + m,
		*L2 = L1 + m, *y = L2 + m, *r = y + m;
	// The line and its bends' world vectors at local -2 .. m + 1, [i + 2].
	double *l = r + m, *bx = l + e, *by = bx + e;
	for (int i = -2; i < m + 2; i++) {
		const int k = fairIndex(t, s + i);
		l[i + 2] = k >= 0 ? t->v[k * F_COUNT + F_LINE] : 0.0;
	}
	for (int i = 0; i < m; i++) r[i] = l[i + 2];

	for (int iter = 0; iter < AILINE_FAIR_ITER; iter++) {
		memset(H0, 0, sizeof(double) * (size_t)(4 * m));
		for (int j = -1; j <= m; j++) {
			int k[3], v[3];
			double cx = 0.0, cy = 0.0;
			for (int q = 0; q < 3; q++) k[q] = fairIndex(t, s + j - 1 + q);
			if (k[0] < 0 || k[1] < 0 || k[2] < 0) continue;
			for (int q = 0; q < 3; q++) {
				const int i = j - 1 + q;
				const float* a = at + 4 * k[q];
				v[q] = i >= 0 && i < m && !(flags[k[q]] & 2) && !state[i] ? i : -1;
				cx += c[q] * a[0];
				cy += c[q] * a[1];
				if (v[q] < 0) {
					cx += c[q] * a[2] * l[i + 2];
					cy += c[q] * a[3] * l[i + 2];
				}
			}
			for (int p = 0; p < 3; p++) {
				if (v[p] < 0) continue;
				const float* ap = at + 4 * k[p];
				rhs[v[p]] -= c[p] * (ap[2] * cx + ap[3] * cy);
				for (int q = p; q < 3; q++) {
					if (v[q] < 0) continue;
					const float* aq = at + 4 * k[q];
					const double h = c[p] * c[q] * (ap[2] * aq[2] + ap[3] * aq[3]);
					if (q == p) H0[v[p]] += h;
					else if (q == p + 1) H1[v[p]] += h;
					else H2[v[p]] += h;
				}
			}
		}
		for (int i = 0; i < m; i++) {
			const int k = fairIndex(t, s + i);
			if ((flags[k] & 2) || state[i]) {
				H0[i] = 1.0;
				rhs[i] = l[i + 2];
			} else {
				const double w = ((flags[k] & 4) ? 0.0 : AILINE_FAIR_HOLD) + 1e-4;
				H0[i] += w;
				rhs[i] += w * r[i];
			}
		}
		bool ok = true;
		for (int i = 0; i < m && ok; i++) {
			L2[i] = i >= 2 ? H2[i - 2] / L0[i - 2] : 0.0;
			L1[i] = i >= 1 ? (H1[i - 1] - L2[i] * L1[i - 1]) / L0[i - 1] : 0.0;
			const double d = H0[i] - L1[i] * L1[i] - L2[i] * L2[i];
			if (d > 1e-12) L0[i] = sqrt(d);
			else ok = false;
		}
		if (!ok) break;
		for (int i = 0; i < m; i++)
			y[i] = (rhs[i] - (i >= 1 ? L1[i] * y[i - 1] : 0.0) - (i >= 2 ? L2[i] * y[i - 2] : 0.0)) / L0[i];
		for (int i = m - 1; i >= 0; i--) {
			double x = y[i];
			if (i + 1 < m) x -= L1[i + 1] * l[i + 3];
			if (i + 2 < m) x -= L2[i + 2] * l[i + 4];
			l[i + 2] = x / L0[i];
		}

		int added = 0;
		for (int i = 0; i < m; i++) {
			const int k = fairIndex(t, s + i);
			if ((flags[k] & 2) || state[i]) continue;
			if (l[i + 2] < lo[k]) { state[i] = 1; l[i + 2] = lo[k]; added++; }
			else if (l[i + 2] > hi[k]) { state[i] = 2; l[i + 2] = hi[k]; added++; }
		}
		if (added) continue;
		// Every bend, then which held sample the bending pulls hardest off its edge.
		for (int j = -1; j <= m; j++) {
			bx[j + 1] = by[j + 1] = 0.0;
			for (int q = 0; q < 3; q++) {
				const int k = fairIndex(t, s + j - 1 + q);
				if (k < 0) { bx[j + 1] = by[j + 1] = 0.0; break; }
				const float* a = at + 4 * k;
				bx[j + 1] += c[q] * (a[0] + a[2] * l[j + 1 + q]);
				by[j + 1] += c[q] * (a[1] + a[3] * l[j + 1 + q]);
			}
		}
		int worst = -1;
		double most = 1e-6;
		for (int i = 0; i < m; i++) {
			if (!state[i]) continue;
			const int k = fairIndex(t, s + i);
			const float* a = at + 4 * k;
			double grad = (((flags[k] & 4) ? 0.0 : AILINE_FAIR_HOLD) + 1e-4) * (l[i + 2] - r[i]);
			for (int q = 0; q < 3; q++)
				grad += c[q] * (a[2] * bx[i + 2 - q] + a[3] * by[i + 2 - q]);
			const double pull = state[i] == 1 ? -grad : grad;
			if (pull > most) { most = pull; worst = i; }
		}
		if (worst < 0) break;
		state[worst] = 0;
	}
	for (int i = 0; i < m; i++) {
		const int k = fairIndex(t, s + i);
		if (flags[k] & 2) continue;
		double x = l[i + 2];
		if (x < lo[k]) x = lo[k];
		if (x > hi[k]) x = hi[k];
		t->v[k * F_COUNT + F_LINE] = (float)x;
	}
	free(mem);
	free(state);
}

typedef void (*FairRunFn)(AiLine* t, const float* at, const float* lo, const float* hi,
	const u8* flags, int s, int m);

// Every stretch the wall pass touched (flags bit 8) faired by `run`,
// AILINE_FAIR_PAD samples either side (bit 16).
static void fairLine(AiLine* t, const float* at, const float* lo, const float* hi, u8* flags,
		FairRunFn run) {
	const int n = t->n;
	bool any = false;
	for (int k = 0; k < n; k++) {
		if (!(flags[k] & 8)) continue;
		any = true;
		for (int d = -AILINE_FAIR_PAD; d <= AILINE_FAIR_PAD; d++) {
			const int j = fairIndex(t, k + d);
			if (j >= 0) flags[j] |= 16;
		}
	}
	if (!any) return;
	// A sprint from its start; a loop from a sample outside every stretch, or
	// where they are the whole loop, from the first one held still.
	int start = 0, from = 0;
	if (t->loop) {
		start = -1;
		for (int k = 0; k < n && start < 0; k++)
			if (!(flags[k] & 16)) start = k;
		if (start < 0) {
			int h = 0;
			for (int k = 0; k < n; k++)
				if (flags[k] & 2) { h = k; break; }
			if (n > 8) run(t, at, lo, hi, flags, h + 2, n - 4);
			return;
		}
		from = 1;
	}
	for (int c = from; c < n; ) {
		if (!(flags[fairIndex(t, start + c)] & 16)) { c++; continue; }
		int e = c;
		while (e < n && (flags[fairIndex(t, start + e)] & 16)) e++;
		run(t, at, lo, hi, flags, start + c, e - c);
		c = e;
	}
}

// The wall pass. Returns how many samples it moved the line at; `crossings`
// gets how many walls the line has no way round and `squeezed` at how many
// samples the ramps in from either side left it no room.
static int wallPass(AiLine* t, const float* at, const AiLineWalls* walls,
		float* lo, float* hi, int* crossings, int* squeezed) {
	*crossings = *squeezed = 0;
	WallGaps* gaps = (WallGaps*)malloc(sizeof(WallGaps) * (size_t)t->n);
	int* gap = (int*)malloc(sizeof(int) * (size_t)t->n);
	u8* cross = (u8*)calloc((size_t)t->n, 1);
	AiLineWall* buf = (AiLineWall*)malloc(sizeof(AiLineWall) * AILINE_WALL_GATHER);
	float* base = (float*)malloc(sizeof(float) * (size_t)t->n);
	float* road = (float*)malloc(sizeof(float) * (size_t)t->n);
	if (!gaps || !gap || !cross || !buf || !base || !road) {
		free(gaps); free(gap); free(cross); free(buf); free(base); free(road);
		return 0;
	}
	for (int k = 0; k < t->n; k++) {
		const float* o = t->v + k * F_COUNT;
		sampleGaps(walls, buf, at + 4 * k, o[F_Z], o[F_LINE], &gaps[k], &road[k]);
		base[k] = o[F_LINE];
	}

	// Which gap the line is to be in at every sample: the cheapest way through
	// them all. Staying in one road from a sample to the next is free, going
	// through a wall costs AILINE_WALL_THROUGH, a sample costs how far the
	// matched line is outside the gap, and AILINE_WALL_ASIDE more where the race
	// route is plainly in another gap. A sample with no gaps at all is a single
	// state anything leads into and out of. from[] is the way back.
	u8* from = (u8*)malloc((size_t)t->n * AILINE_WALL_GAPS);
	if (!from) {
		free(gaps); free(gap); free(cross); free(buf); free(base); free(road);
		return 0;
	}
	float cost[AILINE_WALL_GAPS], next[AILINE_WALL_GAPS];
	for (int k = 0; k < t->n; k++) {
		const WallGaps* g = &gaps[k];
		const WallGaps* p = k > 0 ? &gaps[k - 1] : NULL;
		int on = -1;
		for (int i = 0; i < g->n; i++)
			if (g->a[i] + AILINE_WALL_ROAD <= 0.0f && g->b[i] - AILINE_WALL_ROAD >= 0.0f) on = i;
		const int states = g->n ? g->n : 1;
		for (int j = 0; j < states; j++) {
			float c = 0.0f;
			if (g->n) {
				const float l = base[k];
				c = l < g->a[j] ? g->a[j] - l : (l > g->b[j] ? l - g->b[j] : 0.0f);
				if (on >= 0 && on != j) c += AILINE_WALL_ASIDE;
			}
			float best = 0.0f;
			int bi = 0;
			if (p) {
				best = 1e30f;
				for (int i = 0; i < (p->n ? p->n : 1); i++) {
					const float x = cost[i] +
						(g->n && p->n && !gapsMeet(g, j, p, i) ? AILINE_WALL_THROUGH : 0.0f);
					if (x < best) { best = x; bi = i; }
				}
			}
			next[j] = best + c;
			from[k * AILINE_WALL_GAPS + j] = (u8)bi;
		}
		memcpy(cost, next, sizeof(float) * (size_t)states);
	}
	{
		const int last = t->n - 1;
		int j = 0;
		for (int i = 1; i < (gaps[last].n ? gaps[last].n : 1); i++)
			if (cost[i] < cost[j]) j = i;
		for (int k = last; k >= 0; k--) {
			gap[k] = gaps[k].n ? j : -1;
			if (k == 0) break;
			const int i = from[k * AILINE_WALL_GAPS + j];
			if (gaps[k].n && gaps[k - 1].n && !gapsMeet(&gaps[k], j, &gaps[k - 1], i)) {
				cross[k] |= 1;
				(*crossings)++;
			}
			j = i;
		}
	}
	free(from);
	// The line bounded to that gap, but left free AILINE_WALL_CROSS samples
	// either side of a wall it has to go through (cross[] bit 2).
	for (int k = 0; k < t->n; k++) {
		lo[k] = -1e30f;
		hi[k] = 1e30f;
		for (int s = -AILINE_WALL_CROSS; s <= AILINE_WALL_CROSS; s++) {
			const int j = k + s;
			if (!t->loop && (j < 0 || j >= t->n)) continue;
			if (cross[wrapIndex(t, j)] & 1) cross[k] |= 2;
		}
		if (gap[k] < 0 || (cross[k] & 2)) continue;
		gapClear(&gaps[k], gap[k], &lo[k], &hi[k]);
	}
	// Ramped in, and the line put inside them. Where the gaps move sideways
	// faster than a ramp can follow the two meet; the line goes where they
	// do, but no further out than the sample's own gap.
	envelope(t, lo, hi, AILINE_STEP_RATE);
	for (int k = 0; k < t->n; k++) {
		float* o = t->v + k * F_COUNT;
		float l = base[k];
		if (lo[k] > hi[k]) {
			l = 0.5f * (lo[k] + hi[k]);
			if (gap[k] >= 0 && !(cross[k] & 2)) {
				if (l < gaps[k].a[gap[k]]) l = gaps[k].a[gap[k]];
				if (l > gaps[k].b[gap[k]]) l = gaps[k].b[gap[k]];
			}
			cross[k] |= 4;
			(*squeezed)++;
		} else if (l < lo[k]) {
			l = lo[k];
		} else if (l > hi[k]) {
			l = hi[k];
		}
		o[F_LINE] = l;
		if ((cross[k] & 4) || fabsf(l - base[k]) > 0.05f) cross[k] |= 8;
	}
	// Faired inside each sample's own gap (AILINE_FAIR_*).
	for (int k = 0; k < t->n; k++) {
		lo[k] = -1e30f;
		hi[k] = 1e30f;
		if (gap[k] >= 0 && !(cross[k] & 2)) gapClear(&gaps[k], gap[k], &lo[k], &hi[k]);
	}
	fairLine(t, at, lo, hi, cross, fairRun);
	int moved = 0;
	for (int k = 0; k < t->n; k++)
		if (fabsf(t->v[k * F_COUNT + F_LINE] - base[k]) > 0.05f) moved++;

	// Every wall the finished line still goes through from one sample to the
	// next: the ones there was no way round, and the ones the ramps could not
	// take it clear of in time -- 4042F's 3,218 m and 3,282 m faces are both
	// across the whole deck, but are met at an angle, and the line was only
	// ever squeezed against them. A wall counts if it reaches into the body
	// band over the road before it, so the face of a ledge the race drops off
	// is not one.
	for (int k = 1; k < t->n + (t->loop ? 1 : 0); k++) {
		const int k0 = k - 1, k1 = wrapIndex(t, k);
		const float* a0 = at + 4 * k0;
		const float* a1 = at + 4 * k1;
		const float l0 = t->v[k0 * F_COUNT + F_LINE], l1 = t->v[k1 * F_COUNT + F_LINE];
		const float x0 = a0[0] + a0[2] * l0, y0 = a0[1] + a0[3] * l0;
		const float x1 = a1[0] + a1[2] * l1, y1 = a1[1] + a1[3] * l1;
		// The road under the line itself, which where the race route runs
		// along the top of a face is not the route's.
		const float z0 = lineGround(walls, x0, y0, t->v[k0 * F_COUNT + F_Z], road[k0]);
		const float reach = 0.5f * sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)) + 0.5f;
		const int nw = walls->near(walls->ctx, 0.5f * (x0 + x1), 0.5f * (y0 + y1), reach,
			z0 + AILINE_BODY_LO, z0 + AILINE_BODY_HI, buf, AILINE_WALL_GATHER);
		bool hit = false;
		for (int i = 0; i < nw && !hit; i++)
			hit = segmentsCross(x0, y0, x1, y1, &buf[i]);
		if (!hit) continue;
		if (t->nWall < AILINE_WALL_KEEP) {
			AiLineCrossing* c = &t->wall[t->nWall++];
			c->dist = c->end = (float)k * t->step;
			c->zBefore = z0;
			c->zAfter = lineGround(walls, x1, y1, t->v[k1 * F_COUNT + F_Z], road[k1]);
		}
	}

	// A wall at a road's edge is that edge for the field's own offsets.
	for (int k = 0; k < t->n; k++) {
		float* o = t->v + k * F_COUNT;
		if (gap[k] < 0 || (cross[k] & 2)) continue;
		const float a = gaps[k].a[gap[k]] - AILINE_WALL_MARGIN + AILINE_WALL_EDGE;
		const float b = gaps[k].b[gap[k]] + AILINE_WALL_MARGIN - AILINE_WALL_EDGE;
		if (gaps[k].a[gap[k]] > -AILINE_WALL_REACH && a > o[F_LEFT]) o[F_LEFT] = a;
		if (gaps[k].b[gap[k]] < AILINE_WALL_REACH && b < o[F_RIGHT]) o[F_RIGHT] = b;
		if (o[F_LEFT] > o[F_LINE]) o[F_LEFT] = o[F_LINE];
		if (o[F_RIGHT] < o[F_LINE]) o[F_RIGHT] = o[F_LINE];
	}
	free(gaps); free(gap); free(cross); free(buf); free(base); free(road);
	return moved;
}

// ------------------------------------------------------- the grid's road
//
// A retail racer finds its route from where the car is (FindClosestTrackRoute,
// 0x80006968) and keeps to it, choosing the next at each signpost by its
// relevance to the track path (DetermineRelevanceOfRoutesToTrackPath,
// 0x801FE998). The port's one line is matched along the race route instead,
// and the two part where the grid is on a road the race route is not (TASKS.md
// #280). 4002F's five slots are on the southern carriageway of a dual road, 8
// to 19 m right of the route at 3,842 m; a median starts 12 m ahead and splits
// the road as far as the next crossroads, 750 m on, and the race route and the
// line take the northern carriageway. No car can cross in 12 m: each ran into
// the median's nose or up the far side of it, and four never finished. 4083B's
// grid is the same at 482 m. PORT: where the grid cannot reach the line's road
// -- the grid's centre is outside the line's edges within AILINE_GRID_AHEAD,
// give or take AILINE_GRID_SLACK and AILINE_GRID_STEER a metre -- the posted
// road under the grid is followed both ways, taking the nearest route posted
// the race's way within AILINE_GRID_NEXT wherever a posting ends, as a car on
// it would find its next route, until its racing line rejoins the line:
// crosses it, or runs within AILINE_GRID_JOIN of it for AILINE_GRID_JOINED
// samples, at the same height and heading. The stretch between is that road on
// every lap (retail's later laps would choose at the signposts), faded into the
// line over the last AILINE_GRID_RAMP samples before each join -- its laterals
// off the race route change as fast as that road leaves it, so no step limit.
#define AILINE_GRID_AHEAD    40.0f   // m past the grid the line's road has to be reached in
#define AILINE_GRID_STEER    0.3f    // m sideways a car makes per m along
#define AILINE_GRID_SLACK    1.0f    // m outside the line's edges the grid may be
#define AILINE_GRID_NEAR     3.0f    // m outside a posted road the grid's centre may be and be on it
#define AILINE_GRID_Z        3.0f    // m of height a road may be off the grid, or off the line to join it
#define AILINE_GRID_HOLD     8.0f    // m from the grid's centre that road's racing line has to pass
#define AILINE_GRID_NEXT     6.0f    // m from a posting's end the next route may be
#define AILINE_GRID_UNPOSTED 8       // route points on past a posting's end where no route takes over
#define AILINE_GRID_ROUTES   32      // routes one walk may take
#define AILINE_GRID_WALK     400     // route points one walk may take
#define AILINE_GRID_SEARCH   32      // walk points along which a sample's normal is looked for
#define AILINE_GRID_LAT      80.0f   // m off the race route a walk may be
#define AILINE_GRID_JOIN     1.5f    // m from the line a walk has rejoined it...
#define AILINE_GRID_JOINED   3       // ...for this many samples running
#define AILINE_GRID_CANDS    16      // roads under the grid tried
#define AILINE_GRID_RAMP     8       // samples before a join the walk fades into the line over

// A walk point: the route point, its racing line as a world point, the road's
// half-widths to the left and right of the race's way, its height, and the
// race's way along it.
typedef struct {
	float cx, cy, lx, ly, hl, hr, z, dx, dy;
} GridVtx;

// Segment j's direction against `h` as a cosine, 0 where j is not a segment
// of one route.
static float gridAlign(const TrackRouteSet* s, int j, const float h[2]) {
	if (j < 0 || j + 1 >= s->nPts) return 0.0f;
	const TRPoint* a = &s->pts[j];
	const TRPoint* e = &s->pts[j + 1];
	if (a->route != e->route) return 0.0f;
	const float ex = e->x - a->x, ey = e->y - a->y;
	const float l = sqrtf(ex * ex + ey * ey);
	return l < 0.01f ? 0.0f : (ex * h[0] + ey * h[1]) / l;
}

// Whether segment j is posted for the race's way round, heading `h`: 1 down
// the route, -1 up it, 0 neither -- the matcher's test.
static int gridDrives(const TrackRouteSet* s, int j, const float h[2]) {
	const float al = gridAlign(s, j, h);
	if (al == 0.0f) return 0;
	const TRPoint* a = &s->pts[j];
	const TRPoint* e = &s->pts[j + 1];
	if (al >= AILINE_ALIGN && (a->postF | e->postF)) return 1;
	if (al <= -AILINE_ALIGN && (a->postB | e->postB)) return -1;
	return 0;
}

// The point u along segment j, driven up the route or down it. Line 1 is
// measured right of the route's backward direction, so either way the racing
// line is right of the race's way by the offset.
static void gridVtx(const TrackRouteSet* s, int j, bool up, float u, GridVtx* v) {
	const TRPoint* a = &s->pts[j];
	const TRPoint* e = &s->pts[j + 1];
	const float ex = e->x - a->x, ey = e->y - a->y;
	const float l = sqrtf(ex * ex + ey * ey);
	const float tx = (up ? -ex : ex) / l, ty = (up ? -ey : ey) / l;
	const float off = up ? lerpf(a->off[1], e->off[1], u) : lerpf(a->off[0], e->off[0], u);
	const float hl = lerpf(a->halfL, e->halfL, u), hr = lerpf(a->halfR, e->halfR, u);
	v->cx = a->x + ex * u;
	v->cy = a->y + ey * u;
	v->lx = v->cx + ty * off;
	v->ly = v->cy - tx * off;
	v->hl = up ? hr : hl;
	v->hr = up ? hl : hr;
	v->z = lerpf(a->z, e->z, u);
	v->dx = tx;
	v->dy = ty;
}

// The posted road from segment j (driven `up` it or down), with the race
// (`fwd`) or against it, from the end of j behind the walk -- and one segment
// further back where the route goes on, so the grid's own sample is crossed.
// Down the same route while it is posted the same way; where that ends, onto
// the nearest other route posted the race's way within AILINE_GRID_NEXT, and
// where there is none, on down this one unposted for AILINE_GRID_UNPOSTED
// points. Returns how many points it wrote.
//
// PORT (TASKS.md #280): onto another road the walk keeps the lateral its line
// had and takes the new road's own racing line over at AILINE_STEP_RATE, the
// rate rampSteps evens the matched line's route changes out to. Put on the
// new road's line where it changed, 4002F's walk stepped 3.6 m in a metre
// where route 82 hands over beside the grid and 9.8 m in one sample onto a
// road 30 m wide at 4,676 m: the car third on the grid steered 44 degrees at
// the start into the car beside it, the two sat locked for two seconds, and
// the car behind them was still under 4 m/s 8 s after the start. The steps
// are the walk's own, so they are taken out here; the laterals across the
// race route cannot ramp them, the grid's road there being 55 m from it.
static void gridCarry(const GridVtx* prev, GridVtx* v, float* carry) {
	const float dx = v->cx - prev->cx, dy = v->cy - prev->cy;
	const float fall = AILINE_STEP_RATE * sqrtf(dx * dx + dy * dy);
	*carry = *carry > fall ? *carry - fall : (*carry < -fall ? *carry + fall : 0.0f);
	v->lx += v->dy * *carry;
	v->ly -= v->dx * *carry;
}

static int gridWalk(const TrackRouteSet* s, int j, bool up, bool fwd, GridVtx* out, int max) {
	const TRPoint* pts = s->pts;
	bool inc = fwd != up;
	int n = 0;
	float carry = 0.0f;   // metres right of its road's line the walk's line still is
	GridVtx first;
	gridVtx(s, j, up, inc ? 0.0f : 1.0f, &first);
	const float h0[2] = { first.dx, first.dy };
	const int jb = inc ? j - 1 : j + 1;
	const float alb = gridAlign(s, jb, h0);
	if ((up ? -alb : alb) >= AILINE_ALIGN)
		gridVtx(s, jb, up, inc ? 0.0f : 1.0f, &out[n++]);
	out[n++] = first;
	u16 seen[AILINE_GRID_ROUTES];
	int nSeen = 0;
	seen[nSeen++] = pts[j].route;
	int p = inc ? j : j + 1, unposted = 0;
	while (n < max) {
		const float h[2] = { out[n - 1].dx, out[n - 1].dy };
		const int seg = inc ? p : p - 1;
		if (gridDrives(s, seg, h) == (up ? -1 : 1)) {
			unposted = 0;
		} else {
			// From where a car on the racing line is, as FindClosestTrackRoute
			// asks: the road it is on, nearest that road's own line.
			const float qx = out[n - 1].lx, qy = out[n - 1].ly;
			int best = -1, bestDir = 0;
			float bestCost = 1e30f, bestU = 0.0f;
			for (int i = 0; i + 1 < s->nPts; i++) {
				const int d = gridDrives(s, i, h);
				if (!d) continue;
				bool was = false;
				for (int r = 0; r < nSeen && !was; r++) was = seen[r] == pts[i].route;
				if (was) continue;
				const TRPoint* a = &pts[i];
				const TRPoint* e = &pts[i + 1];
				const float ex = e->x - a->x, ey = e->y - a->y;
				const float l = sqrtf(ex * ex + ey * ey);
				const float tx = ex / l, ty = ey / l;
				const float rx = qx - a->x, ry = qy - a->y;
				const float along = rx * tx + ry * ty;
				float u = along / l;
				u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
				const float over = along < 0.0f ? -along : (along > l ? along - l : 0.0f);
				const float lat = rx * ty - ry * tx;
				const float hl = lerpf(a->halfL, e->halfL, u), hr = lerpf(a->halfR, e->halfR, u);
				const float o = lat > hr ? lat - hr : (lat < -hl ? -hl - lat : 0.0f);
				const float miss = sqrtf(o * o + over * over);
				if (miss > AILINE_GRID_NEXT) continue;
				const float cost = miss + AILINE_NEAR_COST * fabsf(lat);
				if (cost < bestCost) {
					best = i;
					bestDir = d;
					bestCost = cost;
					bestU = u;
				}
			}
			if (best >= 0 && nSeen < AILINE_GRID_ROUTES) {
				up = bestDir < 0;
				inc = fwd != up;
				seen[nSeen++] = pts[best].route;
				GridVtx* nv = &out[n];
				gridVtx(s, best, up, bestU, nv);
				carry = (qx - nv->lx) * nv->dy - (qy - nv->ly) * nv->dx;
				nv->lx += nv->dy * carry;
				nv->ly -= nv->dx * carry;
				n++;
				p = inc ? best : best + 1;
				unposted = 0;
				continue;
			}
			const float al = gridAlign(s, seg, h);
			if ((up ? -al : al) < AILINE_ALIGN || ++unposted > AILINE_GRID_UNPOSTED)
				break;
		}
		gridVtx(s, seg, up, inc ? 1.0f : 0.0f, &out[n]);
		if (carry != 0.0f) gridCarry(&out[n - 1], &out[n], &carry);
		n++;
		p = inc ? p + 1 : p - 1;
	}
	return n;
}

// Where the walk's racing line crosses the normal of the sample at `a4` (its
// route point and right normal), looked for from walk point *from on: the
// lateral, and the walk segment and how far along it. False where none within
// AILINE_GRID_SEARCH crosses within AILINE_GRID_LAT.
static bool gridCross(const GridVtx* v, int nv, int* from, const float* a4, float* lat,
		int* vi, float* vu) {
	const float px = a4[0], py = a4[1], nx = a4[2], ny = a4[3];
	const float tx = -ny, ty = nx;
	for (int i = *from; i + 1 < nv && i < *from + AILINE_GRID_SEARCH; i++) {
		const float s0 = (v[i].lx - px) * tx + (v[i].ly - py) * ty;
		const float s1 = (v[i + 1].lx - px) * tx + (v[i + 1].ly - py) * ty;
		if (s0 * s1 > 0.0f) continue;
		const float u = s0 != s1 ? s0 / (s0 - s1) : 0.0f;
		const float x = lerpf(v[i].lx, v[i + 1].lx, u), y = lerpf(v[i].ly, v[i + 1].ly, u);
		const float l = (x - px) * nx + (y - py) * ny;
		if (fabsf(l) > AILINE_GRID_LAT) continue;
		*from = i;
		*lat = l;
		*vi = i;
		*vu = u;
		return true;
	}
	return false;
}

// The walk from segment j crossed with the samples from kG on (`fwd`) or back,
// into lat/vi/vu by samples from kG, until it rejoins the line. Returns how
// many samples from kG the first one left to the line is, -1 when it does not
// rejoin within `limit`; a sprint's end counts as a join.
static int gridFollow(const TrackRouteSet* s, const AiLine* t, const float* at, int j, bool up,
		bool fwd, int kG, int limit, GridVtx* v, float* lat, int* vi, float* vu) {
	const int nv = gridWalk(s, j, up, fwd, v, AILINE_GRID_WALK);
	int from = 0, run = 0;
	float dPrev = 0.0f;
	for (int i = 0; i <= limit; i++) {
		const int k = wrapIndex(t, kG + (fwd ? i : -i));
		if (!gridCross(v, nv, &from, at + 4 * k, &lat[i], &vi[i], &vu[i])) return -1;
		const GridVtx* p = &v[vi[i]];
		const GridVtx* q = &v[vi[i] + 1];
		const float* o = t->v + k * F_COUNT;
		const float d = lat[i] - o[F_LINE];
		const bool with = fabsf(lerpf(p->z, q->z, vu[i]) - o[F_Z]) <= AILINE_GRID_Z &&
			lerpf(p->dx, q->dx, vu[i]) * o[F_DX] + lerpf(p->dy, q->dy, vu[i]) * o[F_DY] >=
			AILINE_ALIGN;
		if (i > 0 && with && d * dPrev <= 0.0f) return i;
		run = with && fabsf(d) <= AILINE_GRID_JOIN ? run + 1 : 0;
		if (run >= AILINE_GRID_JOINED) return i - (AILINE_GRID_JOINED - 1);
		dPrev = d;
	}
	return t->loop ? -1 : limit + 1;
}

// Sample k as the walk has it where its racing line crosses, into o: the
// line, the road's centre, edges, height and heading, and the edges taken in
// to a wall the way the wall pass takes them in, the line kept off it.
static void gridFill(float* o, const float* at, int k, const GridVtx* v, float lat, int vi,
		float vu, const AiLineWalls* walls, AiLineWall* buf) {
	const float* a = at + 4 * k;
	const GridVtx* p = &v[vi];
	const GridVtx* q = &v[vi + 1];
	const float cx = lerpf(p->cx, q->cx, vu), cy = lerpf(p->cy, q->cy, vu);
	float dx = lerpf(p->dx, q->dx, vu), dy = lerpf(p->dy, q->dy, vu);
	const float dl = sqrtf(dx * dx + dy * dy);
	if (dl > 1e-4f) {
		dx /= dl;
		dy /= dl;
	} else {
		dx = p->dx;
		dy = p->dy;
	}
	const float latC = (cx - a[0]) * a[2] + (cy - a[1]) * a[3];
	const float rn = dy * a[2] - dx * a[3];   // the road's right, across the race route
	const float latR = latC + rn * lerpf(p->hr, q->hr, vu);
	const float latL = latC - rn * lerpf(p->hl, q->hl, vu);
	o[F_LINE] = lat;
	o[F_POINT] = latC;
	o[F_LEFT] = latL < latR ? latL : latR;
	o[F_RIGHT] = latL < latR ? latR : latL;
	o[F_Z] = lerpf(p->z, q->z, vu);
	o[F_DX] = dx;
	o[F_DY] = dy;
	if (!walls || !walls->near) return;
	const float l4[4] = { a[0] + a[2] * lat, a[1] + a[3] * lat, a[2], a[3] };
	WallGaps g;
	float roadZ;
	sampleGaps(walls, buf, l4, o[F_Z], 0.0f, &g, &roadZ);
	int gi = -1;
	float gd = 1e30f;
	for (int i = 0; i < g.n; i++) {
		const float d = g.a[i] > 0.0f ? g.a[i] : (g.b[i] < 0.0f ? -g.b[i] : 0.0f);
		if (d < gd) { gd = d; gi = i; }
	}
	if (gi < 0) return;
	const float ga = g.a[gi], gb = g.b[gi];
	o[F_LINE] = lat + (ga > 0.0f ? ga : (gb < 0.0f ? gb : 0.0f));
	if (ga > -AILINE_WALL_REACH && lat + ga - AILINE_WALL_MARGIN + AILINE_WALL_EDGE > o[F_LEFT])
		o[F_LEFT] = lat + ga - AILINE_WALL_MARGIN + AILINE_WALL_EDGE;
	if (gb < AILINE_WALL_REACH && lat + gb + AILINE_WALL_MARGIN - AILINE_WALL_EDGE < o[F_RIGHT])
		o[F_RIGHT] = lat + gb + AILINE_WALL_MARGIN - AILINE_WALL_EDGE;
}

// Sample k of the line made the walk's w, faded f of the way back to what the
// line had there.
static void gridPut(AiLine* t, int k, const float* w, float f) {
	float* o = t->v + k * F_COUNT;
	for (int q = F_LINE; q <= F_DY; q++) o[q] = lerpf(w[q], o[q], f);
	const float l = sqrtf(o[F_DX] * o[F_DX] + o[F_DY] * o[F_DY]);
	if (l > 1e-4f) {
		o[F_DX] /= l;
		o[F_DY] /= l;
	} else {
		o[F_DX] = w[F_DX];
		o[F_DY] = w[F_DY];
	}
}

// How far sample i of a walk that rejoins at j (0: never, it ran to the end)
// is faded back into the line.
static float gridFade(int i, int j) {
	if (j <= 0) return 0.0f;
	const int r = j < AILINE_GRID_RAMP ? j : AILINE_GRID_RAMP;
	const float f = 1.0f - (float)(j - i) / (float)r;
	return f < 0.0f ? 0.0f : f;
}

// The grid's road put in the line where the grid cannot reach the line's.
// lo/hi are scratch. Returns how many samples it replaced.
static int gridRoad(AiLine* t, const TrackRouteSet* s, const float* at, const float* gridXY,
		int nGrid, float startZ, const AiLineWalls* walls, float* lo, float* hi,
		void (*log)(const char* msg)) {
	char b[288];
	if (!gridXY || nGrid <= 0 || t->n < 4 * AILINE_GRID_RAMP) return 0;
	float G[2] = { 0.0f, 0.0f };
	for (int i = 0; i < nGrid; i++) {
		G[0] += gridXY[2 * i];
		G[1] += gridXY[2 * i + 1];
	}
	G[0] /= (float)nGrid;
	G[1] /= (float)nGrid;
	int kG = 0;
	float near = 1e30f;
	for (int k = 0; k < t->n; k++) {
		const float dx = at[4 * k] - G[0], dy = at[4 * k + 1] - G[1];
		const float dz = t->v[k * F_COUNT + F_Z] - startZ;
		if (dx * dx + dy * dy + dz * dz < near) {
			near = dx * dx + dy * dy + dz * dz;
			kG = k;
		}
	}
	const float* a = at + 4 * kG;
	const float gl = (G[0] - a[0]) * a[2] + (G[1] - a[1]) * a[3];
	const int ahead = (int)(AILINE_GRID_AHEAD / t->step);
	int cut = 0;
	for (int j = 1; j <= ahead && !cut; j++) {
		if (!t->loop && kG + j >= t->n) break;
		const float* o = t->v + wrapIndex(t, kG + j) * F_COUNT;
		const float reach = AILINE_GRID_SLACK + AILINE_GRID_STEER * (float)j * t->step;
		if (gl < o[F_LEFT] - reach || gl > o[F_RIGHT] + reach) cut = j;
	}
	if (!cut) return 0;

	// The posted roads the grid's centre is on, the race's way.
	const float D[2] = { -a[3], a[2] };
	int cj[AILINE_GRID_CANDS];
	float cm[AILINE_GRID_CANDS];
	int nc = 0;
	for (int i = 0; i + 1 < s->nPts; i++) {
		if (!gridDrives(s, i, D)) continue;
		const TRPoint* p = &s->pts[i];
		const TRPoint* e = &s->pts[i + 1];
		const float ex = e->x - p->x, ey = e->y - p->y;
		const float l = sqrtf(ex * ex + ey * ey);
		const float tx = ex / l, ty = ey / l;
		const float rx = G[0] - p->x, ry = G[1] - p->y;
		const float along = rx * tx + ry * ty;
		float u = along / l;
		u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
		const float over = along < 0.0f ? -along : (along > l ? along - l : 0.0f);
		const float lat = rx * ty - ry * tx;
		const float hl = lerpf(p->halfL, e->halfL, u), hr = lerpf(p->halfR, e->halfR, u);
		const float out = lat > hr ? lat - hr : (lat < -hl ? -hl - lat : 0.0f);
		const float miss = sqrtf(out * out + over * over);
		if (miss > AILINE_GRID_NEAR || fabsf(lerpf(p->z, e->z, u) - startZ) > AILINE_GRID_Z)
			continue;
		int slot = nc;
		if (nc < AILINE_GRID_CANDS) {
			nc++;
		} else {
			slot = 0;
			for (int c = 1; c < nc; c++)
				if (cm[c] > cm[slot]) slot = c;
			if (cm[slot] <= miss) continue;
		}
		cj[slot] = i;
		cm[slot] = miss;
	}

	const int limF = t->loop ? t->n / 3 : t->n - 1 - kG;
	const int limB = t->loop ? t->n / 3 : kG;
	const int lim = limF > limB ? limF : limB;
	GridVtx* v = (GridVtx*)malloc(sizeof(GridVtx) * AILINE_GRID_WALK);
	float* lat = (float*)malloc(sizeof(float) * (size_t)(lim + 1));
	float* vu = (float*)malloc(sizeof(float) * (size_t)(lim + 1));
	int* vi = (int*)malloc(sizeof(int) * (size_t)(lim + 1));
	AiLineWall* buf = (AiLineWall*)malloc(sizeof(AiLineWall) * AILINE_WALL_GATHER);
	if (!v || !lat || !vu || !vi || !buf) {
		free(v); free(lat); free(vu); free(vi); free(buf);
		return 0;
	}
	// Each road walked both ways; its racing line has to pass the grid.
	int res[AILINE_GRID_CANDS][2];
	for (int c = 0; c < nc; c++) {
		const bool up = gridDrives(s, cj[c], D) < 0;
		for (int w = 0; w < 2; w++) {
			int r = gridFollow(s, t, at, cj[c], up, w == 0, kG, w == 0 ? limF : limB, v, lat,
				vi, vu);
			if (r >= 0 && fabsf(lat[0] - gl) > AILINE_GRID_HOLD) r = -1;
			res[c][w] = r;
		}
	}
	// One road both ways where there is one, the shortest stretch; else the
	// nearest rejoining each way.
	int bf = -1, bb = -1;
	for (int c = 0; c < nc; c++)
		if (res[c][0] > 0 && res[c][1] >= 0 &&
				(bf < 0 || res[c][0] + res[c][1] < res[bf][0] + res[bf][1]))
			bf = bb = c;
	if (bf < 0) {
		for (int c = 0; c < nc; c++) {
			if (res[c][0] > 0 && (bf < 0 || res[c][0] < res[bf][0])) bf = c;
			if (res[c][1] >= 0 && (bb < 0 || res[c][1] < res[bb][1])) bb = c;
		}
	}
	if (bf < 0 || bb < 0) {
		if (log) {
			snprintf(b, sizeof(b), "ai line: the grid is %+.1f m off the race route at %.0f m, "
				"off the line's road %.0f m on, and none of the %d posted road(s) under it "
				"rejoins the line -- left as matched", gl, (float)kG * t->step,
				(float)cut * t->step, nc);
			log(b);
		}
		free(v); free(lat); free(vu); free(vi); free(buf);
		return 0;
	}
	const int iF = res[bf][0], iB = res[bb][1];
	const int jF = iF <= limF ? iF : 0, jB = iB <= limB ? iB : 0;
	float w[F_COUNT];
	// Back first: each re-walk reads the line it may rejoin, and the forward
	// stretch starts at the grid's own sample, which the backward walk's join
	// test starts from -- written first, the backward walk found the line
	// already on it there, rejoined one sample back, and the rest of the
	// backward stretch was filled from the forward walk's crossings (4002F,
	// a 15 m swing in 16 m behind the grid). Neither stretch reaches the other.
	gridFollow(s, t, at, cj[bb], gridDrives(s, cj[bb], D) < 0, false, kG, limB, v, lat, vi, vu);
	for (int i = 1; i < iB; i++) {
		const int k = wrapIndex(t, kG - i);
		gridFill(w, at, k, v, lat[i], vi[i], vu[i], walls, buf);
		gridPut(t, k, w, gridFade(i, jB));
	}
	gridFollow(s, t, at, cj[bf], gridDrives(s, cj[bf], D) < 0, true, kG, limF, v, lat, vi, vu);
	for (int i = 0; i < iF; i++) {
		const int k = wrapIndex(t, kG + i);
		gridFill(w, at, k, v, lat[i], vi[i], vu[i], walls, buf);
		gridPut(t, k, w, gridFade(i, jF));
	}

	// The walls the matched line went through there are not the road's.
	const float d0 = (float)(kG - iB) * t->step, d1 = (float)(kG + iF) * t->step;
	int nw = 0;
	for (int i = 0; i < t->nWall; i++) {
		const float x = t->wall[i].dist;
		const bool in = (x >= d0 && x <= d1) ||
			(t->loop && ((x + t->len >= d0 && x + t->len <= d1) ||
			(x - t->len >= d0 && x - t->len <= d1)));
		if (!in) t->wall[nw++] = t->wall[i];
	}
	t->nWall = nw;
	rampEdges(t, lo, hi);
	if (log) {
		float wrap0 = d0, wrap1 = d1;
		if (t->loop) {
			wrap0 = fmodf(d0 + t->len, t->len);
			wrap1 = fmodf(d1, t->len);
		}
		snprintf(b, sizeof(b), "ai line: the grid is %+.1f m off the race route at %.0f m, off "
			"the line's road %.0f m on -- the line follows the grid's road (route %d, back "
			"by route %d) from %.0f m to %.0f m", gl, (float)kG * t->step,
			(float)cut * t->step, (int)s->pts[cj[bf]].route, (int)s->pts[cj[bb]].route,
			wrap0, wrap1);
		log(b);
	}
	free(v); free(lat); free(vu); free(vi); free(buf);
	return iF + iB - 1;
}

// PORT (TASKS.md #280): the road's edges take in the grid. The slots are where
// the race puts the field, so they are road, but the posted roads the line
// matched there need not cover the grid: 4104F's line goes from one road to
// another 10 m past the start, the two overlap by 3 m, and rampEdges, closing
// the edges in ahead of that, left a 3.3 m road over a grid 12 m wide between
// walls 35 m apart. The car slotted 6.8 m right of the line was held to its
// edge, its path ran across the car ahead of it, and it sat on the grid for
// 3.7 s while the field pulled away -- 26 cars of 10 grids started outside
// their road. So each slot beside the line widens the edge on its side to
// keep it AILINE_GRID_EDGE in, which is opponent.c's LINE_EDGE_MARGIN, and the
// widening falls off at AILINE_STEP_RATE ahead of it until the edge the line
// had already holds it.
//
// It runs last, after the wall pass, both edge ramps and the grid's road:
// rampEdges reopens an edge after a narrowing only at AILINE_STEP_RATE, and a
// wall 20 m behind 4101F's grid still held Bryce's edge 5 m inside his slot
// when the ramp got there -- but no car on the grid came through that
// narrowing. So the walls are asked again here, at each sample, about where
// the car's middle would be on the widened edge: the edge stays inside that
// gap, the widening ends where the line is in another gap (it never reaches
// across a median to the line), and it closes in ahead of a wall at
// AILINE_STEP_RATE, as rampEdges does. A slot short of a sprint line's first
// sample is not beside it (opponent.c's shortOfStart).
#define AILINE_GRID_EDGE   1.2f
#define AILINE_GRID_BEHIND 2       // samples behind a slot's own it holds, a car being a little either side
#define AILINE_GRID_HOLDS  96      // samples ahead it may hold at most
static void gridHold(AiLine* t, const float* at, const float* gridXY, int nGrid, float startZ,
		const AiLineWalls* walls) {
	if (!gridXY || nGrid <= 0) return;
	const float fall = AILINE_STEP_RATE * t->step;
	AiLineWall* buf = NULL;
	if (walls && walls->near)
		buf = (AiLineWall*)malloc(sizeof(AiLineWall) * AILINE_WALL_GATHER);
	float e[AILINE_GRID_BEHIND + AILINE_GRID_HOLDS];
	for (int i = 0; i < nGrid; i++) {
		const float gx = gridXY[2 * i], gy = gridXY[2 * i + 1];
		int kg = 0;
		float near = 1e30f;
		for (int k = 0; k < t->n; k++) {
			const float dx = at[4 * k] - gx, dy = at[4 * k + 1] - gy;
			const float dz = t->v[k * F_COUNT + F_Z] - startZ;
			if (dx * dx + dy * dy + dz * dz < near) {
				near = dx * dx + dy * dy + dz * dz;
				kg = k;
			}
		}
		const float* a = at + 4 * kg;
		const float along = -(gx - a[0]) * a[3] + (gy - a[1]) * a[2];
		if (fabsf(along) > t->step) continue;
		const float lat = (gx - a[0]) * a[2] + (gy - a[1]) * a[3];
		// The edge on the slot's side, as -1 (left) or +1 (right): s * e is how
		// far out it is, and the widened edge is the larger.
		const float sgn = lat < t->v[kg * F_COUNT + F_LINE] ? -1.0f : 1.0f;
		const int edge = sgn < 0.0f ? F_LEFT : F_RIGHT;
		int m = 0;
		for (; m < AILINE_GRID_BEHIND + AILINE_GRID_HOLDS; m++) {
			const int j = m - AILINE_GRID_BEHIND;
			if (!t->loop && (kg + j < 0 || kg + j >= t->n)) {
				if (j < 0) { e[m] = -1e30f * sgn; continue; }
				break;
			}
			const int k = wrapIndex(t, kg + j);
			const float* o = t->v + k * F_COUNT;
			float w = lat + sgn * (AILINE_GRID_EDGE - fall * (float)(j < 0 ? 0 : j));
			if (sgn * w <= sgn * o[edge]) break;   // the line's own edge holds it from here
			bool last = false;
			if (buf) {
				WallGaps g;
				float roadZ;
				sampleGaps(walls, buf, at + 4 * k, o[F_Z], o[F_LINE], &g, &roadZ);
				const float mid = w - sgn * AILINE_GRID_EDGE;
				int gi = -1;
				float gd = 1e30f;
				for (int q = 0; q < g.n; q++) {
					const float d = g.a[q] > mid ? g.a[q] - mid : (g.b[q] < mid ? mid - g.b[q] : 0.0f);
					if (d < gd) { gd = d; gi = q; }
				}
				if (gi < 0 || o[F_LINE] < g.a[gi] - AILINE_WALL_MARGIN ||
						o[F_LINE] > g.b[gi] + AILINE_WALL_MARGIN) {
					w = o[F_LINE];
					last = true;
				} else if (sgn < 0.0f && g.a[gi] > -AILINE_WALL_REACH &&
						g.a[gi] - AILINE_WALL_MARGIN + AILINE_WALL_EDGE > w) {
					w = g.a[gi] - AILINE_WALL_MARGIN + AILINE_WALL_EDGE;
				} else if (sgn > 0.0f && g.b[gi] < AILINE_WALL_REACH &&
						g.b[gi] + AILINE_WALL_MARGIN - AILINE_WALL_EDGE < w) {
					w = g.b[gi] + AILINE_WALL_MARGIN - AILINE_WALL_EDGE;
				}
			}
			e[m] = w;
			if (last) { m++; break; }
		}
		// Closed in ahead of whatever stops it, the way the field drives.
		for (int q = m - 2; q >= 0; q--)
			if (sgn * e[q] > sgn * e[q + 1] + fall) e[q] = e[q + 1] + sgn * fall;
		for (int q = 0; q < m; q++) {
			const int j = q - AILINE_GRID_BEHIND;
			if (!t->loop && (kg + j < 0 || kg + j >= t->n)) continue;
			float* o = t->v + wrapIndex(t, kg + j) * F_COUNT;
			if (sgn * e[q] > sgn * o[edge]) o[edge] = e[q];
		}
	}
	free(buf);
}

// PORT (TASKS.md #280): the line is kept as laterals across the race route,
// and a lateral is only the across part of where the road's line was matched.
// On the inside of a route corner tighter than the line is far from the route
// the world points run backwards: 4101F's line is 15 m inside a route corner
// of 11 m at 702 m, ran 9 m back east and crossed itself, and a car finding
// itself on it was turned round. Retail's line is its route points in the
// world and never runs back. Where the line's world points run back against
// the race route, the loop is cut where the line crosses itself -- the
// widest crossing, or where it comes nearest itself, within
// AILINE_FOLD_REACH samples of the fold either way -- the samples between
// spread along the cut, its corner rounded over AILINE_FOLD_ROUND samples,
// and the laterals put back from the world points.
#define AILINE_FOLD_REACH   16      // samples either side of a fold searched for where the line crosses
#define AILINE_FOLD_NEAR    0.5f    // metres apart two stretches of the line count as crossing
#define AILINE_FOLD_ROUND   2       // samples either side of a cut its corner is rounded over

static float unit01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// The nearest points of segments p0-p1 and q0-q1, as fractions along each;
// the squared distance between them.
static float segNearest(const float* p0, const float* p1, const float* q0, const float* q1,
		float* outS, float* outT) {
	const float ux = p1[0] - p0[0], uy = p1[1] - p0[1];
	const float vx = q1[0] - q0[0], vy = q1[1] - q0[1];
	const float wx = p0[0] - q0[0], wy = p0[1] - q0[1];
	const float a = ux * ux + uy * uy, b = ux * vx + uy * vy, c = vx * vx + vy * vy;
	const float d = ux * wx + uy * wy, e = vx * wx + vy * wy;
	float s = 0.0f, u = 0.0f;
	if (a < 1e-8f && c < 1e-8f) {
	} else if (a < 1e-8f) {
		u = unit01(e / c);
	} else if (c < 1e-8f) {
		s = unit01(-d / a);
	} else {
		const float den = a * c - b * b;
		s = den > 1e-8f ? unit01((b * e - c * d) / den) : 0.0f;
		u = (b * s + e) / c;
		if (u < 0.0f) { u = 0.0f; s = unit01(-d / a); }
		else if (u > 1.0f) { u = 1.0f; s = unit01((b - d) / a); }
	}
	const float dx = wx + ux * s - vx * u, dy = wy + uy * s - vy * u;
	*outS = s;
	*outT = u;
	return dx * dx + dy * dy;
}

static float* lineXY(AiLine* t, int k) { return t->v + wrapIndex(t, k) * F_COUNT + F_WX; }

// How far the line's world point moves along the race route from sample k to the next.
static float foldAdvance(AiLine* t, const float* at, int k) {
	const float* p = lineXY(t, k);
	const float* q = lineXY(t, k + 1);
	const float* a = at + 4 * wrapIndex(t, k);
	return (q[0] - p[0]) * -a[3] + (q[1] - p[1]) * a[2];
}

static int unfold(AiLine* t, const float* at) {
	const int last = t->loop ? t->n : t->n - 1;   // segments: sample k to k + 1
	enum { WIN = 3 * AILINE_FOLD_REACH + 2 * AILINE_FOLD_ROUND + 8 };
	float xy[2 * WIN], sm[2 * WIN];
	int cuts = 0;
	for (int k = 0; k < last; k++) {
		if (foldAdvance(t, at, k) >= 0.0f) continue;
		int b = k;
		while (b + 1 < last && b - k < AILINE_FOLD_REACH && foldAdvance(t, at, b + 1) < 0.0f) b++;
		// A segment before the fold and one after it: the widest pair that
		// cross, else the nearest.
		float nearest = 1e30f, cs = 0.0f, ct = 0.0f;
		int ci = -1, cj = -1;
		bool crossed = false;
		for (int i = k - 1; i >= k - AILINE_FOLD_REACH && (t->loop || i >= 0); i--)
		for (int j = b + 1; j <= b + AILINE_FOLD_REACH && (t->loop || j < last); j++) {
			float s, u;
			const float d2 = segNearest(lineXY(t, i), lineXY(t, i + 1), lineXY(t, j),
				lineXY(t, j + 1), &s, &u);
			const bool near = d2 <= AILINE_FOLD_NEAR * AILINE_FOLD_NEAR;
			if (near ? (!crossed || j - i > cj - ci) : (!crossed && d2 < nearest)) {
				crossed = near;
				nearest = d2;
				ci = i; cj = j; cs = s; ct = u;
			}
		}
		if (ci < 0) { k = b; continue; }
		// Samples ci + 1 .. cj along ci -> the cut -> cj + 1.
		const float* P = lineXY(t, ci);
		const float* Q = lineXY(t, cj + 1);
		const float* p1 = lineXY(t, ci + 1);
		const float* q0 = lineXY(t, cj);
		const float* q1 = lineXY(t, cj + 1);
		float X[2];
		X[0] = 0.5f * (lerpf(P[0], p1[0], cs) + lerpf(q0[0], q1[0], ct));
		X[1] = 0.5f * (lerpf(P[1], p1[1], cs) + lerpf(q0[1], q1[1], ct));
		const float l0 = sqrtf((X[0] - P[0]) * (X[0] - P[0]) + (X[1] - P[1]) * (X[1] - P[1]));
		const float l1 = sqrtf((Q[0] - X[0]) * (Q[0] - X[0]) + (Q[1] - X[1]) * (Q[1] - X[1]));
		const int i0 = ci + 1 - AILINE_FOLD_ROUND, i1 = cj + AILINE_FOLD_ROUND;
		const int m = i1 - i0 + 1;
		if (m + 2 * AILINE_FOLD_ROUND > WIN) { k = b; continue; }
		// xy: samples i0 - ROUND .. i1 + ROUND, the cut put in.
		for (int q = 0; q < m + 2 * AILINE_FOLD_ROUND; q++) {
			const int sk = i0 - AILINE_FOLD_ROUND + q;
			const float* w = lineXY(t, sk);
			xy[2 * q] = w[0];
			xy[2 * q + 1] = w[1];
			if (sk > ci && sk <= cj) {
				const float f = (float)(sk - ci) / (float)(cj + 1 - ci) * (l0 + l1);
				if (f <= l0 && l0 > 1e-6f) {
					xy[2 * q] = lerpf(P[0], X[0], f / l0);
					xy[2 * q + 1] = lerpf(P[1], X[1], f / l0);
				} else {
					const float g = l1 > 1e-6f ? (f - l0) / l1 : 1.0f;
					xy[2 * q] = lerpf(X[0], Q[0], g);
					xy[2 * q + 1] = lerpf(X[1], Q[1], g);
				}
			}
		}
		// Rounded: samples i0 .. i1 each the mean of ROUND either side.
		for (int q = 0; q < m; q++) {
			float sx = 0.0f, sy = 0.0f;
			for (int r = 0; r <= 2 * AILINE_FOLD_ROUND; r++) {
				sx += xy[2 * (q + r)];
				sy += xy[2 * (q + r) + 1];
			}
			sm[2 * q] = sx / (float)(2 * AILINE_FOLD_ROUND + 1);
			sm[2 * q + 1] = sy / (float)(2 * AILINE_FOLD_ROUND + 1);
		}
		for (int q = 0; q < m; q++) {
			if (!t->loop && (i0 + q < 0 || i0 + q >= t->n)) continue;
			float* w = lineXY(t, i0 + q);
			w[0] = sm[2 * q];
			w[1] = sm[2 * q + 1];
		}
		// The laterals and headings from the world points.
		for (int q = i0; q <= i1; q++) {
			if (!t->loop && (q < 0 || q >= t->n)) continue;
			float* o = t->v + wrapIndex(t, q) * F_COUNT;
			const float* a = at + 4 * wrapIndex(t, q);
			o[F_LINE] = (o[F_WX] - a[0]) * a[2] + (o[F_WY] - a[1]) * a[3];
			if (o[F_LEFT] > o[F_LINE]) o[F_LEFT] = o[F_LINE];
			if (o[F_RIGHT] < o[F_LINE]) o[F_RIGHT] = o[F_LINE];
			const float* pa = lineXY(t, q - 1);
			const float* pb = lineXY(t, q + 1);
			const float dx = pb[0] - pa[0], dy = pb[1] - pa[1];
			const float l = sqrtf(dx * dx + dy * dy);
			if (l > 1e-4f) {
				o[F_DX] = dx / l;
				o[F_DY] = dy / l;
			}
		}
		cuts++;
		k = (cj > k ? cj : k);
		if (k >= last) break;
	}
	return cuts;
}

// PORT (TASKS.md #280): the line round a bend no car can follow. The line is
// laterals across the race route, and where the route bends hard itself its
// normals all but meet: 4087B's hairpin route bends on 8 m, and the line 5 -
// 8 m inside it took 4 m of route to 1.3 m of line at 480 m and turned 90
// degrees in 10 m. The field arced round it 5 - 8 m outside, on the road but
// off every offset, and a first-lap pack met itself there (Eric backed out
// from Steve's door at 488 m). No fair across the route can open it out --
// the bending of points on normals that meet is least where they meet, so it
// bunches them -- and retail's FuturePoints interpolates its corners itself
// (FinishInterpolatingCorners, 0x80035C84, from Update), which is not ported.
// So wherever the line's world points bend tighter than AILINE_CORNER_R over
// AILINE_CORNER_SPAN of their own metres either side, the stretch,
// AILINE_FAIR_PAD samples either side, is refitted as free world points: the
// least bending (their second difference), near where they were only outside
// the bend, each inside its own sample's road AILINE_CORNER_EDGE in --
// opponent.c's LINE_EDGE_MARGIN and LINE_CHORD_SAG -- or no further out than
// it already is, but free along it, so the points spread round the bend.
#define AILINE_CORNER_SPAN  8.0f
#define AILINE_CORNER_R     12.0f
#define AILINE_CORNER_EDGE  2.2f
#define AILINE_CORNER_HELD  1e4     // how hard a point put past its bound is held to it

// One stretch refitted (AILINE_CORNER_*): the m samples from s, unwrapped, as
// world points, x and y interleaved. The bends' normal equations are banded
// and solved directly; a point the solution puts past its bound across the
// race route is held at it and the rest solved again, and one held where the
// bending would sooner pull it back in is let go.
static void cornerRun(AiLine* t, const float* at, const float* lo, const float* hi,
		const u8* flags, int s, int m) {
	static const double c[3] = { 1.0, -2.0, 1.0 };
	enum { B = 5 };   // the band's half-width: x reaches the next point but one's x at 4
	const int N = 2 * m;
	double* mem = (double*)malloc(sizeof(double) * (size_t)(2 * N * (B + 1) + 3 * N + 2 * (m + 4)));
	u8* state = (u8*)calloc((size_t)m, 1);   // 1 held at lo, 2 at hi
	if (!mem || !state) {
		free(mem); free(state);
		return;
	}
	double *H = mem, *L = H + N * (B + 1), *rhs = L + N * (B + 1), *y = rhs + N, *z = y + N;
	double* p = z + N;   // the points as they were at local -2 .. m + 1, [2 * (i + 2)]
	for (int i = -2; i < m + 2; i++) {
		const int k = fairIndex(t, s + i);
		const float* w = k >= 0 ? lineXY(t, k) : NULL;
		p[2 * (i + 2)] = w ? w[0] : 0.0;
		p[2 * (i + 2) + 1] = w ? w[1] : 0.0;
	}
	bool solved = false;
	for (int iter = 0; iter < AILINE_FAIR_ITER; iter++) {
		memset(H, 0, sizeof(double) * (size_t)(N * (B + 1)));
		memset(rhs, 0, sizeof(double) * (size_t)N);
		for (int j = -1; j <= m; j++) {
			bool in = true;
			for (int q = 0; q < 3; q++) in = in && fairIndex(t, s + j - 1 + q) >= 0;
			if (!in) continue;
			for (int d = 0; d < 2; d++) {
				double f = 0.0;   // the fixed points' part of this bend
				for (int q = 0; q < 3; q++) {
					const int i = j - 1 + q;
					if (i < 0 || i >= m) f += c[q] * p[2 * (i + 2) + d];
				}
				for (int a = 0; a < 3; a++) {
					const int ia = j - 1 + a;
					if (ia < 0 || ia >= m) continue;
					const int r = 2 * ia + d;
					rhs[r] -= c[a] * f;
					for (int b = 0; b < 3; b++) {
						const int ib = j - 1 + b;
						if (ib < 0 || ib >= m || 2 * ib + d > r) continue;
						H[r * (B + 1) + (r - 2 * ib - d)] += c[a] * c[b];
					}
				}
			}
		}
		for (int i = 0; i < m; i++) {
			const int k = fairIndex(t, s + i);
			const double w = ((flags[k] & 4) ? 0.0 : AILINE_FAIR_HOLD) + 1e-4;
			for (int d = 0; d < 2; d++) {
				H[(2 * i + d) * (B + 1)] += w;
				rhs[2 * i + d] += w * p[2 * (i + 2) + d];
			}
			if (!state[i]) continue;
			const float* a = at + 4 * k;
			const double nx = a[2], ny = a[3];
			const double b = (state[i] == 1 ? lo[k] : hi[k]) + a[0] * nx + a[1] * ny;
			H[2 * i * (B + 1)] += AILINE_CORNER_HELD * nx * nx;
			H[(2 * i + 1) * (B + 1)] += AILINE_CORNER_HELD * ny * ny;
			H[(2 * i + 1) * (B + 1) + 1] += AILINE_CORNER_HELD * nx * ny;
			rhs[2 * i] += AILINE_CORNER_HELD * nx * b;
			rhs[2 * i + 1] += AILINE_CORNER_HELD * ny * b;
		}
		// Cholesky in the band: [r * (B + 1) + e] is row r, column r - e.
		bool ok = true;
		for (int r = 0; r < N && ok; r++) {
			for (int e = r < B ? r : B; e >= 0; e--) {
				const int col = r - e;
				double v = H[r * (B + 1) + e];
				for (int q = r - B > 0 ? r - B : 0; q < col; q++)
					v -= L[r * (B + 1) + (r - q)] * L[col * (B + 1) + (col - q)];
				if (e > 0) L[r * (B + 1) + e] = v / L[col * (B + 1)];
				else if (v > 1e-12) L[r * (B + 1)] = sqrt(v);
				else ok = false;
			}
		}
		if (!ok) break;
		for (int r = 0; r < N; r++) {
			double v = rhs[r];
			for (int q = r - B > 0 ? r - B : 0; q < r; q++) v -= L[r * (B + 1) + (r - q)] * y[q];
			y[r] = v / L[r * (B + 1)];
		}
		for (int r = N - 1; r >= 0; r--) {
			double v = y[r];
			for (int q = r + 1; q < N && q <= r + B; q++) v -= L[q * (B + 1) + (q - r)] * z[q];
			z[r] = v / L[r * (B + 1)];
		}
		solved = true;

		int added = 0;
		for (int i = 0; i < m; i++) {
			if (state[i]) continue;
			const int k = fairIndex(t, s + i);
			const float* a = at + 4 * k;
			const double l = (z[2 * i] - a[0]) * a[2] + (z[2 * i + 1] - a[1]) * a[3];
			if (l < lo[k] - 1e-4) { state[i] = 1; added++; }
			else if (l > hi[k] + 1e-4) { state[i] = 2; added++; }
		}
		if (added) continue;
		// Which held point the bending pulls hardest back inside its bounds.
		int worst = -1;
		double most = 1e-7;
		for (int i = 0; i < m; i++) {
			if (!state[i]) continue;
			const int k = fairIndex(t, s + i);
			const float* a = at + 4 * k;
			const double l = (z[2 * i] - a[0]) * a[2] + (z[2 * i + 1] - a[1]) * a[3];
			const double pull = state[i] == 1 ? l - lo[k] : hi[k] - l;
			if (pull > most) { most = pull; worst = i; }
		}
		if (worst < 0) break;
		state[worst] = 0;
	}
	if (solved) {
		for (int i = 0; i < m; i++) {
			const int k = fairIndex(t, s + i);
			const float* a = at + 4 * k;
			float* o = t->v + k * F_COUNT;
			const double l = (z[2 * i] - a[0]) * a[2] + (z[2 * i + 1] - a[1]) * a[3];
			const double in = l < lo[k] ? lo[k] : (l > hi[k] ? hi[k] : l);
			o[F_WX] = (float)(z[2 * i] + (in - l) * a[2]);
			o[F_WY] = (float)(z[2 * i + 1] + (in - l) * a[3]);
			o[F_LINE] = (float)in;
			if (o[F_LEFT] > o[F_LINE]) o[F_LEFT] = o[F_LINE];
			if (o[F_RIGHT] < o[F_LINE]) o[F_RIGHT] = o[F_LINE];
		}
		// The headings from the world points, one sample past either end.
		for (int i = -1; i <= m; i++) {
			const int k = fairIndex(t, s + i);
			if (k < 0) continue;
			float* o = t->v + k * F_COUNT;
			const float* pa = lineXY(t, s + i - 1);
			const float* pb = lineXY(t, s + i + 1);
			const float dx = pb[0] - pa[0], dy = pb[1] - pa[1];
			const float l = sqrtf(dx * dx + dy * dy);
			if (l > 1e-4f) {
				o[F_DX] = dx / l;
				o[F_DY] = dy / l;
			}
		}
	}
	free(mem);
	free(state);
}

// Returns how many samples it found on bends no car can follow.
static int cornerPass(AiLine* t, const float* at) {
	const int n = t->n;
	float* s = (float*)malloc(sizeof(float) * (size_t)(n + 1));
	float* lo = (float*)malloc(sizeof(float) * (size_t)n);
	float* hi = (float*)malloc(sizeof(float) * (size_t)n);
	u8* flags = (u8*)calloc((size_t)n, 1);
	if (!s || !lo || !hi || !flags) {
		free(s); free(lo); free(hi); free(flags);
		return 0;
	}
	// The line's own metres to each sample, unwrapped; s[n] round to a loop's start.
	s[0] = 0.0f;
	for (int k = 1; k <= n; k++) {
		const float* p = lineXY(t, k - 1);
		const float* q = lineXY(t, k < n ? k : 0);
		const float dx = q[0] - p[0], dy = q[1] - p[1];
		s[k] = s[k - 1] + ((k < n || t->loop) ? sqrtf(dx * dx + dy * dy) : 0.0f);
	}
	const float lapLen = s[n];
	// The metres along to sample u, which may be off either end of a loop.
	#define CORNER_AT(u) (s[wrapIndex(t, (u))] + lapLen * (float)(((u) - wrapIndex(t, (u))) / n))
	int tight = 0;
	for (int k = 0; k < n; k++) {
		int a = k, c = k;
		while ((t->loop || a > 0) && k - a < n / 4 &&
				CORNER_AT(k) - CORNER_AT(a - 1) <= AILINE_CORNER_SPAN) a--;
		while ((t->loop || c < n - 1) && c - k < n / 4 &&
				CORNER_AT(c + 1) - CORNER_AT(k) <= AILINE_CORNER_SPAN) c++;
		if (a == k || c == k) continue;
		const float* p = lineXY(t, a);
		const float* q = lineXY(t, k);
		const float* r = lineXY(t, c);
		const float ab = sqrtf((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]));
		const float bc = sqrtf((r[0] - q[0]) * (r[0] - q[0]) + (r[1] - q[1]) * (r[1] - q[1]));
		const float ca = sqrtf((p[0] - r[0]) * (p[0] - r[0]) + (p[1] - r[1]) * (p[1] - r[1]));
		const float cross = fabsf((q[0] - p[0]) * (r[1] - q[1]) - (q[1] - p[1]) * (r[0] - q[0]));
		if (cross > 1e-6f && ab * bc * ca < 2.0f * AILINE_CORNER_R * cross) {
			flags[k] = 8 | 4;
			tight++;
		}
	}
	#undef CORNER_AT
	if (tight) {
		for (int k = 0; k < n; k++) {
			const float* o = t->v + k * F_COUNT;
			lo[k] = fminf(o[F_LEFT] + AILINE_CORNER_EDGE, o[F_LINE]);
			hi[k] = fmaxf(o[F_RIGHT] - AILINE_CORNER_EDGE, o[F_LINE]);
		}
		fairLine(t, at, lo, hi, flags, cornerRun);
	}
	free(s); free(lo); free(hi); free(flags);
	return tight;
}

// Sample o as segment `seg` has it `u` along, driven up the route or down:
// its racing line, the road's centre and edges, height and heading, all as
// laterals across the race route at `ar` (its point and right normal).
static void matchFill(float* o, const float* ar, const TRPoint* pts, int seg, float u, bool up) {
	const TRPoint* a = &pts[seg];
	const TRPoint* e = &pts[seg + 1];
	const float ex = e->x - a->x, ey = e->y - a->y;
	const float l = sqrtf(ex * ex + ey * ey);
	const float tx = ex / l, ty = ey / l;
	const float qx = a->x + ex * u, qy = a->y + ey * u;
	const float nx = ar[2], ny = ar[3];             // right of the race
	const float Rx = ty, Ry = -tx;                  // right of the route
	const float hl = lerpf(a->halfL, e->halfL, u);
	const float hr = lerpf(a->halfR, e->halfR, u);
	// Line 1 is measured to the right of the route's backward direction.
	const float off = up ? -lerpf(a->off[1], e->off[1], u) : lerpf(a->off[0], e->off[0], u);
	const float latQ = (qx - ar[0]) * nx + (qy - ar[1]) * ny;
	const float latR = latQ + (Rx * nx + Ry * ny) * hr;
	const float latL = latQ - (Rx * nx + Ry * ny) * hl;
	o[F_POINT] = latQ;
	o[F_LINE] = latQ + (Rx * nx + Ry * ny) * off;
	o[F_LEFT] = latL < latR ? latL : latR;
	o[F_RIGHT] = latL < latR ? latR : latL;
	o[F_Z] = lerpf(a->z, e->z, u);
	o[F_DX] = up ? -tx : tx;
	o[F_DY] = up ? -ty : ty;
}

// Where the racing line of route `route`'s segments lo..hi (driven `up` or
// down) crosses the normal at `ar`, nearest segment `near` first: the
// segment, how far along it, and the lateral. False where none does within
// AILINE_WALL_REACH of the race route.
static bool roadCross(const TrackRouteSet* s, int route, int lo, int hi, int near, bool up,
		const float* ar, int* outSeg, float* outU, float* outLat) {
	const float Dx = -ar[3], Dy = ar[2];   // the race's way
	for (int r = 0; r <= hi - lo; r++)
	for (int side = 0; side < 2; side++) {
		const int i = side ? near + r : near - r;
		if ((side && r == 0) || i < lo || i > hi || i + 1 >= s->nPts) continue;
		if ((int)s->pts[i].route != route || (int)s->pts[i + 1].route != route) continue;
		GridVtx A, B;
		gridVtx(s, i, up, 0.0f, &A);
		gridVtx(s, i, up, 1.0f, &B);
		const float s0 = (A.lx - ar[0]) * Dx + (A.ly - ar[1]) * Dy;
		const float s1 = (B.lx - ar[0]) * Dx + (B.ly - ar[1]) * Dy;
		if (s0 * s1 > 0.0f) continue;
		const float u = s0 != s1 ? s0 / (s0 - s1) : 0.0f;
		const float lat = (lerpf(A.lx, B.lx, u) - ar[0]) * ar[2] + (lerpf(A.ly, B.ly, u) - ar[1]) * ar[3];
		if (fabsf(lat) > AILINE_WALL_REACH) continue;
		*outSeg = i;
		*outU = u;
		*outLat = lat;
		return true;
	}
	return false;
}

AiLine* ailine_build(const TrackRouteSet* s, const RaceRoute* route, float startZ,
		const float* gridXY, int nGrid, const AiLineWalls* walls, void (*log)(const char* msg)) {
	char b[288];
	if (!s || !route || s->nPts < 2) return NULL;
	const float len = race_route_length(route);
	if (!(len > 1.0f)) return NULL;

	AiLine* t = (AiLine*)calloc(1, sizeof(AiLine));
	if (!t) return NULL;
	t->loop = race_route_loops(route);
	t->len = len;
	t->n = (int)ceilf(len / AILINE_STEP_MAX) + (t->loop ? 0 : 1);
	if (t->n < 2) t->n = 2;
	t->step = len / (float)(t->loop ? t->n : t->n - 1);
	t->v = (float*)malloc(sizeof(float) * F_COUNT * (size_t)t->n);
	u8* ok = (u8*)calloc((size_t)t->n, 1);
	int* cand = (int*)malloc(sizeof(int) * (size_t)s->nPts);
	// Each sample's race route point and right normal, then the two lines as
	// world points (x, y of the line, x, y of the route point).
	float* at = (float*)malloc(sizeof(float) * 4 * (size_t)t->n);
	float* w = (float*)malloc(sizeof(float) * 4 * (size_t)t->n);
	if (!t->v || !ok || !cand || !at || !w) {
		free(ok);
		free(cand);
		free(at);
		free(w);
		ailine_free(t);
		return NULL;
	}

	const TRPoint* pts = s->pts;
	int nCand = 0, scanAt = -1;
	int prevRoute = -1, matched = 0, switches = 0, upSamples = 0, unposted = 0;
	int offRoad = 0;
	// PORT (TASKS.md #280): FindClosestTrackRoute answers for one car where it
	// is now; the line answers for every sample at once, so its roads are
	// chosen for the whole race route -- the cheapest way along it, each
	// sample's road costing what it did, and each step from one sample's road
	// to the next paying the height change, the jump (AILINE_JUMP_*) and for
	// leaving a route as before. Chosen a sample at a time, the line kept
	// whatever road it had: 4087B's dual road from 3,030 m has a carriageway
	// each way, 4 m apart, and the line took the one against the race (6 more
	// every sample) because the one with it was a 4 m jump away (12, once). It
	// kept it past the median's nose at 3,107 m until it went out of reach at
	// 3,135 m, its 13 m step back to the race's side was ramped across the
	// nose, and the field piled into it.
	MatchState* st = (MatchState*)malloc(sizeof(MatchState) * AILINE_MATCH_STATES * (size_t)t->n);
	u8* nst = (u8*)calloc((size_t)t->n, 1);
	s8* pick = (s8*)malloc((size_t)t->n);
	const bool walled = walls && walls->near;
	AiLineWall* wbuf = walled ? (AiLineWall*)malloc(sizeof(AiLineWall) * AILINE_WALL_GATHER) : NULL;
	if (!st || !nst || !pick || (walled && !wbuf)) {
		free(st);
		free(nst);
		free(pick);
		free(wbuf);
		free(ok);
		free(cand);
		free(at);
		free(w);
		ailine_free(t);
		return NULL;
	}
	// The cheapest way to each of the last sample with roads' roads, and its
	// line, height, route and gap (AILINE_MATCH_WALL_OUT); this sample's go in
	// the other half.
	float dpCost[2][AILINE_MATCH_STATES], dpLine[2][AILINE_MATCH_STATES];
	float dpZ[2][AILINE_MATCH_STATES];
	int dpRoute[2][AILINE_MATCH_STATES], dpGap[2][AILINE_MATCH_STATES];
	WallGaps dpGaps[2];
	int prevN = 0, prevK = -1, cur = 0;
	for (int k = 0; k < t->n; k++) {
		float P[2], D[2];
		if (!race_route_sample(route, (float)k * t->step, 0.0f, P, D)) {
			P[0] = P[1] = 0.0f;
			D[0] = 1.0f;
			D[1] = 0.0f;
		}
		at[4 * k] = P[0];
		at[4 * k + 1] = P[1];
		at[4 * k + 2] = D[1];    // right of the race
		at[4 * k + 3] = -D[0];

		// The segments near enough to matter for the next stretch.
		if (scanAt < 0 || k - scanAt >= AILINE_SCAN_EVERY) {
			scanAt = k;
			nCand = 0;
			for (int i = 0; i + 1 < s->nPts; i++) {
				const TRPoint* a = &pts[i];
				const TRPoint* e = &pts[i + 1];
				if (a->route != e->route) continue;
				const float ex = e->x - a->x, ey = e->y - a->y;
				const float l2 = ex * ex + ey * ey;
				float u = l2 > 1e-6f ? ((P[0] - a->x) * ex + (P[1] - a->y) * ey) / l2 : 0.0f;
				u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
				const float dx = a->x + ex * u - P[0], dy = a->y + ey * u - P[1];
				if (dx * dx + dy * dy <= AILINE_SCAN_REACH * AILINE_SCAN_REACH)
					cand[nCand++] = i;
			}
		}

		// FindClosestTrackRoute's tests: a posted road the sample is on,
		// nearest its own line; else the road it is least outside of. Its
		// second pass relaxes the tests, and so does this one: where no
		// posted road is near -- a junction's connecting routes, the run up
		// to where a race's posting starts -- any road will do.
		MatchState* sk = st + (size_t)k * AILINE_MATCH_STATES;
		float* cost = dpCost[cur];
		float* lineK = dpLine[cur];
		float* zK = dpZ[cur];
		int* routeK = dpRoute[cur];
		int m = 0;
		for (int pass = 0; pass < 2 && m == 0; pass++)
		for (int c = 0; c < nCand; c++) {
			const TRPoint* a = &pts[cand[c]];
			const TRPoint* e = &pts[cand[c] + 1];
			const float ex = e->x - a->x, ey = e->y - a->y;
			const float l = sqrtf(ex * ex + ey * ey);
			if (l < 0.01f) continue;
			const float tx = ex / l, ty = ey / l;
			const float al = tx * D[0] + ty * D[1];
			bool up;
			if (al >= AILINE_ALIGN && (pass || (a->postF | e->postF))) up = false;
			else if (al <= -AILINE_ALIGN && (pass || (a->postB | e->postB))) up = true;
			else continue;
			const float rx = P[0] - a->x, ry = P[1] - a->y;
			const float along = rx * tx + ry * ty;
			float u = along / l;
			u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
			const float over = along < 0.0f ? -along : (along > l ? along - l : 0.0f);
			const float lat = rx * ty - ry * tx;   // right of the route's direction
			const float hl = lerpf(a->halfL, e->halfL, u), hr = lerpf(a->halfR, e->halfR, u);
			const float out = lat > hr ? lat - hr : (lat < -hl ? -hl - lat : 0.0f);
			const float miss = sqrtf(out * out + over * over);
			if (miss > AILINE_REACH) continue;
			const float local = miss + AILINE_NEAR_COST * fabsf(lat) + (up ? AILINE_UP_COST : 0.0f);
			int slot = m;
			if (m == AILINE_MATCH_STATES) {
				slot = 0;
				for (int q = 1; q < m; q++)
					if (cost[q] > cost[slot]) slot = q;
				if (local >= cost[slot]) continue;
			} else {
				m++;
			}
			// Where it puts the line, as a lateral across the race route --
			// the same arithmetic the chosen one does below.
			const float nx = D[1], ny = -D[0];
			const float Rx = ty, Ry = -tx;
			const float off = up ? -lerpf(a->off[1], e->off[1], u) : lerpf(a->off[0], e->off[0], u);
			const float q0 = (a->x + ex * u - P[0]) * nx + (a->y + ey * u - P[1]) * ny;
			sk[slot].seg = cand[c];
			sk[slot].u = u;
			sk[slot].out = out;
			sk[slot].up = up;
			cost[slot] = local;
			lineK[slot] = q0 + (Rx * nx + Ry * ny) * off;
			zK[slot] = lerpf(a->z, e->z, u);
			routeK[slot] = (int)a->route;
		}
		nst[k] = (u8)m;
		if (!m) continue;
		const int pv = cur ^ 1;
		// Each road's line among the walls (AILINE_MATCH_WALL_OUT): its
		// nearest gap, and how far outside it -- nothing past an end of the
		// reach that is no wall.
		WallGaps* g = &dpGaps[cur];
		int* gapK = dpGap[cur];
		g->n = 0;
		if (walled) {
			int bi = 0;
			for (int i = 1; i < prevN; i++)
				if (dpCost[pv][i] < dpCost[pv][bi]) bi = i;
			float roadZ;
			sampleGaps(walls, wbuf, at + 4 * k, prevN ? dpZ[pv][bi] : startZ,
				prevN ? dpLine[pv][bi] : 0.0f, g, &roadZ);
		}
		for (int j = 0; j < m; j++) {
			gapK[j] = -1;
			float outside = 1e30f;
			for (int q = 0; q < g->n; q++) {
				const float l = lineK[j];
				float d = 0.0f;
				if (l < g->a[q] && g->a[q] > -AILINE_WALL_REACH) d = g->a[q] - l;
				else if (l > g->b[q] && g->b[q] < AILINE_WALL_REACH) d = l - g->b[q];
				if (d < outside) { outside = d; gapK[j] = q; }
			}
			if (gapK[j] >= 0) cost[j] += outside * AILINE_MATCH_WALL_OUT;
		}
		// The cheapest way to each from the last sample's roads.
		float least = 1e30f;
		for (int j = 0; j < m; j++) {
			float best;
			int bi = -1;
			if (!prevN) {
				const float dz = fabsf(zK[j] - startZ);
				best = dz > AILINE_Z_FREE ? (dz - AILINE_Z_FREE) * AILINE_Z_COST : 0.0f;
			} else {
				best = 1e30f;
				for (int i = 0; i < prevN; i++) {
					float c = dpCost[pv][i];
					const float dz = fabsf(zK[j] - dpZ[pv][i]);
					if (dz > AILINE_Z_FREE) c += (dz - AILINE_Z_FREE) * AILINE_Z_COST;
					if (routeK[j] == dpRoute[pv][i]) c -= AILINE_SAME_ROUTE;
					const float jump = fabsf(lineK[j] - dpLine[pv][i]);
					if (jump > AILINE_JUMP_FREE) c += (jump - AILINE_JUMP_FREE) * AILINE_JUMP_COST;
					if (prevK == k - 1 && gapK[j] >= 0 && dpGap[pv][i] >= 0 &&
							!gapsMeet(g, gapK[j], &dpGaps[pv], dpGap[pv][i]))
						c += AILINE_MATCH_THROUGH;
					if (c < best) { best = c; bi = i; }
				}
			}
			cost[j] += best;
			sk[j].from = (s8)bi;
			if (cost[j] < least) least = cost[j];
		}
		for (int j = 0; j < m; j++) cost[j] -= least;
		prevN = m;
		prevK = k;
		cur = pv;
	}
	free(wbuf);

	// The way back from the cheapest road at the last sample with any.
	memset(pick, -1, (size_t)t->n);
	{
		int j = -1;
		bool found = false;
		for (int k = t->n - 1; k >= 0; k--) {
			pick[k] = -1;
			if (!nst[k]) continue;
			if (!found) {
				const float* c = dpCost[cur ^ 1];
				j = 0;
				for (int q = 1; q < nst[k]; q++)
					if (c[q] < c[j]) j = q;
				found = true;
			}
			if (j < 0) break;
			pick[k] = (s8)j;
			j = st[(size_t)k * AILINE_MATCH_STATES + j].from;
		}
	}

	for (int k = 0; k < t->n; k++) {
		if (pick[k] < 0) continue;
		const MatchState* ms = st + (size_t)k * AILINE_MATCH_STATES + pick[k];
		// Near enough to score, but not the road the race is on: the race
		// route runs off this road's surface here. Leave the sample
		// unmatched and let the bridge below draw through it.
		if (ms->out > AILINE_OFF_ROAD) {
			offRoad++;
			continue;
		}

		// Everything as laterals across the race route at the sample.
		float* o = t->v + k * F_COUNT;
		const bool bestUp = ms->up != 0;
		const TRPoint* a = &pts[ms->seg];
		const TRPoint* e = &pts[ms->seg + 1];
		matchFill(o, at + 4 * k, pts, ms->seg, ms->u, bestUp);
		ok[k] = 1;
		matched++;
		if (bestUp) upSamples++;
		if (!(bestUp ? (a->postB | e->postB) : (a->postF | e->postF))) unposted++;
		if (prevRoute >= 0 && (int)a->route != prevRoute) switches++;
		prevRoute = (int)a->route;
	}

	// An off-road run on the road matched either side keeps that road (see
	// AILINE_OFF_ROAD): every sample where the road's racing line crosses it,
	// or the whole run is left to the bridge.
	int kept = 0;
	for (int k = 0; k < t->n;) {
		if (ok[k] || pick[k] < 0) {
			k++;
			continue;
		}
		const int k0 = k;
		while (k < t->n && !ok[k] && pick[k] >= 0) k++;
		if (k0 == 0 || k >= t->n || !ok[k0 - 1] || !ok[k]) continue;
		const MatchState* m0 = st + (size_t)(k0 - 1) * AILINE_MATCH_STATES + pick[k0 - 1];
		const MatchState* m1 = st + (size_t)k * AILINE_MATCH_STATES + pick[k];
		const int r = (int)pts[m0->seg].route;
		if ((int)pts[m1->seg].route != r || m0->up != m1->up) continue;
		int lo = m0->seg < m1->seg ? m0->seg : m1->seg;
		int hi = m0->seg < m1->seg ? m1->seg : m0->seg;
		bool keep = true;
		for (int q = k0; q < k && keep; q++) {
			const MatchState* ms = st + (size_t)q * AILINE_MATCH_STATES + pick[q];
			keep = (int)pts[ms->seg].route == r && ms->up == m0->up;
			if (ms->seg < lo) lo = ms->seg;
			if (ms->seg > hi) hi = ms->seg;
		}
		int seg;
		float u, lat;
		for (int q = k0; q < k && keep; q++)
			keep = roadCross(s, r, lo, hi, st[(size_t)q * AILINE_MATCH_STATES + pick[q]].seg,
				m0->up != 0, at + 4 * q, &seg, &u, &lat);
		if (!keep) continue;
		for (int q = k0; q < k; q++) {
			roadCross(s, r, lo, hi, st[(size_t)q * AILINE_MATCH_STATES + pick[q]].seg,
				m0->up != 0, at + 4 * q, &seg, &u, &lat);
			matchFill(t->v + q * F_COUNT, at + 4 * q, pts, seg, u, m0->up != 0);
			ok[q] = 1;
			matched++;
			kept++;
			if (m0->up) upSamples++;
			if (!(m0->up ? (pts[seg].postB | pts[seg + 1].postB)
			             : (pts[seg].postF | pts[seg + 1].postF))) unposted++;
		}
	}
	offRoad -= kept;
	free(st);
	free(nst);
	free(pick);
	free(cand);

	if (matched == 0) {
		if (log) log("ai line: no TrackRoute along the race route");
		free(ok);
		free(at);
		free(w);
		ailine_free(t);
		return NULL;
	}

	// The lines are bridged and smoothed as world points, not as laterals:
	// where the race route itself steps sideways (two TrackPath segments that
	// do not quite meet) a lateral carried across the step would move the
	// line with it.
	for (int k = 0; k < t->n; k++) {
		const float* o = t->v + k * F_COUNT;
		const float* a = at + 4 * k;
		w[4 * k] = a[0] + a[2] * o[F_LINE];
		w[4 * k + 1] = a[1] + a[3] * o[F_LINE];
		w[4 * k + 2] = a[0] + a[2] * o[F_POINT];
		w[4 * k + 3] = a[1] + a[3] * o[F_POINT];
	}

	// Stretches nothing matched take the straight line between the matches
	// either side (round the loop on a circuit, the nearest one past a
	// sprint's end).
	int gaps = 0, gapSamples = 0;
	for (int k = 0; k < t->n; k++) {
		if (ok[k]) continue;
		int j = k;
		while (j < t->n && !ok[j]) j++;
		const int runEnd = j;   // first matched sample after the run, or n
		int lo = k - 1, hi = runEnd;
		if (t->loop) {
			if (lo < 0) { lo = t->n - 1; while (!ok[lo]) lo--; }
			if (hi >= t->n) { hi = 0; while (!ok[hi]) hi++; }
		}
		gaps++;
		gapSamples += runEnd - k;
		// Short enough to call a road, or long enough that only the race
		// route is left to go on.
		const bool chord = runEnd - k <= AILINE_BRIDGE_CHORD;
		if (log && gaps <= 4) {
			snprintf(b, sizeof(b), "ai line: no road from %.0f m to %.0f m -- bridged",
				(float)k * t->step, (float)runEnd * t->step);
			log(b);
		}
		for (int m = k; m < runEnd; m++) {
			float* o = t->v + m * F_COUNT;
			const float* A = (lo >= 0) ? t->v + lo * F_COUNT : NULL;
			const float* B = (hi < t->n) ? t->v + hi * F_COUNT : NULL;
			if (!A) A = B;
			if (!B) B = A;
			int span = hi - lo;
			if (span <= 0) span += t->n;
			int into = m - lo;
			if (into <= 0) into += t->n;
			const float f = (A == B) ? 0.0f : (float)into / (float)span;
			for (int q = 0; q < F_COUNT; q++)
				o[q] = lerpf(A[q], B[q], f);
			if (chord) {
				const int wa = (lo >= 0) ? lo : hi, wb = (hi < t->n) ? hi : wa;
				for (int q = 0; q < 4; q++)
					w[4 * m + q] = lerpf(w[4 * wa + q], w[4 * wb + q], f);
				continue;
			}
			// The laterals above already carry the line across the race
			// route; put them back on the race route itself rather than on
			// a chord across it, and take the race's own heading with them.
			const float* ar = at + 4 * m;
			w[4 * m] = ar[0] + ar[2] * o[F_LINE];
			w[4 * m + 1] = ar[1] + ar[3] * o[F_LINE];
			w[4 * m + 2] = ar[0] + ar[2] * o[F_POINT];
			w[4 * m + 3] = ar[1] + ar[3] * o[F_POINT];
			o[F_DX] = -ar[3];
			o[F_DY] = ar[2];
		}
		k = runEnd;
	}
	free(ok);

	// A route change can step the lines sideways by a metre or so; even that
	// out so the steer point does not jump, then put both back across the
	// race route. The edges and the height stay as matched.
	for (int k = 0; k < t->n; k++) {
		float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (int d = -AILINE_SMOOTH; d <= AILINE_SMOOTH; d++) {
			const float* q = w + 4 * wrapIndex(t, k + d);
			for (int c = 0; c < 4; c++) sum[c] += q[c];
		}
		const float* a = at + 4 * k;
		const float inv = 1.0f / (float)(2 * AILINE_SMOOTH + 1);
		float* o = t->v + k * F_COUNT;
		o[F_LINE] = (sum[0] * inv - a[0]) * a[2] + (sum[1] * inv - a[1]) * a[3];
		o[F_POINT] = (sum[2] * inv - a[0]) * a[2] + (sum[3] * inv - a[1]) * a[3];
	}
	free(w);

	// Steps ramped, then the walls; lo/hi are scratch for both.
	int ramped = 0, wallMoved = 0, crossings = 0, squeezed = 0, cornered = 0;
	float* lo = (float*)malloc(sizeof(float) * (size_t)t->n);
	float* hi = (float*)malloc(sizeof(float) * (size_t)t->n);
	if (lo && hi) {
		ramped = rampSteps(t, lo, hi);
		if (walls && walls->near)
			wallMoved = wallPass(t, at, walls, lo, hi, &crossings, &squeezed);
		rampEdges(t, lo, hi);
		gridRoad(t, s, at, gridXY, nGrid, startZ, walls, lo, hi, log);
		gridHold(t, at, gridXY, nGrid, startZ, walls);
	}
	free(lo);
	free(hi);
	for (int k = 0; k < t->n; k++) {
		float* o = t->v + k * F_COUNT;
		const float* a = at + 4 * k;
		o[F_WX] = a[0] + a[2] * o[F_LINE];
		o[F_WY] = a[1] + a[3] * o[F_LINE];
	}
	int unfolded = 0;
	for (int pass = 0; pass < 3; pass++) {
		const int c = unfold(t, at);
		if (!c) break;
		unfolded += c;
	}
	cornered = cornerPass(t, at);
	for (int pass = 0; cornered && pass < 3; pass++) {
		const int c = unfold(t, at);
		if (!c) break;
		unfolded += c;
	}
	if (log && unfolded) {
		snprintf(b, sizeof(b), "ai line: %d fold(s) cut where the line ran back against the "
			"race route", unfolded);
		log(b);
	}
	free(at);
	t->arc = (float*)malloc(sizeof(float) * (size_t)(t->n + 1));
	if (t->arc) {
		t->arc[0] = 0.0f;
		for (int k = 1; k <= t->n; k++) {
			const float* p = t->v + (k - 1) * F_COUNT;
			const float* q = t->v + (k < t->n ? k : (t->loop ? 0 : t->n - 1)) * F_COUNT;
			t->arc[k] = t->arc[k - 1] + sqrtf((q[F_WX] - p[F_WX]) * (q[F_WX] - p[F_WX]) +
				(q[F_WY] - p[F_WY]) * (q[F_WY] - p[F_WY]));
		}
	}

	// Walls AILINE_HOP_JOIN apart or less are one -- 4042F's face at 3,316 m
	// is a 0.2 m step and then the rest of it 4 m on.
	{
		int n = 0;
		for (int i = 0; i < t->nWall; i++) {
			if (n > 0 && t->wall[i].dist - t->wall[n - 1].end <= AILINE_HOP_JOIN) {
				t->wall[n - 1].end = t->wall[i].end;
				t->wall[n - 1].zAfter = t->wall[i].zAfter;
			} else {
				t->wall[n++] = t->wall[i];
			}
		}
		t->nWall = n;
	}
	if (log) {
		snprintf(b, sizeof(b), "ai line: %d sample(s) ramped; walls %s: %d with no way round, "
			"line squeezed at %d sample(s), moved at %d sample(s); %d sample(s) on bends "
			"refitted", ramped, walls && walls->near ? "checked" : "not checked", crossings,
			squeezed, wallMoved, cornered);
		log(b);
		for (int i = 0; i < t->nWall; i++) {
			const AiLineCrossing* c = &t->wall[i];
			snprintf(b, sizeof(b), "ai line: through a wall at %.0f - %.0f m, road %.2f -> %.2f%s",
				c->dist, c->end, c->zBefore, c->zAfter,
				c->zAfter - c->zBefore > AILINE_HOP_UP ? " -- a face, hopped" : "");
			log(b);
		}
	}
	if (log) {
		float most = 0.0f;
		for (int k = 0; k < t->n; k++) {
			const float v = fabsf(t->v[k * F_COUNT + F_LINE]);
			if (v > most) most = v;
		}
		snprintf(b, sizeof(b), "ai line: %d of %d samples (%.1f m apart) on a "
			"TrackRoute, %d unposted, %d up a route, %d off the road it matched "
			"(%d more kept on the road either side), %d route change(s), %d gap(s) "
			"of %d sample(s) bridged; line at most %.1f m off the race route",
			matched, t->n, t->step, unposted, upSamples, offRoad, kept, switches, gaps,
			gapSamples, most);
		log(b);
	}
	return t;
}

void ailine_free(AiLine* t) {
	if (!t) return;
	free(t->v);
	free(t->arc);
	free(t);
}

float ailine_arc(const AiLine* t, float dist) {
	if (!t || !t->arc) return dist;
	if (t->loop) {
		const float lap = floorf(dist / t->len);
		const float x = (dist - lap * t->len) / t->step;
		int k = (int)x;
		if (k > t->n - 1) k = t->n - 1;
		return lap * t->arc[t->n] + lerpf(t->arc[k], t->arc[k + 1], x - (float)k);
	}
	if (dist <= 0.0f) return dist;
	if (dist >= t->len) return t->arc[t->n - 1] + (dist - t->len);
	const float x = dist / t->step;
	int k = (int)x;
	if (k > t->n - 2) k = t->n - 2;
	return lerpf(t->arc[k], t->arc[k + 1], x - (float)k);
}

float ailine_along(const AiLine* t, float dist, float metres) {
	if (!t || !t->arc) return dist + metres;
	float s = ailine_arc(t, dist) + metres;
	float base = 0.0f;
	int hi = t->n - 1;   // the last span a sprint has
	if (t->loop) {
		const float lap = floorf(s / t->arc[t->n]);
		s -= lap * t->arc[t->n];
		base = lap * t->len;
		hi = t->n;
	} else {
		if (s <= 0.0f) return s;
		if (s >= t->arc[t->n - 1]) return t->len + (s - t->arc[t->n - 1]);
	}
	int lo = 0;   // arc[lo] <= s < arc[hi]
	while (hi - lo > 1) {
		const int mid = (lo + hi) / 2;
		if (t->arc[mid] <= s) lo = mid;
		else hi = mid;
	}
	const float l = t->arc[hi] - t->arc[lo];
	const float f = l > 1e-6f ? (s - t->arc[lo]) / l : 0.0f;
	return base + ((float)lo + f) * t->step;
}

bool ailine_at(const AiLine* t, float dist, AiLineSample* out) {
	if (!t || !out) return false;
	int k0, k1;
	float f;
	lineSpan(t, dist, &k0, &k1, &f);
	const float* a = t->v + k0 * F_COUNT;
	const float* b = t->v + k1 * F_COUNT;
	out->line = lerpf(a[F_LINE], b[F_LINE], f);
	out->point = lerpf(a[F_POINT], b[F_POINT], f);
	out->left = lerpf(a[F_LEFT], b[F_LEFT], f);
	out->right = lerpf(a[F_RIGHT], b[F_RIGHT], f);
	out->z = lerpf(a[F_Z], b[F_Z], f);
	float dx = lerpf(a[F_DX], b[F_DX], f), dy = lerpf(a[F_DY], b[F_DY], f);
	const float l = sqrtf(dx * dx + dy * dy);
	if (l > 1e-4f) {
		dx /= l;
		dy /= l;
	} else {
		dx = a[F_DX];
		dy = a[F_DY];
	}
	out->dir[0] = dx;
	out->dir[1] = dy;
	return true;
}

// The line's own heading at sample k, from its neighbours either side (one
// side at a sprint's ends). wrapIndex clamps on a sprint.
static void lineHeading(const AiLine* t, int k, float out[2]) {
	const int p = wrapIndex(t, k - 1), q = wrapIndex(t, k + 1);
	const float* a = t->v + p * F_COUNT;
	const float* b = t->v + q * F_COUNT;
	float dx = b[F_WX] - a[F_WX], dy = b[F_WY] - a[F_WY];
	float l = sqrtf(dx * dx + dy * dy);
	if (l < 1e-4f) {
		dx = t->v[k * F_COUNT + F_DX];
		dy = t->v[k * F_COUNT + F_DY];
		l = sqrtf(dx * dx + dy * dy);
		if (l < 1e-4f) { dx = 1.0f; dy = 0.0f; l = 1.0f; }
	}
	out[0] = dx / l;
	out[1] = dy / l;
}

// The two samples either side of `dist` and how far it is between them,
// wrapped or clamped the way ailine_at does it.
static void lineSpan(const AiLine* t, float dist, int* k0, int* k1, float* f) {
	if (t->loop) {
		dist = fmodf(dist, t->len);
		if (dist < 0.0f) dist += t->len;
		const float x = dist / t->step;
		*k0 = wrapIndex(t, (int)x);
		*f = x - floorf(x);
		*k1 = wrapIndex(t, *k0 + 1);
		return;
	}
	if (dist < 0.0f) dist = 0.0f;
	if (dist > t->len) dist = t->len;
	const float x = dist / t->step;
	*k0 = (int)x;
	if (*k0 >= t->n - 1) {
		*k0 = *k1 = t->n - 1;
		*f = 0.0f;
	} else {
		*k1 = *k0 + 1;
		*f = x - (float)*k0;
	}
}

static void lineDir(const AiLine* t, int k0, int k1, float f, float out[2]) {
	float h0[2], h1[2];
	lineHeading(t, k0, h0);
	lineHeading(t, k1, h1);
	float dx = lerpf(h0[0], h1[0], f), dy = lerpf(h0[1], h1[1], f);
	const float l = sqrtf(dx * dx + dy * dy);
	if (l > 1e-4f) { dx /= l; dy /= l; }
	else { dx = h0[0]; dy = h0[1]; }
	out[0] = dx;
	out[1] = dy;
}

bool ailine_pose(const AiLine* t, float dist, float lateral, float outPos[2], float outDir[2]) {
	if (!t) return false;
	int k0, k1;
	float f, h[2];
	lineSpan(t, dist, &k0, &k1, &f);
	lineDir(t, k0, k1, f, h);
	const float* a = t->v + k0 * F_COUNT;
	const float* b = t->v + k1 * F_COUNT;
	if (outPos) {
		outPos[0] = lerpf(a[F_WX], b[F_WX], f) + h[1] * lateral;
		outPos[1] = lerpf(a[F_WY], b[F_WY], f) - h[0] * lateral;
	}
	if (outDir) {
		outDir[0] = h[0];
		outDir[1] = h[1];
	}
	return true;
}

float ailine_track(const AiLine* t, float dist, float back, float ahead,
		const float pos[2], float* outLateral, float outDir[2]) {
	if (!t) return dist;
	int first = (int)floorf((dist - back) / t->step);
	int last = (int)ceilf((dist + ahead) / t->step);
	if (!t->loop) {
		if (first < 0) first = 0;
		if (last > t->n - 1) last = t->n - 1;
	} else if (last - first >= t->n) {
		last = first + t->n - 1;
	}
	int best = -1;
	float bestD2 = 1e30f, bestU = 0.0f;
	for (int s = first; ; s++) {
		// Past the window only while each segment is still nearer than the
		// last: a car faster than `ahead` allows for keeps being found.
		if (s >= last && best != s - 1) break;
		if (s >= first + t->n || (!t->loop && s >= t->n - 1)) break;
		const float* a = t->v + wrapIndex(t, s) * F_COUNT;
		const float* b = t->v + wrapIndex(t, s + 1) * F_COUNT;
		const float ex = b[F_WX] - a[F_WX], ey = b[F_WY] - a[F_WY];
		const float l2 = ex * ex + ey * ey;
		float u = l2 > 1e-8f ? ((pos[0] - a[F_WX]) * ex + (pos[1] - a[F_WY]) * ey) / l2 : 0.0f;
		u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
		const float dx = a[F_WX] + ex * u - pos[0], dy = a[F_WY] + ey * u - pos[1];
		const float d2 = dx * dx + dy * dy;
		if (d2 < bestD2) {
			bestD2 = d2;
			best = s;
			bestU = u;
		}
	}
	if (best < 0) return dist;
	const int k0 = wrapIndex(t, best), k1 = t->loop ? wrapIndex(t, best + 1) : best + 1;
	float h[2];
	lineDir(t, k0, k1, bestU, h);
	const float* a = t->v + k0 * F_COUNT;
	const float* b = t->v + k1 * F_COUNT;
	const float qx = lerpf(a[F_WX], b[F_WX], bestU), qy = lerpf(a[F_WY], b[F_WY], bestU);
	if (outLateral) *outLateral = (pos[0] - qx) * h[1] - (pos[1] - qy) * h[0];
	if (outDir) {
		outDir[0] = h[0];
		outDir[1] = h[1];
	}
	float d = ((float)best + bestU) * t->step;
	if (t->loop) {
		d = fmodf(d, t->len);
		if (d < 0.0f) d += t->len;
	}
	return d;
}

bool ailine_face_ahead(const AiLine* t, float from, float within, AiLineCrossing* out) {
	if (!t) return false;
	for (int i = 0; i < t->nWall; i++) {
		const AiLineCrossing* c = &t->wall[i];
		if (c->zAfter - c->zBefore <= AILINE_HOP_UP) continue;
		// Ahead until its last wall is passed.
		float d = c->end - from;
		if (t->loop && d <= 0.0f) d += t->len;
		if (d > 0.0f && d - (c->end - c->dist) <= within) {
			if (out) *out = *c;
			return true;
		}
	}
	return false;
}

// ------------------------------------------------------------ traffic lanes
//
// See trackroute.h.

// A road near a sample: the segment of a route nearest the line within
// LANES_REACH that runs within LANES_ALIGN of the line's heading (either way,
// cos) and is posted for racers the way it is driven -- the roads a racer
// drives; a Routes file's unposted ones (4201F's first four) are traffic's.
// Where a race splits, the two roads run 20 m and more either side of the
// line and 9 m above or below it (4201F from 1263 m), so neither the reach nor
// the height is held to the line's own road.
#define LANES_REACH  30.0f
#define LANES_ALIGN  0.7f
#define LANES_N      (TRACK_LANE_LAST - TRACK_LANE_FIRST + 1)
// A lane moves onto another road that has it where that road runs on at least
// LANES_LONGER m further, with the lane there within LANES_SWITCH m across and
// AILINE_GRID_Z of height, and goes from the one to the other across
// LANES_BLEND samples. Where its road ends, it carries on along whichever has
// it nearest, inside the same limits.
#define LANES_LONGER 20.0f
#define LANES_SWITCH 3.5f
#define LANES_BLEND  10

struct TrackLanes {
	const AiLine* line;
	TrackLaneRoad* cell;   // line->n * LANES_N; lat NAN where no road has the lane
};

typedef struct {
	bool on, fwd;
	int seg;
	float u, d2, ahead;   // how far the route runs on, the way it is driven
	float cx, cy, z;   // the road's middle and height there
	float wx, wy;      // the way it is driven
	float halfL, halfR;
} LaneRoad;

static bool routeHasLanes(const TRRoute* rt, bool fwd) {
	for (int l = 0; l < LANES_N; l++)
		if (rt->line[TRACK_LANE_FIRST + l + (fwd ? 0 : 4)]) return true;
	return false;
}

// Route `r` near the line's sample `k` at (px, py) heading `h`; `run` is each
// point's distance along its route.
static LaneRoad laneRoad(const TrackRouteSet* s, const float* run, int r, float px, float py,
		const float h[2]) {
	LaneRoad o;
	memset(&o, 0, sizeof(o));
	const TRRoute* rt = &s->routes[r];
	if (px < rt->bbMin[0] - LANES_REACH || px > rt->bbMax[0] + LANES_REACH ||
			py < rt->bbMin[1] - LANES_REACH || py > rt->bbMax[1] + LANES_REACH)
		return o;
	float bestD2 = LANES_REACH * LANES_REACH, bestU = 0.0f;
	for (int i = rt->first; i + 1 < rt->first + rt->count; i++) {
		const TRPoint* a = &s->pts[i];
		const TRPoint* e = &s->pts[i + 1];
		const float ex = e->x - a->x, ey = e->y - a->y;
		const float l2 = ex * ex + ey * ey;
		if (l2 < 1e-6f) continue;
		const float u = unit01(((px - a->x) * ex + (py - a->y) * ey) / l2);
		const float dx = a->x + ex * u - px, dy = a->y + ey * u - py;
		const float d2 = dx * dx + dy * dy;
		if (d2 >= bestD2) continue;
		const float c = (ex * h[0] + ey * h[1]) / sqrtf(l2);
		if (fabsf(c) < LANES_ALIGN) continue;
		const bool fwd = c > 0.0f;
		if (!(fwd ? (a->postF | e->postF) : (a->postB | e->postB))) continue;
		if (!routeHasLanes(rt, fwd)) continue;
		o.on = true;
		o.fwd = fwd;
		o.seg = i;
		bestD2 = d2;
		bestU = u;
	}
	if (!o.on) return o;
	const TRPoint* a = &s->pts[o.seg];
	const TRPoint* e = &s->pts[o.seg + 1];
	const float ex = e->x - a->x, ey = e->y - a->y;
	const float el = sqrtf(ex * ex + ey * ey);
	const float at = lerpf(run[o.seg], run[o.seg + 1], bestU);
	o.u = bestU;
	o.d2 = bestD2;
	o.ahead = o.fwd ? run[rt->first + rt->count - 1] - at : at;
	o.cx = a->x + ex * bestU;
	o.cy = a->y + ey * bestU;
	o.z = lerpf(a->z, e->z, bestU);
	o.wx = (o.fwd ? ex : -ex) / el;
	o.wy = (o.fwd ? ey : -ey) / el;
	// The route's own left and right are the driver's either way round.
	o.halfL = lerpf(o.fwd ? a->halfL : a->halfR, o.fwd ? e->halfL : e->halfR, bestU);
	o.halfR = lerpf(o.fwd ? a->halfR : a->halfL, o.fwd ? e->halfR : e->halfL, bestU);
	return o;
}

// Lane `l` (0 for lane 2) of a road near the line at (px, py) heading `h`, as
// metres right of the line; false where the road has no such lane.
static bool laneOnRoad(const TrackRouteSet* s, const LaneRoad* rd, int l, float px, float py,
		const float h[2], TrackLaneRoad* out) {
	if (!rd->on) return false;
	const TRPoint* a = &s->pts[rd->seg];
	const TRPoint* e = &s->pts[rd->seg + 1];
	const TRRoute* rt = &s->routes[a->route];
	const int idx = l + (rd->fwd ? 0 : 4);   // lane[]: line - 2
	if (!rt->line[TRACK_LANE_FIRST + idx]) return false;
	const float off = lerpf(a->lane[idx], e->lane[idx], rd->u);
	const float lx = rd->cx + off * rd->wy, ly = rd->cy - off * rd->wx;
	const float mid = (rd->cx - px) * h[1] - (rd->cy - py) * h[0];
	out->lat = (lx - px) * h[1] - (ly - py) * h[0];
	out->lo = mid - rd->halfL;
	out->hi = mid + rd->halfR;
	out->z = rd->z;
	return true;
}

TrackLanes* trackroute_lanes(const TrackRouteSet* s, const AiLine* line,
		void (*log)(const char* msg)) {
	if (!s || !line || line->n < 2 || s->nRoutes < 1) return NULL;
	TrackLanes* t = (TrackLanes*)calloc(1, sizeof(TrackLanes));
	LaneRoad* rd = (LaneRoad*)malloc(sizeof(LaneRoad) * (size_t)s->nRoutes);
	float* run = (float*)malloc(sizeof(float) * (size_t)s->nPts);
	// The road each sample's lane is read off (-1 none), for the log.
	s8* from = (s8*)malloc((size_t)line->n * LANES_N);
	if (t) {
		t->line = line;
		t->cell = (TrackLaneRoad*)malloc(sizeof(TrackLaneRoad) * LANES_N * (size_t)line->n);
	}
	if (!t || !t->cell || !rd || !run || !from) {
		free(rd);
		free(run);
		free(from);
		trackroute_lanes_free(t);
		return NULL;
	}
	for (int r = 0; r < s->nRoutes; r++) {
		const TRRoute* rt = &s->routes[r];
		for (int i = rt->first; i < rt->first + rt->count; i++) {
			const TRPoint* p = &s->pts[i];
			const TRPoint* q = &s->pts[i > rt->first ? i - 1 : i];
			run[i] = (i > rt->first ? run[i - 1] : 0.0f) +
				sqrtf((p->x - q->x) * (p->x - q->x) + (p->y - q->y) * (p->y - q->y));
		}
	}
	int cur[LANES_N], was[LANES_N], blendAt[LANES_N];
	bool had[LANES_N] = { false };
	TrackLaneRoad prev[LANES_N];
	for (int l = 0; l < LANES_N; l++) cur[l] = was[l] = blendAt[l] = -1;
	int have = 0, per[LANES_N] = { 0 };
	for (int k = 0; k < line->n; k++) {
		const float* v = line->v + k * F_COUNT;
		const float px = v[F_WX], py = v[F_WY];
		float h[2];
		lineHeading(line, k, h);
		for (int r = 0; r < s->nRoutes; r++) rd[r] = laneRoad(s, run, r, px, py, h);
		bool any = false;
		for (int l = 0; l < LANES_N; l++) {
			TrackLaneRoad* o = &t->cell[k * LANES_N + l];
			TrackLaneRoad c, q;
			int pick = -1;
			if (cur[l] >= 0 && laneOnRoad(s, &rd[cur[l]], l, px, py, h, &c)) {
				// On the road it was on, unless one that has it runs on further.
				pick = cur[l];
				float far = rd[pick].ahead + LANES_LONGER;
				int to = -1;
				TrackLaneRoad there = c;
				for (int r = 0; r < s->nRoutes; r++) {
					if (r == pick || !laneOnRoad(s, &rd[r], l, px, py, h, &q)) continue;
					if (rd[r].ahead <= far || fabsf(q.z - c.z) > AILINE_GRID_Z ||
							fabsf(q.lat - c.lat) > LANES_SWITCH)
						continue;
					to = r;
					far = rd[r].ahead;
					there = q;
				}
				if (to >= 0) {
					was[l] = pick;
					blendAt[l] = k;
					pick = to;
					c = there;
				}
			} else {
				// Its road gone, or none yet: where it was, the road that has it
				// nearest there; otherwise the nearest road with it at the
				// line's height.
				blendAt[l] = -1;
				float best = had[l] ? LANES_SWITCH : 1e30f;
				for (int r = 0; r < s->nRoutes; r++) {
					if (!laneOnRoad(s, &rd[r], l, px, py, h, &q)) continue;
					float e;
					if (had[l]) {
						if (fabsf(q.z - prev[l].z) > AILINE_GRID_Z) continue;
						e = fabsf(q.lat - prev[l].lat);
					} else {
						if (fabsf(q.z - v[F_Z]) > AILINE_GRID_Z) continue;
						e = rd[r].d2;
					}
					if (e > best) continue;
					best = e;
					pick = r;
					c = q;
				}
			}
			cur[l] = pick;
			from[k * LANES_N + l] = (s8)pick;
			if (pick < 0) {
				o->lat = o->lo = o->hi = o->z = NAN;
				had[l] = false;
				continue;
			}
			// Leaving one road for another: from the old lane to the new one,
			// the road either's.
			if (blendAt[l] >= 0 && k - blendAt[l] < LANES_BLEND &&
					laneOnRoad(s, &rd[was[l]], l, px, py, h, &q)) {
				const float w = (float)(k - blendAt[l] + 1) / (float)(LANES_BLEND + 1);
				c.lat = lerpf(q.lat, c.lat, w);
				c.z = lerpf(q.z, c.z, w);
				c.lo = fminf(q.lo, c.lo);
				c.hi = fmaxf(q.hi, c.hi);
			} else {
				blendAt[l] = -1;
			}
			*o = c;
			prev[l] = c;
			had[l] = true;
			per[l]++;
			any = true;
		}
		if (any) have++;
	}
	if (log) {
		char msg[200];
		snprintf(msg, sizeof(msg),
			"trackroute: lanes on %d of %d samples (lane 2..5: %d %d %d %d)",
			have, line->n, per[0], per[1], per[2], per[3]);
		log(msg);
		// The lanes a quarter, half and three quarters of the way: they must
		// run left to right, 2 to 5, as GetNearestTrafficLane counts them.
		for (int qt = 1; qt <= 3; qt++) {
			const int k = line->n * qt / 4;
			const TrackLaneRoad* o = t->cell + k * LANES_N;
			snprintf(msg, sizeof(msg), "trackroute: lanes at %.0f m: %.1f %.1f %.1f %.1f",
				(double)((float)k * line->step), (double)o[0].lat, (double)o[1].lat,
				(double)o[2].lat, (double)o[3].lat);
			log(msg);
		}
		// Each lane's roads, and where: a lane's end, and its move from road to
		// road, are where its changes stop.
		for (int l = 0; l < LANES_N; l++) {
			int n = snprintf(msg, sizeof(msg), "trackroute: lane %d on", TRACK_LANE_FIRST + l);
			int k = 0;
			while (k < line->n && n < (int)sizeof(msg) - 24) {
				const int key = from[k * LANES_N + l];
				int e = k;
				while (e + 1 < line->n && from[(e + 1) * LANES_N + l] == key) e++;
				if (key >= 0)
					n += snprintf(msg + n, sizeof(msg) - (size_t)n, " r%d %.0f-%.0f", key,
						(double)((float)k * line->step), (double)((float)e * line->step));
				k = e + 1;
			}
			log(msg);
		}
	}
	free(rd);
	free(run);
	free(from);
	if (have == 0) {
		trackroute_lanes_free(t);
		return NULL;
	}
	return t;
}

void trackroute_lanes_free(TrackLanes* t) {
	if (!t) return;
	free(t->cell);
	free(t);
}

bool trackroute_lane_road(const TrackLanes* t, int lane, float dist, TrackLaneRoad* out) {
	if (!t || lane < TRACK_LANE_FIRST || lane > TRACK_LANE_LAST) return false;
	const int l = lane - TRACK_LANE_FIRST;
	int k0, k1;
	float f;
	lineSpan(t->line, dist, &k0, &k1, &f);
	const TrackLaneRoad* a = &t->cell[k0 * LANES_N + l];
	const TrackLaneRoad* b = &t->cell[k1 * LANES_N + l];
	// Where the lane starts or ends between two samples, the nearer one says.
	TrackLaneRoad r;
	if (!isnan(a->lat) && !isnan(b->lat)) {
		r.lat = lerpf(a->lat, b->lat, f);
		r.lo = lerpf(a->lo, b->lo, f);
		r.hi = lerpf(a->hi, b->hi, f);
		r.z = lerpf(a->z, b->z, f);
	} else if (!isnan(a->lat) && f < 0.5f) {
		r = *a;
	} else if (!isnan(b->lat) && f >= 0.5f) {
		r = *b;
	} else {
		return false;
	}
	if (out) *out = r;
	return true;
}

bool trackroute_lane_at(const TrackLanes* t, int lane, float dist, float* outLat) {
	TrackLaneRoad r;
	if (!trackroute_lane_road(t, lane, dist, &r)) return false;
	if (outLat) *outLat = r.lat;
	return true;
}

float trackroute_lane_run(const TrackLanes* t, int lane, float dist, float cap) {
	if (!trackroute_lane_at(t, lane, dist, NULL)) return 0.0f;
	const int l = lane - TRACK_LANE_FIRST;
	const AiLine* line = t->line;
	int k = (int)ceilf(dist / line->step);
	float run = (float)k * line->step - dist;
	for (; run < cap; run += line->step, k++) {
		if (!line->loop && k >= line->n) return cap;
		if (isnan(t->cell[wrapIndex(line, k) * LANES_N + l].lat)) return run > 0.0f ? run : 0.0f;
	}
	return cap;
}

// --------------------------------------------- the race's forks (TASKS.md #409)
//
// See trackroute.h.

#define FORK_REACH     70.0f   // m either side of the line a branch is looked for on
#define FORK_NEAR      40.0f   // m from the line the sign's old point may be
#define FORK_WINDOW    120.0f  // m of race either side of the sign's raceDist searched
#define FORK_RAMP      6       // samples the line blends into and out of a branch over
#define FORK_OUT       1.0f    // m outside the line's road a branch has to go to be one
#define FORK_IN        0.5f    // m inside the line's road edges a branch is back on it...
#define FORK_RAMP_IN_MAX 20    // samples, 40 m: a blend in is under the 60 m racers decide at
#define FORK_REJOIN    8       // ...for this many metres running
#define FORK_BACKTRACK 3.0f    // samples a branch may step back where two routes meet
#define FORK_MAX_RUN   400     // samples a branch may span
#define FORK_MAX       64

typedef struct {
	int k0, n;          // first line sample (wrapped on a loop), samples spanned
	int sign;           // line samples before the sign's own, from k0
	float p;            // the chance a racer takes it
	int branch;         // its route number
	float endLat;       // where its branch ends, m right of the line (the log)
	const char* end;    // and why there
	TrackLaneRoad* cell;
	float* xy;          // per cell, the path in the world (for curv, then freed)
	float* curv;        // per cell, the curvature of the path its racer drives
} TRFork;

struct TrackForks {
	const AiLine* line;
	int n;
	TRFork f[FORK_MAX];
};

static int routeByNum(const TrackRouteSet* s, int num) {
	for (int r = 0; r < s->nRoutes; r++)
		if (s->routes[r].num == num) return r;
	return -1;
}

// The line's sample nearest (x, y) within FORK_WINDOW m of race either side of
// `rd`; its distance in `outD`.
static int forkNearest(const AiLine* t, float x, float y, float rd, float* outD) {
	const int span = (int)(FORK_WINDOW / t->step);
	const int c = (int)(rd / t->step);
	int best = -1;
	float bestD2 = 1e30f;
	for (int j = c - span; j <= c + span; j++) {
		if (!t->loop && (j < 0 || j >= t->n)) continue;
		const float* v = t->v + wrapIndex(t, j) * F_COUNT;
		const float dx = v[F_WX] - x, dy = v[F_WY] - y;
		const float d2 = dx * dx + dy * dy;
		if (d2 < bestD2) {
			bestD2 = d2;
			best = wrapIndex(t, j);
		}
	}
	*outD = sqrtf(bestD2);
	return best;
}

// The branch as a driver takes it: route `r`'s points from `from` on, down
// the route (`up` false) or up it. Each vertex: its racing line, its road's
// middle, the driver's half-widths, height and the race's distance.
typedef struct {
	float lx, ly, cx, cy, hl, hr, z, rd;
} ForkVtx;

// The type-2 sign at route `r`'s point `j` a car driving it `up` (or down)
// reads -- IsAlways, the sign every car takes -- or NULL.
static const TRSign* forkExit(const TrackRouteSet* s, int r, int j, bool up) {
	const int num = s->routes[r].num;
	for (int i = 0; i < s->nSigns; i++) {
		const TRSign* g = &s->signs[i];
		if (g->oldRn != num || g->oldPt != j || (g->bits & 15u) != 2u || (g->bits >> 20) & 1u)
			continue;
		if ((bool)((g->bits >> 18) & 1u) == !up) continue;   // the other way's
		return g;
	}
	return NULL;
}

// Where a branch leads it is a chain of routes: at a type-2 sign the car
// drives on onto the sign's route (r918 p9 -> r784 on 4014F) until it is back
// on the line's road. At most FORK_HOPS of them.
#define FORK_HOPS 6
#define FORK_VTX  1024

static int forkWalk(const TrackRouteSet* s, int r, int from, bool up, ForkVtx* out, int max) {
	int nv = 0, hops = 0;
	for (;;) {
		const TRRoute* rt = &s->routes[r];
		if (rt->count < 2) return nv;
		int next = -1, nextPt = 0;
		bool nextUp = up;
		for (int j = from; j >= 0 && j < rt->count && nv < max; j += up ? -1 : 1) {
			const int i = rt->first + j;
			const TRPoint* p = &s->pts[i];
			// The route's own direction there, for its right; the last point
			// takes the segment before it.
			const TRPoint* a = &s->pts[j + 1 < rt->count ? i : i - 1];
			const TRPoint* b = &s->pts[j + 1 < rt->count ? i + 1 : i];
			float tx = b->x - a->x, ty = b->y - a->y;
			const float l = sqrtf(tx * tx + ty * ty);
			if (l < 1e-4f) continue;
			tx /= l;
			ty /= l;
			const float Rx = ty, Ry = -tx;   // right of the route
			// Line 1 is measured to the right of the route's backward direction
			// (matchFill).
			const float off = up ? -p->off[1] : p->off[0];
			ForkVtx* v = &out[nv++];
			v->cx = p->x;
			v->cy = p->y;
			v->lx = p->x + Rx * off;
			v->ly = p->y + Ry * off;
			// The route's own left and right are the driver's either way round.
			v->hl = up ? p->halfR : p->halfL;
			v->hr = up ? p->halfL : p->halfR;
			v->z = p->z;
			v->rd = p->raceDist;
			if (j == from || hops >= FORK_HOPS) continue;
			const TRSign* g = forkExit(s, r, j, up);
			const int to = g ? routeByNum(s, g->newRn) : -1;
			if (to < 0 || g->newPt < 0 || g->newPt >= s->routes[to].count) continue;
			next = to;
			nextPt = g->newPt;
			// Bit 23: the new route the other way round.
			nextUp = ((g->bits >> 23) & 1u) ? !up : up;
			break;
		}
		if (next < 0 || nv >= max) return nv;
		r = next;
		from = nextPt;
		up = nextUp;
		hops++;
	}
}

// (x, y) on the line, searched over samples k0 .. k1 (unwrapped): how far
// along, in samples (unwrapped), and how far right of it, in `d` and `lat`;
// the foot and the right normal there in `foot` and `n`. The distance off.
static float forkProject(const AiLine* t, float x, float y, int k0, int k1, float* d, float* lat,
		float foot[2], float n[2]) {
	float best = 1e30f;
	for (int k = k0; k < k1; k++) {
		if (!t->loop && (k < 0 || k + 1 >= t->n)) continue;
		const float* a = t->v + wrapIndex(t, k) * F_COUNT;
		const float* e = t->v + wrapIndex(t, k + 1) * F_COUNT;
		const float ex = e[F_WX] - a[F_WX], ey = e[F_WY] - a[F_WY];
		const float l2 = ex * ex + ey * ey;
		if (l2 < 1e-6f) continue;
		const float u = unit01(((x - a[F_WX]) * ex + (y - a[F_WY]) * ey) / l2);
		const float fx = a[F_WX] + ex * u, fy = a[F_WY] + ey * u;
		const float dd = (x - fx) * (x - fx) + (y - fy) * (y - fy);
		if (dd >= best) continue;
		best = dd;
		const float l = sqrtf(l2);
		n[0] = ey / l;
		n[1] = -ex / l;
		foot[0] = fx;
		foot[1] = fy;
		*d = (float)k + u;
		*lat = (x - fx) * n[0] + (y - fy) * n[1];
	}
	return sqrtf(best);
}

// One metre of the branch as the line sees it.
typedef struct {
	float d, lat, mid, hl, hr, z;
	float x, y;   // where it is in the world
} ForkPt;
#define FORK_PTS 1024

static void forkFree(TRFork* k) {
	free(k->cell);
	k->cell = NULL;
	free(k->curv);
	k->curv = NULL;
	free(k->xy);
	k->xy = NULL;
}

// The fork's path, its racing line through the blends, as the world sees it
// at cell `c` (the line's own line past either end).
static void forkPos(const AiLine* line, const TRFork* k, int c, float out[2]) {
	if (c >= 0 && c < k->n && k->xy) {
		out[0] = k->xy[2 * c];
		out[1] = k->xy[2 * c + 1];
		return;
	}
	const float lat = 0.0f;
	float d = (float)(k->k0 + c) * line->step;
	if (line->loop) {
		d = fmodf(d, line->len);
		if (d < 0.0f) d += line->len;
	}
	if (!ailine_pose(line, d, lat, out, NULL)) out[0] = out[1] = 0.0f;
}

// Each cell's curvature, as aispeed_build measures the line's: the circle
// through the path FORK_CURV_SPAN metres either side (the speed profile's
// AISPEED_CURV_SPAN). Retail's racer on a branch reads the branch route's own
// limits; the line's profile knows nothing of a branch's bends.
#define FORK_CURV_SPAN 15.0f
static void forkCurv(const AiLine* line, TRFork* k) {
	k->curv = (float*)malloc(sizeof(float) * (size_t)k->n);
	if (!k->curv) return;
	const int reach = (int)(4.0f * FORK_CURV_SPAN / line->step) + 2;
	for (int c = 0; c < k->n; c++) {
		float a[2], b[2], e[2], p[2], q[2];
		forkPos(line, k, c, b);
		// FORK_CURV_SPAN metres of the path back and on.
		for (int dir = -1; dir <= 1; dir += 2) {
			float* o = dir < 0 ? a : e;
			p[0] = b[0];
			p[1] = b[1];
			float run = 0.0f;
			for (int j = 1; j <= reach && run < FORK_CURV_SPAN; j++) {
				forkPos(line, k, c + dir * j, q);
				run += sqrtf((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]));
				p[0] = q[0];
				p[1] = q[1];
			}
			o[0] = p[0];
			o[1] = p[1];
		}
		const float abx = b[0] - a[0], aby = b[1] - a[1];
		const float bcx = e[0] - b[0], bcy = e[1] - b[1];
		const float cax = a[0] - e[0], cay = a[1] - e[1];
		const float den = sqrtf(abx * abx + aby * aby) * sqrtf(bcx * bcx + bcy * bcy)
			* sqrtf(cax * cax + cay * cay);
		const float cross = abx * bcy - aby * bcx;
		k->curv[c] = den > 1e-6f && fabsf(cross) > 1e-6f ? 2.0f * fabsf(cross) / den : 0.0f;
	}
	free(k->xy);
	k->xy = NULL;
}

// One sign as a fork off the line; false where it is not one a racer takes.
// `why` says why not, for the log.
static bool forkBuild(const TrackRouteSet* s, const AiLine* t, const TRSign* g, ForkVtx* vb,
		TRFork* out, const char** why) {
	const u32 bits = g->bits;
	*why = "not a racer's choice";
	// GetSignType(ePostednessType 0): bits 0-3. Type 4, a racer's own choice
	// (DecideSignpostAsRacer); bit 20 (the sign's bool 0x14) a sign it never
	// takes.
	if ((bits & 15u) != 4u || (bits >> 20) & 1u) return false;
	*why = "unresolved";
	const int ro = routeByNum(s, g->oldRn), rn = routeByNum(s, g->newRn);
	if (ro < 0 || rn < 0 || g->oldPt < 0 || g->oldPt >= s->routes[ro].count ||
			g->newPt < 0 || g->newPt >= s->routes[rn].count)
		return false;
	*why = "a closed route";
	if ((s->routes[rn].bits >> 10) & 1u) return false;
	const TRPoint* po = &s->pts[s->routes[ro].first + g->oldPt];
	float near;
	int ks = forkNearest(t, po->x, po->y, po->raceDist, &near);
	*why = "off the line";
	if (ks < 0 || near > FORK_NEAR) return false;
	// Which way the race drives the old route there: a sign stands on one
	// side, bit 18, and is read by whoever drives the other way.
	float h[2];
	lineHeading(t, ks, h);
	const bool fwd = po->fwd[0] * h[0] + po->fwd[1] * h[1] > 0.0f;
	*why = "the other way's";
	if ((bool)((bits >> 18) & 1u) == fwd) return false;
	// Bit 23: the new route is driven the other way round from the old.
	const bool newFwd = ((bits >> 23) & 1u) ? !fwd : fwd;
	const int nv = forkWalk(s, rn, g->newPt, !newFwd, vb, FORK_VTX);
	*why = "too short";
	if (nv < 3) return false;
	// The branch's racing line a metre at a time, each put on the line where
	// it is nearest it. It is a fork while it runs on along the line; where it
	// turns back or gets FORK_REACH off, the road is no longer one beside the
	// line's.
	static ForkPt bp[FORK_PTS];
	int nb = 0;
	float lastD = (float)(ks - FORK_RAMP);
	float farOut = 0.0f;
	int nearRun = 0;
	const char* end = "its end";
	for (int i = 0; i + 1 < nv && nb < FORK_PTS; i++) {
		const ForkVtx* a = &vb[i];
		const ForkVtx* e = &vb[i + 1];
		const float sl = sqrtf((e->lx - a->lx) * (e->lx - a->lx) + (e->ly - a->ly) * (e->ly - a->ly));
		const int steps = sl > 1.0f ? (int)ceilf(sl) : 1;
		bool stop = false;
		for (int j = (i == 0 ? 0 : 1); j <= steps && nb < FORK_PTS; j++) {
			const float u = (float)j / (float)steps;
			const float x = lerpf(a->lx, e->lx, u), y = lerpf(a->ly, e->ly, u);
			float d = 0.0f, lat = 0.0f, foot[2] = { 0.0f, 0.0f }, nn[2] = { 0.0f, 1.0f };
			const int k0 = (int)floorf(lastD) - 4;
			const float off = forkProject(t, x, y, k0, k0 + 64, &d, &lat, foot, nn);
			if (off > FORK_REACH) { end = "too far off"; stop = true; break; }
			// A route's first point can sit a metre short of the last one's.
			if (nb > 0 && d < bp[nb - 1].d - FORK_BACKTRACK) {
				end = "turning back";
				stop = true;
				break;
			}
			// Off the line's own road (its edges where the branch is), and
			// back on it: inside them for FORK_REJOIN metres, not just
			// crossing it.
			const float* lv = t->v + wrapIndex(t, (int)(d + 0.5f)) * F_COUNT;
			const float rlo = lv[F_LEFT] - lv[F_LINE], rhi = lv[F_RIGHT] - lv[F_LINE];
			const float out = lat < rlo ? rlo - lat : (lat > rhi ? lat - rhi : 0.0f);
			nearRun = lat > rlo + FORK_IN && lat < rhi - FORK_IN ? nearRun + 1 : 0;
			if (farOut >= FORK_OUT && nearRun >= FORK_REJOIN) {
				end = "the line's road";
				stop = true;
				break;
			}
			if (out > farOut) farOut = out;
			const float cx = lerpf(a->cx, e->cx, u), cy = lerpf(a->cy, e->cy, u);
			ForkPt* q = &bp[nb++];
			q->d = d > lastD ? d : lastD;
			q->lat = lat;
			q->mid = (cx - foot[0]) * nn[0] + (cy - foot[1]) * nn[1];
			q->hl = lerpf(a->hl, e->hl, u);
			q->hr = lerpf(a->hr, e->hr, u);
			q->z = lerpf(a->z, e->z, u);
			q->x = x;
			q->y = y;
			lastD = q->d;
		}
		if (stop) break;
	}
	*why = "not at its sign";
	if (nb > 0 && fabsf(bp[0].d - (float)ks) > 10.0f) return false;
	// Across the line's samples it spans.
	TrackLaneRoad tmp[FORK_MAX_RUN];
	static float tmpXY[FORK_MAX_RUN][2];
	int n = 0, first = 0;
	float far = 0.0f;
	if (nb >= 2) {
		first = (int)ceilf(bp[0].d);
		int i = 0;
		for (int k = first; (float)k <= bp[nb - 1].d && n < FORK_MAX_RUN - 2 * FORK_RAMP; k++) {
			while (i + 2 < nb && bp[i + 1].d < (float)k) i++;
			const ForkPt* a = &bp[i];
			const ForkPt* e = &bp[i + 1];
			const float w = e->d > a->d ? unit01(((float)k - a->d) / (e->d - a->d)) : 0.0f;
			tmpXY[n][0] = lerpf(a->x, e->x, w);
			tmpXY[n][1] = lerpf(a->y, e->y, w);
			TrackLaneRoad* c = &tmp[n++];
			c->lat = lerpf(a->lat, e->lat, w);
			const float mid = lerpf(a->mid, e->mid, w);
			c->lo = mid - lerpf(a->hl, e->hl, w);
			c->hi = mid + lerpf(a->hr, e->hr, w);
			c->z = lerpf(a->z, e->z, w);
			if (fabsf(c->lat) > far) far = fabsf(c->lat);
		}
	}
	ks = first;
	if (out) out->endLat = n > 0 ? tmp[n - 1].lat : 0.0f;
	if (out) out->end = end;
	*why = "the line's own road";
	if (n < 4 || farOut < FORK_OUT) return false;
	// Blended into and out of from the line, over FORK_RAMP samples or as
	// many as a car takes to cover the gap at AILINE_STEP_RATE, whichever is
	// more: in the blend the road is both roads.
	int ramp[2];
	for (int e = 0; e < 2; e++) {
		const float gap = fabsf(tmp[e ? n - 1 : 0].lat);
		const int r = (int)ceilf(gap / (AILINE_STEP_RATE * t->step));
		ramp[e] = r > FORK_RAMP ? r : FORK_RAMP;
	}
	// The blend in starts after a racer has decided (opponent.c's
	// FORK_DECIDE_M, 60 m before the sign).
	if (ramp[0] > FORK_RAMP_IN_MAX) ramp[0] = FORK_RAMP_IN_MAX;
	const int total = n + ramp[0] + ramp[1];
	out->cell = (TrackLaneRoad*)malloc(sizeof(TrackLaneRoad) * (size_t)total);
	out->xy = (float*)malloc(sizeof(float) * 2 * (size_t)total);
	*why = "out of memory";
	if (!out->cell || !out->xy) {
		forkFree(out);
		return false;
	}
	out->k0 = wrapIndex(t, ks - ramp[0]);
	if (!t->loop && ks - ramp[0] < 0) out->k0 = 0;
	out->n = total;
	out->sign = ramp[0];
	out->p = ((bits >> 27) & 1u) ? 1.0f / 3.0f : 0.5f;   // DAT_80316020 / DAT_8031601C
	out->branch = g->newRn;
	for (int i = 0; i < total; i++) {
		const int k = ks - ramp[0] + i;
		const float* v = t->v + wrapIndex(t, k) * F_COUNT;
		const float lineLo = v[F_LEFT] - v[F_LINE], lineHi = v[F_RIGHT] - v[F_LINE];
		TrackLaneRoad* c = &out->cell[i];
		if (i < ramp[0] || i >= ramp[0] + n) {
			const bool in = i < ramp[0];
			const TrackLaneRoad* e = &tmp[in ? 0 : n - 1];
			const float w = in ? (float)(i + 1) / (float)(ramp[0] + 1)
				: (float)(total - i) / (float)(ramp[1] + 1);
			c->lat = e->lat * w;
			c->lo = fminf(lineLo, e->lo);
			c->hi = fmaxf(lineHi, e->hi);
			c->z = lerpf(v[F_Z], e->z, w);
		} else {
			*c = tmp[i - ramp[0]];
			// Where the branch leaves and rejoins the line's road the two are
			// one road.
			if (i - ramp[0] < 2 || i - ramp[0] >= n - 2) {
				c->lo = fminf(c->lo, lineLo);
				c->hi = fmaxf(c->hi, lineHi);
			}
		}
		// The blends where the line puts them; the branch where it is.
		float* xy = &out->xy[2 * i];
		if (i >= ramp[0] && i < ramp[0] + n) {
			xy[0] = tmpXY[i - ramp[0]][0];
			xy[1] = tmpXY[i - ramp[0]][1];
		} else {
			float d = (float)k * t->step;
			if (t->loop) {
				d = fmodf(d, t->len);
				if (d < 0.0f) d += t->len;
			}
			if (!ailine_pose(t, d, c->lat, xy, NULL)) xy[0] = xy[1] = 0.0f;
		}
	}
	return true;
}

TrackForks* trackroute_forks(const TrackRouteSet* s, const AiLine* line,
		void (*log)(const char* msg)) {
	if (!s || !line || line->n < 2 || s->nSigns < 1) return NULL;
	TrackForks* t = (TrackForks*)calloc(1, sizeof(TrackForks));
	ForkVtx* vb = (ForkVtx*)malloc(sizeof(ForkVtx) * FORK_VTX);
	if (!t || !vb) {
		free(t);
		free(vb);
		return NULL;
	}
	t->line = line;
	char msg[200];
	int fours = 0;
	for (int i = 0; i < s->nSigns && t->n < FORK_MAX; i++) {
		const TRSign* g = &s->signs[i];
		if ((g->bits & 15u) == 4u) fours++;
		TRFork k;
		memset(&k, 0, sizeof(k));
		const char* why = "";
		if (!forkBuild(s, line, g, vb, &k, &why)) {
			if ((g->bits & 15u) == 4u && log) {
				snprintf(msg, sizeof(msg), "trackroute: sign %d r%d p%d -> r%d p%d: %s", i,
					g->oldRn, g->oldPt, g->newRn, g->newPt, why);
				log(msg);
			}
			continue;
		}
		// Two signs onto the same branch at the same place are one fork.
		bool dup = false;
		for (int j = 0; j < t->n && !dup; j++)
			dup = t->f[j].branch == k.branch && abs(t->f[j].k0 - k.k0) <= FORK_RAMP;
		if (dup) {
			forkFree(&k);
			continue;
		}
		forkCurv(line, &k);
		t->f[t->n++] = k;
		if (log) {
			float far = 0.0f;
			for (int c = 0; c < k.n; c++)
				if (fabsf(k.cell[c].lat) > far) far = fabsf(k.cell[c].lat);
			snprintf(msg, sizeof(msg),
				"trackroute: fork %d (sign %d) onto r%d at %.0f m, %.0f m long with its blends, up to %.1f m "
				"off the line, %+.1f m at %s, taken %.2f", t->n - 1, i, k.branch,
				(double)((float)(k.k0 + k.sign) * line->step),
				(double)((float)(k.n - 1) * line->step), (double)far,
				(double)k.endLat, k.end, (double)k.p);
			log(msg);
			float kmax = 0.0f;
			int kat = 0;
			for (int c = 0; k.curv && c < k.n; c++)
				if (k.curv[c] > kmax) { kmax = k.curv[c]; kat = c; }
			snprintf(msg, sizeof(msg), "trackroute:   fork %d tightest bend r %.0f m at %.0f m",
				t->n - 1, kmax > 0.0f ? (double)(1.0f / kmax) : 9999.0,
				(double)((float)(k.k0 + kat) * line->step));
			log(msg);
			// Its road every 10 m: the branch's line [its edges], m right.
			const int every = line->step > 0.0f ? (int)(10.0f / line->step + 0.5f) : 5;
			for (int c = 0; c < k.n;) {
				int w = snprintf(msg, sizeof(msg), "trackroute:   fork %d from %.0f m:", t->n - 1,
					(double)((float)(k.k0 + c) * line->step));
				for (int m = 0; m < 8 && c < k.n; m++, c += every > 0 ? every : 1)
					w += snprintf(msg + w, sizeof(msg) - (size_t)w, " %+.0f[%+.0f,%+.0f]",
						(double)k.cell[c].lat, (double)k.cell[c].lo, (double)k.cell[c].hi);
				log(msg);
			}
		}
	}
	free(vb);
	if (log) {
		snprintf(msg, sizeof(msg), "trackroute: %d fork(s) from %d racer's sign(s) of %d",
			t->n, fours, s->nSigns);
		log(msg);
	}
	if (t->n == 0) {
		free(t);
		return NULL;
	}
	// In order along the line.
	for (int a = 1; a < t->n; a++)
		for (int b = a; b > 0 && t->f[b].k0 + t->f[b].sign < t->f[b - 1].k0 + t->f[b - 1].sign; b--) {
			const TRFork x = t->f[b];
			t->f[b] = t->f[b - 1];
			t->f[b - 1] = x;
		}
	return t;
}

void trackroute_forks_free(TrackForks* t) {
	if (!t) return;
	for (int i = 0; i < t->n; i++) forkFree(&t->f[i]);
	free(t);
}

int trackroute_fork_count(const TrackForks* t) { return t ? t->n : 0; }

bool trackroute_fork_info(const TrackForks* t, int i, TrackFork* out) {
	if (!t || i < 0 || i >= t->n) return false;
	const TRFork* k = &t->f[i];
	const float step = t->line->step;
	out->start = (float)k->k0 * step;
	out->sign = (float)(k->k0 + k->sign) * step;
	out->span = (float)(k->n - 1) * step;
	out->p = k->p;
	out->branch = k->branch;
	return true;
}

// `dist` as samples on from fork `k`'s first, a loop's wrapped; -1 outside it.
static float forkRel(const TrackForks* t, const TRFork* k, float dist) {
	const AiLine* line = t->line;
	float rel = dist / line->step - (float)k->k0;
	if (line->loop) {
		const float n = (float)line->n;
		rel = fmodf(rel, n);
		if (rel < 0.0f) rel += n;
	}
	return rel >= 0.0f && rel <= (float)(k->n - 1) ? rel : -1.0f;
}

bool trackroute_fork_road(const TrackForks* t, int i, float dist, TrackLaneRoad* out) {
	if (!t || i < 0 || i >= t->n) return false;
	const TRFork* k = &t->f[i];
	const float rel = forkRel(t, k, dist);
	if (rel < 0.0f) return false;
	const int a = (int)rel;
	const int b = a + 1 < k->n ? a + 1 : a;
	const float f = rel - (float)a;
	const TrackLaneRoad* p = &k->cell[a];
	const TrackLaneRoad* q = &k->cell[b];
	out->lat = lerpf(p->lat, q->lat, f);
	out->lo = lerpf(p->lo, q->lo, f);
	out->hi = lerpf(p->hi, q->hi, f);
	out->z = lerpf(p->z, q->z, f);
	return true;
}

bool trackroute_fork_curv(const TrackForks* t, int i, float dist, float* curv) {
	if (!t || i < 0 || i >= t->n || !t->f[i].curv) return false;
	const TRFork* k = &t->f[i];
	const float rel = forkRel(t, k, dist);
	if (rel < 0.0f) return false;
	const int a = (int)rel;
	const int b = a + 1 < k->n ? a + 1 : a;
	*curv = lerpf(k->curv[a], k->curv[b], rel - (float)a);
	return true;
}

float trackroute_fork_ahead(const TrackForks* t, int i, float dist) {
	if (!t || i < 0 || i >= t->n) return 1e9f;
	const TRFork* k = &t->f[i];
	const AiLine* line = t->line;
	float d = (float)(k->k0 + k->sign) * line->step - dist;
	if (line->loop) {
		d = fmodf(d, line->len);
		if (d < -0.5f * line->len) d += line->len;
		else if (d > 0.5f * line->len) d -= line->len;
	}
	return d;
}
