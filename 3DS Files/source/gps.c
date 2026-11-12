#include "gps.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See gps.h for what the four retail objects are. Every constant below is the
// Speed.elf float it is read from; the addresses are given where each is used.

// ---------------------------------------------------------------------------
// The routes: TRACKS/ROUTESL4RA/RoutesFreeRoam.bin
// ---------------------------------------------------------------------------
//
// The same chunk list trackroute.c reads for a race (LoaderTrackRoutes,
// 0x801FF230): 0x34121 is the routes back to back, each 0x8C of header then
// its points at 0x38 each; 0x34122 is the signposts, a 4-byte header and then
// 0x20 each. Retail keeps two route tables: DAT_803EFDE4[route number] for
// every by-number lookup (a point's route, a signpost's target) and
// DAT_803F2CCC[i], the routes in file order, which BuildInitialOpenList walks.
//
// 0x34123 is the TrafficIntersections, 0x114 each; LoaderTrackRoutes keeps
// the chunk as it is (r13-0x62B0, the count at r13-0x62AC) and a point names
// its own in bits 19-24 of its bits, 0x3F for none.
#define GPS_CHUNK_ROUTES    0x00034121u
#define GPS_CHUNK_SIGNPOSTS 0x00034122u
#define GPS_CHUNK_INTERS    0x00034123u
#define GPS_ROUTE_HDR       0x8C
#define GPS_POINT_SIZE      0x38
#define GPS_SIGN_SIZE       0x20
#define GPS_INTER_SIZE      0x114
#define GPS_INTER_NONE      0x3F
#define GPS_ROUTE_NUMS      0x800 // a point's route number is bits 0-10
#define GPS_LINES           10    // driving lines per point and route

// One TrackRoutePoint, kept in its file encoding and converted on read the way
// the engine's accessors do.
typedef struct {
	float x, y;     // +0x00 / +0x04
	float raceDist; // +0x08, metres round the race (0 in RoutesFreeRoam.bin)
	u32 bits;       // +0x0C: 0-10 route number, 27 posted for players
	u16 len;        // +0x10, metres x 256 to the next point (GetLength)
	s16 z8;         // +0x12, height x 8 (<<13 then /65536)
	u16 halfL;      // +0x14, x 256, left of the route's own direction
	u16 halfR;      // +0x16, x 256, right of it
	s16 fx, fy;     // +0x18 / +0x1A, the forward direction x 32767
	u16 sign[2];    // +0x1C, signpost indices, 0xFFFF for none
	s8 side;        // +0x20, cross slope x 128 (<<9 then /65536)
	s8 slope;       // +0x21, forward slope x 128
	// +0x22, one per driving line: its offset across the road as a fraction
	// (x 128) of the wider half-width. Lines 0-1 are the racing lines, 2-5
	// the forward traffic lanes, 6-9 the backward ones (IsTrafficLane).
	s8 line[GPS_LINES];
	// +0x2C, one per driving line: its speed in m/s, which FuturePoint::
	// GetDrivingLineSpeed hands InterpolateDrivingLineSpeed. 100 on the
	// racing lines (no limit), 6..20 on the traffic lanes.
	u8 speed[GPS_LINES];
} GpsPoint;

typedef struct {
	int first;              // the route's first point in gn->pts
	int count;              // +0x34
	float bbMin[2], bbMax[2]; // +0x60 / +0x68
	u32 bits;               // +0x70, TrackRoute::GetBits
	s8 line[GPS_LINES];     // +0x82 + line: non-zero where the route has it
} GpsRoute;

// One Signpost. `to` and `toOld` are GetNewRoutePoint resolved at load: the
// point a taken signpost lands on, through the new fields or, for type 3, the
// old ones (GetNewRoute/GetNewRoutePoint pick by type).
typedef struct {
	u32 bits;  // +0x0C: 0-3 type, 18 side, 23 flips direction, 26 corner
	int to;    // -1 when the file's indices do not resolve
	int from;  // GetOldRoutePoint (+0x06 on route +0x0A), -1 likewise
	s16 oldRn; // GetOldRouteNumber, +0x0A
	s16 newRn; // GetNewRouteNumber: +0x08, or +0x0A for a dead end
} GpsSign;

// One TrafficIntersection's fixed part: the two routes that cross there
// (+0x04, by number in the file, by index here) and the point on each where
// they do (+0x0A). The rest of the record (+0x08 the soonest car each way,
// +0x10 the baton, +0x14 the cars) is the traffic's own state (traffic.c).
typedef struct {
	int type;      // +0x00: 1 give way, 2 the second route stops, 3 all stop
	int route[2];  // -1 where the number names no route
	int point[2];  // the point on each, -1 where it does not resolve
	bool relevant; // IsRelevant (0x8002C1C8): both routes, each with bit 6
} GpsInter;

// One road network: a Routes file as LoaderTrackRoutes keeps it.
typedef struct {
	bool loaded;
	GpsPoint* pts;
	s16* ptRoute; // the route each point sits in, by position
	int nPts;
	GpsRoute* routes;
	int nRoutes;
	s16 byNum[GPS_ROUTE_NUMS];
	GpsSign* signs;
	int nSigns;
	GpsInter* inters;
	int nInters;
	// InitTrackRoutes' barrier pass (gps_set_barriers): the file's own bits,
	// kept so a new set of barriers starts again from them as retail's
	// RestoreSignposts does, and the key of the set last applied.
	u32* bakPtBits;
	u16* bakPtSign; // two per point
	u32* bakSignBits;
	u32* bakRouteBits;
	u32 barKey;
} GpsNet;

// The two networks the port keeps: RoutesFreeRoam.bin, and a race's own
// Routes<track><F|B>.bin for its traffic (gps_race_load). Retail's
// TrackRouteManager holds one or the other, whichever the mode loaded; the
// port keeps the free-roam one through a race, for the world it returns to,
// and `gn` says which one every accessor below reads (gps_net_select).
static GpsNet g_net[2];
static GpsNet* gn = &g_net[GPS_NET_FREEROAM];

static u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
static float rdf(const u8* p) { u32 v = rd32(p); float f; memcpy(&f, &v, 4); return f; }

// --- TrackRoutePoint accessors -------------------------------------------

static float ptX(int p) { return gn->pts[p].x; }
static float ptY(int p) { return gn->pts[p].y; }
static float ptZ(int p) { return gn->pts[p].z8 * (8192.0f / 65536.0f); }
static float ptLen(int p) { return gn->pts[p].len * (256.0f / 65536.0f); }
static float ptHalfL(int p) { return gn->pts[p].halfL * (256.0f / 65536.0f); }
static float ptHalfR(int p) { return gn->pts[p].halfR * (256.0f / 65536.0f); }
static bool ptPosted(int p) { return (gn->pts[p].bits >> 27) & 1; } // IsPosted, type 2

// GetForwardDirection (0x8030230C).
static void ptFwd(int p, float d[2]) {
	d[0] = gn->pts[p].fx * (1.0f / 32767.0f);
	d[1] = gn->pts[p].fy * (1.0f / 32767.0f);
}

static int ptIndex(int p) { return p - gn->routes[gn->ptRoute[p]].first; }

// GetPreviousPoint (0x801F7D2C): NULL at a route's first point.
static int ptPrev(int p) { return ptIndex(p) > 0 ? p - 1 : -1; }

static int ptNext(int p) {
	const GpsRoute* r = &gn->routes[gn->ptRoute[p]];
	return p + 1 < r->first + r->count ? p + 1 : -1;
}

// GetRelativePoint: the next point the way the walk faces.
static int ptRelative(int p, bool fwd) { return fwd ? ptNext(p) : ptPrev(p); }

// The length behind a point: the previous point's, or its own at a route's
// start -- the engine's own fallback wherever it measures backwards.
static float ptLenBack(int p) {
	const int q = ptPrev(p);
	return ptLen(q >= 0 ? q : p);
}

// GetBackwardDirection: the reverse of the previous point's direction, or of
// the point's own at a route's start.
static void ptBackDir(int p, float d[2]) {
	const int q = ptPrev(p);
	ptFwd(q >= 0 ? q : p, d);
	d[0] = -d[0];
	d[1] = -d[1];
}

static int ptNumSigns(int p) {
	int n = 0;
	while (n < 2 && gn->pts[p].sign[n] != 0xFFFF)
		n++;
	return n;
}

static int ptSign(int p, int i) { return gn->pts[p].sign[i]; }

// How far (px,py) is outside a point's road box, the test CloseEnoughToGoal
// (0x8003F504), BuildInitialOpenList and GetClosestRoutePoint all make: < 0
// inside. Sideways against the two half-widths, along against the length
// ahead and the length behind.
static float ptOutLateral(int p, float px, float py) {
	float d[2];
	ptFwd(p, d);
	const float lat = (px - ptX(p)) * d[1] - (py - ptY(p)) * d[0];
	return lat <= 0.0f ? -(ptHalfL(p) + lat) : lat - ptHalfR(p);
}

static float ptOutAlong(int p, float px, float py) {
	float d[2];
	ptFwd(p, d);
	const float along = (px - ptX(p)) * d[0] + (py - ptY(p)) * d[1];
	return along <= 0.0f ? -(along + ptLenBack(p)) : along - ptLen(p);
}

// --- Signpost accessors ---------------------------------------------------
//
// ePostednessType 2 is the player's. Its predicates on the type nibble:
// IsAlways for 2 and 8, Ignore for 7, IsDontTake for 5 and 10, and type 3 is a
// dead end every walk stops at.
#define SIGN_DEAD_END 3

static int spType(int s) { return (int)(gn->signs[s].bits & 15); }
static bool spBit(int s, int b) { return (gn->signs[s].bits >> b) & 1; }
// A signpost stands on one side of the road: bit 18 clear is for traffic
// going the route's way, set for the other.
static bool spApplies(int s, bool fwd) { return spBit(s, 18) != fwd; }
static bool spAlways(int s) { const int t = spType(s); return t == 2 || t == 8; }
static bool spIgnore(int s) { return spType(s) == 7; }
static bool spDontTake(int s) { const int t = spType(s); return t == 5 || t == 10; }

// --- trackroute_nearest's source (TASKS.md #279) ---------------------------

static void srcRoute(const void* ctx, int r, int* first, int* count, float bbMin[2],
		float bbMax[2], u32* bits) {
	(void)ctx;
	const GpsRoute* rt = &gn->routes[r];
	*first = rt->first;
	*count = rt->count;
	bbMin[0] = rt->bbMin[0];
	bbMin[1] = rt->bbMin[1];
	bbMax[0] = rt->bbMax[0];
	bbMax[1] = rt->bbMax[1];
	*bits = rt->bits;
}

static void srcPoint(const void* ctx, int p, TrackRoutePt* out) {
	(void)ctx;
	out->pos[0] = ptX(p);
	out->pos[1] = ptY(p);
	out->pos[2] = ptZ(p);
	ptFwd(p, out->fwd);
	ptBackDir(p, out->back);
	out->raceDist = gn->pts[p].raceDist; // 0 throughout the free-roam file
	out->posted = ptPosted(p);
	out->speed = gn->pts[p].speed[0];
}

TrackRouteSource gps_route_source(void) {
	TrackRouteSource s;
	memset(&s, 0, sizeof(s));
	if (gn->loaded) {
		s.nRoutes = gn->nRoutes;
		s.route = srcRoute;
		s.point = srcPoint;
	}
	return s;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static void netFree(GpsNet* n) {
	free(n->pts);
	free(n->ptRoute);
	free(n->routes);
	free(n->signs);
	free(n->inters);
	free(n->bakPtBits);
	free(n->bakPtSign);
	free(n->bakSignBits);
	free(n->bakRouteBits);
	memset(n, 0, sizeof(*n));
}

void gps_unload(void) { netFree(&g_net[GPS_NET_FREEROAM]); }

bool gps_loaded(void) { return g_net[GPS_NET_FREEROAM].loaded; }

static void gpsLog(void (*log)(const char*), const char* msg) {
	if (log)
		log(msg);
}

// LoaderTrackRoutes (0x801FF230) into the network `gn` points at.
static bool netLoad(const char* path, void (*log)(const char* msg)) {
	char line[160];
	const char* const tag = gn == &g_net[GPS_NET_RACE] ? "gps race" : "gps";
	netFree(gn);
	FILE* f = fopen(path, "rb");
	if (!f) {
		snprintf(line, sizeof(line), "%s: %s missing", tag, path);
		gpsLog(log, line);
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long fileSize = ftell(f);

	// Pass one: find the chunks and count the routes and points, reading
	// only headers.
	long routesOff = -1, signsOff = -1, intersOff = -1;
	u32 routesSize = 0, signsSize = 0, intersSize = 0;
	for (long off = 0; off + 8 <= fileSize; ) {
		u8 h[8];
		if (fseek(f, off, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8)
			break;
		const u32 type = rd32(h), size = rd32(h + 4);
		if ((long)size > fileSize - (off + 8))
			break;
		if (type == GPS_CHUNK_ROUTES && routesOff < 0) {
			routesOff = off + 8;
			routesSize = size;
		} else if (type == GPS_CHUNK_SIGNPOSTS && signsOff < 0) {
			signsOff = off + 8;
			signsSize = size;
		} else if (type == GPS_CHUNK_INTERS && intersOff < 0) {
			intersOff = off + 8;
			intersSize = size;
		}
		off += 8 + (long)size;
	}
	int nRoutes = 0, nPts = 0;
	for (u32 o = 0; routesOff >= 0 && o + GPS_ROUTE_HDR <= routesSize; ) {
		u8 h[2];
		if (fseek(f, routesOff + (long)o + 0x34, SEEK_SET) != 0 || fread(h, 1, 2, f) != 2)
			break;
		const int n = (s16)rd16(h);
		if (n < 0 || o + GPS_ROUTE_HDR + (u32)n * GPS_POINT_SIZE > routesSize)
			break;
		nRoutes++;
		nPts += n;
		o += GPS_ROUTE_HDR + (u32)n * GPS_POINT_SIZE;
	}
	const int nSigns = signsOff >= 0 && signsSize >= 4 ? (int)((signsSize - 4) / GPS_SIGN_SIZE) : 0;
	if (nRoutes == 0 || nPts == 0) {
		fclose(f);
		snprintf(line, sizeof(line), "%s: %s holds no routes", tag, path);
		gpsLog(log, line);
		return false;
	}

	gn->pts = (GpsPoint*)malloc(sizeof(GpsPoint) * (size_t)nPts);
	gn->ptRoute = (s16*)malloc(sizeof(s16) * (size_t)nPts);
	gn->routes = (GpsRoute*)malloc(sizeof(GpsRoute) * (size_t)nRoutes);
	gn->signs = (GpsSign*)malloc(sizeof(GpsSign) * (size_t)(nSigns > 0 ? nSigns : 1));
	const int nInters = intersOff >= 0 ? (int)(intersSize / GPS_INTER_SIZE) : 0;
	gn->inters = (GpsInter*)malloc(sizeof(GpsInter) * (size_t)(nInters > 0 ? nInters : 1));
	u8* buf = (u8*)malloc(GPS_ROUTE_HDR + 0x400 * GPS_POINT_SIZE);
	if (!gn->pts || !gn->ptRoute || !gn->routes || !gn->signs || !gn->inters || !buf) {
		free(buf);
		fclose(f);
		netFree(gn);
		snprintf(line, sizeof(line), "%s: out of memory", tag);
		gpsLog(log, line);
		return false;
	}
	for (int i = 0; i < GPS_ROUTE_NUMS; i++)
		gn->byNum[i] = -1;

	// Pass two: decode. A route is read whole -- at most 0x400 points, the
	// loader's own bound (it strides by 0xE08C - (0x400 - n) * 0x38).
	int badNum = 0;
	u32 o = 0;
	for (int r = 0; r < nRoutes; r++) {
		u8 h[GPS_ROUTE_HDR];
		if (fseek(f, routesOff + (long)o, SEEK_SET) != 0 || fread(h, 1, GPS_ROUTE_HDR, f) != GPS_ROUTE_HDR)
			break;
		int n = (s16)rd16(h + 0x34);
		if (n > 0x400)
			n = 0x400;
		if (fread(buf, GPS_POINT_SIZE, (size_t)n, f) != (size_t)n)
			break;
		GpsRoute* rt = &gn->routes[r];
		rt->first = gn->nPts;
		rt->count = n;
		rt->bbMin[0] = rdf(h + 0x60);
		rt->bbMin[1] = rdf(h + 0x64);
		rt->bbMax[0] = rdf(h + 0x68);
		rt->bbMax[1] = rdf(h + 0x6C);
		rt->bits = rd32(h + 0x70);
		memcpy(rt->line, h + 0x82, GPS_LINES);
		const int num = rd16(h + 0x0A);
		if (num < GPS_ROUTE_NUMS)
			gn->byNum[num] = (s16)r;
		for (int i = 0; i < n; i++) {
			const u8* q = buf + i * GPS_POINT_SIZE;
			GpsPoint* p = &gn->pts[gn->nPts];
			p->x = rdf(q);
			p->y = rdf(q + 4);
			p->raceDist = rdf(q + 8);
			p->bits = rd32(q + 0x0C);
			p->len = rd16(q + 0x10);
			p->z8 = (s16)rd16(q + 0x12);
			p->halfL = rd16(q + 0x14);
			p->halfR = rd16(q + 0x16);
			p->fx = (s16)rd16(q + 0x18);
			p->fy = (s16)rd16(q + 0x1A);
			p->sign[0] = rd16(q + 0x1C);
			p->sign[1] = rd16(q + 0x1E);
			p->side = (s8)q[0x20];
			p->slope = (s8)q[0x21];
			memcpy(p->line, q + 0x22, GPS_LINES);
			memcpy(p->speed, q + 0x2C, GPS_LINES);
			if ((int)(p->bits & 0x7FF) != num)
				badNum++;
			gn->ptRoute[gn->nPts] = (s16)r;
			gn->nPts++;
		}
		gn->nRoutes++;
		o += GPS_ROUTE_HDR + (u32)n * GPS_POINT_SIZE;
	}
	free(buf);

	// The signposts, resolved to the point each one lands on.
	int badSign = 0;
	u8 sp[GPS_SIGN_SIZE];
	for (int i = 0; i < nSigns; i++) {
		if (fseek(f, signsOff + 4 + (long)i * GPS_SIGN_SIZE, SEEK_SET) != 0 ||
			fread(sp, 1, GPS_SIGN_SIZE, f) != GPS_SIGN_SIZE)
			break;
		GpsSign* s = &gn->signs[i];
		s->bits = rd32(sp + 0x0C);
		const bool old = (s->bits & 15) == SIGN_DEAD_END;
		const int pt = (s16)rd16(sp + (old ? 6 : 4));
		const int rn = (s16)rd16(sp + (old ? 0x0A : 8));
		const int r = rn >= 0 && rn < GPS_ROUTE_NUMS ? gn->byNum[rn] : -1;
		s->to = r >= 0 && pt >= 0 && pt < gn->routes[r].count ? gn->routes[r].first + pt : -1;
		if (s->to < 0)
			badSign++;
		{
			const int opt = (s16)rd16(sp + 6);
			const int orn = (s16)rd16(sp + 0x0A);
			const int orr = orn >= 0 && orn < GPS_ROUTE_NUMS ? gn->byNum[orn] : -1;
			s->from = orr >= 0 && opt >= 0 && opt < gn->routes[orr].count ? gn->routes[orr].first + opt : -1;
			s->oldRn = (s16)orn;
			s->newRn = (s16)rn;
		}
		gn->nSigns++;
	}

	// The traffic intersections, their routes and points by index.
	int relevant = 0;
	u8 ir[0x0E];
	for (int i = 0; i < nInters; i++) {
		if (fseek(f, intersOff + (long)i * GPS_INTER_SIZE, SEEK_SET) != 0 ||
			fread(ir, 1, sizeof(ir), f) != sizeof(ir))
			break;
		GpsInter* in = &gn->inters[i];
		in->type = (int)rd32(ir);
		in->relevant = true;
		for (int k = 0; k < 2; k++) {
			const int rn = (s16)rd16(ir + 4 + k * 2);
			const int pt = (s16)rd16(ir + 0x0A + k * 2);
			const int r = rn >= 0 && rn < GPS_ROUTE_NUMS ? gn->byNum[rn] : -1;
			in->route[k] = r;
			in->point[k] = r >= 0 && pt >= 0 && pt < gn->routes[r].count ? gn->routes[r].first + pt : -1;
			// GetBool(6) of each route.
			if (r < 0 || !((gn->routes[r].bits >> 6) & 1u) || in->point[k] < 0)
				in->relevant = false;
		}
		relevant += in->relevant;
		gn->nInters++;
	}
	fclose(f);

	// A point's signpost list is read packed from slot 0, and every index must
	// name a signpost; anything else is cut off here rather than walked.
	int badRef = 0;
	for (int p = 0; p < gn->nPts; p++) {
		for (int k = 0; k < 2; k++) {
			const u16 s = gn->pts[p].sign[k];
			if (s != 0xFFFF && (s >= gn->nSigns || (k == 1 && gn->pts[p].sign[0] == 0xFFFF))) {
				gn->pts[p].sign[k] = 0xFFFF;
				badRef++;
			}
		}
	}

	gn->loaded = true;
	snprintf(line, sizeof(line),
		"%s: %d routes, %d points, %d signposts (%d/%d/%d unresolved), "
		"%d traffic intersections (%d relevant)",
		tag, gn->nRoutes, gn->nPts, gn->nSigns, badNum, badSign, badRef, gn->nInters, relevant);
	gpsLog(log, line);
	return true;
}

bool gps_load(const char* path, void (*log)(const char* msg)) {
	GpsNet* const keep = gn;
	gn = &g_net[GPS_NET_FREEROAM];
	const bool ok = netLoad(path, log);
	gn = keep;
	return ok;
}

bool gps_race_load(const char* path, void (*log)(const char* msg)) {
	GpsNet* const keep = gn;
	gn = &g_net[GPS_NET_RACE];
	const bool ok = netLoad(path, log);
	gn = keep;
	return ok;
}

void gps_race_unload(void) {
	if (gn == &g_net[GPS_NET_RACE])
		gn = &g_net[GPS_NET_FREEROAM];
	netFree(&g_net[GPS_NET_RACE]);
}

// ---------------------------------------------------------------------------
// InitTrackRoutes' barrier pass (TASKS.md #358)
// ---------------------------------------------------------------------------
//
// InitTrackRoutes (0x801FFA18), run by TrackLoader::FinishedLoadingPart1
// after InitTopologyAndSceneryGroups has closed the gates EnableBarriers
// names, takes RoutesFreeRoam.bin's signposts back to their file types
// (RestoreSignposts 0x801FEF44 -- the shipped bits 0-3/4-7 already equal
// their originals at 8-11/12-15, so that is the file as loaded) and then,
// in free roam (the TrackInfo race flags all clear):
//   CheckForBarrierOverlap 0x801FE828  route bool 12: its box touches a gate
//   EliminateBarrierViolations 0x801F5F18, until nothing changes
//   ResetPostedness 0x801FDE1C + DeterminePostedness 0x801FE304
//   MakeUnpostedRoutesIrrelevant 0x801FE6EC, FindIrrelevantSignposts 0x801F708C
// Without it the GPS and the racers drove the roads stage 0's
// BARRIERS_CAREER0 closes: the GPS kept routing the player down the closed
// road beside Rachel's car and re-routing (with UpdateGPS's sound) every few
// metres as they drove round it. A host replay of this pass on the shipped
// file with no gates reproduces its posted bits exactly.

#define BAR_TYPES 3 // ePostednessType: 0 racers, 1 traffic, 2 players

static float msSinceBar(u64 t0) { return (float)(svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000.0f); }

typedef struct { float a[2], b[2]; } BarGate;
static BarGate* s_gates;
static int s_nGates;

static int barType(int s, int t) {
	const u32 b = gn->signs[s].bits;
	if (t == 1)
		return (int)((b >> 4) & 15);
	return (int)(b & 15);
}

// ChangeSignType / SetSignType (0x801F4C90).
static void barSetType(int s, int t, int v) {
	u32* b = &gn->signs[s].bits;
	if (t == 1)
		*b = (*b & ~0xF0u) | ((u32)v << 4);
	else
		*b = (*b & ~0x0Fu) | (u32)v;
}

static bool barAlways(int s, int t) {
	const int ty = barType(s, t);
	return t == 2 ? (ty == 2 || ty == 8) : ty == 2;
}
static bool barIgnore(int s, int t) {
	const int ty = barType(s, t);
	return t == 0 ? (ty == 7 || ty == 8) : ty == 7;
}
static bool barNeverTake(int s, int t) { return barType(s, t) == 10; }

static int barRouteOf(int rn) { return rn >= 0 && rn < GPS_ROUTE_NUMS ? gn->byNum[rn] : -1; }
static int barOldRoute(int s) { return barRouteOf(gn->signs[s].oldRn); }
static int barNewRoute(int s) { return barRouteOf(gn->signs[s].newRn); }
static bool barRBool(int r, int b) { return (gn->routes[r].bits >> b) & 1u; }
static void barSetRBool(int r, int b, bool v) {
	gn->routes[r].bits = (gn->routes[r].bits & ~(1u << b)) | ((v ? 1u : 0u) << b);
}

// IsPosted / IsPostedDirectional / SetPosted per type: racers 11/18,
// traffic 25/26, players 27/28.
static const u8 kPostBit[BAR_TYPES][2] = { { 11, 18 }, { 25, 26 }, { 27, 28 } };
static bool barPosted(int p, int t) { return (gn->pts[p].bits >> kPostBit[t][0]) & 1u; }
static bool barPostedDir(int p, int t, bool fwd) { return (gn->pts[p].bits >> kPostBit[t][fwd ? 0 : 1]) & 1u; }

// DoLinesIntersect (0x801FFCF8).
static bool barLinesCross(const float a[2], const float b[2], const float c[2], const float d[2]) {
	const float f1 = b[1] - a[1], f4 = d[0] - c[0], f6 = b[0] - a[0], f3 = d[1] - c[1];
	const float f7 = f6 * f3 - f1 * f4;
	if (f7 == 0.0f)
		return false;
	const float f5 = a[0] - c[0], f2 = a[1] - c[1];
	const float u = (f2 * f4 - f5 * f3) / f7;
	if (u < 0.0f || u > 1.0f)
		return false;
	const float v = (f2 * f6 - f5 * f1) / f7;
	return v >= 0.0f && v <= 1.0f;
}

// CrossesBarrier (0x801F598C): the first closed gate the step crosses, which
// counts for traffic only when its +0x12 is 0 -- and EnableBarriers sets
// +0x12 with +0x10 on every gate it closes, so traffic crosses them all.
static bool barCrosses(int p, int q, int t) {
	if (t == 1)
		return false;
	const float a[2] = { ptX(p), ptY(p) }, b[2] = { ptX(q), ptY(q) };
	for (int i = 0; i < s_nGates; i++)
		if (barLinesCross(s_gates[i].a, s_gates[i].b, a, b))
			return true;
	return false;
}

// GetNextPointOnSegment (0x801F7E28) with takeZero false.
static int barNextOnSeg(int p, bool* fwd, int t, bool skipPosted) {
	if (!skipPosted && !barPosted(p, t))
		return -1;
	const int ns = ptNumSigns(p);
	for (int i = 0; i < ns; i++) {
		const int s = ptSign(p, i);
		if (spApplies(s, *fwd) && barAlways(s, t)) {
			if (spBit(s, 23))
				*fwd = !*fwd;
			if (gn->signs[s].to != p)
				return gn->signs[s].to;
			break;
		}
	}
	const int q = ptRelative(p, *fwd);
	if (q < 0)
		return -1;
	return skipPosted || barPosted(q, t) ? q : -1;
}

// WillCrossBarrier (0x801F5A6C): does driving on from p (the step from
// `prev` first) reach a closed gate within 200 m (0x8033684C), before a
// signpost onto another route?
static bool barWillCross(int p, int t, bool fwd, int sign, int prev) {
	if (prev >= 0 && p >= 0 && barCrosses(prev, p, t))
		return true;
	if (p < 0 || !barRBool(gn->ptRoute[p], 12))
		return false;
	float dist = 0.0f;
	while (p >= 0) {
		const int ns = ptNumSigns(p);
		for (int i = 0; i < ns; i++) {
			const int s = ptSign(p, i);
			if (s != sign && !barIgnore(s, t) && spApplies(s, fwd) &&
				gn->signs[s].oldRn != gn->signs[s].newRn)
				return false;
		}
		const int q = barNextOnSeg(p, &fwd, t, true);
		if (q >= 0) {
			if (barCrosses(p, q, t))
				return true;
			dist += hypotf(ptX(p) - ptX(q), ptY(p) - ptY(q));
		}
		p = q;
		if (dist >= 200.0f)
			return false;
	}
	return false;
}

// GetTwin (0x801F4D14): the signpost at the landing point leading back.
static int barTwin(int s) {
	const int o = gn->signs[s].from, n = gn->signs[s].to;
	if (n == o)
		return s;
	if (n < 0)
		return -1;
	const bool b18 = spBit(s, 18), b23 = spBit(s, 23);
	const int ns = ptNumSigns(n);
	for (int i = 0; i < ns; i++) {
		const int u = ptSign(n, i);
		const bool ok = !spBit(u, 18) ? (b18 != b23) : (b18 == b23);
		if (ok && gn->signs[u].to == o)
			return u;
	}
	return -1;
}

// MostFruitfulExit (0x801F4FA4). Its tie-break compares the race distance
// 30 m down each exit, 0 throughout the free-roam file, so the first wins.
static int barMostFruitful(int p, bool fwd, int t) {
	const int ns = ptNumSigns(p);
	for (int i = 0; i < ns && i < 2; i++) {
		const int s = ptSign(p, i);
		if (!barIgnore(s, t) && gn->signs[s].oldRn != gn->signs[s].newRn && spApplies(s, fwd))
			return s;
	}
	return -1;
}

// MakeUnreachable (0x801F5218): close the way into the road behind `sign`.
static void barMakeUnreachable(int sign, int t, int depth) {
	if (depth >= 0x65)
		return;
	const int r0 = barOldRoute(sign);
	if (r0 < 0 || !barRBool(r0, 6))
		return;
	const bool b18 = spBit(sign, 18);
	const bool fwd = !b18;
	int p = gn->signs[sign].from;
	while (p >= 0) {
		// (Route bool 7 is set nowhere in the free-roam file.)
		int ns = ptNumSigns(p);
		for (int i = 0; i < ns; i++) {
			int s = ptSign(p, i);
			if (s == sign || !spApplies(s, fwd) || barIgnore(s, t) || barNeverTake(s, t))
				continue;
			const int nr = barNewRoute(s);
			if (nr == r0)
				continue;
			if (nr >= 0 && barRBool(nr, 6)) {
				if (barAlways(s, t))
					return;
				if (spBit(s, 26)) {
					const int m = barMostFruitful(p, fwd, t);
					if (m >= 0)
						s = m;
				}
				barSetType(s, t, 2);
				return;
			}
			barSetType(s, t, 7);
		}
		ns = ptNumSigns(p);
		for (int i = 0; i < ns; i++) {
			const int s = ptSign(p, i);
			if (s == sign || spApplies(s, fwd))
				continue;
			const int tw = barTwin(s);
			if (tw < 0 || barIgnore(tw, t) || barNeverTake(tw, t) || spType(s) == SIGN_DEAD_END)
				continue;
			const int ro = barOldRoute(tw);
			if (ro < 0 || !barRBool(ro, 6))
				continue;
			const bool always = barAlways(tw, t);
			barSetType(tw, t, 7);
			if (always)
				barMakeUnreachable(tw, t, depth + 1);
		}
		p = ptRelative(p, b18);
	}
}

static int barEliminate(void) {
	int changed = 0;
	for (int s = 0; s < gn->nSigns; s++) {
		for (int t = 0; t < BAR_TYPES; t++) {
			if (barIgnore(s, t))
				continue;
			const int o = gn->signs[s].from, n = gn->signs[s].to;
			const bool u7 = !spBit(s, 18);
			const bool b11 = u7 != spBit(s, 23);
			const int ty = barType(s, t);
			bool b1 = false, b2 = false, b3 = false;
			if (ty == 2) {
				if (barWillCross(n, t, b11, s, o))
					b1 = b3 = true;
			} else if (ty == 0 || ty == 1 || ty == 4 || ty == 6 || ty == 9) {
				const bool a = barWillCross(o, t, u7, s, -1);
				const bool b = barWillCross(n, t, b11, s, o);
				if (a && b)
					b1 = b3 = true;
				else if (b)
					b1 = true;
				else if (a)
					b2 = true;
			}
			if (b1)
				barSetType(s, t, 7);
			if (b2)
				barSetType(s, t, 2);
			if (b3)
				barMakeUnreachable(s, t, 1);
			changed += b1 || b2 || b3;
		}
	}
	return changed;
}

// MarkPostedness (0x801FDF40), literally: the loop's flags carry over from
// one point to the next as retail's do.
static void barMark(int p, bool dir, int t) {
	const bool b10 = ((gn->routes[gn->ptRoute[p]].bits >> 1) & 1u) ? !dir : dir;
	bool b1 = false, b2 = false;
	int q = p;
	for (;;) {
		q = ptRelative(q, dir);
		if (q < 0) {
			b2 = true;
		} else if (!barPostedDir(q, t, b10)) {
			const int ns = ptNumSigns(q);
			for (int i = 0; i < ns; i++) {
				const int s = ptSign(q, i);
				if (barIgnore(s, t))
					continue;
				const int ty = spType(s);
				const bool ap = !spBit(s, 18) ? dir : !dir;
				b2 = ap && (ty == SIGN_DEAD_END || barAlways(s, t));
				const bool b3 = ((gn->signs[s].bits >> 8) & 15u) == SIGN_DEAD_END || t == 1;
				b1 = ty == SIGN_DEAD_END ? b3 : true;
			}
		} else {
			b1 = b2 = true;
		}
		if (b1 && q >= 0) {
			b1 = false;
			const int r = gn->ptRoute[p];
			barSetRBool(r, 8, true);
			int i0 = ptIndex(p), i1 = ptIndex(q);
			if (i0 > i1) {
				const int k = i0;
				i0 = i1;
				i1 = k;
			}
			const u32 bit = 1u << kPostBit[t][b10 ? 0 : 1];
			for (int i = i0; i <= i1; i++)
				gn->pts[gn->routes[r].first + i].bits |= bit;
			p = q;
		}
		if (b2)
			return;
	}
}

static void barDetermine(void) {
	for (int t = 0; t < BAR_TYPES; t++) {
		for (int s = 0; s < gn->nSigns; s++) {
			if (barIgnore(s, t) || gn->signs[s].to < 0)
				continue;
			const int ty = barType(s, t);
			if (ty == 0 || ty == 1 || ty == 2 || ty == 4 || ty == 5 || ty == 8 || ty == 9 || ty == 10) {
				barMark(gn->signs[s].to, !spBit(s, 18) != spBit(s, 23), t);
			} else if (ty == SIGN_DEAD_END) {
				// The dead end's own original type is 3 throughout the file;
				// route bool 7, the other way in, is set nowhere.
				if (((gn->signs[s].bits >> 8) & 15u) != SIGN_DEAD_END && t != 1)
					continue;
				barMark(gn->signs[s].to, spBit(s, 18), t);
			}
		}
	}
}

// The 0x3414D gates of a Paths file that hold one of `ids`, into s_gates.
static int barReadGates(const char* pathsFile, const u32* ids, int nIds) {
	free(s_gates);
	s_gates = NULL;
	s_nGates = 0;
	FILE* f = fopen(pathsFile, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	long off = 0, gOff = -1;
	u32 gSize = 0;
	while (off + 8 <= size && gOff < 0) {
		u8 h[8];
		if (fseek(f, off, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8)
			break;
		const u32 type = rd32(h), sz = rd32(h + 4);
		if ((long)sz > size - (off + 8))
			break;
		if (type & 0x80000000u) {
			long sub = off + 8;
			const long end = sub + (long)sz;
			while (sub + 8 <= end) {
				u8 sh[8];
				if (fseek(f, sub, SEEK_SET) != 0 || fread(sh, 1, 8, f) != 8)
					break;
				const u32 st = rd32(sh), ss = rd32(sh + 4);
				if ((long)ss > end - (sub + 8))
					break;
				if (st == 0x0003414Du) {
					gOff = sub + 8;
					gSize = ss;
					break;
				}
				sub += 8 + (long)ss;
			}
		}
		off += 8 + (long)sz;
	}
	const int n = (int)(gSize / 0x24);
	u8* buf = n > 0 ? (u8*)malloc(gSize) : NULL;
	if (!buf || fseek(f, gOff, SEEK_SET) != 0 || fread(buf, 1, gSize, f) != gSize) {
		free(buf);
		fclose(f);
		return n > 0 ? -1 : 0;
	}
	fclose(f);
	int on = 0;
	for (int i = 0; i < n; i++) {
		const u8* g = buf + i * 0x24;
		const int k = (s8)g[0x11];
		for (int j = 0; j < k && j < 4; j++) {
			const u32 id = rd32(g + 0x14 + j * 4);
			bool hit = false;
			for (int w = 0; w < nIds; w++)
				hit |= id == ids[w];
			if (hit) {
				on++;
				break;
			}
		}
	}
	s_gates = (BarGate*)malloc(sizeof(BarGate) * (size_t)(on > 0 ? on : 1));
	if (!s_gates) {
		free(buf);
		return -1;
	}
	for (int i = 0; i < n; i++) {
		const u8* g = buf + i * 0x24;
		const int k = (s8)g[0x11];
		bool hit = false;
		for (int j = 0; j < k && j < 4 && !hit; j++) {
			const u32 id = rd32(g + 0x14 + j * 4);
			for (int w = 0; w < nIds; w++)
				hit |= id == ids[w];
		}
		if (hit) {
			BarGate* o = &s_gates[s_nGates++];
			o->a[0] = rdf(g);
			o->a[1] = rdf(g + 4);
			o->b[0] = rdf(g + 8);
			o->b[1] = rdf(g + 12);
		}
	}
	free(buf);
	return s_nGates;
}

static bool barBackup(void) {
	if (gn->bakPtBits)
		return true;
	gn->bakPtBits = (u32*)malloc(sizeof(u32) * (size_t)gn->nPts);
	gn->bakPtSign = (u16*)malloc(sizeof(u16) * 2 * (size_t)gn->nPts);
	gn->bakSignBits = (u32*)malloc(sizeof(u32) * (size_t)(gn->nSigns > 0 ? gn->nSigns : 1));
	gn->bakRouteBits = (u32*)malloc(sizeof(u32) * (size_t)gn->nRoutes);
	if (!gn->bakPtBits || !gn->bakPtSign || !gn->bakSignBits || !gn->bakRouteBits) {
		free(gn->bakPtBits);
		free(gn->bakPtSign);
		free(gn->bakSignBits);
		free(gn->bakRouteBits);
		gn->bakPtBits = NULL;
		gn->bakPtSign = NULL;
		gn->bakSignBits = NULL;
		gn->bakRouteBits = NULL;
		return false;
	}
	for (int p = 0; p < gn->nPts; p++) {
		gn->bakPtBits[p] = gn->pts[p].bits;
		gn->bakPtSign[p * 2] = gn->pts[p].sign[0];
		gn->bakPtSign[p * 2 + 1] = gn->pts[p].sign[1];
	}
	for (int s = 0; s < gn->nSigns; s++)
		gn->bakSignBits[s] = gn->signs[s].bits;
	for (int r = 0; r < gn->nRoutes; r++)
		gn->bakRouteBits[r] = gn->routes[r].bits;
	return true;
}

static void barRestore(void) {
	for (int p = 0; p < gn->nPts; p++) {
		gn->pts[p].bits = gn->bakPtBits[p];
		gn->pts[p].sign[0] = gn->bakPtSign[p * 2];
		gn->pts[p].sign[1] = gn->bakPtSign[p * 2 + 1];
	}
	for (int s = 0; s < gn->nSigns; s++)
		gn->signs[s].bits = gn->bakSignBits[s];
	for (int r = 0; r < gn->nRoutes; r++)
		gn->routes[r].bits = gn->bakRouteBits[r];
}

int gps_set_barriers(const char* pathsFile, const u32* ids, int nIds, void (*log)(const char* msg)) {
	GpsNet* const keep = gn;
	gn = &g_net[GPS_NET_FREEROAM];
	char line[192];
	int closedSigns = -1;
	u32 key = 0x811C9DC5u;
	for (int i = 0; i < nIds; i++)
		key = (key ^ ids[i]) * 0x01000193u;
	if (!gn->loaded || key == gn->barKey || !barBackup()) {
		gn = keep;
		return -1;
	}
	const u64 t0 = svcGetSystemTick();
	barRestore();
	const int nGates = barReadGates(pathsFile, ids, nIds);
	int passes = 0, changes = 0, unposted = 0, dropped = 0;
	if (nGates > 0) {
		// CheckForBarrierOverlap.
		for (int r = 0; r < gn->nRoutes; r++)
			barSetRBool(r, 12, false);
		for (int i = 0; i < s_nGates; i++) {
			const BarGate* g = &s_gates[i];
			const float mn[2] = { fminf(g->a[0], g->b[0]), fminf(g->a[1], g->b[1]) };
			const float mx[2] = { fmaxf(g->a[0], g->b[0]), fmaxf(g->a[1], g->b[1]) };
			for (int r = 0; r < gn->nRoutes; r++) {
				const GpsRoute* rt = &gn->routes[r];
				if (!barRBool(r, 6) || barRBool(r, 12))
					continue;
				barSetRBool(r, 12, !(mx[0] < rt->bbMin[0] || mn[0] > rt->bbMax[0] ||
					mx[1] < rt->bbMin[1] || mn[1] > rt->bbMax[1]));
			}
		}
		int c;
		do {
			c = barEliminate();
			changes += c;
			passes++;
		} while (c != 0 && passes < 64);
		// ResetPostedness, DeterminePostedness.
		u32 m = 0;
		for (int t = 0; t < BAR_TYPES; t++)
			m |= (1u << kPostBit[t][0]) | (1u << kPostBit[t][1]);
		for (int p = 0; p < gn->nPts; p++)
			gn->pts[p].bits &= ~m;
		for (int r = 0; r < gn->nRoutes; r++)
			barSetRBool(r, 8, false);
		barDetermine();
		// MakeUnpostedRoutesIrrelevant.
		for (int r = 0; r < gn->nRoutes; r++) {
			if (!barRBool(r, 8)) {
				barSetRBool(r, 9, false);
				barSetRBool(r, 7, false);
				barSetRBool(r, 6, false);
				unposted++;
			}
		}
		// FindIrrelevantSignposts: a signpost to or from an irrelevant route
		// sorts past the end of the list, gone; here it leaves its point's.
		for (int p = 0; p < gn->nPts; p++) {
			u16 keepS[2] = { 0xFFFF, 0xFFFF };
			int k = 0;
			for (int i = 0; i < 2; i++) {
				const u16 s = gn->pts[p].sign[i];
				if (s == 0xFFFF)
					break;
				const int ro = barOldRoute(s), rn = barNewRoute(s);
				if (ro < 0 || rn < 0 || !barRBool(ro, 6) || !barRBool(rn, 6)) {
					dropped++;
					continue;
				}
				keepS[k++] = s;
			}
			gn->pts[p].sign[0] = keepS[0];
			gn->pts[p].sign[1] = keepS[1];
		}
	}
	// IsRelevant on the intersections again.
	for (int i = 0; i < gn->nInters; i++) {
		GpsInter* in = &gn->inters[i];
		in->relevant = true;
		for (int k = 0; k < 2; k++)
			if (in->route[k] < 0 || !barRBool(in->route[k], 6) || in->point[k] < 0)
				in->relevant = false;
	}
	int lostF = 0, lostB = 0;
	for (int p = 0; p < gn->nPts; p++) {
		lostF += ((gn->bakPtBits[p] >> 27) & 1u) && !((gn->pts[p].bits >> 27) & 1u);
		lostB += ((gn->bakPtBits[p] >> 28) & 1u) && !((gn->pts[p].bits >> 28) & 1u);
	}
	gn->barKey = key;
	closedSigns = changes;
	snprintf(line, sizeof(line),
		"gps: barriers -- %d gate(s) closed, %d signpost change(s) in %d pass(es), "
		"%d route(s) unposted, %d signpost(s) dropped; players lose %d/%d point(s) fwd/bwd, %.1f ms",
		nGates, changes, passes, unposted, dropped, lostF, lostB, msSinceBar(t0));
	gpsLog(log, line);
	gn = keep;
	return closedSigns;
}

int gps_net_select(int net) {
	const int was = (int)(gn - g_net);
	gn = &g_net[net == GPS_NET_RACE ? GPS_NET_RACE : GPS_NET_FREEROAM];
	return was;
}

// ---------------------------------------------------------------------------
// PathfindingNode
// ---------------------------------------------------------------------------
//
// A search state: a point and a facing, the cost so far (g, metres driven) and
// the estimate to go (h, straight-line metres), and the signpost decisions that
// led here. The fields are retail's in retail's order; offsets are given so
// the decompile can be followed.
#define GPS_MAX_SUCC 3   // two signposts and straight on
#define GPS_MAX_DEC  64  // +0x38, u16 each; RecordSignpostDecision stops at 0x3F

typedef struct {
	int start;            // +0x00
	int cur;              // +0x04, -1 for NULL
	float g, h;           // +0x08 / +0x0C
	float goal[3];        // +0x10
	struct {
		int pt;
		s16 sign;         // -1 for straight on, or for an IsAlways signpost
		u8 fwd;
	} succ[GPS_MAX_SUCC]; // +0x20
	u16 dec[GPS_MAX_DEC]; // +0x38: bits 0-12 the signpost, bit 13 taken
	s8 nSucc;             // +0xB8
	s8 nDec;              // +0xB9
	u8 reached;           // +0xBA
	u8 startFwd;          // +0xBB
	u8 fwd;               // +0xBC
} GpsNode;

// EstimateCostToGoal (0x8003FFDC).
static void nodeEstimate(GpsNode* n) {
	if (n->cur < 0)
		n->h = 99999.0f; // 0x80318B64
	else if (n->reached)
		n->h = 0.0f;
	else
		n->h = hypotf(ptX(n->cur) - n->goal[0], ptY(n->cur) - n->goal[1]);
}

// AddSuccessor (0x8003FEAC). Retail has no bound; a point carries at most two
// signposts and IsAlways returns early, so three is the most it ever adds and
// the bound here is only a guard.
static void nodeAddSucc(GpsNode* n, int pt, bool fwd, int sign) {
	if (n->nSucc >= GPS_MAX_SUCC)
		return;
	n->succ[n->nSucc].pt = pt;
	n->succ[n->nSucc].fwd = fwd;
	n->succ[n->nSucc].sign = (s16)(sign < 0 || spAlways(sign) ? -1 : sign);
	n->nSucc++;
}

// FigureOutSuccessors (0x8003FCAC).
static void nodeFigureOutSuccessors(GpsNode* n) {
	n->nSucc = 0;
	if (n->cur < 0 || n->reached)
		return;
	const int ns = ptNumSigns(n->cur);
	for (int i = 0; i < ns; i++) {
		const int s = ptSign(n->cur, i);
		if (spApplies(s, n->fwd)) {
			if (spAlways(s)) {
				if (gn->signs[s].to >= 0)
					nodeAddSucc(n, gn->signs[s].to, n->fwd != spBit(s, 23), s);
				return;
			}
			if (!spIgnore(s) && !spDontTake(s) && gn->signs[s].to >= 0)
				nodeAddSucc(n, gn->signs[s].to, n->fwd != spBit(s, 23), s);
		} else if (spType(s) == SIGN_DEAD_END) {
			return;
		}
	}
	const int next = ptRelative(n->cur, n->fwd);
	if (next >= 0)
		nodeAddSucc(n, next, n->fwd, -1);
}

// Init (0x8003F3F4).
static void nodeInit(GpsNode* n, int pt, bool fwd, const float goal[3]) {
	memset(n, 0, sizeof(*n));
	n->start = pt;
	n->cur = pt;
	n->g = 0.0f;
	n->fwd = fwd;
	n->startFwd = fwd;
	n->reached = 0;
	n->nDec = 0;
	memcpy(n->goal, goal, sizeof(n->goal));
	nodeEstimate(n);
	nodeFigureOutSuccessors(n);
}

// RecordSignpostDecision (0x8003FF58).
static void nodeRecord(GpsNode* n, int sign, bool take) {
	if (n->nDec < 0x3F) {
		n->dec[(int)n->nDec] = (u16)((sign & 0x1FFF) | (take ? 0x2000 : 0));
		n->nDec++;
	}
}

// CloseEnoughToGoal (0x8003F504): the goal inside this point's road box, and
// within 4 m of its height (0x80318B3C).
static bool nodeCloseEnough(const GpsNode* n, int p) {
	if (ptOutLateral(p, n->goal[0], n->goal[1]) >= 0.0f)
		return false;
	if (ptOutAlong(p, n->goal[0], n->goal[1]) >= 0.0f)
		return false;
	return fabsf(ptZ(p) - n->goal[2]) < 4.0f;
}

// The cost of jumping from `from` to `to` across a taken signpost: the jump
// measured along the way the walk faced.
static float jumpCost(int from, int to, bool fwd) {
	float d[2];
	if (fwd)
		ptFwd(from, d);
	else
		ptBackDir(from, d);
	return (ptX(to) - ptX(from)) * d[0] + (ptY(to) - ptY(from)) * d[1];
}

// Traverse (0x8003F738): drive on from the node's point until the goal is in
// reach, a real choice comes up, the road ends or 10 km (0x80318B40) have gone
// by -- signposts the player must take are taken on the way.
static void nodeTraverse(GpsNode* n) {
	const float g0 = n->g;
	while (n->cur >= 0) {
		if (n->g - g0 > 10000.0f) {
			n->cur = -1;
			break;
		}
		if (nodeCloseEnough(n, n->cur)) {
			n->reached = 1;
			break;
		}
		if (!ptPosted(n->cur)) {
			n->cur = -1;
			break;
		}
		int taken = -1;
		bool choice = false;
		const int ns = ptNumSigns(n->cur);
		for (int i = 0; i < ns && taken < 0 && !choice; i++) {
			const int s = ptSign(n->cur, i);
			if (!spApplies(s, n->fwd))
				continue;
			if (spType(s) == SIGN_DEAD_END) {
				n->cur = -1;
				break;
			}
			if (spAlways(s))
				taken = s;
			else if (!spIgnore(s) && !spDontTake(s))
				choice = true;
		}
		if (n->cur < 0 || choice)
			break;
		if (taken < 0) {
			n->g += n->fwd ? ptLen(n->cur) : ptLenBack(n->cur);
			n->cur = ptRelative(n->cur, n->fwd);
		} else {
			const int to = gn->signs[taken].to;
			if (to < 0) {
				n->cur = -1;
				break;
			}
			n->g += jumpCost(n->cur, to, n->fwd);
			n->cur = to;
			n->fwd ^= spBit(taken, 23);
		}
	}
	nodeEstimate(n);
	nodeFigureOutSuccessors(n);
}

// GetSuccessor (0x8003FAAC): the node moved on to successor i, with every
// signpost before it recorded as not taken and its own (if any) as taken.
static bool nodeGetSuccessor(const GpsNode* n, int i, GpsNode* out) {
	if (i >= n->nSucc)
		return false;
	*out = *n;
	out->cur = n->succ[i].pt;
	out->fwd = n->succ[i].fwd;
	out->g += jumpCost(n->cur, out->cur, n->fwd);
	for (int j = 0; j < i; j++)
		nodeRecord(out, n->succ[j].sign, false);
	if (n->succ[i].sign != -1)
		nodeRecord(out, n->succ[i].sign, true);
	return true;
}

// GetClosestRoutePoint (0x80040094): walk the found path from its start the
// way it was found -- signposts taken or passed by its decisions -- and stop at
// the first point whose road box (5 m of slack, 0x80318B8C) holds the car.
// NULL when the walk reaches the goal's point, or the end of the road, first.
// `outDir` is the eTrackDirection the walk faces there (0 forwards).
static int nodeClosestRoutePoint(const GpsNode* n, const float pos[2], int* outDir) {
	int p = n->start;
	bool fwd = n->startFwd;
	int decIdx = 0;
	int found = -1;
	while (p >= 0) {
		if (ptOutLateral(p, n->goal[0], n->goal[1]) < 0.0f &&
			ptOutAlong(p, n->goal[0], n->goal[1]) < 0.0f)
			break;
		if (ptOutLateral(p, pos[0], pos[1]) < 5.0f && ptOutAlong(p, pos[0], pos[1]) < 5.0f) {
			found = p;
			break;
		}
		int taken = -1;
		bool plain = false;
		const int ns = ptNumSigns(p);
		for (int i = 0; i < ns && taken < 0; i++) {
			const int s = ptSign(p, i);
			if (!spApplies(s, fwd)) {
				if (spType(s) == SIGN_DEAD_END) {
					plain = true;
					break;
				}
				continue;
			}
			if (spIgnore(s) || spDontTake(s))
				continue;
			if (spAlways(s)) {
				taken = s;
			} else if (decIdx < n->nDec) {
				// Retail's DumpSignpostDecisions on a mismatch is a debug print;
				// the walk just goes on.
				if ((n->dec[decIdx] & 0x1FFF) == (u16)s) {
					if (n->dec[decIdx] & 0x2000)
						taken = s;
					decIdx++;
				}
			}
		}
		if (taken >= 0 && !plain && gn->signs[taken].to >= 0) {
			p = gn->signs[taken].to;
			fwd ^= spBit(taken, 23);
		} else {
			p = ptRelative(p, fwd);
		}
	}
	*outDir = fwd ? 0 : 1;
	return found;
}

// ---------------------------------------------------------------------------
// PathfindingManager
// ---------------------------------------------------------------------------
#define PM_OPEN_MAX   0x9F  // ProcessAStar appends only below this
#define PM_CLOSED_MAX 0x4FF // and closes only below this

// PathfindingNodeLite: what the closed list keeps of a node.
typedef struct {
	int cur;
	float g;
	u8 fwd;
} GpsLite;

enum { PM_IDLE = 0, PM_BUILD = 1, PM_ASTAR = 2, PM_DONE = 3 };

static struct {
	int state;       // +0x00
	int status;      // +0x04: 1 searching, 2 found, 3 no path, 4 nothing to start from
	int lastStatus;  // +0x08
	float pos[3];    // +0x0C
	float dir[2];    // +0x1C
	float goal[3];   // +0x24
	int routeIter;   // +0x34
	bool requested;  // +0x1790, the car that asked: the player, or none
	GpsNode pool[PM_OPEN_MAX + 1]; // retail's slot pool; one spare for the
	                               // successor being weighed
	GpsNode* open[PM_OPEN_MAX];    // +0x38
	int nOpen;                     // +0x16B8
	int initialOpen;               // +0x16C0
	GpsNode* freeList[PM_OPEN_MAX + 1];
	int nFree;
	GpsLite closed[PM_CLOSED_MAX]; // +0x2B8
	int nClosed;                   // +0x16BC
	GpsNode result;                // +0x16CC
	bool haveResult;               // +0x178C
} pm;

// RealController's GPS state (+0x3EC ..) and its GPS FuturePoints (+0x4B8),
// and the Player's arrow cache (+0x114). Forward-declared for the manager,
// which hands its result to the controller.
static void ctlSetState(int state, const GpsNode* node);

static GpsNode* pmAlloc(void) { return pm.nFree > 0 ? pm.freeList[--pm.nFree] : NULL; }
static void pmFree(GpsNode* n) { if (n) pm.freeList[pm.nFree++] = n; }

// ClearLists (0x8003E5B0).
static void pmClearLists(void) {
	pm.nFree = 0;
	for (int i = PM_OPEN_MAX; i >= 0; i--)
		pm.freeList[pm.nFree++] = &pm.pool[i];
	pm.nOpen = 0;
	pm.nClosed = 0;
}

// Init (0x8003E45C). status and lastStatus survive it.
static void pmInit(void) {
	pm.haveResult = false;
	pm.routeIter = -1;
	pm.state = PM_IDLE;
	pm.nOpen = 0;
	pm.nClosed = 0;
}

// SubmitRequest (0x8003E50C): taken only when idle.
static bool pmSubmit(const GpsCar* car, const float goal[3]) {
	if (pm.state != PM_IDLE)
		return false;
	memcpy(pm.pos, car->pos, sizeof(pm.pos));
	pm.dir[0] = car->fwd[0];
	pm.dir[1] = car->fwd[1];
	memcpy(pm.goal, goal, sizeof(pm.goal));
	pm.requested = true;
	pm.haveResult = false;
	pm.routeIter = 0;
	pm.status = 1;
	pm.state = PM_BUILD;
	pmClearLists();
	pm.initialOpen = 0;
	return true;
}

// CancelRequest (0x8003F350).
static void pmCancel(void) {
	if (pm.requested) {
		ctlSetState(0, NULL);
		pm.requested = false;
	}
	pmClearLists();
	pmInit();
	pm.status = 0;
	pm.lastStatus = 0;
}

// SortOpenList (0x8003EE44): retail's bubble sort, highest g+h first, so the
// cheapest node is the one popped off the end.
static void pmSort(void) {
	for (int pass = 1; pass <= pm.nOpen; pass++) {
		bool swapped = false;
		for (int i = 0; i + 1 < pm.nOpen; i++) {
			GpsNode* a = pm.open[i];
			GpsNode* b = pm.open[i + 1];
			if (a->g + a->h < b->g + b->h) {
				pm.open[i] = b;
				pm.open[i + 1] = a;
				swapped = true;
			}
		}
		if (!swapped)
			break;
	}
}

// The slice. Retail's bGetTickerDifference is milliseconds, and both loops
// stop once the elapsed time plus 0.4 ms (0x80318AD8 / 0x80318B0C) would pass
// 1 ms (0x80318ADC / 0x80318B10).
static float msSince(u64 t0) { return (float)(svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000.0f); }
static bool overBudget(u64 t0) { return msSince(t0) + 0.4f > 1.0f; }

// BuildInitialOpenList (0x8003E948): every route whose box holds the car is
// walked, sampling about one point every 50 m (0x80318B08), for the first
// posted point whose road box (5 m slack, 0x80318AF8) holds the car within 4 m
// of height (0x80318AFC); that point starts two nodes, one facing each way.
static void pmBuild(u64 t0) {
	for (;;) {
		if (pm.routeIter >= gn->nRoutes) {
			pmSort();
			pm.state = PM_ASTAR;
			pm.initialOpen = pm.nOpen;
			return;
		}
		if (overBudget(t0))
			return;
		const GpsRoute* r = &gn->routes[pm.routeIter];
		if (pm.pos[0] >= r->bbMin[0] && pm.pos[0] <= r->bbMax[0] &&
			pm.pos[1] >= r->bbMin[1] && pm.pos[1] <= r->bbMax[1]) {
			bool added = false;
			for (int i = 0; i < r->count && !added; ) {
				const int p = r->first + i;
				float d[2];
				ptFwd(p, d);
				const float facing = d[0] * pm.dir[0] + d[1] * pm.dir[1];
				if (fabsf(facing) > 0.0f && ptPosted(p)) {
					const float along = (pm.pos[0] - ptX(p)) * d[0] + (pm.pos[1] - ptY(p)) * d[1];
					const float lat = (pm.pos[0] - ptX(p)) * d[1] - (pm.pos[1] - ptY(p)) * d[0];
					// bBoundingBoxIsInside: [(0, -halfL), (len, halfR)] grown by 5.
					if (along >= -5.0f && along <= ptLen(p) + 5.0f &&
						lat >= -ptHalfL(p) - 5.0f && lat <= ptHalfR(p) + 5.0f &&
						fabsf(ptZ(p) - pm.pos[2]) < 4.0f) {
						added = true;
						for (int k = 0; k < 2; k++) {
							// PORT: retail pushes without a bound; the pool
							// holds PM_OPEN_MAX.
							if (pm.nOpen >= PM_OPEN_MAX)
								break;
							GpsNode* n = pmAlloc();
							if (!n)
								break;
							nodeInit(n, p, k == 0 ? facing > 0.0f : facing <= 0.0f, pm.goal);
							nodeTraverse(n);
							pm.open[pm.nOpen++] = n;
						}
					}
				}
				int step = (int)(hypotf(ptX(p) - pm.pos[0], ptY(p) - pm.pos[1]) / 50.0f);
				i += step < 1 ? 1 : step;
			}
		}
		pm.routeIter++;
	}
}

static bool nodeEqual(const GpsNode* a, const GpsNode* b) { return a->cur == b->cur && a->fwd == b->fwd; }

// ProcessAStar (0x8003EEDC).
static void pmAStar(u64 t0) {
	while (pm.nOpen > 0) {
		if (pm.haveResult)
			break;
		if (overBudget(t0))
			return;
		GpsNode* node = pm.open[--pm.nOpen];
		if (!node->reached) {
			for (int i = 0; i < node->nSucc; i++) {
				GpsNode* s = pmAlloc();
				if (!s)
					break;
				nodeGetSuccessor(node, i, s);
				nodeTraverse(s);
				bool discard = false;
				int openIdx = -1, closedIdx = -1;
				for (int k = 0; k < pm.nOpen && !discard; k++) {
					if (nodeEqual(s, pm.open[k])) {
						discard = pm.open[k]->g <= s->g;
						openIdx = k;
					}
				}
				for (int k = 0; k < pm.nClosed && !discard; k++) {
					const GpsLite* c = &pm.closed[k];
					if (c->cur == s->cur && c->fwd == s->fwd) {
						discard = c->g <= s->g;
						closedIdx = k;
					}
				}
				if (discard) {
					pmFree(s);
					continue;
				}
				if (closedIdx >= 0) {
					memmove(&pm.closed[closedIdx], &pm.closed[closedIdx + 1],
						sizeof(GpsLite) * (size_t)(pm.nClosed - closedIdx - 1));
					pm.nClosed--;
				}
				if (openIdx < 0) {
					if (pm.nOpen < PM_OPEN_MAX)
						pm.open[pm.nOpen++] = s;
					else
						pmFree(s);
				} else {
					*pm.open[openIdx] = *s;
					pmFree(s);
				}
				pmSort();
			}
			if (pm.nClosed < PM_CLOSED_MAX) {
				GpsLite* c = &pm.closed[pm.nClosed++];
				c->cur = node->cur;
				c->g = node->g;
				c->fwd = node->fwd;
			}
		} else if (!pm.haveResult) {
			// SubmitResult (0x8003E658): the first node to arrive wins.
			pm.result = *node;
			pm.haveResult = true;
		}
		pmFree(node);
	}
	pmClearLists();
	pm.state = PM_DONE;
}

// Service (0x8003E730): build, search, and hand the answer to the car, all
// against one tick taken on entry.
static void pmService(void) {
	const u64 t0 = svcGetSystemTick();
	if (pm.state == PM_BUILD)
		pmBuild(t0);
	if (pm.state == PM_ASTAR)
		pmAStar(t0);
	if (pm.state != PM_DONE)
		return;
	GpsNode node;
	if (pm.haveResult) {
		node = pm.result;
		pm.status = 2;
	} else {
		nodeInit(&node, -1, false, pm.goal);
		pm.status = pm.initialOpen < 1 ? 4 : 3;
	}
	pm.lastStatus = pm.status;
	if (pm.requested)
		ctlSetState(pm.status == 2 ? 1 : (pm.status == 3 || pm.status == 4) ? 3 : 0, &node);
	pmInit();
}

// ---------------------------------------------------------------------------
// FuturePoints, the GPS instance
// ---------------------------------------------------------------------------
//
// The player's GPS FuturePoints is the route ahead as up to 40 points, each a
// route point and a distance D down the path from the first one. The fields
// kept are the ones the GPS reads; the driving-line data a racer's
// FuturePoints also carries (aggregate fixups, line speeds, splines) is never
// read for the arrow, which asks for the centre line (line -1, L = 0).
#define FP_MAX 40

typedef struct {
	float D;        // +0x50
	float offset;   // +0x54: nonzero only on an interpolated point
	s16 route;      // +0x58, the route number
	s16 point;      // +0x5A, the point's index in it
	int sign;       // +0x5C, the signpost crossed to reach it, -1 none
	u8 dir;         // eTrackDirection, 0 forwards
	u8 interp;      // +0x70
} GpsFP;

static struct {
	GpsFP fp[FP_MAX];
	int count;
	int cached;     // +0xAC, the search start GetClosestPoint was last given
} fps;

static int fpPoint(const GpsFP* f) {
	if (f->route < 0 || f->point < 0 || f->route >= GPS_ROUTE_NUMS)
		return -1;
	const int r = gn->byNum[f->route];
	return r >= 0 && f->point < gn->routes[r].count ? gn->routes[r].first + f->point : -1;
}

// The direction of travel at a future point: the point's own forward
// direction, or its backward one when the path runs against the route.
static void fpDir(const GpsFP* f, int p, float d[2]) {
	if (f->dir == 0)
		ptFwd(p, d);
	else
		ptBackDir(p, d);
}

// GetPosition (0x80039F5C) with line -1: `L` metres to the right of the path
// and `D - f.D` (plus the point's offset) along it, the height following the
// road's cross and forward slopes.
static void fpPosition(const GpsFP* f, float D, float L, float out[3]) {
	const int p = fpPoint(f);
	if (p < 0) {
		out[0] = out[1] = out[2] = 0.0f;
		return;
	}
	const float d = f->offset + D - f->D;
	if (f->dir == 0) {
		float v[2];
		ptFwd(p, v);
		out[0] = ptX(p) + L * v[1] + d * v[0];
		out[1] = ptY(p) - L * v[0] + d * v[1];
		out[2] = ptZ(p) + L * (gn->pts[p].side / 128.0f) + d * (gn->pts[p].slope / 128.0f);
	} else {
		float b[2];
		ptBackDir(p, b);
		const int q = ptPrev(p) >= 0 ? ptPrev(p) : p;
		out[0] = ptX(p) + L * b[1] + d * b[0];
		out[1] = ptY(p) - L * b[0] + d * b[1];
		out[2] = ptZ(p) - L * (gn->pts[p].side / 128.0f) - d * (gn->pts[q].slope / 128.0f);
	}
}

// GetD (0x8003A6FC) and GetL (0x8003A3E0, line -1).
static float fpGetD(const GpsFP* f, const float pos[2]) {
	const int p = fpPoint(f);
	if (p < 0)
		return 0.0f;
	float d[2];
	fpDir(f, p, d);
	return (pos[0] - ptX(p)) * d[0] + (pos[1] - ptY(p)) * d[1] - f->offset + f->D;
}

static float fpGetL(const GpsFP* f, const float pos[2]) {
	const int p = fpPoint(f);
	if (p < 0)
		return 0.0f;
	float d[2];
	fpDir(f, p, d);
	return (pos[0] - ptX(p)) * d[1] - (pos[1] - ptY(p)) * d[0];
}

// GetUnadjustedAmountOutOfBounds (0x80039EE0): how far L lies outside the
// point's lateral bounds, negative inside.
// PORT: retail asks the point's virtual GetMinL/GetMaxL, which for a racer's
// line are filled in by FillInFuturePointMinAndMaxL from the driving-line
// data. The GPS keeps no line data, so the bounds are the road's own
// half-widths turned to the direction of travel.
static float fpOutOfBounds(const GpsFP* f, float L) {
	const int p = fpPoint(f);
	if (p < 0)
		return 99999.0f;
	const float right = f->dir == 0 ? ptHalfR(p) : ptHalfL(p);
	const float left = f->dir == 0 ? ptHalfL(p) : ptHalfR(p);
	return L > 0.0f ? L - right : -L - left;
}

static void fpClear(void) {
	fps.count = 0;
	fps.cached = -1;
}

// GetClosestPoint (0x80037C10), position form, no flags: the cheapest point
// by 2D distance plus ten times the height difference, skipping ahead about a
// point per 50 m (0x80318674) of distance.
static int fpClosest(const float pos[3], int start) {
	int best = -1;
	float bestCost = 0.0f;
	for (int i = start > 0 ? start : 0; i < fps.count; ) {
		const GpsFP* f = &fps.fp[i];
		float at[3];
		fpPosition(f, f->D, 0.0f, at);
		float dist = hypotf(pos[0] - at[0], pos[1] - at[1]);
		if (dist > 99999.0f)
			dist = 99999.0f;
		const int p = fpPoint(f);
		const float cost = fabsf((p >= 0 ? ptZ(p) : 0.0f) - pos[2]) * 10.0f + dist;
		if (cost < bestCost || best == -1) {
			best = i;
			bestCost = cost;
		}
		const int skip = (int)(dist / 50.0f) - 2;
		i += (skip > 0 ? skip : 0) + 1;
	}
	return best;
}

// GetClosestPoint (0x8003828C), distance form with flag 1: the nearest point
// at or past D.
static int fpClosestD(float D, int start) {
	int best = -1;
	float bestDiff = 99999.0f;
	for (int i = start; i < fps.count; i++) {
		if (fps.fp[i].D < D)
			continue;
		const float diff = fabsf(fps.fp[i].D - D);
		if (best == -1 || diff < bestDiff) {
			best = i;
			bestDiff = diff;
		}
	}
	return best;
}

// GetDL (0x80037EC4).
static int fpGetDL(const float pos[3], float* D, float* L) {
	*D = *L = 0.0f;
	if (fps.count < 1)
		return 0;
	const int i = fpClosest(pos, 0);
	if (i != -1) {
		*D = fpGetD(&fps.fp[i], pos);
		*L = fpGetL(&fps.fp[i], pos);
	}
	return i;
}

// GetWorldPosition (0x8003814C): the point D down the path, L to its right.
static bool fpWorldPosition(float D, float L, int start, float out[3]) {
	const int i = fpClosestD(D, start);
	if (i == -1)
		return false;
	if (i < 1) {
		fpPosition(&fps.fp[i], D, L, out);
		return true;
	}
	float a[3], b[3];
	fpPosition(&fps.fp[i], fps.fp[i].D, L, a);
	fpPosition(&fps.fp[i - 1], fps.fp[i - 1].D, L, b);
	const float span = fps.fp[i].D - fps.fp[i - 1].D;
	const float t = span != 0.0f ? (D - fps.fp[i - 1].D) / span : 0.0f;
	for (int k = 0; k < 3; k++)
		out[k] = b[k] + (a[k] - b[k]) * t;
	return true;
}

static bool fpAdd(float D, float offset, int route, int point, int sign, int dir) {
	if (fps.count >= FP_MAX)
		return false;
	GpsFP* f = &fps.fp[fps.count++];
	f->D = D;
	f->offset = offset;
	f->route = (s16)route;
	f->point = (s16)point;
	f->sign = sign;
	f->dir = (u8)dir;
	f->interp = 0;
	return true;
}

static int routeNumOf(int p) { return (int)(gn->pts[p].bits & 0x7FF); }

// ShiftLeft (0x80037858): drop the first n points and measure D from the new
// first one.
static void fpShiftLeft(int n) {
	if (n >= fps.count)
		return;
	const float base = fps.fp[n].D;
	memmove(&fps.fp[0], &fps.fp[n], sizeof(GpsFP) * (size_t)(fps.count - n));
	fps.count -= n;
	for (int i = 0; i < fps.count; i++)
		fps.fp[i].D -= base;
	fps.cached = fps.cached - n > 0 ? fps.cached - n : 0;
}

// RemoveInterpolatedPointsFromFront (0x800342C4) -- which, despite the name,
// trims interpolated points off the far end.
static void fpRemoveInterpolated(void) {
	while (fps.count > 0 && fabsf(fps.fp[fps.count - 1].offset) > 0.001f)
		fps.count--;
}

// GetSignpostDecisionFromPathfinding (0x80032D9C).
static GpsNode ctlPath;

static bool decisionFromPathfinding(int s) {
	const int t = spType(s);
	if (t == 2)
		return true;
	if (t == 7 || t == 5 || t == 10)
		return false;
	for (int i = 0; i < ctlPath.nDec; i++)
		if ((ctlPath.dec[i] & 0x1FFF) == (u16)s)
			return (ctlPath.dec[i] & 0x2000) != 0;
	return false;
}

// AdvancePoint (0x800339C4) for the player's own FuturePoints (RealController
// +0x68): a dead end stops the walk and a signpost the car must take is
// taken, as for any car. PORT: the other signposts are the player's
// DecideSignpost (RealController's DrivingChoice, from where the player has
// been steering), which the port does not have; it keeps to the route while
// the route goes on, and at its end takes the signpost that turns least.
static bool fpAdvancePlayer(int* p, bool* fwd, int* sign) {
	const int ns = ptNumSigns(*p);
	int best = -1;
	float bestDot = -2.0f, here[2];
	if (*fwd)
		ptFwd(*p, here);
	else
		ptBackDir(*p, here);
	for (int i = 0; i < ns; i++) {
		const int s = ptSign(*p, i);
		if (!spApplies(s, *fwd))
			continue;
		if (spType(s) == SIGN_DEAD_END) {
			*sign = -1;
			return false;
		}
		const int to = gn->signs[s].to;
		if (to < 0)
			continue;
		const bool toFwd = spBit(s, 23) != *fwd;
		if (spAlways(s)) {
			*p = to;
			*fwd = toFwd;
			*sign = s;
			return true;
		}
		float d[2];
		if (toFwd)
			ptFwd(to, d);
		else
			ptBackDir(to, d);
		const float dot = d[0] * here[0] + d[1] * here[1];
		if (dot > bestDot) {
			bestDot = dot;
			best = s;
		}
	}
	*sign = -1;
	const int next = ptRelative(*p, *fwd);
	if (next >= 0) {
		*p = next;
		return true;
	}
	if (best < 0)
		return false;
	*fwd = spBit(best, 23) != *fwd;
	*p = gn->signs[best].to;
	*sign = best;
	return true;
}

// AdvancePoint (0x800339C4), the GPS case: step to the next point down the
// path, across a signpost when the path takes it. `*fwd` is the walk's facing.
// Set while gps_pfp_build walks the player's own FuturePoints, whose
// signposts are the driver's choice and not the GPS path's.
static bool s_fpPlayer;

static bool fpAdvance(int* p, bool* fwd, int* sign) {
	const int ns = ptNumSigns(*p);
	if (s_fpPlayer)
		return fpAdvancePlayer(p, fwd, sign);
	for (int i = 0; i < ns; i++) {
		const int s = ptSign(*p, i);
		if (spType(s) == SIGN_DEAD_END && spApplies(s, *fwd)) {
			*sign = -1;
			return false;
		}
		if (!spApplies(s, *fwd))
			continue;
		// MustTakeSignpost (IsAlways), else SignPostDecision's GPS arm.
		const bool take = spAlways(s) || decisionFromPathfinding(s);
		if (take && gn->signs[s].to >= 0) {
			*p = gn->signs[s].to;
			*fwd = spBit(s, 23) != *fwd;
			*sign = s;
			return true;
		}
	}
	*sign = -1;
	const int next = ptRelative(*p, *fwd);
	if (next < 0)
		return false;
	*p = next;
	return true;
}

// FarEnoughFromPrevPoint (0x80034D98): a new point on a different route that
// is not at a signpost or a traffic intersection must be over 5 m
// (0x80318558) past the last one, or it is dropped.
static bool fpFarEnough(const GpsFP* nf) {
	if (fps.count < 1)
		return true;
	const GpsFP* last = &fps.fp[fps.count - 1];
	const int p = fpPoint(nf);
	if (p < 0)
		return true;
	if (gn->pts[p].sign[0] != 0xFFFF || ((gn->pts[p].bits >> 19) & 0x3F) != 0x3F)
		return true;
	if (nf->route == last->route)
		return true;
	float at[3];
	fpPosition(nf, nf->D, 0.0f, at);
	return fpGetD(last, at) - last->D > 5.0f;
}

// AddInterpolatedPoints (0x80034FC4): one point every 8 m (0x80318564) between
// the last point and a new one, each the last point carried straight on.
static void fpAddInterpolated(const GpsFP* nf) {
	if (fps.count < 1)
		return;
	const GpsFP last = fps.fp[fps.count - 1];
	const int n = (int)((nf->D - last.D) * 0.125f) - 1;
	float step = 0.0f;
	for (int k = 0; k < n && fps.count < FP_MAX; k++) {
		step += 8.0f;
		if (fpAdd(last.D + step, last.offset + step, last.route, last.point, -1, last.dir))
			fps.fp[fps.count - 1].interp = 1;
	}
}

// FillUpArray (0x800344F4): walk on from the last point until the array
// reaches `need` metres (and at least ten points), or is full.
static void fpFillUp(float need) {
	if (fps.count < 1)
		return;
	const GpsFP* last = &fps.fp[fps.count - 1];
	int p = fpPoint(last);
	if (p < 0)
		return;
	bool fwd = last->dir == 0;
	float lastD = last->D;
	float cornerD = 0.0f;
	for (int iter = 0; ; ) {
		if ((need < lastD && fps.count > 9 && cornerD <= lastD) || fps.count > 39)
			return;
		const int prev = p;
		const bool prevFwd = fwd;
		int sign;
		if (!fpAdvance(&p, &fwd, &sign))
			return;
		float inc;
		const bool corner = sign >= 0 && spBit(sign, 26);
		if (sign < 0) {
			if (fps.count < 1) {
				inc = prevFwd ? ptLen(prev) : ptLenBack(prev);
			} else {
				const GpsFP* l = &fps.fp[fps.count - 1];
				const float at[2] = { ptX(p), ptY(p) };
				inc = fpGetD(l, at) - l->D;
			}
		} else if (!corner) {
			// PORT: retail checks DAT_803FCA80 (TheRaceParameters +0) and a
			// second global here, and while either is set it trims the array
			// back instead; in free roam both stay clear, which is this arm:
			// keep advancing until the new point is 3 m (0x80318530) on.
			inc = jumpCost(prev, p, prevFwd);
			while (inc < 3.0f) {
				int dummy;
				if (!fpAdvance(&p, &fwd, &dummy))
					return;
				inc = jumpCost(prev, p, prevFwd);
				if (++iter > 100)
					return;
			}
		} else {
			inc = hypotf(ptX(prev) - ptX(p), ptY(prev) - ptY(p));
		}
		GpsFP nf;
		nf.D = lastD + inc;
		nf.offset = 0.0f;
		nf.route = (s16)routeNumOf(p);
		nf.point = (s16)ptIndex(p);
		nf.sign = sign;
		nf.dir = fwd ? 0 : 1;
		nf.interp = 0;
		if (!corner) {
			if (fpFarEnough(&nf)) {
				fpAddInterpolated(&nf);
				fpAdd(nf.D, nf.offset, nf.route, nf.point, nf.sign, nf.dir);
				lastD += inc;
			}
		} else {
			// PORT: retail rounds the corner with InterpolateFuturePointsFor-
			// Corner (0x80035230), which bends the last few points onto an
			// arc between the two routes' edges. The arrow only ever samples
			// the centre line 100 m and more ahead, so the corner is taken
			// as the straight cut its own fallback (LAB_800356C0) makes:
			// the new point is added as is.
			fpAdd(nf.D, nf.offset, nf.route, nf.point, nf.sign, nf.dir);
			lastD = nf.D;
			cornerD = nf.D;
		}
		if (++iter > 100)
			return;
	}
}

// Update (0x80033FD0), the GPS instance (+0xA8 set: no direction check).
static void fpUpdate(const GpsCar* car, int idx, int pt, int dir, float need) {
	if (pt < 0)
		return;
	float base = 0.0f;
	if (idx == -1)
		idx = fpClosest(car->pos, fps.cached);
	if (idx == -1) {
		fpClear();
	} else {
		fps.cached = idx;
		fpShiftLeft(idx);
		base = fps.count > 0 ? fps.fp[0].D : 0.0f;
	}
	fpRemoveInterpolated();
	if (fps.count == 0) {
		// BuildArray (0x80034430).
		fpAdd(0.0f, 0.0f, routeNumOf(pt), ptIndex(pt), -1, dir);
		fps.cached = 0;
		fpFillUp(need);
	} else {
		fpFillUp(base + need);
	}
	if (fps.count != 0) {
		if (fps.cached == -1)
			fps.cached = fpClosest(car->pos, 0);
		if (fps.cached < 0)
			fps.cached = 0;
		if (fps.cached > fps.count - 1)
			fps.cached = fps.count - 1;
	} else {
		fps.cached = -1;
	}
}

// ---------------------------------------------------------------------------
// The player's driving FuturePoints (gps.h, gps_pfp_build)
// ---------------------------------------------------------------------------

static struct {
	int count;
	GpsPfpPoint pt[FP_MAX];
} pfp;

int gps_pfp_build(int start, bool fwd, const float carPos[3], float need, int* closest) {
	pfp.count = 0;
	*closest = -1;
	if (!gn->loaded || start < 0 || start >= gn->nPts)
		return 0;
	// The walk is the GPS instance's own code, run on the array the GPS keeps
	// and then given back to it as it was.
	static u8 keep[sizeof(fps)];
	memcpy(keep, &fps, sizeof(fps));
	fpClear();
	// BuildArray (0x80034430): the point the car is on, then FillUpArray.
	fpAdd(0.0f, 0.0f, routeNumOf(start), ptIndex(start), -1, fwd ? 0 : 1);
	s_fpPlayer = true;
	fpFillUp(need);
	s_fpPlayer = false;
	for (int i = 0; i < fps.count; i++) {
		const GpsFP* f = &fps.fp[i];
		GpsPfpPoint* o = &pfp.pt[i];
		float at1[3];
		fpPosition(f, f->D, 0.0f, o->pos);
		fpPosition(f, f->D, 1.0f, at1);
		o->D = f->D;
		o->right[0] = at1[0] - o->pos[0];
		o->right[1] = at1[1] - o->pos[1];
		o->zPerL = at1[2] - o->pos[2];
		// GetUnadjustedMinL / MaxL, as fpOutOfBounds takes them: the road's
		// half-widths turned to the way the path runs.
		const int q = fpPoint(f);
		const float hl = q >= 0 ? ptHalfL(q) : 0.0f, hr = q >= 0 ? ptHalfR(q) : 0.0f;
		o->minL = -(f->dir == 0 ? hl : hr);
		o->maxL = f->dir == 0 ? hr : hl;
	}
	pfp.count = fps.count;
	*closest = fpClosest(carPos, 0);
	memcpy(&fps, keep, sizeof(fps));
	return pfp.count;
}

const GpsPfpPoint* gps_pfp_point(int i) {
	return i >= 0 && i < pfp.count ? &pfp.pt[i] : NULL;
}

void gps_pfp_position(int i, float L, float out[3]) {
	const GpsPfpPoint* o = gps_pfp_point(i);
	if (!o) {
		out[0] = out[1] = out[2] = 0.0f;
		return;
	}
	out[0] = o->pos[0] + L * o->right[0];
	out[1] = o->pos[1] + L * o->right[1];
	out[2] = o->pos[2] + L * o->zPerL;
}

// ---------------------------------------------------------------------------
// RealController's GPS
// ---------------------------------------------------------------------------

static struct {
	int state;   // +0x3EC, eGPSState: 0 off, 1 following, 3 no path
	int curPt;   // +0x3F0, the path point to rebuild from, -1 none
	bool curFwd; // +0x3F4
} ctl;

// SetGPSState (0x800329EC).
static void ctlSetState(int state, const GpsNode* node) {
	ctl.state = state;
	if (node)
		ctlPath = *node;
}

// CalculateGPSArrowLookaheadDistance (0x8003269C): 1.5 s of driving
// (0x80318488), never under 100 m (0x8031848C).
static float lookahead(const GpsCar* car) {
	const float d = car->speed * 1.5f;
	return d < 100.0f ? 100.0f : d;
}

// UpdateGPSFuturePoints (0x800326C4).
static void ctlUpdate(const GpsCar* car) {
	int idx = -1;
	if (ctl.state != 0) {
		// Arrival: within 30 m (0x803184A0) and 4 m of height (0x803184A4).
		const float dist = hypotf(car->pos[0] - ctlPath.goal[0], car->pos[1] - ctlPath.goal[1]);
		if (dist < 30.0f && fabsf(car->pos[2] - ctlPath.goal[2]) < 4.0f) {
			ctl.state = 0;
			pmCancel();
		}
	}
	if (ctl.state == 0) {
		ctl.curPt = -1;
		return;
	}
	if (ctl.state == 3) {
		ctl.curPt = -1;
	} else {
		bool onTrack = false;
		if (fps.count != 0) {
			float D, L;
			fpGetDL(car->pos, &D, &L);
			idx = fpClosest(car->pos, 0);
			if (fps.fp[0].D - 50.0f <= D && D <= fps.fp[fps.count - 1].D && idx >= 0 &&
				fpOutOfBounds(&fps.fp[idx], L) <= car->halfWidth)
				onTrack = true;
		}
		if (!onTrack) {
			int dir = 0;
			ctl.curPt = nodeClosestRoutePoint(&ctlPath, car->pos, &dir);
			idx = -1;
			ctl.curFwd = dir == 0;
			fpClear();
		}
	}
	if (ctl.curPt >= 0) {
		fpUpdate(car, idx, ctl.curPt, ctl.curFwd ? 0 : 1, lookahead(car) + 30.0f);
		ctl.state = 1;
		return;
	}
	// RecalculateGPSPath (0x80032AEC), taken only once the manager is idle.
	pmSubmit(car, ctlPath.goal);
	fpClear();
}

// ---------------------------------------------------------------------------
// GPSManager
// ---------------------------------------------------------------------------

static struct {
	float target[3]; // +0x00
	float icon[2];   // +0x10
	bool found;      // +0x18
	bool started;    // +0x1C
} mgr;

int gps_status(void) {
	if (!mgr.started)
		return GPS_STATUS_IDLE;
	int s;
	if (pm.status == 1) {
		s = GPS_STATUS_SEARCHING;
	} else if (pm.status == 2) {
		s = GPS_STATUS_ACTIVE;
		mgr.found = true;
	} else {
		s = GPS_STATUS_IDLE;
	}
	if (pm.lastStatus == 3)
		return GPS_STATUS_NO_PATH;
	if (pm.lastStatus == 4)
		return GPS_STATUS_LOST;
	return s;
}

void gps_stop(void) {
	pmCancel();
	memset(&mgr, 0, sizeof(mgr));
}

void gps_start(const GpsCar* car, const float target[3], const float icon[2]) {
	if (!gn->loaded)
		return;
	if (gps_status() != GPS_STATUS_IDLE)
		gps_stop();
	memcpy(mgr.target, target, sizeof(mgr.target));
	mgr.icon[0] = icon[0];
	mgr.icon[1] = icon[1];
	pmSubmit(car, mgr.target);
	mgr.started = true;
}

bool gps_is_target(const float target[3]) {
	if (gps_status() == GPS_STATUS_IDLE)
		return false;
	// bEqual with 0.001 (0x80329488).
	for (int k = 0; k < 3; k++)
		if (fabsf(mgr.target[k] - target[k]) > 0.001f)
			return false;
	return true;
}

bool gps_icon(float out[2]) {
	if (!mgr.started)
		return false;
	out[0] = mgr.icon[0];
	out[1] = mgr.icon[1];
	return true;
}

int gps_follow_state(void) {
	return ctl.state;
}

void gps_update(const GpsCar* car) {
	if (!gn->loaded)
		return;
	pmService();
	ctlUpdate(car);
}

// ---------------------------------------------------------------------------
// The arrow
// ---------------------------------------------------------------------------

static struct {
	bool has;       // +0x114
	float pos[3];   // +0x118
} arrow;

void gps_arrow_reset(void) { arrow.has = false; }

bool gps_arrow_target(const GpsCar* car, float out[3]) {
	if (!gn->loaded || !(ctl.state == 1 || ctl.state == 2)) {
		arrow.has = false;
		return false;
	}
	// WantToDisplayGPSArrow and a current path point: the FuturePoints.
	if (ctl.curPt >= 0 && fps.count != 0) {
		float D, L;
		const int idx = fpGetDL(car->pos, &D, &L);
		if (idx != -1 && fpWorldPosition(D + lookahead(car), 0.0f, idx, out)) {
			memcpy(arrow.pos, out, sizeof(arrow.pos));
			arrow.has = true;
			return true;
		}
	}
	if (!arrow.has)
		return false;
	memcpy(out, arrow.pos, sizeof(arrow.pos));
	return true;
}

static void vnorm(float v[3]) {
	const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 0.0f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

static void vcross(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

// RenderArrow (0x802181AC). The arrow hangs at half the half-FOV above the
// centre of the view, cos(half-FOV) x 10 m (0x80339CB8) out, and its -z points
// at the target with +y kept as close to world up as that allows.
void gps_arrow_matrix(const float eye[3], const float fwd[3], const float up[3],
	u16 fovU16, const float target[3], float rows[4][3]) {
	const float toRad = 6.28318531f / 65536.0f;
	const u16 half = (u16)(fovU16 >> 1);
	const float k = cosf(half * toRad) * 10.0f;
	const int a = (int)(half * 0.5f); // 0x80339CC8
	// Camera space is x right, y down, z forward (eCreateLookAtMatrix), and
	// the point is (0, sin(-a) k, cos(-a) k) -- above the middle of the view.
	const float cy = sinf(-a * toRad) * k;
	const float cz = cosf(-a * toRad) * k;
	float P[3], dir[3], right[3], upv[3];
	for (int i = 0; i < 3; i++)
		P[i] = eye[i] + fwd[i] * cz - up[i] * cy;
	for (int i = 0; i < 3; i++)
		dir[i] = P[i] - target[i];
	vnorm(dir);
	const float z[3] = { 0.0f, 0.0f, 1.0f };
	vcross(z, dir, right);
	vnorm(right);
	vcross(dir, right, upv);
	vnorm(upv);
	memcpy(rows[0], right, sizeof(right));
	memcpy(rows[1], upv, sizeof(upv));
	memcpy(rows[2], dir, sizeof(dir));
	memcpy(rows[3], P, sizeof(P));
}

void gps_debug(int* state, int* pmState, int* fpCount, int* pathDecisions) {
	*state = ctl.state;
	*pmState = pm.state;
	*fpCount = fps.count;
	*pathDecisions = ctlPath.nDec;
}

// ---------------------------------------------------------------------------
// The road network, for traffic (TASKS.md #316)
// ---------------------------------------------------------------------------

int gps_num_routes(void) { return gn->loaded ? gn->nRoutes : 0; }
int gps_num_points(void) { return gn->loaded ? gn->nPts : 0; }
int gps_route_first(int r) { return gn->routes[r].first; }
int gps_route_points(int r) { return gn->routes[r].count; }
void gps_route_bbox(int r, float mn[2], float mx[2]) {
	mn[0] = gn->routes[r].bbMin[0];
	mn[1] = gn->routes[r].bbMin[1];
	mx[0] = gn->routes[r].bbMax[0];
	mx[1] = gn->routes[r].bbMax[1];
}
int gps_point_route(int p) { return gn->ptRoute[p]; }

bool gps_route_has_line(int r, int line) {
	return line >= 0 && line < GPS_LINES && gn->routes[r].line[line] != 0;
}

bool gps_route_has_traffic(int r, bool fwd) {
	const int base = fwd ? GPS_LINE_TRAFFIC_FWD : GPS_LINE_TRAFFIC_BACK;
	for (int i = 0; i < GPS_LANES_PER_SIDE; i++)
		if (gn->routes[r].line[base + i])
			return true;
	return false;
}

void gps_point_pos(int p, float out[3]) {
	out[0] = ptX(p);
	out[1] = ptY(p);
	out[2] = ptZ(p);
}

void gps_point_fwd(int p, float d[2]) { ptFwd(p, d); }
void gps_point_back(int p, float d[2]) { ptBackDir(p, d); }
float gps_point_len(int p) { return ptLen(p); }
float gps_point_len_back(int p) { return ptLenBack(p); }
u32 gps_point_bits(int p) { return gn->pts[p].bits; }
float gps_point_race_dist(int p) { return gn->pts[p].raceDist; }
u32 gps_route_bits(int r) { return gn->routes[r].bits; }
int gps_point_next(int p, bool fwd) { return ptRelative(p, fwd); }

void gps_point_halves(int p, float* left, float* right) {
	*left = ptHalfL(p);
	*right = ptHalfR(p);
}

float gps_point_half(int p) {
	const float l = ptHalfL(p), r = ptHalfR(p);
	return l - r < 0.0f ? r : l;
}

float gps_lane_offset(int p, int line) {
	return gps_point_half(p) * gn->pts[p].line[line] * (1.0f / 128.0f);
}

float gps_line_offset(int p, int line) {
	const float off = gps_lane_offset(p, line);
	// IsForwardsTrafficLane (0x800188B4) or IsForwardsRacingLine (0x800188A8,
	// line 0 only): line 1 is the racing line the other way
	// (MakeDrivingLineNumberBackwards), which the free-roam racers drive
	// (encounter.c).
	if ((unsigned)(line - GPS_LINE_TRAFFIC_FWD) < GPS_LANES_PER_SIDE || line == 0)
		return off;
	// A backward line: GetBackwardPosition (0, off), measured back across the
	// forward direction.
	float b[2], f[2];
	ptBackDir(p, b);
	ptFwd(p, f);
	const float x = off * b[1], y = -off * b[0];
	return x * f[1] - y * f[0];
}

float gps_lane_speed(int p, int line) { return (float)gn->pts[p].speed[line]; }

bool gps_point_posted_traffic(int p, bool fwd) {
	return (gn->pts[p].bits >> (fwd ? 25 : 26)) & 1;
}

bool gps_point_posted_at_all(int p, bool fwd) {
	const u32 b = gn->pts[p].bits;
	return fwd ? ((b >> 11) | (b >> 25) | (b >> 27)) & 1u
	           : ((b >> 18) | (b >> 26) | (b >> 28)) & 1u;
}

int gps_num_signs(void) { return gn->loaded ? gn->nSigns : 0; }

int gps_point_num_signs(int p) { return ptNumSigns(p); }
int gps_point_sign(int p, int i) { return ptSign(p, i); }
u32 gps_sign_bits(int s) { return gn->signs[s].bits; }
int gps_sign_to(int s) { return gn->signs[s].to; }

int gps_num_intersections(void) { return gn->loaded ? gn->nInters : 0; }

int gps_point_intersection(int p) {
	const int i = (int)((gn->pts[p].bits >> 19) & 0x3Fu);
	if (i == GPS_INTER_NONE || i >= gn->nInters || !gn->inters[i].relevant)
		return -1;
	return i;
}

int gps_intersection_type(int i) { return gn->inters[i].type; }

int gps_intersection_side(int i, int r) {
	for (int k = 0; k < 2; k++)
		if (gn->inters[i].route[k] == r)
			return k;
	return -1;
}

float gps_intersection_length(int i, int side, bool fwd) {
	const GpsInter* in = &gn->inters[i];
	const int p = in->point[side], q = in->point[side == 0];
	float d[2], o[2];
	if (fwd) ptFwd(p, d);
	else     ptBackDir(p, d);
	ptFwd(q, o);
	// The other road's two edges, GetForwardPosition (0, -halfL) and
	// (0, halfR) at its point, along this route's way at this one.
	const float ex = ptX(q) - ptX(p), ey = ptY(q) - ptY(p);
	const float l = ptHalfL(q), r = ptHalfR(q);
	const float a = (ex - l * o[1]) * d[0] + (ey + l * o[0]) * d[1];
	const float b = (ex + r * o[1]) * d[0] + (ey - r * o[0]) * d[1];
	return -(a < b ? a : b);
}
