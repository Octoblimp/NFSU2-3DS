#include "traffic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "difficulty.h"
#include "gps.h"
#include "lightmat.h"
#include "rideinfo.h"
#include "trackroute.h"

// See traffic.h for what this is and which retail objects it ports.

#define TR_PI     3.14159265f
#define TR_TWO_PI 6.28318531f

// TrafficController / TrafficTeleporter figures (Speed.elf).
#define TR_MAX_SPEED       24.5865f  // +0x7AC, the mover's top speed and GetSpeedLimit
#define TR_THINK_SEC       0.2f      // Think's period (0x80317D54)
#define TR_TELEPORT_SEC    1.0f      // DoTimestep's think
#define TR_VISIBLE_M       250.0f    // CheckIsVisibleOnRaceTrack (0x80318234)
#define TR_TELEPORT_FROM_M 250.0f    // GuessTargetDestinations' distance (r13-0x7EBC)
#define TR_TELEPORT_FAR_M  500.0f    // TryToTeleportCarOntoRaceTrack gives up past it
#define TR_NO_TELEPORT     99999.0f  // GetMinDistanceBetweenCars: no teleport
#define TR_PLACE_TRIES     100       // PlaceNewCarOntoRaceTrack
#define TR_TELEPORT_STEPS  100       // TryToTeleportCarOntoRaceTrack
#define TR_STALL_PLAYER    30.0f     // TooManyStalledCarsNearThisCar: player under it
#define TR_STALL_CAR       1.0f      //   and a car under this
#define TR_HIT_STOP        30.0f     // CheckForMajorCollisions (0x80317D9C)
#define TR_SPEED_EPS       1e-5f     // CreateNewCar: no speed at or under it (0x80318124)

// The lane walker (PORT: FuturePoints' role, see traffic.h).
#define TR_WALK_AHEAD      100.0f    // metres of lane kept ahead of the car
#define TR_WALK_KEEP       2         // nodes kept behind the one the car is past
#define TR_CORNER_GAP_M    30.0f     // AdvancePoint: corners at least this far apart
#define TR_CORNER_SPEED    6.0f      // FinishInterpolatingCorners: traffic lanes' (0x803185A4)
#define TR_SCRUB_PER_M     0.2f      // ScrubSpeeds: traffic lanes, m/s a metre (0x803185F4)
#define TR_NO_CAP          99999.0f  // a node no corner caps

// The physics a traffic car runs while it is not on the TrafficMover, and how
// far from the player any of it runs (PORT: the world is only paged in round
// the player; a car out past this has no ground to drive on and is the
// teleporter's within a second or two anyway).
#define TR_PHYS_RANGE      220.0f
#define TR_CONTACT_RANGE   150.0f
#define TR_FALL_M          6.0f      // PORT: a mellow car this far under its lane fell through

// PORT: the step each car drives in. Retail's World::DoTimestep runs once a
// frame on the frame's real time (PrepareRealTimestep, at most
// MaxTicksPerTimestep 3/60 s), so on the PC at 60 fps a car's PsychoDriver and
// its mover step 1/60 s at a time. The traffic columns' fixed-weight steering
// (movement index 3 and 4) is a sampled controller, and it holds the lane at
// that step but not at the 3DS's ~1/30 s frame: there it weaves, the wheel
// flapping 7 to 10 degrees either way about once a second and the nose 2 to 5,
// on the TrafficMover and the MellowMover alike, every traffic car (an offline
// run of traffic.c's drive over the cars' own physics, TASKS.md #316). So the
// frame is cut into steps of at most this.
#define TR_STEP_SEC        (1.0f / 60.0f)

// Think -> ChooseNewAction puts a car on the MellowMover for an avoid, a hit
// and an unstuck (AIAction types 2, 8 and 1, 0x8002AC6C). Off holds an
// avoiding car on the TrafficMover, which brakes it all the same.
static const bool kAvoidMellow = true;

// TrafficMover's acceleration and braking, m/s^2, 4 samples over [0, 24.5865]
// (TrafficAcceleration / TrafficBraking).
static const float kAccel[4] = { 3.57622f, 2.68216f, 2.01162f, 1.34108f };
static const float kBrake[4] = { 6.70541f, 13.41082f, 20.11623f, 26.82164f };

// bStringHash.
static u32 strHash(const char* s) {
	u32 h = 0xFFFFFFFFu;
	for (; *s; s++) h = h * 33u + (u8)*s;
	return h;
}

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static float tableAt(const float t[4], float speed) {
	const float x = clampf(speed, 0.0f, TR_MAX_SPEED) * (3.0f / TR_MAX_SPEED);
	int i = (int)x;
	if (i >= 3) return t[3];
	return t[i] + (t[i + 1] - t[i]) * (x - (float)i);
}

// bRandom on the traffic's own state (PORT: retail draws from the World's).
static float randf(Traffic* t, float f) { return difficulty_random(&t->rng, f); }
static int randi(Traffic* t, int n) { return n > 0 ? (int)rideinfo_random(&t->rng, (u32)n) : 0; }

static float dist2d(const float a[2], const float b[2]) {
	const float dx = a[0] - b[0], dy = a[1] - b[1];
	return sqrtf(dx * dx + dy * dy);
}

static float carHalfL(const CarSim* s) {
	return s->chassis.length > 1.0f ? s->chassis.length * 0.5f : 2.2f;
}
static float carHalfW(const CarSim* s) {
	return s->chassis.width > 0.5f ? s->chassis.width * 0.5f : 0.9f;
}

// ---------------------------------------------------------------- lanes

static int laneLine(int lane, bool fwd) { return lane + (fwd ? 0 : GPS_LANES_PER_SIDE); }

static int nodeLane(const TrafficNode* nd) {
	return nd->fwd ? nd->line : nd->line - GPS_LANES_PER_SIDE;
}

static bool routeHasLane(int r, int lane, bool fwd) {
	return r >= 0 && lane >= 2 && lane <= 5 && gps_route_has_line(r, laneLine(lane, fwd));
}

// The lane's position at point `p` and the way it drives there.
static void lanePos(int p, int lane, bool fwd, float out[3], float dir[2]) {
	float d[2];
	if (fwd) gps_point_fwd(p, d);
	else     gps_point_back(p, d);
	gps_point_pos(p, out);
	const float off = gps_lane_offset(p, laneLine(lane, fwd));
	out[0] += off * d[1];
	out[1] -= off * d[0];
	if (dir) { dir[0] = d[0]; dir[1] = d[1]; }
}

// GetFurthestOutsideTrafficLane: the highest lane, 5 down to 2, route `r`
// has that way; -1 for none.
static int outsideLane(int r, bool fwd) {
	for (int lane = 5; lane >= 2; lane--)
		if (routeHasLane(r, lane, fwd)) return lane;
	return -1;
}

// ChooseNearestTrafficLane: the lane of point `p`'s route, that way, whose
// position is nearest `pos`; -1 for none.
static int nearestLane(int p, bool fwd, const float pos[2]) {
	const int r = gps_point_route(p);
	int best = -1;
	float bestD = 1e30f;
	for (int lane = 2; lane <= 5; lane++) {
		if (!routeHasLane(r, lane, fwd)) continue;
		float q[3];
		lanePos(p, lane, fwd, q, NULL);
		const float d = dist2d(q, pos);
		if (d < bestD) { bestD = d; best = lane; }
	}
	return best;
}

// MaybeSwitchTrafficLanes (0x8001D7A8): `lane` where point `p`'s route has
// it that way, else the nearest one it has within three, the higher first at
// each step; `lane` unchanged when there is none.
static int laneAt(int p, bool fwd, int lane) {
	const int r = gps_point_route(p);
	if (routeHasLane(r, lane, fwd)) return lane;
	for (int k = 1; k < 4; k++) {
		if (routeHasLane(r, lane + k, fwd)) return lane + k;
		if (routeHasLane(r, lane - k, fwd)) return lane - k;
	}
	return lane;
}

// ChooseRandomValidTrafficLane (0x8002D7A0): the driving line, 2..9, or -1.
static int randomLaneLine(Traffic* t, int p, bool fwd) {
	const int r = gps_point_route(p);
	for (int side = 0; side < 2; side++) {
		const bool f = side == 0 ? fwd : !fwd;
		const int base = f ? GPS_LINE_TRAFFIC_FWD : GPS_LINE_TRAFFIC_BACK;
		for (int k = 0; k < 10; k++) {
			const int line = randi(t, 400000) / 100000 + base;
			if (gps_route_has_line(r, line)) return line;
		}
		for (int i = 0; i < GPS_LANES_PER_SIDE; i++)
			if (gps_route_has_line(r, base + i)) return base + i;
	}
	return -1;
}

// DecideSignpostAsTraffic (0x8000A754) for a type-4 signpost: a coin, which
// at a corner (bit 26) only a car in its route's furthest outside lane, whose
// lane the new route has, may take.
static bool decideTurn(Traffic* t, int p, bool fwd, int lane, int sign) {
	const u32 bits = gps_sign_bits(sign);
	float chance = 0.5f;
	if (bits & (1u << 26)) {
		const int to = gps_sign_to(sign);
		const bool newFwd = (((bits >> 23) & 1u) != 0) != fwd;
		if (to < 0 || lane < 0 || !routeHasLane(gps_point_route(to), lane, newFwd) ||
				lane != outsideLane(gps_point_route(p), fwd))
			chance = 0.0f;
	}
	return randf(t, 1.0f) < chance;
}

// FuturePoints::AdvancePoint (0x800339C4) for a traffic car (gps.c's
// fpAdvance is the GPS's twin): the signposts at `*p` that apply to the way
// the car is going (bit 18 against the direction), a dead end (traffic type 3)
// ending the lane, the ones it must take (2 and 8) and the coin of type 4;
// else the next point. A corner (bit 26) the car need not take is only
// decided once the walk is 30 m (0x803184FC) past the last corner it took,
// `sinceCorner`. `*lane` follows the car onto a new route, the nearest lane
// where the new route lacks it; `*taken` gets the signpost taken, -1 for
// none. False at the lane's end.
static bool walkAdvance(Traffic* t, int* p, bool* fwd, int* lane, float sinceCorner,
		int* taken) {
	*taken = -1;
	const int n = gps_point_num_signs(*p);
	for (int i = 0; i < n; i++) {
		const int s = gps_point_sign(*p, i);
		const u32 bits = gps_sign_bits(s);
		const bool applies = ((bits >> 18) & 1u) != (u32)(*fwd ? 1 : 0);
		if (!applies) continue;
		const int type = (int)((bits >> 4) & 15u);
		if (type == 3) return false;
		bool take = type == 2 || type == 8;
		if (!take && type == 4 && (!((bits >> 26) & 1u) || sinceCorner > TR_CORNER_GAP_M))
			take = decideTurn(t, *p, *fwd, *lane, s);
		const int to = gps_sign_to(s);
		if (take && to >= 0) {
			float here[3];
			lanePos(*p, *lane, *fwd, here, NULL);
			*fwd = (((bits >> 23) & 1u) != 0) != *fwd;
			*p = to;
			*taken = s;
			if (!routeHasLane(gps_point_route(to), *lane, *fwd)) {
				*lane = nearestLane(to, *fwd, here);
				if (*lane < 0) return false;
			}
			return true;
		}
	}
	const int next = gps_point_next(*p, *fwd);
	if (next < 0) return false;
	*p = next;
	return true;
}

// How far point `to` is on from `from`, along the way a car facing `fwd`
// drives there: FillUpArray's measure of a signpost's jump (gps.c jumpCost).
static float jumpAhead(int from, bool fwd, int to) {
	float d[2], a[3], b[3];
	if (fwd) gps_point_fwd(from, d);
	else     gps_point_back(from, d);
	gps_point_pos(from, a);
	gps_point_pos(to, b);
	return (b[0] - a[0]) * d[0] + (b[1] - a[1]) * d[1];
}

// FarEnoughFromPrevPoint (0x80034D98): a node on another route, at a point
// with no signpost and no traffic intersection, must be over 5 m (0x80318558)
// on from the last one along its lane, or it is dropped.
static bool farEnough(const TrafficNode* last, const TrafficNode* nd) {
	if (gps_point_num_signs(nd->point) > 0 || ((gps_point_bits(nd->point) >> 19) & 0x3Fu) != 0x3Fu)
		return true;
	if (gps_point_route(nd->point) == gps_point_route(last->point))
		return true;
	float d[2];
	if (last->fwd) gps_point_fwd(last->point, d);
	else           gps_point_back(last->point, d);
	return (nd->pos[0] - last->pos[0]) * d[0] + (nd->pos[1] - last->pos[1]) * d[1] > 5.0f;
}

static void setNode(TrafficNode* nd, int p, bool fwd, int lane, float s) {
	nd->point = p;
	nd->fwd = fwd;
	nd->line = laneLine(lane, fwd);
	lanePos(p, lane, fwd, nd->pos, NULL);
	nd->s = s;
	const float v = gps_lane_speed(p, nd->line);
	nd->speed = v > TR_SPEED_EPS ? v : 0.0f;
	nd->localD = 0.0f;
	nd->cap = TR_NO_CAP;
}

static void travelDir(int p, bool fwd, float d[2]) {
	if (fwd) gps_point_fwd(p, d);
	else     gps_point_back(p, d);
}

static float pointLen(int p, bool fwd) {
	return fwd ? gps_point_len(p) : gps_point_len_back(p);
}

// The road's edges at point `p`, metres right of the way `fwd` drives:
// GetUnadjustedMinL / MaxL's +0x14 and +0x16, which a FuturePointBackwards
// reads the other way round.
static void roadEdges(int p, bool fwd, float* left, float* right) {
	float l, r;
	gps_point_halves(p, &l, &r);
	*left = fwd ? -l : -r;
	*right = fwd ? r : l;
}

// Node `nd` moved `localD` metres on from its point the way its lane runs
// (FuturePoint::GetPosition from +0x54), its height eased toward the next
// point's.
static void nodeOn(TrafficNode* nd, float localD) {
	nd->localD = localD;
	if (localD == 0.0f) return;
	float d[2];
	travelDir(nd->point, nd->fwd, d);
	nd->pos[0] += d[0] * localD;
	nd->pos[1] += d[1] * localD;
	const int nx = gps_point_next(nd->point, nd->fwd);
	const float len = pointLen(nd->point, nd->fwd);
	if (nx >= 0 && len > 0.1f) {
		float a[3], b[3];
		gps_point_pos(nd->point, a);
		gps_point_pos(nx, b);
		nd->pos[2] += (b[2] - a[2]) * clampf(localD / len, 0.0f, 1.0f);
	}
}

// ScrubSpeeds (0x80036CD8) on the traffic lanes: no node faster than the one
// before it plus 0.2 m/s a metre -- FillInDrivingLineSpeeds' pass over each
// new point -- and up to the last corner none faster than the one after it
// plus as much -- FinishInterpolatingCorners'. A corner's two nodes carry its
// caps.
static void scrubSpeeds(TrafficCar* c) {
	int lastCap = -1;
	for (int k = 0; k < c->nNodes; k++) {
		TrafficNode* nd = &c->node[k];
		if (nd->cap < TR_NO_CAP) {
			nd->speed = fminf(nd->speed, nd->cap);
			lastCap = k;
		}
	}
	for (int k = lastCap - 1; k >= 0; k--) {
		const TrafficNode* b = &c->node[k + 1];
		TrafficNode* a = &c->node[k];
		a->speed = fminf(a->speed, b->speed + TR_SCRUB_PER_M * (b->s - a->s));
	}
	for (int k = 1; k < c->nNodes; k++) {
		const TrafficNode* a = &c->node[k - 1];
		TrafficNode* b = &c->node[k];
		b->speed = fminf(b->speed, a->speed + TR_SCRUB_PER_M * (b->s - a->s));
	}
}

// InterpolateFuturePointsForCorner (0x80035230) on the one lane walked: the
// old lane is cut short of the new road's near edge, and the walk rejoins the
// new route 3 m (0x80318584) past the corner's apex. A lane's apex is the
// middle of its quarter circle round the inner edge -- sqrt(d^2 / 2) on and in
// from the edge, d its distance from it -- less 1.2 m (0x80318580); on a right
// turn onto a route with an outside traffic lane that lane's apex is taken,
// else the racing line's on a 5 m arc (sqrt 12.5, 0x8031856C). The cut is the
// nearest of them all. `*p`, `*fwd` and `*lane` come in at the new route's
// point and leave where the walk goes on from; `nd` is the node it rejoins
// at, `*cornerD` the near edge's metres. False is retail's fallback
// (LAB_800356C0): no old lane left short of the cut, the point taken as it is.
//
// PORT: GetFixup's per-line fixup is not applied. The cubic of Finish-
// InterpolatingCorners (InterpolateSplineDrivingLinesForCorner 0x800361A8)
// bends only the racing line, and its type-2 InterpolateDrivingLinesForCorner
// keeps nothing, so a traffic lane runs straight from the cut to the new
// point, as on the GameCube.
static bool cornerCut(Traffic* t, TrafficCar* c, int from, bool fromFwd, int* p, bool* fwd,
		int* lane, TrafficNode* nd, float* cornerD) {
	float od[2], nwd[2], p0[3], p1[3];
	travelDir(from, fromFwd, od);
	travelDir(*p, *fwd, nwd);
	gps_point_pos(from, p0);
	gps_point_pos(*p, p1);
	const float oRight[2] = { od[1], -od[0] };
	const float nRight[2] = { nwd[1], -nwd[0] };
	TrafficNode* last = &c->node[c->nNodes - 1];
	// D on the old route: the walk's metres, through the old point.
	const float s0 = last->s + (p0[0] - last->pos[0]) * od[0] + (p0[1] - last->pos[1]) * od[1];

	// The new road's nearer edge along the old route, and the turn's side:
	// right when the old road's right edge is the further along the new one.
	float ol, orr, nl, nr;
	roadEdges(from, fromFwd, &ol, &orr);
	roadEdges(*p, *fwd, &nl, &nr);
	float edgeD = 1e30f;
	for (int k = 0; k < 2; k++) {
		const float e = k == 0 ? nl : nr;
		const float x = p1[0] + nRight[0] * e - p0[0], y = p1[1] + nRight[1] * e - p0[1];
		edgeD = fminf(edgeD, s0 + x * od[0] + y * od[1]);
	}
	const bool right = oRight[0] * nwd[0] + oRight[1] * nwd[1] >= 0.0f;

	// Each traffic slot's apex, the slots the route lacks too (their offsets
	// read about the centre).
	const float rRace = sqrtf(12.5f);
	float r[4], rMin = rRace;
	for (int k = 0; k < 4; k++) {
		const float off = gps_lane_offset(from, laneLine(k + 2, fromFwd));
		const float d = (right ? orr : ol) - off;
		const float h = 0.5f * d * d;
		r[k] = (h > 5e-11f ? sqrtf(h) : 0.0f) - 1.2f;
		rMin = fminf(rMin, r[k]);
	}
	const float cut = edgeD + rMin;

	// The old lane's nodes from 3 m short of the cut go, and it runs on
	// straight in 3 m steps to within 6 m (0x80318588) of it.
	while (c->nNodes > c->cur + 2 && c->node[c->nNodes - 1].s > cut - 3.0f) c->nNodes--;
	last = &c->node[c->nNodes - 1];
	if (last->s > cut - 3.0f) return false;
	while (last->s < cut - 6.0f && c->nNodes < TRAFFIC_NODES - 1) {
		TrafficNode* e = &c->node[c->nNodes++];
		setNode(e, last->point, last->fwd, nodeLane(last), last->s + 3.0f);
		nodeOn(e, last->localD + 3.0f);
		last = e;
	}

	int k = -1;
	if (right) {
		const int o = outsideLane(gps_point_route(*p), *fwd);
		if (o >= 0) k = o - 2;
	}
	const float ra = k >= 0 ? r[k] : rRace;
	const float on = edgeD + ra - s0, across = right ? orr - ra : ol + ra;
	const float apex[2] = { p0[0] + od[0] * on + oRight[0] * across,
	                        p0[1] + od[1] * on + oRight[1] * across };

	// Down the new route (AdvancePoint) until the apex, seen along it, plus
	// 3 m is inside the point's length.
	float localD = 0.0f;
	for (int n = 0; ; n++) {
		float d[2], q[3];
		travelDir(*p, *fwd, d);
		gps_point_pos(*p, q);
		localD = (apex[0] - q[0]) * d[0] + (apex[1] - q[1]) * d[1] + 3.0f;
		if (localD <= pointLen(*p, *fwd) || n >= 100) break;
		int p2 = *p, lane2 = *lane, sign;
		bool fwd2 = *fwd;
		if (!walkAdvance(t, &p2, &fwd2, &lane2, 0.0f, &sign)) break;
		*p = p2;
		*fwd = fwd2;
		*lane = lane2;
	}
	setNode(nd, *p, *fwd, *lane, 0.0f);
	nodeOn(nd, localD);
	nd->s = last->s + dist2d(last->pos, nd->pos);
	*cornerD = edgeD;
	return true;
}

// Walks the car's lane on until it reaches TR_WALK_AHEAD past the car:
// FillUpArray (0x800344F4) with one lane instead of every driving line. A
// corner (a signpost with bit 26) is cut as InterpolateFuturePointsForCorner
// does (cornerCut), and FinishInterpolatingCorners caps the lane at 6 m/s
// there: the old lane's last node at 6 m/s plus 0.2 a metre short of the new
// road's edge, the new one at 6 plus 0.2 a metre past it; scrubSpeeds then
// ramps the nodes either side.
static void walkExtend(Traffic* t, TrafficCar* c) {
	// Drop the nodes the car is well past.
	if (c->cur > TR_WALK_KEEP) {
		const int drop = c->cur - TR_WALK_KEEP;
		memmove(&c->node[0], &c->node[drop], sizeof(TrafficNode) * (size_t)(c->nNodes - drop));
		c->nNodes -= drop;
		c->cur -= drop;
	}
	for (int iter = 0; !c->walkEnded && c->nNodes < TRAFFIC_NODES &&
			c->node[c->nNodes - 1].s - c->progress < TR_WALK_AHEAD; iter++) {
		const TrafficNode* last = &c->node[c->nNodes - 1];
		const int from = c->walkPoint;
		const bool fromFwd = c->walkFwd;
		int p = from, lane = c->lane, sign;
		bool fwd = fromFwd;
		if (iter > 100 || !walkAdvance(t, &p, &fwd, &lane, last->s - c->cornerS, &sign)) {
			c->walkEnded = true;
			break;
		}
		const bool corner = sign >= 0 && ((gps_sign_bits(sign) >> 26) & 1u);
		if (sign >= 0 && !corner) {
			// A lane change or a merge can land level with, or behind, where
			// the signpost is: outside a race FillUpArray walks on down the
			// new route until it is 3 m (0x80318530) on.
			int n = 0, dummy;
			while (!c->walkEnded && jumpAhead(from, fromFwd, p) < 3.0f)
				if (++n > 100 || !walkAdvance(t, &p, &fwd, &lane, last->s - c->cornerS, &dummy))
					c->walkEnded = true;
			if (c->walkEnded) break;
		}
		TrafficNode nd;
		float cornerD = 0.0f;
		const bool cut = corner && cornerCut(t, c, from, fromFwd, &p, &fwd, &lane, &nd, &cornerD);
		last = &c->node[c->nNodes - 1];
		c->walkPoint = p;
		c->walkFwd = fwd;
		c->lane = lane;
		if (!cut) {
			setNode(&nd, p, fwd, lane, 0.0f);
			nd.s = last->s + dist2d(last->pos, nd.pos);
		}
		if (corner) {
			c->cornerS = last->s;
			if (cut) t->cornersCut++;
			else     t->cornersStraight++;
			if (cut) {
				TrafficNode* old = &c->node[c->nNodes - 1];
				old->cap = fminf(old->cap, TR_CORNER_SPEED - TR_SCRUB_PER_M * (old->s - cornerD));
				nd.cap = TR_CORNER_SPEED + TR_SCRUB_PER_M * (nd.s - cornerD);
				t->cornerOld = old->s - cornerD;
				t->cornerNew = nd.s - cornerD;
				t->cornerOn = nd.localD;
			}
		} else if (!farEnough(last, &nd)) {
			continue;
		}
		c->node[c->nNodes++] = nd;
	}
	scrubSpeeds(c);
}

// The lane's position, heading and speed `s` metres along the walk.
static void walkSample(const TrafficCar* c, float s, float pos[3], float* speed) {
	int i = c->cur;
	while (i + 1 < c->nNodes - 1 && c->node[i + 1].s < s) i++;
	if (c->nNodes < 2) {
		memcpy(pos, c->node[0].pos, sizeof(float) * 3);
		if (speed) *speed = c->node[0].speed;
		return;
	}
	const TrafficNode* a = &c->node[i];
	const TrafficNode* b = &c->node[i + 1];
	const float len = b->s - a->s;
	const float u = len > 1e-4f ? clampf((s - a->s) / len, 0.0f, 1.0f) : 1.0f;
	for (int k = 0; k < 3; k++) pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * u;
	if (speed) *speed = a->speed + (b->speed - a->speed) * u;
}

// Projects the car onto its lane: `progress`, `cur`, the lane's height and
// direction under it and the car's offset off it, metres to the right.
static void walkTrack(TrafficCar* c, float dir[2], float* lateral) {
	const float q[2] = { c->x, c->y };
	dir[0] = cosf(c->heading);
	dir[1] = sinf(c->heading);
	*lateral = 0.0f;
	if (c->nNodes < 2) return;
	for (;;) {
		const TrafficNode* a = &c->node[c->cur];
		const TrafficNode* b = &c->node[c->cur + 1];
		const float ex = b->pos[0] - a->pos[0], ey = b->pos[1] - a->pos[1];
		const float len2 = ex * ex + ey * ey;
		float u = len2 > 1e-8f ? ((q[0] - a->pos[0]) * ex + (q[1] - a->pos[1]) * ey) / len2 : 1.0f;
		if (c->cur + 2 < c->nNodes) {
			bool past = u >= 1.0f;
			if (!past && len2 > 1e-8f) {
				// PORT (#401): a car inside a corner never gets past the far
				// node along its segment, so `progress` stalls; Think's
				// ChangeCurrentL then takes the growing L as the lane's and
				// the car orbits a target that no longer moves -- a donut at
				// 6 m/s round any sharp corner (tools/carsim_walltest.c
				// trafficcorner). Past the bisector at the node, it is on the
				// next segment.
				const TrafficNode* n2 = &c->node[c->cur + 2];
				const float fx = n2->pos[0] - b->pos[0], fy = n2->pos[1] - b->pos[1];
				const float m2 = fx * fx + fy * fy;
				if (m2 > 1e-8f) {
					const float il = 1.0f / sqrtf(len2), im = 1.0f / sqrtf(m2);
					const float bx = ex * il + fx * im, by = ey * il + fy * im;
					past = bx * bx + by * by > 0.01f &&
						(q[0] - b->pos[0]) * bx + (q[1] - b->pos[1]) * by >= 0.0f;
				}
			}
			if (past) {
				c->cur++;
				continue;
			}
		}
		u = clampf(u, 0.0f, 1.0f);
		c->progress = a->s + (b->s - a->s) * u;
		c->roadZ = a->pos[2] + (b->pos[2] - a->pos[2]) * u;
		if (len2 > 1e-8f) {
			const float l = sqrtf(len2);
			dir[0] = ex / l;
			dir[1] = ey / l;
			const float px = a->pos[0] + ex * u, py = a->pos[1] + ey * u;
			*lateral = (q[0] - px) * dir[1] - (q[1] - py) * dir[0];
		}
		return;
	}
}

// Whether the car has run off the end of a lane that ends.
static bool walkFinished(const TrafficCar* c) {
	if (!c->walkEnded || c->nNodes < 2) return false;
	return c->progress >= c->node[c->nNodes - 1].s - 0.5f;
}

// The lane, 2..5, the walk's node `nd` is in.
// FuturePoints +0xB8: how far along the walk lane `lane` runs unbroken from
// the car's node.
static float laneExtent(const TrafficCar* c, int lane) {
	float s = c->node[c->cur].s;
	for (int k = c->cur; k < c->nNodes; k++) {
		const TrafficNode* nd = &c->node[k];
		if (!routeHasLane(gps_point_route(nd->point), lane, nd->fwd)) break;
		s = nd->s;
	}
	return s;
}

// CanChangeDrivingLine (0x8003AF48) from traffic lane `from` to `to`: `to`
// is at the car's point (IsDrivingLineValid) and runs to the walk's end
// (DoesDrivingLineExtendAllTheWayForCar; a traffic car is control mode 4, so
// it is asked) or 25 m (0x803187C0) further than `from` does
// (DoesDrivingLineExtendAtLeastAsFar). InvalidLaneChange is the drag race's
// and is not asked outside one.
static bool canChangeLane(const TrafficCar* c, int from, int to) {
	if (to < 2 || to > 5 || c->nNodes < 2) return false;
	const TrafficNode* nd = &c->node[c->cur];
	if (!routeHasLane(gps_point_route(nd->point), to, nd->fwd)) return false;
	const float ext = laneExtent(c, to);
	return ext >= c->node[c->nNodes - 1].s || ext - laneExtent(c, from) >= 25.0f;
}

// SetChoice (0x8001CE04) to another lane. Retail's FuturePoints hold every
// driving line at once, so its choice just picks another; the port's walk
// holds one, so its points move to the new lane (laneAt where a route lacks
// it, as FillUpDriveTarget's MaybeSwitchTrafficLanes does) and the walk goes
// on in it. The drive target's L eases the car across (driveLAt).
static void changeLane(Traffic* t, TrafficCar* c, int lane) {
	for (int k = 0; k < c->nNodes; k++) {
		TrafficNode* nd = &c->node[k];
		const float s = k == 0 ? nd->s : c->node[k - 1].s;
		const float localD = nd->localD, cap = nd->cap;
		setNode(nd, nd->point, nd->fwd, laneAt(nd->point, nd->fwd, lane), s);
		nodeOn(nd, localD);
		nd->cap = cap;
		if (k > 0) nd->s = s + dist2d(c->node[k - 1].pos, nd->pos);
	}
	scrubSpeeds(c);
	c->lane = laneAt(c->walkPoint, c->walkFwd, lane);
	t->laneChanges++;
}

// The way the walk runs `s` metres along it.
static void walkDirAt(const TrafficCar* c, float s, float dir[2]) {
	int i = c->cur;
	while (i + 1 < c->nNodes - 1 && c->node[i + 1].s < s) i++;
	dir[0] = cosf(c->heading);
	dir[1] = sinf(c->heading);
	if (c->nNodes < 2) return;
	const float ex = c->node[i + 1].pos[0] - c->node[i].pos[0];
	const float ey = c->node[i + 1].pos[1] - c->node[i].pos[1];
	const float l = sqrtf(ex * ex + ey * ey);
	if (l > 1e-4f) {
		dir[0] = ex / l;
		dir[1] = ey / l;
	}
}

// ChangeCurrentL (0x8001DADC) down the drive target: from the car's L at the
// last Think, back toward the lane (SetChoice's L, 0) by 0.151 m (0x803172C4)
// a metre. PORT: the clamp to the road's edges (GetMinL / GetMaxL) is left
// out; the lanes are inside them.
static float driveLAt(const TrafficCar* c, float s) {
	const float m = fmaxf(0.0f, fabsf(c->driveL) - 0.151f * fmaxf(0.0f, s - c->driveS));
	return c->driveL < 0.0f ? -m : m;
}

// ---------------------------------------------------------------- placing

static void playerPos(const CarSim* player, float out[3]) {
	out[0] = player->rbPos[0];
	out[1] = player->rbPos[1];
	out[2] = player->rbPos[2];
}

// TheTrackPathManager +0x2C (0x803F4494): a race's path is loaded, so a
// distance round it means something. traffic_update has the race's frame in
// t->rc whenever this is true.
static bool racing(const Traffic* t) { return t->race; }

// pCurrentRace's state is RACE_STAGE_RACING. Free roam's race always is.
static bool raceOn(const Traffic* t) {
	return !t->race || (t->rc && t->rc->stage == RACE_STAGE_RACING);
}

// CompareDistances (0x8020309C): a - b round the race.
static float compareDist(const Traffic* t, float a, float b) {
	return race_route_compare(t->rc->route, a, b);
}

// DistanceAroundTrackIsValid (0x801F4E28): racing, the point's route one the
// race measures (not bit 9), and the point's own bit 29.
static bool distValid(const Traffic* t, int p) {
	return racing(t) && !((gps_route_bits(gps_point_route(p)) >> 9) & 1u) &&
		((gps_point_bits(p) >> 29) & 1u);
}

// GetBits(route, 1, 1) while racing: route `r` runs against the race, so its
// sides swap wherever traffic picks one by the race's way.
static bool againstRace(const Traffic* t, int r) {
	return racing(t) && ((gps_route_bits(r) >> 1) & 1u);
}

// WantTrafficCarGoingForwardsWRTRace (0x8002F4C8): Random(1) < 1 - TrackInfo
// +0x108[level].
static bool wantForwards(Traffic* t) { return randf(t, 1.0f) < 1.0f - t->oncoming; }

// TooCloseToAnotherCar(bVector2) (0x8002F844): within `minDist` of any car in
// the world but `self` -- the player, the race's AI cars, and each traffic car
// that is on the road and not stopped.
static bool tooClose(const Traffic* t, int self, const CarSim* player, const float pos[3],
		float minDist) {
	if (player) {
		float pp[3];
		playerPos(player, pp);
		if (dist2d(pp, pos) < minDist) return true;
	}
	if (t->rc)
		for (int k = 0; k < t->rc->nRacers; k++)
			if (dist2d(t->rc->racerPos[k], pos) < minDist) return true;
	for (int i = 0; i < t->count; i++) {
		const TrafficCar* o = &t->car[i];
		if (i == self || !o->active || o->stopped) continue;
		const float op[2] = { o->x, o->y };
		if (dist2d(op, pos) < minDist) return true;
	}
	return false;
}

// TooCloseToAnotherCar(float) (0x8002F9AC): the same round the race, from
// distance `d`. A traffic car is too close either way; the player or a racer
// only when `d` is ahead of it, by under `minDist`.
static bool tooCloseDist(const Traffic* t, int self, float d, float minDist) {
	const TrafficRace* rc = t->rc;
	float x = compareDist(t, d, rc->playerDist);
	if (x < minDist && x > 0.0f) return true;
	for (int k = 0; k < rc->nRacers; k++) {
		x = compareDist(t, d, rc->racerDist[k]);
		if (x < minDist && x > 0.0f) return true;
	}
	for (int i = 0; i < t->count; i++) {
		const TrafficCar* o = &t->car[i];
		if (i == self || !o->active || o->stopped) continue;
		if (fabsf(compareDist(t, d, o->coord.dist)) < minDist) return true;
	}
	return false;
}

// CheckIsVisibleOnRaceTrack(bVector2) (0x8002F3E4): the race on and the
// player under 250 m away.
static bool visibleAt(const Traffic* t, const CarSim* player, const float pos[3]) {
	if (!player || !raceOn(t)) return false;
	float pp[3];
	playerPos(player, pp);
	return dist2d(pp, pos) < TR_VISIBLE_M;
}

// CheckIsVisibleOnRaceTrack(float) (0x8002F434): the race on and the player
// under 250 m away round it (GetDATToClosestPlayer 0x80211418).
static bool visibleDist(const Traffic* t, float d) {
	return raceOn(t) && fabsf(compareDist(t, t->rc->playerDist, d)) < TR_TELEPORT_FROM_M;
}

// CheckIsVisibleOnRaceTrack(Car*) (0x8002F348): by the car's distance round
// the race while racing, else where it is.
static bool visibleCar(const Traffic* t, const TrafficCar* c, const CarSim* player) {
	if (racing(t)) return visibleDist(t, c->coord.dist);
	const float cp[3] = { c->x, c->y, c->roadZ };
	return visibleAt(t, player, cp);
}

// CanPlaceCarHere (0x8002F130) at route point `p`. Before a race starts no
// car goes behind the start line or within the track's +0xF8 metres past it
// (unless its +0xF4 allows); then the point must be out of the player's sight
// and `minDist` from every car -- round the race where the point has a race
// distance, else in a straight line.
static bool canPlace(const Traffic* t, int self, const CarSim* player, int p, float minDist) {
	if (!t->startOk && racing(t) && !t->rc->started &&
			compareDist(t, gps_point_race_dist(p), t->rc->startDist) < t->startClear)
		return false;
	if (distValid(t, p)) {
		const float d = gps_point_race_dist(p);
		return !visibleDist(t, d) && !tooCloseDist(t, self, d, minDist);
	}
	float pos[3];
	gps_point_pos(p, pos);
	return !visibleAt(t, player, pos) && !tooClose(t, self, player, pos, minDist);
}

// CreateNewCar (0x8002D4C4) / TeleportCar (0x8002EE7C): the car on `line` at
// point `p`, facing the lane's way, rolling at the lane's speed, on the
// TrafficMover.
static void putCar(Traffic* t, int i, int p, int line) {
	TrafficCar* c = &t->car[i];
	const bool fwd = line < GPS_LINE_TRAFFIC_BACK;
	const int lane = fwd ? line : line - GPS_LANES_PER_SIDE;
	float pos[3], dir[2];
	lanePos(p, lane, fwd, pos, dir);

	c->lane = lane;
	c->nNodes = 1;
	c->cur = 0;
	c->walkEnded = false;
	c->walkPoint = p;
	c->walkFwd = fwd;
	c->cornerS = 0.0f;
	c->progress = 0.0f;
	c->driveL = 0.0f;
	c->driveS = 0.0f;
	setNode(&c->node[0], p, fwd, lane, 0.0f);
	walkExtend(t, c);

	c->x = pos[0];
	c->y = pos[1];
	c->heading = atan2f(dir[1], dir[0]);
	c->speed = c->node[0].speed;
	c->roadZ = pos[2];
	c->desired = c->speed;
	c->active = true;
	c->stopped = false;
	c->stopDriving = false;
	c->stopWhy = 0;
	c->mellow = false;
	c->hitSum = 0.0f;
	c->hitLast = 99.0f;
	c->action = TRAFFIC_ACT_LANE;
	c->avoidWho = -1;
	c->avoidLook = 30.0f;
	c->avoidTime = 0.0f;
	c->obeyInter = -1;
	c->obeyStopped = false;
	c->firstInter = -1;

	if (racing(t) && t->rc)
		race_coord_init(t->rc->route, &c->coord, pos);

	carsim_set_mellow(&c->sim, false, 0.0f, 0.0f);
	carsim_place(&c->sim, NULL, pos, c->heading);
	carsim_traffic_pose(&c->sim, NULL, c->x, c->y, c->roadZ, c->heading, c->speed, 0.0f);
	aidriver_zero_memory(&c->drv);
	c->drv.trafficMover = true;
	c->drv.dirState = 1;
	c->drv.prevThrottle = 0.0f;
	c->drv.prevBrake = 0.0f;
}

static void disableCar(TrafficCar* c) {
	c->active = false;
	c->stopped = true;
}

// GenerateRandomPoint: uniform over the points of every route with traffic
// lanes on the side `fwd` says.
static int randomPoint(Traffic* t, bool fwd) {
	const int* cum = fwd ? t->cumFwd : t->cumBack;
	const int total = fwd ? t->totalFwd : t->totalBack;
	if (!cum || total <= 0) return -1;
	const int k = randi(t, total);
	int lo = 0, hi = t->nRoutes - 1;
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		if (cum[mid + 1] <= k) lo = mid + 1;
		else hi = mid;
	}
	return gps_route_first(lo) + (k - cum[lo]);
}

// PlaceNewCarOntoRaceTrack (0x8002D0B0): a point posted for traffic both
// ways and not its route's last, where CanPlaceCarHere allows at the track's
// spacing, then a lane the way the car is to go -- the race's way, on a route
// that runs against the race.
static void placeNewCar(Traffic* t, int i, const CarSim* player) {
	for (int tries = 0; tries < TR_PLACE_TRIES; tries++) {
		const bool fwd = wantForwards(t);
		const int p = randomPoint(t, fwd);
		if (p < 0) break;
		if (!gps_point_posted_traffic(p, true) || !gps_point_posted_traffic(p, false)) continue;
		if (gps_point_next(p, true) < 0) continue;
		if (!canPlace(t, i, player, p, t->initialSpacing)) continue;
		const int line = randomLaneLine(t, p, fwd != againstRace(t, gps_point_route(p)));
		if (line < 0) continue;
		putCar(t, i, p, line);
		return;
	}
	disableCar(&t->car[i]);
}

// TooManyStalledCarsNearThisCar (0x80030040): a slow player and a traffic car
// all but stopped within 250 m of it round the race. PORT: free roam has no
// race distance, so there it is straight-line. Retail's is map-wide there:
// PathsFreeRoam.bin has no 0x34148 path points, so UpdatePosition (0x8020281C)
// finds none and every car's Car +0x8EC is 0 (TASKS.md #341).
static bool tooManyStalled(const Traffic* t, const CarSim* player) {
	if (!player || player->speed >= TR_STALL_PLAYER) return false;
	float pp[3];
	playerPos(player, pp);
	for (int i = 0; i < t->count; i++) {
		const TrafficCar* c = &t->car[i];
		if (!c->active || c->stopped || c->sim.speed >= TR_STALL_CAR) continue;
		const float cp[2] = { c->x, c->y };
		const float d = racing(t) ? fabsf(compareDist(t, t->rc->playerDist, c->coord.dist))
			: dist2d(cp, pp);
		if (d < TR_TELEPORT_FROM_M) return true;
	}
	return false;
}

// Why a teleport failed, for the log: the player outside every traffic zone,
// a stalled car near a slow player, no route point near the player, the walk
// at a lane's end, past 500 m of the player, out of steps, or (racing) a spot
// further from the player than the car already is.
enum { TF_NO_ZONE, TF_STALLED, TF_NO_POINT, TF_DEAD_END, TF_TOO_FAR, TF_NO_ROOM, TF_FURTHER };

// GetMinDistanceBetweenCars (0x8002FC10): the spacing the player's traffic
// zone asks for at the traffic level (the zone's +0x30 + (level - 1) * 4), or
// TR_NO_TELEPORT and why.
static float minDistance(Traffic* t, const CarSim* player, int* why) {
	*why = TF_NO_ZONE;
	if (!t->paths || !player) return TR_NO_TELEPORT;
	const TrackPathZone* z = trackpath_find(t->paths, player->rbPos[0], player->rbPos[1],
		TPZ_TRAFFIC_TYPE, NULL);
	if (!z) return TR_NO_TELEPORT;
	*why = TF_STALLED;
	if (tooManyStalled(t, player)) return TR_NO_TELEPORT;
	float lo, hi;
	difficulty_traffic_spacing((int)z->unk30[t->level - 1], &lo, &hi);
	return randf(t, hi - lo) + lo;
}

// The nearest route point to the player and whether the player is going the
// route's way there.
static int playerPoint(const CarSim* player, bool* fwd) {
	TrackRouteSource src = gps_route_source();
	TrackRouteQuery q;
	memset(&q, 0, sizeof(q));
	playerPos(player, q.pos);
	TrackRoutePick pick;
	if (!trackroute_nearest(&src, &q, &pick)) return -1;
	float d[2];
	gps_point_fwd(pick.point, d);
	float v[2] = { player->vel[0], player->vel[1] };
	if (v[0] * v[0] + v[1] * v[1] < 1.0f) {
		v[0] = player->rot[0][0];
		v[1] = player->rot[0][1];
	}
	*fwd = v[0] * d[0] + v[1] * d[1] >= 0.0f;
	return pick.point;
}

// GuessTargetDestinations (0x800174E8) as the teleporter calls it: one
// target, for traffic. From point `*p` going `*fwd`, `metres` on: each point
// takes its length off what is left (going back, the start point's own is
// added first), and the first point to take it under 0 (DAT_80316FE4) is the
// target, written back to `*p` / `*fwd`.
//
// At a point, its signposts that apply to the way the walk is going (bit 18
// against it), in order:
// - sign type 3, a dead end: what is left becomes -1 (DAT_80316FE8), so the
//   point itself is the target;
// - with the point posted for traffic -- forward, whichever the way, as
//   retail reads it -- traffic sign type 7 is passed by and type 2 taken: the
//   walk jumps to the signpost's point less its along-distance (the route's
//   own forward direction) and turns round on bit 23;
// - any other is a junction: the straight on goes in at the front of a
//   20-entry ring deque, and the turn at the front on a coin under 0.5
//   (DAT_80316FF0; racing, when its sign type is 0) or else at the back. The
//   walk stops there, after the point's other signposts have had their go.
// With none of them in the way the walk takes the route's next point. A walk
// also stops at a route's end and on a point not posted at all the way the
// search started (cr4, set once from the argument: a jump that turns the walk
// round does not change it). A stopped walk goes on from the deque's front;
// an empty deque is a failure. Caps: numRoutes << 10 steps (TheTrackRoute-
// Manager +0x2EF8) and NumSignposts pops (r13-0x62B8).
//
// PORT: a signpost whose point does not resolve (gps_sign_to -1) is a turn
// that cannot be taken: a forced one stops the walk, a junction keeps only
// its straight on. Retail's data has none.
#define TR_GUESS_DEQUE 20

typedef struct {
	int p;
	float left;
	bool fwd;
} GuessStep;

// How far a signpost's point `to` is along route point `p`'s own forward
// direction: what GuessTargetDestinations takes off for the jump.
static float guessAlong(int p, int to) {
	float d[2], a[3], b[3];
	gps_point_fwd(p, d);
	gps_point_pos(p, a);
	gps_point_pos(to, b);
	return (b[0] - a[0]) * d[0] + (b[1] - a[1]) * d[1];
}

static bool guessTargetRng(u32* rng, bool race, int* p, bool* fwd, float metres) {
	if (*p < 0) return false;
	GuessStep dq[TR_GUESS_DEQUE];
	int head = 0, tail = 0;
	bool full = false;
	const bool startFwd = *fwd;
	int cur = *p;
	bool dir = *fwd;
	float left = metres;
	if (!dir) left += gps_point_len(cur);
	const int maxSteps = gps_num_routes() << 10, maxPops = gps_num_signs();
	int steps = 0;
	for (int pops = 0; pops <= maxPops; pops++) {
		while (cur >= 0) {
			if (++steps > maxSteps) return false;
			left -= gps_point_len(cur);
			if (left < 0.0f) {
				*p = cur;
				*fwd = dir;
				return true;
			}
			bool next = true, branched = false;
			const int n = gps_point_num_signs(cur);
			for (int i = 0; i < n; i++) {
				const int s = gps_point_sign(cur, i);
				const u32 bits = gps_sign_bits(s);
				if (((bits >> 18) & 1u) == (u32)(dir ? 1 : 0)) continue;
				next = false;
				if ((bits & 15u) == 3) {
					left = -1.0f;
					break;
				}
				const bool posted = gps_point_posted_traffic(cur, true);
				const u32 type = (bits >> 4) & 15u;
				if (posted && type == 7) {
					next = true;
					continue;
				}
				const int to = gps_sign_to(s);
				const bool turnDir = (((bits >> 23) & 1u) != 0) != dir;
				if (posted && type == 2) {
					if (to < 0) {
						branched = true;
						break;
					}
					left += gps_point_len(cur) - guessAlong(cur, to);
					cur = to;
					dir = turnDir;
					break;
				}
				const int straight = gps_point_next(cur, dir);
				if (straight >= 0 && !full) {
					head = head == 0 ? TR_GUESS_DEQUE - 1 : head - 1;
					dq[head] = (GuessStep){ straight, left, dir };
					full = tail == head;
				}
				const bool front = race ? (bits & 15u) == 0 : difficulty_random(rng, 1.0f) < 0.5f;
				if (to >= 0 && !full) {
					const GuessStep turn = { to, left + gps_point_len(cur) - guessAlong(cur, to),
						turnDir };
					if (front) {
						head = head == 0 ? TR_GUESS_DEQUE - 1 : head - 1;
						dq[head] = turn;
					} else {
						dq[tail] = turn;
						tail = (tail + 1) % TR_GUESS_DEQUE;
					}
					full = tail == head;
				}
				branched = true;
			}
			if (branched) cur = -1;
			if (next && cur >= 0) cur = gps_point_next(cur, dir);
			if (cur >= 0 && !gps_point_posted_at_all(cur, startFwd)) cur = -1;
		}
		if (!full && head == tail) return false;
		cur = dq[head].p;
		left = dq[head].left;
		dir = dq[head].fwd;
		head = (head + 1) % TR_GUESS_DEQUE;
		full = false;
	}
	return false;
}

static bool guessTarget(Traffic* t, int* p, bool* fwd, float metres) {
	return guessTargetRng(&t->rng, racing(t), p, fwd, metres);
}

bool traffic_guess_target(u32* rng, bool racing, int* p, bool* fwd, float metres) {
	return guessTargetRng(rng, racing, p, fwd, metres);
}

int traffic_route_point(const CarSim* car, bool* fwd) {
	return car ? playerPoint(car, fwd) : -1;
}

// GetAlternateStartingPoints (0x8002E390): the player's point `p`, walked the
// way `fwd` says, and up to two more on other routes alongside -- the first
// point of each route whose box, 30 m (0x803181B8) past the road's wider half
// all round, holds `p`, that runs within 0.85 (0x803181C8) of along or against
// it, is posted for traffic the way it would be walked, and lies 2 to 30 m
// (0x803181BC) out past either edge and no further along or back than the
// point's length or 20 m (0x803181B4). A route's points are tried a
// distance / 50 m (0x803181D8) apart. Each is walked the player's way, or
// the other where its route runs against the player's.
static int alternateStarts(int p, bool fwd, int pts[3], bool fwds[3]) {
	pts[0] = p;
	fwds[0] = fwd;
	int n = 1;
	float pp[3], d[2], hl, hr;
	gps_point_pos(p, pp);
	gps_point_fwd(p, d);
	gps_point_halves(p, &hl, &hr);
	const float along = fmaxf(20.0f, gps_point_len(p));
	const float margin = fmaxf(hl, hr) + 30.0f;
	const int self = gps_point_route(p);
	for (int r = 0; r < gps_num_routes() && n < 3; r++) {
		float mn[2], mx[2];
		gps_route_bbox(r, mn, mx);
		if (r == self || pp[0] < mn[0] - margin || pp[0] > mx[0] + margin ||
				pp[1] < mn[1] - margin || pp[1] > mx[1] + margin)
			continue;
		const int first = gps_route_first(r), count = gps_route_points(r);
		for (int k = 0; k < count; ) {
			const int q = first + k;
			float qp[3], qd[2];
			gps_point_pos(q, qp);
			gps_point_fwd(q, qd);
			const float dx = qp[0] - pp[0], dy = qp[1] - pp[1];
			const float dot = qd[0] * d[0] + qd[1] * d[1];
			const bool f = dot >= 0.0f ? fwd : !fwd;
			if (fabsf(dot) > 0.85f && gps_point_posted_traffic(q, f)) {
				const float x = dx * d[0] + dy * d[1], y = dx * d[1] - dy * d[0];
				if (fabsf(x) <= along && ((y >= -hl - 30.0f && y <= -hl - 2.0f) ||
						(y >= hr + 2.0f && y <= hr + 30.0f))) {
					pts[n] = q;
					fwds[n] = f;
					n++;
					break;
				}
			}
			const int step = (int)(sqrtf(dx * dx + dy * dy) / 50.0f);
			k += step < 1 ? 1 : step;
		}
	}
	return n;
}

// TryToTeleportCarOntoRaceTrack (0x8002E9BC): from the player's point or one
// of its alternates, in turn (the counter at +0xC), 250 m on down the road and
// then point by point to 500 m.
//
// Racing, the walk goes the race's way, or back down it when the player is
// going the wrong way at speed or, below it, on a coin; and a car still on the
// road is never sent further from the player round the race than it is.
// PORT: in free roam the walk starts the way the player is driving rather
// than down the route (retail's direction 1 there).
static bool teleportOntoTrack(Traffic* t, int i, const CarSim* player, float minDist, int* why) {
	TrafficCar* const c = &t->car[i];
	const bool wantFwd = wantForwards(t);
	const float carRel = racing(t) ? compareDist(t, c->coord.dist, t->rc->playerDist) : 0.0f;
	bool walkFwd;
	int p = playerPoint(player, &walkFwd);
	*why = TF_NO_POINT;
	if (p < 0) return false;
	bool ahead = true;
	if (player->speed >= TR_MAX_SPEED) {
		if (racing(t) && t->rc->playerWrongWay) ahead = false;
	} else if (randf(t, 1.0f) < 0.5f) {
		ahead = false;
	}
	if (racing(t))
		walkFwd = ahead != againstRace(t, gps_point_route(p));
	else if (!ahead)
		walkFwd = !walkFwd;
	int pts[3];
	bool fwds[3];
	const int n = alternateStarts(p, walkFwd, pts, fwds);
	t->altStart = (t->altStart + 1) % n;
	p = pts[t->altStart];
	walkFwd = fwds[t->altStart];
	t->altStarts += t->altStart > 0;
	*why = TF_DEAD_END;
	if (!guessTarget(t, &p, &walkFwd, TR_TELEPORT_FROM_M)) return false;
	float pp[3];
	playerPos(player, pp);
	for (int step = 0; step < TR_TELEPORT_STEPS; step++) {
		float q[3];
		gps_point_pos(p, q);
		const float rel = distValid(t, p)
			? compareDist(t, gps_point_race_dist(p), t->rc->playerDist) : dist2d(q, pp);
		*why = TF_TOO_FAR;
		if (rel > TR_TELEPORT_FAR_M) return false;
		const int line = randomLaneLine(t, p, wantFwd == walkFwd);
		if (line >= 0 && gps_point_posted_traffic(p, wantFwd) &&
				canPlace(t, i, player, p, minDist)) {
			*why = TF_FURTHER;
			if (racing(t) && !c->stopped &&
					(ahead ? carRel > 0.0f && carRel < rel : carRel < 0.0f && rel < carRel))
				return false;
			putCar(t, i, p, line);
			return true;
		}
		// The next step: the spacing, or the point's own length if longer. A
		// search that fails or ends on the same point (a dead end there) is a
		// failure: retail's doubling loop never gets a second go.
		const float len = gps_point_len(p);
		const int was = p;
		*why = TF_DEAD_END;
		if (!guessTarget(t, &p, &walkFwd, minDist > len ? minDist : len) || p == was)
			return false;
	}
	*why = TF_NO_ROOM;
	return false;
}

// ---------------------------------------------------------------- the drag

// FindClosestPointAheadAtLeastThisFarAway (0x801F88D4) on route `r`: the
// point whose distance away from `dist` -- GetDistanceAway: round the race
// (0x801F8160), or with `pos` the larger of the straight line to it and the
// race gap less 50 m (0x801F7FB4) -- passes `ahead` by the least, of those
// ahead of `dist` round the race. The scan skips (excess / 50) - 2 points
// past one that is far over. -1 for none.
static int routePointAhead(const Traffic* t, int r, float dist, float ahead, const float* pos) {
	const int first = gps_route_first(r), n = gps_route_points(r);
	int best = -1;
	float bestOver = 999999.0f;
	for (int k = 0; k < n; k++) {
		const int p = first + k;
		const float rel = compareDist(t, gps_point_race_dist(p), dist);
		float away = rel;
		if (pos) {
			float q[3];
			gps_point_pos(p, q);
			const float straight = dist2d(q, pos), round = fabsf(rel) - 50.0f;
			away = straight < round ? round : straight;
		}
		const float over = away - ahead;
		if (over >= 0.0f && over < bestOver && rel > 0.0f) {
			best = p;
			bestOver = over;
		}
		const int skip = (int)(over / 50.0f) - 2;
		if (skip > 0) k += skip;
	}
	return best;
}

// GetClosestPointToThisPosition (0x8002DCF8): over every route with traffic
// lanes the way `fwd` asks (flipped on a route against the race), its point
// above, if posted for traffic `fwd` and not at the route's end that way
// (IsEnoughRoomLeftOnRoute 0x8002DE94); of those, the one nearest ahead of
// `dist` round the race. -1 for none.
static int closestPointAhead(const Traffic* t, float dist, float ahead, const float* pos, bool fwd) {
	int best = -1;
	float bestRel = 999999.0f;
	for (int r = 0; r < gps_num_routes(); r++) {
		const bool f = fwd != againstRace(t, r);
		if (!gps_route_has_traffic(r, f)) continue;
		const int p = routePointAhead(t, r, dist, ahead, pos);
		if (p < 0 || !gps_point_posted_traffic(p, fwd)) continue;
		const int idx = p - gps_route_first(r);
		if (f ? idx >= gps_route_points(r) - 1 : idx <= 0) continue;
		const float rel = compareDist(t, gps_point_race_dist(p), dist);
		if (rel > 0.0f && rel < bestRel) {
			best = p;
			bestRel = rel;
		}
	}
	return best;
}

// PlaceNewCarOntoDragTrack (0x8002D2C8), PlaceNewCar's first choice in a
// drag (TheRaceParameters +0x40, g_tweakIsDragRace): car `i` (DriverInfo id
// 50 + i, AddTrafficCars') goes down the strip from the start, up to 100
// tries, the n-th at (n + i) x the track's +0x100 spacing on, at a point
// CanPlaceCarHere allows at that spacing, in a lane the way it is to go.
// PORT: PlaceNewCar falls back on PlaceNewCrossTrafficCar, not ported.
static bool placeNewCarDrag(Traffic* t, int i, const CarSim* player) {
	if (!racing(t)) return false;
	const float spacing = t->initialSpacing;
	for (int n = 0; n < TR_PLACE_TRIES; n++) {
		const bool fwd = wantForwards(t);
		const int p = closestPointAhead(t, 0.0f, (float)(n + i) * spacing, NULL, fwd);
		if (p < 0) continue;
		const int line = randomLaneLine(t, p, fwd != againstRace(t, gps_point_route(p)));
		if (line < 0) continue;
		if (!canPlace(t, i, player, p, spacing)) continue;
		putCar(t, i, p, line);
		return true;
	}
	return false;
}

// TryToTeleportCarOntoDragTrack (0x8002DB3C): the nearest point 250 m
// (r13 - 0x7EBC) or more ahead of the player round the strip -- by the
// larger of the straight line and the race gap less 50 m -- in a lane the
// wanted way, if CanPlaceCarHere allows at the zone's spacing. One go.
static bool teleportOntoDrag(Traffic* t, int i, const CarSim* player, float minDist, int* why) {
	*why = TF_NO_POINT;
	if (!racing(t)) return false;
	const bool fwd = wantForwards(t);
	float pp[3];
	playerPos(player, pp);
	const int p = closestPointAhead(t, t->rc->playerDist, TR_TELEPORT_FROM_M, pp, fwd);
	if (p < 0) return false;
	*why = TF_NO_ROOM;
	const int line = randomLaneLine(t, p, fwd != againstRace(t, gps_point_route(p)));
	if (line < 0 || !canPlace(t, i, player, p, minDist)) return false;
	putCar(t, i, p, line);
	return true;
}

// DbgTrafficBehind, a diagnostic and not retail: car 0 put down `dbgBehindM`
// metres back down the road from the player, in the player's lane, so that it
// comes up on the player -- the avoid's lane change on demand. Negative puts
// it that far up the road instead, standing on its brakes (StopDriving), for
// the player to drive into: the crash camera's wild hit (main.c, #335).
// False when there is no lane to put it in yet; only the first miss is logged.
static bool dbgPlaceBehind(Traffic* t, const CarSim* player, void (*log)(const char*)) {
	// Ahead, it waits for the player to be going fast enough for the hit to
	// be a crash: wildCollision's impulse of 30 (carsim.c) comes to about 30
	// m/s of closing speed between two cars.
	if (t->dbgBehindM < 0.0f && hypotf(player->vel[0], player->vel[1]) < 33.0f)
		return false;
	// The nearest lane that runs the way the player faces: a two-way street
	// is often two one-way routes, so the nearest point alone may be the
	// other side's.
	float pos[3];
	playerPos(player, pos);
	const float hx = player->rot[0][0], hy = player->rot[0][1];
	int pp = -1, lane = -1;
	bool fwd = true;
	float bestD = 30.0f;
	for (int q = 0; q < gps_num_points(); q++) {
		float d[2];
		gps_point_fwd(q, d);
		const float along = d[0] * hx + d[1] * hy;
		if (fabsf(along) < 0.9f) continue;
		const bool f = along > 0.0f;
		const int l = nearestLane(q, f, pos);
		if (l < 0) continue;
		float lp[3];
		lanePos(q, l, f, lp, NULL);
		const float dd = dist2d(lp, pos);
		if (dd < bestD) { bestD = dd; pp = q; lane = l; fwd = f; }
	}
	char b[160];
	const bool quiet = t->dbgTries++ > 0;
	if (!t->car[0].used || pp < 0) {
		snprintf(b, sizeof(b), "traffic: DbgTrafficBehind -- no lane near the player "
			"(car 0 %s, heading %.2f,%.2f), trying again each second",
			t->car[0].used ? "used" : "unused", hx, hy);
		if (log && !quiet) log(b);
		return false;
	}
	// Back down the player's own route (or on up it), point by point: no
	// signposts, which walkOn would take the way a car leaving the player
	// would.
	const bool ahead = t->dbgBehindM < 0.0f;
	const float want = fabsf(t->dbgBehindM);
	int p = pp;
	float done = 0.0f;
	while (done < want) {
		const int prev = gps_point_next(p, ahead ? fwd : !fwd);
		if (prev < 0) break;
		float a[3], c[3];
		gps_point_pos(p, a);
		gps_point_pos(prev, c);
		done += dist2d(a, c);
		p = prev;
	}
	if (done < want * 0.5f) {
		snprintf(b, sizeof(b), "traffic: DbgTrafficBehind -- route %d ends %.0f m %s "
			"the player", gps_point_route(pp), done, ahead ? "ahead of" : "behind");
		if (log && !quiet) log(b);
		return false;
	}
	const int l = laneAt(p, fwd, lane);
	putCar(t, 0, p, laneLine(l, fwd));
	if (ahead) {
		// Square on the line the player is driving, not the lane's middle,
		// so a car held straight runs into it.
		TrafficCar* c = &t->car[0];
		const float vl = hypotf(player->vel[0], player->vel[1]);
		if (vl > 5.0f) {
			c->x = pos[0] + player->vel[0] / vl * done;
			c->y = pos[1] + player->vel[1] / vl * done;
		}
		c->speed = c->desired = 0.0f;
		c->stopDriving = true;
		c->stopWhy = 3;
		carsim_traffic_pose(&c->sim, NULL, c->x, c->y, c->roadZ, c->heading, 0.0f, 0.0f);
	}
	snprintf(b, sizeof(b), "traffic: DbgTrafficBehind -- car 0 in lane %d at (%.1f, %.1f), "
		"%.0f m %s, the player in lane %d", l, t->car[0].x, t->car[0].y, done,
		ahead ? "ahead, parked" : "behind", lane);
	if (log) log(b);
	return true;
}

// TrafficTeleporter::DoTimestep's think (0x8002C8B0): round robin from the
// last car, ONE attempt at a car it may teleport (CanWeTeleportThisCar
// 0x8002CB6C: stopped, or out of the player's sight).
static void teleporterThink(Traffic* t, const CarSim* player) {
	for (int n = 0; n < t->count; n++) {
		const int i = (t->lastCar + 1 + n) % t->count;
		TrafficCar* c = &t->car[i];
		if (!c->used || (!c->stopped && c->active && visibleCar(t, c, player))) continue;
		t->lastCar = i;
		int why;
		const float minDist = minDistance(t, player, &why);
		bool ok = false;
		// TryToTeleportCar (0x8002D9D0): the drag strip's own in a drag,
		// else round the race. PORT: cross traffic first in retail; not
		// ported.
		if (minDist != TR_NO_TELEPORT)
			ok = t->drag ? teleportOntoDrag(t, i, player, minDist, &why)
				: teleportOntoTrack(t, i, player, minDist, &why);
		if (ok) {
			t->teleports++;
		} else {
			t->teleportFails++;
			t->failWhy[why]++;
			disableCar(c);
		}
		return;
	}
}

// ---------------------------------------------------------------- Think

// IsSomeoneAheadInThisLane (0x8000EC44) over the port's stand-in for the
// NearbyAvoidableList (Build 0x8022F998): the player's car and every other
// traffic car on the road within `range`, measured along the lane's direction
// at the car and across it. The nearest one ahead and in the lane -- across
// it by no more than the two half-widths and 0.5 m -- that is closing on the
// car (the gap over the closing speed), or within `look` metres, or within
// 20 m of a car that is all but stopped. Returns who (TRAFFIC_MAX for the
// player, -1 for nobody), with its time and gap. The lane is centred `shift`
// metres right of the car: 0 for where the car is (retail's L, the car's
// own), a lane's offset for TryLane's (L 0 on that lane).
static int someoneAhead(const Traffic* t, int self, const CarSim* player, const float dir[2],
		float range, float look, float shift, float* outTime, float* outGap, float* outRelV) {
	const TrafficCar* c = &t->car[self];
	const CarSim* me = &c->sim;
	const float myV = me->vel[0] * dir[0] + me->vel[1] * dir[1];
	int best = -1;
	float bestD = 1e30f;
	*outTime = TR_NO_TELEPORT;
	*outGap = 0.0f;
	if (outRelV) *outRelV = 0.0f;
	for (int k = 0; k <= t->count; k++) {
		const CarSim* b;
		if (k == t->count) {
			b = player;
		} else {
			if (k == self || !t->car[k].active) continue;
			b = &t->car[k].sim;
		}
		if (!b) continue;
		const float dx = b->rbPos[0] - me->rbPos[0], dy = b->rbPos[1] - me->rbPos[1];
		if (dx * dx + dy * dy > range * range) continue;
		if (fabsf(b->rbPos[2] - me->rbPos[2]) > 4.0f) continue;
		const float d = dx * dir[0] + dy * dir[1];
		const float right = dx * dir[1] - dy * dir[0];
		if (d < 0.0f) continue;
		if (fabsf(right - shift) > carHalfW(me) + carHalfW(b) + 0.5f) continue;
		float gap = d - carHalfL(b) - carHalfL(me);
		if (gap < 0.0f) gap = 0.0f;
		const float relV = b->vel[0] * dir[0] + b->vel[1] * dir[1] - myV;
		float time = relV < -0.01f ? gap / fabsf(relV) : TR_NO_TELEPORT;
		if (gap <= look || (gap <= 20.0f && me->speed < 0.1f)) time = 0.0f;
		if (time >= TR_NO_TELEPORT || d >= bestD) continue;
		bestD = d;
		best = k == t->count ? TRAFFIC_MAX : k;
		*outTime = time;
		*outGap = gap;
		if (outRelV) *outRelV = relV;
	}
	return best;
}

// SetDesiredSpeedAsTraffic (0x8000B2D8).
static float desiredSpeed(const TrafficCar* c) {
	if (c->stopped || c->stopDriving) return 0.0f;
	const float v = c->sim.speed;
	float maxV = TR_MAX_SPEED;
	// The steepest drop in slope over the next max(20, 1.3 v) m.
	const float window = fmaxf(20.0f, 1.3f * v);
	float worst = 0.0f, worstAt = 0.0f;
	for (int i = c->cur; i + 2 < c->nNodes; i++) {
		const TrafficNode* a = &c->node[i];
		const TrafficNode* b = &c->node[i + 1];
		const TrafficNode* n = &c->node[i + 2];
		const float at = b->s - c->progress;
		if (at > window) break;
		if (at < 0.0f) continue;
		const float l0 = b->s - a->s, l1 = n->s - b->s;
		if (l0 < 0.5f || l1 < 0.5f) continue;
		const float change = (n->pos[2] - b->pos[2]) / l1 - (b->pos[2] - a->pos[2]) / l0;
		if (change < worst) { worst = change; worstAt = at; }
	}
	if (worst < -0.1f) {
		// TrafficSpeedDistanceToJumpSpeedReduction, {(10, 1), (40, 0)}.
		const float g = clampf((40.0f - worstAt) / 30.0f, 0.0f, 1.0f);
		maxV = (maxV - 10.0f) * (1.0f - g) + 10.0f;
	}
	float here, ahead;
	float pos[3];
	walkSample(c, c->progress, pos, &here);
	walkSample(c, c->progress + fmaxf(8.0f, 1.3f * v), pos, &ahead);
	return fminf(maxV, fminf(here, ahead));
}

// ---------------------------------------------------------------- intersections
//
// TrafficIntersection (0x8002B930 on): where two routes cross, which one has
// the right of way (the baton) and the traffic coming down each. Every one of
// the PC's 4636, free roam's 38 among them, is type 1: the first route goes,
// the second gives way to it.

#define TR_STOP_ZONE_M   12.0f     // GetLengthOfStoppingZone (0x80317E70)
#define TR_GIVE_WAY_ETA  3.0f      // AllowedToProceed: the first route's soonest car (0x80317E74)
#define TR_ETA_NONE      99999.0f  // GetSoonestIncomingCarETA with nobody coming (0x80317E7C)
#define TR_OBEY_LOOK     30.0f     // Obey's CalculatePointsToDrive (0x80316C0C)

// RecomputeSoonestIncomingCar (0x8002C058).
static void interRecompute(TrafficInter* in, int side) {
	in->soonest[side] = -1;
	float best = 999999.0f; // 0x80317E84
	for (int k = 0; k < TRAFFIC_MAX; k++) {
		if (in->car[k].in && in->car[k].side == side && in->car[k].eta < best) {
			in->soonest[side] = (s8)k;
			best = in->car[k].eta;
		}
	}
}

// GetSoonestIncomingCarETA (0x8002C028).
static float interSoonestEta(const TrafficInter* in, int side) {
	return in->soonest[side] < 0 ? TR_ETA_NONE : in->car[in->soonest[side]].eta;
}

// RemoveMeFromAllTrafficIntersections (0x80008C10): RemoveCarFromList
// (0x8002BFC8) at each.
static void interRemoveAll(Traffic* t, int car) {
	for (int x = 0; x < t->nInter; x++) {
		TrafficInter* in = &t->inter[x];
		in->car[car].in = false;
		for (int k = 0; k < 2; k++) {
			if (in->soonest[k] == car) {
				interRecompute(in, k);
				break;
			}
		}
	}
}

// AllowedToProceed (0x8002BC20) for types 1 and 2: the route with the baton
// goes and the other waits; with no baton the first goes, and the second
// only if nothing on the first is under 3 s off.
static bool interAllowed(const TrafficInter* in, int type, int side) {
	if (type < 1 || type > 2) return false;
	if (in->baton == side) return true;
	if (in->baton == (side == 0 ? 1 : 0)) return false;
	return side == 0 || interSoonestEta(in, 0) >= TR_GIVE_WAY_ETA;
}

// MustStopBeforeProceeding (0x8002BD1C).
static bool interMustStop(int type, int side) {
	return type == 3 || (type == 2 && side == 1);
}

// SetBaton (0x8002BCBC): taken unless the other route holds it.
static void interSetBaton(TrafficInter* in, int side) {
	if (in->baton != (side == 0 ? 1 : 0)) in->baton = (s8)side;
}

// MaybeReleaseBaton (0x8002C0CC), every frame for each (UpdateTraffic-
// Intersections 0x8002FD48): the baton goes once nothing on its route is
// within the stopping zone.
static void interRelease(TrafficInter* in) {
	if (in->baton < 0) return;
	bool near[2] = { false, false };
	for (int k = 0; k < TRAFFIC_MAX; k++)
		if (in->car[k].in && in->car[k].dist < TR_STOP_ZONE_M) near[(int)in->car[k].side] = true;
	if (!near[(int)in->baton]) in->baton = -1;
}

// FlagUpcomingTrafficIntersections (0x8002B42C): the first traffic
// intersection up the lane (GetFirstTrafficIntersection 0x80038FA4), how far
// the car's nose is from its stopping zone -- the crossing point, less
// GetLength, less the 12 m zone (GetDistanceToFirstTrafficIntersection
// 0x80038FC8) -- and when it gets there at the speed it has (AddCarToList
// 0x8002BEFC).
static void flagUpcoming(Traffic* t, int i) {
	TrafficCar* c = &t->car[i];
	c->firstInter = -1;
	for (int k = c->cur + 1; k < c->nNodes && t->inter; k++) {
		const TrafficNode* nd = &c->node[k];
		const int x = gps_point_intersection(nd->point);
		if (x < 0) continue;
		const int side = gps_intersection_side(x, gps_point_route(nd->point));
		if (side < 0) continue;
		const float dist = nd->s - c->progress - gps_intersection_length(x, side, nd->fwd) -
			TR_STOP_ZONE_M - carHalfL(&c->sim);
		const float v = c->sim.speed;
		float eta = 0.0f;
		if (dist > 0.0f) eta = v <= 0.001f ? TR_ETA_NONE : fmaxf(dist / v, 0.0f);
		c->firstInter = x;
		c->firstSide = side;
		c->firstDist = dist;
		TrafficInter* in = &t->inter[x];
		in->car[i].in = true;
		in->car[i].side = (s8)side;
		in->car[i].dist = dist;
		in->car[i].eta = eta;
		if (eta < interSoonestEta(in, side)) in->soonest[side] = (s8)i;
		return;
	}
}

// NeedToAvoidTrafficCar (0x8000E86C): a traffic car need not be avoided when
// the two are coming to the car's first intersection down different routes,
// neither of them in its stopping zone yet (AreCarsApproachingAlongDifferent-
// Routes 0x8002BD78). The player is always avoided.
static bool needToAvoid(const Traffic* t, int self, int who) {
	const TrafficCar* c = &t->car[self];
	if (who < 0 || who >= TRAFFIC_MAX || c->firstInter < 0) return true;
	const TrafficInter* in = &t->inter[c->firstInter];
	const bool apart = in->car[self].in && in->car[who].in && in->car[self].dist >= 0.0f &&
		in->car[who].dist >= 0.0f && in->car[self].side != in->car[who].side;
	return !apart;
}

// IsUpcomingStopSign (0x8002ACFC): a dead end (signpost type 3) at the
// walk's last point, facing the car's way, that the car reaches inside a
// second at max(its speed, 0.1). Think then has it StopDriving (0x80008B3C)
// for good. Only the race routes carry any (3400 over the PC's route files);
// RoutesFreeRoam.bin has none.
static bool upcomingStopSign(const TrafficCar* c) {
	if (c->nNodes < 1) return false;
	const TrafficNode* nd = &c->node[c->nNodes - 1];
	const int n = gps_point_num_signs(nd->point);
	for (int k = 0; k < n; k++) {
		const u32 bits = gps_sign_bits(gps_point_sign(nd->point, k));
		const bool applies = ((bits >> 18) & 1u) != (u32)(nd->fwd ? 1 : 0);
		if (applies && (bits & 15u) == 3u)
			return (nd->s - c->progress) / fmaxf(c->sim.speed, 0.1f) < 1.0f;
	}
	return false;
}

// ---------------------------------------------------------------- actions

static int actionScore(int action) {
	return action == TRAFFIC_ACT_OBEY ? 600 : (action == TRAFFIC_ACT_AVOID ? 300 : 0);
}

// AIActionAvoidAvoidableAsTraffic::TryLane (0x80013F3C): whether the car,
// in lane `cur` and `lat` metres right of it, may drive in lane `lane` -- a
// forward traffic lane it can change to, with nobody ahead in it within the
// avoid's look-ahead, or nobody it must avoid, or nobody under 5 s
// (0x80316AD0) off, or someone other than the car it avoids that is further
// off in time than that one.
static bool tryLane(const Traffic* t, int i, const CarSim* player, const float dir[2],
		float range, int lane, int cur, float lat) {
	const TrafficCar* c = &t->car[i];
	if (lane < 2 || lane > 5) return false;
	if (lane != cur && !canChangeLane(c, cur, lane)) return false;
	const TrafficNode* nd = &c->node[c->cur];
	const float shift = gps_lane_offset(nd->point, laneLine(lane, nd->fwd)) -
		gps_lane_offset(nd->point, laneLine(cur, nd->fwd)) - lat;
	float time, gap;
	const int who = someoneAhead(t, i, player, dir, range, c->avoidLook, shift, &time, &gap, NULL);
	if (who < 0 || !needToAvoid(t, i, who) || time > 5.0f) return true;
	return time > c->avoidTime && who != c->avoidWho;
}

// Think (0x8002A8EC) every 0.2 s: off every intersection's list, then the
// first one up the lane flagged, then AIGoalBeTrafficCar's action. The one
// running is kept until it is done (HasBeenAccomplished) or ChooseNewAction
// (0x8000CC34) finds one that scores higher: obey the intersection ahead
// (600), else avoid what is ahead in the lane (300), else drive the lane (0).
// Every action builds the drive target afresh (BuildDriveTarget 0x8001CFBC),
// from where the car is across its lane.
static void think(Traffic* t, int i, const CarSim* player) {
	TrafficCar* c = &t->car[i];
	float dir[2], lat;
	walkTrack(c, dir, &lat);
	c->driveL = lat;
	c->driveS = c->progress;
	interRemoveAll(t, i);
	if (c->stopDriving) {
		c->desired = 0.0f;
		return;
	}
	if (upcomingStopSign(c)) {
		c->stopDriving = true;
		c->stopWhy = 2;
		c->desired = 0.0f;
		return;
	}
	flagUpcoming(t, i);

	const float look = aidriver_lookahead(&c->drv, &c->sim, c->sim.speed);
	const float range = fmaxf(30.0f, 3.0f * look);
	float desired = desiredSpeed(c);
	float time, gap, relV;

	// UpdateStatus: whether the action running is done.
	bool done = false;
	if (c->action == TRAFFIC_ACT_OBEY) {
		// HasBeenAccomplished (0x80014B50): no intersection up the lane; a
		// new one starts it afresh (ResetForNewIntersection 0x80014618).
		done = c->firstInter < 0;
		if (!done && c->firstInter != c->obeyInter) {
			c->obeyInter = c->firstInter;
			c->obeyStopped = false;
		}
	} else if (c->action == TRAFFIC_ACT_AVOID) {
		// HasBeenAccomplished (0x80013D80): the one it avoids is still ahead
		// and not pulling away by more than 5 m/s, or someone is ahead within
		// 5 s.
		bool keep = false;
		const int who = someoneAhead(t, i, player, dir, range, TR_NO_TELEPORT, 0.0f, &time, &gap, &relV);
		if (who == c->avoidWho && relV <= 5.0f) keep = true;
		if (!keep) {
			const int w2 = someoneAhead(t, i, player, dir, range, c->avoidLook, 0.0f, &time, &gap, NULL);
			if (w2 >= 0 && time <= 5.0f) {
				keep = true;
				if (w2 != c->avoidWho) {
					c->avoidWho = w2;
					c->avoidTime = time;
					c->avoidLook = 30.0f;
				}
			}
		}
		done = !keep;
	}

	// ChooseNewAction. A finished action counts as none, at -1.
	const int cur = done ? -1 : c->action;
	const int curScore = done ? -1 : actionScore(c->action);
	int want = TRAFFIC_ACT_LANE, avoidWho = -1;
	float avoidTime = 0.0f;
	if (cur != TRAFFIC_ACT_OBEY && curScore <= 600 && c->firstInter >= 0) {
		want = TRAFFIC_ACT_OBEY;
	} else if (cur != TRAFFIC_ACT_AVOID && curScore <= 300) {
		// CalculateScore (0x80013BC4): anyone ahead in the lane within 5 s
		// whom NeedToAvoidTrafficCar does not excuse -- unless it is a racer
		// pulling away at over 20 m/s from a car on the TrafficMover.
		const int who = someoneAhead(t, i, player, dir, range, TR_NO_TELEPORT, 0.0f, &time, &gap, NULL);
		if (who >= 0 && time <= 5.0f &&
				!(who == TRAFFIC_MAX && !c->mellow && player->speed > 20.0f) &&
				needToAvoid(t, i, who)) {
			want = TRAFFIC_ACT_AVOID;
			avoidWho = who;
			avoidTime = time;
		}
	}
	if (actionScore(want) > curScore) {
		c->action = want;
		if (want == TRAFFIC_ACT_OBEY) {
			c->obeyInter = c->firstInter;
			c->obeyStopped = false;
		} else if (want == TRAFFIC_ACT_AVOID) {
			c->avoidWho = avoidWho;
			c->avoidTime = avoidTime;
			c->avoidLook = 30.0f;
			if (kAvoidMellow && !c->mellow) {
				c->mellow = true;
				carsim_set_mellow(&c->sim, true, 0.0f, carsim_mellow_mu(false, 0u));
				c->drv.trafficMover = false;
			}
		}
	}

	if (c->action == TRAFFIC_ACT_AVOID) {
		// CalculatePointsToDrive (0x8001405C). Avoiding the player, and the
		// player not coming at it faster than 40 m/s (0x80316B14), the car
		// tries its own lane and then the ones beside it -- first the side
		// the player is not drifting to -- and takes the first TryLane lets
		// it have; with none, it brakes.
		const int cur = nodeLane(&c->node[c->cur]);
		float toPlayer = 0.0f;
		bool brake = false;
		if (c->avoidWho == TRAFFIC_MAX && player) {
			const CarSim* me = &c->sim;
			const float dx = player->rbPos[0] - me->rbPos[0], dy = player->rbPos[1] - me->rbPos[1];
			const float vx = player->vel[0] - me->vel[0], vy = player->vel[1] - me->vel[1];
			const float relLong = vx * dir[0] + vy * dir[1];
			if (dx * dx + dy * dy <= range * range && relLong > -40.0f) {
				const float relLat = vx * dir[1] - vy * dir[0];
				toPlayer = fmaxf(0.0f, dx * dir[0] + dy * dir[1] - carHalfL(player) - carHalfL(me));
				const int k = relLat <= 0.0f ? 1 : -1;
				const int order[5] = { cur, cur + k, cur - k, cur + 2 * k, cur - 2 * k };
				int lane = -1;
				for (int n = 0; n < 5 && lane < 0; n++)
					if (tryLane(t, i, player, dir, range, order[n], cur, lat)) lane = order[n];
				if (lane < 0) {
					brake = true;
				} else if (lane != cur) {
					changeLane(t, c, lane);
					walkTrack(c, dir, &lat);
					c->driveL = lat;
					c->driveS = c->progress;
				}
			}
		}
		// Then whoever is ahead in the lane it has chosen: IsSomeoneAhead-
		// InThisLane at the choice's L, `lat` off the car's own across.
		const int who = someoneAhead(t, i, player, dir, range, c->avoidLook, -lat, &time, &gap, NULL);
		if (who >= 0) {
			if (who != c->avoidWho) {
				c->avoidWho = who;
				c->avoidLook = 30.0f;
			}
			c->avoidTime = time;
			if (c->sim.speed < 0.5f) {
				// At rest behind it: the look-ahead stretched to the gap, and
				// the lane the car has come to be in (GetNearestTrafficLane
				// 0x80018650) taken as its own.
				c->avoidLook = fmaxf(gap, fmaxf(c->avoidLook, 30.0f));
				const TrafficNode* nd = &c->node[c->cur];
				const float pos[2] = { c->x, c->y };
				const int lane = nearestLane(nd->point, nd->fwd, pos);
				if (lane >= 0 && lane != nodeLane(nd)) {
					changeLane(t, c, lane);
					walkTrack(c, dir, &lat);
					c->driveL = lat;
					c->driveS = c->progress;
				}
			}
			// Under 1.5 s (0x80316B20) off, or the player within the look-
			// ahead, it slows; `toPlayer` stays 0 when it is not the player
			// being avoided, so behind anything else it always does.
			brake = time < 1.5f || toPlayer <= c->avoidLook;
		}
		if (brake) desired = gap <= 15.0f ? 0.0f : fminf(desired, 6.0f);
	} else if (c->action == TRAFFIC_ACT_OBEY) {
		// CalculatePointsToDrive (0x80014CD0): the car ahead first, as the
		// avoid would, and then the intersection. Held, the car slows to
		// 6 m/s inside 30 m of the stopping zone and stops in it; at rest
		// there (under 0.5 m/s), a stop sign has been obeyed. Let through,
		// it takes the baton.
		const int who = someoneAhead(t, i, player, dir, range, TR_OBEY_LOOK, 0.0f, &time, &gap, NULL);
		if (who >= 0 && needToAvoid(t, i, who) && (time < 1.5f || gap < TR_OBEY_LOOK))
			desired = gap <= 15.0f ? fminf(desired, 0.0f) : fminf(desired, 6.0f);
		TrafficInter* in = &t->inter[c->obeyInter];
		const int type = gps_intersection_type(c->obeyInter);
		bool hold = false;
		if (!c->obeyStopped && interMustStop(type, c->firstSide))
			hold = true;
		else if (!interAllowed(in, type, c->firstSide))
			hold = true;
		else
			interSetBaton(in, c->firstSide);
		if (hold) {
			if (c->firstDist <= TR_OBEY_LOOK) desired = fminf(desired, 6.0f);
			if (c->firstDist < 0.0f) {
				desired = fminf(desired, 0.0f);
				if (c->sim.speed < 0.5f) {
					if (!c->obeyStopped) t->yields++;
					c->obeyStopped = true;
				}
			}
		}
	}
	c->desired = desired;
}

// ---------------------------------------------------------------- driving

// TrafficMover::Move (0x8002A080).
static void moverMove(TrafficCar* c, float dt) {
	const float fx = cosf(c->heading), fy = sinf(c->heading);
	const float lx = -fy, ly = fx;
	float steer = (float)c->drv.steerU16 * (TR_TWO_PI / 65536.0f);
	if (steer > TR_PI) steer -= TR_TWO_PI;
	const float rearX = c->sim.wheel[3].pos[0];
	const float wb = c->sim.wheel[0].pos[0] - rearX;
	const float d = c->speed * dt;
	if (wb > 0.0f && fabsf(steer) >= wb / 100.0f) {
		const float r = wb / steer;
		const float dth = d / r;
		const float cx = c->x + fx * rearX + lx * r;
		const float cy = c->y + fy * rearX + ly * r;
		const float ox = c->x - cx, oy = c->y - cy;
		const float cs = cosf(dth), sn = sinf(dth);
		c->x = cx + ox * cs - oy * sn;
		c->y = cy + ox * sn + oy * cs;
		c->heading += dth;
		if (c->heading > TR_PI) c->heading -= TR_TWO_PI;
		else if (c->heading < -TR_PI) c->heading += TR_TWO_PI;
	} else {
		c->x += fx * d;
		c->y += fy * d;
	}
	c->speed += dt * c->drv.prevThrottle * tableAt(kAccel, c->speed) -
		dt * c->drv.prevBrake * tableAt(kBrake, c->speed);
	c->speed = clampf(c->speed, 0.0f, TR_MAX_SPEED);
}

static void driveCar(Traffic* t, int i, const CarSimWorld* w, const CarSim* player, float dt) {
	TrafficCar* c = &t->car[i];
	walkExtend(t, c);
	float dir[2], lat;
	walkTrack(c, dir, &lat);
	if (walkFinished(c)) {
		// FuturePoints::AllowTeleportation: the lane ran out, the car goes.
		disableCar(c);
		return;
	}
	c->thinkSec -= dt;
	if (c->thinkSec <= 0.0f) {
		c->thinkSec += TR_THINK_SEC;
		if (c->thinkSec <= 0.0f) c->thinkSec = TR_THINK_SEC;
		think(t, i, player);
		walkTrack(c, dir, &lat); // a lane change moves the walk
	}

	// The drive target: the lane at the driver's look-ahead, off it by the
	// L the target has eased to there; the lateral offset is the car's off
	// the target where it is.
	const float bodySpeed = c->sim.vel[0] * c->sim.rot[0][0] + c->sim.vel[1] * c->sim.rot[0][1];
	const float look = aidriver_lookahead(&c->drv, &c->sim, bodySpeed);
	float tgt[3], tdir[2];
	walkSample(c, c->progress + look, tgt, NULL);
	walkDirAt(c, c->progress + look, tdir);
	const float l = driveLAt(c, c->progress + look);
	c->drv.steerTo[0] = tgt[0] + l * tdir[1];
	c->drv.steerTo[1] = tgt[1] - l * tdir[0];
	c->drv.lateral_m = lat - driveLAt(c, c->progress);
	c->drv.desired = c->desired;

	CarSimInput in;
	aidriver_drive(&c->drv, &c->sim, dt, &in);
	if (c->stopDriving) {
		// StopDriving: the pedals let go, the brake held.
		in.throttle = 0.0f;
		in.brake = 1.0f;
		c->drv.prevThrottle = 0.0f;
		c->drv.prevBrake = 1.0f;
	}

	const bool nearPlayer = c->playerDist <= TR_PHYS_RANGE;
	if (c->mellow && nearPlayer && w) {
		carsim_update(&c->sim, &in, w, dt);
		c->x = c->sim.rbPos[0];
		c->y = c->sim.rbPos[1];
		c->heading = atan2f(c->sim.rot[0][1], c->sim.rot[0][0]);
		c->speed = c->sim.speed;
		if (c->sim.rbPos[2] < c->roadZ - TR_FALL_M)
			disableCar(c);
	} else {
		// PORT: a mellow car out of the world's reach rolls on the
		// TrafficMover until the teleporter takes it.
		moverMove(c, dt);
		walkTrack(c, dir, &lat);
		carsim_traffic_pose(&c->sim, nearPlayer ? w : NULL, c->x, c->y, c->roadZ, c->heading,
			c->speed, dt);
	}
}

// Car against car: the player's against each traffic car near it, and the
// traffic's against each other. A hit puts a car on the MellowMover (Think's
// AIActionTraffic hit, type 8); CheckForMajorCollisions (0x8002B5A4) stops
// it driving once a collision's impulse passes 30.
static void hitCar(TrafficCar* c, float impulse) {
	if (!c->mellow) {
		c->mellow = true;
		carsim_set_mellow(&c->sim, true, 0.0f, carsim_mellow_mu(false, 0u));
		c->drv.trafficMover = false;
	}
	if (c->hitLast > 0.5f) c->hitSum = 0.0f;
	c->hitSum += impulse;
	c->hitLast = 0.0f;
	if (c->hitSum > TR_HIT_STOP && !c->stopDriving) {
		c->stopDriving = true;
		c->stopWhy = 1;
	}
}

static bool contact(CarSim* a, TrafficCar* ca, CarSim* b, TrafficCar* cb, float dt) {
	float pa[3], pb[3];
	memcpy(pa, a->P, sizeof(pa));
	memcpy(pb, b->P, sizeof(pb));
	if (!carsim_car_contact(a, b, dt)) return false;
	if (ca) {
		const float d[3] = { a->P[0] - pa[0], a->P[1] - pa[1], a->P[2] - pa[2] };
		hitCar(ca, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
	}
	if (cb) {
		const float d[3] = { b->P[0] - pb[0], b->P[1] - pb[1], b->P[2] - pb[2] };
		hitCar(cb, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
	}
	return true;
}

static void contacts(Traffic* t, CarSim* player, float dt) {
	for (int i = 0; i < t->count; i++) {
		TrafficCar* a = &t->car[i];
		if (!a->active || a->playerDist > TR_CONTACT_RANGE) continue;
		if (player && !t->playerGhost) contact(player, NULL, &a->sim, a, dt);
		for (int j = i + 1; j < t->count; j++) {
			TrafficCar* b = &t->car[j];
			if (!b->active || b->playerDist > TR_CONTACT_RANGE) continue;
			const float dx = a->x - b->x, dy = a->y - b->y;
			if (dx * dx + dy * dy > 100.0f) continue;
			contact(&a->sim, a, &b->sim, b, dt);
		}
	}
}

void traffic_car_contact(Traffic* t, CarSim* car, float dt) {
	if (!t || !t->open || !car || dt <= 0.0f) return;
	const float cp[2] = { car->pos[0], car->pos[1] };
	for (int i = 0; i < t->count; i++) {
		TrafficCar* a = &t->car[i];
		if (!a->active || a->playerDist > TR_CONTACT_RANGE) continue;
		const float ap[2] = { a->x, a->y };
		if (dist2d(cp, ap) < 10.0f && contact(car, NULL, &a->sim, a, dt)) t->racerHits++;
	}
}

// ---------------------------------------------------------------- types

static bool packExists(const char* root, const char* dir) {
	char path[320];
	snprintf(path, sizeof(path), "%s/CARS/%s.nfsc", root, dir);
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

static int findSlot(const Model3D* m, u32 hash) {
	const int n = model3d_texture_count(m);
	for (int i = 0; i < n; i++)
		if (model3d_texture_hash(m, i) == hash) return i;
	return -1;
}

// The pack's TRFX block (tools/car_pack.py): the light material each batch is
// authored with, the body's head- and brake-light markers, and from version 2
// where each corner's tyre turns. A pack without one draws with the plain car
// lighting and no flares; one before version 2 keeps its tyres still.
#define TRFX_MAGIC   0x58465254u  // "TRFX"
#define TRFX_HDR     16u
#define TRFX_LIGHT   28u
#define TRFX_PIVOTS  48u
static void loadExtras(Traffic* t, TrafficType* ty) {
	u32 bytes, hdr[4];
	const u8* x = (const u8*)model3d_extra(ty->model, &bytes);
	if (!x || bytes < TRFX_HDR) return;
	memcpy(hdr, x, sizeof(hdr));
	const u32 nBatch = hdr[2], nLight = hdr[3];
	const u32 pivots = hdr[1] >= 2u ? TRFX_PIVOTS : 0u;
	if (hdr[0] != TRFX_MAGIC || hdr[1] < 1u || hdr[1] > 2u || nBatch > 0xFFFFu ||
	    nLight > CARKIT_LIGHTS || TRFX_HDR + nBatch * 4u + nLight * TRFX_LIGHT + pivots > bytes)
		return;
	if (pivots) {
		float pv[4][3];
		memcpy(pv, x + TRFX_HDR + nBatch * 4u + nLight * TRFX_LIGHT, sizeof(pv));
		model3d_set_car_pivots(ty->model, (const float (*)[3])pv);
	}
	u8* mats = nBatch && lightmat_load(t->dataRoot) ? (u8*)malloc(nBatch) : NULL;
	if (mats) {
		for (u32 i = 0; i < nBatch; i++) {
			u32 h;
			memcpy(&h, x + TRFX_HDR + i * 4u, 4);
			mats[i] = lightmat_index(lightmat_get(h));
		}
		ty->lit = model3d_set_light_mats(ty->model, mats, (int)nBatch);
		free(mats);
	}
	static CarKitInfo info;
	memset(&info, 0, sizeof(info));
	const u8* l = x + TRFX_HDR + nBatch * 4u;
	for (u32 i = 0; i < nLight; i++, l += TRFX_LIGHT) {
		CarKitLight* k = &info.light[info.lights++];
		k->type = l[0];
		k->side = l[1];
		memcpy(k->pos, l + 4, sizeof(k->pos));
		memcpy(k->dir, l + 16, sizeof(k->dir));
	}
	carlights_setup(&ty->lights, &info);
}

static bool loadType(Traffic* t, TrafficType* ty, void (*log)(const char*)) {
	char path[320], name[48], b[400];
	snprintf(path, sizeof(path), "%s/CARS/%s.nfsc", t->dataRoot, ty->car.geomDir);
	ty->model = model3d_load(path);
	if (!ty->model) {
		if (log) { snprintf(b, sizeof(b), "traffic: %s would not load", path); log(b); }
		return false;
	}
	model3d_set_car_lods(ty->model, true);
	// The rear tyres' inboard axes, for TireFace's swap to TIRE_FRONT_B
	// (car_pack.py BATCH_CORNER_SHIFT): the right one sits as authored,
	// running inboard along +y, the left one half a turn about z. A pack from
	// before the tags has no batch to swap.
	static const float rimAxis[2][3] = { { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } };
	model3d_set_car_rims(ty->model, rimAxis);
	loadExtras(t, ty);
	// After the light materials, which are per batch as packed (#345).
	{
		const int before = model3d_batch_count(ty->model);
		const int gone = model3d_merge_batches(ty->model);
		if (log && gone) {
			snprintf(b, sizeof(b), "traffic: %s's %d batches drawn as %d", ty->car.geomDir,
				before, before - gone);
			log(b);
		}
	}
	snprintf(name, sizeof(name), "%s_SKIN1", ty->car.geomDir);
	ty->skin1Slot = findSlot(ty->model, strHash(name));
	const int n = model3d_texture_count(ty->model);
	int skins = 0;
	for (int k = 2; k <= CARDB_MAX_SKINS && ty->skin1Slot >= 0 && n > 0; k++) {
		snprintf(name, sizeof(name), "%s_SKIN%d", ty->car.geomDir, k);
		const int slot = findSlot(ty->model, strHash(name));
		if (slot < 0) continue;
		C3D_Tex** arr = (C3D_Tex**)malloc(sizeof(C3D_Tex*) * (size_t)n);
		if (!arr) break;
		for (int s = 0; s < n; s++) arr[s] = model3d_own_texture(ty->model, s);
		arr[ty->skin1Slot] = model3d_own_texture(ty->model, slot);
		ty->skinTex[k - 1] = arr;
		skins++;
	}
	if (log) {
		snprintf(b, sizeof(b), "traffic: type %s, weight %d, %d texture(s), SKIN1 slot %d, "
			"%d more skin(s) of %d, %s, %d light flare(s), headlamps %.2f m ahead", ty->car.geomDir,
			ty->weight, n, ty->skin1Slot, skins, (int)ty->car.skinCount,
			ty->lit ? "lit by its materials" : "PLAIN lighting", ty->lights.lights,
			(double)carlights_head_x(&ty->lights));
		log(b);
	}
	return true;
}

// SetInitialDesiredCarTypes (0x8003C760): up to four types, each capped at
// its weight in the zone; the cars draw among the types that still have room
// (PORT: the order of the draws is the port's own).
static int pickType(Traffic* t, const CarDb* db) {
	int eligible[TRAFFIC_CANDIDATES + TRAFFIC_TYPES];
	int n = 0;
	for (int k = 0; k < t->nTypes; k++)
		if (t->type[k].count < t->type[k].weight) eligible[n++] = k;
	if (t->nTypes < TRAFFIC_TYPES) {
		for (int c = 0; c < t->nCand; c++) {
			bool used = false;
			for (int k = 0; k < t->nTypes; k++) used |= t->type[k].rec == t->cand[c];
			if (!used && db->recs[t->cand[c]].trafficWeight[t->zoneType] > 0)
				eligible[n++] = -1 - c;
		}
	}
	if (n == 0) return t->nTypes > 0 ? 0 : -1;
	const int e = eligible[randi(t, n)];
	if (e >= 0) return e;
	const int rec = t->cand[-1 - e];
	TrafficType* ty = &t->type[t->nTypes];
	memset(ty, 0, sizeof(*ty));
	ty->rec = rec;
	ty->car = db->recs[rec];
	ty->weight = db->recs[rec].trafficWeight[t->zoneType];
	return t->nTypes++;
}

// ---------------------------------------------------------------- API

static bool trafficOpen(Traffic* t, const TrafficOpen* o);

bool traffic_open(Traffic* t, const TrafficOpen* o) {
	memset(t, 0, sizeof(*t));
	if (!o || !o->cars || !o->track || o->level < 1 || o->level >= TRACKDB_TRAFFIC_LEVELS)
		return false;
	const int net = gps_net_select(o->race ? GPS_NET_RACE : GPS_NET_FREEROAM);
	const bool ok = trafficOpen(t, o);
	gps_net_select(net);
	return ok;
}

static bool trafficOpen(Traffic* t, const TrafficOpen* o) {
	if (gps_num_routes() <= 0) return false;
	char b[200];
	snprintf(t->dataRoot, sizeof(t->dataRoot), "%s", o->dataRoot ? o->dataRoot : "");
	t->paths = o->paths;
	t->rng = o->seed ? o->seed : 0x1D872B41u;
	const int rev = o->reversed ? 1 : 0;
	t->initialSpacing = o->track->trafficSpacing[rev] > 1.0f ? o->track->trafficSpacing[rev] : 31.0f;
	t->dbgBehindM = o->dbgBehindM;
	t->lastCar = -1;
	t->level = o->level;
	t->race = o->race;
	t->drag = o->race && o->drag;
	t->oncoming = o->track->trafficOncoming[o->level];
	t->startOk = o->track->trafficStartOk[rev] != 0;
	t->startClear = o->track->trafficStartClear[rev];

	// AddTrafficCars (0x80119E20): min(16, TrackInfo +0xEC[reversed + level
	// * 2]). Free roam is level 3, not reversed.
	int count = o->track->trafficCars[rev + o->level * 2];
	if (count > TRAFFIC_MAX) count = TRAFFIC_MAX;

	// The zone the player starts in picks the weights (+0x3C).
	t->zoneType = 0;
	if (o->paths && o->playerPos) {
		const TrackPathZone* z = trackpath_find(o->paths, o->playerPos[0], o->playerPos[1],
			TPZ_TRAFFIC_TYPE, NULL);
		if (z && z->unk30[3] < CARDB_TRAFFIC_ZONE_TYPES) t->zoneType = (int)z->unk30[3];
	}

	for (int r = 0; r < o->cars->count && t->nCand < TRAFFIC_CANDIDATES; r++) {
		const CarRec* rec = &o->cars->recs[r];
		if (rec->kind != 2 || !rec->geomDir[0]) continue;
		if (!packExists(t->dataRoot, rec->geomDir)) continue;
		t->cand[t->nCand++] = r;
	}

	// The routes with traffic each way, for GenerateRandomPoint -- each way
	// the race's, on a route that runs against it.
	t->nRoutes = gps_num_routes();
	if (t->nRoutes > 0) {
		t->cumFwd = (int*)calloc((size_t)t->nRoutes + 1, sizeof(int));
		t->cumBack = (int*)calloc((size_t)t->nRoutes + 1, sizeof(int));
	}
	if (t->cumFwd && t->cumBack) {
		for (int r = 0; r < t->nRoutes; r++) {
			const int n = gps_route_points(r);
			const bool flip = againstRace(t, r);
			t->cumFwd[r + 1] = t->cumFwd[r] + (gps_route_has_traffic(r, !flip) ? n : 0);
			t->cumBack[r + 1] = t->cumBack[r] + (gps_route_has_traffic(r, flip) ? n : 0);
		}
		t->totalFwd = t->cumFwd[t->nRoutes];
		t->totalBack = t->cumBack[t->nRoutes];
	}

	// The intersections' state, as the chunk has it: no baton, nobody coming.
	t->nInter = gps_num_intersections();
	t->inter = t->nInter > 0 ? (TrafficInter*)calloc((size_t)t->nInter, sizeof(TrafficInter)) : NULL;
	if (!t->inter) t->nInter = 0;
	for (int x = 0; x < t->nInter; x++) {
		t->inter[x].baton = -1;
		t->inter[x].soonest[0] = t->inter[x].soonest[1] = -1;
	}

	if (o->log) {
		snprintf(b, sizeof(b), "traffic: %s level %d, %d car(s), zone type %d, %d candidate "
			"type(s) packed, %d/%d points with traffic fwd/back, spacing %.0f m, %d "
			"intersection(s)", t->race ? "race" : "free roam", t->level, count, t->zoneType,
			t->nCand, t->totalFwd, t->totalBack, t->initialSpacing, t->nInter);
		o->log(b);
	}
	if (count <= 0 || t->nCand == 0 || t->totalFwd + t->totalBack == 0) {
		traffic_close(t);
		return false;
	}

	for (int i = 0; i < count; i++) {
		TrafficCar* c = &t->car[i];
		const int k = pickType(t, o->cars);
		if (k < 0) break;
		TrafficType* ty = &t->type[k];
		ty->count++;
		c->type = k;
		c->skin = ty->car.skinCount > 0 ? ty->car.skins[randi(t, ty->car.skinCount)] : 1;
		const float zero[3] = { 0.0f, 0.0f, 0.0f };
		carsim_init(&c->sim, &ty->car.perf, &ty->car.chassis, &ty->car.phys, zero, 0.0f);
		aidriver_init(&c->drv, aidriver_car_class_raw(ty->car.measureStock));
		c->drv.racing = true;
		c->drv.traffic = true;
		c->drv.trafficMover = true;
		// Spread the cars' Thinks over the period.
		c->thinkSec = TR_THINK_SEC * (float)i / (float)count;
		c->used = true;
		c->stopped = true;
		t->count = i + 1;
	}
	for (int k = 0; k < t->nTypes; k++) {
		if (!loadType(t, &t->type[k], o->log)) {
			// A type that will not load takes no cars.
			for (int i = 0; i < t->count; i++)
				if (t->car[i].type == k) t->car[i].used = false;
		}
	}
	for (int i = 0; i < t->count; i++) {
		TrafficCar* c = &t->car[i];
		const TrafficType* ty = &t->type[c->type];
		c->tex = (c->skin >= 2 && c->skin <= CARDB_MAX_SKINS) ? ty->skinTex[c->skin - 1] : NULL;
	}
	if (o->log) {
		for (int k = 0; k < t->nTypes; k++) {
			snprintf(b, sizeof(b), "traffic: %s x%d", t->type[k].car.geomDir, t->type[k].count);
			o->log(b);
		}
	}
	t->open = t->count > 0;
	if (!t->open) traffic_close(t);
	return t->open;
}

void traffic_hide_all(Traffic* t, bool hide) {
	if (!t || !t->open) return;
	for (int i = 0; i < t->count; i++) {
		TrafficCar* c = &t->car[i];
		if (!c->used) continue;
		if (hide) {
			// DisableCar, then RecycleCar's +0x784.
			c->hideActive = c->active;
			disableCar(c);
			interRemoveAll(t, i);
		} else if (c->hideActive) {
			// EnableCar: back on the road where it was, still recycled.
			// PORT: only the cars that were on it; retail's also brings back
			// one the teleporter had failed to place, where it stood.
			c->active = true;
		}
	}
	t->hidden = hide;
}

void traffic_close(Traffic* t) {
	for (int k = 0; k < TRAFFIC_TYPES; k++) {
		TrafficType* ty = &t->type[k];
		for (int s = 0; s < CARDB_MAX_SKINS; s++) free(ty->skinTex[s]);
		if (ty->model) model3d_free(ty->model);
	}
	free(t->cumFwd);
	free(t->cumBack);
	free(t->inter);
	memset(t, 0, sizeof(*t));
}

static void trafficUpdate(Traffic* t, const CarSimWorld* w, CarSim* player, float dt,
		void (*log)(const char* msg));

void traffic_update(Traffic* t, const CarSimWorld* w, CarSim* player,
		const TrafficRace* race, float dt, void (*log)(const char* msg)) {
	if (!t || !t->open || !player || dt <= 0.0f) return;
	if (t->race && (!race || !race->route)) return;
	if (dt > 0.1f) dt = 0.1f;
	t->rc = race;
	const int net = gps_net_select(t->race ? GPS_NET_RACE : GPS_NET_FREEROAM);
	trafficUpdate(t, w, player, dt, log);
	gps_net_select(net);
	t->rc = NULL;
}

static void trafficUpdate(Traffic* t, const CarSimWorld* w, CarSim* player, float dt,
		void (*log)(const char* msg)) {

	if (!t->placed) {
		// AddTrafficCars: each car put down somewhere on the network.
		// PlaceNewCar (0x8002CD00): the drag strip's own in a drag (where a
		// miss leaves the car off: PlaceNewCarOntoRaceTrack is not tried).
		for (int i = 0; i < t->count; i++) {
			if (!t->car[i].used) continue;
			if (t->drag) {
				if (!placeNewCarDrag(t, i, player)) disableCar(&t->car[i]);
			} else {
				placeNewCar(t, i, player);
			}
		}
		t->placed = true;
	}
	if (t->dbgBehindM != 0.0f && !t->dbgPlaced && (t->dbgTrySec -= dt) <= 0.0f) {
		t->dbgTrySec = 1.0f;
		t->dbgPlaced = dbgPlaceBehind(t, player, log);
	}

	float pp[3];
	playerPos(player, pp);
	for (int i = 0; i < t->count; i++) {
		TrafficCar* c = &t->car[i];
		const float cp[2] = { c->x, c->y };
		c->playerDist = dist2d(cp, pp);
		c->hitLast += dt;
		// Each car's TrackPathCoordinate, as every Car keeps one in a race.
		if (racing(t) && c->active) race_coord_update(t->rc->route, &c->coord, cp);
	}

	// DoTimestep (0x8002C8B0) thinks only while the race is on, and not
	// while MakeAllTrafficCarsDisappear has the teleporter off.
	if (raceOn(t) && !t->hidden) {
		t->thinkSec -= dt;
		if (t->thinkSec <= 0.0f) {
			t->thinkSec += TR_TELEPORT_SEC;
			if (t->thinkSec <= 0.0f) t->thinkSec = TR_TELEPORT_SEC;
			teleporterThink(t, player);
		}
	}

	// UpdateTrafficIntersections (0x8002FD48), every frame. PORT: a car off
	// the road leaves the lists here; retail leaves it on them until its
	// next Think, which a car the teleporter could not place may never have.
	for (int i = 0; i < t->count; i++)
		if (!t->car[i].active) interRemoveAll(t, i);
	for (int x = 0; x < t->nInter; x++) interRelease(&t->inter[x]);

	int steps = (int)ceilf(dt / TR_STEP_SEC - 0.01f);
	if (steps < 1) steps = 1;
	const float h = dt / (float)steps;
	for (int i = 0; i < t->count; i++) {
		TrafficCar* c = &t->car[i];
		for (int k = 0; k < steps && c->used && c->active; k++)
			driveCar(t, i, w, player, h);
	}
	contacts(t, player, dt);

	// #401's donut watch (PORT, a diagnostic): a car that turns a whole circle
	// in 8 s without leaving a 20 m circle says how it is driving.
	for (int i = 0; i < t->count; i++) {
		TrafficCar* c = &t->car[i];
		if (!c->used || !c->active) {
			c->dnSec = 0.0f;
			continue;
		}
		if (c->dnSec <= 0.0f) {
			c->dnTurn = 0.0f;
			c->dnPrevH = c->heading;
			c->dnProg0 = c->progress;
			c->dnX = c->x;
			c->dnY = c->y;
			c->dnSec = 8.0f;
			continue;
		}
		float dh = c->heading - c->dnPrevH;
		if (dh > TR_PI) dh -= TR_TWO_PI;
		else if (dh < -TR_PI) dh += TR_TWO_PI;
		c->dnTurn += dh;
		c->dnPrevH = c->heading;
		c->dnSec -= dt;
		const float mx = c->x - c->dnX, my = c->y - c->dnY;
		if (fabsf(c->dnTurn) >= TR_TWO_PI && mx * mx + my * my < 20.0f * 20.0f) {
			if (log && !c->dnLogged) {
				char b[400];
				float steer = (float)c->drv.steerU16 * (TR_TWO_PI / 65536.0f);
				if (steer > TR_PI) steer -= TR_TWO_PI;
				const float tx = c->drv.steerTo[0] - c->x, ty = c->drv.steerTo[1] - c->y;
				const float fx = cosf(c->heading), fy = sinf(c->heading);
				snprintf(b, sizeof(b), "traffic: #401 donut -- car %d turned %.0f deg in %.1f s, lane +%.1f m; "
					"%s%s%s, speed %.1f (sim %.1f), steer %.2f rad, desired %.1f, action %d, lane %d, "
					"progress %.1f of walk %.1f (%d nodes, cur %d%s), target %.1f m off at %.0f deg, "
					"lat %.1f, driveL %.1f, hits %.1f %.1fs ago, player %.0f m, at (%.1f, %.1f)",
					i, fabsf(c->dnTurn) * 57.29578f, 8.0f - c->dnSec, c->progress - c->dnProg0,
					c->mellow ? "mellow" : "mover", c->stopDriving ? " stopDriving" : "",
					c->playerDist <= TR_PHYS_RANGE ? " in phys range" : "", c->speed, c->sim.speed,
					steer, c->desired, c->action, c->lane, c->progress,
					c->nNodes ? c->node[c->nNodes - 1].s : 0.0f, c->nNodes, c->cur,
					c->walkEnded ? ", walk ended" : "", sqrtf(tx * tx + ty * ty),
					atan2f(fx * ty - fy * tx, fx * tx + fy * ty) * 57.29578f, c->drv.lateral_m,
					c->driveL, c->hitSum, c->hitLast, c->playerDist, c->x, c->y);
				log(b);
				c->dnLogged = true;
			}
			c->dnSec = 0.0f;
		} else if (c->dnSec <= 0.0f) {
			c->dnLogged = false;
		}
	}

	t->logSec -= dt;
	if (log && t->logSec <= 0.0f) {
		t->logSec = 10.0f;
		int active = 0, seen = 0, mellow = 0, avoid = 0, obey = 0, held = 0, stopped = 0;
		for (int i = 0; i < t->count; i++) {
			const TrafficCar* c = &t->car[i];
			if (!c->active) { stopped++; continue; }
			active++;
			seen += c->playerDist <= TR_VISIBLE_M;
			mellow += c->mellow;
			avoid += c->action == TRAFFIC_ACT_AVOID;
			obey += c->action == TRAFFIC_ACT_OBEY;
			held += c->action == TRAFFIC_ACT_OBEY && c->firstDist < 0.0f && c->sim.speed < 0.5f;
		}
		int batons = 0;
		for (int x = 0; x < t->nInter; x++) batons += t->inter[x].baton >= 0;
		const int* f = t->failWhy;
		char b[512];
		snprintf(b, sizeof(b), "traffic: %d on the road (%d within 250 m, %d physics, %d "
			"avoiding, %d at an intersection, %d held there), %d off; teleports %d ok / %d "
			"failed (%d no zone, %d stalled, %d no point, %d dead end, %d past 500 m, %d no "
			"room, %d further; %d from a route alongside); %d baton(s) out, %d give-way "
			"stop(s), %d lane change(s), %d racer contact(s); corners %d cut, %d straight (the "
			"last %.1f / %.1f m from the edge, %.1f on)", active, seen, mellow, avoid, obey,
			held, stopped,
			t->teleports, t->teleportFails, f[TF_NO_ZONE], f[TF_STALLED], f[TF_NO_POINT],
			f[TF_DEAD_END], f[TF_TOO_FAR], f[TF_NO_ROOM], f[TF_FURTHER], t->altStarts, batons,
			t->yields, t->laneChanges, t->racerHits, t->cornersCut, t->cornersStraight, t->cornerOld,
			t->cornerNew, t->cornerOn);
		log(b);
		if (player->speed < TR_STALL_PLAYER) {
			// PORT, #341: what TooManyStalled is counting right now.
			int nHit = 0, nSign = 0, nHeld = 0, nMellow = 0, nOther = 0;
			float nearest = 1e9f;
			for (int i = 0; i < t->count; i++) {
				const TrafficCar* c = &t->car[i];
				if (!c->active || c->stopped || c->sim.speed >= TR_STALL_CAR) continue;
				if (c->playerDist >= TR_TELEPORT_FROM_M) continue;
				if (c->playerDist < nearest) nearest = c->playerDist;
				if (c->stopDriving && c->stopWhy == 1) nHit++;
				else if (c->stopDriving && c->stopWhy == 2) nSign++;
				else if (c->action == TRAFFIC_ACT_OBEY) nHeld++;
				else if (c->mellow) nMellow++;
				else nOther++;
			}
			if (nHit + nSign + nHeld + nMellow + nOther > 0) {
				snprintf(b, sizeof(b), "traffic: stalled within 250 m, player %.1f m/s: %d hit-stopped, "
					"%d at a stop sign, %d held at an intersection, %d physics, %d other; nearest %.0f m",
					player->speed, nHit, nSign, nHeld, nMellow, nOther, nearest);
				log(b);
			}
		}
	}
}
