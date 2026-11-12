#include "draglane.h"

#include <math.h>

// See draglane.h.

// How far ahead a racer's FuturePoints see a lane run (DoesDrivingLineExtend
// AllTheWay). PORT: retail's span is however far its future points reach,
// which is not decoded; a lane that runs this far counts as running all the
// way.
#define DRAGLANE_AHEAD   100.0f
// CanChangeDrivingLine's DoesDrivingLineExtendAtLeastAsFar margins: the lane
// changed to must run 25 m further than the one left, unless it runs all the
// way (0x803187C0); each lane crossed on the way at least as far (-0.1,
// 0x803187C4).
#define DRAGLANE_LONGER  25.0f
#define DRAGLANE_AS_FAR  (-0.1f)
// IsValidLaneChange: a flagged ban starts this much sooner for an AI driver
// (0x803187C8).
#define DRAGLANE_AI_SOONER 80.0f
// DragController::Think lets the exclusion go once the car is within this of
// its choice's line (GetDL's L, 0x8031740C).
#define DRAGCTL_ARRIVED  0.5f
// DragLateralSpeedGraph (r13-0x540C), in mph (2.237 a m/s, 0x80317410).
#define DRAG_MPH         2.237f
#define DRAG_RATE_SLOW   0.75f   // at 0 mph
#define DRAG_RATE_FAST   0.25f   // at 100 mph
#define DRAG_RATE_MPH    100.0f
// PORT: retail's lanes are those of the route a car is on (IsDrivingLineValid
// on its FuturePoints); the port's table takes each lane off whichever road
// has it (trackroute.h). A car's lanes are the ones on its road: within
// DRAGLANE_SAME_Z of its height -- where 4201F's two roads cross, 7-9 m apart
// -- and, for a change, each on the other's road, here and DRAGLANE_JOIN_AHEAD
// on, so that a change is done before two roads part (the post at 1265 m).
#define DRAGLANE_SAME_Z     3.0f
#define DRAGLANE_JOIN_AHEAD 30.0f

// InvalidLaneChangesTrack4211/4212/4213 (0x80353360, 0x803534E0, 0x80353690):
// no change from `from` to `to` between `start` and `end` metres into a race
// run `dir`. Every entry ships flagged.
typedef struct {
	s8 from, to;
	s16 start, end;
	u8 flag, dir;
} LaneBan;

static const LaneBan kBans4211[] = {
	{ 2, 3, 847, 874, 1, 0 }, { 4, 3, 847, 874, 1, 0 }, { 4, 5, 847, 874, 1, 0 },
	{ 5, 4, 847, 874, 1, 0 }, { 2, 3, 988, 1012, 1, 0 }, { 3, 2, 988, 1012, 1, 0 },
	{ 3, 4, 988, 1012, 1, 0 }, { 5, 4, 988, 1012, 1, 0 }, { 2, 3, 1270, 1295, 1, 1 },
	{ 4, 3, 1270, 1295, 1, 1 }, { 4, 5, 1270, 1295, 1, 1 }, { 5, 4, 1270, 1295, 1, 1 },
	{ 2, 3, 1409, 1434, 1, 1 }, { 3, 2, 1409, 1434, 1, 1 }, { 3, 4, 1409, 1434, 1, 1 },
	{ 5, 4, 1409, 1434, 1, 1 },
};
static const LaneBan kBans4212[] = {
	{ 2, 3, 849, 873, 1, 0 }, { 3, 2, 849, 873, 1, 0 }, { 3, 4, 849, 873, 1, 0 },
	{ 5, 4, 849, 873, 1, 0 }, { 2, 3, 988, 1012, 1, 0 }, { 3, 2, 988, 1012, 1, 0 },
	{ 3, 4, 988, 1012, 1, 0 }, { 4, 3, 988, 1012, 1, 0 }, { 4, 5, 988, 1012, 1, 0 },
	{ 3, 2, 1270, 1295, 1, 1 }, { 3, 4, 1270, 1295, 1, 1 }, { 4, 3, 1270, 1295, 1, 1 },
	{ 4, 5, 1270, 1295, 1, 1 }, { 5, 4, 1270, 1295, 1, 1 }, { 2, 3, 1409, 1434, 1, 1 },
	{ 4, 3, 1409, 1434, 1, 1 }, { 4, 5, 1409, 1434, 1, 1 }, { 5, 4, 1409, 1434, 1, 1 },
};
static const LaneBan kBans4213[] = {
	{ 3, 2, 847, 874, 1, 0 }, { 3, 4, 847, 874, 1, 0 }, { 5, 4, 847, 874, 1, 0 },
	{ 2, 3, 988, 1012, 1, 0 }, { 4, 3, 988, 1012, 1, 0 }, { 4, 5, 988, 1012, 1, 0 },
	{ 3, 2, 1270, 1295, 1, 1 }, { 3, 4, 1270, 1295, 1, 1 }, { 5, 4, 1270, 1295, 1, 1 },
	{ 2, 3, 1409, 1434, 1, 1 }, { 4, 3, 1409, 1434, 1, 1 }, { 4, 5, 1409, 1434, 1, 1 },
};

// InvalidLaneChange::IsValidLaneChange (0x8003B1D0).
static bool validLaneChange(const DragLanes* d, float raceDist, int from, int to, bool ai) {
	const LaneBan* t;
	int n;
	switch (d->track) {
	case 4211: t = kBans4211; n = (int)(sizeof(kBans4211) / sizeof(kBans4211[0])); break;
	case 4212: t = kBans4212; n = (int)(sizeof(kBans4212) / sizeof(kBans4212[0])); break;
	case 4213: t = kBans4213; n = (int)(sizeof(kBans4213) / sizeof(kBans4213[0])); break;
	default: return true;
	}
	for (int k = 0; k < n; k++) {
		const LaneBan* e = &t[k];
		const float start = (float)e->start - (e->flag && ai ? DRAGLANE_AI_SOONER : 0.0f);
		if (e->dir == d->dir && e->from == from && e->to == to &&
				raceDist >= start && raceDist <= (float)e->end)
			return false;
	}
	return true;
}

static bool laneValid(const DragLanes* d, int lane, float dist) {
	return trackroute_lane_at(d->lanes, lane, dist, NULL);
}

// Lanes `a` and `b` at `dist` on one road: at one height, and either's
// lateral inside the other's road.
static bool joinedAt(const DragLanes* d, int a, int b, float dist) {
	TrackLaneRoad ra, rb;
	if (!trackroute_lane_road(d->lanes, a, dist, &ra) ||
			!trackroute_lane_road(d->lanes, b, dist, &rb))
		return false;
	if (fabsf(ra.z - rb.z) > DRAGLANE_SAME_Z) return false;
	return (rb.lat >= ra.lo && rb.lat <= ra.hi) || (ra.lat >= rb.lo && ra.lat <= rb.hi);
}

static bool joined(const DragLanes* d, int a, int b, float dist) {
	if (!joinedAt(d, a, b, dist)) return false;
	// Past the lanes' end there is nothing to part from.
	const float on = dist + DRAGLANE_JOIN_AHEAD;
	if (!laneValid(d, a, on) || !laneValid(d, b, on)) return true;
	return joinedAt(d, a, b, on);
}

static int nearestAt(const DragLanes* d, float dist, float lat, const float* z, int exLo,
		int exHi) {
	const bool all = exLo == DRAGLANE_NONE || exHi == DRAGLANE_NONE;
	int best = -1;
	float bestD = 9e9f;   // 0x80317058
	for (int l = TRACK_LANE_FIRST; l <= TRACK_LANE_LAST; l++) {
		TrackLaneRoad r;
		if (!all && l >= exLo && l <= exHi) continue;
		if (!trackroute_lane_road(d->lanes, l, dist, &r)) continue;
		if (z && fabsf(r.z - *z) > DRAGLANE_SAME_Z) continue;
		const float e = fabsf(r.lat - lat);
		if (e < bestD) {
			best = l;
			bestD = e;
		}
	}
	return best;
}

int draglane_nearest(const DragLanes* d, float dist, float lat, float z, int exLo, int exHi) {
	if (!d || !d->lanes) return -1;
	// PORT: no lane at the car's height (in the air off a crest, say): the
	// nearest at any.
	const int l = nearestAt(d, dist, lat, &z, exLo, exHi);
	return l >= 0 ? l : nearestAt(d, dist, lat, NULL, exLo, exHi);
}

bool draglane_can_change(const DragLanes* d, float dist, float raceDist, int from, int to,
		bool ai, bool drag) {
	// IsForwardsTrafficLane(to), then IsDrivingLineValid(to) and how far it runs.
	if (!d || !d->lanes || to < TRACK_LANE_FIRST || to > TRACK_LANE_LAST) return false;
	if (!laneValid(d, to, dist)) return false;
	const bool fromValid = from >= TRACK_LANE_FIRST && from <= TRACK_LANE_LAST &&
		laneValid(d, from, dist);
	const float runTo = trackroute_lane_run(d->lanes, to, dist, DRAGLANE_AHEAD);
	const float runFrom = fromValid ?
		trackroute_lane_run(d->lanes, from, dist, DRAGLANE_AHEAD) : 0.0f;
	if (!drag && runTo < DRAGLANE_AHEAD && runTo - runFrom < DRAGLANE_LONGER) return false;
	if (!fromValid || from == to) return true;
	// In a drag, every lane on the way: there, as far, and not banned.
	const int step = to > from ? 1 : -1;
	for (int l = from; l != to; l += step) {
		const int next = l + step;
		if (!laneValid(d, next, dist) || !joined(d, l, next, dist)) return false;
		const float runL = trackroute_lane_run(d->lanes, l, dist, DRAGLANE_AHEAD);
		const float runN = trackroute_lane_run(d->lanes, next, dist, DRAGLANE_AHEAD);
		if (runN - runL < DRAGLANE_AS_FAR) return false;
		if (!validLaneChange(d, raceDist, l, next, ai)) return false;
	}
	return true;
}

float draglane_lateral_speed(float speed) {
	float t = speed * DRAG_MPH / DRAG_RATE_MPH;
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	return DRAG_RATE_SLOW + (DRAG_RATE_FAST - DRAG_RATE_SLOW) * t;
}

// ---------------------------------------------------------- DragController

static void notLockOnto(DragController* c, int lo, int hi) {
	c->exLo = lo;
	c->changing = true;
	c->exHi = hi;
}

void dragctl_init(DragController* c) {
	c->changing = false;
	c->changeAt = 0.0f;
	c->state = 2;
	notLockOnto(c, DRAGLANE_NONE, DRAGLANE_NONE);
	c->changing = false;
	c->leftHeld = c->rightHeld = false;
	c->rethrow = false;
	c->lane = -1;
	c->rate = DRAG_RATE_SLOW;
}

// JoyHandler_ChangeLane for one event whose held state changed.
static void changeLaneEvent(DragController* c, bool right, bool down) {
	if (!down) {
		c->rethrow = true;
		c->changing = false;
		c->state = 2;
		return;
	}
	const int want = right ? 1 : 0;
	if (c->state == want) return;
	c->state = want;
	c->changing = false;
	notLockOnto(c, DRAGLANE_NONE, DRAGLANE_NONE);
	c->changing = false;
}

void dragctl_input(DragController* c, bool left, bool right) {
	if (left != c->leftHeld) {
		c->leftHeld = left;
		changeLaneEvent(c, false, left);
	}
	if (right != c->rightHeld) {
		c->rightHeld = right;
		changeLaneEvent(c, true, right);
	}
}

// ChangeLaneNumber (0x8001EDB8): the choice up to `n` lanes over from the
// nearest, as far as CanChangeDrivingLine allows each step. True when it moved.
static bool changeLaneNumber(DragController* c, const DragLanes* d, float dist, float lat,
		float z, float speed, float raceDist, int n) {
	const int near = draglane_nearest(d, dist, lat, z, DRAGLANE_NONE, DRAGLANE_NONE);
	// PORT: retail chooses lane 2 with no lane near and steps from -1; with no
	// lane under the car the port keeps the choice it has.
	if (near < 0) return false;
	c->rate = draglane_lateral_speed(speed);
	int lane = near;
	const int step = n > 0 ? 1 : -1;
	for (int k = n; k != 0; k -= step) {
		if (!draglane_can_change(d, dist, raceDist, lane, lane + step, false, true)) break;
		lane += step;
	}
	c->lane = lane;
	return lane != near;
}

// LockOnToNearestTrafficLane (0x8001ECE8).
static void lockOn(DragController* c, const DragLanes* d, float dist, float lat, float z,
		float speed) {
	int lane = draglane_nearest(d, dist, lat, z, c->exLo, c->exHi);
	if (lane == -1) {
		notLockOnto(c, DRAGLANE_NONE, DRAGLANE_NONE);
		c->changing = false;
		lane = draglane_nearest(d, dist, lat, z, c->exLo, c->exHi);
	}
	// PORT: SetChoice(-1) leaves retail's drive target on no line; the port
	// keeps the lane it had.
	if (lane >= 0) c->lane = lane;
	c->rate = draglane_lateral_speed(speed);
}

void dragctl_think(DragController* c, const DragLanes* d, float dist, float lat, float z,
		float speed, float raceDist) {
	if (!c || !d || !d->lanes) return;
	// The exclusion goes once the car is on its choice's line and the lane
	// nearest it is not one of those left out.
	if (!(c->exLo == DRAGLANE_NONE && c->exHi == DRAGLANE_NONE)) {
		const int near = draglane_nearest(d, dist, lat, z, DRAGLANE_NONE, DRAGLANE_NONE);
		float at;
		if ((near < c->exLo || near > c->exHi) && c->lane >= 0 &&
				trackroute_lane_at(d->lanes, c->lane, dist, &at) &&
				fabsf(lat - at) < DRAGCTL_ARRIVED) {
			notLockOnto(c, DRAGLANE_NONE, DRAGLANE_NONE);
			c->changing = false;
		}
	}
	if (c->state == 1 || c->state == 0) {
		const bool right = c->state == 1;
		const bool moved = changeLaneNumber(c, d, dist, lat, z, speed, raceDist, right ? 4 : -4);
		if (c->changing || !moved) return;
		const int near = draglane_nearest(d, dist, lat, z, DRAGLANE_NONE, DRAGLANE_NONE);
		if (right) {
			c->exHi = near;
			c->exLo = TRACK_LANE_FIRST;
		} else {
			c->exLo = near;
			c->exHi = TRACK_LANE_LAST;
		}
		c->changing = true;
		c->changeAt = raceDist;
	} else if (c->state == 2) {
		lockOn(c, d, dist, lat, z, speed);
	}
}

void dragctl_hit_wall(DragController* c, const DragLanes* d, float dist, float lat, float z,
		float speed) {
	if (!c || !d || !d->lanes) return;
	notLockOnto(c, DRAGLANE_NONE, DRAGLANE_NONE);
	const int lane = draglane_nearest(d, dist, lat, z, DRAGLANE_NONE, DRAGLANE_NONE);
	if (lane >= 0) c->lane = lane;
	c->rate = draglane_lateral_speed(speed);
}

void dragctl_hit_car(DragController* c, int lane) {
	if (!c) return;
	notLockOnto(c, DRAGLANE_NONE, DRAGLANE_NONE);
	c->changing = false;
	if (lane >= 0) c->lane = lane;
}
